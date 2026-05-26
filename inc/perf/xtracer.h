#ifndef AURA_PERF_XTRACER_H_
#define AURA_PERF_XTRACER_H_

#include <string>

#include "perf/xperf_types.h"

namespace au {
namespace perf {

void traceBegin(PerfScope* scope, const std::string& name) noexcept;
void traceSubBegin(PerfScope* scope, const std::string& name) noexcept;
void traceSubEnd(PerfScope* scope) noexcept;
void traceEnd(PerfScope* scope) noexcept;

class XTracerScoped
{
public:
    explicit XTracerScoped(const std::string& name) noexcept { au::perf::traceBegin(&mScope, name); }
    ~XTracerScoped() noexcept { au::perf::traceEnd(&mScope); }

    XTracerScoped(const XTracerScoped&)            = delete;
    XTracerScoped& operator=(const XTracerScoped&) = delete;
    XTracerScoped(XTracerScoped&&)                 = delete;
    XTracerScoped& operator=(XTracerScoped&&)      = delete;

    void sub(const std::string& name) noexcept { au::perf::traceSubBegin(&mScope, name); }
    void sub() noexcept { au::perf::traceSubEnd(&mScope); }

private:
    PerfScope mScope;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTRACER_H_
