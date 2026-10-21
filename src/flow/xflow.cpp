#include "flow/xflow.h"

#include "flow/core.h"

au::flow::XFlow& au::flow::XFlow::get() noexcept
{
    static au::flow::XFlow facade;
    return facade;
}
const char* au::flow::getErrorStr(int code) noexcept
{
    switch (code) {
        case au::flow::kErrorDependencyFailed: return "ErrorDependencyFailed";
        case au::flow::kErrorCycleDetected: return "ErrorCycleDetected";
        case au::flow::kErrorWouldDeadlock: return "ErrorWouldDeadlock";
        case au::flow::kErrorUnhandledException: return "ErrorUnhandledException";
        default: return au::err::getErrorStr(code);
    }
}
int au::flow::XFlow::getWorkers(au::flow::WorkerInfo* out, std::size_t capacity, std::size_t& count) const noexcept
{
    if (capacity && !out)
        return au::err::kErrorInvalidParam;
    std::array<au_flow_worker, au::flow::detail::kMaxWorkers> values{};
    for (auto& value : values)
        value.mStructSize = sizeof(value);
    const int ret =
        au_flow_workers(values.data(), std::min<std::size_t>(capacity, au::flow::detail::kMaxWorkers), &count);
    for (std::size_t i = 0; i < std::min(capacity, count); ++i)
        out[i] = {sizeof(au::flow::WorkerInfo), values[i].mWorkerId,       values[i].mThreadId,
                  values[i].mRequestedVersion,  values[i].mAppliedVersion, values[i].mAffinityCode};
    return ret;
}

uint32_t au_flow_version() noexcept { return AURA_FLOW_ABI_VERSION; }
int      au_flow_payload_allocate(std::size_t size, std::size_t alignment, void** out) noexcept
{
    if (!out)
        return au::err::kErrorNullPointer;
    *out = nullptr;
    if (!size || !alignment || (alignment & (alignment - 1)))
        return au::err::kErrorInvalidParam;
    if (size > SIZE_MAX - alignment)
        return au::err::kErrorOverflow;
    try {
#ifdef AURA_FLOW_TESTING
        if (au::flow::detail::fail(au::flow::testing::Failure::Payload))
            return au::err::kErrorNoMemory;
#endif
        *out = au::flow::detail::core().payloads().allocate(size, alignment);
        return 0;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}
void au_flow_payload_release(void* payload, std::size_t size, std::size_t alignment) noexcept
{
    if (payload)
        au::flow::detail::core().payloads().release(payload, size, alignment);
}
int au_flow_make(au_flow_invoke invoke, au_flow_destroy destroy, void* payload, au_flow_handle* out) noexcept
{
    au::flow::detail::Closure owner(invoke, destroy, payload);
    if (out)
        *out = nullptr;
    if (!out || !invoke)
        return au::err::kErrorInvalidParam;
    try {
        auto node      = au::flow::detail::core().make(au::flow::detail::Kind::Work);
        node->mClosure = std::move(owner);
        *out           = node.release();
        return 0;
    } catch (const au::flow::detail::CreateFailure& error) {
        return error.mCode;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}
int au_flow_group(au_flow_handle* out) noexcept
{
    if (!out)
        return au::err::kErrorNullPointer;
    *out = nullptr;
    try {
        auto node    = au::flow::detail::core().make(au::flow::detail::Kind::Group);
        node->mEntry = au::flow::detail::core().make(au::flow::detail::Kind::Entry);
        *out         = node.release();
        return 0;
    } catch (const au::flow::detail::CreateFailure& error) {
        return error.mCode;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}
void au_flow_retain(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (node && au::flow::detail::core().owns(node))
        au::flow::detail::retain(node);
}
void au_flow_release(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (node && au::flow::detail::core().owns(node))
        au::flow::detail::release(node);
}
uint64_t au_flow_id(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return au::flow::detail::core().owns(node) ? node->mId : 0;
}
uint32_t au_flow_state(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return static_cast<uint32_t>(au::flow::detail::core().owns(node) ? node->mState.load(std::memory_order_acquire)
                                                                     : au::flow::TaskState::Invalid);
}
int au_flow_submission(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return au::flow::detail::core().owns(node) ? node->mSubmission.load(std::memory_order_acquire)
                                               : au::err::kErrorInvalidHandle;
}
int au_flow_result_get(au_flow_handle handle, au_flow_result* out) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (!au::flow::detail::core().owns(node))
        return au::err::kErrorInvalidHandle;
    if (!out)
        return au::err::kErrorNullPointer;
    if (out->mStructSize != sizeof(*out))
        return au::err::kErrorVersionMismatch;
    if (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire)))
        return au::err::kErrorNotReady;
    *out = node->mResult;
    return 0;
}
int au_flow_precede(au_flow_handle before, au_flow_handle after) noexcept
{
    return au::flow::detail::core().precede(static_cast<au::flow::detail::Node*>(before),
                                            static_cast<au::flow::detail::Node*>(after));
}
int au_flow_builder_error(au_flow_handle handle, int code) noexcept
{
    return au::flow::detail::core().builderError(static_cast<au::flow::detail::Node*>(handle), code);
}
int au_flow_add(au_flow_handle group, au_flow_handle member) noexcept
{
    return au::flow::detail::core().add(static_cast<au::flow::detail::Node*>(group),
                                        static_cast<au::flow::detail::Node*>(member));
}
void au_flow_abandon(au_flow_handle group) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(group);
    if (au::flow::detail::core().owns(node) &&
        node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft)
        au::flow::detail::core().reject(node, au::err::kErrorAborted);
}
int au_flow_submit(au_flow_handle handle) noexcept
{
    return au::flow::detail::core().submit(static_cast<au::flow::detail::Node*>(handle));
}
void au_flow_record_error(int code) noexcept
{
    au::flow::detail::core().scopedError(
        code, code == au::err::kErrorNoMemory ? au::flow::ErrorDomain::Memory : au::flow::ErrorDomain::Flow);
}
void au_flow_record_exception() noexcept
{
    if (au::flow::detail::gTileContext) {
        au::flow::detail::gTileContext->mException = true;
        au::flow::detail::core().scopedError(au::flow::kErrorUnhandledException, au::flow::ErrorDomain::Flow);
        return;
    }
    if (au::flow::detail::gFrame) {
        au::flow::detail::Node* node = au::flow::detail::gFrame->mNode;
        node->mErrors.exception();
    }
}
int au_flow_wait(au_flow_handle handle, uint64_t timeout, au_flow_result* out) noexcept
{
    return au::flow::detail::core().wait(static_cast<au::flow::detail::Node*>(handle), timeout, out);
}
int au_flow_wait_all() noexcept { return au::flow::detail::core().waitAll(); }
int au_flow_tiled(size_t range, size_t tile, au_flow_tile invoke, void* payload) noexcept
{
    return au::flow::detail::core().tiled(range, tile, invoke, payload);
}
int au_flow_affinity(uint32_t mode, const uint32_t* cpus, size_t count) noexcept
{
    return au::flow::detail::core().affinity(mode, cpus, count);
}
int au_flow_workers(au_flow_worker* out, size_t capacity, size_t* count) noexcept
{
    return au::flow::detail::core().workers(out, capacity, count);
}
int au_flow_worker_affinity(uint32_t id, uint32_t* cpus, size_t capacity, size_t* count) noexcept
{
    return au::flow::detail::core().workerAffinity(id, cpus, capacity, count);
}
int au_flow_shutdown() noexcept { return au::flow::detail::core().shutdown(); }
