#include "file/xfile.h"
#include "flow/xflow.h"

int main()
{
    auto&     flow    = au::flow::XFlow::get();
    const int enabled = flow.setTracing(true);
#if AURA_EXPECT_TRACE
    if (enabled || flow.resetTrace() != au::err::kErrorStateInvalid)
        return 1;
#else
    if (enabled != au::err::kErrorNotSupported || flow.dumpTrace("flow-optional.json") != au::err::kErrorNotSupported)
        return 2;
#endif
    if (flow.setTracing(false) || flow.resetTrace() || flow.setGraphCapture(true))
        return 3;
    auto parent = flow.enqueue([&]() -> int {
        auto      child = flow.makeTask([]() -> int { return 0; });
        const int named = child.setName("DOT-only child");
        return named ? named : flow.enqueue(child).wait();
    });
    if (parent.wait() || flow.dumpDot(parent, "flow-optional.dot"))
        return 4;
    std::string dot;
    if (au::file::read("flow-optional.dot", dot) || dot.find("DOT-only child") == std::string::npos)
        return 5;
    if (au::file::removeFile("flow-optional.dot") || flow.setGraphCapture(false))
        return 6;
    return flow.shutdown();
}
