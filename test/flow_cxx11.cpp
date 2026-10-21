#include <atomic>
#include <memory>
#include <string>

#include "flow/xthread_flow.h"
struct Member
{
    int body(int& v) const
    {
        v = 17;
        return 0;
    }
};
int main()
{
    au::flow::Flow& flow  = au::flow::Flow::get();
    int             value = 0;
    Member          member;
    au::flow::Task  a = flow.createNamed("member", &Member::body, &member, std::ref(value));
    au::flow::Task  b = flow.create([] {});
    if ((a >> b).code())
        return 1;
    au::flow::Tasks group("C++11");
    if (group.add(a, b) || flow.enqueue(group) || group.wait() || value != 17)
        return 2;
    au::flow::Task tail = flow.enqueueAfter(group, []() -> int { return 23; });
    if (tail.wait() != 23 || !group.dumpDot())
        return 3;
    if (flow.waitAll() != 23)
        return 6;
    std::unique_ptr<int> p(new int(11));
    if (flow.enqueue([](std::unique_ptr<int> v) -> int { return *v == 11 ? 0 : 1; }, std::move(p)).wait())
        return 4;
    std::atomic<size_t> count(0);
    flow.parallelizeTiledTasks(31, 4, [&](size_t b, size_t e) { count.fetch_add(e - b, std::memory_order_relaxed); });
    if (count.load(std::memory_order_relaxed) != 31)
        return 5;
    return flow.waitAll();
}
