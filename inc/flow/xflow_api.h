#ifndef AURA_INC_FLOW_XFLOW_API_H_
#define AURA_INC_FLOW_XFLOW_API_H_
#include <stddef.h>
#include <stdint.h>

#include "sys/xvisibility.h"
#define AURA_FLOW_ABI_VERSION 3u
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
enum XFlowErrorCode
{
    XFLOW_SUCCESS                = 0,
    XFLOW_ERROR_INVALID_ARGUMENT = -6001,
    XFLOW_ERROR_CYCLE_DETECTED   = -6002,
    XFLOW_ERROR_ALREADY_ENQUEUED = -6003,
    XFLOW_ERROR_DEP_FAILED       = -6004,
    XFLOW_ERROR_AFFINITY_FAILED  = -6005,
    XFLOW_ERROR_NO_MEMORY        = -6006,
    XFLOW_ERROR_WOULD_DEADLOCK   = -6007,
    XFLOW_ERROR_NOT_READY        = -6008,
    XFLOW_ERROR_EXCEPTION        = -6009
};
enum XFlowAffinityPolicy
{
    XFLOW_AFFINITY_DEFAULT     = 0,
    XFLOW_AFFINITY_PERFORMANCE = 1,
    XFLOW_AFFINITY_POWERSAVING = 2,
    XFLOW_AFFINITY_CUSTOM_MASK = 3
};
AURA_API XFlowTaskHandle  XFlow_CreateTask(const char*, XFlowTaskFn, void*, XFlowDeleterFn) XFLOW_NOEXCEPT;
AURA_API void             XFlow_RetainTask(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API void             XFlow_ReleaseTask(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_AddDependency(XFlowTaskHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;
AURA_API int              XFlow_EnqueueTask(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_WaitTask(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_GetTaskResult(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API int64_t          XFlow_GetTaskCostUs(XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API XFlowGroupHandle XFlow_CreateGroup(const char*) XFLOW_NOEXCEPT;
AURA_API void             XFlow_RetainGroup(XFlowGroupHandle) XFLOW_NOEXCEPT;
AURA_API void             XFlow_ReleaseGroup(XFlowGroupHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_GroupAddTask(XFlowGroupHandle, XFlowTaskHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_AddGroupDependency(XFlowGroupHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;
AURA_API int              XFlow_AddTaskGroupDependency(XFlowTaskHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;
AURA_API int              XFlow_AddGroupTaskDependency(XFlowGroupHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;
AURA_API int              XFlow_EnqueueGroup(XFlowGroupHandle) XFLOW_NOEXCEPT;
AURA_API int              XFlow_WaitGroup(XFlowGroupHandle) XFLOW_NOEXCEPT;
/* Thread-local text, valid until the next dump on this thread. NULL on failure. */
AURA_API const char* XFlow_DumpGroupDot(XFlowGroupHandle) XFLOW_NOEXCEPT;
/* Synchronous [begin,end) callbacks. tile==0 means 1. Callbacks must not throw. */
AURA_API void XFlow_ParallelizeTile(size_t range, size_t tile, XFlowTileFn, void*) XFLOW_NOEXCEPT;
/* Global quiescence; first completion error since the previous barrier; consumed on return. */
AURA_API int      XFlow_WaitAll(void) XFLOW_NOEXCEPT;
AURA_API int      XFlow_SetAffinity(int policy, uint64_t custom_mask) XFLOW_NOEXCEPT;
AURA_API uint64_t XFlow_GetAffinityMask(void) XFLOW_NOEXCEPT;
AURA_API size_t   XFlow_GetWorkerCount(void) XFLOW_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
