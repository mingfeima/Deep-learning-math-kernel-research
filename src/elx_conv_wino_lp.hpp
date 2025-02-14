#ifndef __ELX_CONV_WINO_LP_HPP__
#define __ELX_CONV_WINO_LP_HPP__

#include "euler.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "elx_conv_wino_u8s8_gemm.hpp"
#include "elx_conv_wino_trans_input.hpp"
#include "elx_conv_wino_trans_output.hpp"
#include "elx_conv_wino_trans_weights.hpp"

/*
  Winograd data types: (input,weights,output)
  +--------------+------+-----+-------------+--------------+--------------+-----------+
  |Name          |XOPT  |F16C |UserTypes    |TarrayTypes   |GarrayTypes   |GemmOpTypes|
  +--------------+------+-----+-------------+--------------+--------------+-----------+
  |int8          |TBD   |false|u8,fp32,u8/s8|fp32          |u8,s8,fp32    |u8,s8,int32|
  |fp16-int8     |A161… |true |fp16         |fp32          |u8,s8,fp32    |u8,s8,int32|
  |fp32-int8     |A161… |false|fp32         |fp32          |u8,s8,fp32    |u8,s8,int32|
  +--------------+------+-----+-------------+--------------+--------------+-----------+

  * INT8 mode, input/weights type of TarrayTypes equals to TrOpType, output type
    of TarrayTypes equals to output of GarrayTypes.
*/

namespace euler {

#define Template_elx_conv_wino_lp_t                                            \
  template <typename UserTypes, typename TarrayTypes,                          \
      const int A, const int K, const int V, const int I>

#define Instance_elx_conv_wino_lp_t                                            \
  elx_conv_wino_lp_t<UserTypes, TarrayTypes, A, K, V, I>

Template_elx_conv_wino_lp_t
class elx_conv_wino_lp_t : public elx_conv_t {
public:
  // Configurable parameters
  using elx_conv_t::IC;
  using elx_conv_t::OC;
  using elx_conv_t::T;
  using elx_conv_t::I2;
  using elx_conv_t::O2;
  using elx_conv_t::oc4;
  using elx_conv_t::ic3;
  using elx_conv_t::oc3;
  using elx_conv_t::V1;
  using elx_conv_t::Vx;
  using InputType = typename UserTypes::InputType;
  using WeightsType = typename UserTypes::WeightsType;
  using OutputType = typename UserTypes::OutputType;
  using BiasType = typename UserTypes::BiasType;

  using TrOpType = float;

  // t-buffer type
  using TinputType = float;
  using TweightsType = float;
  using ToutputType = typename TarrayTypes::OutputType;
  using TscaleType = typename TarrayTypes::ScaleType;

  constexpr static bool is_border = true;
  constexpr static bool has_bias = true;
  constexpr static bool has_relu = true;
  constexpr static bool has_sum = true;
  constexpr static bool no = false;

public:
  elx_conv_wino_lp_t(eld_conv_t &dc);
  virtual ~elx_conv_wino_lp_t();

  virtual void execute(void *output, void *input, void *weights, void *bias);

private:
  void __execute_a133(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);
  void __execute_a161(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);
  void __execute_a173(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);

  int prepare_execute_opt();
  void set_workspace_buffers();
  void set_scratchpad_buffers();
  void bind_execute_functions();
  void prepare_quant_calibration(eld_conv_t &dc);

  void (elx_conv_wino_lp_t::*execute_opt_)(
      OutputType *, InputType *, WeightsType *, BiasType *);

  void trans_weights(WeightsType *weights);

  // ??? XXX: Deduction error here
  elx_conv_wino_trans_input_t<uint8_t, InputType, I, A, K, V>
    trans_input_u8;

  elx_conv_wino_trans_weights_t<int8_t, WeightsType, I, A, K, V>
    trans_weights_s8;

  elx_conv_wino_u8s8_gemm_t<TarrayTypes, A, V, I> u8s8_gemm;

  elx_conv_wino_trans_output_t<OutputType, BiasType, ToutputType, I, A, K, V>
      trans_output;

  unsigned int xopt_;
  bool is_first_run_;
  bool inference_acc_;
  bool is_bfmt_;
  bool input_is_bfmt_;
  bool weights_is_bfmt_;
  bool output_is_bfmt_;
  bool input_as_bfmt_;
  bool weights_as_bfmt_;
  bool output_as_bfmt_;
  int mthr_;
  size_t workspace_size_;
  size_t tweights_size_;
  size_t tinput_size_;
  size_t toutput_size_;
  size_t binput_size_;
  size_t bweights_size_;
  size_t boutput_size_;
  size_t tinput_u8_size_;
  size_t tinput_quant_scale_size_;
  size_t tweights_s8_size_;
  size_t tweights_quant_scale_size_;
  size_t tweights_quant_factor_size_;
  void *workspace_;
  void *scratch_;

  TweightsType *tweights_;
  TinputType *tinput_;
  ToutputType *toutput_;
  InputType *binput_; // blocked input
  WeightsType *bweights_;
  OutputType *boutput_;
  uint8_t *tinput_u8_;
  TscaleType *tinput_quant_scale_;
  int8_t *tweights_s8_;
  TscaleType *tweights_quant_scale_;
  TscaleType *tweights_quant_factor_;
};

// fp32-u8s8f32
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F32, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F32, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F32, 6, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F32, 7, 3, 16, ISA_SKX_AVX512>;

// u8f32u8f32-u8s8f32
template class elx_conv_wino_lp_t<conv::U8F32U8F32, conv_impl::INT8_F32, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32U8F32, conv_impl::INT8_F32, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32U8F32, conv_impl::INT8_F32, 6, 3, 16, ISA_SKX_AVX512>;

// u8f32s8f32-u8s8f32
template class elx_conv_wino_lp_t<conv::U8F32S8F32, conv_impl::INT8_F32, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32S8F32, conv_impl::INT8_F32, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32S8F32, conv_impl::INT8_F32, 6, 3, 16, ISA_SKX_AVX512>;

// u8f32f32f32-u8s8f32
template class elx_conv_wino_lp_t<conv::U8F32F32F32, conv_impl::INT8_F32, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32F32F32, conv_impl::INT8_F32, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::U8F32F32F32, conv_impl::INT8_F32, 6, 3, 16, ISA_SKX_AVX512>;

// fp32-u8s8f16
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F16o, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F16o, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP32, conv_impl::INT8_F16o, 6, 3, 16, ISA_SKX_AVX512>;

#ifdef ENABLE_USER_FP16
// fp16-u8s8f32
template class elx_conv_wino_lp_t<conv::FP16, conv_impl::INT8_F16b, 4, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP16, conv_impl::INT8_F16b, 5, 3, 16, ISA_SKX_AVX512>;
template class elx_conv_wino_lp_t<conv::FP16, conv_impl::INT8_F16b, 6, 3, 16, ISA_SKX_AVX512>;
#endif

}  // namespace euler
#endif  // __ELX_CONV_WINO_LP_HPP__
