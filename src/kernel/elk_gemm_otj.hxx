#pragma once

#include "el_intrin.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "el_stl.hpp"
#include "elx_conv.hpp"

// S: stride
// O: OC blocking unit
// T: tile blocking unit
// F: format
// V: vector size
// Vx: packed size of data with InputType
// I: ISA
// has_Ir: has tailing ic

namespace euler {

// GEMM kernel format
// Input - weights - output
// C: compact
//    Input: I2, T, S, V, Vx
//    Weights: O1, I2, V, O, V, Vx
//    Output: O1, O, T, V
//    factor: O1, O, V
//    weights_scale: O1, O, V
// D: discrete
//    Input: I2, ih, iw, V, Vx
//    Weights: O1, O, ic2, V, V, Vx
//    Output: O1, O, oh, ow, V
//    factor: O1, O, V
//    weights_scale: O1, O, V
const int GKF_CCC = 0xccc;
const int GKF_CCD = 0xccd;
const int GKF_DCD = 0xdcd;
const int GKF_DDD = 0xddd;

// (weights) pipeline length
template <int O, int T, bool has_Ir, typename Wtype, typename C = void>
struct P_traits {};

// if Wtype = fp32 || fp16
//   O == 1: T + P <= 32
//   O > 1: O (T + P) + 1 <= 32
// if Wtype = int8_t
//   O == 1: T + P + 1(one) + 1(t0) <= 32
//   O > 1: O (T + P) + 1(bcast) + 1(one) + 1(t0) <= 32
template <int T>
struct P_traits<1, T, false, float,
    typename std::enable_if<(T <= 28)>::type> {
  static constexpr int P = 4;
};

template <int T>
struct P_traits<1, T, false, float16,
    typename std::enable_if<(T <= 28)>::type> {
  static constexpr int P = 4;
};

template <int T>
struct P_traits<1, T, false, int8_t,
    typename std::enable_if<(T <= 26)>::type> {
  static constexpr int P = 4;
};

template <int T>
struct P_traits<1, T, false, float,
    typename std::enable_if<(T == 29 || T == 30)>::type> {
  static constexpr int P = 2;
};

template <int T>
struct P_traits<1, T, false, float16,
    typename std::enable_if<(T == 29 || T == 30)>::type> {
  static constexpr int P = 2;
};

template <int T>
struct P_traits<1, T, false, int8_t,
    typename std::enable_if<(T == 27 || T == 28)>::type> {
  static constexpr int P = 2;
};

template <int T>
struct P_traits<1, T, false, float,
    typename std::enable_if<(T >= 31)>::type> {
  static constexpr int P = 1;
};

template <int T>
struct P_traits<1, T, false, float16,
    typename std::enable_if<(T >= 31)>::type> {
  static constexpr int P = 1;
};

template <int T>
struct P_traits<1, T, false, int8_t,
    typename std::enable_if<(T >= 29)>::type> {
  static constexpr int P = 1;
};

template <int O, int T>
struct P_traits<O, T, false, float,
    typename std::enable_if<(O > 1 && (31 / O - T) >= 4)>::type> {
  static constexpr int P = 4;
};

template <int O, int T>
struct P_traits<O, T, false, float16,
    typename std::enable_if<(O > 1 && (31 / O - T) >= 4)>::type> {
  static constexpr int P = 4;
};

template <int O, int T>
struct P_traits<O, T, false, int8_t,
    typename std::enable_if<(O > 1 && (29 / O - T) >= 4)>::type> {
  static constexpr int P = 4;
};

template <int O, int T>
struct P_traits<O, T, false, float,
    typename std::enable_if<(
        O > 1 && (31 / O - T == 2 || 31 / O - T == 3))>::type> {
  static constexpr int P = 2;
};

template <int O, int T>
struct P_traits<O, T, false, float16,
    typename std::enable_if<(
        O > 1 && (31 / O - T == 2 || 31 / O - T == 3))>::type> {
  static constexpr int P = 2;
};

template <int O, int T>
struct P_traits<O, T, false, int8_t,
    typename std::enable_if<(
        O > 1 && (29 / O - T == 2 || 29 / O - T == 3))>::type> {
  static constexpr int P = 2;
};

template <int O, int T>
struct P_traits<O, T, false, float,
    typename std::enable_if<(O > 1 && (31 / O - T) == 1)>::type> {
  static constexpr int P = 1;
};

template <int O, int T>
struct P_traits<O, T, false, float16,
    typename std::enable_if<(O > 1 && (31 / O - T) == 1)>::type> {
  static constexpr int P = 1;
};

template <int O, int T>
struct P_traits<O, T, false, int8_t,
    typename std::enable_if<(O > 1 && (29 / O - T) <= 1)>::type> {
  static constexpr int P = 1;
};

template <int O, int T, typename Wtype>
struct P_traits<O, T, true, Wtype> {
  static constexpr int P = 1;
};

// Jamming
template <int O, int T, bool has_Ir, typename Wtype = float, typename C = void>
struct J_traits {};

template <int T, bool has_Ir, typename Wtype>
struct J_traits<8, T, has_Ir, Wtype, typename std::enable_if<T == 6>::type> {
  static constexpr int J = 2;
  static constexpr int O0 = 4;
  static constexpr int O1 = 4;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, Wtype>::P;
  static constexpr int P1 = P_traits<O1, T, has_Ir, Wtype>::P;
  static constexpr int P2 = 0;
};

template <int T, bool has_Ir, typename Wtype>
struct J_traits<8, T, has_Ir, Wtype,
    typename std::enable_if<T == 7 || T == 8, void>::type> {
  static constexpr int J = 3;
  static constexpr int O0 = 3;
  static constexpr int O1 = 3;
  static constexpr int O2 = 2;
  static constexpr int P0 = P_traits<O0, T, has_Ir, Wtype>::P;
  static constexpr int P1 = P_traits<O1, T, has_Ir, Wtype>::P;
  static constexpr int P2 = P_traits<O2, T, has_Ir, Wtype>::P;
};

template <int T, bool has_Ir, typename Wtype>
struct J_traits<8, T, has_Ir, Wtype,
    typename std::enable_if<(T >= 3 && T < 6), void>::type> {
  static constexpr int J = 2;
  static constexpr int O0 = 4;
  static constexpr int O1 = 4;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, Wtype>::P;
  static constexpr int P1 = P_traits<O1, T, has_Ir, Wtype>::P;
  static constexpr int P2 = 0;
};

template <int T, bool has_Ir, typename Wtype>
struct J_traits<4, T, has_Ir, Wtype,
    typename std::enable_if<(T >= 7 && T < 15), void>::type> {
  static constexpr int J = 2;
  static constexpr int O0 = 2;
  static constexpr int O1 = 2;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, Wtype>::P;
  static constexpr int P1 = P_traits<O1, T, has_Ir, Wtype>::P;
  static constexpr int P2 = 0;
};

template <int T, bool has_Ir, typename Wtype>
struct J_traits<3, T, has_Ir, Wtype,
    typename std::enable_if<(T >= 10 && T < 15), void>::type> {
  static constexpr int J = 2;
  static constexpr int O0 = 2;
  static constexpr int O1 = 1;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, Wtype>::P;
  static constexpr int P1 = P_traits<O1, T, has_Ir, Wtype>::P;
  static constexpr int P2 = 0;
};

template <int O, int T, bool has_Ir>
struct J_traits<O, T, has_Ir, float,
    typename std::enable_if<((O == 1 && T < 32)) || (O == 2 && T < 15)
        || (O == 3 && T < 10) || (O == 4 && T < 7) || (O == 5 && T < 6)
        || (O == 6 && T < 5) || (O == 7 && T < 4) || (O == 8 && T < 3)>::type> {
  static constexpr int J = 1;
  static constexpr int O0 = O;
  static constexpr int O1 = 0;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, float>::P;
  static constexpr int P1 = 0;
  static constexpr int P2 = 0;
};

template <int O, int T, bool has_Ir>
struct J_traits<O, T, has_Ir, float16,
    typename std::enable_if<((O == 1 && T < 32)) || (O == 2 && T < 15)
        || (O == 3 && T < 10) || (O == 4 && T < 7) || (O == 5 && T < 6)
        || (O == 6 && T < 5) || (O == 7 && T < 4) || (O == 8 && T < 3)>::type> {
  static constexpr int J = 1;
  static constexpr int O0 = O;
  static constexpr int O1 = 0;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, float16>::P;
  static constexpr int P1 = 0;
  static constexpr int P2 = 0;
};

template <int O, int T, bool has_Ir>
struct J_traits<O, T, has_Ir, int8_t,
    typename std::enable_if<((O == 1 && T < 32)) || (O == 2 && T < 15)
        || (O == 3 && T < 10) || (O == 4 && T < 7) || (O == 5 && T < 6)
        || (O == 6 && T < 5) || (O == 7 && T < 4) || (O == 8 && T < 3)>::type> {
  static constexpr int J = 1;
  static constexpr int O0 = O;
  static constexpr int O1 = 0;
  static constexpr int O2 = 0;
  static constexpr int P0 = P_traits<O0, T, has_Ir, int8_t>::P;
  static constexpr int P1 = 0;
  static constexpr int P2 = 0;
};

template <int F>
struct F_traits {
  static constexpr bool is_compact_input = (F & 0xF00) == 0xC00;
  static constexpr bool is_compact_weights = (F & 0xF0) == 0xC0;
  static constexpr bool is_compact_output = (F & 0xF) == 0xC;
};

template <typename GarrayTypes, int V, int Vx, int I, typename KP>
struct gemm_kernel_otj {
  static inline void execute(
      elx_conv_params_t &, typename GarrayTypes::OutputType *,
      typename GarrayTypes::InputType *,
      typename GarrayTypes::WeightsType *,
      typename GarrayTypes::BiasType *, int,
      typename GarrayTypes::ScaleType *,
      typename GarrayTypes::ScaleType *,
      typename GarrayTypes::ScaleType *) {}
};

template <typename GarrayTypes, int V, int Vx, int ...Kp>
struct gemm_kernel_otj<GarrayTypes, V, Vx, ISA_SKX_AVX512,
    estl::integer_sequence<Kp...>> {
  using kparams = estl::integer_sequence<Kp...>;
  static_assert(sizeof...(Kp) == 5,
      "Kernel parameters must be GarrayTypes, V, Vx, I, <S, F, O, T, has_Ir>");

  using InputType = typename GarrayTypes::InputType;
  using WeightsType = typename GarrayTypes::WeightsType;
  using OutputType = typename GarrayTypes::OutputType;
  using BiasType = typename GarrayTypes::BiasType;
  using ScaleType = typename GarrayTypes::ScaleType;

  constexpr static auto S = estl::get<0, int, kparams>();
  constexpr static auto F = estl::get<1, int, kparams>();
  constexpr static auto O = estl::get<2, int, kparams>();
  constexpr static auto T = estl::get<3, int, kparams>();
  constexpr static auto has_Ir = estl::get<4, bool, kparams>();

  // Jamming components
  constexpr static int J = J_traits<O, T, has_Ir, WeightsType>::J;
  constexpr static int JO0 = J_traits<O, T, has_Ir, WeightsType>::O0;
  constexpr static int JP0 = J_traits<O, T, has_Ir, WeightsType>::P0;
  constexpr static int JO1 = J_traits<O, T, has_Ir, WeightsType>::O1;
  constexpr static int JP1 = J_traits<O, T, has_Ir, WeightsType>::P1;
  constexpr static int JO2 = J_traits<O, T, has_Ir, WeightsType>::O2;
  constexpr static int JP2 = J_traits<O, T, has_Ir, WeightsType>::P2;

  static inline __i<V> __op_int8_fma(__i<V>& out, __i<V>& a, __i<V>& b) {
    // TODO: check ISA
#if defined(WITH_VNNI)
    out = _mm512_dpbusds_epi32(out, a, b);
#else
    __i<V> one = _mm<V>::set1_epi16(1);
    __i<V> t0 = _mm<V>::maddubs_epi16(a, b);
    t0 = _mm<V>::madd_epi16(t0, one);
    out = _mm<V>::add_epi32(t0, out);
#endif
    return out;
  }

  // f32f32f32 fma
  template <int JO, int P>
  static inline typename std::enable_if<
      !std::is_same<InputType, uint8_t>::value
      && (P == 1 && has_Ir == false), void>::type
  op_fma(elx_conv_params_t &xc,
      OutputType *output, InputType *input, WeightsType *weights, BiasType *bias,
      int attr, ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor,
      int _O1, int _O0)
  {
    static_assert(std::is_same<InputType, float>::value,
        "only fp32 input type");
    static_assert(std::is_same<WeightsType, float>::value
        || std::is_same<WeightsType, float16>::value,
        "only fp32/fp16 weights type");
    static_assert(std::is_same<OutputType, float>::value
        || std::is_same<OutputType, float16>::value,
        "only fp32/fp16 output type");
    static_assert(std::is_same<BiasType, float>::value
        || std::is_same<BiasType, float16>::value,
        "only fp32/fp16 bias type");

    __m<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V : xc.ih * xc.iw * V;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(OutputType, aoutput, output, JO, O_stride);
    MD2(InputType, ainput, input, xc.I2, I2_stride);
    MD2(BiasType, abias2, bias, JO, V);

    if (get_attr(attr, r_output_idx)) {
      if (get_attr(attr, bias_idx)) {
        // load bias
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          __m<V> tmp;
          if (std::is_same<BiasType, float>::value) {
            tmp = _mm<V>::load_ps(&md2(abias2, _O, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(abias2, _O, 0));
            tmp = _mm<V>::cvtph_ps(fp16v);
          }
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
        }
      } else {
        // clear output
        __m<V> tmp = _mm<V>::setzero_ps();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
      }
      // load output
      if (get_attr(attr, ip_sum_idx)) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T) {
            MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
            if (std::is_same<OutputType, float>::value) {
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::load_ps(&md2(aoutput2, _T, 0)));
            } else {
              auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::cvtph_ps(fp16v));
            }
          }
        }
      }
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
          if (std::is_same<OutputType, float>::value) {
            mmout[_O][_T] = _mm<V>::load_ps(&md2(aoutput2, _T, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
            mmout[_O][_T] =  _mm<V>::cvtph_ps(fp16v);
          }
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0] = _mm<V>::load_ps(&md5(aweights5, _I2, _V, 0, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V, 0, _O, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V, 0, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V, 0, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
            if (std::is_same<InputType, float>::value) {
              __m<V> mmbcst = _mm<V>::set1_ps(md4(ainput4, _T, 0, _V, 0));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            } else {
              __m<V> mmbcst = _mm<V>::set1_ps(
                  half_2_float(md4(ainput4, _T, 0, _V, 0)));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            }
          }
        }
      }
    }

    // store output
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
        if (get_attr(attr, relu_idx)) {
          __m<V> zero = _mm<V>::setzero_ps();
          mmout[_O][_T] = _mm<V>::max_ps(mmout[_O][_T], zero);
        }
        if (get_attr(attr, s_output_idx)) {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::stream_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::stream_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        } else {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::store_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::store_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        }
      }
    }
  }

  template <int JO, int P>
  static inline typename std::enable_if<
      !std::is_same<InputType, uint8_t>::value
      && (P == 1 && has_Ir == true), void>::type
  op_fma(elx_conv_params_t &xc,
      OutputType *output, InputType *input, WeightsType *weights, BiasType *bias,
      int attr, ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor,
      int _O1, int _O0)
  {
    static_assert(std::is_same<InputType, float>::value,
        "only fp32 input type");
    static_assert(std::is_same<WeightsType, float>::value
        || std::is_same<WeightsType, float16>::value,
        "only fp32/fp16 weights type");
    static_assert(std::is_same<OutputType, float>::value
        || std::is_same<OutputType, float16>::value,
        "only fp32/fp16 output type");
    static_assert(std::is_same<BiasType, float>::value
        || std::is_same<BiasType, float16>::value,
        "only fp32/fp16 bias type");

    __m<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V : xc.ih * xc.iw * V;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(OutputType, aoutput, output, JO, O_stride);
    MD2(InputType, ainput, input, xc.I2, I2_stride);
    MD2(BiasType, abias2, bias, JO, V);

    if (get_attr(attr, r_output_idx)) {
      if (get_attr(attr, bias_idx)) {
        // load bias
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          __m<V> tmp;
          if (std::is_same<BiasType, float>::value) {
            tmp = _mm<V>::load_ps(&md2(abias2, _O, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(abias2, _O, 0));
            tmp = _mm<V>::cvtph_ps(fp16v);
          }
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
        }
      } else {
        // clear output
        __m<V> tmp = _mm<V>::setzero_ps();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
      }
      // load output
      if (get_attr(attr, ip_sum_idx)) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T) {
            MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
            if (std::is_same<OutputType, float>::value) {
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::load_ps(&md2(aoutput2, _T, 0)));
            } else {
              auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::cvtph_ps(fp16v));
            }
          }
        }
      }
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
          if (std::is_same<OutputType, float>::value) {
            mmout[_O][_T] = _mm<V>::load_ps(&md2(aoutput2, _T, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
            mmout[_O][_T] =  _mm<V>::cvtph_ps(fp16v);
          }
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2 - 1; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V, 1, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0] = _mm<V>::load_ps(&md5(aweights5, _I2, _V, 0, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V, 0, _O, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V, 1, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V, 0, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V, 0, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V, 1);
            if (std::is_same<InputType, float>::value) {
              __m<V> mmbcst = _mm<V>::set1_ps(md4(ainput4, _T, 0, _V, 0));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            } else {
              __m<V> mmbcst = _mm<V>::set1_ps(
                  half_2_float(md4(ainput4, _T, 0, _V, 0)));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            }
          }
        }
      }
    }
    // Ir
    {
#pragma nounroll
      for (int _V = 0; _V < xc.Ir; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V, 1, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md5(aweights5, xc.I2 - 1, _V, 0, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, xc.I2 - 1, _V, 0, _O, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V, 1, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, xc.I2 - 1, _V, 0, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, xc.I2 - 1, _V, 0, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            MD4(InputType, ainput4, &md2(ainput, xc.I2 - 1, 0), T, S, V, 1);
            if (std::is_same<InputType, float>::value) {
              __m<V> mmbcst = _mm<V>::set1_ps(md4(ainput4, _T, 0, _V, 0));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            } else {
              __m<V> mmbcst = _mm<V>::set1_ps(
                  half_2_float(md4(ainput4, _T, 0, _V, 0)));
              mmout[_O][_T]
                  = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
            }
          }
        }
      }

    }

    // store output
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
        if (get_attr(attr, relu_idx)) {
          __m<V> zero = _mm<V>::setzero_ps();
          mmout[_O][_T] = _mm<V>::max_ps(mmout[_O][_T], zero);
        }
        if (get_attr(attr, s_output_idx)) {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::stream_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::stream_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        } else {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::store_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::store_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        }
      }
    }
  }


  template <int JO, int P>
  static inline typename std::enable_if<
      !std::is_same<InputType, uint8_t>::value && P == 2, void>::type
  op_fma(elx_conv_params_t &xc,
      OutputType *output, InputType *input, WeightsType *weights, BiasType *bias,
      int attr, ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor,
      int _O1, int _O0)
  {
    static_assert(std::is_same<InputType, float>::value,
        "only fp32 input type");
    static_assert(std::is_same<WeightsType, float>::value
        || std::is_same<WeightsType, float16>::value,
        "only fp32/fp16 weights type");
    static_assert(std::is_same<OutputType, float>::value
        || std::is_same<OutputType, float16>::value,
        "only fp32/fp16 output type");
    static_assert(std::is_same<BiasType, float>::value
        || std::is_same<BiasType, float16>::value,
        "only fp32/fp16 bias type");

    __m<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V : xc.ih * xc.iw * V;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(OutputType, aoutput, output, JO, O_stride);
    MD2(InputType, ainput, input, xc.I2, I2_stride);
    MD2(BiasType, abias2, bias, JO, V);

    // preload weights
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
      if (F_traits<F>::is_compact_weights) {
        MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
        if (std::is_same<WeightsType, float>::value) {
          mmwei[_O][0] = _mm<V>::load_ps(&md5(aweights5, 0, 0, 0, _O, 0));
        } else {
          auto fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md5(aweights5, 0, 0, 0, _O, 0));
          mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
        }
      } else {
        MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
        if (std::is_same<WeightsType, float>::value) {
          mmwei[_O][0] = _mm<V>::load_ps(&md6(aweights6, _O, 0, 0, 0, 0, 0));
        } else {
          auto fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md6(aweights6, _O, 0, 0, 0, 0, 0));
          mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
        }
      }
    }

    if (get_attr(attr, r_output_idx)) {
      if (get_attr(attr, bias_idx)) {
        // load bias
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          __m<V> tmp;
          if (std::is_same<BiasType, float>::value) {
            tmp = _mm<V>::load_ps(&md2(abias2, _O, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(abias2, _O, 0));
            tmp = _mm<V>::cvtph_ps(fp16v);
          }
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
        }
      } else {
        // clear output
        __m<V> tmp = _mm<V>::setzero_ps();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
      }
      // load output
      if (get_attr(attr, ip_sum_idx)) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T) {
            MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
            if (std::is_same<OutputType, float>::value) {
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::load_ps(&md2(aoutput2, _T, 0)));
            } else {
              auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::cvtph_ps(fp16v));
            }
          }
        }
      }
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
          if (std::is_same<OutputType, float>::value) {
            mmout[_O][_T] = _mm<V>::load_ps(&md2(aoutput2, _T, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
            mmout[_O][_T] =  _mm<V>::cvtph_ps(fp16v);
          }
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
        // _P = 0
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][1] = _mm<V>::load_ps(&md5(aweights5, _I2, _V, 1, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V, 1, _O, 0));
              mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][1]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V, 1, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V, 1, 0));
              mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
        }
        // _P = 1
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md5(aweights5, _I2, _V + 1, 0, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V + 1, 0, _O, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 1));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][1], mmbcst, mmout[_O][_T]);
        }
      }
    }

    // store output
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
        if (get_attr(attr, relu_idx)) {
          __m<V> zero = _mm<V>::setzero_ps();
          mmout[_O][_T] = _mm<V>::max_ps(mmout[_O][_T], zero);
        }
        if (get_attr(attr, s_output_idx)) {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::stream_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::stream_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        } else {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::store_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::store_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        }
      }
    }
  }

  template <int JO, int P>
  static inline typename std::enable_if<
      !std::is_same<InputType, uint8_t>::value && P == 4, void>::type
  op_fma(elx_conv_params_t &xc,
      OutputType *output, InputType *input, WeightsType *weights, BiasType *bias,
      int attr, ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor,
      int _O1, int _O0)
  {
    static_assert(std::is_same<InputType, float>::value,
        "only fp32 input type");
    static_assert(std::is_same<WeightsType, float>::value
        || std::is_same<WeightsType, float16>::value,
        "only fp32/fp16 weights type");
    static_assert(std::is_same<OutputType, float>::value
        || std::is_same<OutputType, float16>::value,
        "only fp32/fp16 output type");
    static_assert(std::is_same<BiasType, float>::value
        || std::is_same<BiasType, float16>::value,
        "only fp32/fp16 bias type");

    __m<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V : xc.ih * xc.iw * V;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(OutputType, aoutput, output, JO, O_stride);
    MD2(InputType, ainput, input, xc.I2, I2_stride);
    MD2(BiasType, abias2, bias, JO, V);

    // preload weights
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
      if (F_traits<F>::is_compact_weights) {
        MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
        if (std::is_same<WeightsType, float>::value) {
          mmwei[_O][0] = _mm<V>::load_ps(&md5(aweights5, 0, 0, 0, _O, 0));
          mmwei[_O][1] = _mm<V>::load_ps(&md5(aweights5, 0, 0, 1, _O, 0));
        } else {
          auto fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md5(aweights5, 0, 0, 0, _O, 0));
          mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
          fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md5(aweights5, 0, 0, 1, _O, 0));
          mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
        }
      } else {
        MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
        if (std::is_same<WeightsType, float>::value) {
          mmwei[_O][0] = _mm<V>::load_ps(&md6(aweights6, _O, 0, 0, 0, 0, 0));
          mmwei[_O][1] = _mm<V>::load_ps(&md6(aweights6, _O, 0, 0, 0, 1, 0));
        } else {
          auto fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md6(aweights6, _O, 0, 0, 0, 0, 0));
          mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
          fp16v = _mm<V/2>::load_si256(
              (__m256i *)&md6(aweights6, _O, 0, 0, 0, 1, 0));
          mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
        }
      }
    }
    if (get_attr(attr, r_output_idx)) {
      if (get_attr(attr, bias_idx)) {
        // load bias
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          __m<V> tmp;
          if (std::is_same<BiasType, float>::value) {
            tmp = _mm<V>::load_ps(&md2(abias2, _O, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(abias2, _O, 0));
            tmp = _mm<V>::cvtph_ps(fp16v);
          }
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
        }
      } else {
        // clear output
        __m<V> tmp = _mm<V>::setzero_ps();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T)
            mmout[_O][_T] = tmp;
      }
      // load output
      if (get_attr(attr, ip_sum_idx)) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
          for (int _T = 0; _T < T; ++_T) {
            MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
            if (std::is_same<OutputType, float>::value) {
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::load_ps(&md2(aoutput2, _T, 0)));
            } else {
              auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
              mmout[_O][_T] = _mm<V>::add_ps(mmout[_O][_T],
                  _mm<V>::cvtph_ps(fp16v));
            }
          }
        }
      }
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
          if (std::is_same<OutputType, float>::value) {
            mmout[_O][_T] = _mm<V>::load_ps(&md2(aoutput2, _T, 0));
          } else {
            auto fp16v = _mm<V/2>::load_si256((__m256i *)&md2(aoutput2, _T, 0));
            mmout[_O][_T] =  _mm<V>::cvtph_ps(fp16v);
          }
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
        // _P = 0
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][2] = _mm<V>::load_ps(&md5(aweights5, _I2, _V, 2, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V, 2, _O, 0));
              mmwei[_O][2] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][2]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V, 2, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V, 2, 0));
              mmwei[_O][2] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][0], mmbcst, mmout[_O][_T]);
        }
#pragma unroll(JO)
        // _P = 1
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][3] = _mm<V>::load_ps(&md5(aweights5, _I2, _V, 3, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V, 3, _O, 0));
              mmwei[_O][3] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][3]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V, 3, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V, 3, 0));
              mmwei[_O][3] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 1));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][1], mmbcst, mmout[_O][_T]);
        }
        // _P = 2
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md5(aweights5, _I2, _V + 1, 0, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V + 1, 0, _O, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][0]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
              mmwei[_O][0] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 2));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][2], mmbcst, mmout[_O][_T]);
        }
        // _P = 3
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(WeightsType, aweights5, weights, xc.I2, V / P, P, O, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][1]
                  = _mm<V>::load_ps(&md5(aweights5, _I2, _V + 1, 1, _O, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md5(aweights5, _I2, _V + 1, 1, _O, 0));
              mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
            }
          } else {
            MD6(WeightsType, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            if (std::is_same<WeightsType, float>::value) {
              mmwei[_O][1]
                  = _mm<V>::load_ps(&md6(aweights6, _O, 0, _I2, _V + 1, 1, 0));
            } else {
              auto fp16v = _mm<V/2>::load_si256(
                  (__m256i *)&md6(aweights6, _O, 0, _I2, _V + 1, 1, 0));
              mmwei[_O][1] = _mm<V>::cvtph_ps(fp16v);
            }
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD4(InputType, ainput4, &md2(ainput, _I2, 0), T, S, V / P, P);
          __m<V> mmbcst
              = _mm<V>::broadcastss_ps(*(__m128 *)&md4(ainput4, _T, 0, _V, 3));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O)
            mmout[_O][_T]
                = _mm<V>::fmadd_ps(mmwei[_O][3], mmbcst, mmout[_O][_T]);
        }
      }
    }
    // store output
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(OutputType, aoutput2, &md2(aoutput, _O, 0), T, V);
        if (get_attr(attr, relu_idx)) {
          __m<V> zero = _mm<V>::setzero_ps();
          mmout[_O][_T] = _mm<V>::max_ps(mmout[_O][_T], zero);
        }
        if (get_attr(attr, s_output_idx)) {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::stream_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::stream_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        } else {
          if (std::is_same<OutputType, float>::value) {
            _mm<V>::store_ps(&md2(aoutput2, _T, 0), mmout[_O][_T]);
          } else {
            auto fp16v = _mm<V>::cvtps_ph(mmout[_O][_T],
                _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            _mm<V/2>::store_si256((__m256i *)&md2(aoutput2, _T, 0), fp16v);
          }
        }
      }
    }
  }


  // u8s8f32 fma
  template <int JO, int P>
  static inline typename std::enable_if<(P == 1 && has_Ir == false), void>::type
  op_fma(elx_conv_params_t &xc,
      float *output, uint8_t *input, int8_t *weights, float *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor, int _O1, int _O0)
  {
    __i<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V * Vx: xc.ih * xc.iw * V * Vx;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(int, aoutput, output, JO, O_stride);
    MD2(uint8_t, ainput, input, xc.I2, I2_stride);

    if (get_attr(attr, r_output_idx)) {
        // clear output
        __i<V> tmp = _mm<V>::setzero_epi32();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T)
          mmout[_O][_T] = tmp;
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
          mmout[_O][_T] = _mm<V>::load_epi32(&md2(aoutput2, _T, 0));
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V, 0, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V, 0, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 0, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][0]);
          }
        }
      }
    }

    // store output
    if (get_attr(attr, c_output_idx)) {
      MD3(float, aweights_scale3, weights_scale, xc.O1, O, V);
      MD2(float, aweights_scale, &md3(aweights_scale3, _O1, _O0, 0), JO, V);
      MD3(float, afactor3, factor, xc.O1, O, V);
      MD2(float, afactor, &md3(afactor3, _O1, _O0, 0), JO, V);

#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(float, aoutput2, &md2(aoutput, _O, 0), T, V);
        // 1. calculate coeffi. ## src_scale * weights_scale
        __m<V> coeffi = _mm<V>::broadcastss_ps(*(__m128 *)&src_scale[_T]);
        coeffi = _mm<V>::mul_ps(*(__m<V> *)&md2(aweights_scale, _O, 0), coeffi);
        // 2. convert mmout from int32 to float
        // 3. restore output ## (r - s) * coeffi
        __m<V> fout = _mm<V>::cvtepi32_ps(mmout[_O][_T]);
        fout = _mm<V>::sub_ps(fout, *(__m<V> *)&md2(afactor, _O, 0));
        fout = _mm<V>::mul_ps(fout, coeffi);
        // 1. add bias (direct conv 1x1)
        if (get_attr(attr, bias_idx)) {
          MD2(float, abias2, bias, JO, V);
          fout = _mm<V>::add_ps(fout, _mm<V>::load_ps(&md2(abias2, _O, 0)));
        }
        // 2. fuse relu (direct conv 1x1)
        if (get_attr(attr, relu_idx)) {
          fout = _mm<V>::max_ps(fout, _mm<V>::setzero_ps());
        }
        // 3. store output
        _mm<V>::store_ps(&md2(aoutput2, _T, 0), fout);
      }}
    } else {
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
        _mm<V>::store_epi32(&md2(aoutput2, _T, 0), mmout[_O][_T]);
      }}
    }
  }

  // TODO: handling V and Vx tail
  template <int JO, int P>
  static inline typename std::enable_if<(P == 1 && has_Ir == true), void>::type
  op_fma(elx_conv_params_t &xc,
      float *output, uint8_t *input, int8_t *weights, float *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor, int _O1, int _O0)
  {
    __i<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V * Vx: xc.ih * xc.iw * V * Vx;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(int, aoutput, output, JO, O_stride);
    MD2(uint8_t, ainput, input, xc.I2, I2_stride);

    if (get_attr(attr, r_output_idx)) {
        // clear output
        __i<V> tmp = _mm<V>::setzero_epi32();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T)
          mmout[_O][_T] = tmp;
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
          mmout[_O][_T] = _mm<V>::load_epi32(&md2(aoutput2, _T, 0));
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2 - 1; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V, 0, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34,
                xc.I2, V / P, P, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V, 0, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 0, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][0]);
          }
        }
      }
    }
    // Ir
    {
#pragma nounroll
      for (int _V = 0; _V < xc.Ir; ++_V) {
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(uint8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md5(aweights5, xc.I2 - 1, _V, 0, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, xc.I2 - 1, _V, 0, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, xc.I2 - 1, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 0, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][0]);
          }
        }
      }
    }

    // store output
    if (get_attr(attr, c_output_idx)) {
      MD3(float, aweights_scale3, weights_scale, xc.O1, O, V);
      MD2(float, aweights_scale, &md3(aweights_scale3, _O1, _O0, 0), JO, V);
      MD3(float, afactor3, factor, xc.O1, O, V);
      MD2(float, afactor, &md3(afactor3, _O1, _O0, 0), JO, V);

#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(float, aoutput2, &md2(aoutput, _O, 0), T, V);
        // 1. calculate coeffi. ## src_scale * weights_scale
        __m<V> coeffi = _mm<V>::broadcastss_ps(*(__m128 *)&src_scale[_T]);
        coeffi = _mm<V>::mul_ps(*(__m<V> *)&md2(aweights_scale, _O, 0), coeffi);
        // 2. convert mmout from int32 to float
        // 3. restore output ## (r - s) * coeffi
        __m<V> fout = _mm<V>::cvtepi32_ps(mmout[_O][_T]);
        fout = _mm<V>::sub_ps(fout, *(__m<V> *)&md2(afactor, _O, 0));
        fout = _mm<V>::mul_ps(fout, coeffi);
        // 1. add bias (direct conv 1x1)
        if (get_attr(attr, bias_idx)) {
          MD2(float, abias2, bias, JO, V);
          fout = _mm<V>::add_ps(fout, _mm<V>::load_ps(&md2(abias2, _O, 0)));
        }
        // 2. fuse relu (direct conv 1x1)
        if (get_attr(attr, relu_idx)) {
          fout = _mm<V>::max_ps(fout, _mm<V>::setzero_ps());
        }
        // 3. store output
        _mm<V>::store_ps(&md2(aoutput2, _T, 0), fout);
      }}
    } else {
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
        _mm<V>::store_epi32(&md2(aoutput2, _T, 0), mmout[_O][_T]);
      }}
    }
  }


  template <int JO, int P>
  static inline typename std::enable_if<P == 2, void>::type
  op_fma(elx_conv_params_t &xc,
      float *output, uint8_t *input, int8_t *weights, float *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor, int _O1, int _O0)
  {
    __i<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V * Vx: xc.ih * xc.iw * V * Vx;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(int, aoutput, output, JO, O_stride);
    MD2(uint8_t, ainput, input, xc.I2, I2_stride);

    // preload weights
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
      if (F_traits<F>::is_compact_weights) {
        MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
        mmwei[_O][0] = _mm<V>::load_epi32(&md5(aweights5, 0, 0, 0, _O, 0));
      } else {
        MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
        mmwei[_O][0] = _mm<V>::load_epi32(&md6(aweights6, _O, 0, 0, 0, 0, 0));
      }
    }

    if (get_attr(attr, r_output_idx)) {
      // clear output
      __i<V> tmp = _mm<V>::setzero_epi32();
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T)
        mmout[_O][_T] = tmp;
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
          mmout[_O][_T] = _mm<V>::load_epi32(&md2(aoutput2, _T, 0));
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
        // _P = 0
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][1]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V, 1, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V * Vx);
            mmwei[_O][1]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V, 1, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 0, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][0]);
          }
        }
        // _P = 1
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V + 1, 0, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 1, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][1]);
          }
        }
      }
    }

    // store output
    if (get_attr(attr, c_output_idx)) {
      MD3(float, aweights_scale3, weights_scale, xc.O1, O, V);
      MD2(float, aweights_scale, &md3(aweights_scale3, _O1, _O0, 0), JO, V);
      MD3(float, afactor3, factor, xc.O1, O, V);
      MD2(float, afactor, &md3(afactor3, _O1, _O0, 0), JO, V);

#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(float, aoutput2, &md2(aoutput, _O, 0), T, V);
        // 1. calculate coeffi. ## src_scale * weights_scale
        __m<V> coeffi = _mm<V>::broadcastss_ps(*(__m128 *)&src_scale[_T]);
        coeffi = _mm<V>::mul_ps(*(__m<V> *)&md2(aweights_scale, _O, 0), coeffi);
        // 2. convert mmout from int32 to float
        // 3. restore output ## (r - s) * coeffi
        __m<V> fout = _mm<V>::cvtepi32_ps(mmout[_O][_T]);
        fout = _mm<V>::sub_ps(fout, *(__m<V> *)&md2(afactor, _O, 0));
        fout = _mm<V>::mul_ps(fout, coeffi);
        // 1. add bias (direct conv 1x1)
        if (get_attr(attr, bias_idx)) {
          MD2(float, abias2, bias, JO, V);
          fout = _mm<V>::add_ps(fout, _mm<V>::load_ps(&md2(abias2, _O, 0)));
        }
        // 2. fuse relu (direct conv 1x1)
        if (get_attr(attr, relu_idx)) {
          fout = _mm<V>::max_ps(fout, _mm<V>::setzero_ps());
        }
        // 3. store output
        _mm<V>::store_ps(&md2(aoutput2, _T, 0), fout);
      }}
    } else {
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
        _mm<V>::store_epi32(&md2(aoutput2, _T, 0), mmout[_O][_T]);
      }}
    }
  }

  template <int JO, int P>
  static inline typename std::enable_if<P == 4, void>::type
  op_fma(elx_conv_params_t &xc,
      float *output, uint8_t *input, int8_t *weights, float *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor, int _O1, int _O0)
  {
    __i<V> mmout[JO][T], mmwei[JO][P];
    const int I2_stride
        = F_traits<F>::is_compact_input ? T * V * Vx: xc.ih * xc.iw * V * Vx;
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(int, aoutput, output, JO, O_stride);
    MD2(uint8_t, ainput, input, xc.I2, I2_stride);

    // preload weights
#pragma unroll(JO)
    for (int _O = 0; _O < JO; ++_O) {
      if (F_traits<F>::is_compact_weights) {
        MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
        mmwei[_O][0] = _mm<V>::load_epi32(&md5(aweights5, 0, 0, 0, _O, 0));
        mmwei[_O][1] = _mm<V>::load_epi32(&md5(aweights5, 0, 0, 1, _O, 0));
      } else {
        MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
        mmwei[_O][0] = _mm<V>::load_epi32(&md6(aweights6, _O, 0, 0, 0, 0, 0));
        mmwei[_O][1] = _mm<V>::load_epi32(&md6(aweights6, _O, 0, 0, 0, 1, 0));
      }
    }

    if (get_attr(attr, r_output_idx)) {
      // clear output
      __i<V> tmp = _mm<V>::setzero_epi32();
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O)
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T)
          mmout[_O][_T] = tmp;
    } else {
      // load output
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
          mmout[_O][_T] = _mm<V>::load_epi32(&md2(aoutput2, _T, 0));
        }
      }
    }

    for (int _I2 = 0; _I2 < xc.I2; ++_I2) {
#pragma nounroll
      for (int _V = 0; _V < V / P; ++_V) {
        // _P = 0
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][2]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V, 2, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            mmwei[_O][2]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V, 2, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 0, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][0]);
          }
        }
        // _P = 1
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][3]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V, 3, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            mmwei[_O][3]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V, 3, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 1, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][1]);
          }
        }
        // _P = 2
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V + 1, 0, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            mmwei[_O][0]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V + 1, 0, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 2, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][2]);
          }
        }
        // _P = 3
#pragma unroll(JO)
        for (int _O = 0; _O < JO; ++_O) {
          if (F_traits<F>::is_compact_weights) {
            MD5(int8_t, aweights5, weights, xc.I2, V / P, P, O, V * Vx);
            mmwei[_O][1]
                = _mm<V>::load_epi32(&md5(aweights5, _I2, _V + 1, 1, _O, 0));
          } else {
            MD6(int8_t, aweights6, weights, JO, xc.ic34, xc.I2, V / P, P, V);
            mmwei[_O][1]
                = _mm<V>::load_epi32(&md6(aweights6, _O, 0, _I2, _V + 1, 1, 0));
          }
        }
#pragma unroll(T)
        for (int _T = 0; _T < T; ++_T) {
          MD5(uint8_t, ainput5, &md2(ainput, _I2, 0), T, S, V / P, P, Vx);
          __i<V> bcast
              = _mm<V>::set1_epi32(*(int32_t *)&md5(ainput5, _T, 0, _V, 3, 0));
#pragma unroll(JO)
          for (int _O = 0; _O < JO; ++_O) {
            mmout[_O][_T] = __op_int8_fma(mmout[_O][_T], bcast, mmwei[_O][3]);
          }
        }
      }
    }

    // store output
    if (get_attr(attr, c_output_idx)) {
      MD3(float, aweights_scale3, weights_scale, xc.O1, O, V);
      MD2(float, aweights_scale, &md3(aweights_scale3, _O1, _O0, 0), JO, V);
      MD3(float, afactor3, factor, xc.O1, O, V);
      MD2(float, afactor, &md3(afactor3, _O1, _O0, 0), JO, V);

#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(float, aoutput2, &md2(aoutput, _O, 0), T, V);
        // 1. calculate coeffi. ## src_scale * weights_scale
        __m<V> coeffi = _mm<V>::broadcastss_ps(*(__m128 *)&src_scale[_T]);
        coeffi = _mm<V>::mul_ps(*(__m<V> *)&md2(aweights_scale, _O, 0), coeffi);
        // 2. convert mmout from int32 to float
        // 3. restore output ## (r - s) * coeffi
        __m<V> fout = _mm<V>::cvtepi32_ps(mmout[_O][_T]);
        fout = _mm<V>::sub_ps(fout, *(__m<V> *)&md2(afactor, _O, 0));
        fout = _mm<V>::mul_ps(fout, coeffi);
        // 1. add bias (direct conv 1x1)
        if (get_attr(attr, bias_idx)) {
          MD2(float, abias2, bias, JO, V);
          fout = _mm<V>::add_ps(fout, _mm<V>::load_ps(&md2(abias2, _O, 0)));
        }
        // 2. fuse relu (direct conv 1x1)
        if (get_attr(attr, relu_idx)) {
          fout = _mm<V>::max_ps(fout, _mm<V>::setzero_ps());
        }
        // 3. store output
        _mm<V>::store_ps(&md2(aoutput2, _T, 0), fout);
      }}
    } else {
#pragma unroll(JO)
      for (int _O = 0; _O < JO; ++_O) {
#pragma unroll(T)
      for (int _T = 0; _T < T; ++_T) {
        MD2(int, aoutput2, &md2(aoutput, _O, 0), T, V);
        _mm<V>::store_epi32(&md2(aoutput2, _T, 0), mmout[_O][_T]);
      }}
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 1)
      && (F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(WeightsType, aweights, weights, xc.O1, xc.I2 * V * O * V * Vx);
    MD2(OutputType, aoutput, output, xc.O1, O * O_stride);
    MD2(BiasType, abias, bias, xc.O1, O * V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md2(aoutput, _O1, 0), input, &md2(aweights, _O1, 0),
          &md2(abias, _O1, 0), attr, src_scale, weights_scale, factor, _O1, 0);
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 1)
      && !(F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD2(WeightsType, aweights, weights, xc.O1, O * xc.IC * V);
    MD2(OutputType, aoutput, output, xc.O1, O * O_stride);
    MD2(BiasType, abias, bias, xc.O1, O * V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md2(aoutput, _O1, 0), input, &md2(aweights, _O1, 0),
          &md2(abias, _O1, 0), attr, src_scale, weights_scale, factor, _O1, 0);
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 2)
      && (F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD4(WeightsType, aweights, weights, xc.O1, xc.I2 * V, O, V * Vx);
    MD3(OutputType, aoutput, output, xc.O1, O, O_stride);
    MD3(BiasType, abias, bias, xc.O1, O, V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md3(aoutput, _O1, 0, 0), input,
          &md4(aweights, _O1, 0, 0, 0), &md3(abias, _O1, 0, 0),
          attr, src_scale, weights_scale, factor, _O1, 0);
      op_fma<JO1, JP1>(xc, &md3(aoutput, _O1, JO0, 0), input,
          &md4(aweights, _O1, 0, JO0, 0), &md3(abias, _O1, JO0, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0);
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 2)
      && !(F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD3(WeightsType, aweights, weights, xc.O1, O, xc.IC * V);
    MD3(OutputType, aoutput, output, xc.O1, O, O_stride);
    MD3(BiasType, abias, bias, xc.O1, O, V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md3(aoutput, _O1, 0, 0), input,
          &md3(aweights, _O1, 0, 0), &md3(abias, _O1, 0, 0),
          attr, src_scale, weights_scale, factor, _O1, 0);
      op_fma<JO1, JP1>(xc, &md3(aoutput, _O1, JO0, 0), input,
          &md3(aweights, _O1, JO0, 0), &md3(abias, _O1, JO0, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0);
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 3)
      && (F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD4(WeightsType, aweights, weights, xc.O1, xc.I2 * V, O, V * Vx);
    MD3(OutputType, aoutput, output, xc.O1, O, O_stride);
    MD3(BiasType, abias, bias, xc.O1, O, V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md3(aoutput, _O1, 0, 0), input,
          &md4(aweights, _O1, 0, 0, 0), &md3(abias, _O1, 0, 0),
          attr, src_scale, weights_scale, factor, _O1, 0);
      op_fma<JO1, JP1>(xc, &md3(aoutput, _O1, JO0, 0), input,
          &md4(aweights, _O1, 0, JO0, 0), &md3(abias, _O1, JO0, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0);
      op_fma<JO2, JP2>(xc, &md3(aoutput, _O1, JO0 + JO1, 0), input,
          &md4(aweights, _O1, 0, JO0 + JO1, 0),
          &md3(abias, _O1, JO0 + JO1, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0 + JO1);
    }
  }

  template <int O = O, int T = T>
  static inline typename std::enable_if<(J_traits<O, T, has_Ir, WeightsType>::J == 3)
      && !(F_traits<F>::is_compact_weights)>::type
  execute(elx_conv_params_t &xc, OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias, int attr,
      ScaleType *src_scale, ScaleType *weights_scale, ScaleType *factor)
  {
    const int O_stride
        = F_traits<F>::is_compact_output ? T * V : xc.oh * xc.ow * V;

    MD3(WeightsType, aweights, weights, xc.O1, O, xc.IC * V);
    MD3(OutputType, aoutput, output, xc.O1, O, O_stride);
    MD3(BiasType, abias, bias, xc.O1, O, V);

    for (int _O1 = 0; _O1 < xc.O1; ++_O1) {
      op_fma<JO0, JP0>(xc, &md3(aoutput, _O1, 0, 0), input,
          &md3(aweights, _O1, 0, 0), &md3(abias, _O1, 0, 0),
          attr, src_scale, weights_scale, factor, _O1, 0);
      op_fma<JO1, JP1>(xc, &md3(aoutput, _O1, JO0, 0), input,
          &md3(aweights, _O1, JO0, 0), &md3(abias, _O1, JO0, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0);
      op_fma<JO2, JP2>(xc, &md3(aoutput, _O1, JO0 + JO1, 0), input,
          &md3(aweights, _O1, JO0 + JO1, 0), &md3(abias, _O1, JO0 + JO1, 0),
          attr, src_scale, weights_scale, factor, _O1, JO0 + JO1);
    }
  }
};

struct gemm_kernel_binder {
  template <typename GarrayTypes, int V, int Vx, int I, int... Kp>
  using gemm_ker_cls = typename euler::gemm_kernel_otj<GarrayTypes,
      V, Vx, I, estl::integer_sequence<Kp...>>;

  template <typename GarrayTypes>
  using ker = decltype(gemm_ker_cls<GarrayTypes, 1, 1, 1, 1, 1, 1, 1, false>::execute);

#if defined(WITH_GKTII) // gemm kernel template implicit instantiation
  template <typename GarrayTypes, int V, int Vx, int I, int S, int F, bool has_Ir>
  static inline void bind(int O, int T, ker<GarrayTypes> **func)
  {
    switch (O) {
    case 1:
      LOOP_FROM_TO(_T, 1, 32, {
        if (T == _T)
          (*func = gemm_ker_cls< GarrayTypes, V, Vx, I,
               S, F, 1, _T, has_Ir>::execute);
      });
      if (T >= 32)
        el_error("gemm_kernel: O = 1, T >= 32 not supported");
      break;
    case 2:
      LOOP_FROM_TO(_T, 1, 15, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 2, _T, has_Ir>::execute);
      });
      if (T >= 15)
        el_error("gemm_kernel: O = 2, T >= 15 not supported");
      break;
    case 3:
      LOOP_FROM_TO(_T, 1, 15, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 3, _T, has_Ir>::execute);
      });
      if (T >= 15)
        el_error("gemm_kernel: O = 3, T >= 15 not supported");
      break;
    case 4:
      LOOP_FROM_TO(_T, 1, 15, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 4, _T, has_Ir>::execute);
      });
      if (T >= 15)
        el_error("gemm_kernel: O = 4, T >= 15 not supported");
      break;
    case 5:
      LOOP_FROM_TO(_T, 1, 6, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 5, _T, has_Ir>::execute);
      });
      if (T >= 6)
        el_error("gemm_kernel: O = 5, T >= 6 not supported");
      break;
    case 6:
      LOOP_FROM_TO(_T, 1, 5, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 6, _T, has_Ir>::execute);
      });
      if (T >= 5)
        el_error("gemm_kernel: O = 6, T >= 5 not supported");
      break;
    case 7:
      LOOP_FROM_TO(_T, 1, 4, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 7, _T, has_Ir>::execute);
      });
      if (T >= 4)
        el_error("gemm_kernel: O = 7, T >= 4 not supported");
      break;
    case 8:
      LOOP_FROM_TO(_T, 1, 9, {
        if (T == _T)
          (*func = gemm_ker_cls<GarrayTypes, V, Vx, I,
               S, F, 8, _T, has_Ir>::execute);
      });
      if (T >= 9)
        el_error("gemm_kernel: O = 8, T >= 9 not supported");
      break;
    default:
      el_error("gemm_kenrel: O > 8 unsupported");
    }
  }
#else

  // Save compile time
  static ker<conv_impl::FP32> *ker_s1_ccc[8][32][2];
  static ker<conv_impl::FP32> *ker_s1_ccd[8][32][2];
  static ker<conv_impl::FP32> *ker_s1_dcd[8][32][2];
  static ker<conv_impl::FP32> *ker_s1_ddd[8][32][2];
  static ker<conv_impl::FP32> *ker_s2_ccc[8][32][2];
  static ker<conv_impl::FP32> *ker_s2_ccd[8][32][2];
  static ker<conv_impl::FP32> *ker_s2_dcd[8][32][2];
  static ker<conv_impl::FP32> *ker_s2_ddd[8][32][2];
  static ker<conv_impl::FP32_F16> *ker_f16_s1_ccc[8][32][2];
  static ker<conv_impl::INT8_F32> *ker_i8_s1_ccc[8][32][2];

  template <typename GarrayTypes, int V, int Vx, int I, int S, int F, bool has_Ir>
  static inline void bind(int O, int T, ker<conv_impl::FP32> **func)
  {
    switch (F) {
    case GKF_CCC:
      if (S == 1)
        *func = ker_s1_ccc[O - 1][T - 1][has_Ir];
      else if (S == 2)
        *func = ker_s2_ccc[O - 1][T - 1][has_Ir];
      break;
    case GKF_CCD:
      if (S == 1)
        *func = ker_s1_ccd[O - 1][T - 1][has_Ir];
      else if (S == 2)
        *func = ker_s2_ccd[O - 1][T - 1][has_Ir];
      break;
    case GKF_DCD:
      if (S == 1)
        *func = ker_s1_dcd[O - 1][T - 1][has_Ir];
      else if (S == 2)
        *func = ker_s2_dcd[O - 1][T - 1][has_Ir];
      break;
    case GKF_DDD:
      if (S == 1)
        *func = ker_s1_ddd[O - 1][T - 1][has_Ir];
      else if (S == 2)
        *func = ker_s2_ddd[O - 1][T - 1][has_Ir];
      break;
    default:
      break;
    }
  }

  template <typename GarrayTypes, int V, int Vx, int I, int S, int F, bool has_Ir>
  static inline void bind(int O, int T, ker<conv_impl::FP32_F16> **func)
  {
    switch (F) {
    case GKF_CCC:
      if (S == 1)
        *func = ker_f16_s1_ccc[O - 1][T - 1][has_Ir];
      break;
    default:
      break;
    }
  }

  template <typename GarrayTypes, int V, int Vx, int I, int S, int F, bool has_Ir>
  static inline void bind(int O, int T, ker<conv_impl::INT8_F32> **func)
  {
    switch (F) {
    case GKF_CCC:
      if (S == 1)
        *func = ker_i8_s1_ccc[O - 1][T - 1][has_Ir];
      break;
    default:
      break;
    }
  }

  template <typename GarrayTypes, int V, int Vx, int I, int S, int F, bool has_Ir>
  static inline void bind(int O, int T, ker<conv_impl::INT8_F16> **func)
  {}
#endif
};

} // namespace euler
