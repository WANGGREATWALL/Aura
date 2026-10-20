#include "flow/xflow.h"
#include "flow/testing.h"
#include <memory>

namespace {
int ordinary(int& result) { result = 11; return 0; }
struct Module {
    int member(int& result) const { result = 12; return 0; }
};
}
int main(int argc, char**)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    if (argc > 1) {
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, 0);
        au::flow::Task rejected = flow.enqueue([]() -> int { return 0; });
        if (rejected.submissionCode() != au::err::kErrorPlatformAPI) return 4;
        au::flow::TaskResult error{};
        if (rejected.result(error) || error.mDomain != au::flow::ErrorDomain::System) return 5;
    }
    int result = 0;
    au::flow::TaskResult status{};
    au::flow::Task a = flow.enqueue(ordinary, std::ref(result));
    if (flow.wait(a, &status) || status.mCode || result != 11) return 1;
    const Module module{};
    au::flow::Task b = flow.enqueue(&Module::member, std::cref(module), std::ref(result));
    if (flow.wait(b, &status) || status.mCode || result != 12) return 2;
    std::unique_ptr<int> owned(new int(13));
    au::flow::Task c = flow.enqueue([](std::unique_ptr<int> value) -> int { return *value == 13 ? 0 : 3; }, std::move(owned));
    if (flow.wait(c, &status) || status.mCode) return 3;
    if (flow.shutdown()) return 6;
    if (c.result(status) || status.mCode) return 7;
    if (flow.enqueue(ordinary, std::ref(result)).submissionCode() != au::err::kErrorStateInvalid) return 8;
    return 0;
}
