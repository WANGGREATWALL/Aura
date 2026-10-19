#if ENABLE_TEST_XSYSTEM

#include "gtest/gtest.h"
#include "log/xerror.h"
#include "sys/xsystem.h"
#include "sys/xsystem_vivo.h"
#include "sys/xsystem_property_detail.h"

#include <climits>
#include <string>
#include <thread>

namespace {

using namespace au::sys;

static_assert(AU_OS_WINDOWS + AU_OS_MACOS + AU_OS_IOS + AU_OS_ANDROID + AU_OS_LINUX <= 1,
              "OS macros must be mutually exclusive");
static_assert(AU_OS_APPLE == (AU_OS_MACOS || AU_OS_IOS),
              "AU_OS_APPLE denotes the Apple family");

TEST(XSystem, BuildPlatformAndNames)
{
#if AU_OS_WINDOWS
    EXPECT_EQ(kBuildPlatform, Platform::Windows);
#elif AU_OS_MACOS
    EXPECT_EQ(kBuildPlatform, Platform::macOS);
#elif AU_OS_IOS
    EXPECT_EQ(kBuildPlatform, Platform::iOS);
#elif AU_OS_ANDROID
    EXPECT_EQ(kBuildPlatform, Platform::Android);
#elif AU_OS_LINUX
    EXPECT_EQ(kBuildPlatform, Platform::Linux);
#endif
    EXPECT_STRNE(platformName(kBuildPlatform), "");
    EXPECT_STRNE(archName(kBuildArch), "");
    EXPECT_STREQ(platformName(Platform::Unknown), "Unknown");
}

TEST(XSystem, CpuMemoryAndHost)
{
    EXPECT_GT(getLogicalCpuCount(), 0);
    EXPECT_GT(getOnlineCpuCount(), 0);
    EXPECT_GT(getMemoryTotalBytes(), 0u);
    EXPECT_LE(getMemoryAvailableBytes(), getMemoryTotalBytes());
    EXPECT_GT(getHardwareConcurrency(), 0);
    EXPECT_FALSE(getHostName().empty());
    // CPU model can be unavailable on some platforms; an empty result is valid.
    (void)getCpuModelName();
}

TEST(XSystem, Environment)
{
    const std::string name = "AURA_XSYSTEM_TEST_VAR";
    EXPECT_FALSE(hasEnvVar("AURA_XSYSTEM_NONEXISTENT_VAR_42"));
    EXPECT_EQ(getEnvVar("AURA_XSYSTEM_NONEXISTENT_VAR_42", "fallback"), "fallback");
    EXPECT_EQ(setEnvVar(name, "one"), err::kSuccess);
    EXPECT_TRUE(hasEnvVar(name));
    EXPECT_EQ(getEnvVar(name), "one");
    EXPECT_EQ(setEnvVar(name, ""), err::kSuccess);
    EXPECT_TRUE(hasEnvVar(name));
    EXPECT_EQ(getEnvVar(name, "fallback"), "");
    EXPECT_EQ(setEnvVar("", "value"), err::kErrorInvalidParam);
    EXPECT_EQ(setEnvVar("BAD=NAME", "value"), err::kErrorInvalidParam);
}

TEST(XSystem, AndroidPropertyContract)
{
#if !AU_OS_ANDROID
    EXPECT_EQ(getSystemPropertyValue("aura.missing", 47), 47);
    EXPECT_FLOAT_EQ(getSystemPropertyValue("aura.missing", 1.5f), 1.5f);
    EXPECT_EQ(getSystemPropertyValue("aura.missing", std::string("fallback")), "fallback");
    EXPECT_EQ(setSystemPropertyValue("aura.test", 1), err::kErrorNotSupported);
    EXPECT_EQ(setSystemPropertyValue("aura.test", 1.0f), err::kErrorNotSupported);
    EXPECT_EQ(setSystemPropertyValue("aura.test", std::string("value")),
              err::kErrorNotSupported);
    EXPECT_FALSE(vivo::isOverseasDevice());
    EXPECT_FALSE(vivo::isDomesticDevice());
    EXPECT_EQ(vivo::getSolutionVendor(), SocVendor::Unknown);
#endif
    EXPECT_EQ(detail::parsePropertyInt("12", 7), 12);
    EXPECT_EQ(detail::parsePropertyInt("12oops", 7), 7);
    EXPECT_EQ(detail::parsePropertyInt("999999999999999999999", 7), 7);
    EXPECT_EQ(detail::parsePropertyInt("", 7), 7);
    EXPECT_FLOAT_EQ(detail::parsePropertyFloat("2.25", 1.5f), 2.25f);
    EXPECT_FLOAT_EQ(detail::parsePropertyFloat("nan", 1.5f), 1.5f);
    EXPECT_FLOAT_EQ(detail::parsePropertyFloat("2.2oops", 1.5f), 1.5f);
}

TEST(XSystem, InjectedAndroidAndVivoProperties)
{
    const auto hardware = [](const char* key) -> std::string {
        return std::string(key) == "ro.hardware" ? "qcom" : "";
    };
    EXPECT_EQ(detail::detectSocVendor(hardware), SocVendor::Qualcomm);

    const auto overseas = [](const char* key) -> std::string {
        const std::string property(key);
        if (property == "ro.vendor.vivo.product.overseas") return "yes";
        if (property == "ro.vendor.vivo.product.solution") return "MTK";
        if (property == "ro.vendor.vivo.product.platform") return "MT6991";
        return "";
    };
    EXPECT_EQ(detail::detectVivoRegion(overseas), detail::VivoRegion::Overseas);
    EXPECT_EQ(detail::detectVivoSolutionVendor(overseas), SocVendor::MediaTek);
    EXPECT_EQ(detail::readVivoChipId(overseas), "MT6991");

    const auto modelFallback = [](const char* key) -> std::string {
        return std::string(key) == "ro.vendor.vivo.product.model" ? "PD2527F_EX" : "";
    };
    EXPECT_EQ(detail::detectVivoRegion(modelFallback), detail::VivoRegion::Overseas);
    const auto noProperties = [](const char*) -> std::string { return ""; };
    EXPECT_EQ(detail::detectVivoRegion(noProperties), detail::VivoRegion::Unknown);
    EXPECT_EQ(detail::detectVivoSolutionVendor(noProperties), SocVendor::Unknown);
}

TEST(XSystem, ProcessAndThreadIds)
{
    const uint64_t process = getCurrentProcessId();
    const uint64_t mainThread = getCurrentThreadId();
    EXPECT_GT(process, 0u);
    EXPECT_GT(mainThread, 0u);
    uint64_t workerThread = 0;
    std::thread worker([&] { workerThread = getCurrentThreadId(); });
    worker.join();
    EXPECT_GT(workerThread, 0u);
    EXPECT_NE(workerThread, mainThread);
    EXPECT_EQ(getCurrentProcessId(), process);
}

}  // namespace

#endif  // ENABLE_TEST_XSYSTEM
