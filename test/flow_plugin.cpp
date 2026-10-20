#include "flow/xflow.h"

#if defined(_WIN32)
#define AURA_FLOW_TEST_EXPORT __declspec(dllexport)
#else
#define AURA_FLOW_TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" {
AURA_FLOW_TEST_EXPORT void* flow_test_instance() { return &au::flow::XFlow::get(); }
AURA_FLOW_TEST_EXPORT int flow_test_make(au_flow_handle* handle)
{
    return au_flow_make([](void*) -> int { return 0; }, nullptr, nullptr, handle);
}
AURA_FLOW_TEST_EXPORT int flow_test_submit(au_flow_handle handle) { return au_flow_submit(handle); }
AURA_FLOW_TEST_EXPORT int flow_test_wait(au_flow_handle handle) { return au_flow_wait(handle, UINT64_MAX, nullptr); }
AURA_FLOW_TEST_EXPORT void flow_test_release(au_flow_handle handle) { au_flow_release(handle); }
AURA_FLOW_TEST_EXPORT int flow_test_shutdown() { return au_flow_shutdown(); }
}
