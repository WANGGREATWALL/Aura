#if ENABLE_TEST_XERROR

#include <algorithm>
#include <climits>
#include <cstddef>
#include <vector>

#include "gtest/gtest.h"
#include "log/xerror.h"

// ============================================================================
// isError
// ============================================================================

TEST(XError, isError_success_returns_false) { EXPECT_FALSE(au::err::isError(au::err::kSuccess)); }

TEST(XError, isError_all_error_codes_return_true)
{
#define XX_CHECK_IS_ERROR(name, val, str, desc)                                                \
    if (au::err::name < 0) {                                                                   \
        EXPECT_TRUE(au::err::isError(au::err::name)) << "Expected isError(" #name ") == true"; \
    }
    XERROR_MAP(XX_CHECK_IS_ERROR)
#undef XX_CHECK_IS_ERROR
}

TEST(XError, isError_positive_values_return_false)
{
    EXPECT_FALSE(au::err::isError(1));
    EXPECT_FALSE(au::err::isError(100));
    EXPECT_FALSE(au::err::isError(INT_MAX));
}

TEST(XError, isError_unknown_negative_values_return_true)
{
    EXPECT_TRUE(au::err::isError(-1));
    EXPECT_TRUE(au::err::isError(-999));
    EXPECT_TRUE(au::err::isError(-9999));
    EXPECT_TRUE(au::err::isError(INT_MIN));
}

// ============================================================================
// getErrorStr
// ============================================================================

TEST(XError, getErrorStr_all_known_codes)
{
#define XX_CHECK_STR(name, val, str, desc) \
    EXPECT_STREQ(au::err::getErrorStr(au::err::name), str) << "Mismatch for " #name;
    XERROR_MAP(XX_CHECK_STR)
#undef XX_CHECK_STR
}

TEST(XError, getErrorStr_unknown_code_returns_default)
{
    EXPECT_STREQ(au::err::getErrorStr(-1), "UnknownErrorCode");
    EXPECT_STREQ(au::err::getErrorStr(1), "UnknownErrorCode");
    EXPECT_STREQ(au::err::getErrorStr(-5000), "UnknownErrorCode");
    EXPECT_STREQ(au::err::getErrorStr(9999), "UnknownErrorCode");
    EXPECT_STREQ(au::err::getErrorStr(INT_MAX), "UnknownErrorCode");
    EXPECT_STREQ(au::err::getErrorStr(INT_MIN), "UnknownErrorCode");
}

// ============================================================================
// Code values
// ============================================================================

TEST(XError, code_values_unique)
{
    // Verify no two error codes share the same numeric value (excluding success)
    std::vector<int> values;
#define XX_COLLECT(name, val, str, desc) values.push_back(au::err::name);
    XERROR_MAP(XX_COLLECT)
#undef XX_COLLECT

    std::sort(values.begin(), values.end());
    for (std::size_t i = 1; i < values.size(); ++i) {
        EXPECT_NE(values[i - 1], values[i]) << "Duplicate value: " << values[i];
    }
}

TEST(XError, code_range_common_state)
{
    // Common & State errors are in (-1999, -1000]
    EXPECT_EQ(au::err::kErrorNotInitialized, -1001);
    EXPECT_EQ(au::err::kErrorUnknown, -1099);
    EXPECT_EQ(au::err::kSuccess, 0);
}

TEST(XError, code_range_file_io)
{
    // File & IO errors are in (-2999, -2000]
    EXPECT_EQ(au::err::kErrorFileNotFound, -2001);
    EXPECT_EQ(au::err::kErrorDiskFull, -2008);
}

TEST(XError, code_range_dlib_platform)
{
    // Dynamic Library & Platform errors are in (-3999, -3000]
    EXPECT_EQ(au::err::kErrorDlibOpenFailed, -3001);
    EXPECT_EQ(au::err::kErrorPlatformAPI, -3004);
}

TEST(XError, code_range_memory_buffer)
{
    // Memory & Buffer errors are in (-4999, -4000]
    EXPECT_EQ(au::err::kErrorNoMemory, -4001);
    EXPECT_EQ(au::err::kErrorBufferUnderflow, -4005);
}

#endif  // ENABLE_TEST_XERROR
