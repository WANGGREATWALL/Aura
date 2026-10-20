#include "flow/xflow_api.h"

static int work(void* payload) { *(int*)payload = 1; return 0; }
int main(void)
{
    int called = 0;
    au_flow_handle handle = 0;
    au_flow_result result = {0};
    result.mStructSize = sizeof(result);
    if (au_flow_version() != AURA_FLOW_ABI_VERSION) return 4;
    if (au_flow_make(work, 0, &called, &handle)) return 1;
    if (au_flow_submit(handle)) return 2;
    if (au_flow_wait(handle, UINT64_MAX, &result) || result.mCode || called != 1) return 3;
    au_flow_release(handle);
    return au_flow_shutdown();
}
