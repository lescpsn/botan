/*
* GCM Mode Encryption
* (C) 2013,2015 Jack Lloyd
* (C) 2016 Daniel Neus, Rohde & Schwarz Cybersecurity
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/gcm.h>
#include <botan/block_cipher.h>
#include <botan/internal/ct_utils.h>
#include <botan/loadstor.h>
#include <botan/ctr.h>

#if defined(BOTAN_HAS_GCM_CLMUL)
  #include <botan/internal/clmul.h>
  #include <botan/cpuid.h>
#elif defined(BOTAN_HAS_GCM_PMULL)
  #include <botan/internal/pmull.h>
  #include <botan/cpuid.h>
#endif

namespace Botan {

static const size_t GCM_BS = 16;

void GHASH::gcm_multiply(secure_vector<uint8_t>& x,
                         const uint8_t input[],
                         size_t blocks)
   {
   if(blocks == 0)
      return;

#if defined(BOTAN_HAS_GCM_CLMUL)
   if(CPUID::has_clmul())
      return gcm_multiply_clmul(x.data(), m_H.data(), input, blocks);
#elif defined(BOTAN_HAS_GCM_PMULL)
   if(CPUID::has_arm_pmull())
      return gcm_multiply_pmull(x.data(), m_H.data(), input, blocks);
#endif

   CT::poison(x.data(), x.size());

   // SSE2 might be useful here

   const uint64_t ones = 0xFFFFFFFFFFFFFFFF;

   uint64_t X0 = load_be<uint64_t>(x.data(), 0);
   uint64_t X1 = load_be<uint64_t>(x.data(), 1);

   for(size_t b = 0; b != blocks; ++b)
      {
      X0 ^= load_be<uint64_t>(input, 2*b);
      X1 ^= load_be<uint64_t>(input, 2*b+1);

      uint64_t Z[2] = { 0, 0 };

      for(size_t i = 0; i != 64; ++i)
         {
         const uint64_t X0MASK = ones * ((X0 >> (63-i)) & 1);
         const uint64_t X1MASK = ones * ((X1 >> (63-i)) & 1);
         Z[0] ^= m_HM[4*i  ] & X0MASK;
         Z[1] ^= m_HM[4*i+1] & X0MASK;
         Z[0] ^= m_HM[4*i+2] & X1MASK;
         Z[1] ^= m_HM[4*i+3] & X1MASK;
         }

      X0 = Z[0];
      X1 = Z[1];
      }

   store_be<uint64_t>(x.data(), X0, X1);
   CT::unpoison(x.data(), x.size());
   }

void GHASH::ghash_update(secure_vector<uint8_t>& ghash,
                         const uint8_t input[], size_t length)
   {
   /*
   This assumes if less than block size input then we're just on the
   final block and should pad with zeros
   */

   const size_t full_blocks = length / GCM_BS;
   const size_t final_bytes = length - (full_blocks * GCM_BS);

   if(full_blocks > 0)
      {
      gcm_multiply(ghash, input, full_blocks);
      }

   if(final_bytes)
      {
      secure_vector<uint8_t> last_block(GCM_BS);
      copy_mem(last_block.data(), input + full_blocks * GCM_BS, final_bytes);
      gcm_multiply(ghash, last_block.data(), 1);
      }
   }

void GHASH::key_schedule(const uint8_t key[], size_t length)
   {
   m_H.assign(key, key+length);
   m_H_ad.resize(GCM_BS);
   m_ad_len = 0;
   m_text_len = 0;

   uint64_t H0 = load_be<uint64_t>(m_H.data(), 0);
   uint64_t H1 = load_be<uint64_t>(m_H.data(), 1);

   const uint64_t R = 0xE100000000000000;

   m_HM.resize(256);

   // precompute the multiples of H
   for(size_t i = 0; i != 2; ++i)
      {
      for(size_t j = 0; j != 64; ++j)
         {
         /*
         we interleave H^1, H^65, H^2, H^66, ...
         to make indexing nicer in the multiplication code
         */
         m_HM[4*j+2*i] = H0;
         m_HM[4*j+2*i+1] = H1;

         // GCM's bit ops are reversed so we carry out of the bottom
         const uint64_t carry = R * (H1 & 1);
         H1 = (H1 >> 1) | (H0 << 63);
         H0 = (H0 >> 1) ^ carry;
         }
      }
   }

void GHASH::start(const uint8_t nonce[], size_t len)
   {
   m_nonce.assign(nonce, nonce + len);
   m_ghash = m_H_ad;
   }

void GHASH::set_associated_data(const uint8_t input[], size_t length)
   {
   zeroise(m_H_ad);

   ghash_update(m_H_ad, input, length);
   m_ad_len = length;
   }

void GHASH::update_associated_data(const uint8_t ad[], size_t length)
   {
   BOTAN_ASSERT(m_ghash.size() == GCM_BS, "Key was set");
   m_ad_len += length;
   ghash_update(m_ghash, ad, length);
   }

void GHASH::update(const uint8_t input[], size_t length)
   {
   BOTAN_ASSERT(m_ghash.size() == GCM_BS, "Key was set");
   m_text_len += length;
   ghash_update(m_ghash, input, length);
   }

void GHASH::add_final_block(secure_vector<uint8_t>& hash,
                            size_t ad_len, size_t text_len)
   {
   secure_vector<uint8_t> final_block(GCM_BS);
   store_be<uint64_t>(final_block.data(), 8*ad_len, 8*text_len);
   ghash_update(hash, final_block.data(), final_block.size());
   }

secure_vector<uint8_t> GHASH::final()
   {
   add_final_block(m_ghash, m_ad_len, m_text_len);

   secure_vector<uint8_t> mac;
   mac.swap(m_ghash);

   mac ^= m_nonce;
   m_text_len = 0;
   return mac;
   }

secure_vector<uint8_t> GHASH::nonce_hash(const uint8_t nonce[], size_t nonce_len)
   {
   BOTAN_ASSERT(m_ghash.size() == 0, "nonce_hash called during wrong time");
   secure_vector<uint8_t> y0(GCM_BS);

   ghash_update(y0, nonce, nonce_len);
   add_final_block(y0, 0, nonce_len);

   return y0;
   }

void GHASH::clear()
   {
   zeroise(m_H);
   reset();
   }

void GHASH::reset()
   {
   zeroise(m_H_ad);
   m_ghash.clear();
   m_nonce.clear();
   m_text_len = m_ad_len = 0;
   }

/*
* GCM_Mode Constructor
*/
GCM_Mode::GCM_Mode(BlockCipher* cipher, size_t tag_size) :
   m_tag_size(tag_size),
   m_cipher_name(cipher->name())
   {
   if(cipher->block_size() != GCM_BS)
      throw Invalid_Argument("Invalid block cipher for GCM");

   m_ghash.reset(new GHASH);

   m_ctr.reset(new CTR_BE(cipher, 4)); // CTR_BE takes ownership of cipher

   /* We allow any of the values 128, 120, 112, 104, or 96 bits as a tag size */
   /* 64 bit tag is still supported but deprecated and will be removed in the future */
   if(m_tag_size != 8 && (m_tag_size < 12 || m_tag_size > 16))
      throw Invalid_Argument(name() + ": Bad tag size " + std::to_string(m_tag_size));
   }

GCM_Mode::~GCM_Mode() { /* for unique_ptr */ }

void GCM_Mode::clear()
   {
   m_ctr->clear();
   m_ghash->clear();
   reset();
   }

void GCM_Mode::reset()
   {
   m_ghash->reset();
   }

std::string GCM_Mode::name() const
   {
   return (m_cipher_name + "/GCM(" + std::to_string(tag_size()) + ")");
   }

std::string GCM_Mode::provider() const
   {
#if defined(BOTAN_HAS_GCM_CLMUL)
   if(CPUID::has_clmul())
      return "clmul";
#endif

   return "base";
   }

size_t GCM_Mode::update_granularity() const
   {
   return GCM_BS;
   }

Key_Length_Specification GCM_Mode::key_spec() const
   {
   return m_ctr->key_spec();
   }

void GCM_Mode::key_schedule(const uint8_t key[], size_t keylen)
   {
   m_ctr->set_key(key, keylen);

   const std::vector<uint8_t> zeros(GCM_BS);
   m_ctr->set_iv(zeros.data(), zeros.size());

   secure_vector<uint8_t> H(GCM_BS);
   m_ctr->encipher(H);
   m_ghash->set_key(H);
   }

void GCM_Mode::set_associated_data(const uint8_t ad[], size_t ad_len)
   {
   m_ghash->set_associated_data(ad, ad_len);
   }

void GCM_Mode::start_msg(const uint8_t nonce[], size_t nonce_len)
   {
   if(!valid_nonce_length(nonce_len))
      throw Invalid_IV_Length(name(), nonce_len);

   secure_vector<uint8_t> y0(GCM_BS);

   if(nonce_len == 12)
      {
      copy_mem(y0.data(), nonce, nonce_len);
      y0[15] = 1;
      }
   else
      {
      y0 = m_ghash->nonce_hash(nonce, nonce_len);
      }

   m_ctr->set_iv(y0.data(), y0.size());

   secure_vector<uint8_t> m_enc_y0(GCM_BS);
   m_ctr->encipher(m_enc_y0);

   m_ghash->start(m_enc_y0.data(), m_enc_y0.size());
   }

size_t GCM_Encryption::process(uint8_t buf[], size_t sz)
   {
   BOTAN_ARG_CHECK(sz % update_granularity() == 0);
   m_ctr->cipher(buf, buf, sz);
   m_ghash->update(buf, sz);
   return sz;
   }

void GCM_Encryption::finish(secure_vector<uint8_t>& buffer, size_t offset)
   {
   BOTAN_ARG_CHECK(offset <= buffer.size());
   const size_t sz = buffer.size() - offset;
   uint8_t* buf = buffer.data() + offset;

   m_ctr->cipher(buf, buf, sz);
   m_ghash->update(buf, sz);
   auto mac = m_ghash->final();
   buffer += std::make_pair(mac.data(), tag_size());
   }

size_t GCM_Decryption::process(uint8_t buf[], size_t sz)
   {
   BOTAN_ARG_CHECK(sz % update_granularity() == 0);
   m_ghash->update(buf, sz);
   m_ctr->cipher(buf, buf, sz);
   return sz;
   }

void GCM_Decryption::finish(secure_vector<uint8_t>& buffer, size_t offset)
   {
   BOTAN_ARG_CHECK(offset <= buffer.size());
   const size_t sz = buffer.size() - offset;
   uint8_t* buf = buffer.data() + offset;

   if(sz < tag_size())
      throw Exception("Insufficient input for GCM decryption, tag missing");

   const size_t remaining = sz - tag_size();

   // handle any final input before the tag
   if(remaining)
      {
      m_ghash->update(buf, remaining);
      m_ctr->cipher(buf, buf, remaining);
      }

   auto mac = m_ghash->final();

   const uint8_t* included_tag = &buffer[remaining+offset];

   if(!constant_time_compare(mac.data(), included_tag, tag_size()))
      throw Integrity_Failure("GCM tag check failed");

   buffer.resize(offset + remaining);
   }

}
