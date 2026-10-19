#ifndef AURA_SYS_XSYSTEM_VIVO_H_
#define AURA_SYS_XSYSTEM_VIVO_H_

#include "sys/xsystem.h"

namespace au {
namespace sys {
namespace vivo {

/// Vivo Android property queries. On other platforms or when the property is
/// unavailable, predicates return false, vendor returns Unknown and names
/// return an empty string. Unknown region is neither domestic nor overseas.
AU_API SocVendor getSolutionVendor() noexcept;
AU_API bool isQualcommPlatform() noexcept;
AU_API bool isMediaTekPlatform() noexcept;
AU_API bool isOverseasDevice() noexcept;
AU_API bool isDomesticDevice() noexcept;

/// Read ro.vendor.vivo.product.platform: Vivo's SoC model identifier, such as
/// "SM8750" or "MT6991". This is a model name, not a unique chip serial number.
/// Returns an empty string if the property is absent or Android is unavailable.
AU_API std::string getChipId() noexcept;

/// Read ro.product.vendor.device: the Android device codename, such as
/// "PD2454". This is not the consumer-facing product/marketing name.
/// Returns an empty string if the property is absent or Android is unavailable.
AU_API std::string getDeviceName() noexcept;

}  // namespace vivo
}  // namespace sys
}  // namespace au

#endif  // AURA_SYS_XSYSTEM_VIVO_H_
