#ifndef AURA_TEST_HELPERS_H_
#define AURA_TEST_HELPERS_H_

#include <functional>
#include <string>
#include "gtest/gtest.h"

namespace aura {
namespace test {

inline std::string captureStdout(const std::function<void()>& fn)
{
    testing::internal::CaptureStdout();
    fn();
    std::fflush(stdout);
    return testing::internal::GetCapturedStdout();
}

inline bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

inline int countOccurrences(const std::string& s, const std::string& sub)
{
    int         cnt = 0;
    std::size_t pos = s.find(sub);
    while (pos != std::string::npos) {
        ++cnt;
        pos = s.find(sub, pos + sub.size());
    }
    return cnt;
}

}  // namespace test
}  // namespace aura

#endif  // AURA_TEST_HELPERS_H_
