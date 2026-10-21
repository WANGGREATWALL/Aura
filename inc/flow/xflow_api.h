#ifndef AURA_FLOW_XFLOW_API_H_
#define AURA_FLOW_XFLOW_API_H_

/* Versioned, C-compatible waist. Handles belong to one statically linked Aura
 * instance and must be released before its algorithm shared object is unloaded. */
#include <stddef.h>
#include <stdint.h>

#define AURA_FLOW_ABI_VERSION 1u

#ifdef __cplusplus
#define AU_FLOW_NOEXCEPT noexcept
extern "C" {
#else
#define AU_FLOW_NOEXCEPT
#endif

/* A successful make/group returns one owning reference. Copying this pointer
 * does not retain it: call retain before transferring a reference across threads
 * and release each owned reference once. Queries require a live reference from
 * this instance. Released/fabricated pointers are invalid inputs, not safely
 * detectable errors; pool address reuse can alias another task. */
typedef void* au_flow_handle;
typedef int (*au_flow_invoke)(void*);
typedef void (*au_flow_destroy)(void*);
typedef int (*au_flow_tile)(void*, size_t, size_t);

enum au_flow_task_state
{
    AU_FLOW_INVALID = 0,
    AU_FLOW_DRAFT,
    AU_FLOW_PENDING,
    AU_FLOW_READY,
    AU_FLOW_RUNNING,
    AU_FLOW_WAITING,
    AU_FLOW_SUCCEEDED,
    AU_FLOW_FAILED,
    AU_FLOW_SKIPPED,
    AU_FLOW_REJECTED
};
enum au_flow_error_domain
{
    AU_FLOW_DOMAIN_NONE = 0,
    AU_FLOW_DOMAIN_FLOW,
    AU_FLOW_DOMAIN_CALLABLE,
    AU_FLOW_DOMAIN_MEMORY,
    AU_FLOW_DOMAIN_OPENCL,
    AU_FLOW_DOMAIN_SYSTEM
};

typedef struct au_flow_result
{
    uint32_t mStructSize;
    uint32_t mState;
    uint32_t mDomain;
    int      mCode;
    int      mNativeCode;
    uint64_t mCauseTaskId;
} au_flow_result;

typedef struct au_flow_worker
{
    uint32_t mStructSize;
    uint32_t mWorkerId;
    uint64_t mThreadId;
    uint64_t mRequestedVersion;
    uint64_t mAppliedVersion;
    int      mAffinityCode;
} au_flow_worker;

/* make consumes payload on every path, including rejection. */
uint32_t au_flow_version(void) AU_FLOW_NOEXCEPT;
/* Binding storage: release with the same instance, size and alignment. The C++11
 * facade constructs/destructs the concrete callable; the waist only owns bytes. */
int  au_flow_payload_allocate(size_t size, size_t alignment, void** out) AU_FLOW_NOEXCEPT;
void au_flow_payload_release(void* payload, size_t size, size_t alignment) AU_FLOW_NOEXCEPT;
int  au_flow_make(au_flow_invoke invoke, au_flow_destroy destroy, void* payload, au_flow_handle* out) AU_FLOW_NOEXCEPT;
int  au_flow_group(au_flow_handle* out) AU_FLOW_NOEXCEPT;
/* Only live handles of this instance participate; null/foreign retain and
 * release are no-ops. This does not make a stale/dangling handle valid. */
void     au_flow_retain(au_flow_handle handle) AU_FLOW_NOEXCEPT;
void     au_flow_release(au_flow_handle handle) AU_FLOW_NOEXCEPT;
uint64_t au_flow_id(au_flow_handle handle) AU_FLOW_NOEXCEPT;
uint32_t au_flow_state(au_flow_handle handle) AU_FLOW_NOEXCEPT;
int      au_flow_submission(au_flow_handle handle) AU_FLOW_NOEXCEPT;
int      au_flow_result_get(au_flow_handle handle, au_flow_result* out) AU_FLOW_NOEXCEPT;
int      au_flow_name(au_flow_handle handle, const char* name) AU_FLOW_NOEXCEPT;
int      au_flow_builder_error(au_flow_handle handle, int code) AU_FLOW_NOEXCEPT;
/* after must be Draft. Always check the return code: accepted endpoints cannot
 * be changed or rejected retroactively. Draft targets preserve the first build
 * error. Serialize editing/submitting/abandoning/releasing a Draft builder. */
int  au_flow_precede(au_flow_handle before, au_flow_handle after) AU_FLOW_NOEXCEPT;
int  au_flow_add(au_flow_handle group, au_flow_handle member) AU_FLOW_NOEXCEPT;
void au_flow_abandon(au_flow_handle group) AU_FLOW_NOEXCEPT;
int  au_flow_submit(au_flow_handle handle) AU_FLOW_NOEXCEPT;
void au_flow_record_error(int code) AU_FLOW_NOEXCEPT;
void au_flow_record_exception(void) AU_FLOW_NOEXCEPT;
/* C waist: zero means the wait completed, not that the task succeeded.
 * Inspect out->mCode for the task result. C++ wait methods combine both codes. */
int au_flow_wait(au_flow_handle handle, uint64_t timeoutMs, au_flow_result* out) AU_FLOW_NOEXCEPT;
/* Snapshot of asynchronous roots/descendants, excluding external synchronous
 * tiled Jobs. shutdown drains both. wait_all/shutdown from a tile are invalid. */
int au_flow_wait_all(void) AU_FLOW_NOEXCEPT;
/* Synchronous [0, range), tile > 0; callback receives (offset, count <= tile),
 * not (tileIndex, end). Short tails are valid and zero range invokes no callback.
 * One borrowed payload/callback may be used concurrently until all workers join.
 * Async task/GPU publication inside a tile is
 * rejected and recorded by the loop, including on the caller/serial paths.
 * Draft editing and nested synchronous tiled calls remain legal. */
int au_flow_tiled(size_t range, size_t tile, au_flow_tile invoke, void* payload) AU_FLOW_NOEXCEPT;
int au_flow_affinity(uint32_t mode, const uint32_t* cpus, size_t count) AU_FLOW_NOEXCEPT;
/* Enumerates live slots, not a permanent contiguous ID range. After idle
 * retirement a slot may be reused by a new thread; query retired slots returns
 * NotReady. A native affinity query pins the live thread until it returns. */
int au_flow_workers(au_flow_worker* out, size_t capacity, size_t* count) AU_FLOW_NOEXCEPT;
int au_flow_worker_affinity(uint32_t worker, uint32_t* cpus, size_t capacity, size_t* count) AU_FLOW_NOEXCEPT;
int au_flow_tracing(int enabled) AU_FLOW_NOEXCEPT;
/* Independent DOT history (values, no execution-node owners). Legacy tracing
 * also toggles graph capture. Enable before root submission; descendants inherit
 * the accepted scope's choice. Toggling does not retrofit old roots or discard
 * already captured history. */
int au_flow_graph_capture(int enabled) AU_FLOW_NOEXCEPT;
/* Requires tracing disabled; detaches stored timeline. Concurrent readers or
 * writers safely finish on their pinned old window. Does not clear DOT history. */
int au_flow_trace_reset(void) AU_FLOW_NOEXCEPT;
int au_flow_dot(au_flow_handle handle, const char* path) AU_FLOW_NOEXCEPT;
int au_flow_trace(const char* path) AU_FLOW_NOEXCEPT;
int au_flow_shutdown(void) AU_FLOW_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef AU_FLOW_NOEXCEPT
#endif
