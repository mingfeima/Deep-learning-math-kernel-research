#pragma once
#include <x86intrin.h>
#include "el_intrin.hpp"
#include "elk_def.hpp"
#include "el_utils.hpp"
#include "elk_conv_wino.hpp"
#include <math.h>

namespace euler {

#define AVX512_CALCULATE_I_5(n)                                                \
  t0##n = -ADD(FMSUB(z17_10, c3, ADD(c1, c2)), FMSUB(z17_5, c4, ADD(c5, c6))); \
  _mm<V>::store_ps(T(0, n), t0##n);                                            \
  t1##n = SUB(FMADD(z17_5, c4, SUB(c5, c6)), FMSUB(z17_10, c3, SUB(c1, c2)));  \
  _mm<V>::store_ps(T(1, n), t1##n);                                            \
  t2##n = -ADD(                                                                \
      ADD(FMSUB(z1_2, c1, c3), FMSUB(z1_4, c2, c4)), FMADD(z2, c5, c6));       \
  _mm<V>::store_ps(T(2, n), t2##n);                                            \
  t3##n                                                                        \
      = ADD(SUB(FMSUB(z1_2, c1, c3), FMSUB(z1_4, c2, c4)), FMSUB(z2, c5, c6)); \
  _mm<V>::store_ps(T(3, n), t3##n);                                            \
  t4##n = SUB(FMSUB(z4, SUB(c4, c2), FMADD(z1_2, c5, c6)), FMSUB(z2, c1, c3)); \
  _mm<V>::store_ps(T(4, n), t4##n);                                            \
  t5##n = ADD(FMADD(z4, SUB(c4, c2), FMSUB(z1_2, c5, c6)), FMSUB(z2, c1, c3)); \
  _mm<V>::store_ps(T(5, n), t5##n);

template <typename InputType, int format, bool is_border,
    int V>
struct elk_conv_wino_trans_input<float, InputType, format, is_border,
    ISA_SKX_AVX512, 7, V> {
  constexpr static int A = 7;

  static void execute(elx_conv_params_t &xc, float *tinput,
      InputType *input, int hA_start, int hA_end, int wA_start, int wA_end)
  {
    ENABLE_AVX512F();
    MD3(float, atinput, tinput, A, A, V);

    // Inputs
    __m<V> f00, f01, f02, f03, f04, f05, f06, f10, f11, f12, f13, f14, f15, f16,
        f20, f21, f22, f23, f24, f25, f26, f30, f31, f32, f33, f34, f35, f36,
        f40, f41, f42, f43, f44, f45, f46, f50, f51, f52, f53, f54, f55, f56,
        f60, f61, f62, f63, f64, f65, f66;
    // Cache
    __m<V> c1, c2, c3, c4, c5, c6;
    // Outputs
    __m<V> t00, t01, t02, t03, t04, t05, t06, t10, t11, t12, t13, t14, t15, t16,
        t20, t21, t22, t23, t24, t25, t26, t30, t31, t32, t33, t34, t35, t36,
        t40, t41, t42, t43, t44, t45, t46, t50, t51, t52, t53, t54, t55, t56,
        t60, t61, t62, t63, t64, t65, t66;

    __m<V> z0 = _mm<V>::setzero_ps();
    __m<V> z2 = _mm<V>::set_ps(IMM_BCAST16(2.0f));
    __m<V> z4 = _mm<V>::set_ps(IMM_BCAST16(4.0f));
    __m<V> z1_2 = _mm<V>::set_ps(IMM_BCAST16(1.0f / 2.0f));
    __m<V> z1_4 = _mm<V>::set_ps(IMM_BCAST16(1.0f / 4.0f));
    __m<V> z5_2 = _mm<V>::set_ps(IMM_BCAST16(5.0f / 2.0f));
    __m<V> z5_4 = _mm<V>::set_ps(IMM_BCAST16(5.0f / 4.0f));
    __m<V> z17_4 = _mm<V>::set_ps(IMM_BCAST16(17.0f / 4.0f));
    __m<V> z17_5 = _mm<V>::set_ps(IMM_BCAST16(17.0f / 5.0f));
    __m<V> z17_10 = _mm<V>::set_ps(IMM_BCAST16(17.0f / 10.0f));
    __m<V> z21_4 = _mm<V>::set_ps(IMM_BCAST16(21.0f / 4.0f));
    __m<V> z21_10 = _mm<V>::set_ps(IMM_BCAST16(21.0f / 10.0f));
    __m<V> z85_8 = _mm<V>::set_ps(IMM_BCAST16(85.0f / 8.0f));
    __m<V> z85_16 = _mm<V>::set_ps(IMM_BCAST16(85.0f / 16.0f));

    auto f_cb = [&](int _h, int _w) {
      if (format == TKF_COMPACT) {
        MD3(InputType, ainput, input, A, A, V);
        if (std::is_same<InputType, float>::value)
          return _mm<V>::load_ps(&md3(ainput, _h, _w, 0));
        else {
          auto f16 = _mm<V / 2>::load_si256((__m256i *)&md3(ainput, _h, _w, 0));
          return _mm<V>::cvtph_ps(f16);
        }
      } else if (format == TKF_BLOCKED) {
        MD3(InputType, ainput, input, xc.ih, xc.iw, V);
        if (is_border
            && (_h < hA_start || _w < wA_start || _h > hA_end || _w > wA_end))
          return z0;
        else if (std::is_same<InputType, float>::value)
          return _mm<V>::load_ps(&md3(ainput, _h, _w, 0));
        else {
          auto f16 = _mm<V / 2>::load_si256((__m256i *)&md3(ainput, _h, _w, 0));
          return _mm<V>::cvtph_ps(f16);
        }
      } else { // TKF_NHWC
        MD3(InputType, ainput0, input, xc.ih, xc.iw, xc.ic);
        // TODO: overflow on last V
        MD2(InputType, ainput1, &md3(ainput0, _h, _w, 0),
            xc.ic4 * xc.ic3 * xc.I2, V);
        if (is_border
            && (_h < hA_start || _w < wA_start || _h > hA_end || _w > wA_end))
          return z0;
        else if (std::is_same<InputType, float>::value)
          return _mm<V>::load_ps(&md2(ainput1, 0, 0));
        else {
          auto f16 = _mm<V / 2>::load_si256((__m256i *)&md2(ainput1, 0, 0));
          return _mm<V>::cvtph_ps(f16);
        }
      }
    };

#undef F
#undef C
#undef T
#define F(_h, _w) f_cb(_h, _w)
#define T(h, w) (&md3(atinput, h, w, 0))

#undef f
#undef OP
#define f(m, n) f##m##n
#define OP(m, n) f(m, n) = F(m, n)

    MATRIX_DEF(7, 6);

    c1 = SUB(ADD(f00, f01), FMSUB(z17_4, ADD(f02, f03), ADD(f04, f05)));
    c2 = SUB(ADD(f10, f11), FMSUB(z17_4, ADD(f12, f13), ADD(f14, f15)));
    c3 = FMSUB(
        z5_2, ADD(ADD(f20, f21), ADD(f24, f25)), MUL(z85_8, ADD(f22, f23)));
    c4 = FMSUB(
        z5_4, ADD(ADD(f30, f31), ADD(f34, f35)), MUL(z85_16, ADD(f32, f33)));
    c5 = SUB(ADD(f40, f41), FMSUB(z17_4, ADD(f42, f43), ADD(f44, f45)));
    c6 = SUB(ADD(f50, f51), FMSUB(z17_4, ADD(f52, f53), ADD(f54, f55)));
    AVX512_CALCULATE_I_5(0)
    t60 = SUB(ADD(FMSUB(z21_4, c5, ADD(f60, f61)),
                  FMSUB(z17_4, ADD(f62, f63), ADD(f64, f65))),
        FMSUB(z21_10, c3, c1));
    _mm<V>::store_ps(T(6, 0), t60);

    c1 = ADD(SUB(f00, f01), FMADD(z17_4, SUB(f03, f02), SUB(f04, f05)));
    c2 = ADD(SUB(f10, f11), FMADD(z17_4, SUB(f13, f12), SUB(f14, f15)));
    c3 = FMADD(
        z5_2, ADD(SUB(f20, f21), SUB(f24, f25)), MUL(z85_8, SUB(f23, f22)));
    c4 = FMADD(
        z5_4, ADD(SUB(f30, f31), SUB(f34, f35)), MUL(z85_16, SUB(f33, f32)));
    c5 = ADD(SUB(f40, f41), FMADD(z17_4, SUB(f43, f42), SUB(f44, f45)));
    c6 = ADD(SUB(f50, f51), FMADD(z17_4, SUB(f53, f52), SUB(f54, f55)));
    AVX512_CALCULATE_I_5(1)
    t61 = SUB(ADD(FMSUB(z21_4, c5, SUB(f60, f61)),
                  FMSUB(z17_4, SUB(f62, f63), SUB(f64, f65))),
        FMSUB(z21_10, c3, c1));
    _mm<V>::store_ps(T(6, 1), t61);

    __m<V> z5 = _mm<V>::set_ps(IMM_BCAST16(5.0f));
    __m<V> z5_8 = _mm<V>::set_ps(IMM_BCAST16(5.0f / 8.0f));
    __m<V> z5_16 = _mm<V>::set_ps(IMM_BCAST16(5.0f / 16.0f));
    __m<V> z25_4 = _mm<V>::set_ps(IMM_BCAST16(25.0f / 4.0f));
    __m<V> z25_8 = _mm<V>::set_ps(IMM_BCAST16(25.0f / 8.0f));
    __m<V> z25_16 = _mm<V>::set_ps(IMM_BCAST16(25.0f / 16.0f));

    c1 = FMADD(z5_2, f02,
        FMSUB(z5_4, f03,
            FMADD(z1_2, f00, FMADD(z1_4, f01, FMADD(z2, f04, f05)))));
    c2 = FMADD(z5_2, f12,
        FMSUB(z5_4, f13,
            FMADD(z1_2, f10, FMADD(z1_4, f11, FMADD(z2, f14, f15)))));
    c3 = FMADD(z25_4, f22,
        FMSUB(z25_8, f23,
            FMADD(
                z5_4, f20, FMADD(z5_8, f21, FMADD(z5, f24, MUL(z5_2, f25))))));
    c4 = FMADD(z25_8, f32,
        FMSUB(z25_16, f33,
            FMADD(z5_8, f30,
                FMADD(z5_16, f31, FMADD(z5_2, f34, MUL(z5_4, f35))))));
    c5 = FMADD(z5_2, f42,
        FMSUB(z5_4, f43,
            FMADD(z1_2, f40, FMADD(z1_4, f41, FMADD(z2, f44, f45)))));
    c6 = FMADD(z5_2, f52,
        FMSUB(z5_4, f53,
            FMADD(z1_2, f50, FMADD(z1_4, f51, FMADD(z2, f54, f55)))));
    AVX512_CALCULATE_I_5(2)
    t62 = FMADD(z21_4, c5,
        FMADD(z1_2, f60,
            FMADD(z1_4, f61,
                FMSUB(z2, f64,
                    FMADD(z21_10, c3,
                        FMADD(z5_2, f62, FMSUB(z5_4, f63, ADD(c1, f65))))))));
    _mm<V>::store_ps(T(6, 2), t62);

    c1 = FMADD(z1_2, f00,
        FMADD(z5_4, f03,
            FMSUB(z2, f04, FMADD(z1_4, f01, FMADD(z5_2, f02, f05)))));
    c2 = FMADD(z1_2, f10,
        FMADD(z5_4, f13,
            FMSUB(z2, f14, FMADD(z1_4, f11, FMADD(z5_2, f12, f15)))));
    c3 = FMADD(z5_4, f20,
        FMADD(z25_8, f23,
            FMSUB(
                z5, f24, FMADD(z5_8, f21, FMADD(z25_4, f22, MUL(z5_2, f25))))));
    c4 = FMADD(z5_8, f30,
        FMADD(z25_16, f33,
            FMSUB(z5_2, f34,
                FMADD(z5_16, f31, FMADD(z25_8, f32, MUL(z5_4, f35))))));
    c5 = FMADD(z1_2, f40,
        FMADD(z5_4, f43,
            FMSUB(z2, f44, FMADD(z1_4, f41, FMADD(z5_2, f42, f45)))));
    c6 = FMADD(z1_2, f50,
        FMADD(z5_4, f53,
            FMSUB(z2, f54, FMADD(z1_4, f51, FMADD(z5_2, f52, f55)))));
    AVX512_CALCULATE_I_5(3)
    t63 = FMADD(z21_4, c5,
        FMADD(z1_4, f61,
            FMSUB(z5_2, f62,
                FMADD(z21_10, c3,
                    FMADD(z1_2, f60,
                        FMADD(z5_4, f63, FMSUB(z2, f64, ADD(c1, f65))))))));
    _mm<V>::store_ps(T(6, 3), t63);

    __m<V> z10 = _mm<V>::set_ps(IMM_BCAST16(10.0f));
    __m<V> z25_2 = _mm<V>::set_ps(IMM_BCAST16(25.0f / 2.0f));

    c1 = FMADD(z5_2, f02,
        FMSUB(z5, f03, FMADD(z2, f00, FMADD(z4, f01, FMADD(z1_2, f04, f05)))));
    c2 = FMADD(z5_2, f12,
        FMSUB(z5, f13, FMADD(z2, f10, FMADD(z4, f11, FMADD(z1_2, f14, f15)))));
    c3 = FMADD(z25_4, f22,
        FMSUB(z25_2, f23,
            FMADD(z5, f20, FMADD(z10, f21, FMADD(z5_4, f24, MUL(z5_2, f25))))));
    c4 = FMADD(z25_8, f32,
        FMSUB(z25_4, f33,
            FMADD(
                z5_2, f30, FMADD(z5, f31, FMADD(z5_8, f34, MUL(z5_4, f35))))));
    c5 = FMADD(z5_2, f42,
        FMSUB(z5, f43, FMADD(z2, f40, FMADD(z4, f41, FMADD(z1_2, f44, f45)))));
    c6 = FMADD(z5_2, f52,
        FMSUB(z5, f53, FMADD(z2, f50, FMADD(z4, f51, FMADD(z1_2, f54, f55)))));
    AVX512_CALCULATE_I_5(4)
    t64 = FMADD(z21_4, c5,
        FMADD(z2, f60,
            FMADD(z4, f61,
                FMSUB(z1_2, f64,
                    FMADD(z21_10, c3,
                        FMADD(z5_2, f62, FMSUB(z5, f63, ADD(c1, f65))))))));
    _mm<V>::store_ps(T(6, 4), t64);

    c1 = FMADD(z2, f00,
        FMADD(
            z5, f03, FMSUB(z1_2, f04, FMADD(z4, f01, FMADD(z5_2, f02, f05)))));
    c2 = FMADD(z2, f10,
        FMADD(
            z5, f13, FMSUB(z1_2, f14, FMADD(z4, f11, FMADD(z5_2, f12, f15)))));
    c3 = FMADD(z5, f20,
        FMADD(z25_2, f23,
            FMSUB(z5_4, f24,
                FMADD(z10, f21, FMADD(z25_4, f22, MUL(z5_2, f25))))));
    c4 = FMADD(z5_2, f30,
        FMADD(z25_4, f33,
            FMSUB(
                z5_8, f34, FMADD(z5, f31, FMADD(z25_8, f32, MUL(z5_4, f35))))));
    c5 = FMADD(z2, f40,
        FMADD(
            z5, f43, FMSUB(z1_2, f44, FMADD(z4, f41, FMADD(z5_2, f42, f45)))));
    c6 = FMADD(z2, f50,
        FMADD(
            z5, f53, FMSUB(z1_2, f54, FMADD(z4, f51, FMADD(z5_2, f52, f55)))));
    AVX512_CALCULATE_I_5(5)
    t65 = FMADD(z21_4, c5,
        FMADD(z4, f61,
            FMSUB(z5_2, f62,
                FMADD(z21_10, c3,
                    FMADD(z2, f60,
                        FMADD(z5, f63, FMSUB(z1_2, f64, ADD(c1, f65))))))));
    _mm<V>::store_ps(T(6, 5), t65);

    f06 = F(0, 6);
    f16 = F(1, 6);
    f26 = F(2, 6);
    f36 = F(3, 6);
    f46 = F(4, 6);
    f56 = F(5, 6);
    f66 = F(6, 6);
    __m<V> z105_8 = _mm<V>::set_ps(IMM_BCAST16(105.0f / 8.0f));
    __m<V> z105_16 = _mm<V>::set_ps(IMM_BCAST16(105.0f / 16.0f));

    c1 = FMADD(z21_4, SUB(f04, f02), SUB(f00, f06));
    c2 = FMADD(z21_4, SUB(f14, f12), SUB(f10, f16));
    c3 = FMADD(z5_2, SUB(f20, f26), MUL(z105_8, SUB(f24, f22)));
    c4 = FMADD(z5_4, SUB(f30, f36), MUL(z105_16, SUB(f34, f32)));
    c5 = FMADD(z21_4, SUB(f44, f42), SUB(f40, f46));
    c6 = FMADD(z21_4, SUB(f54, f52), SUB(f50, f56));
    AVX512_CALCULATE_I_5(6)
    t66 = SUB(FMADD(z21_4, SUB(ADD(c5, f62), f64), ADD(c1, f66)),
        FMADD(z21_10, c3, f60));
    _mm<V>::store_ps(T(6, 6), t66);
  }
}; // elk_conv_wino_trans_input
} // namespace euler
