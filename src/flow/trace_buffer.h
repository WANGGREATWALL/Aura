#ifndef AURA_FLOW_TRACE_BUFFER_H_
#define AURA_FLOW_TRACE_BUFFER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "flow/completion.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

struct TraceSlot {
    std::atomic<bool> mPublished{false};
    std::uint64_t mTime = 0;
    std::uint64_t mTask = 0;
    std::uint64_t mThread = 0;
    const char* mName = nullptr; // Static literal only, no task-owned strings.
    char mPhase = 'i';
    int mCode = 0;
    std::uint64_t mParent = 0;
    char mLabel[64]{};
    bool mLabelTruncated = false;
    bool mHasProfile = false;
    au::flow::detail::ExternalProfile mProfile;
};
class TraceBuffer {
public:
    void record(std::uint64_t task, std::uint64_t thread, const char* name, char phase, int code,
                const au::flow::detail::ExternalProfile* profile = nullptr, std::uint64_t parent = 0,
                const char* label = nullptr, std::size_t labelSize = 0) noexcept
    {
        std::size_t index = mNext.load(std::memory_order_relaxed); // Slot reservation only.
        do {
            if (index >= mSlots.size()) {
                mDropped.fetch_add(1, std::memory_order_relaxed); // Diagnostic count, not a scheduling predicate.
                return;
            }
        } while (!mNext.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)); // Bounded, never wrap into live slots.
        au::flow::detail::TraceSlot& slot = mSlots[index];
        slot.mTime = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        slot.mTask = task; slot.mThread = thread; slot.mName = name; slot.mPhase = phase; slot.mCode = code;
        slot.mParent = parent;
        if (label && labelSize) {
            auto bytes = std::min(labelSize, sizeof(slot.mLabel) - 1);
            // Avoid cutting a valid UTF-8 code point at the fixed byte limit.
            if (bytes < labelSize) while (bytes && (static_cast<unsigned char>(label[bytes]) & 0xc0) == 0x80) --bytes;
            std::memcpy(slot.mLabel, label, bytes); slot.mLabel[bytes] = '\0'; slot.mLabelTruncated = bytes < labelSize;
        }
        if (profile) { slot.mHasProfile = true; slot.mProfile = *profile; }
        slot.mPublished.store(true, std::memory_order_release); // Export reads complete immutable records only.
    }
    const std::array<au::flow::detail::TraceSlot, 8192>& slots() const noexcept { return mSlots; }
    std::size_t dropped() const noexcept { return mDropped.load(std::memory_order_relaxed); }
private:
    std::array<au::flow::detail::TraceSlot, 8192> mSlots;
    std::atomic<std::size_t> mNext{0};
    std::atomic<std::size_t> mDropped{0};
};

} // namespace detail
} // namespace flow
} // namespace au
#endif
