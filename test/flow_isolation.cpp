#include "log/xerror.h"
#include <dlfcn.h>

int main(int argc, char** argv)
{
    if (argc != 3) return 1;
    void* a = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    void* b = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!a || !b) return 2;
    void* (*instanceA)() = reinterpret_cast<void* (*)()>(dlsym(a, "flow_test_instance"));
    void* (*instanceB)() = reinterpret_cast<void* (*)()>(dlsym(b, "flow_test_instance"));
    int (*makeA)(void**) = reinterpret_cast<int (*)(void**)>(dlsym(a, "flow_test_make"));
    int (*submitA)(void*) = reinterpret_cast<int (*)(void*)>(dlsym(a, "flow_test_submit"));
    int (*submitB)(void*) = reinterpret_cast<int (*)(void*)>(dlsym(b, "flow_test_submit"));
    int (*waitA)(void*) = reinterpret_cast<int (*)(void*)>(dlsym(a, "flow_test_wait"));
    void (*releaseA)(void*) = reinterpret_cast<void (*)(void*)>(dlsym(a, "flow_test_release"));
    void (*releaseB)(void*) = reinterpret_cast<void (*)(void*)>(dlsym(b, "flow_test_release"));
    void (*retainB)(void*) = reinterpret_cast<void (*)(void*)>(dlsym(b, "flow_test_retain"));
    int (*shutdownA)() = reinterpret_cast<int (*)()>(dlsym(a, "flow_test_shutdown"));
    int (*shutdownB)() = reinterpret_cast<int (*)()>(dlsym(b, "flow_test_shutdown"));
    if (!instanceA || !instanceB || !makeA || !submitA || !submitB || !waitA || !releaseA || !releaseB || !retainB || !shutdownA || !shutdownB) return 3;
    if (instanceA() == instanceB()) return 4;
    void* handle = nullptr;
    if (makeA(&handle)) return 5;
    if (submitB(handle) != au::err::kErrorInvalidHandle) return 6;
    retainB(handle); releaseB(handle); releaseB(handle); // Foreign ownership operations must all be no-ops.

    if (submitA(handle) || waitA(handle)) return 7;
    releaseA(handle);
    int (*beginMemoryA)(void**) = reinterpret_cast<int (*)(void**)>(dlsym(a, "flow_test_mm_begin"));
    int (*finishMemoryA)(void*) = reinterpret_cast<int (*)(void*)>(dlsym(a, "flow_test_mm_finish"));
    int (*finishMemoryB)(void*) = reinterpret_cast<int (*)(void*)>(dlsym(b, "flow_test_mm_finish"));
    if (!beginMemoryA || !finishMemoryA || !finishMemoryB) return 10;
    void* scope = nullptr;
    if (beginMemoryA(&scope) || finishMemoryB(scope) != au::err::kErrorInvalidHandle || finishMemoryA(scope)) return 11;
    int (*cppA)() = reinterpret_cast<int (*)()>(dlsym(a, "flow_test_cpp_run"));
    int (*cppB)() = reinterpret_cast<int (*)()>(dlsym(b, "flow_test_cpp_run"));
    if (!cppA || !cppB || cppA() || cppB()) return 12;
    int (*cppMemoryA)() = reinterpret_cast<int (*)()>(dlsym(a, "flow_test_cpp_memory"));
    int (*cppMemoryB)() = reinterpret_cast<int (*)()>(dlsym(b, "flow_test_cpp_memory"));
    if (!cppMemoryA || !cppMemoryB || cppMemoryA() || cppMemoryB()) return 13;
    const void* (*gpuA)() = reinterpret_cast<const void* (*)()>(dlsym(a, "flow_test_gpu_instance"));
    const void* (*gpuB)() = reinterpret_cast<const void* (*)()>(dlsym(b, "flow_test_gpu_instance"));
    void* (*backendA)() = reinterpret_cast<void* (*)()>(dlsym(a, "flow_test_gpu_backend"));
    void* (*backendB)() = reinterpret_cast<void* (*)()>(dlsym(b, "flow_test_gpu_backend"));
    if (!gpuA || !gpuB || !backendA || !backendB || gpuA() == gpuB()) return 14;
    void* nativeA = backendA(); void* nativeB = backendB();
    if ((nativeA || nativeB) && (!nativeA || !nativeB || nativeA == nativeB)) return 15;
    if (shutdownA() || shutdownB()) return 8;
    if (dlclose(b) || dlclose(a)) return 9;
    return 0;
}
