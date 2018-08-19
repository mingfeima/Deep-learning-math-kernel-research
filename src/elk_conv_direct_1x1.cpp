#include <assert.h>
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "elk_conv_direct_1x1.hpp"

#define INCLUDE_DIRECT_CONVOLUTION_1X1_KERNEL

#include "kernel/elk_conv_direct_1x1_gemm_s1.hxx"
#include "kernel/elk_conv_direct_1x1_gemm_s2.hxx"
#include "kernel/elk_conv_direct_1x1_gemm_s1_ccs.hxx"
