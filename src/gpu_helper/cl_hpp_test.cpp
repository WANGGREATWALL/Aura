// Optional OpenCL 1.2 hardware integration executable. Exit 77 means unavailable.
#include "gpu/xopencl.h"
#include "mm/xmemory.h"
#include "mm/xaccess.h"
#include <cstdio>
#include <cstring>

int main()
{
    const auto& gpu = au::gpu::CLWrapper::get();
    const int initialized = gpu.init();
    if (initialized) {
        std::fprintf(stderr, "OpenCL GPU initialization code=%d\n", initialized);
        return initialized == au::err::kErrorNotSupported ? 77 : 1;
    }
    struct Cleanup {
        void* mPointer = nullptr;
        ~Cleanup() {
            (void)au::gpu::CLWrapper::get().deinit();
            if (mPointer) (void)au::mm::free(mPointer);
        }
    } cleanup;
    au::mm::MemBlock memory;
    if (au::mm::alloc(64, au::mm::MemType::Pss, memory)) return 2;
    cleanup.mPointer = memory.ptr;
    std::memset(memory.ptr, 3, memory.size);
    const char* source = "__kernel void add(__global uchar* value, uchar delta) { value[get_global_id(0)] += delta; }";
    const int result = [&]() -> int {
        int code = gpu.setNDRange({64}).enqueue("add", source, nullptr,
            au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)), static_cast<unsigned char>(7));
        if (code) return code;
        au::mm::CpuAccessScope cpu;
        code = cpu.begin({au::mm::readAccess(memory.ptr, memory.size)});
        if (code) return code;
        for (std::size_t i = 0; i < memory.size; ++i)
            if (static_cast<unsigned char*>(memory.ptr)[i] != 10) return cpu.finish(91);
        return cpu.finish();
    }();
    const int completed = gpu.finish();
    if (result || completed) {
        std::fprintf(stderr, "OpenCL hardware test code=%d completion=%d\n", result, completed);
        return 3;
    }
    return 0;
}
