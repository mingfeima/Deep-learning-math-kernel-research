#include <stdlib.h>
#include <assert.h>
#include <float.h>
#include "euler.hpp"
#include "el_def.hpp"
#include "el_isa.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "elx_conv_wino.hpp"
#include "elx_conv_wino_lp.hpp"
#include "elx_conv_direct_1x1.hpp"
#include "elx_conv_direct_1x1_lp.hpp"
#include "elx_conv_direct.hpp"
#include "elx_conv_direct_lp.hpp"

namespace euler {

eld_conv_t::eld_conv_t()
{
  pads = { 1, 1, 1, 1 };
  strides = { 1, 1 };
  dilations = { 1, 1 };
  sizes = { 0, 0, 0, 0 };
  algorithm = CONV_DIRECT;
  tile_size = 0;
  with_relu = false;
  with_bias = false;
  with_ip_sum = false;
  with_op_sum = false;
  f16c_opt = false;
  xc = nullptr;
  nthreads = 0;
  execution_mode = 0;
  blocking = { 0, 0 };
  flatting = { 0, 0 };
  partition = { 1, 1 };
  streaming_hint = { 0, 0 };
  format_as_blocked = { false, false, false };
  input_quant = {EL_NO_CALI, EL_NO_CALI};
  output_quant = {EL_NO_CALI, EL_NO_CALI};
  sampling_kind = FINE;
}

eld_conv_t::~eld_conv_t()
{
  if (xc != nullptr) {
    delete xc;
  }
}

int eld_conv_t::setup()
{
  // Dimensions
  if (dims.input.c != dims.weights.i || dims.input.n != dims.output.n
      || dims.output.c != dims.weights.o) {
    el_error("Dimension error");
    return ELD_GENERAL_ERROR;
  }
  if (with_bias && dims.bias.c != dims.output.c) {
    el_error("Dimension error");
    return ELD_GENERAL_ERROR;
  }

  const int V = cpu_vector_length() / 4;
  const int ic = dims.input.c, IC = ALIGNUP(ic, V);
  const int oc = dims.output.c, OC = ALIGNUP(oc, V);
  if (V != 16) {
    // TODO: V == 8
    el_error("CPU vector not support");
  }

  // Select format
  if (formats.input == format_undef) {
    formats.input = ic < V ? nchw : V == 16 ? nChw16c : nChw8c;
  }
  if (formats.weights == format_undef) {
    formats.weights = hwio;
  }
  if (formats.output == format_undef) {
    formats.output = V == 16 ? nChw16c : nChw8c;
  }

  sizes.input = dims.input.n * dims.input.h * dims.input.w;
  sizes.weights = dims.weights.h * dims.weights.w;
  sizes.output = dims.output.n * dims.output.h * dims.output.w;

  using dt = decltype(data_type);
  uint32_t user_type = data_type.flat;
  uint32_t user_type_f32 = dt{ { { f32, f32, f32, f32 } } }.flat;
  uint32_t user_type_f16o = dt{ { { f32, f32, f16, f32 } } }.flat;
  uint32_t user_type_u8f32f32f32 = dt{ { { u8, f32, f32, f32 } } }.flat;
  uint32_t user_type_u8f32u8f32 = dt{ { { u8, f32, u8, f32 } } }.flat;
  uint32_t user_type_u8f32s8f32 = dt{ { { u8, f32, s8, f32 } } }.flat;
#ifdef ENABLE_USER_FP16
  uint32_t user_type_f16 = dt{ { { f16, f16, f16, f16 } } }.flat;
#endif

  sizes.input *= estl::any_of(formats.input, nChw16c, nChw8c) ? IC : ic;
  sizes.weights *= estl::any_of(formats.weights, OIhw16i16o, OIhw8i8o)
      ? OC * IC : oc * ic;
  sizes.weights += 4 * V; // for weights pipeline
  sizes.output *= estl::any_of(formats.output, nChw16c, nChw8c) ? OC : oc;
  sizes.bias = estl::any_of(formats.output, nChw16c, nChw8c) ? OC : oc;

  auto get_elem_size = [](uint8_t dtype) -> size_t {
    switch (dtype) {
    case f32: return sizeof(float);
    case f16: return sizeof(short);
    case u8:
    case s8: return sizeof(uint8_t);
    default: return 0;
    }
  };

  byte_sizes.input = get_elem_size(data_type.input) * sizes.input;
  byte_sizes.weights = get_elem_size(data_type.weights) * sizes.weights;
  byte_sizes.output = get_elem_size(data_type.output) * sizes.output;
  byte_sizes.bias = get_elem_size(data_type.bias) * sizes.bias;

  // Validate padding
  int oh = (dims.input.h + pads.t + pads.b - dims.weights.h) / strides.h + 1;
  int ow = (dims.input.w + pads.l + pads.r - dims.weights.w) / strides.w + 1;
  if (oh != dims.output.h || ow != dims.output.w) {
    el_error("Padding parameter error");
    return ELX_GENERAL_ERROR;
  }

  xc = nullptr;

  if (prop_kind != forward_inference) {
    el_error("Propagation kind error");
    return ELD_GENERAL_ERROR;
  }

  if (algorithm == CONV_AUTO) {
    if (dims.weights.h == 1 && dims.weights.w == 1) {
      algorithm = CONV_DIRECT_1X1;
    } else if (ic >= V && dims.weights.h == 3 && dims.weights.w == 3
        && dilations.h == 1 && dilations.w == 1 && strides.h == 1
        && strides.w == 1 && pads.l == 1 && pads.r == 1 && pads.t == 1
        && pads.b == 1) {
      algorithm = CONV_WINOGRAD;
    } else {
      algorithm = CONV_DIRECT;
    }
  }

  // Direct
  if (algorithm == CONV_DIRECT) {
    if (user_type == user_type_f32) {
      if (f16c_opt)
        xc = new elx_conv_direct_t<conv::FP32, conv_impl::FP32_F16w, 16, ISA_SKX_AVX512>(*this);
      else
        xc = new elx_conv_direct_t<conv::FP32, conv_impl::FP32, 16, ISA_SKX_AVX512>(*this);
    } else if (user_type == user_type_u8f32u8f32) {
      xc = new elx_conv_direct_lp_t<conv::U8F32U8F32, conv_impl::INT8_F32, 16, ISA_SKX_AVX512>(*this);
    } else if (user_type == user_type_u8f32s8f32) {
      xc = new elx_conv_direct_lp_t<conv::U8F32S8F32, conv_impl::INT8_F32, 16, ISA_SKX_AVX512>(*this);
#ifdef ENABLE_USER_FP16
    } else if (user_type == user_type_f16o)
      xc = new elx_conv_direct_t<conv::FP16O, conv_impl::FP32_F16o, 16, ISA_SKX_AVX512>(*this);
#endif
    } else
      el_error("TODO: FP16 UserTypes for DIRECT.");
  } else if (algorithm == CONV_DIRECT_1X1) {
    if (dims.weights.h != 1 || dims.weights.w != 1) {
      el_error("Algorithm CONV_DIRECT_1X1 not supported for this shape.");
      return ELD_GENERAL_ERROR;
    }
    if (user_type == user_type_f32) {
      if (f16c_opt)
        xc = new elx_conv_direct_1x1_t<conv::FP32, conv_impl::FP32_F16w, 16, ISA_SKX_AVX512>(*this);
      else
        xc = new elx_conv_direct_1x1_t<conv::FP32, conv_impl::FP32, 16, ISA_SKX_AVX512>(*this);
    } else if (user_type == user_type_u8f32u8f32) {
        xc = new elx_conv_direct_1x1_lp_t<conv::U8F32U8F32, conv_impl::INT8_F32, 16, ISA_SKX_AVX512>(*this);
    } else if (user_type == user_type_u8f32s8f32) {
        xc = new elx_conv_direct_1x1_lp_t<conv::U8F32S8F32, conv_impl::INT8_F32, 16, ISA_SKX_AVX512>(*this);
    } else
      el_error("TODO: FP16 UserTypes for DIRECT 1x1.");
  } else if (algorithm == CONV_WINOGRAD) {
    // Winograd
    if (dilations.h > 1 || dilations.w > 1 ||
        strides.h != 1 || strides.w != 1 ||
        dims.weights.h != 3 || dims.weights.w != 3) {
      el_error("Algorithm CONV_WINOGRAD: data shape not supported");
      return ELD_UNIMPLEMENTED;
    }

    if (tile_size == 0) {
      int t = dims.output.n * ((dims.output.h + 3) / 4)
                            * ((dims.output.w + 3) / 4);
      float mac_per_read = (t * OC) * 1.0f / (t + OC);
#define MAC_PER_READ_A6_LIMIT (25.0f)
      tile_size = mac_per_read > MAC_PER_READ_A6_LIMIT  ? 6 : 4;
    }

    #define F_5_3_OFF_CASE(UT, TT, type) \
      case 7: break

    #define F_5_3_ON_CASE(UT, TT, type) \
      case 7: \
        xc = new elx_conv_##type##_t<UT, TT, 7, 3, 16, \
            ISA_SKX_AVX512>(*this); \
        break

    #define create_conv_wino(UT, TT, prefix, type) \
      switch (tile_size) { \
      case 4: \
        xc = new elx_conv_##type##_t<UT, TT, 4, 3, 16, \
            ISA_SKX_AVX512>(*this); \
        break; \
      case 5: \
        xc = new elx_conv_##type##_t<UT, TT, 5, 3, 16, \
            ISA_SKX_AVX512>(*this); \
        break; \
      case 6: \
        xc = new elx_conv_##type##_t<UT, TT, 6, 3, 16, \
            ISA_SKX_AVX512>(*this); \
        break; \
      prefix##_CASE(UT, TT, type); \
      default: \
        el_error("Unimplemented tile size"); \
        break; \
      }

    // TODO: forward, backward_data, backward_weights
    if ((execution_mode & 0xF00) != 0x100) {
      if (f16c_opt && user_type == user_type_f32) {
        create_conv_wino(conv::FP32, conv_impl::FP32_F16iwo, F_5_3_ON, wino);
#ifdef ENABLE_USER_FP16
      } else if (user_type == user_type_f16) {
        create_conv_wino(conv::FP16, conv_impl::FP32_F16wob, F_5_3_ON, wino);
#endif
      } else if (user_type != user_type_f16o) {
        create_conv_wino(conv::FP32, conv_impl::FP32, F_5_3_ON, wino);
      }
    } else {
      if (f16c_opt && user_type == user_type_f32) {
        create_conv_wino(conv::FP32, conv_impl::INT8_F16o, F_5_3_OFF, wino_lp);
#ifdef ENABLE_USER_FP16
      } else if (user_type == user_type_f16) {
        create_conv_wino(conv::FP16, conv_impl::INT8_F16b, F_5_3_OFF, wino_lp);
#endif
      } else if (user_type == user_type_u8f32u8f32) {
        create_conv_wino(conv::U8F32U8F32, conv_impl::INT8_F32, F_5_3_OFF, wino_lp);
      } else if (user_type == user_type_u8f32s8f32) {
        create_conv_wino(conv::U8F32S8F32, conv_impl::INT8_F32, F_5_3_OFF, wino_lp);
      } else if (user_type == user_type_u8f32f32f32) {
        create_conv_wino(conv::U8F32F32F32, conv_impl::INT8_F32, F_5_3_OFF, wino_lp);
      } else if (user_type != user_type_f16o) {
        create_conv_wino(conv::FP32, conv_impl::INT8_F32, F_5_3_ON, wino_lp);
      }
    }
  }

  return ELD_OK;
}

}  // namespace euler
