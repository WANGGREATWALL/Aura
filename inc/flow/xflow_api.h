#ifndef AURA_XFLOW_API_H_
#define AURA_XFLOW_API_H_

/**
 * @file xflow_api.h
 * @brief Task scheduling C ABI for C99 and C++11 callers; the C++11 facade is flow/xflow.h.
 *
 * @par Task lifecycle
 * A task is unsubmitted until an enqueue call accepts it, either directly or as an upstream
 * dependency. Once accepted, it is pending (waiting for dependencies), running, or done.
 * Enqueueing an accepted task returns 0 and never executes it again.
 *
 * @par Group model
 * A group has two internal gates: an entry gate and an exit gate. Adding an unsubmitted member
 * to a group that has not been enqueued links entry -> member -> exit. Adding an accepted task
 * to that group links task -> exit only, because its execution order is already fixed. A group
 * is in use while any owning handle exists or a downstream task or group depends on its exit.
 *
 * @note Completion means the callable returned or the task was skipped, and its payload was
 *       destroyed. Device work the callable starts asynchronously is not tracked.
 * @note All entry points are noexcept and report failures as negative error codes; no C++
 *       exception crosses this ABI.
 */

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
/**
 * @brief Process one tile [begin, end) of a synchronous parallel loop.
 * @param begin First element (inclusive).
 * @param end One past the last element (exclusive).
 * @param worker_idx Execution lane for per-loop scratch storage: 0 is the calling thread, and
 *                   1..8 are pool worker slots. Lane values need not be contiguous.
 * @param user_data Borrowed pointer passed to au_flow_parallelize_tile_ex.
 * @note Called concurrently from multiple threads; each element is processed exactly once.
 *       At most one callback per lane runs at a time within one loop, so lane-indexed scratch
 *       needs no lock. Separate loops still need separate scratch arrays.
 *       Must not throw (a throw terminates) or wait on unfinished tasks.
 */
typedef void (*XFlowTileExFn)(size_t begin, size_t end, size_t worker_idx, void* user_data);

/**
 * @brief Create a task without enqueueing it.
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
 * @note Call once after placement construction and before enqueue; this does not enqueue the task.
 */
AU_API int au_flow_commit_task_payload(XFlowTaskHandle task) XFLOW_NOEXCEPT;

AU_API void au_flow_retain_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

AU_API void au_flow_release_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Make succ run only after pred completes; a failed pred skips succ and propagates its error.
 * @param pred Predecessor in any state: unsubmitted, pending, running, or done. A completed pred is
 *             not rerun; its failure is inherited immediately.
 * @param succ Successor; must still be unsubmitted.
 * @return 0 on success, or kErrorInvalidParam, kErrorStateInvalid, kErrorFlowCycleDetected, or
 *         kErrorNoMemory.
 * @note Duplicate edges are ignored and return 0; self-edges and longer cycles are rejected.
 *       If succ is still unsubmitted, the first build error is saved on it and returned by the
 *       next enqueue or wait. A failed call with an already accepted succ (pending, running, or
 *       done) leaves it unchanged.
 * @note Device completion is not tracked: this orders task bodies only. Synchronize at device-to-CPU
 *       boundaries before a CPU consumer reads results produced by device work.
 */
AU_API int au_flow_add_dependency(XFlowTaskHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Atomically enqueue a task and all unsubmitted tasks reachable through its predecessors.
 * @param task Task to enqueue.
 * @return 0 if accepted now or previously; otherwise an error, and no task is accepted by this call.
 * @note The upstream walk collects unsubmitted predecessors and stops at any predecessor that is
 *       already accepted (pending, running, or done). Accepted tasks are not enqueued again or
 *       executed twice; their own predecessors were accepted at the same time.
 * @note An unsubmitted task that belongs to a group cannot be enqueued alone while that group is
 *       still in use. Enqueue the whole group, or a successor of the group, so that its exit is
 *       part of the same enqueue batch; otherwise this returns kErrorInvalidParam. After the group
 *       is released and no downstream task depends on its exit, the task may be enqueued alone,
 *       but its existing dependencies and any saved build error still apply.
 * @note Completion means the callable returned or the task was skipped, and its payload was
 *       destroyed. Device work the callable starts asynchronously is not tracked.
 */
AU_API int au_flow_enqueue_task(XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Wait for a task and return its final result.
 * @param task Task to wait for.
 * @return Task result; for an unsubmitted task, its saved build error or kErrorStateInvalid;
 *         otherwise a wait error.
 * @note Waiting is cooperative: the caller may execute other tasks that are already accepted.
 *       Waiting never enqueues its target. A task completes when its callable has returned or
 *       been skipped, and its payload has been destroyed. Device work started asynchronously
 *       by the callable is not tracked.
 * @note A wait that could deadlock returns kErrorFlowWouldDeadlock without blocking. This includes
 *       waiting from a tile callback, exceeding the cooperative-wait depth limit, waiting on a
 *       task that depends on the waiting task, or a wait cycle between tasks.
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
 * @note au_flow_get_task_result returns kErrorNotReady both for an unfinished task and for a
 *       finished task whose callable returned that value. Call au_flow_task_done first to
 *       distinguish the two.
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
 * @brief Create an empty task group for batch enqueue and completion.
 * @param name Optional group name; copied on creation.
 * @return An owned group handle, or null on failure.
 */
AU_API XFlowGroupHandle au_flow_create_group(const char* name) XFLOW_NOEXCEPT;

AU_API void au_flow_retain_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

AU_API void au_flow_release_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Add a task that the group must wait for.
 * @param group Group that has not been enqueued.
 * @param task Member task; duplicate additions are ignored.
 * @return 0 on success, or an argument, state, or dependency error.
 * @note An unsubmitted task can belong to only one group that is still in use. It can join a new
 *       group after the previous group is released and no downstream task or group depends on its
 *       exit; its existing dependencies and saved build errors remain. An accepted task (pending,
 *       running, or done) can be added to several groups as a completion dependency: those
 *       groups wait for it, but its execution order is unchanged and it is not rerun.
 * @note The group keeps its first add error and remains failed; enqueue and wait report it.
 *       Duplicate additions are ignored. Create a new group to retry after a failed add.
 */
AU_API int au_flow_group_add_task(XFlowGroupHandle group, XFlowTaskHandle task) XFLOW_NOEXCEPT;

/**
 * @brief Start succ only after every task in pred finishes.
 * @param pred Predecessor group.
 * @param succ Successor group whose members must still be unsubmitted.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Connects the predecessor group's exit to the successor group's entry; failures
 *       short-circuit downstream tasks.
 */
AU_API int au_flow_add_group_dependency(XFlowGroupHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Start a group only after a predecessor task finishes.
 * @param pred Predecessor task.
 * @param succ Successor group whose members must still be unsubmitted.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Failures in pred short-circuit the successor group. Device completion is not tracked.
 */
AU_API int au_flow_add_task_group_dependency(XFlowTaskHandle pred, XFlowGroupHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Run a task only after every task in a group finishes.
 * @param pred Predecessor group.
 * @param succ Successor task; must still be unsubmitted.
 * @return 0 on success, or an argument, state or cycle error.
 * @note Failures in the group short-circuit succ.
 */
AU_API int au_flow_add_group_task_dependency(XFlowGroupHandle pred, XFlowTaskHandle succ) XFLOW_NOEXCEPT;

/**
 * @brief Atomically enqueue all unsubmitted group members and their upstream dependencies.
 * @param group Group to enqueue.
 * @return 0 if accepted, or the first build/enqueue error.
 * @note All collected tasks are validated before any is accepted: this call accepts either all
 *       of them or none. Members already accepted (pending, running, or done) are skipped and
 *       never rerun. A build error retained by the group also fails when a downstream task or
 *       group that depends on it is enqueued.
 */
AU_API int au_flow_enqueue_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Wait for the group to complete.
 * @param group Group to wait for.
 * @return 0 if all work succeeds, a propagated task error, or a build/wait error.
 * @note Does not enqueue the group; has the same cooperative wait rules as au_flow_wait_task.
 */
AU_API int au_flow_wait_group(XFlowGroupHandle group) XFLOW_NOEXCEPT;

/**
 * @brief Run [0, range) as a synchronous parallel loop; returns after all tiles finish.
 * @param range Number of elements; zero returns immediately.
 * @param tile Elements per tile; zero means 1. The last tile may be smaller.
 * @param fn Callback for one [begin, end) range; null returns immediately.
 * @param user_data Borrowed data passed to fn; must remain valid until this call returns.
 * @note Each element is processed exactly once. Tiles may be claimed in any order, so a lane
 *       may receive non-adjacent ranges and callbacks may run concurrently.
 * @note The calling thread uses lane 0 and runs tiles itself, so the loop does not depend on a
 *       free worker. Up to 3 pool workers join as helpers. Nested calls run serially on the
 *       calling thread with worker_idx 0; external waiters do not help.
 * @note Workers that become free first help this loop (up to 3 helpers); only then do they take
 *       more queued async tasks. The caller also runs tiles, so the loop completes even if no
 *       worker is free. Once the loop finishes, all workers resume queued async tasks. Because
 *       callbacks must not block, neither side can wait on the other forever.
 * @note fn must not throw (a throw terminates) or wait on unfinished tasks. Concurrent loops
 *       must use separate scratch arrays. Record callback errors in user_data and check them
 *       after this call returns.
 */
AU_API void au_flow_parallelize_tile_ex(size_t range, size_t tile, XFlowTileExFn fn, void* user_data) XFLOW_NOEXCEPT;

/**
 * @brief Wait until accepted tasks and synchronous tile calls have all finished.
 * @return 0 when drained, or a wait error; task failures remain available from their handles.
 * @note Ignores unsubmitted tasks and does not wait for device work. New enqueues may extend the wait.
 *       Calling from a task or tile returns kErrorFlowWouldDeadlock.
 */
AU_API int au_flow_wait_all(void) XFLOW_NOEXCEPT;

/**
 * @brief Request a CPU mask for pool workers; the calling thread is not rebound.
 * @param mask Bit N selects CPU N (CPU6+7 = 0xC0); 0 restores the initial allowed CPU set.
 * @return 0 if accepted, or an argument/platform error; unsupported platforms return kErrorNotSupported.
 * @note The mask affects pool workers only. A worker applies it at its next scheduler entry
 *       (before its next task or tile, or during a cooperative wait), not when this call returns.
 *       A task calling this function keeps its current affinity until then; use the platform
 *       affinity API if the calling thread itself must be rebound immediately.
 *       Requested CPUs must belong to the scheduler's initial allowed set. OS application
 *       failures are logged; set again to retry. Use sys helpers to select CPUs.
 */
AU_API int au_flow_set_affinity_mask(uint64_t mask) XFLOW_NOEXCEPT;

/**
 * @brief Read the requested worker affinity mask for CPUs 0..N.
 * @return Requested mask, or 0 when unavailable.
 * @note Not proof of each worker's actual binding; a zero request also restores allowed CPUs numbered above 63.
 */
AU_API uint64_t au_flow_get_affinity_mask(void) XFLOW_NOEXCEPT;

/// Created pool workers (0..8), including sleepers but excluding callers. Returns 0 on query failure.
AU_API size_t au_flow_get_worker_count(void) XFLOW_NOEXCEPT;

/**
 * @brief Drain accepted work, join all worker threads, and reset the pool to zero workers.
 * @return 0 on success; kErrorFlowWouldDeadlock if called from a task, tile, or worker;
 *         kErrorStateInvalid while the runtime is shutting down; or another error if the drain fails.
 * @note First waits until no accepted tasks or active tile calls remain, then joins all workers.
 *       Unsubmitted tasks are not waited for, and task failures stay on their handles.
 *       External enqueues block during the drain; running tasks/tiles may still enqueue and
 *       extend it. After the drain the pool is empty and the next enqueue or tile call starts
 *       workers lazily. Affinity settings are preserved.
 *       This does not cancel tasks or wait for device work, and it does not shut down the
 *       scheduler. Task handles and results remain valid.
 *       Finish all external API calls before unloading the library.
 */
AU_API int au_flow_release_workers(void) XFLOW_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif
