#include <string.h>
#include "xintrin.hpp"
#include "el_utils.hpp"
#include "elx_conv_wino.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elk_conv_wino.hpp"
#include "elx_conv.hpp"
#include "euler.hpp"

namespace euler {

//
// -------------+-------------------+--------------+---------------
//  execute-opt | thread-teaming-by | fusion-along | duplication
// -------------+-------------------+--------------+---------------
//     A040     |        _          |      t       |    _
// -------------+-------------------+--------------+---------------
//     A048*    |        _          |      t       |    W
// -------------+-------------------+--------------+---------------
//     A060     |        _          |    t + o     |    _
// -------------+-------------------+--------------+---------------
//     A061     |        _          |    t + o     |    I
// -------------+-------------------+--------------+---------------
//     A069*    |        _          |    t + o     |  I + W
// -------------+-------------------+--------------+---------------
//     A0e1     |        _          |  t + o + wA  |    I
// -------------+-------------------+--------------+---------------
//     A0e0     |        _          |  t + o + wA  |    _
// -------------+-------------------+--------------+---------------
//     A072     |        _          |  i + t + o   |    O
// -------------+-------------------+--------------+---------------
//     A448     |        t          |      t       |    W
// -------------+-------------------+--------------+---------------
//     A241     |        o          |      t       |    I
// -------------+-------------------+--------------+---------------
//     A000     |        _          |      _       |    _
// -------------+-------------------+--------------+---------------
//     A010     |        _          |      i       |    _
// -------------+-------------------+--------------+---------------
//     A201     |        o          |      _       |    I
// -------------+-------------------+--------------+---------------
//     A020*    |        _          |      o       |    _
// -------------+-------------------+--------------+---------------
//     A021*    |        _          |      o       |    I
// -------------+-------------------+--------------+---------------
//  *: TODO
//

const unsigned XOPT_MSK = 0xA000;

const unsigned TTM_MSK = 0xF00;
const unsigned TTM_I   = 0x100;
const unsigned TTM_O   = 0x200;
const unsigned TTM_T   = 0x400;

const unsigned FUS_MSK = 0xF0;
const unsigned FUS_I   = 0x10;
const unsigned FUS_O   = 0x20;
const unsigned FUS_T   = 0x40;
const unsigned FUS_A   = 0x80;

const unsigned DUP_MSK = 0xF;
const unsigned DUP_I   = 0x1;
const unsigned DUP_O   = 0x2;
const unsigned DUP_W   = 0x8;

template <typename Type, const int A, const int K, const int V, const int I>
elx_conv_wino_t<Type, A, K, V, I>::elx_conv_wino_t(
    eld_conv_t<Type>& dc)
    : elx_conv_t<Type>(dc)
{
  // TODO: error when V!=16 && fmt=OIhw16i16o

  this->IC = ALIGNUP(this->ic, V);
  this->OC = ALIGNUP(this->oc, V);

  this->V = V;
  this->ic2 = this->IC / V;
  this->oc2 = this->OC / V;

  this->A = A;
  this->ht = (this->oh + A - K) / (A - K + 1);
  this->wt = (this->ow + A - K) / (A - K + 1);
  this->nt = this->ht * this->wt;
  this->t = this->nt * this->n;

  hOA_end_ = this->oh % (A - K + 1) - 1;
  if (hOA_end_ == -1) hOA_end_ = A - K;
  wOA_end_ = this->ow % (A - K + 1) - 1;
  if (wOA_end_ == -1) wOA_end_ = A - K;
  hA_end_ = (this->ih + this->tp) - (this->ht - 1) * (A - K + 1) - 1;
  wA_end_ = (this->iw + this->lp) - (this->wt - 1) * (A - K + 1) - 1;

  // TODO: santize user settings
  if (this->O == 0) this->O = 1; // TODO: O selection
  if (this->O1 == 0) this->O1 = 1; // TODO: O1 selection
  if (this->I2 == 0) this->I2 = 1; // TODO: I2 selection
  if (this->T == 0)  this->T = 1; // TODO: T selection
  this->O2 = this->O * this->O1;

  // Tailing
  this->Tr = this->t % this->T ? this->t % this->T : this->T;
  this->Ir = this->ic % V ? this->ic % V : V;
  this->Or = this->oc % V ? this->oc % V : V;

  is_first_run_ = true;
  inference_acc_ = false;
  mthr_ = omp_get_max_threads();
  if (this->nteams == 0 || this->nthreads == 0
      || this->nteams * this->nthreads > mthr_
      || this->nteams > MAX_THREAD_TEAMS) {
    this->nteams = 1;
    this->nthreads = mthr_;
  } else {
    mthr_ = this->nteams * this->nthreads;
  }
  inference_acc_ = this->prop_kind == forward_inference;

  this->oc4 = this->oc4 == 0 ? 1 : this->oc4;
  this->ic4 = this->ic4 == 0 ? 1 : this->ic4;

  // further divide packed oc/ic
  this->oc3 = this->oc2 / this->O2;
  this->ic3 = this->ic2 / this->I2;

  this->t2 = (this->t + this->T - 1) / this->T;

  // In case of Ir != V && blocked-format, assume bias also
  // padded to Vx.

  xopt_ = this->execution_mode;
  if (!(xopt_ & XOPT_MSK)) {
    // TODO: deduce xopt
    xopt_ = TTM_O | FUS_T | DUP_I;
  }

  prepare_execute_opt();

  bind_execute_functions();

  // dbg
  printf("############################################################\n");
  printf("T=%d, Tr=%d, t2=%d, t=%d\n", this->T, this->Tr, this->t2, this->t);
  printf("V=%d, Ir=%d, I2=%d, ic3=%d, ic4=%d, IC=%d\n", this->V, this->Ir, this->I2, this->ic3, this->ic4, this->IC);
  printf("V=%d, Or=%d, O2=%d (O=%d, O1=%d), oc3=%d, oc4=%d, OC=%d\n", this->V, this->Or, this->O2, this->O, this->O1, this->oc3, this->oc4, this->OC);

#ifdef DEBUG
  if (this->V * this->I2 * this->ic3 * this->ic4 != this->IC) {
      el_warn("V * I2 * ic3 * ic4 != this->IC\n Force ic4 = IC / (V * I2 * ic3)");
      this->ic4 = this->IC / (this->V * this->I2 * this->ic3);
  } 

  if (this->V * this->O2 * this->oc3 * this->oc4 != this->OC) {
      el_warn("V * O2 * oc3 * oc4 != this->OC\n Force oc4 = OC / (V * O2 * oc3)");
      this->oc4 = this->OC / (this->V * this->O2 * this->oc3);
  }
#else
  if (this->V * this->I2 * this->ic3 * this->ic4 != this->IC) {
      el_error("V * I2 * ic3 * ic4 != this->IC\n)");
  }

  if (this->V * this->O2 * this->oc3 * this->oc4 != this->OC) {
      el_error("V * O2 * oc3 * oc4 != this->OC\n)");
  }
#endif
}
template <typename Type, const int A, const int K, const int V, const int I>
int  elx_conv_wino_t<Type, A, K, V, I>::prepare_execute_opt()
{
  size_t tweights_size = 0, tinput_size = 0, toutput_size = 0;
  size_t toutputa_size = 0;
  size_t binput_size = 0, bweights_size = 0, boutput_size = 0;
  size_t l1_usage = 0, l2_usage = 0;

  auto divide_tasks_ttm = [this](size_t tasks) {
    size_t ntasks_base = tasks / this->nteams;
    size_t rem = tasks - this->nteams * ntasks_base;
    for (size_t s = 0; s < this->nteams; s++) {
      if (s < rem) {
        ttm_[s].start = (ntasks_base + 1) * s;
        ttm_[s].end = ttm_[s].start + ntasks_base;
      } else {
        ttm_[s].start = rem * (ntasks_base + 1) + (s - rem) * ntasks_base;
        ttm_[s].end = ttm_[s].start + ntasks_base - 1;
      }
      // dbg
      printf("ttm_[%ld]=[%d,%d]\n", s, ttm_[s].start, ttm_[s].end);
    }
  };

  stream_in_ = this->streaming_input
      ? (this->streaming_input == STORE_STREAMING)
      : !(xopt_ & FUS_MSK) ? true : false;
  stream_wei_ = this->streaming_weights
      ? (this->streaming_weights == STORE_STREAMING)
      : !(xopt_ & FUS_MSK) ? true : false;
  stream_out_ = this->streaming_output
      ? (this->streaming_output == STORE_STREAMING)
      : false;

  if (!(xopt_ & TTM_MSK)) {
    this->nthreads = mthr_;
    this->nteams = 1;
  }
  if (xopt_ & TTM_T) {
    divide_tasks_ttm(this->t2);
  }
  if (xopt_ & TTM_O) {
    if (this->oc3 % this->nteams != 0) {
      // Force single nteams
      this->nthreads = mthr_;
      this->nteams = 1;
    } else {
      // ignore user --pat-o=oc4
      this->oc3 /= this->nteams;
      this->oc4 = this->nteams;
    }
  }
  if (xopt_ & FUS_O) {
    this->oc3 /= this->oc4;
    if (V * this->O2 * this->oc3 * this->oc4 != this->OC) {
      el_error("Config error!");
      return -1;
    }
  }
  if (xopt_ & FUS_I) {
    this->ic3 /= this->ic4;
    if (V * this->I2 * this->ic3 * this->ic4 != this->IC) {
      el_error("Config error!");
      return -1;
    }
  }

  input_is_bfmt_ = this->input_fmt == nchw ? false : true;
  weights_is_bfmt_ = this->weights_fmt == oihw ? false : true;
  output_is_bfmt_ = this->output_fmt == nchw ? false : true;
  input_as_bfmt_ = !input_is_bfmt_ && this->input_as_blocked;
  weights_as_bfmt_ = !weights_is_bfmt_ && this->weights_as_blocked;
  output_as_bfmt_ = !output_is_bfmt_ && this->output_as_blocked;
  is_bfmt_ = input_is_bfmt_ && weights_is_bfmt_ && output_is_bfmt_;

  if (this->ic4 > 1 && this->Ir != V) {
    el_error("Unimplemented: ic4 > 1 for IC % V != 0");
  }
  if (this->oc4 > 1 && this->Or != V
      && (!output_as_bfmt_ || !weights_as_bfmt_)) {
    el_error("Unimplemented: oc4 > 1 for OC % V != 0");
  }

  if (input_as_bfmt_)
    binput_size = this->n * this->IC * this->ih * this->iw;
  if (weights_as_bfmt_)
    bweights_size = this->OC * this->IC * this->kh * this->kw;
  if (output_as_bfmt_)
    boutput_size = this->n * this->OC * this->oh * this->ow;

  tweights_ = nullptr;
  tinput_ = nullptr;
  toutput_ = nullptr;
  toutputa_ = nullptr;
  binput_ = nullptr;
  bweights_ = nullptr;
  boutput_ = nullptr;

  l1_usage = sizeof(Type)
      * (this->O2 * this->I2 * V * V + this->T * V * (this->I2 + this->O2));

  switch (xopt_) {
  case 0xa000:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->t;
    toutput_size = A * A * this->OC * this->t;
    l2_usage = this->IC * this->OC / this->oc3
        + this->T * (this->IC + this->OC / this->oc3);
    break;
  case 0xa010:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->ic3 * this->I2 * V * this->t;
    toutput_size = A * A * this->OC * this->t;
    l2_usage = this->IC * this->OC / this->oc3
        + this->T * (this->IC + this->OC / this->oc3);
    break;
  case 0xa040:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->T * mthr_;
    toutput_size = A * A * this->OC * this->T * mthr_;
    l2_usage = tweights_size + A * A * this->T * (this->IC + this->OC);
    break;
  case 0xa060:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->t;
    toutput_size = A * A * (this->OC / this->oc4) * this->T * mthr_;
    l2_usage = tweights_size / this->oc4
        + A * A * this->T * (this->IC + this->OC / this->oc4);
    break;
  case 0xa061:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->T * mthr_;
    toutput_size = A * A * (this->OC / this->oc4) * this->T * mthr_;
    l2_usage = tweights_size / this->oc4
        + A * A * this->T * (this->IC + this->OC / this->oc4);
    break;
  case 0xa072:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * (this->IC / this->ic4) * this->T * mthr_;
    toutput_size = A * A * this->OC * this->t;
    l2_usage = tweights_size / this->ic4 / this->oc4
        + A * A * this->T * (this->IC / this->ic4 + this->OC / this->oc4);
    break;
  case 0xa0e0:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->t;
    toutput_size = A * (this->OC / this->oc4) * this->T * mthr_;
    toutputa_size = A * (A - K + 1) * this->OC * this->t;
    l2_usage = tweights_size / this->oc4 / A
        + A * this->T * (this->IC + this->OC / this->oc4);
    break;
  case 0xa0e1:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * this->IC * this->T * mthr_;
    toutput_size = A * (this->OC / this->oc4) * this->T * mthr_;
    toutputa_size = A * (A - K + 1) * this->OC * this->t;
    l2_usage = tweights_size / this->oc4 / A
        + A * this->T * (this->IC + this->OC / this->oc4);
    break;
  case 0xa201:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->T * this->t2 * this->nteams;
    toutput_size = A * A * this->OC * this->T * this->t2;
    l2_usage = this->IC * this->OC / this->oc3 / this->oc4
        + this->T * (this->IC + this->OC / this->oc3 / this->oc4);
    break;
  case 0xa241:
    tweights_size = A * A * this->IC * this->OC;
    tinput_size = A * A * this->IC * this->T * mthr_;
    toutput_size = A * A * this->OC * this->T * mthr_;
    l2_usage = tweights_size / this->oc4
        + A * A * this->T * (this->IC + this->OC / this->oc4);
    break;
  case 0xa448:
    tweights_size = A * A * this->IC * this->OC * this->nteams;
    tinput_size = A * A * this->IC * this->T * mthr_;
    toutput_size = A * A * this->OC * this->T * mthr_;
    l2_usage = tweights_size / this->nteams
        + A * A * this->T * (this->IC + this->OC);
    break;
  default:
      el_error("Config error!");
      return -1;
    break;
  }

  l2_usage *= sizeof(Type);

  if (tweights_size > 0)
    MEMALIGN64(&tweights_, (tweights_size + WEIGHTS_MAX_PRELOAD * V) * sizeof(Type));
  if (tinput_size > 0)
    MEMALIGN64(&tinput_, tinput_size * sizeof(Type));
  if (toutput_size > 0)
    MEMALIGN64(&toutput_, toutput_size * sizeof(Type));
  if (toutputa_size > 0)
    MEMALIGN64(&toutputa_, toutputa_size * sizeof(Type));
  if (binput_size > 0)
    MEMALIGN64(&binput_, binput_size * sizeof(Type));
  if (bweights_size > 0)
    MEMALIGN64(&bweights_, bweights_size * sizeof(Type));
  if (boutput_size > 0)
    MEMALIGN64(&boutput_, boutput_size * sizeof(Type));

  // dbg


  printf("nteams=%d, nthreads=%d, mthr_=%d\n",
      this->nteams, this->nthreads, mthr_);
  printf("gemmker_input_footprint = %ld\n", gemmker_input_footprint());
  printf("gemmker_weights_footprint = %ld\n", gemmker_weights_footprint());
  printf("gemmker_output_footprint = %ld\n", gemmker_output_footprint());
  printf("gemm_input_reuse_set = %ld\n", gemm_input_reuse_set());
  printf("gemm_output_reuse_set = %ld\n", gemm_output_reuse_set());

  auto plan = execute_plan(this->nthreads, 1, 1024 * 1024, 32 * 1024);
  plan.dump();

  return 0;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::bind_execute_functions()
{
  ker_trans_input_
    = convolution_winograd_kernel<
        Type, I, V, A, K>::template trans_input<no>;
  ker_trans_input0_
    = convolution_winograd_kernel<
        Type, I, V, A, K>::template trans_input<is_border>;
  ker_trans_inputa_
    = convolution_winograd_kernel<
        Type, I, V, A, K>::template trans_inputa<no>;
  ker_trans_inputa0_
    = convolution_winograd_kernel<
        Type, I, V, A, K>::template trans_inputa<is_border>;
  ker_trans_weights_
    = convolution_winograd_kernel<Type, I, V, A, K>::trans_weights;

  // TODO: ker_trans_output_nobias_norelu_nosum (no fusion)
  // Fusion operation is done in related ker_trans_output_
  ker_trans_output_nobias_ = convolution_winograd_kernel<Type, I, V, A, K>::
      template trans_output<no, no, no, no>;
  ker_trans_output0_nobias_ = convolution_winograd_kernel<Type, I, V, A, K>::
      template trans_output<is_border, no, no, no>;

  using kernel_set = convolution_winograd_kernel<Type, I, V, A, K>;
  static const struct {
    decltype (ker_trans_output_) f1_;
    decltype (ker_trans_output0_) f2_;
    decltype (ker_trans_outputa_bh_) f3_;
    decltype (ker_trans_outputa0_bh_) f4_;
  } pointer_table[2][2][2] = {
    {{{kernel_set::template trans_output<0, 0, 0, 0>
    , kernel_set::template trans_output<1, 0, 0, 0>
    , kernel_set::template trans_outputa_bh<0, 0, 0, 0>
    , kernel_set::template trans_outputa_bh<1, 0, 0, 0>}

    ,{kernel_set::template trans_output<0, 0, 0, 1>
    , kernel_set::template trans_output<1, 0, 0, 1>
    , kernel_set::template trans_outputa_bh<0, 0, 0, 1>
    , kernel_set::template trans_outputa_bh<1, 0, 0, 1>}}

    ,{{kernel_set::template trans_output<0, 0, 1, 0>
    , kernel_set::template trans_output<1, 0, 1, 0>
    , kernel_set::template trans_outputa_bh<0, 0, 1, 0>
    , kernel_set::template trans_outputa_bh<1, 0, 1, 0>}

    ,{kernel_set::template trans_output<0, 0, 1, 1>
    , kernel_set::template trans_output<1, 0, 1, 1>
    , kernel_set::template trans_outputa_bh<0, 0, 1, 1>
    , kernel_set::template trans_outputa_bh<1, 0, 1, 1>}}}

    ,{{{kernel_set::template trans_output<0, 1, 0, 0>
    , kernel_set::template trans_output<1, 1, 0, 0>
    , kernel_set::template trans_outputa_bh<0, 1, 0, 0>
    , kernel_set::template trans_outputa_bh<1, 1, 0, 0>}

    ,{kernel_set::template trans_output<0, 1, 0, 1>
    , kernel_set::template trans_output<1, 1, 0, 1>
    , kernel_set::template trans_outputa_bh<0, 1, 0, 1>
    , kernel_set::template trans_outputa_bh<1, 1, 0, 1>}}

    ,{{kernel_set::template trans_output<0, 1, 1, 0>
    , kernel_set::template trans_output<1, 1, 1, 0>
    , kernel_set::template trans_outputa_bh<0, 1, 1, 0>
    , kernel_set::template trans_outputa_bh<1, 1, 1, 0>}

    ,{kernel_set::template trans_output<0, 1, 1, 1>
    , kernel_set::template trans_output<1, 1, 1, 1>
    , kernel_set::template trans_outputa_bh<0, 1, 1, 1>
    , kernel_set::template trans_outputa_bh<1, 1, 1, 1>}}}
  };

  auto slot = pointer_table[this->with_bias][this->with_relu][this->with_sum];
  ker_trans_output_ = slot.f1_;
  ker_trans_output0_ = slot.f2_;
  ker_trans_outputa_bh_ = slot.f3_;
  ker_trans_outputa0_bh_ = slot.f4_;

  ker_trans_outputa_th_ = convolution_winograd_kernel<Type, I, V, A, K>::
      template trans_outputa_th<no, no, no, no>;

  auto bind_gemm_kernel =
    [&](int O, int T, bool has_Ir, gemm_kernel_binder::ker **func) {
    if (this->Ir != V && has_Ir) {
      gemm_kernel_binder::bind<Type, V, I, 1, GKF_CCC, true, false, false,
          false, false>(O, T, func);
    } else {
      gemm_kernel_binder::bind<Type, V, I, 1, GKF_CCC, false, false, false,
          false, false>(O, T, func);
    }
  };

  bind_gemm_kernel(this->O, this->T, false, &ker_gemm_);
  bind_gemm_kernel(this->O, this->Tr, false, &ker_gemm0_);
  bind_gemm_kernel(this->O, this->T, true, &ker_gemm_tail_);
  bind_gemm_kernel(this->O, this->Tr, true, &ker_gemm0_tail_);

#define EXECUTE_CASE(n)                                                      \
  case 0x##n:                                                                \
    printf("execute_opt=" #n "\n");                                          \
    execute_opt_ = &elx_conv_wino_t<Type, A, K, V, I>::__execute_##n;        \
    break

  switch (xopt_) {
  EXECUTE_CASE(a241);
  EXECUTE_CASE(a201);
  EXECUTE_CASE(a448);
  EXECUTE_CASE(a040);
  EXECUTE_CASE(a060);
  EXECUTE_CASE(a061);
  EXECUTE_CASE(a0e1);
  EXECUTE_CASE(a0e0);
  EXECUTE_CASE(a072);
  EXECUTE_CASE(a000);
  EXECUTE_CASE(a010);
  default:
    el_error("Unimplemented");
    break;
  }
}

template <typename Type, const int A, const int K, const int V, const int I>
elx_conv_wino_t<Type, A, K, V, I>::~elx_conv_wino_t() {
  if (tweights_ != nullptr) {
    free(tweights_);
    tweights_ = nullptr;
  }
  if (tinput_ != nullptr) {
    free(tinput_);
    tinput_ = nullptr;
  }
  if (toutput_ != nullptr) {
    free(toutput_);
    toutput_ = nullptr;
  }
  if (toutputa_ != nullptr) {
    free(toutputa_);
    toutputa_ = nullptr;
  }
  if (binput_ != nullptr) {
    free(binput_);
    binput_ = nullptr;
  }
  if (bweights_ != nullptr) {
    free(bweights_);
    bweights_ = nullptr;
  }
  if (boutput_ != nullptr) {
    free(boutput_);
    boutput_ = nullptr;
  }
}

#define t2spato(__t2, __T, __n, __oh, __ow, __hOA_end, __wOA_end)            \
  do {                                                                       \
    int _t = __t2 * this->T + __T;                                           \
    int _nt = _t % this->nt;                                                 \
    int _ht = _nt / this->wt;                                                \
    int _wt = _nt % this->wt;                                                \
    __n = _t / this->nt;                                                     \
    __oh = _ht * (A - K + 1);                                                \
    __ow = _wt * (A - K + 1);                                                \
    __hOA_end = (_ht < this->ht - 1) ? A - K : hOA_end_;                     \
    __wOA_end = (_wt < this->wt - 1) ? A - K : wOA_end_;                     \
  } while (0)

#define t2spati(                                                             \
    __t2, __T, __n, __ih, __iw, __hA_start, __hA_end, __wA_start, __wA_end)  \
  do {                                                                       \
    int _t = __t2 * this->T + __T;                                           \
    int _nt = _t % this->nt;                                                 \
    int _ht = _nt / this->wt;                                                \
    int _wt = _nt % this->wt;                                                \
    __n = _t / this->nt;                                                     \
    __ih = _ht * (A - K + 1) - this->tp;                                     \
    __iw = _wt * (A - K + 1) - this->lp;                                     \
    __hA_start = (_ht > 0) ? 0 : this->tp;                                   \
    __wA_start = (_wt > 0) ? 0 : this->lp;                                   \
    __hA_end = (_ht < this->ht - 1) ? A - 1 : hA_end_;                       \
    __wA_end = (_wt < this->wt - 1) ? A - 1 : wA_end_;                       \
  } while (0)

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weights_plain(
    Type * __restrict tweights, Type * __restrict weights, int oc4)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, oc3, ic3, wA, hA, O2, I2, V, V
  MD11(Type, aweights_v, weights, oc4, this->oc3, this->O1, this->O, V, this->ic4, this->ic3, this->I2, V, K, K);
  MD11(Type, atweights, tweights, oc4, this->ic4, this->oc3, this->ic3, A, A, this->O1, this->I2, V, this->O, V);

  SET_EPI32(this->ic * this->kh * this->kw)

  auto readin_v = [&](Type ain[K][K][V][V], Type *wei) {
    MD5(Type, awei, wei, V, this->ic2, V, K, K);

    iter_each (_hK, K) {
    iter_each (_wK, K) {
    iter_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        constexpr auto scale = sizeof(Type);
        auto t = _mm<V>::i32gather_ps(vindex,
            &md5(awei, 0, 0, _iV, _hK, _wK), scale);
        _mm<V>::store_ps(ain[_hK][_wK][_iV], t);
      } else {
        iter_each (_oV, V)
          ain[_hK][_wK][_iV][_oV] = md5(awei, _oV, 0, _iV, _hK, _wK);
      }
    }}}
  };

  auto readin_r = [&](Type ain[K][K][V][V], int _oc4, int _oc3, int _O2,
                      int _ic4, int _ic3, int _I2, bool is_Ir, bool is_Or) {
    MD4(Type, awei, weights, this->oc, this->ic, K, K);

    assert(this->ic4 == 1 && this->oc4 == 1);
    int _oc2 = _oc4 * this->oc3 * this->O2 + _oc3 * this->O2 + _O2;
    int _ic2 = _ic4 * this->ic3 * this->I2 + _ic3 * this->I2 + _I2;
    int iV = is_Ir ? this->Ir : V;

    if (is_Or) {
      iter_each (_hK, K) {
      iter_each (_wK, K) {
      iter_each (_iV, iV) {
#pragma omp simd
      iter_each (_oV, this->Or) {
        ain[_hK][_wK][_iV][_oV]
            = md4(awei, _oc2 * V + _oV, _ic2 * V + _iV, _hK, _wK);
      }}}}
    } else {
      iter_each (_hK, K) {
      iter_each (_wK, K) {
      iter_each (_iV, iV) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr auto scale = sizeof(Type);
          auto t = _mm<V>::i32gather_ps(vindex,
              &md4(awei, _oc2 * V, _ic2 * V + _iV, _hK, _wK), scale);
          _mm<V>::store_ps(ain[_hK][_wK][_iV], t);
        } else {
#pragma omp simd
          iter_each (_oV, V) {
            ain[_hK][_wK][_iV][_oV]
                = md4(awei, _oc2 * V + _oV, _ic2 * V + _iV, _hK, _wK);
          }
        }
      }}}
    }
  };

#pragma omp for nowait collapse(6) schedule(static)
  iter_each (_oc4, oc4) {
  iter_each (_ic4, this->ic4) {
  iter_each (_oc3, this->oc3) {
  iter_each (_ic3, this->ic3) {
  iter_each (_O1, this->O1) {
  iter_each (_I2, this->I2) {
  iter_each (_O, this->O) {
    bool is_Ir = this->Ir != V && _ic4 == this->ic4 - 1
        && _ic3 == this->ic3 - 1 && _I2 == this->I2 - 1;
    bool is_Or = this->Or != V && _oc4 == this->oc4 - 1
        && _oc3 == this->oc3 - 1 && _O1 == this->O1 - 1 && _O == this->O - 1;

    alignas(64) Type ain[K][K][V][V];
    alignas(64) Type aout[A][A][V][V];

    if (this->Ir != V || is_Ir || is_Or)
      readin_r(ain, _oc4, _oc3, _O1 + this->O + _O, _ic4, _ic3, _I2, is_Ir, is_Or);
    else
      readin_v(
          ain, &md11(aweights_v, _oc4, _oc3, _O1, _O, 0, _ic4, _ic3, _I2, 0, 0, 0));

    ker_trans_weights_(aout, ain);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_wei_) {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_stream_ps(&md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA,
                               _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_store_ps(&md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA,
                              _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      }
    } else {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
      iter_each (_iV, V) {
#pragma omp simd
      iter_each (_oV, V) {
        md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA, _O1, _I2, _iV, _O, _oV)
            = aout[_wA][_hA][_iV][_oV];
      }}}}
    }
  }}}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weights_blocked(
    Type *tweights, Type *weights, int oc4)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, oc3, ic3, wA, hA, O2, I2, V, V
  MD11(Type, aweights, weights, oc4, this->oc3, this->O1, this->O, this->ic4, this->ic3, this->I2, K, K, V, V);
  MD11(Type, atweights, tweights, oc4, this->ic4, this->oc3, this->ic3, A, A, this->O1, this->I2, V, this->O, V);

#pragma omp for nowait collapse(6) schedule(static)
  iter_each (_oc4, oc4) {
  iter_each (_ic4, this->ic4) {
  iter_each (_oc3, this->oc3) {
  iter_each (_ic3, this->ic3) {
  iter_each (_O1, this->O1) {
  iter_each (_I2, this->I2) {
  iter_each (_O, this->O) {
    alignas(64) Type aout[A][A][V][V];
    Type *in = &md11(aweights, _oc4, _oc3, _O1, _O, _ic4, _ic3, _I2, 0, 0, 0, 0);
    using Array = Type[K][K][V][V];
    ker_trans_weights_(aout, *(Array *)in);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_wei_) {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_stream_ps(&md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA,
                               _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_store_ps(&md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA,
                              _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      }
    } else {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
      iter_each (_iV, V) {
#pragma omp simd
        iter_each (_oV, V)
          md11(atweights, _oc4, _ic4, _oc3, _ic3, _wA, _hA, _O1, _I2, _iV, _O, _oV)
              = aout[_wA][_hA][_iV][_oV];
      }}}
    }
  }}}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_weights(
    Type *tweights, Type *weights, int oc4)
{
  if (weights_is_bfmt_ || weights_as_bfmt_)
    __trans_weights_blocked(tweights, weights, oc4);
  else
    __trans_weights_plain(tweights, weights, oc4);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weightsa_blocked(
    Type *tweights, Type *weights)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, wA, hA, oc3, ic3, O2, I2, V, V
  MD11(Type, aweights, weights, this->oc4, this->oc3, this->O1, this->O, this->ic4, this->ic3, this->I2, K, K, V, V);
  MD11(Type, atweights, tweights, this->oc4, this->ic4, A, A, this->oc3, this->ic3, this->O1, this->I2, V, this->O, V);

#pragma omp for nowait collapse(6) schedule(static)
  iter_each (_oc4, this->oc4) {
  iter_each (_ic4, this->ic4) {
  iter_each (_oc3, this->oc3) {
  iter_each (_ic3, this->ic3) {
  iter_each (_O1, this->O1) {
  iter_each (_I2, this->I2) {
  iter_each (_O, this->O) {
    alignas(64) Type aout[A][A][V][V];
    Type *in = &md11(aweights, _oc4, _oc3, _O1, _O, _ic4, _ic3, _I2, 0, 0, 0, 0);
    using Array = Type[K][K][V][V];
    ker_trans_weights_(aout, *(Array *)in);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_wei_) {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_stream_ps(&md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3,
                               _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_store_ps(&md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3,
                              _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      }
    } else {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
      iter_each (_iV, V) {
#pragma omp simd
      iter_each (_oV, V) {
        md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3, _O1, _I2, _iV, _O, _oV)
            = aout[_wA][_hA][_iV][_oV];
      }}}}
    }
  }}}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weightsa_plain(
    Type * __restrict tweights, Type * __restrict weights)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, wA, hA, oc3, ic3, O2, I2, V, V
  MD11(Type, aweights, weights, this->oc4, this->oc3, this->O1, this->O, V, this->ic4, this->ic3, this->I2, V, K, K);
  MD11(Type, atweights, tweights, this->oc4, this->ic4, A, A, this->oc3, this->ic3, this->O1, this->I2, V, this->O, V);

  SET_EPI32(this->ic * this->kh * this->kw)

  auto readin_v = [&](Type ain[K][K][V][V], Type *wei) {
    MD5(Type, awei, wei, V, this->ic2, V, K, K);

    iter_each (_hK, K) {
    iter_each (_wK, K) {
    iter_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        constexpr auto scale = sizeof(Type);
        auto t = _mm<V>::i32gather_ps(vindex,
            &md5(awei, 0, 0, _iV, _hK, _wK), scale);
        _mm<V>::store_ps(ain[_hK][_wK][_iV], t);
      } else {
        iter_each (_oV, V)
          ain[_hK][_wK][_iV][_oV] = md5(awei, _oV, 0, _iV, _hK, _wK);
      }
    }}}
  };

  auto readin_r = [&](Type ain[K][K][V][V], int _oc4, int _oc3, int _O2,
                      int _ic4, int _ic3, int _I2, bool is_Ir, bool is_Or) {
    MD4(Type, awei, weights, this->oc, this->ic, K, K);

    int _oc2 = _oc4 * this->oc3 * this->O2 + _oc3 * this->O2 + _O2;
    int _ic2 = _ic4 * this->ic3 * this->I2 + _ic3 * this->I2 + _I2;
    int iV = is_Ir ? this->Ir : V;

    if (is_Or) {
      iter_each (_hK, K) {
      iter_each (_wK, K) {
      iter_each (_iV, iV) {
#pragma omp simd
      iter_each (_oV, this->Or) {
        ain[_hK][_wK][_iV][_oV]
            = md4(awei, _oc2 * V + _oV, _ic2 * V + _iV, _hK, _wK);
      }}}}
    } else {
      iter_each (_hK, K) {
      iter_each (_wK, K) {
      iter_each (_iV, iV) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr auto scale = sizeof(Type);
          auto t = _mm<V>::i32gather_ps(vindex,
              &md4(awei, _oc2 * V, _ic2 * V + _iV, _hK, _wK), scale);
          _mm<V>::store_ps(ain[_hK][_wK][_iV], t);
        } else {
#pragma omp simd
          iter_each (_oV, V) {
            ain[_hK][_wK][_iV][_oV]
                = md4(awei, _oc2 * V + _oV, _ic2 * V + _iV, _hK, _wK);
          }
        }
      }}}
    }
  };

#pragma omp for nowait collapse(6) schedule(static)
  iter_each (_oc4, this->oc4) {
  iter_each (_ic4, this->ic4) {
  iter_each (_oc3, this->oc3) {
  iter_each (_ic3, this->ic3) {
  iter_each (_O1, this->O1) {
  iter_each (_I2, this->I2) {
  iter_each (_O, this->O) {

    bool is_Ir = this->Ir != V && _ic4 == this->ic4 - 1
        && _ic3 == this->ic3 - 1 && _I2 == this->I2 - 1;
    bool is_Or = this->Or != V && _oc4 == this->oc4 - 1
        && _oc3 == this->oc3 - 1 && _O1 == this->O1 - 1 && _O == this->O - 1;

    alignas(64) Type ain[K][K][V][V];
    alignas(64) Type aout[A][A][V][V];

    if (this->Ir != V || is_Ir || is_Or)
      readin_r(ain, _oc4, _oc3, _O1 * this->O + _O, _ic4, _ic3, _I2, is_Ir, is_Or);
    else
      readin_v(
          ain, &md11(aweights, _oc4, _oc3, _O1, _O, 0, _ic4, _ic3, _I2, 0, 0, 0));

    ker_trans_weights_(aout, ain);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_wei_) {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_stream_ps(&md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3,
                               _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
        iter_each (_iV, V) {
          _mm512_store_ps(&md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3,
                              _O1, _I2, _iV, _O, 0),
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        }}}
      }
    } else {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
      iter_each (_iV, V) {
#pragma omp simd
      iter_each (_oV, V) {
        md11(atweights, _oc4, _ic4, _wA, _hA, _oc3, _ic3, _O1, _I2, _iV, _O, _oV)
            = aout[_wA][_hA][_iV][_oV];
      }}}}
    }
  }}}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_weightsa(
    Type *tweights, Type *weights)
{
  if (weights_is_bfmt_ || weights_as_bfmt_)
    __trans_weightsa_blocked(tweights, weights);
  else
    __trans_weightsa_plain(tweights, weights);
}


template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_plain(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  // n, IC, ih, iw => t2 | wA, hA, ic3, I2, T, V
  MD6(Type, atinput, tinput, A, A, this->ic3, this->I2, Tz, V);

  alignas(64) Type aout[A][A][V];
  alignas(64) Type ain[A][A][V];
  SET_EPI32(this->ih * this->iw)

  auto readin_v = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, V, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    iter_each (_hA, A) {
    iter_each (_wA, A) {
      if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
          || _wA > _wA_end) {
#pragma omp simd
        iter_each (_V, V)
          ain[_hA][_wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m<V> t = _mm<V>::i32gather_ps(vindex,
              &md7(ainput, _n, 0, _ic3, _I2, 0, _ih + _hA, _iw + _wA),
              scale);
          _mm<V>::store_ps(ain[_hA][_wA], t);
        } else {
#pragma omp simd
          iter_each (_V, V)
            ain[_hA][_wA][_V]
                = md7(ainput, _n, 0, _ic3, _I2, _V, _ih + _hA, _iw + _wA);
        }
      }
    }}
  };

  auto readin_r = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD4(Type, ainput, input, this->n, this->ic, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);
    bool is_Ir = _ic3 == this->ic3 - 1 && _I2 == this->I2 - 1;

    assert(this->ic4 == 1);
    if (is_Ir) {
      iter_each (_hA, A) {
      iter_each (_wA, A) {
        if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
            || _wA > _wA_end) {
#pragma omp simd
          iter_each (_V, V)
            ain[_hA][_wA][_V] = 0.0f;
        } else {
#pragma omp simd
          iter_each (_v, this->Ir)
            ain[_hA][_wA][_v] = md4(ainput, _n,
                (this->ic2 - 1) * V + _v, _ih + _hA, _iw + _wA);
        }
      }}
    } else {
      iter_each (_hA, A) {
      iter_each (_wA, A) {
        if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
            || _wA > _wA_end) {
#pragma omp simd
          iter_each (_V, V)
            ain[_hA][_wA][_V] = 0.0f;
        } else {
          if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
            constexpr int scale = sizeof(Type);
            __m<V> t = _mm<V>::i32gather_ps(vindex,
                &md4(ainput, _n, (_ic3 * this->I2 + _I2) * V, _ih + _hA, _iw + _wA),
                scale);
            _mm<V>::store_ps(ain[_hA][_wA], t);
          } else {
#pragma omp simd
            iter_each (_v, V)
              ain[_hA][_wA][_v] = md4(ainput, _n,
                  (_ic3 * this->I2 + _I2) * V + _v, _ih + _hA, _iw + _wA);
          }
        }
      }}
    }
  };

  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
    iter_each (_T, Tz) {
      if (this->Ir != V) {
        readin_r(_ic3, _I2, _T, ain);
      } else
        readin_v(_ic3, _I2, _T, ain);

      ker_trans_input_(*this, aout, (Type *)ain, 0, 0, 0, -1);

      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_in_) {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::stream_ps(&md6(atinput, _wA, _hA, _ic3, _I2, _T, 0),
                *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        } else {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::store_ps(&md6(atinput, _wA, _hA, _ic3, _I2, _T, 0),
                *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        }
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
#pragma omp simd
        iter_each (_V, V) {
          md6(atinput, _wA, _hA, _ic3, _I2, _T, _V) = aout[_wA][_hA][_V];
        }}}
      }
    }
  }}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_blocked(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  // n, ic2, ih, iw, V => t2 | wA, hA, ic3, I2, T, V
  MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, this->ih, this->iw, V);
  MD6(Type, atinput, tinput, A, A, this->ic3, this->I2, Tz, V);

  alignas(64) Type aout[A][A][V];

  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
  iter_each (_T, Tz) {
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    Type *in = &md7(ainput, _n, 0, _ic3, _I2, _ih, _iw, 0);
    if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
        && _wA_end == A - 1)
      ker_trans_input_(*this, aout, in, 0, A - 1, 0, A - 1);
    else
      ker_trans_input0_(
          *this, aout, in, _hA_start, _hA_end, _wA_start, _wA_end);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_in_) {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
          _mm<V>::stream_ps(&md6(atinput, _wA, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_wA][_hA][0]));
        }}
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
          _mm<V>::store_ps(&md6(atinput, _wA, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_wA][_hA][0]));
        }}
      }
    } else {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        md6(atinput, _wA, _hA, _ic3, _I2, _T, _V) = aout[_wA][_hA][_V];
      }}}
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_input(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  if (input_is_bfmt_ || input_as_bfmt_)
    __trans_input_blocked(tinput, input, _t2, Tz);
  else
    __trans_input_plain(tinput, input, _t2, Tz);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_blocked(
    Type * __restrict tinput, Type * __restrict input)
{
  // n, ic2, ih, iw, V => t2, wA, hA, ic3, I2, T, V
  MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, this->ih, this->iw, V);
  MD2(Type, atinput2, tinput, this->t2, A * A * this->T * this->ic3 * this->I2 * V);

#pragma omp for nowait collapse(3)
  iter_each (_t2, this->t2) {
  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD6(Type, atinput6, &md2(atinput2, _t2, 0), A, A, this->ic3, this->I2, Tz, V);
    alignas(64) Type aout[A][A][V];

    iter_each (_T, Tz) {
      int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
      t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

      Type *in = &md7(ainput, _n, 0, _ic3, _I2, _ih, _iw, 0);
      if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
          && _wA_end == A - 1)
        ker_trans_input_(*this, aout, in, 0, A - 1, 0, A - 1);
      else
        ker_trans_input0_(
            *this, aout, in, _hA_start, _hA_end, _wA_start, _wA_end);

      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_in_) {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::stream_ps(&md6(atinput6, _wA, _hA, _ic3, _I2, _T, 0),
                           *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        } else {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::store_ps(&md6(atinput6, _wA, _hA, _ic3, _I2, _T, 0),
                          *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        }
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
#pragma omp simd
        iter_each (_V, V) {
          md6(atinput6, _wA, _hA, _ic3, _I2, _T, _V) = aout[_wA][_hA][_V];
        }}}
      }
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_plain(
    Type * __restrict tinput, Type * __restrict input)
{
  // n, ic2, ih, iw, V => t2, wA, hA, ic3, I2, T, V
  MD2(Type, atinput2, tinput, this->t2, A * A * this->T * this->ic3 * this->I2 * V);

  SET_EPI32(this->ih * this->iw)

  auto readin_v = [&](int _t2, int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, V, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    iter_each (_hA, A) {
    iter_each (_wA, A) {
      if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
          || _wA > _wA_end) {
#pragma omp simd
        iter_each (_V, V)
          ain[_hA][_wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m<V> t = _mm<V>::i32gather_ps(vindex,
              &md7(ainput, _n, 0, _ic3, _I2, 0, _ih + _hA, _iw + _wA),
              scale);
          _mm<V>::store_ps(ain[_hA][_wA], t);
        } else {
#pragma omp simd
          iter_each (_V, V)
            ain[_hA][_wA][_V]
                = md7(ainput, _n, 0, _ic3, _I2, _V, _ih + _hA, _iw + _wA);
        }
      }
    }}
  };

  auto readin_r = [&](int _t2, int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD4(Type, ainput, input, this->n, this->ic, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    assert(this->ic4 == 1);
    bool is_Ir = _ic3 == this->ic3 - 1 && _I2 == this->I2 - 1;

    if (is_Ir) {
      iter_each (_hA, A) {
        iter_each (_wA, A) {
          if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
              || _wA > _wA_end) {
#pragma omp simd
            iter_each (_V, V)
              ain[_hA][_wA][_V] = 0.0f;
          } else {
#pragma omp simd
            iter_each (_v, this->Ir)
              ain[_hA][_wA][_v] = md4(ainput, _n,
                  (this->ic2 - 1) * V + _v, _ih + _hA, _iw + _wA);
          }
        }
      }
    } else {
      iter_each (_hA, A) {
        iter_each (_wA, A) {
          if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
              || _wA > _wA_end) {
#pragma omp simd
            iter_each (_V, V)
              ain[_hA][_wA][_V] = 0.0f;
          } else {
            if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
              constexpr int scale = sizeof(Type);
              __m<V> t = _mm<V>::i32gather_ps(vindex,
                  &md4(ainput, _n, (_ic3 * this->I2 + _I2) * V, _ih + _hA, _iw + _wA),
                  scale);
              _mm<V>::store_ps(ain[_hA][_wA], t);
            } else {
#pragma omp simd
              iter_each (_v, V)
                ain[_hA][_wA][_v] = md4(ainput, _n,
                    (_ic3 * this->I2 + _I2) * V + _v, _ih + _hA, _iw + _wA);
            }
          }
        }
      }

    }
  };

#pragma omp for nowait collapse(3)
  iter_each (_t2, this->t2) {
  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD6(Type, atinput6, &md2(atinput2, _t2, 0), A, A, this->ic3, this->I2, Tz, V);
    alignas(64) Type aout[A][A][V];
    alignas(64) Type ain[A][A][V];

    iter_each (_T, Tz) {
      if (this->Ir != V)
        readin_r(_t2, _ic3, _I2, _T, ain);
      else
        readin_v(_t2, _ic3, _I2, _T, ain);
      ker_trans_input_(*this, aout, (Type *)ain, 0, 0, 0, -1);

      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_in_) {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::stream_ps(&md6(atinput6, _wA, _hA, _ic3, _I2, _T, 0),
                *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        } else {
          iter_each (_wA, A) {
          iter_each (_hA, A) {
            _mm<V>::store_ps(&md6(atinput6, _wA, _hA, _ic3, _I2, _T, 0),
                *((__m<V> *)&aout[_wA][_hA][0]));
          }}
        }
      } else {
        iter_each (_wA, A) {
        iter_each (_hA, A) {
#pragma omp simd
        iter_each (_V, V) {
          md6(atinput6, _wA, _hA, _ic3, _I2, _T, _V) = aout[_wA][_hA][_V];
        }}}
      }
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_input(
    Type *tinput, Type *input)
{
  if (input_is_bfmt_ || input_as_bfmt_)
    __trans_input_blocked(tinput, input);
  else
    __trans_input_plain(tinput, input);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_inputa_blocked(
    Type *tinput, Type *input, int _t2, int _wA, int Tz)
{
  // n, ic2, ih, iw, V => t2, wA | hA, ic3, I2, T, V
  MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, this->ih, this->iw, V);
  MD5(Type, atinput, tinput, A, this->ic3, this->I2, Tz, V);

  alignas(64) Type aout[A][A][V];

  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
  iter_each (_T, Tz) {
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    Type *in = &md7(ainput, _n, 0, _ic3, _I2, _ih, _iw, 0);
    if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
        && _wA_end == A - 1) {
      ker_trans_inputa_(*this, aout, in, _wA, 0, A - 1, 0, A - 1);
    } else {
      ker_trans_inputa0_(
          *this, aout, in, _wA, _hA_start, _hA_end, _wA_start, _wA_end);
    }

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_in_) {
        iter_each (_hA, A) {
          _mm<V>::stream_ps(&md5(atinput, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_hA][_wA][0]));
        }
      } else {
        iter_each (_hA, A) {
          _mm<V>::store_ps(&md5(atinput, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_hA][_wA][0]));
        }
      }
    } else {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        md5(atinput, _hA, _ic3, _I2, _T, _V) = aout[_hA][_wA][_V];
      }}
    }
  }}}
}
 
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_inputa_plain(
    Type * __restrict tinput, Type * __restrict input, int _t2, int _wA, int Tz)
{
  // n, ic2, ih, iw, V => t2, wA | hA, ic3, I2, T, V
  MD5(Type, atinput, tinput, A, this->ic3, this->I2, Tz, V);

  alignas(64) Type aout[A][A][V];
  alignas(64) Type ain[A][A][V];
  SET_EPI32(this->ih * this->iw)

  auto readin_v = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD7(Type, ainput, input, this->n, this->ic4, this->ic3, this->I2, V, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    iter_each (__wA, A) {
    iter_each (__hA, A) {
      if (__hA < _hA_start || __hA > _hA_end || __wA < _wA_start
          || __wA > _wA_end) {
#pragma omp simd
        iter_each (_V, V)
          ain[__hA][__wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m<V> t = _mm<V>::i32gather_ps(vindex,
              &md7(ainput, _n, 0, _ic3, _I2, 0, _ih + __hA, _iw + __wA),
              scale);
          _mm<V>::store_ps(ain[__hA][__wA], t);
        } else {
#pragma omp simd
          iter_each (_V, V)
            ain[__hA][__wA][_V]
                = md7(ainput, _n, 0, _ic3, _I2, _V, _ih + __hA, _iw + __wA);
        }
      }
    }}
  };

  auto readin_r = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD4(Type, ainput, input, this->n, this->ic, this->ih, this->iw);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    assert(this->ic4 == 1);
    bool is_Ir = _ic3 == this->ic3 - 1 && _I2 == this->I2 - 1;

    if (is_Ir) {
      iter_each (__wA, A) {
        iter_each (__hA, A) {
          if (__hA < _hA_start || __hA > _hA_end || __wA < _wA_start
              || __wA > _wA_end) {
#pragma omp simd
            iter_each (_V, V)
              ain[__hA][__wA][_V] = 0.0f;
          } else {
#pragma omp simd
            iter_each (_V, this->Ir)
              ain[__hA][__wA][_V] = md4(ainput, _n,
                  (_ic3 * this->I2 + _I2) * V + _V, _ih + __hA, _iw + __wA);
          }
        }
      }
    } else {
      iter_each (__wA, A) {
        iter_each (__hA, A) {
          if (__hA < _hA_start || __hA > _hA_end || __wA < _wA_start
              || __wA > _wA_end) {
#pragma omp simd
            iter_each (_V, V)
              ain[__hA][__wA][_V] = 0.0f;
          } else {
            if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
              constexpr int scale = sizeof(Type);
              __m<V> t = _mm<V>::i32gather_ps(vindex,
                  &md4(ainput, _n, (_ic3 * this->I2 + _I2) * V, _ih + __hA, _iw + __wA),
                  scale);
              _mm<V>::store_ps(ain[__hA][__wA], t);
            } else {
#pragma omp simd
              iter_each (_V, V)
                ain[__hA][__wA][_V] = md4(ainput, _n,
                    (_ic3 * this->I2 + _I2) * V + _V, _ih + __hA, _iw + __wA);
            }
          }
        }
      }
    }
  };

  iter_each (_ic3, this->ic3) {
  iter_each (_I2, this->I2) {
  iter_each (_T, Tz) {
    if (this->Ir != V)
      readin_r(_ic3, _I2, _T, ain);
    else
      readin_v(_ic3, _I2, _T, ain);
    ker_trans_inputa_(*this, aout, (Type *)ain, _wA, 0, A - 1, 0, -1);

    if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
      if (stream_in_) {
        iter_each (_hA, A)
          _mm<V>::stream_ps(&md5(atinput, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_hA][_wA][0]));
      } else {
        iter_each (_hA, A)
          _mm<V>::store_ps(&md5(atinput, _hA, _ic3, _I2, _T, 0),
              *((__m<V> *)&aout[_hA][_wA][0]));
      }
    } else {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        md5(atinput, _hA, _ic3, _I2, _T, _V) = aout[_hA][_wA][_V];
      }}
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_inputa(
    Type *tinput, Type *input, int _t2, int _wA, int Tz)
{
  if(input_is_bfmt_ || input_as_bfmt_)
    __trans_inputa_blocked(tinput, input, _t2, _wA, Tz);
  else
    __trans_inputa_plain(tinput, input, _t2, _wA, Tz);
}

// tweights:     oc4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:       t2 | A, A, ic3, I2, T, V
// toutput:  t2, oc4 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemm(
    Type *toutput, Type *tinput, Type *tweights, int _t2, int Tz, int _ic4)
{
  auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;
  auto ker_gemm_tail = (_t2 == this->t2 - 1) ? ker_gemm0_tail_ : ker_gemm_tail_;

  MD6(Type, atinput, tinput, A, A, this->ic3, this->I2, Tz, V);
  MD6(Type, atoutput, toutput, A, A, this->oc3, this->O2, Tz, V);
  MD5(Type, atweights, tweights, this->oc3, this->ic3, A, A, this->O2 * this->I2 * V * V);

  iter_each (_wA, A) {
    iter_each (_hA, A) {
      iter_each (_oc3, this->oc3) {
        bool last_ic4 = _ic4 == this->ic4 - 1;
        int ic3 = last_ic4 ? this->ic3 - 1 : this->ic3;
        iter_each (_ic3, ic3) {
          ker_gemm(*this, &md6(atoutput, _wA, _hA, _oc3, 0, 0, 0),
              &md6(atinput, _wA, _hA, _ic3, 0, 0, 0),
              &md5(atweights, _oc3, _ic3, _wA, _hA, 0),
	      nullptr, _ic3 == 0 && _ic4 == 0, false);
        }
        if (last_ic4) {
          ker_gemm_tail(*this, &md6(atoutput, _wA, _hA, _oc3, 0, 0, 0),
              &md6(atinput, _wA, _hA, this->ic3 - 1, 0, 0, 0),
              &md5(atweights, _oc3, this->ic3 - 1, _wA, _hA, 0),
	      nullptr, this->ic3 == 1 && this->ic4 == 1, false);
        }
      }
    }
  }
}

// tweights: oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2, A, A, ic3, I2, T, V
// toutput:  t2, A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemm(
    Type * __restrict toutput, Type * __restrict tinput, Type * __restrict tweights, int _ic4)
{
  MD2(Type, atinput2, tinput, this->t2, A * A * this->T * this->ic3 * this->I2 * V);
  MD2(Type, atoutput2, toutput, this->t2, A * A * this->T * this->oc3 * this->O2 * V);
  MD5(Type, atweights, tweights, this->oc3, this->ic3, A, A, this->O2 * this->I2 * V * V);

#pragma omp for nowait collapse(4)
  iter_each (_t2, this->t2) {
    iter_each (_wA, A) {
      iter_each (_hA, A) {
        iter_each (_oc3, this->oc3) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;
          auto ker_gemm_tail
              = (_t2 == this->t2 - 1) ? ker_gemm0_tail_ : ker_gemm_tail_;
          MD6(Type, atinput6, &md2(atinput2, _t2, 0), A, A, this->ic3, this->I2, Tz, V);
          MD6(Type, atoutput6, &md2(atoutput2, _t2, 0), A, A, this->oc3, this->O2, Tz, V);
          bool last_ic4 = _ic4 == this->ic4 - 1;
          int ic3 = last_ic4 ? this->ic3 - 1 : this->ic3;

          iter_each (_ic3, ic3) {
            ker_gemm(*this, &md6(atoutput6, _wA, _hA, _oc3, 0, 0, 0),
                &md6(atinput6, _wA, _hA, _ic3, 0, 0, 0),
                &md5(atweights, _oc3, _ic3, _wA, _hA, 0),
		nullptr, _ic3 == 0 && _ic4 == 0, false);
          }
          if (last_ic4) {
            ker_gemm_tail(*this, &md6(atoutput6, _wA, _hA, _oc3, 0, 0, 0),
                &md6(atinput6, _wA, _hA, this->ic3 - 1, 0, 0, 0),
                &md5(atweights, _oc3, this->ic3 - 1, _wA, _hA, 0),
		nullptr, this->ic3 == 1 && this->ic4 == 1, false);
          }
        }
      }
    }
  }
}

// tweights:    oc4, A | A, oc3, ic3, O2, I2, V, V
// tinputs:      t2, A | A, ic3, I2, T, V
// toutput: t2, oc4, A | A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemma(
    Type * __restrict toutput, Type * __restrict tinput, Type *tweights, int _t2, int Tz)
{
  auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;
  auto ker_gemm_tail = (_t2 == this->t2 - 1) ? ker_gemm0_tail_ : ker_gemm_tail_;

  MD5(Type, atinput, tinput,  A, this->ic3, this->I2, Tz, V);
  MD5(Type, atoutput, toutput, A, this->oc3, this->O2, Tz, V);
  MD4(Type, atweights, tweights, A, this->oc3, this->ic3, this->O2 * this->I2 * V * V);

  iter_each (_hA, A) {
    iter_each (_oc3, this->oc3) {
      iter_each (_ic3, this->ic3 - 1) {
        ker_gemm(*this, &md5(atoutput, _hA, _oc3, 0, 0, 0),
            &md5(atinput, _hA, _ic3, 0, 0, 0),
            &md4(atweights, _hA, _oc3, _ic3, 0),
	    nullptr, _ic3 == 0, false);
      }
      ker_gemm_tail(*this, &md5(atoutput, _hA, _oc3, 0, 0, 0),
          &md5(atinput, _hA, this->ic3 - 1, 0, 0, 0),
          &md4(atweights, _hA, _oc3, this->ic3 - 1, 0),
	  nullptr, this->ic3 == 1, false);
    }
  }
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_plain(
    Type * __restrict output, Type * __restrict toutput, Type * __restrict bias
    , int _t2, int Tz)
{
  // A, A, oc3, O2, T, V -> n, OC, oh, ow
  MD6(Type, atoutput, toutput, A, A, this->oc3, this->O2, Tz, V);
  MD3(Type, abias, bias, this->oc3, this->O2, V);

  alignas(64) Type ain[A][A][V];
  alignas(64) Type aout[A - K + 1][A - K + 1][V];
  SET_EPI32(this->oh * this->ow)

  auto writeout_v = [&](int _oc3, int _O2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD7(Type, aoutput, output, this->n, this->oc4, this->oc3, this->O2, V, this->oh, this->ow);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
          constexpr int scale = sizeof(Type);
          _mm<V>::i32scatter_ps(
              &md7(aoutput, _n, 0, _oc3, _O2, 0, _oh + _hA, _ow + _wA),
              vindex, t, scale);
        } else {
#pragma omp simd
          iter_each (_V, V)
            md7(aoutput, _n, 0, _oc3, _O2, _V, _oh + _hA, _ow + _wA)
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

  auto writeout_r = [&](int _oc3, int _O2, int _T,
                        Type aout[A - K + 1][A - K + 1][V]) {
    MD4(Type, aoutput, output, this->n, this->oc, this->oh, this->ow);

    assert(this->oc4 == 1);
    int is_Or = _oc3 == this->oc3 - 1 && _O2 == this->O2 - 1;
    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (is_Or) {
#pragma omp simd
        iter_each (_V, this->Or)
          md4(aoutput, _n, (this->oc2 - 1) * V + _V, _oh + _hA, _ow + _wA)
              = aout[_hA][_wA][_V];
        } else {
          if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
            __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
            constexpr int scale = sizeof(Type);
            _mm<V>::i32scatter_ps(
                &md4(aoutput, _n, (_oc3 * this->O2 + _O2) * V, _oh + _hA, _ow + _wA),
                vindex, t, scale);
          } else {
#pragma omp simd
            iter_each (_V, V)
              md4(aoutput, _n, (_oc3 * this->O2 + _O2) * V + _V, _oh + _hA,
                  _ow + _wA)
                  = aout[_hA][_wA][_V];
          }
        }
      }
    }
  };

  iter_each (_oc3, this->oc3) {
  iter_each (_O2, this->O2) {
    iter_each (_T, Tz) {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        ain[_wA][_hA][_V] = md6(atoutput, _wA, _hA, _oc3, _O2, _T, _V);
      }}}

      ker_trans_output_(
          *this, (Type *)aout, ain, &md3(abias, _oc3, _O2, 0), 0, -1);

      if (this->Or != V)
        writeout_r(_oc3, _O2, _T, aout);
      else
        writeout_v(_oc3, _O2, _T, aout);
    }
  }}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_blocked(
    Type *output, Type *toutput, Type *bias, int _t2, int Tz)
{
  // A, A, oc3, O2, T, V -> n, oc2, oh, ow, V
  MD6(Type, atoutput, toutput, A, A, this->oc3, this->O2, Tz, V);
  MD7(Type, aoutput, output, this->n, this->oc4, this->oc3, this->O2, this->oh, this->ow, V);
  MD3(Type, abias, bias, this->oc3, this->O2, V);

  alignas(64) Type ain[A][A][V];

  iter_each (_oc3, this->oc3) {
  iter_each (_O2, this->O2) {
  iter_each (_T, Tz) {
    iter_each (_wA, A) {
    iter_each (_hA, A) {
#pragma omp simd
    iter_each (_V, V) {
      ain[_wA][_hA][_V] = md6(atoutput, _wA, _hA, _oc3, _O2, _T, _V);
    }}}

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
    Type *out = &md7(aoutput, _n, 0, _oc3, _O2, _oh, _ow, 0);

    if (_hOA_end < A - K || _wOA_end < A - K)
      ker_trans_output0_(*this, out, ain, &md3(abias, _oc3, _O2, 0), _hOA_end, _wOA_end);
    else
      ker_trans_output_(*this, out, ain, &md3(abias, _oc3, _O2, 0), A - K, A - K);
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_output(
    Type *output, Type *toutput, Type *bias, int _t2, int Tz)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_output_blocked(output, toutput, bias, _t2, Tz);
  else
    __trans_output_plain(output, toutput, bias, _t2, Tz);
}

// toutput:  mthr | hA/A, oc3, O2, T, V
// toutputa: t2, oc4 | oc3, O2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_outputa_th(
    Type *toutputa, Type *toutput, int Tz)
{
  MD4(Type, atoutput, toutput, A, this->oc3 * this->O2, Tz, V);
  MD4(Type, atoutputa, toutputa, this->oc3 * this->O2, Tz, A, (A - K + 1) * V);

  iter_each (_oc, this->oc3 * this->O2) {
    iter_each (_T, Tz) {
      ker_trans_outputa_th_(*this, &md4(atoutputa, _oc, _T, 0, 0),
        &md4(atoutput, 0, _oc, _T, 0), Tz, stream_out_);
    }
  }
}

// output: n, oc2, h, w, V
// toutputa: t2, oc2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_outputa_bh_blocked(
    Type *output, Type *toutputa, Type *bias)
{
  MD5(Type, aoutput, output, this->n, this->oc2, this->oh, this->ow, V);
  MD2(Type, abias, bias, this->oc2, V);
  MD2(Type, atoutputa2, toutputa, this->t2, A * (A - K + 1) * this->T * this->OC);

#pragma omp for nowait collapse(2)
  iter_each (_t2, this->t2) {
  iter_each (_oc2, this->oc2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD3(Type, atoutputa3, &md2(atoutputa2, _t2, 0), this->oc2, Tz, A * (A - K + 1) * V);

    iter_each (_T, Tz) {
      int _n, _oh, _ow, _hOA_end, _wOA_end;
      t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
      Type *out = &md5(aoutput, _n, _oc2, _oh, _ow, 0);
      using Array1 = Type[A][A - K + 1][V];
      Array1 *in = (Array1 *)&md3(atoutputa3, _oc2, _T, 0);

      if (_hOA_end < A - K || _wOA_end < A - K)
        ker_trans_outputa0_bh_(
            *this, out, *in, &md2(abias, _oc2, 0), _hOA_end, _wOA_end);
      else
        ker_trans_outputa_bh_(
            *this, out, *in, &md2(abias, _oc2, 0), A - K, A - K);
    }
  }}
}

// output: n, OC, h, w
// toutputa: t2, oc2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_outputa_bh_plain(
    Type * __restrict output, Type * __restrict toutputa, Type *bias)
{
  MD2(Type, abias, bias, this->oc2, V);
  MD2(Type, atoutputa2, toutputa, this->t2, A * (A - K + 1) * this->T * this->OC);

  SET_EPI32(this->oh * this->ow)

  auto writeout_v = [&](int _t2, int _oc2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD5(Type, aoutput, output, this->n, this->oc2, V, this->oh, this->ow);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
          constexpr auto scale = sizeof(Type);
          _mm<V>::i32scatter_ps(
              &md5(aoutput, _n, _oc2, 0, _oh + _hA, _ow + _wA),
              vindex, t, scale);
        } else {
#pragma omp simd
          iter_each (_V, V)
            md5(aoutput, _n, _oc2, _V, _oh + _hA, _ow + _wA)
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

  auto writeout_r = [&](int _t2, int _oc2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD4(Type, aoutput, output, this->n, this->oc, this->oh, this->ow);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    assert(this->oc4 == 1);
    bool is_Or = _oc2 == this->oc2 - 1;
    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (is_Or) {
#pragma omp simd
          iter_each (_V, this->Or)
            md4(aoutput, _n, _oc2 * V + _V, _oh + _hA, _ow + _wA)
                = aout[_hA][_wA][_V];
        } else {
          if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
            __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
            constexpr auto scale = sizeof(Type);
            _mm<V>::i32scatter_ps(
                &md4(aoutput, _n, _oc2 * V, _oh + _hA, _ow + _wA),
                vindex, t, scale);
          } else {
#pragma omp simd
            iter_each (_V, V)
              md4(aoutput, _n, _oc2 * V + _V, _oh + _hA, _ow + _wA)
                  = aout[_hA][_wA][_V];
          }
        }
      }
    }
  };

#pragma omp for nowait collapse(2)
  iter_each (_t2, this->t2) {
  iter_each (_oc2, this->oc2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD3(Type, atoutputa3, &md2(atoutputa2, _t2, 0), this->oc2, Tz, A * (A - K + 1) * V);
    alignas(64) Type aout[A - K + 1][A - K + 1][V];

    iter_each (_T, Tz) {
      using Array1 = Type[A][A - K + 1][V];
      Array1 *in = (Array1 *)&md3(atoutputa3, _oc2, _T, 0);

      ker_trans_outputa_bh_(
          *this, (Type *)aout, *in, &md2(abias, _oc2, 0), 0, -1);

      if (this->Or != V)
        writeout_r(_t2, _oc2, _T, aout);
      else
        writeout_v(_t2, _oc2, _T, aout);
    }
  }}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_outputa_bh(
    Type *output, Type *toutputa, Type *bias)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_outputa_bh_blocked(output, toutputa, bias);
  else
    __trans_outputa_bh_plain(output, toutputa, bias);

}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_blocked(
    Type *output, Type *toutput, Type *bias)
{
  // A, A, oc3, O2, T, V -> n, oc2, oh, ow, V
  MD7(Type, aoutput, output, this->n, this->oc4, this->oc3, this->O2, this->oh, this->ow, V);
  MD2(Type, atoutput2, toutput, this->t2, A * A * this->T * this->oc3 * this->O2 * V);
  MD3(Type, abias, bias, this->oc3, this->O2, V);

#pragma omp for nowait collapse(3)
  iter_each (_t2, this->t2) {
  iter_each (_oc3, this->oc3) {
  iter_each (_O2, this->O2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD6(Type, atoutput, &md2(atoutput2, _t2, 0), A, A, this->oc3, this->O2, Tz, V);
    alignas(64) Type ain[A][A][V];

    iter_each (_T, Tz) {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        ain[_wA][_hA][_V] = md6(atoutput, _wA, _hA, _oc3, _O2, _T, _V);
      }}}

      int _n, _oh, _ow, _hOA_end, _wOA_end;
      t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
      Type *out = &md7(aoutput, _n, 0, _oc3, _O2, _oh, _ow, 0);

      if (_hOA_end < A - K || _wOA_end < A - K)
        ker_trans_output0_(
            *this, out, ain, &md3(abias, _oc3, _O2, 0), _hOA_end, _wOA_end);
      else
        ker_trans_output_(
            *this, out, ain, &md3(abias, _oc3, _O2, 0), A - K, A - K);
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_plain(
    Type * __restrict output, Type * __restrict toutput, Type *bias)
{
  // A, A, oc3, O2, T, V -> n, OC, oh, ow
  MD3(Type, abias, bias, this->oc3, this->O2, V);
  MD2(Type, atoutput2, toutput, this->t2, A * A * this->T * this->oc3 * this->O2 * V);

  SET_EPI32(this->oh * this->ow)

  auto writeout_v = [&](int _t2, int _oc3, int _O2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD7(Type, aoutput, output, this->n, this->oc4, this->oc3, this->O2, V, this->oh, this->ow);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
          constexpr auto scale = sizeof(Type);
          _mm<V>::i32scatter_ps(
              &md7(aoutput, _n, 0, _oc3, _O2, 0, _oh + _hA, _ow + _wA),
              vindex, t, scale);
        } else {
#pragma omp simd
          iter_each (_V, V)
            md7(aoutput, _n, 0, _oc3, _O2, _V, _oh + _hA, _ow + _wA)
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

  auto writeout_r = [&](int _t2, int _oc3, int _O2, int _T,
                        Type aout[A - K + 1][A - K + 1][V]) {
    MD4(Type, aoutput, output, this->n, this->oc, this->oh, this->ow);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    assert(this->oc4 == 1);
    bool is_Or = _oc3 == this->oc3 - 1 && _O2 == this->O2 - 1;

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (is_Or) {
#pragma omp simd
          iter_each (_V, this->Or)
            md4(aoutput, _n, (this->oc2 - 1) * V + _V, _oh + _hA, _ow + _wA)
                = aout[_hA][_wA][_V];
        } else {
          if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
            __m<V> t = _mm<V>::load_ps(aout[_hA][_wA]);
            constexpr auto scale = sizeof(Type);
            _mm<V>::i32scatter_ps(
                &md4(aoutput, _n, (_oc3 * this->O2 + _O2) * V, _oh + _hA, _ow + _wA),
                vindex, t, scale);
          } else {
#pragma omp simd
            iter_each (_V, V)
              md4(aoutput, _n, (_oc3 * this->O2 + _O2) * V + _V, _oh + _hA,
                  _ow + _wA)
                  = aout[_hA][_wA][_V];
          }
        }
      }
    }
  };

#pragma omp for nowait collapse(3)
  iter_each (_t2, this->t2) {
  iter_each (_oc3, this->oc3) {
  iter_each (_O2, this->O2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD6(Type, atoutput6, &md2(atoutput2, _t2, 0), A, A, this->oc3, this->O2, Tz, V);
    alignas(64) Type ain[A][A][V];
    alignas(64) Type aout[A - K + 1][A - K + 1][V];

    iter_each (_T, Tz) {
      iter_each (_wA, A) {
      iter_each (_hA, A) {
#pragma omp simd
      iter_each (_V, V) {
        ain[_wA][_hA][_V] = md6(atoutput6, _wA, _hA, _oc3, _O2, _T, _V);
      }}}

      ker_trans_output_(
          *this, (Type *)aout, ain, &md3(abias, _oc3, _O2, 0), 0, -1);

      if (this->Or != V)
        writeout_r(_t2, _oc3, _O2, _T, aout);
      else
        writeout_v(_t2, _oc3, _O2, _T, aout);
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_output(
    Type *output, Type *toutput, Type *bias)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_output_blocked(output, toutput, bias);
  else
    __trans_output_plain(output, toutput, bias);
}

// tweights: oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2 | A, A, ic3, I2, T, V
// toutput:  t2 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a040(
    Type *output, Type *input, Type *weights, Type *bias)
{
  MD2(Type, atinput2, tinput_, mthr_, A * A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, mthr_, A * A * this->T * this->OC);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(1)
    iter_each (_t2, this->t2) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(&md2(atinput2, ithr, 0), input, _t2, Tz);
      gemm(&md2(atoutput2, ithr, 0),
           &md2(atinput2, ithr, 0), tweights_, _t2, Tz);
      trans_output(output, &md2(atoutput2, ithr, 0), bias, _t2, Tz);
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// tweights:     oc4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2      | A, A, ic3, I2, T, V
// toutput:  t2, oc4 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a061(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD2(Type, atinput2, tinput_, mthr_, A * A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, mthr_, A * A * this->T * this->oc3 * this->O2 * V);
  MD2(Type, atweights2, tweights_, this->oc4, A * A * this->IC * this->oc3 * this->O2 * V);

  MD3(Type, aoutput, output, this->n, this->oc4, this->oh * this->ow * this->oc3 * this->O2 * V);
  MD2(Type, abias, bias, this->oc4, this->oc3 * this->O2 * V);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights, this->oc4);
#pragma omp barrier
    }

    auto t2_history = -1;

#pragma omp for nowait collapse(2)
    iter_each (_t2, this->t2) {
      iter_each (_oc4, this->oc4) {
        int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
        size_t ithr = omp_get_thread_num();

        if (t2_history != _t2) {
          trans_input(&md2(atinput2, ithr, 0), input, _t2, Tz);
          t2_history = _t2;
        }
        gemm(&md2(atoutput2, ithr, 0), &md2(atinput2, ithr, 0),
            &md2(atweights2, _oc4, 0), _t2, Tz);
        trans_output(&md3(aoutput, 0, _oc4, 0), &md2(atoutput2, ithr, 0),
            &md2(abias, _oc4, 0), _t2, Tz);
      }
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// tweights:     oc4, wA | hA, oc3, ic3, O2, I2, V, V
// tinputa:  t2,      wA | hA, ic3, I2, T, V
// toutput:  t2, oc4, wA | hA, oc3, O2, T, V
// toutputa: t2, oc4, oc3, O2, T, wA, hA, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a0e1(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD2(Type, atinputa2, tinput_, mthr_, A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, mthr_, A * this->T * this->oc3 * this->O2 * V);
  MD2(Type, atoutputa2, toutputa_, this->t2, this->OC * A * (A - K + 1) * this->T);
  MD3(Type, atweights3, tweights_, this->oc4, A, A * this->IC * this->oc3 * this->O2 * V);
  MD3(Type, aoutput, output, this->n, this->oc4, this->oh * this->ow * this->oc3 * this->O2 * V);
  MD2(Type, abias, bias, this->oc4, this->oc3 * this->O2 * V);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weightsa(tweights_, weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(3)
    iter_each (_t2, this->t2) {
      iter_each (_oc4, this->oc4) {
        iter_each (_wA, A) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          size_t ithr = omp_get_thread_num();

          MD6(Type, atoutputa6, &md2(atoutputa2, _t2, 0), this->oc4, this->oc3, this->O2, Tz, A, (A - K + 1) * V);
          trans_inputa(&md2(atinputa2, ithr, 0), input, _t2, _wA, Tz);
          gemma(&md2(atoutput2, ithr, 0), &md2(atinputa2, ithr, 0),
              &md3(atweights3, _oc4, _wA, 0), _t2, Tz);
          trans_outputa_th(&md6(atoutputa6, _oc4, 0, 0, 0, _wA, 0),
              &md2(atoutput2, ithr, 0), Tz);
        }
      }
    }
#pragma omp barrier
    trans_outputa_bh(output, toutputa_, bias);
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// tweights:     oc4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2      | A, A, ic3, I2, T, V
// toutput:  t2, oc4 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a060(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD2(Type, atinput2, tinput_, this->t2, A * A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, mthr_, A * A * this->T * this->oc3 * this->O2 * V);
  MD2(Type, atweights2, tweights_, this->oc4, A * A * this->IC * this->oc3 * this->O2 * V);

  MD3(Type, aoutput, output, this->n, this->oc4, this->oh * this->ow * this->oc3 * this->O2 * V);
  MD2(Type, abias, bias, this->oc4, this->oc3 * this->O2 * V);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights, this->oc4);
    }
    trans_input(tinput_, input);
#pragma omp barrier

#pragma omp for nowait collapse(2)
    iter_each (_t2, this->t2) {
      iter_each (_oc4, this->oc4) {
        int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
        size_t ithr = omp_get_thread_num();

        gemm(&md2(atoutput2, ithr, 0), &md2(atinput2, _t2, 0),
            &md2(atweights2, _oc4, 0), _t2, Tz);
        trans_output(&md3(aoutput, 0, _oc4, 0), &md2(atoutput2, ithr, 0),
            &md2(abias, _oc4, 0), _t2, Tz);
      }
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// tweights:     oc4, wA | hA, oc3, ic3, O2, I2, V, V
// tinputa:  t2,      wA | hA, ic3, I2, T, V
// toutput:  t2, oc4, wA | hA, oc3, O2, T, V
// toutputa: t2, oc4, oc3, O2, T, wA, hA, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a0e0(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD2(Type, atinput2, tinput_, this->t2, A * A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, mthr_, A * this->T * this->oc3 * this->O2 * V);
  MD2(Type, atoutputa2, toutputa_, this->t2, this->OC * A * (A - K + 1) * this->T);
  MD3(Type, atweights3, tweights_, this->oc4, A, A * this->IC * this->oc3 * this->O2 * V);
  MD3(Type, aoutput, output, this->n, this->oc4, this->oh * this->ow * this->oc3 * this->O2 * V);
  MD2(Type, abias, bias, this->oc4, this->oc3 * this->O2 * V);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weightsa(tweights_, weights);
    }
    trans_input(tinput_, input);
#pragma omp barrier

#pragma omp for nowait collapse(3)
    iter_each (_t2, this->t2) {
      iter_each (_oc4, this->oc4) {
        iter_each (_wA, A) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          size_t ithr = omp_get_thread_num();

          MD6(Type, atoutputa6, &md2(atoutputa2, _t2, 0), this->oc4, this->oc3, this->O2, Tz, A, (A - K + 1) * V);
          MD2(Type, atinputa2, &md2(atinput2, _t2, 0), A, A * Tz * this->IC);
          gemma(&md2(atoutput2, ithr, 0), &md2(atinputa2, _wA, 0),
              &md3(atweights3, _oc4, _wA, 0), _t2, Tz);
          trans_outputa_th(&md6(atoutputa6, _oc4, 0, 0, 0, _wA, 0),
              &md2(atoutput2, ithr, 0), Tz);
        }
      }
    }
#pragma omp barrier
    trans_outputa_bh(output, toutputa_, bias);
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// tweights:     oc4, ic4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2,      ic4 | A, A, ic3, I2, T, V
// toutput:  t2, oc4      | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a072(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD2(Type, atinput2, tinput_, mthr_, A * A * this->T * this->ic3 * this->I2 * V);
  MD2(Type, atoutput2, toutput_, this->t2, this->oc4 * A * A * this->T * this->oc3 * this->O2 * V);
  MD3(Type, atweights3, tweights_, this->oc4, this->ic4, A * A * this->ic3 * this->I2 * V * this->oc3 * this->O2 * V);

  MD3(Type, ainput, input, this->n, this->ic4, this->ih * this->iw * this->ic3 * this->I2 * V);
  MD3(Type, aoutput, output, this->n, this->oc4, this->oh * this->ow * this->oc3 * this->O2 * V);
  MD2(Type, abias, bias, this->oc4, this->oc3 * this->O2 * V);

  if (is_first_run_) {
#pragma omp parallel num_threads(mthr_) proc_bind(close)
    trans_weights(tweights_, weights, this->oc4);
  }

  iter_each(_ic4, this->ic4) {
    int last_ic4 = -1, last_t2 = -1;
#pragma omp parallel num_threads(mthr_) proc_bind(close) firstprivate(last_ic4, last_t2)
#pragma omp for nowait collapse(2)
    iter_each(_t2, this->t2) {
      iter_each(_oc4, this->oc4) {
        int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
        size_t ithr = omp_get_thread_num();
        MD2(Type, atoutput3, &md2(atoutput2, _t2, 0), this->oc4, A * A * Tz * this->oc3 * this->O2 * V);

        if (last_ic4 != _ic4 || last_t2 != _t2) {
          trans_input(
              &md2(atinput2, ithr, 0), &md3(ainput, 0, _ic4, 0), _t2, Tz);
          last_t2 = _t2;
          last_ic4 = _ic4;
        }
        gemm(&md2(atoutput3, _oc4, 0), &md2(atinput2, ithr, 0),
            &md3(atweights3, _oc4, _ic4, 0), _t2, Tz, _ic4);
        if (_ic4 == this->ic4 - 1)
          trans_output(&md3(aoutput, 0, _oc4, 0), &md2(atoutput3, _oc4, 0),
              &md2(abias, _oc4, 0), _t2, Tz);
      }
    }
  }

  if (inference_acc_)
    is_first_run_ = false;
}

// Thread-teaming along 't' dimension.
// Fuse trans-input, gemm and trans-output along 't' dimension
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a448(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD3(Type, atinput3, tinput_, this->nteams, this->nthreads, A * A * this->T * this->IC);
  MD3(Type, atoutput3, toutput_, this->nteams, this->nthreads, A * A * this->T * this->OC);
  MD2(Type, atweights2, tweights_, this->nteams, this->OC * this->IC * A * A);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(&md2(atweights2, s, 0), weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(1) schedule(static)
    for (int _t2 = ttm_[s].start; _t2 <= ttm_[s].end; _t2++) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(&md3(atinput3, s, ithr, 0), input, _t2, Tz);
      gemm(&md3(atoutput3, s, ithr, 0), &md3(atinput3, s, ithr, 0),
          &md2(atweights2, s, 0), _t2, Tz);
      trans_output(output, &md3(atoutput3, s, ithr, 0), bias, _t2, Tz);
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// Flat mode
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a000(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_)
      trans_weights(tweights_, weights);
    trans_input(tinput_, input);
#pragma omp barrier
    gemm(toutput_, tinput_, tweights_);
#pragma omp barrier
    trans_output(output, toutput_, bias);
  }
  if (inference_acc_) is_first_run_ = false;
}


template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a010(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD3(Type, ainput, input, this->n, this->ic4, this->ih * this->iw * this->ic3 * this->I2 * V);
  MD2(Type, atweights, tweights_, this->ic4, A * A * this->ic3 * this->I2 * V * this->oc3 * this->O2 * V);

  if (is_first_run_) {
#pragma omp parallel num_threads(mthr_) proc_bind(close)
    trans_weights(tweights_, weights);
  }

  iter_each(_ic4, this->ic4)
#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    trans_input(tinput_, &md3(ainput, 0, _ic4, 0));
#pragma omp barrier
    gemm(toutput_, tinput_, &md2(atweights, _ic4, 0), _ic4);
  }

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  trans_output(output, toutput_, bias);

  if (inference_acc_) is_first_run_ = false;
}

// Thread teaming along 'o' dimension.
// Flat mode (no-fusion)
//
// tweights: nteams | oc3, ic3, A, A, O2, I2, V, V (oc3 /= nteams)
// tinputs:  nteams | t2, A, A, ic3, I2, T, V (dup)
// toutput:  nteams | t2, A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a201(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD3(Type, aoutput3, output, this->n, this->nteams, this->OC * this->oh * this->ow / this->nteams);
  MD2(Type, aweights2, weights, this->nteams, this->OC * this->IC * K * K / this->nteams);
  MD2(Type, abias2, bias, this->nteams, this->OC / this->nteams);
  MD2(Type, atinput2, tinput_, this->nteams, this->t2 * A * A * this->T * this->IC);
  MD2(Type, atoutput2, toutput_, this->nteams, this->t2 * A * A * this->T * this->OC / this->nteams);
  MD2(Type, atweights2, tweights_, this->nteams, this->OC * this->IC * A * A / this->nteams);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_)
      trans_weights(&md2(atweights2, s, 0), &md2(aweights2, s, 0));
    trans_input(&md2(atinput2, s, 0), input);
#pragma omp barrier
    gemm(&md2(atoutput2, s, 0), &md2(atinput2, s, 0), &md2(atweights2, s, 0));
#pragma omp barrier
    trans_output(
        &md3(aoutput3, 0, s, 0), &md2(atoutput2, s, 0), &md2(abias2, s, 0));
  }
  if (inference_acc_)
    is_first_run_ = false;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a241(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD3(Type, aoutput3, output, this->n, this->nteams, this->OC * this->oh * this->ow / this->nteams);
  MD2(Type, aweights2, weights, this->nteams, this->OC * this->IC * K * K / this->nteams);
  MD2(Type, abias2, bias, this->nteams, this->OC / this->nteams);
  MD3(Type, atinput3, tinput_, this->nteams, this->nthreads, A * A * this->T * this->IC);
  MD3(Type, atoutput3, toutput_, this->nteams, this->nthreads, A * A * this->T * this->OC / this->nteams);
  MD2(Type, atweights2, tweights_, this->nteams, this->OC * this->IC * A * A / this->nteams);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(&md2(atweights2, s, 0), &md2(aweights2, s, 0));
#pragma omp barrier
    }
#pragma omp for nowait collapse(1)
    iter_each (_t2, this->t2) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(&md3(atinput3, s, ithr, 0), input, _t2, Tz);
      gemm(&md3(atoutput3, s, ithr, 0), &md3(atinput3, s, ithr, 0),
          &md2(atweights2, s, 0), _t2, Tz);
      trans_output(&md3(aoutput3, 0, s, 0), &md3(atoutput3, s, ithr, 0),
          &md2(abias2, s, 0), _t2, Tz);
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::execute(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  if (is_bfmt_)
    return (this->*execute_opt_)(output, input, weights, bias);
  else {
    Type *in = input;
    Type *wei = weights;
    Type *out = output_as_bfmt_ ? boutput_ : output;

    if (input_as_bfmt_) {
      MD5(Type, abinput, binput_, this->n, this->ic2, this->ih, this->iw, V);
      MD4(Type, ainput, input, this->n, this->ic, this->ih, this->iw);

#pragma omp parallel for collapse(3)
      iter_each (_n, this->n) {
      iter_each (_ic2, this->ic2) {
      iter_each (_ih, this->ih) {
        int v = _ic2 == this->ic2 - 1 ? this->Ir : V;
        iter_each (_iw, this->iw) {
#pragma omp simd
          iter_each (_v, v)
            md5(abinput, _n, _ic2, _ih, _iw, _v)
                = md4(ainput, _n, _ic2 * V + _v, _ih, _iw);
        }
      }}}
      in = binput_;
    }

    if (weights_as_bfmt_) {
      MD6(Type, abweights, bweights_, this->oc2, this->ic2, this->kh, this->kw, V, V);
      MD4(Type, aweights, weights, this->oc, this->ic, this->kh, this->kw);

#pragma omp parallel for collapse(3)
      iter_each (_oc2, this->oc2) {
      iter_each (_ic2, this->ic2) {
      iter_each (_kh, this->kh) {
        int iv = _ic2 == this->ic2 - 1 ? this->Ir : V;
        int ov = _oc2 == this->oc2 - 1 ? this->Or : V;
        iter_each (_kw, this->kw) {
          iter_each (_iv, iv) {
#pragma omp simd
            iter_each (_ov, ov) {
              md6(abweights, _oc2, _ic2, _kh, _kw, _iv, _ov)
                = md4(aweights, _oc2 * V + _ov, _ic2 * V + _iv, _kh, _kw);
            }
          }
        }
      }}}
      wei = bweights_;
    }

    // TODO: padding bias

    (this->*execute_opt_)(out, in, wei, bias);

    if (output_as_bfmt_) {
      MD5(Type, aboutput, boutput_, this->n, this->oc2, this->oh, this->ow, V);
      MD4(Type, aoutput, output, this->n, this->oc, this->oh, this->ow);

#pragma omp parallel for collapse(3)
      iter_each (_n, this->n) {
      iter_each (_oc2, this->oc2) {
      iter_each (_oh, this->oh) {
        int v = _oc2 == this->oc2 - 1 ? this->Or : V;
        iter_each (_V, v) {
          iter_each (_ow, this->ow) {
            md4(aoutput, _n, _oc2 * V + _V, _oh, _ow)
              = md5(aboutput, _n, _oc2, _oh, _ow, _V);
          }
        }
      }}}
    }
  }
}

} // namespace euler
