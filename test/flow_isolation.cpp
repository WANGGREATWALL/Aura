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
    int (*shutdownA)() = reinterpret_cast<int (*)()>(dlsym(a, "flow_test_shutdown"));
    int (*shutdownB)() = reinterpret_cast<int (*)()>(dlsym(b, "flow_test_shutdown"));
    if (!instanceA || !instanceB || !makeA || !submitA || !submitB || !waitA || !releaseA || !shutdownA || !shutdownB) return 3;
    if (instanceA() == instanceB()) return 4;
    void* handle = nullptr;
    if (makeA(&handle)) return 5;
    if (submitB(handle) != au::err::kErrorInvalidHandle) return 6;
    if (submitA(handle) || waitA(handle)) return 7;
    releaseA(handle);
    if (shutdownA() || shutdownB()) return 8;
    if (dlclose(b) || dlclose(a)) return 9;
    return 0;
}
