#include "sys/xsystem_vivo.h"

#include "sys/xsystem_property_detail.h"

namespace au {
namespace sys {
namespace vivo {
namespace {

#if AU_OS_ANDROID
std::string readVivoProperty(const char* key)
{
    return getSystemPropertyValue(key, std::string());
}
#endif

}  // namespace

SocVendor getSolutionVendor() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::detectVivoSolutionVendor(readVivoProperty);
    } catch (...) {
        return SocVendor::Unknown;
    }
#else
    return SocVendor::Unknown;
#endif
}

bool isQualcommPlatform() noexcept
{
    return getSolutionVendor() == SocVendor::Qualcomm;
}

bool isMediaTekPlatform() noexcept
{
    return getSolutionVendor() == SocVendor::MediaTek;
}

bool isOverseasDevice() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::detectVivoRegion(readVivoProperty) == detail::VivoRegion::Overseas;
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

bool isDomesticDevice() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::detectVivoRegion(readVivoProperty) == detail::VivoRegion::Domestic;
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

std::string getChipId() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::readVivoChipId(readVivoProperty);
    } catch (...) {
        return {};
    }
#else
    return {};
#endif
}

std::string getDeviceName() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::readVivoDeviceName(readVivoProperty);
    } catch (...) {
        return {};
    }
#else
    return {};
#endif
}

}  // namespace vivo
}  // namespace sys
}  // namespace au
