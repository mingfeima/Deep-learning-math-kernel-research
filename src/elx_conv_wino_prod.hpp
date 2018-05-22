#include "euler.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"

#ifndef __ELX_CONV_WINO_PROD_HPP__
#define __ELX_CONV_WINO_PROD_HPP__

namespace euler {

template <typename T, const int A, const int K, const int V, const int I>
class elx_conv_wino_prod_t : public elx_conv_t<T> {
 public:
  elx_conv_wino_prod_t(eld_conv_t<T> &dc);
  virtual ~elx_conv_wino_prod_t();

  virtual void execute(T *output, T *input, T *weights, T *bias);

 private:
  void trans_weights(T *tweights, T *weights);
  void trans_input(T *tinput, T *input);
  void product_trans_output(T *output, T *tinput, T *tweights);

  T *tinput_;
  T *toutput_;
  T *tweights_;
};

template class elx_conv_wino_prod_t<float, 5, 3, 16, ISA_GENERIC>;
template class elx_conv_wino_prod_t<float, 5, 3, 16, ISA_SKX_AVX512>;

}  // namespace euler
#endif  // __ELX_CONV_WINO_PROD_HPP__
