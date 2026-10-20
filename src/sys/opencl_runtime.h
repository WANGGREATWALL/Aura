#ifndef AURA_SYS_OPENCL_RUNTIME_H_
#define AURA_SYS_OPENCL_RUNTIME_H_
namespace au {
namespace sys {
class XDLib;
namespace opencl {
// OS-specific loader paths stay in sys. No link-time OpenCL dependency.
int loadLibrary(au::sys::XDLib& library) noexcept;
} // namespace opencl
} // namespace sys
} // namespace au
#endif
