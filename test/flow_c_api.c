#include "flow/xflow_api.h"
#include "gpu/xopencl_api.h"
#include "mm/xaccess_api.h"

_Static_assert(AURA_FLOW_ABI_VERSION == 1u, "flow ABI version");
_Static_assert(AU_FLOW_INVALID == 0 && AU_FLOW_REJECTED == 9, "flow state numbering");

static int work(void* payload)
{
    *(int*)payload = 1;
    return 0;
}
static int failure(void* payload)
{
    (void)payload;
    return 17;
}
static void release(void* payload) { ++*(int*)payload; }
int         main(void)
{
    int                   called       = 0;
    unsigned char         bytes[128]   = {0};
    au_mm_external_memory memory       = {0};
    au_mm_access_set      access       = {0};
    void*                 registration = 0;
    void*                 scope        = 0;
    memory.mStructSize                 = sizeof(memory);
    memory.mData                       = bytes;
    memory.mSize                       = sizeof(bytes);
    memory.mFd                         = -1;
    access.mStructSize                 = sizeof(access);
    access.mCount                      = 1;
    access.mRanges[0].mData            = bytes;
    access.mRanges[0].mSize            = sizeof(bytes);
    access.mRanges[0].mMode            = AU_MM_WRITE;
    if (au_mm_register(&memory, &registration))
        return 5;
    size_t bufferOffset     = 0;
    access.mRanges[0].mData = bytes + 7;
    access.mRanges[0].mSize = 16;
    if (au_gpu_buffer_offset(&access, &bufferOffset) || bufferOffset != 7)
        return 8;
    access.mRanges[0].mData = bytes;
    access.mRanges[0].mSize = sizeof(bytes);
    if (au_mm_cpu_begin(&access, 1, &scope) || au_mm_cpu_finish(scope, 0) || au_mm_unregister(registration))
        return 6;
    au_flow_handle handle = 0;
    au_flow_result result = {0};
    result.mStructSize    = sizeof(result);
    if (au_flow_version() != AURA_FLOW_ABI_VERSION)
        return 4;
    if (au_mm_access_version() != AURA_MM_ACCESS_ABI_VERSION)
        return 7;
    if (au_gpu_opencl_version() != AURA_GPU_OPENCL_ABI_VERSION)
        return 9;
    int            destroyed = 0;
    au_flow_handle rejected  = &destroyed;
    if (!au_flow_make(0, release, &destroyed, &rejected) || rejected || destroyed != 1)
        return 12;
    if (au_flow_make(work, 0, &called, &handle))
        return 1;
    if (!au_flow_id(handle) || au_flow_state(handle) != AU_FLOW_DRAFT || !au_flow_submission(handle))
        return 13;
    const uint64_t id       = au_flow_id(handle);
    au_flow_handle retained = handle;
    au_flow_retain(retained);
    au_flow_release(handle);
    handle = retained; /* One live owned reference remains. */
    if (au_flow_id(handle) != id)
        return 14;
    if (au_flow_submit(handle))
        return 2;
    if (au_flow_wait(handle, UINT64_MAX, &result) || result.mCode || called != 1)
        return 3;
    au_flow_release(handle);
    if (au_flow_make(failure, 0, 0, &handle) || au_flow_submit(handle))
        return 10;
    if (au_flow_wait(handle, UINT64_MAX, &result) || result.mCode != 17)
        return 11;
    au_flow_release(handle); /* The C waist still reports wait status separately. */
    return au_flow_shutdown();
}
