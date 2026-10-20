#include "cl_symbols.h"
#include <cstdio>

int main()
{
    const bool loaded = au::gpu::CLSymbols::isLoaded();
    cl_uint count = 0;
    const cl_int code = ::clGetPlatformIDs(0, nullptr, &count);
    std::printf("OpenCL loader=%s platforms=%u status=%d\n", loaded ? "loaded" : "unavailable", count, code);
    if (!loaded) return count == 0 && code == -1001 ? 0 : 1;
    // An ICD loader with no installed device implementation is a supported
    // unavailable state, not evidence that a kernel or cache path was tested.
    return (code == CL_SUCCESS && count > 0) || (code == -1001 && count == 0) ? 0 : 2;
}
