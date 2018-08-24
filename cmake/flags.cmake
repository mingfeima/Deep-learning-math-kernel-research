if (__flags_included)
  return ()
endif()

set(__basic_flags "-Wall -Werror -Wextra")
list(APPEND __basic_flags "-fopenmp")
list(APPEND __basic_flags "-Wno-sign-compare")
list(APPEND __basic_flags "-Wno-uninitialized")
list(APPEND __basic_flags "-Wno-unused-variable")
list(APPEND __basic_flags "-Wno-unused-parameter")

set(__cxx_flags "-std=c++14")

if (__debug)
  set(__opt_flags "-O0 -g -DDEBUG")
else ()
  set(__opt_flags "-O2 -DNDEBUG")
endif ()

if (WITH_GK)
  list(APPEND __opt_flags "-DWITH_GK")
endif()

if (CMAKE_CXX_COMPILER MATCHES "icpc")
  list(APPEND __opt_flags "-xHost")
  #  list(APPEND __opt_flags "-qopt-report=5")
  list(APPEND __opt_flags "-qopt-zmm-usage=high")
  list(APPEND __opt_flags "-no-inline-max-size")
  list(APPEND __opt_flags "-no-inline-max-total-size")

  # FIXME: Workaround ICC 18.0.2 inline bug. Remove this if ICC bug fixed.
  if (NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18.0.2)
    set_source_files_properties(
      src/elx_conv_wino.cpp PROPERTIES COMPILE_FLAGS -finline-limit=80)
  endif ()
elseif(CMAKE_CXX_COMPILER MATCHES "clang")
  list(APPEND __opt_flags "-mavx512f")
  list(APPEND __opt_flags "-mavx512dq")
else()
  list(APPEND __basic_flags "-Wno-unused-result")
  list(APPEND __basic_flags "-Wno-unused-but-set-variable")
  list(APPEND __basic_flags "-Wno-misleading-indentation")
  list(APPEND __basic_flags "-Wno-unknown-pragmas")
  list(APPEND __basic_flags "-Wno-implicit-fallthrough")
  list(APPEND __opt_flags "-mavx512f")
  list(APPEND __opt_flags "-mavx512dq")
endif()

add_definitions(${__basic_flags} ${__cxx_flags} ${__opt_flags})
