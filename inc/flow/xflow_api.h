#ifndef AURA_INC_FLOW_XFLOW_API_H_
#define AURA_INC_FLOW_XFLOW_API_H_

#include <stddef.h>
#include <stdint.h>

#include "log/xerror.h"
#include "sys/xvisibility.h"
#ifdef __cplusplus
#define XFLOW_NOEXCEPT noexcept
extern "C" {
#else
#define XFLOW_NOEXCEPT
#endif
/* Owning handles: retain before copying, release once per owned reference.
 * Creation consumes user_data, including failure. Deleters must not throw. */
typedef struct XFlowTaskNode*  XFlowTaskHandle;
typedef struct XFlowGroupNode* XFlowGroupHandle;
typedef int (*XFlowTaskFn)(void* user_data);
typedef void (*XFlowDeleterFn)(void* user_data);
typedef void (*XFlowTileFn)(size_t begin, size_t end, void* user_data);
typedef void (*XFlowTileExFn)(size_t begin, size_t end, size_t worker_idx, void* user_data);

enum XFlowAffinityPolicy
{
    XFLOW_AFFINITY_DEFAULT     = 0,
    XFLOW_AFFINITY_PERFORMANCE = 1,
    XFLOW_AFFINITY_POWERSAVING = 2,
    XFLOW_AFFINITY_CUSTOM_MASK = 3
};

AURA_API XFlowTaskHandle au_flow_create_task(const char*, XFlowTaskFn, void*, XFlowDeleterFn) XFLOW_NOEXCEPT;

/* Allocates unconstructed storage, using a 64-byte inline region when possible.
 * out_payload is cleared on failure. Alignment must be a nonzero power of two.
 * Placement-construct, then commit; release without commit only frees storage.
 * dtor_only destroys the object; the runtime frees any fallback allocation. */
AURA_API XFlowTaskHandle au_flow_create_task_ex(const char* name, size_t payload_size, size_t payload_align,
                                                XFlowTaskFn fn, XFlowDeleterFn dtor_only,
                                                void** out_payload) XFLOW_NOEXCEPT;

AURA_API int au_flow_commit_task_payload(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API void au_flow_retain_task(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API void au_flow_release_task(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_add_dependency(XFlowTaskHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

AURA_API int au_flow_enqueue_task(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_wait_task(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_get_task_result(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API int64_t au_flow_get_task_cost_us(XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API XFlowGroupHandle au_flow_create_group(const char*) XFLOW_NOEXCEPT;

AURA_API void au_flow_retain_group(XFlowGroupHandle) XFLOW_NOEXCEPT;

AURA_API void au_flow_release_group(XFlowGroupHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_group_add_task(XFlowGroupHandle, XFlowTaskHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_add_group_dependency(XFlowGroupHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

AURA_API int au_flow_add_task_group_dependency(XFlowTaskHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

AURA_API int au_flow_add_group_task_dependency(XFlowGroupHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

AURA_API int au_flow_enqueue_group(XFlowGroupHandle) XFLOW_NOEXCEPT;

AURA_API int au_flow_wait_group(XFlowGroupHandle) XFLOW_NOEXCEPT;

/* Thread-local text, valid until the next dump on this thread. NULL on failure. */
AURA_API const char* au_flow_dump_group_dot(XFlowGroupHandle) XFLOW_NOEXCEPT;

/* Synchronous [begin,end) callbacks. tile==0 means 1. Callbacks must not throw. */
AURA_API void au_flow_parallelize_tile(size_t range, size_t tile, XFlowTileFn, void*) XFLOW_NOEXCEPT;

/* worker_idx is unique within one loop: caller=0, pool slot+1=1..8.
 * Concurrent loops must use separate scratch arrays. External waiters do not
 * help tiles, since they have no unique pool slot. Nested loops use caller=0. */
AURA_API void au_flow_parallelize_tile_ex(size_t range, size_t tile, XFlowTileExFn, void*) XFLOW_NOEXCEPT;

/* Global quiescence; first completion error since the previous barrier; consumed on return. */
AURA_API int au_flow_wait_all(void) XFLOW_NOEXCEPT;

AURA_API int au_flow_set_affinity(int policy, uint64_t custom_mask) XFLOW_NOEXCEPT;

AURA_API uint64_t au_flow_get_affinity_mask(void) XFLOW_NOEXCEPT;

AURA_API size_t au_flow_get_worker_count(void) XFLOW_NOEXCEPT;

/* Drains accepted tasks and synchronous tiles, joins workers, then permits
 * lazy respawn. Concurrent external submissions wait for this boundary;
 * in-flight task/tile submissions remain allowed to finish the drain.
 * Calling from a task/tile is a no-op to avoid joining the calling worker.
 * Like wait_all(), consumes the accumulated completion error. */
AURA_API void au_flow_release_idle_workers(void) XFLOW_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif
