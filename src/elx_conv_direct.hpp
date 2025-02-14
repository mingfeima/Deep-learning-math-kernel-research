#ifndef __ELX_CONV_DIRECT_HPP__
#define __ELX_CONV_DIRECT_HPP__

#include "euler.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "kernel/elk_gemm_otj_binder.hxx"
#include "kernel/elk_conv_otj_binder.hxx"

namespace euler {

#define Template_elx_conv_direct_t                                             \
  template <typename UserTypes, typename TarrayTypes, const int V, const int I>

#define Instance_elx_conv_direct_t                                             \
  elx_conv_direct_t<UserTypes, TarrayTypes, V, I>

Template_elx_conv_direct_t class elx_conv_direct_t : public elx_conv_t {
  using InputType = typename UserTypes::InputType;
  using WeightsType = typename UserTypes::WeightsType;
  using OutputType = typename UserTypes::OutputType;
  using BiasType = typename UserTypes::BiasType;

  // t-buffer type
  using TinputType = typename TarrayTypes::InputType;
  using TweightsType = typename TarrayTypes::WeightsType;
  using ToutputType = typename TarrayTypes::OutputType;

  public:
  elx_conv_direct_t(eld_conv_t &dc);
  virtual ~elx_conv_direct_t();

  virtual void execute(void *output, void *input, void *weights, void *bias);

  private:
  void __execute_a060(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);
  void __execute_b060(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);
  void __execute_d060(OutputType *output, InputType *input,
      WeightsType *weights, BiasType *bias);

  void trans_weights_to_compact(TweightsType *tweights, WeightsType *weights);
  inline void __trans_weights_post(WeightsType *aweights, TweightsType *tweights,
      int _g, int _oc4, int _ic4, int _oc3, int _ic3, int _kh, int _kw, int _O1,
      int _I2, int _iV, int _O);
  inline void __trans_weights_Or_post(WeightsType *aweights, TweightsType *tweights,
      int _g, int _oc4, int _ic4, int _oc3, int _ic3, int _kh, int _kw, int _O1,
      int _I2, int _iV, int _O);

  void conv_a060(OutputType *output, InputType *input, TweightsType *weights,
      BiasType *bias, int _ic4, int _oc4, int _ht, int _wt);
  void conv_b060(OutputType *output, InputType *input, TweightsType *weights,
      BiasType *bias, int _ic4, int _ic3, int _oc4, int _ht, int _wt);
  void gemm_d060(OutputType *toutput, InputType *tinput, TweightsType *tweights,
      BiasType *bias, int _ic4, int _oc4, int _ht, int _wt);

  void set_trans_buffers();
  int prepare_execute_opt();
  void bind_execute_functions();

  // TODO: optimize it
  gemm_kernel_binder::kgemm<TarrayTypes> *ker_gemm_[128][8];
  conv_kernel_binder::kconv<TarrayTypes> *ker_conv_;
  conv_kernel_binder::kconv<TarrayTypes> *ker_conv_Tr_;

  void (elx_conv_direct_t::*execute_opt_)(
      OutputType *, InputType *, WeightsType *, BiasType *);

  bool is_first_run_;
  bool inference_acc_;

  size_t tweights_size_;
  TweightsType *tweights_;
  size_t toutput_size_;
  ToutputType *toutput_;
  unsigned int xopt_;
  int attr_;
  int mthr_;
  void *scratch_;
  void *workspace_;
};

// fp32-f32f32f32
template class elx_conv_direct_t<conv::FP32, conv_impl::FP32, 16, ISA_SKX_AVX512>;
// fp32-f32f16f32
template class elx_conv_direct_t<conv::FP32, conv_impl::FP32_F16w, 16, ISA_SKX_AVX512>;

#ifdef ENABLE_USER_FP16
// fp16o-f32f32f16
template class elx_conv_direct_t<conv::FP16O, conv_impl::FP32_F16o, 16, ISA_SKX_AVX512>;
#endif

} // namespace euler
#endif // __ELX_CONV_DIRECT_HPP__
