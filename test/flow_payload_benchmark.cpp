#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

#include "flow/xflow.h"
#include "perf/xtimer.h"

namespace {
template <std::size_t N>
int measure()
{
    std::array<unsigned char, N> bytes{};
    bytes[0]        = 3;
    bytes[N / 2]    = 7;
    bytes[N - 1]    = 11;
    const auto body = [bytes]() -> int { return bytes[0] + bytes[N / 2] + bytes[N - 1] == 21 ? 0 : 1; };
    auto&      flow = au::flow::XFlow::get();
    for (int i = 0; i < 16; ++i)
        if (flow.enqueue(body).wait())
            return 1;
    std::array<std::uint64_t, 101> samples{};
    for (auto& sample : samples) {
        const auto start = std::chrono::steady_clock::now();
        const int  code  = flow.enqueue(body).wait();
        sample           = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        if (code)
            return code;
    }
    std::sort(samples.begin(), samples.end());
    std::printf("case=closure capture_bytes=%zu samples=101 p50_ns=%llu p95_ns=%llu\n", N,
                static_cast<unsigned long long>(samples[50]), static_cast<unsigned long long>(samples[95]));
    return 0;
}
}  // namespace
int main()
{
    au::perf::Config::get().setEnabled(false);
    std::printf(
        "scope=host_async_closure_creation_submit_wait diagnostics=removed "
        "cpp_allocation_counts_measured_separately\n");
    if (measure<32>() || measure<128>() || measure<768>() || measure<3072>())
        return 1;
    return au::flow::XFlow::get().shutdown();
}
