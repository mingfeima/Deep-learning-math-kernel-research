#pragma once
#include <cmath>
#include <mutex>
#include <iostream>
#include <cstring>

namespace euler {

template <typename Type>
class cosim_base {
  constexpr static int max_errors = 20;
public:
  static void compare_small(const Type *__restrict l,
      const Type * __restrict r, int elem_count, double acc = 3e-3) {
    int errors = 0;
    static std::mutex mu;
    for (int i = 0; i < elem_count; ++ i) {
      auto l_d = static_cast<double>(l[i]);
      auto r_d = static_cast<double>(r[i]);

      auto delta = std::fabs(l_d - r_d);
      // auto rel_diff = delta / std::fabs(r_d + 1e-5);

      if (delta > acc) {
        mu.lock();
        std::cerr<<"Exceeded "<<acc<<" at index:"<<i<<"! "
          <<r_d<<" expected than "<<l_d<<"."<<std::endl;
        mu.unlock();
        if (errors ++ > max_errors)
          break;
      }
    }
  }
};

};
