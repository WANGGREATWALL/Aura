#include <cstdio>
#include <memory>

#include "file/xfile.h"
#include "flow/diagnostics.h"
#include "flow/node.h"
#include "flow/trace_buffer.h"
#include "flow/xflow.h"
#include "perf/xtimer.h"

namespace {
std::size_t pss()
{
#if defined(__linux__)
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen("/proc/self/smaps_rollup", "r"), &std::fclose);
    if (file) {
        char          line[128];
        unsigned long kb = 0;
        while (std::fgets(line, sizeof(line), file.get()))
            if (std::sscanf(line, "Pss: %lu kB", &kb) == 1)
                return kb;
    }
#endif
    return 0;
}
}  // namespace
int main()
{
    au::perf::Config::get().setEnabled(false);
    std::printf("scope=host_diagnostics node_bytes=%zu trace_slot_bytes=%zu process_pss_kb=%zu\n",
                sizeof(au::flow::detail::Node), sizeof(au::flow::detail::TraceSlot), pss());
    {
        au::flow::detail::Diagnostics trace;
        const auto                    initial = pss();
        const int                     code    = trace.setEnabled(true);
        if (code != 0 && code != au::err::kErrorNotSupported)
            return 1;
        const auto enabled = pss();
        (void)trace.setEnabled(false);
#ifdef AURA_B6_DIAGNOSTICS
        const auto retained = trace.retainedBytes();
        if (trace.reset())
            return 2;
        std::printf(
            "capture=timeline enabled_code=%d pss_before_kb=%zu pss_enabled_kb=%zu retained_bytes_after_disable=%zu "
            "retained_bytes_after_reset=%zu pss_after_reset_kb=%zu capacity_per_lane=%zu\n",
            code, initial, enabled, retained, trace.retainedBytes(), pss(),
            au::flow::detail::configuredTraceCapacity());
#else
        std::printf(
            "capture=timeline enabled_code=%d pss_before_kb=%zu pss_enabled_kb=%zu pss_disabled_kb=%zu "
            "reset=unavailable capacity_per_lane=8192\n",
            code, initial, enabled, pss());
#endif
    }
    auto& flow = au::flow::XFlow::get();
#ifdef AURA_B6_DIAGNOSTICS
    if (flow.setGraphCapture(true))
        return 3;
#else
    if (flow.setTracing(true))
        return 3;
#endif
    const auto     before = pss();
    au_flow_handle first  = nullptr;
    auto           parent = flow.enqueue([&]() -> int {
        for (unsigned int i = 0; i < 512; ++i) {
            au_flow_handle child = nullptr;
            int            code  = au_flow_make([](void*) -> int { return 0; }, nullptr, nullptr, &child);
            if (code)
                return code;
            (void)au_flow_name(child, "history-child");
            code = au_flow_submit(child);
            if (!code)
                code = au_flow_wait(child, UINT64_MAX, nullptr);
            if (!i)
                first = child;
            else
                au_flow_release(child);
            if (code)
                return code;
        }
        return 0;
    });
    if (parent.wait() || !first)
        return 4;
    const auto refs = static_cast<au::flow::detail::Node*>(first)->mRefs.load(std::memory_order_acquire);
    au_flow_release(first);
    if (flow.dumpDot(parent, "/tmp/flow-diagnostic-benchmark.dot"))
        return 5;
    std::printf(
        "capture=dynamic_dot children=512 held_child_refs_after_parent=%zu pss_before_kb=%zu pss_after_kb=%zu\n", refs,
        before, pss());
    parent = {};
    (void)au::file::removeFile("/tmp/flow-diagnostic-benchmark.dot");
    (void)flow.setTracing(false);
    return flow.shutdown();
}
