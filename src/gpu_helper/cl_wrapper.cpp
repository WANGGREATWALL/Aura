#include "cl_wrapper.h"

// Compatibility translation unit. The two C++11 facades share the native
// completion/resource implementation in src/gpu/opencl.cpp. No driver
// callback, mutable singleton NDRange or legacy VImage ABI remains here.
