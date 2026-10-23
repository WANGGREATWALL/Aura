#ifndef AURA_XFLOW_API_H_
#define AURA_XFLOW_API_H_

#include <stddef.h>
#include <stdint.h>

#include "log/xerror.h"
#include "sys/xvisibility.h"

/**
 * @file xflow_api.h
 * @brief Task scheduling ABI for C99 and C++11 callers.
 * @note Completion covers the task body, not GPU work it merely submits.
 */
#ifdef __cplusplus
#define XFLOW_NOEXCEPT noexcept
extern "C" {
#else
#define XFLOW_NOEXCEPT
#endif
/**
 * @brief Owning task handle; retain before sharing and release each owned reference.
 * @note Release does not cancel or wait. Keep an owned reference while using the handle.
 */
typedef struct XFlowTaskNode* XFlowTaskHandle;
/**
 * @brief Owning group handle; keeps its member tasks alive.
 * @note Retain/release each owned reference. Release does not cancel or wait for members.
 */
typedef struct XFlowGroupNode* XFlowGroupHandle;
/// Task body: return 0 on success, otherwise an error passed to dependents.
typedef int (*XFlowTaskFn)(void* user_data);
/// Payload cleanup callback; must not throw.
typedef void (*XFlowDeleterFn)(void* user_data);
/// Tile callback with a per-loop execution lane: caller=0, pool workers=1..8.
typedef void (*XFlowTileExFn)(size_t begin, size_t end, size_t worker_idx, void* user_data);

/**
 * @brief Create a task without submitting it.
 * @param name Optional task name; copied on creation.
 * @param fn Task body; must not be null.
 * @param user_data Data passed to the task body.
 * @param deleter Cleanup for user_data, or null for borrowed data.
 * @return An owned handle, or null on failure.
 * @note Cleanup runs after execution or skipping, or when an unsubmitted task is destroyed.
 *       A supplied deleter also runs on creation failure; do not free the data again.
 */
AU_API XFlowTaskHandle au_flow_create_task(const char* name, XFlowTaskFn fn, void* user_data,
                                           XFlowDeleterFn deleter) XFLOW_NOEXCEPT;

/**
 * @brief Reserve unconstructed storage for a task's callable and arguments (its payload).
 * @param name Optional task name; copied on creation.
 * @param payload_size Object size in bytes; must be nonzero.
 * @param payload_align Required alignment; a nonzero power of two.
 * @param fn Task body that receives the constructed object.
 * @param dtor_only Object destructor callback; must not free storage or throw.
 * @param[out] out_payload Storage address; cleared to null on failure.
 * @return An owned handle, or null on failure.
 * @note Construct in place, then commit. Release before commit skips destruction.
 *       Up to 64 bytes with standard alignment fit inline; the runtime frees fallback storage.
 */
AU_API XFlowTaskHandle au_flow_create_task_ex(const char* name, size_t payload_size, size_t payload_align,
                                              XFlowTaskFn fn, XFlowDeleterFn dtor_only,
                                              void** out_payload) XFLOW_NOEXCEPT;

/**
 * @brief Mark an in-place payload as successfully constructed.
 * @param task Unsubmitted task returned by create_task_ex.
 * @return 0 on success, or an error if the task is invalid or already committed.
 * @note Call once after placement construction and before enqueue; this does not submit the task.
 */
AU_API int au_flow_commit_task_payload(XFlowTaskHandle task) XFLOW_NOEXCEPT;

AU_API void au_flow_retain_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

AU_API void au_flow_release_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Run succ only after pred completes; propagate predecessor failures without running succ.
 * @param pred Predecessor, including a running or completed task.
 * @param succ Successor; must not be submitted yet.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Device completion is not tracked; synchronize at device-to-CPU boundaries.
 *       Duplicate edges are ignored. Build errors on an unsubmitted successor are saved for enqueue.
 */
AU_API int au_flow_add_dependency(XFlowTaskHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Submit a task for asynchronous execution when its dependencies finish.
 * @param task Task to submit.
 * @return 0 if accepted or previously submitted; otherwise a submission error.
 * @note Submit unsubmitted predecessors first, or together in a group. Re-enqueue never reruns a task.
 */
AU_API int au_flow_enqueue_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Wait for a task and return its final result.
 * @param task Task to wait for.
 * @return Task result, kErrorStateInvalid for an unsubmitted task, or a build/wait error.
 * @note Does not synchronize device work. May execute other ready tasks; never submits the target.
 *       Disallowed waits return kErrorFlowWouldDeadlock.
 */
AU_API int au_flow_wait_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Read the task result without waiting for completion.
 * @param task Task to query.
 * @return Final result when done, a saved build error, kErrorNotReady otherwise, or an argument error.
 */
AU_API int au_flow_get_task_result(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Test completion without waiting.
 * @param task Task to query.
 * @return 1 only after completion; 0 for unfinished or null tasks, or on query failure.
 * @note A completed task may itself return kErrorNotReady; use this query to distinguish it.
 */
AU_API int au_flow_task_done(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Read the final execution time in microseconds.
 * @param task Task to query.
 * @return Elapsed execution time; 0 before completion, for an invalid handle, or on query failure.
 * @note Does not wait. Wall time excludes queue/dependency delay; a completed task can also report 0.
 */
AU_API int64_t au_flow_get_task_cost_us(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Create an empty task group for batch submission and completion.
 * @param name Optional group name; copied on creation.
 * @return An owned group handle, or null on failure.
 */
AU_API XFlowGroupHandle au_flow_create_group(const char* name) XFLOW_NOEXCEPT;

AU_API void au_flow_retain_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

AU_API void au_flow_release_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Add a task that the group must wait for.
 * @param group Group whose submission has not started.
 * @param task Member task; duplicate additions are ignored.
 * @return 0 on success, or an argument, state or dependency error.
 * @note A task not yet submitted can join one group; submitted tasks can be joined by several.
 *       The group retains the first add error and reports it on enqueue or wait.
 */
AU_API int au_flow_group_add_task(XFlowGroupHandle group, XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Start succ only after every task in pred finishes.
 * @param pred Predecessor group.
 * @param succ Successor group whose members are not yet submitted.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Connects the predecessor exit to the successor entry; failures short-circuit downstream tasks.
 */
AU_API int au_flow_add_group_dependency(XFlowGroupHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Start a group only after a predecessor task finishes.
 * @param pred Predecessor task.
 * @param succ Successor group whose members are not yet submitted.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Failures in pred short-circuit the successor group. Device completion is not tracked.
 */
AU_API int au_flow_add_task_group_dependency(XFlowTaskHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Run a task only after every task in a group finishes.
 * @param pred Predecessor group.
 * @param succ Successor task; must not be submitted yet.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Failures in the group short-circuit succ.
 */
AU_API int au_flow_add_group_task_dependency(XFlowGroupHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Submit all unsubmitted group members as one batch.
 * @param group Group to submit.
 * @return 0 if accepted, or the first build/submission error.
 * @note Already submitted members are not rerun; validation precedes publishing new members.
 */
AU_API int au_flow_enqueue_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Wait for the group completion node.
 * @param group Group to wait for.
 * @return 0 if all work succeeds, a propagated task error, or a build/wait error.
 * @note Does not submit the group; has the same cooperative wait rules as wait_task.
 */
AU_API int au_flow_wait_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Split [0, range) into synchronous tiles with a per-loop lane for scratch storage.
 * @param range Number of elements; zero does nothing.
 * @param tile Elements per tile; zero means 1.
 * @param fn Callback with worker_idx: caller=0, pool slot=1..8; null does nothing.
 * @param user_data Borrowed callback data, valid until this call returns.
 * @note Indices need not be contiguous; concurrent loops need separate scratch arrays.
 *       Nested loops run serially with lane 0; external waiters do not help tiles.
 *       Caller plus at most 3 helpers; callbacks must not throw or wait on unfinished tasks.
 */
AU_API void au_flow_parallelize_tile_ex(size_t range, size_t tile, XFlowTileExFn fn, void* user_data) XFLOW_NOEXCEPT;

/**
 * @brief Wait until accepted tasks and synchronous tile calls have all finished.
 * @return 0 when drained, or a wait error; task failures remain available from their handles.
 * @note Ignores unsubmitted tasks and does not wait for device work. New submissions may extend the wait.
 *       Calling from a task or tile returns kErrorFlowWouldDeadlock.
 */
AU_API int au_flow_wait_all(void) XFLOW_NOEXCEPT;

/**
 * @brief Request a CPU mask for pool workers without binding the calling thread.
 * @param mask Bit N selects CPU N (CPU6+7 = 0xC0); 0 restores the initial full CPU set.
 * @return 0 if accepted, or an argument/platform error; unsupported platforms return kErrorNotSupported.
 * @note Requested CPUs must belong to the scheduler's initial allowed set. Workers apply changes
 *       asynchronously; OS application failures are logged. Set again to retry; use sys helpers to select CPUs.
 */
AU_API int au_flow_set_affinity_mask(uint64_t mask) XFLOW_NOEXCEPT;

/**
 * @brief Read the requested worker affinity mask for CPUs 0..63.
 * @return Requested mask, or 0 when unavailable.
 * @note Not proof of each worker's actual binding; a zero request also restores allowed CPUs numbered above 63.
 */
AU_API uint64_t au_flow_get_affinity_mask(void) XFLOW_NOEXCEPT;

/// Created pool workers (0..8), including sleepers but excluding callers. Returns 0 on query failure.
AU_API size_t au_flow_get_worker_count(void) XFLOW_NOEXCEPT;

/**
 * @brief Drain accepted tasks and tiles, join all workers, and allow lazy creation again.
 * @return 0 on success, or kErrorFlowWouldDeadlock from a task/tile/worker; other failures return an error.
 * @note External submissions wait at the boundary; in-flight tasks/tiles may still submit.
 *       Task failures are read from their handles. Device work still needs an explicit wait.
 *       Callers must finish all external API calls before unloading the library.
 */
AU_API int au_flow_release_workers(void) XFLOW_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif
