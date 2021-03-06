/*
* (C) 2007 Falko Strenzke
*     2007 Manuel Hartl
*     2009,2015 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include "tests.h"

#if defined(BOTAN_HAS_ECC_GROUP)
   #include <botan/bigint.h>
   #include <botan/numthry.h>
   #include <botan/curve_nistp.h>
   #include <botan/pk_keys.h>
   #include <botan/point_gfp.h>
   #include <botan/ec_group.h>
   #include <botan/reducer.h>
   #include <botan/hex.h>
   #include <botan/data_src.h>
   #include <botan/x509_key.h>
#endif

namespace Botan_Tests {

namespace {

#if defined(BOTAN_HAS_ECC_GROUP)

Botan::BigInt test_integer(Botan::RandomNumberGenerator& rng, size_t bits, BigInt max)
   {
   /*
   Produces integers with long runs of ones and zeros, for testing for
   carry handling problems.
   */
   Botan::BigInt x = 0;

   auto flip_prob = [](size_t i) -> double
      {
      if(i % 64 == 0)
         {
         return .5;
         }
      if(i % 32 == 0)
         {
         return .4;
         }
      if(i % 8 == 0)
         {
         return .05;
         }
      return .01;
      };

   bool active = rng.next_byte() % 2;
   for(size_t i = 0; i != bits; ++i)
      {
      x <<= 1;
      x += static_cast<int>(active);

      const double prob = flip_prob(i);
      const double sample = double(rng.next_byte() % 100) / 100.0; // biased

      if(sample < prob)
         {
         active = !active;
         }
      }

   if(max > 0)
      {
      while(x >= max)
         {
         const size_t b = x.bits() - 1;
         BOTAN_ASSERT(x.get_bit(b) == true, "Set");
         x.clear_bit(b);
         }
      }

   return x;
   }

Botan::PointGFp create_random_point(Botan::RandomNumberGenerator& rng,
                                    const Botan::EC_Group& group)
   {
   const Botan::BigInt& p = group.get_p();

   Botan::Modular_Reducer mod_p(p);

   while(true)
      {
      const Botan::BigInt x = Botan::BigInt::random_integer(rng, 1, p);
      const Botan::BigInt x3 = mod_p.multiply(x, mod_p.square(x));
      const Botan::BigInt ax = mod_p.multiply(group.get_a(), x);
      const Botan::BigInt y = mod_p.reduce(x3 + ax + group.get_b());
      const Botan::BigInt sqrt_y = ressol(y, p);

      if(sqrt_y > 1)
         {
         BOTAN_ASSERT_EQUAL(mod_p.square(sqrt_y), y, "Square root is correct");
         Botan::PointGFp point = group.point(x, sqrt_y);
         return point;
         }
      }
   }

class ECC_Randomized_Tests final : public Test
   {
   public:
      std::vector<Test::Result> run() override;
   };

std::vector<Test::Result> ECC_Randomized_Tests::run()
   {
   std::vector<Test::Result> results;
   std::set<std::string> named_groups = Botan::EC_Group::known_named_groups();
   for(auto const& group_name : named_groups)
      {
      Test::Result result("ECC randomized " + group_name);

      result.start_timer();

      Botan::EC_Group group(group_name);

      const Botan::PointGFp& base_point = group.get_base_point();
      const Botan::BigInt& group_order = group.get_order();

      const Botan::PointGFp inf = base_point * group_order;
      result.test_eq("infinite order correct", inf.is_zero(), true);
      result.test_eq("infinity on the curve", inf.on_the_curve(), true);

      std::vector<Botan::BigInt> blind_ws;

      try
         {
         const size_t trials = (Test::run_long_tests() ? 10 : 3);
         for(size_t i = 0; i < trials; ++i)
            {
            const size_t w = 1 + (Test::rng().next_byte() % 8);

            Botan::PointGFp_Blinded_Multiplier blinded(base_point, w);

            const Botan::BigInt a = Botan::BigInt::random_integer(Test::rng(), 2, group_order);
            const Botan::BigInt b = Botan::BigInt::random_integer(Test::rng(), 2, group_order);
            const Botan::BigInt c = a + b;

            const Botan::PointGFp P = base_point * a;
            const Botan::PointGFp Q = base_point * b;
            const Botan::PointGFp R = base_point * c;

            const Botan::PointGFp P1 = blinded.mul(a, group_order, Test::rng(), blind_ws);
            const Botan::PointGFp Q1 = blinded.mul(b, group_order, Test::rng(), blind_ws);
            const Botan::PointGFp R1 = blinded.mul(c, group_order, Test::rng(), blind_ws);

            const Botan::PointGFp A1 = P + Q;
            const Botan::PointGFp A2 = Q + P;

            result.test_eq("p + q", A1, R);
            result.test_eq("q + p", A2, R);

            result.test_eq("p on the curve", P.on_the_curve(), true);
            result.test_eq("q on the curve", Q.on_the_curve(), true);
            result.test_eq("r on the curve", R.on_the_curve(), true);

            result.test_eq("P1", P1, P);
            result.test_eq("Q1", Q1, Q);
            result.test_eq("R1", R1, R);
            }
         }
      catch(std::exception& e)
         {
         result.test_failure(group_name, e.what());
         }
      result.end_timer();
      results.push_back(result);
      }

   return results;
   }

BOTAN_REGISTER_TEST("ecc_randomized", ECC_Randomized_Tests);

class NIST_Curve_Reduction_Tests final : public Test
   {
   public:
      typedef std::function<void (Botan::BigInt&, Botan::secure_vector<Botan::word>&)> reducer_fn;
      std::vector<Test::Result> run() override
         {
         std::vector<Test::Result> results;

         // Using lambdas here to avoid strange UbSan warning (#1370)

#if defined(BOTAN_HAS_NIST_PRIME_REDUCERS_W32)
         results.push_back(random_redc_test(
	    "P-384",
	    Botan::prime_p384(),
	    [](Botan::BigInt& p, Botan::secure_vector<Botan::word>& ws) -> void
	        {
		Botan::redc_p384(p, ws);
		}));
         results.push_back(random_redc_test(
	    "P-256",
	    Botan::prime_p256(),
	    [](Botan::BigInt& p, Botan::secure_vector<Botan::word>& ws) -> void
	        {
		Botan::redc_p256(p, ws);
		}));
         results.push_back(random_redc_test(
	    "P-224",
	    Botan::prime_p224(),
	    [](Botan::BigInt& p, Botan::secure_vector<Botan::word>& ws) -> void
	        {
		Botan::redc_p224(p, ws);
		}));
         results.push_back(random_redc_test(
	    "P-192",
	    Botan::prime_p192(),
	    [](Botan::BigInt& p, Botan::secure_vector<Botan::word>& ws) -> void
	        {
		Botan::redc_p192(p, ws);
		}));
#endif
         results.push_back(random_redc_test(
	    "P-521",
	    Botan::prime_p521(),
	    [](Botan::BigInt& p, Botan::secure_vector<Botan::word>& ws) -> void
	        {
	        Botan::redc_p521(p, ws);
	        }));

         return results;
         }

      Test::Result random_redc_test(const std::string& prime_name,
                                    const Botan::BigInt& p,
                                    reducer_fn redc_fn)
         {
         const Botan::BigInt p2 = p * p;
         const size_t p_bits = p.bits();

         Botan::Modular_Reducer p_redc(p);
         Botan::secure_vector<Botan::word> ws;

         Test::Result result("NIST " + prime_name + " reduction");
         result.start_timer();

         const size_t trials = (Test::run_long_tests() ? 128 : 16);

         for(size_t i = 0; i <= trials; ++i)
            {
            const Botan::BigInt x = test_integer(Test::rng(), 2 * p_bits, p2);

            // TODO: time and report all three approaches
            const Botan::BigInt v1 = x % p;
            const Botan::BigInt v2 = p_redc.reduce(x);

            Botan::BigInt v3 = x;
            redc_fn(v3, ws);

            if(!result.test_eq("reference redc", v1, v2) ||
                  !result.test_eq("specialized redc", v2, v3))
               {
               result.test_note("failing input" + Botan::hex_encode(Botan::BigInt::encode(x)));
               }
            }

         result.end_timer();

         return result;
         }
   };

BOTAN_REGISTER_TEST("nist_redc", NIST_Curve_Reduction_Tests);

Test::Result test_groups()
   {
   Test::Result result("ECC Unit");
   std::set<std::string> named_groups = Botan::EC_Group::known_named_groups();
   for(auto const& group_name : named_groups)
      {
      const Botan::EC_Group group(group_name);
      result.confirm("EC_Group is known", !group.get_curve_oid().empty());
      result.test_eq("EC_Group has correct bit size", group.get_p().bits(), group.get_p_bits());
      result.test_eq("EC_Group has byte size", group.get_p().bytes(), group.get_p_bytes());

      bool a_is_minus_3 = group.a_is_minus_3();

      if(a_is_minus_3)
         result.test_eq("Group A equals -3", group.get_a(), group.get_p() - 3);
      else
         result.test_ne("Group " + group_name + " A does not equal -3", group.get_a(), group.get_p() - 3);
      }
   return result;
   }

Test::Result test_coordinates()
   {
   Test::Result result("ECC Unit");

   const Botan::BigInt exp_affine_x("16984103820118642236896513183038186009872590470");
   const Botan::BigInt exp_affine_y("1373093393927139016463695321221277758035357890939");

   // precalculation
   const Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   const Botan::PointGFp point_exp = secp160r1.point(exp_affine_x, exp_affine_y);
   result.confirm("Point is on the curve", point_exp.on_the_curve());

   const Botan::PointGFp p1 = p_G * 2;
   result.test_eq("Point affine x", p1.get_affine_x(), exp_affine_x);
   result.test_eq("Point affine y", p1.get_affine_y(), exp_affine_y);
   return result;
   }


/**
Test point multiplication according to
--------
SEC 2: Test Vectors for SEC 1
Certicom Research
Working Draft
September, 1999
Version 0.3;
Section 2.1.2
--------
*/
Test::Result test_point_transformation()
   {
   Test::Result result("ECC Unit");

   // get a valid point
   Botan::EC_Group dom_pars("secp160r1");
   Botan::PointGFp p = dom_pars.get_base_point() * Test::rng().next_nonzero_byte();

   // get a copy
   Botan::PointGFp q = p;

   p.randomize_repr(Test::rng());
   q.randomize_repr(Test::rng());

   result.test_eq("affine x after copy", p.get_affine_x(), q.get_affine_x());
   result.test_eq("affine y after copy", p.get_affine_y(), q.get_affine_y());
   return result;
   }

Test::Result test_point_mult()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   Botan::BigInt d_U("0xaa374ffc3ce144e6b073307972cb6d57b2a4e982");
   Botan::PointGFp Q_U = d_U * p_G;

   result.test_eq("affine x", Q_U.get_affine_x(), Botan::BigInt("466448783855397898016055842232266600516272889280"));
   result.test_eq("affine y", Q_U.get_affine_y(), Botan::BigInt("1110706324081757720403272427311003102474457754220"));
   return result;
   }

Test::Result test_point_negative()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   const Botan::PointGFp p1 = p_G * 2;

   result.test_eq("affine x", p1.get_affine_x(), Botan::BigInt("16984103820118642236896513183038186009872590470"));
   result.test_eq("affine y", p1.get_affine_y(), Botan::BigInt("1373093393927139016463695321221277758035357890939"));

   const Botan::PointGFp p1_neg = -p1;

   result.test_eq("affine x", p1_neg.get_affine_x(), p1.get_affine_x());
   result.test_eq("affine y", p1_neg.get_affine_y(),  Botan::BigInt("88408243403763901739989511495005261618427168388"));
   return result;
   }

Test::Result test_zeropoint()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");

   Botan::PointGFp p1 = secp160r1.point(Botan::BigInt("16984103820118642236896513183038186009872590470"),
                                        Botan::BigInt("1373093393927139016463695321221277758035357890939"));

   result.confirm("point is on the curve", p1.on_the_curve());

   Botan::PointGFp p2 = p1;
   p1 -= p2;

   result.confirm("p - q with q = p results in zero", p1.is_zero());
   return result;
   }

Test::Result test_zeropoint_enc_dec()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");

   Botan::PointGFp p = secp160r1.zero_point();
   result.confirm("zero point is zero", p.is_zero());

   std::vector<uint8_t> sv_p = unlock(EC2OSP(p, Botan::PointGFp::UNCOMPRESSED));
   result.test_eq("encoded/decode rt works", secp160r1.OS2ECP(sv_p), p);

   sv_p = unlock(EC2OSP(p, Botan::PointGFp::COMPRESSED));
   result.test_eq("encoded/decode compressed rt works", secp160r1.OS2ECP(sv_p), p);

   sv_p = unlock(EC2OSP(p, Botan::PointGFp::HYBRID));
   result.test_eq("encoded/decode hybrid rt works", secp160r1.OS2ECP(sv_p), p);
   return result;
   }

Test::Result test_calc_with_zeropoint()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");

   Botan::PointGFp p = secp160r1.point(Botan::BigInt("16984103820118642236896513183038186009872590470"),
                                       Botan::BigInt("1373093393927139016463695321221277758035357890939"));

   result.confirm("point is on the curve", p.on_the_curve());
   result.confirm("point is not zero", !p.is_zero());

   Botan::PointGFp zero = secp160r1.zero_point();
   result.confirm("zero point is zero", zero.is_zero());

   Botan::PointGFp res = p + zero;
   result.test_eq("point + 0 equals the point", p, res);

   res = p - zero;
   result.test_eq("point - 0 equals the point", p, res);

   res = zero * 32432243;
   result.confirm("point * 0 is the zero point", res.is_zero());
   return result;
   }

Test::Result test_add_point()
   {
   Test::Result result("ECC Unit");

   // precalculation
   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   Botan::PointGFp p0 = p_G;
   Botan::PointGFp p1 = p_G * 2;

   p1 += p0;

   Botan::PointGFp expected = secp160r1.point(Botan::BigInt("704859595002530890444080436569091156047721708633"),
                                              Botan::BigInt("1147993098458695153857594941635310323215433166682"));

   result.test_eq("point addition", p1, expected);
   return result;
   }

Test::Result test_sub_point()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   Botan::PointGFp p0 = p_G;
   Botan::PointGFp p1 = p_G * 2;

   p1 -= p0;

   Botan::PointGFp expected = secp160r1.point(Botan::BigInt("425826231723888350446541592701409065913635568770"),
                                              Botan::BigInt("203520114162904107873991457957346892027982641970"));

   result.test_eq("point subtraction", p1, expected);
   return result;
   }

Test::Result test_mult_point()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   Botan::PointGFp p0 = p_G;
   Botan::PointGFp p1 = p_G * 2;

   p1 *= p0.get_affine_x();

   const Botan::BigInt exp_mult_x(std::string("967697346845926834906555988570157345422864716250"));
   const Botan::BigInt exp_mult_y(std::string("512319768365374654866290830075237814703869061656"));
   Botan::PointGFp expected = secp160r1.point(exp_mult_x, exp_mult_y);

   result.test_eq("point mult", p1, expected);
   return result;
   }

Test::Result test_basic_operations()
   {
   Test::Result result("ECC Unit");

   // precalculation
   Botan::EC_Group secp160r1("secp160r1");
   const Botan::PointGFp& p_G = secp160r1.get_base_point();

   const Botan::PointGFp p0 = p_G;
   const Botan::PointGFp p1 = p_G * 2;

   result.test_eq("p1 affine x", p1.get_affine_x(), Botan::BigInt("16984103820118642236896513183038186009872590470"));
   result.test_eq("p1 affine y", p1.get_affine_y(), Botan::BigInt("1373093393927139016463695321221277758035357890939"));

   const Botan::PointGFp simplePlus = p1 + p0;
   const Botan::PointGFp exp_simplePlus = secp160r1.point(Botan::BigInt("704859595002530890444080436569091156047721708633"),
                                                          Botan::BigInt("1147993098458695153857594941635310323215433166682"));

   result.test_eq("point addition", simplePlus, exp_simplePlus);

   const Botan::PointGFp simpleMinus = p1 - p0;
   const Botan::PointGFp exp_simpleMinus=  secp160r1.point(Botan::BigInt("425826231723888350446541592701409065913635568770"),
                                                           Botan::BigInt("203520114162904107873991457957346892027982641970"));

   result.test_eq("point subtraction", simpleMinus, exp_simpleMinus);

   const Botan::PointGFp simpleMult = p1 * 123456789;

   result.test_eq("point mult affine x", simpleMult.get_affine_x(),
                  Botan::BigInt("43638877777452195295055270548491599621118743290"));
   result.test_eq("point mult affine y", simpleMult.get_affine_y(),
                  Botan::BigInt("56841378500012376527163928510402662349220202981"));

   return result;
   }

Test::Result test_enc_dec_compressed_160()
   {
   Test::Result result("ECC Unit");

   // Test for compressed conversion (02/03) 160bit
   Botan::EC_Group secp160r1("secp160r1");

   const std::vector<uint8_t> G_comp = Botan::hex_decode("024A96B5688EF573284664698968C38BB913CBFC82");

   const Botan::PointGFp p = secp160r1.OS2ECP(G_comp);

   std::vector<uint8_t> sv_result = unlock(Botan::EC2OSP(p, Botan::PointGFp::COMPRESSED));

   result.test_eq("result", sv_result, G_comp);
   return result;
   }

Test::Result test_enc_dec_compressed_256()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group group("secp256r1");

   const std::string G_secp_comp = "036B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296";
   const std::vector<uint8_t> sv_G_secp_comp = Botan::hex_decode(G_secp_comp);

   Botan::PointGFp p_G = group.OS2ECP(sv_G_secp_comp);
   std::vector<uint8_t> sv_result = unlock(EC2OSP(p_G, Botan::PointGFp::COMPRESSED));

   result.test_eq("compressed_256", sv_result, sv_G_secp_comp);
   return result;
   }


Test::Result test_enc_dec_uncompressed_112()
   {
   Test::Result result("ECC Unit");

   // Test for uncompressed conversion (04) 112bit

   // Curve is secp112r2

   const Botan::BigInt p("0xdb7c2abf62e35e668076bead208b");
   const Botan::BigInt a("0x6127C24C05F38A0AAAF65C0EF02C");
   const Botan::BigInt b("0x51DEF1815DB5ED74FCC34C85D709");

   const Botan::BigInt g_x("0x4BA30AB5E892B4E1649DD0928643");
   const Botan::BigInt g_y("0xADCD46F5882E3747DEF36E956E97");

   const Botan::BigInt order("0x36DF0AAFD8B8D7597CA10520D04B");
   const Botan::BigInt cofactor("4"); // !

   const Botan::EC_Group group(p, a, b, g_x, g_y, order, cofactor);

   const std::string G_secp_uncomp = "044BA30AB5E892B4E1649DD0928643ADCD46F5882E3747DEF36E956E97";
   const std::vector<uint8_t> sv_G_secp_uncomp = Botan::hex_decode(G_secp_uncomp);

   Botan::PointGFp p_G = group.OS2ECP(sv_G_secp_uncomp);
   std::vector<uint8_t> sv_result = unlock(EC2OSP(p_G, Botan::PointGFp::UNCOMPRESSED));

   result.test_eq("uncompressed_112", sv_result, sv_G_secp_uncomp);
   return result;
   }

Test::Result test_enc_dec_uncompressed_521()
   {
   Test::Result result("ECC Unit");

   // Test for uncompressed conversion(04) with big values(521 bit)

   const std::string G_secp_uncomp =
      "0400C6858E06B70404E9CD9E3ECB662395B4429C648139053FB521F828AF606B4D3DBAA14B5E77EFE75928FE1DC127A2ffA8DE3348B3C1856A429BF97E7E31C2E5BD66011839296A789A3BC0045C8A5FB42C7D1BD998F54449579B446817AFBD17273E662C97EE72995EF42640C550B9013FAD0761353C7086A272C24088BE94769FD16650";

   const std::vector<uint8_t> sv_G_secp_uncomp = Botan::hex_decode(G_secp_uncomp);

   Botan::EC_Group group("secp521r1");

   Botan::PointGFp p_G = group.OS2ECP(sv_G_secp_uncomp);

   std::vector<uint8_t> sv_result = unlock(EC2OSP(p_G, Botan::PointGFp::UNCOMPRESSED));

   result.test_eq("expected", sv_result, sv_G_secp_uncomp);
   return result;
   }

Test::Result test_gfp_store_restore()
   {
   Test::Result result("ECC Unit");

   // generate point
   Botan::EC_Group dom_pars("secp160r1");
   Botan::PointGFp p = dom_pars.get_base_point();

   std::vector<uint8_t> sv_mes = unlock(EC2OSP(p, Botan::PointGFp::COMPRESSED));
   Botan::PointGFp new_p = dom_pars.OS2ECP(sv_mes);

   result.test_eq("original and restored points are same", p, new_p);
   return result;
   }

Test::Result test_more_zeropoint()
   {
   Test::Result result("ECC Unit");

   // by Falko

   Botan::EC_Group secp160r1("secp160r1");

   Botan::PointGFp p1 = secp160r1.point(Botan::BigInt("16984103820118642236896513183038186009872590470"),
                                        Botan::BigInt("1373093393927139016463695321221277758035357890939"));

   result.confirm("point is on the curve", p1.on_the_curve());
   Botan::PointGFp minus_p1 = -p1;
   result.confirm("point is on the curve", minus_p1.on_the_curve());
   Botan::PointGFp shouldBeZero = p1 + minus_p1;
   result.confirm("point is on the curve", shouldBeZero.on_the_curve());
   result.confirm("point is zero", shouldBeZero.is_zero());

   Botan::BigInt y1 = p1.get_affine_y();
   y1 = secp160r1.get_p() - y1;

   result.test_eq("minus point x", minus_p1.get_affine_x(), p1.get_affine_x());
   result.test_eq("minus point y", minus_p1.get_affine_y(), y1);

   Botan::PointGFp zero = secp160r1.zero_point();
   result.confirm("zero point is on the curve", zero.on_the_curve());
   result.test_eq("addition of zero does nothing", p1, p1 + zero);

   return result;
   }

Test::Result test_mult_by_order()
   {
   Test::Result result("ECC Unit");

   // generate point
   Botan::EC_Group dom_pars("secp160r1");
   Botan::PointGFp p = dom_pars.get_base_point();
   Botan::PointGFp shouldBeZero = p * dom_pars.get_order();

   result.confirm("G * order = 0", shouldBeZero.is_zero());
   return result;
   }

Test::Result test_point_swap()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group dom_pars("secp160r1");

   Botan::PointGFp a(create_random_point(Test::rng(), dom_pars));
   Botan::PointGFp b(create_random_point(Test::rng(), dom_pars));
   b *= Botan::BigInt(Test::rng(), 20);

   Botan::PointGFp c(a);
   Botan::PointGFp d(b);

   d.swap(c);
   result.test_eq("swap correct", a, d);
   result.test_eq("swap correct", b, c);

   return result;
   }

/**
* This test verifies that the side channel attack resistant multiplication function
* yields the same result as the normal (insecure) multiplication via operator*=
*/
Test::Result test_mult_sec_mass()
   {
   Test::Result result("ECC Unit");

   Botan::EC_Group dom_pars("secp160r1");
   for(int i = 0; i < 50; i++)
      {
      try
         {
         Botan::PointGFp a(create_random_point(Test::rng(), dom_pars));
         Botan::BigInt scal(Botan::BigInt(Test::rng(), 40));
         Botan::PointGFp b = a * scal;
         Botan::PointGFp c(a);

         c *= scal;
         result.test_eq("same result", b, c);
         }
      catch(std::exception& e)
         {
         result.test_failure("mult_sec_mass", e.what());
         }
      }

   return result;
   }

Test::Result test_ecc_registration()
   {
   Test::Result result("ECC registration");

   // secp112r1
   const Botan::BigInt p("0xDB7C2ABF62E35E668076BEAD208B");
   const Botan::BigInt a("0xDB7C2ABF62E35E668076BEAD2088");
   const Botan::BigInt b("0x659EF8BA043916EEDE8911702B22");

   const Botan::BigInt g_x("0x09487239995A5EE76B55F9C2F098");
   const Botan::BigInt g_y("0xA89CE5AF8724C0A23E0E0FF77500");
   const Botan::BigInt order("0xDB7C2ABF62E35E7628DFAC6561C5");

   const Botan::OID oid("1.3.132.0.6");

   // Creating this object implicitly registers the curve for future use ...
   Botan::EC_Group reg_group(p, a, b, g_x, g_y, order, 1, oid);

   Botan::EC_Group group(oid);

   result.test_eq("Group registration worked", group.get_p(), p);

   return result;
   }

class ECC_Unit_Tests final : public Test
   {
   public:
      std::vector<Test::Result> run() override
         {
         std::vector<Test::Result> results;

         results.push_back(test_groups());
         results.push_back(test_coordinates());
         results.push_back(test_point_transformation());
         results.push_back(test_point_mult());
         results.push_back(test_point_negative());
         results.push_back(test_zeropoint());
         results.push_back(test_zeropoint_enc_dec());
         results.push_back(test_calc_with_zeropoint());
         results.push_back(test_add_point());
         results.push_back(test_sub_point());
         results.push_back(test_mult_point());
         results.push_back(test_basic_operations());
         results.push_back(test_enc_dec_compressed_160());
         results.push_back(test_enc_dec_compressed_256());
         results.push_back(test_enc_dec_uncompressed_112());
         results.push_back(test_enc_dec_uncompressed_521());
         results.push_back(test_gfp_store_restore());
         results.push_back(test_more_zeropoint());
         results.push_back(test_mult_by_order());
         results.push_back(test_point_swap());
         results.push_back(test_mult_sec_mass());
         results.push_back(test_ecc_registration());

         return results;
         }
   };

BOTAN_REGISTER_TEST("ecc_unit", ECC_Unit_Tests);

#if defined(BOTAN_HAS_ECDSA)

class ECC_Invalid_Key_Tests final : public Text_Based_Test
   {
   public:
      ECC_Invalid_Key_Tests() :
         Text_Based_Test("pubkey/ecc_invalid.vec", "SubjectPublicKey") {}

      bool clear_between_callbacks() const override
         {
         return false;
         }

      Test::Result run_one_test(const std::string&, const VarMap& vars) override
         {
         Test::Result result("ECC invalid keys");

         const std::string encoded = get_req_str(vars, "SubjectPublicKey");
         Botan::DataSource_Memory key_data(Botan::hex_decode(encoded));

         try
            {
            std::unique_ptr<Botan::Public_Key> key(Botan::X509::load_key(key_data));
            result.test_eq("public key fails check", key->check_key(Test::rng(), false), false);
            }
         catch(Botan::Decoding_Error&)
            {
            result.test_success("Decoding invalid ECC key results in decoding error exception");
            }

         return result;
         }
   };

BOTAN_REGISTER_TEST("ecc_invalid", ECC_Invalid_Key_Tests);

#endif

#endif

}

}
