#ifndef AURA_GPU_PENDING_H_
#define AURA_GPU_PENDING_H_

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include "mm/memory_control.h"

namespace au {
namespace gpu AU_LOCAL {
namespace detail {
// Provider-local completion data; no scheduler task/token or implicit join.
enum class ErrorDomain { None, OpenCL, Memory, System };
struct PendingStatus {
    int mCode = 0;
    int mNativeCode = 0;
    au::gpu::detail::ErrorDomain mDomain = au::gpu::detail::ErrorDomain::None;
    std::uint64_t mCauseTaskId = 0;
};
bool pendingCleanupActive() noexcept;
class Pending;
class PreparedSubmission;
// Native providers are isolated behind these private operations so the lifetime
// protocol can also be exercised without an installed OpenCL device.
struct PendingOps {
    int (*mQuery)(void*, bool*, au::gpu::detail::PendingStatus*) noexcept = nullptr;
    au::gpu::detail::PendingStatus (*mFinalize)(void*, const au::gpu::detail::PendingStatus&) noexcept = nullptr;
    void (*mDestroy)(void*) noexcept = nullptr;
};
class PendingRegistry final {
public:
    PendingRegistry() = default;
    ~PendingRegistry() noexcept;
    PendingRegistry(const au::gpu::detail::PendingRegistry&) = delete;
    au::gpu::detail::PendingRegistry& operator=(const au::gpu::detail::PendingRegistry&) = delete;
    PendingRegistry(au::gpu::detail::PendingRegistry&&) = delete;
    au::gpu::detail::PendingRegistry& operator=(au::gpu::detail::PendingRegistry&&) = delete;
    void pollSome(std::size_t limit = 8) noexcept;
    au::gpu::detail::PendingStatus drain() noexcept;
    std::size_t size() const noexcept;
    bool finalizingHere() const noexcept;
private:
    friend class au::gpu::detail::Pending;
    friend class au::gpu::detail::PreparedSubmission;
    void add(const std::shared_ptr<au::gpu::detail::Pending>& pending) noexcept;
    void retire(au::gpu::detail::Pending* pending) noexcept;
    // Mutated by concurrent worker queries and normal wrapper entries.
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    bool mDraining = false;
    std::uint64_t mRevision = 0;
    au::gpu::detail::Pending* mHead = nullptr;
    au::gpu::detail::Pending* mTail = nullptr;
    std::size_t mCount = 0;
    au::gpu::detail::PendingStatus mFailure;
};

// Stack preparation guard. Every fallible allocation is
// obtained BEFORE the first native enqueue. Destruction cancels only an
// unpublished record, never a command already accepted by the device.
class PreparedSubmission final {
public:
    PreparedSubmission() noexcept = default;
    ~PreparedSubmission() noexcept;
    PreparedSubmission(const au::gpu::detail::PreparedSubmission&) = delete;
    au::gpu::detail::PreparedSubmission& operator=(const au::gpu::detail::PreparedSubmission&) = delete;
    PreparedSubmission(au::gpu::detail::PreparedSubmission&&) = delete;
    au::gpu::detail::PreparedSubmission& operator=(au::gpu::detail::PreparedSubmission&&) = delete;
    // On success ownership of payload transfers, including standalone use.
    // On failure ownership stays with the caller.
    int prepare(au::gpu::detail::PendingRegistry& registry,
                const au::gpu::detail::PendingOps& ops, void* payload,
                std::uintptr_t context, bool hostCompletion = true) noexcept;
    const std::shared_ptr<au::mm::detail::DeviceFence>& fence() const noexcept { return mFence; }
    // Publish only after committing mm fences and storing the last valid native
    // event. status may describe a flush/partial-submission failure; it does not
    // discard the command. No allocation or native call occurs in publish().
    void publish(const au::gpu::detail::PendingStatus& status = {}) noexcept;
    void cancel(const au::gpu::detail::PendingStatus& status) noexcept;
private:
    // Worker, fence waiter and registry can race to finalize the same record.
    std::shared_ptr<au::gpu::detail::Pending> mPending;
    std::shared_ptr<au::mm::detail::DeviceFence> mFence;
    bool mPublished = false;
};
} // namespace detail
} // namespace gpu
} // namespace au
#endif
