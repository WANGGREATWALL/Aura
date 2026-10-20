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

typedef void* au_flow_handle;
typedef int (*au_flow_invoke)(void*);
typedef void (*au_flow_destroy)(void*);
typedef int (*au_flow_tile)(void*, size_t, size_t);

enum au_flow_task_state {
    AU_FLOW_INVALID = 0, AU_FLOW_DRAFT, AU_FLOW_PENDING, AU_FLOW_READY,
    AU_FLOW_RUNNING, AU_FLOW_WAITING, AU_FLOW_SUCCEEDED, AU_FLOW_FAILED,
    AU_FLOW_SKIPPED, AU_FLOW_REJECTED
};
enum au_flow_error_domain {
    AU_FLOW_DOMAIN_NONE = 0, AU_FLOW_DOMAIN_FLOW, AU_FLOW_DOMAIN_CALLABLE,
    AU_FLOW_DOMAIN_MEMORY, AU_FLOW_DOMAIN_OPENCL, AU_FLOW_DOMAIN_SYSTEM
};

typedef struct au_flow_result {
    uint32_t mStructSize;
    uint32_t mState;
    uint32_t mDomain;
    int mCode;
    int mNativeCode;
    uint64_t mCauseTaskId;
} au_flow_result;

typedef struct au_flow_worker {
    uint32_t mStructSize;
    uint32_t mWorkerId;
    uint64_t mThreadId;
    uint64_t mRequestedVersion;
    uint64_t mAppliedVersion;
    int mAffinityCode;
} au_flow_worker;

/* make consumes payload on every path, including rejection. */
uint32_t au_flow_version(void) AU_FLOW_NOEXCEPT;
int au_flow_make(au_flow_invoke invoke, au_flow_destroy destroy, void* payload,
                 au_flow_handle* out) AU_FLOW_NOEXCEPT;
int au_flow_group(au_flow_handle* out) AU_FLOW_NOEXCEPT;
void au_flow_retain(au_flow_handle handle) AU_FLOW_NOEXCEPT;
void au_flow_release(au_flow_handle handle) AU_FLOW_NOEXCEPT;
uint64_t au_flow_id(au_flow_handle handle) AU_FLOW_NOEXCEPT;
uint32_t au_flow_state(au_flow_handle handle) AU_FLOW_NOEXCEPT;
int au_flow_submission(au_flow_handle handle) AU_FLOW_NOEXCEPT;
int au_flow_result_get(au_flow_handle handle, au_flow_result* out) AU_FLOW_NOEXCEPT;
int au_flow_name(au_flow_handle handle, const char* name) AU_FLOW_NOEXCEPT;
int au_flow_builder_error(au_flow_handle handle, int code) AU_FLOW_NOEXCEPT;
int au_flow_precede(au_flow_handle before, au_flow_handle after) AU_FLOW_NOEXCEPT;
int au_flow_add(au_flow_handle group, au_flow_handle member) AU_FLOW_NOEXCEPT;
void au_flow_abandon(au_flow_handle group) AU_FLOW_NOEXCEPT;
int au_flow_submit(au_flow_handle handle) AU_FLOW_NOEXCEPT;
void au_flow_record_error(int code) AU_FLOW_NOEXCEPT;
void au_flow_record_exception(void) AU_FLOW_NOEXCEPT;
int au_flow_wait(au_flow_handle handle, uint64_t timeoutMs,
                 au_flow_result* out) AU_FLOW_NOEXCEPT;
int au_flow_wait_all(void) AU_FLOW_NOEXCEPT;
int au_flow_tiled(size_t range, size_t tile, au_flow_tile invoke,
                  void* payload) AU_FLOW_NOEXCEPT;
int au_flow_affinity(uint32_t mode, const uint32_t* cpus, size_t count) AU_FLOW_NOEXCEPT;
int au_flow_workers(au_flow_worker* out, size_t capacity, size_t* count) AU_FLOW_NOEXCEPT;
int au_flow_worker_affinity(uint32_t worker, uint32_t* cpus, size_t capacity,
                            size_t* count) AU_FLOW_NOEXCEPT;
int au_flow_tracing(int enabled) AU_FLOW_NOEXCEPT;
int au_flow_dot(au_flow_handle handle, const char* path) AU_FLOW_NOEXCEPT;
int au_flow_trace(const char* path) AU_FLOW_NOEXCEPT;
int au_flow_shutdown(void) AU_FLOW_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef AU_FLOW_NOEXCEPT
#endif
