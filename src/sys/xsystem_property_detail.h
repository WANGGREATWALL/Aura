// Private to src/sys. The template functions take a property reader so the
// Android/Vivo classification rules can be tested with supplied values without
// changing real device properties. Production passes its platform reader.
// This header is not installed or exposed through inc/sys/xsystem.h.
#ifndef AURA_SYS_XSYSTEM_PROPERTY_DETAIL_H_
#define AURA_SYS_XSYSTEM_PROPERTY_DETAIL_H_

#include "sys/xsystem.h"

namespace au {
namespace sys {
namespace detail {

// Pure property-dependent logic. Tests supply a reader without modifying the
// process or device property table; production supplies the platform reader.
template <typename Reader>
SocVendor detectSocVendor(Reader read)
{
    const std::string hw = read("ro.hardware");
    if (hw.find("qcom") != std::string::npos || hw.find("sdm") != std::string::npos ||
        hw.find("msm") != std::string::npos || hw.find("kona") != std::string::npos)
        return SocVendor::Qualcomm;
    if (hw.find("mt") != std::string::npos)
        return SocVendor::MediaTek;
    if (hw.find("exynos") != std::string::npos)
        return SocVendor::Samsung;
    if (hw.find("kirin") != std::string::npos || hw.find("hi") != std::string::npos)
        return SocVendor::HiSilicon;
    return SocVendor::Unknown;
}

enum class VivoRegion { Unknown, Domestic, Overseas };

template <typename Reader>
VivoRegion detectVivoRegion(Reader read)
{
    const std::string overseas = read("ro.vendor.vivo.product.overseas");
    if (overseas == "yes")
        return VivoRegion::Overseas;
    if (overseas == "no")
        return VivoRegion::Domestic;

    const std::string model = read("ro.vendor.vivo.product.model");
    if (model.empty())
        return VivoRegion::Unknown;
    return model.size() >= 3 && model.compare(model.size() - 3, 3, "_EX") == 0
               ? VivoRegion::Overseas
               : VivoRegion::Domestic;
}

template <typename Reader>
SocVendor detectVivoSolutionVendor(Reader read)
{
    const std::string solution = read("ro.vendor.vivo.product.solution");
    if (solution == "QCOM")
        return SocVendor::Qualcomm;
    if (solution == "MTK")
        return SocVendor::MediaTek;
    return SocVendor::Unknown;
}

template <typename Reader>
std::string readVivoChipId(Reader read)
{
    return read("ro.vendor.vivo.product.platform");
}

template <typename Reader>
std::string readVivoDeviceName(Reader read)
{
    return read("ro.product.vendor.device");
}

int parsePropertyInt(const char* value, int fallback) noexcept;
float parsePropertyFloat(const char* value, float fallback) noexcept;

}  // namespace detail
}  // namespace sys
}  // namespace au

#endif  // AURA_SYS_XSYSTEM_PROPERTY_DETAIL_H_
