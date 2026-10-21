#include "flow/diagnostics.h"

#include <array>
#include <sstream>
#include <unordered_set>

#include "file/xfile.h"
#include "flow/constants.h"
#include "flow/trace_buffer.h"
#include "log/xerror.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

int writeDot(const au::flow::detail::GraphSnapshot& graph, const char* path) noexcept
{
    if (!path)
        return au::err::kErrorNullPointer;
    try {
        auto quote = [](std::ostream& output, const std::string& name) {
            for (char c : name) {
                if (c == '\"' || c == '\\')
                    output << '\\';
                if (c == '\n' || c == '\r')
                    output << "\\n";
                else
                    output << c;
            }
        };
        std::ostringstream text;
        text << "digraph AuraFlow {\n  graph [label=\"dynamic capture=" << (graph.mCaptured ? "enabled" : "disabled")
             << "; per-node snapshot; dropped=" << graph.mDropped << "\"];\n";
        std::unordered_set<std::uint64_t> nested;
        for (const auto& node : graph.mNodes) {
            text << "  n" << node.mId << " [label=\"";
            quote(text, node.mName);
            text << " #" << node.mId << "\",state=" << static_cast<unsigned int>(node.mState);
            if (node.mGroup)
                text << ",shape=box";
            if (node.mCode)
                text << ",color=red,error=" << node.mCode << ",cause=\"" << node.mCause << '\"';
            text << "];\n";
            for (auto pred : node.mPreds)
                text << "  n" << pred << " -> n" << node.mId << ";\n";
            for (auto member : node.mMembers) {
                text << "  n" << node.mId << " -> n" << member << " [style=dashed,label=\"member\"];\n";
                if (graph.mNodes[graph.mIndex.at(member)].mGroup)
                    nested.insert(member);
            }
            for (auto child : node.mChildren)
                text << "  n" << node.mId << " -> n" << child << " [style=dotted,label=\"structured child\"];\n";
        }
        struct Cluster
        {
            std::size_t mIndex;
            bool        mClose;
        };
        std::vector<Cluster> stack;
        for (std::size_t i = 0; i < graph.mNodes.size(); ++i)
            if (graph.mNodes[i].mGroup && !nested.count(graph.mNodes[i].mId))
                stack.push_back({i, false});
        while (!stack.empty()) {
            const auto event = stack.back();
            stack.pop_back();
            if (event.mClose) {
                text << "  }\n";
                continue;
            }
            const auto& node = graph.mNodes[event.mIndex];
            text << "  subgraph cluster_" << node.mId << " { label=\"";
            quote(text, node.mName);
            text << "\";\n";
            text << "    n" << node.mId << ";\n";
            stack.push_back({event.mIndex, true});
            for (auto member : node.mMembers) {
                const auto index = graph.mIndex.at(member);
                if (graph.mNodes[index].mGroup)
                    stack.push_back({index, false});
                else
                    text << "    n" << member << ";\n";
            }
        }
        text << "}\n";
        return au::file::write(text.str(), path);
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}

#ifndef AURA_FLOW_TRACE_ENABLED
#define AURA_FLOW_TRACE_ENABLED 1
#endif
#ifndef AURA_FLOW_TRACE_CAPACITY
#define AURA_FLOW_TRACE_CAPACITY 1024
#endif
std::size_t configuredTraceCapacity() noexcept { return AURA_FLOW_TRACE_CAPACITY; }
struct Diagnostics::Storage
{
#if AURA_FLOW_TRACE_ENABLED
    std::array<TraceBuffer, kMaxWorkers + 1> mBuffers;
#endif
};
Diagnostics::Diagnostics() noexcept  = default;
Diagnostics::~Diagnostics() noexcept = default;
int Diagnostics::setEnabled(bool enabled) noexcept
{
#if AURA_FLOW_TRACE_ENABLED
    try {
        std::lock_guard<std::mutex> lock(mMutex);
        if (enabled && !std::atomic_load_explicit(&mStorage, std::memory_order_acquire))
            std::atomic_store_explicit(&mStorage, std::make_shared<Storage>(), std::memory_order_release);
        mEnabled.store(enabled, std::memory_order_release);
        return 0;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
#else
    return enabled ? au::err::kErrorNotSupported : 0;
#endif
}
int Diagnostics::reset() noexcept
{
    std::shared_ptr<Storage> retired;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mEnabled.load(std::memory_order_acquire))
            return au::err::kErrorStateInvalid;
        retired = std::atomic_exchange_explicit(&mStorage, std::shared_ptr<Storage>(), std::memory_order_acq_rel);
    }
    return 0;  // Retired readers/writers pin their own old capture; destructor is outside control lock.
}
std::size_t Diagnostics::retainedBytes() const noexcept
{
#if AURA_FLOW_TRACE_ENABLED
    return std::atomic_load_explicit(&mStorage, std::memory_order_acquire)
               ? sizeof(Storage) + (kMaxWorkers + 1) * configuredTraceCapacity() * sizeof(TraceSlot)
               : 0;
#else
    return 0;
#endif
}
void Diagnostics::record(std::size_t buffer, std::uint64_t task, std::uint64_t thread, const char* name, char phase,
                         int code, std::uint64_t parent, const char* label, std::size_t labelSize,
                         const ExternalProfile* profile) noexcept
{
#if AURA_FLOW_TRACE_ENABLED
    if (!enabled())
        return;
    const auto storage = std::atomic_load_explicit(&mStorage, std::memory_order_acquire);
    if (storage && buffer < storage->mBuffers.size())
        storage->mBuffers[buffer].record(task, thread, name, phase, code, profile, parent, label, labelSize);
#else
    (void)buffer;
    (void)task;
    (void)thread;
    (void)name;
    (void)phase;
    (void)code;
    (void)parent;
    (void)label;
    (void)labelSize;
    (void)profile;
#endif
}

int Diagnostics::dumpTrace(const char* path) const noexcept
{
    if (!path)
        return au::err::kErrorNullPointer;
#if !AURA_FLOW_TRACE_ENABLED
    return au::err::kErrorNotSupported;
#else
    try {
        std::ostringstream out;
        auto               jsonString = [](std::ostream& stream, const char* value) {
            stream << '\"';
            for (const auto* c = reinterpret_cast<const unsigned char*>(value); *c; ++c) {
                if (*c == '\"' || *c == '\\')
                    stream << '\\' << static_cast<char>(*c);
                else if (*c < 32) {
                    const char hex[] = "0123456789abcdef";
                    stream << "\\u00" << hex[*c >> 4] << hex[*c & 15];
                } else
                    stream << static_cast<char>(*c);
            }
            stream << '\"';
        };
        out << "{\"traceEvents\":[";
        bool        first   = true;
        std::size_t dropped = 0;
        const auto  storage = std::atomic_load_explicit(&mStorage, std::memory_order_acquire);
        if (storage)
            for (const auto& buffer : storage->mBuffers) {
                dropped += buffer.dropped();
                for (std::size_t i = 0; i < buffer.capacity(); ++i) {
                    const auto& slot = buffer.slots()[i];
                    if (!slot.mPublished.load(std::memory_order_acquire))
                        continue;  // Safe concurrent export.
                    if (!first)
                        out << ',';
                    first = false;
                    out << "{\"name\":\"" << slot.mName << "\",\"cat\":\"au.flow\",\"ph\":\"" << slot.mPhase
                        << "\",\"pid\":1,\"tid\":" << slot.mThread << ",\"ts\":" << slot.mTime;
                    if (slot.mPhase == 'b' || slot.mPhase == 'e')
                        out << ",\"id\":\"" << slot.mTask << '"';
                    if (slot.mPhase == 'i')
                        out << ",\"s\":\"t\"";
                    out << ",\"args\":{\"task\":\"" << slot.mTask << "\",\"code\":" << slot.mCode;
                    out << ",\"parent\":\"" << slot.mParent << "\",\"taskName\":";
                    jsonString(out, slot.mLabel);
                    out << ",\"taskNameTruncated\":" << (slot.mLabelTruncated ? "true" : "false");
                    if (slot.mHasProfile) {
                        const auto& profile = slot.mProfile;
                        out << ",\"profileAvailable\":" << (profile.mAvailable ? "true" : "false")
                            << ",\"profileNativeCode\":" << profile.mNativeCode
                            << ",\"clockDomain\":\"opencl-device\",\"hostSubmitUs\":" << profile.mHostSubmitUs
                            << ",\"hostObservedUs\":" << profile.mHostObservedUs;
                        if (profile.mAvailable)
                            out << ",\"gpuQueuedNs\":" << profile.mQueuedNs << ",\"gpuSubmitNs\":" << profile.mSubmitNs
                                << ",\"gpuStartNs\":" << profile.mStartNs << ",\"gpuEndNs\":" << profile.mEndNs
                                << ",\"gpuDurationNs\":" << profile.mEndNs - profile.mStartNs;
                    }
                    out << "}}";
                }
            }
        out << "],\"displayTimeUnit\":\"ms\",\"auraDroppedRecords\":" << dropped << "}\n";
        return au::file::write(out.str(), path);
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
#endif
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
