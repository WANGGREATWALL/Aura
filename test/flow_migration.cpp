// C++11 production consumer. These complete examples are referenced by the migration guide.
#include <array>
#include <atomic>
#include <functional>
#include <utility>

#include "cv/ximage_access.h"
#include "flow/xthread_flow.h"  // Historical include remains supported.

namespace {
int dependencyExample()
{
    auto&     flow   = au::flow::XFlow::get();
    int       source = 0, output = 0;
    auto      before = flow.enqueue([&]() -> int {
        source = 7;
        return 0;
    });
    auto      after  = flow.enqueueAfter(before, [&]() -> int {
        output = source * 2;
        return 0;
    });
    const int code   = after.wait();
    return code ? code : (output == 14 ? 0 : 1);
}

int structuredExample()
{
    auto&     flow   = au::flow::XFlow::get();
    int       result = 0;  // External owner remains alive through parent.wait().
    auto      parent = flow.enqueue([&]() -> int {
        (void)flow.enqueue([&]() -> int {
            result = 19;
            return 0;
        });
        return 0;  // May return before child body; parent is not yet complete.
    });
    const int code   = parent.wait();  // Includes discarded child's completion.
    return code ? code : (result == 19 ? 0 : 2);
}

int borrowedImageExample()
{
    auto&          flow = au::flow::XFlow::get();
    au::cv::XImage owner(8, 4, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    if (!owner.isValid())
        return au::err::kErrorNoMemory;
    // Copy is a view, not shared ownership. Keep owner alive until wait returns.
    auto task = flow.enqueue(
        [](au::cv::XImage borrowed) -> int {
            au::mm::CpuAccessScope access;
            const int              code = access.begin({au::cv::readAccess(borrowed)});
            return code ? code : access.finish();
        },
        owner);
    return task.wait();
}

int owningImageExample()
{
    auto&          flow = au::flow::XFlow::get();
    au::cv::XImage owner(8, 4, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    if (!owner.isValid())
        return au::err::kErrorNoMemory;
    // Pass as a moved argument: C++11 needs no C++14 lambda init-capture.
    auto task = flow.enqueue(
        [](au::cv::XImage image) -> int {
            if (!image.ownsMemory())
                return au::err::kErrorStateInvalid;
            au::mm::CpuAccessScope access;
            const int              code = access.begin({au::cv::writeAccess(image)});
            if (code)
                return code;
            image.view().data[0][0] = 42;
            return access.finish();
        },
        std::move(owner));
    if (owner.isValid())
        return 3;
    return task.wait();
}

struct Tiles
{
    std::array<std::atomic<unsigned int>, 31> mVisits{};
    std::atomic<bool>                         mBad{false};
    Tiles()
    {
        for (auto& value : mVisits)
            value.store(0, std::memory_order_relaxed);
    }
    Tiles(const Tiles&)            = delete;
    Tiles& operator=(const Tiles&) = delete;
    int    operator()(std::size_t offset, std::size_t count) &
    {
        if (offset >= 31 || offset % 3 || count != (offset == 30 ? 1u : 3u)) {
            mBad.store(true, std::memory_order_relaxed);
            return 4;
        }
        for (std::size_t i = offset; i < offset + count; ++i)
            mVisits[i].fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    bool correct() const
    {
        if (mBad.load(std::memory_order_relaxed))
            return false;
        for (const auto& value : mVisits)
            if (value.load(std::memory_order_relaxed) != 1)
                return false;
        return true;
    }
};
int ordinaryTile(std::size_t, std::size_t count) { return count ? 0 : 5; }

int tiledCompatibilityExample()
{
    auto& flow = au::flow::XFlow::get();
    Tiles current, legacy;
    if (flow.parallelFor(31, 3, current) || !current.correct())
        return 6;
    if (flow.parallelizeTiledTasks(31, 3, legacy) || !legacy.correct())
        return 7;
    std::array<int, 31> byReference{}, byValue{};
    int code = XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(31, 3) for (std::size_t i = _i; i < _i + _ti; ++i) byReference[i] =
        1;
    XTHREAD_PARALLELIZE_END
    if (code)
        return code;
    int* data = byValue.data();
    code = XTHREAD_PARALLELIZE_TILED_TASK_ASSIGN(31, 3) for (std::size_t i = _i; i < _i + _ti; ++i) data[i] = 2;
    XTHREAD_PARALLELIZE_END
    if (code)
        return code;
    for (std::size_t i = 0; i < byValue.size(); ++i)
        if (byReference[i] != 1 || byValue[i] != 2)
            return 8;
    // void/int, ordinary function, zero/invalid ranges, exact failure code.
    if (flow.parallelFor(31, 3, ordinaryTile))
        return 9;
    if (flow.parallelFor(0, 3, [](std::size_t, std::size_t) -> int { return 1; }))
        return 10;
    if (flow.parallelFor(0, 0, ordinaryTile) != au::err::kErrorInvalidParam)
        return 11;
    if (flow.parallelFor(1, 1, [](std::size_t, std::size_t) -> int { return 17; }) != 17)
        return 12;
    code = flow.parallelFor(1, 1, [&](std::size_t, std::size_t) {
        // Rejected publication still fails the synchronous loop when ignored.
        (void)flow.enqueue([]() -> int { return 0; });
    });
    return code == au::err::kErrorStateInvalid ? 0 : 13;
}
}  // namespace

int main()
{
    const int results[] = {dependencyExample(), structuredExample(), borrowedImageExample(), owningImageExample(),
                           tiledCompatibilityExample()};
    for (int code : results)
        if (code)
            return code;
    return au::flow::XFlow::get().shutdown();
}
