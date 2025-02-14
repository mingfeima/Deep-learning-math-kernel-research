#include "elx_conv_direct_depthwise_lp.hpp"

namespace euler {

Template_elx_conv_direct_depthwise_lp_t void
Instance_elx_conv_direct_depthwise_lp_t::bind_execute_functions()
{
#define BIND_CONV_KERNEL(S, F, K)                                              \
  if (K == 3) {                                                                \
    u8s8_depthwise_conv_kernel_binder::bind<S, F, 3>(O, T, func);              \
  }

  auto bind_conv_kernel = [&](int O, int T,
      u8s8_depthwise_conv_kernel_binder::kconv<TarrayTypes, OutputType> **func,
      int K) {
    switch (xopt_) {
    case (0xa160):
      if (this->input_fmt == nChw16c && this->output_fmt == nChw16c) {
        if (this->ws == 1) {
          BIND_CONV_KERNEL(1, GKF_DCD, K);
        } else if (this->ws == 2) {
          BIND_CONV_KERNEL(2, GKF_DCD, K);
        } else {
          el_error("Stride > 2 not yet bounded");
        }
      } else {
        el_error("direct_depthwise_lp:int8: kernel fmt not supported");
      }
      break;
    default:
      el_error("Unknown xopt");
      break;
    }
  };

  if (xopt_ == 0xa160) {
    bind_conv_kernel(this->O, this->T, &ker_conv_, this->kw);
    bind_conv_kernel(this->O, this->Tr, &ker_conv_Tr_, this->kw);
  }

#define EXECUTE_CASE(n)                                                        \
  case 0x##n:                                                                  \
    printf("execute_opt=" #n "\n");                                            \
    execute_opt_ = &Instance_elx_conv_direct_depthwise_lp_t::__execute_##n;    \
    break

  switch (xopt_) {
    EXECUTE_CASE(a160);
  default:
    el_error("direct_depthwise_lp: Unimplemented xopt");
    break;
  }
}

} // namespace euler
