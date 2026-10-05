#ifndef AURA_XMATH_H_
#define AURA_XMATH_H_

/**
 * @file xmath.h
 * @brief Header-only math primitives: constants, clamping, alignment, range tests.
 *
 * All functions are inline / constexpr / templated and resolve at compile time.
 * No AU_API annotation is required — there is no out-of-line ABI surface.
 */

#include <cmath>
#include <cstdint>
#include <cassert>


namespace au {
namespace math {

// ============================================================================
// constants
// ============================================================================

constexpr double PI       = 3.14159265358979323846;
constexpr double TWO_PI   = 6.28318530717958647692;
constexpr double HALF_PI  = 1.57079632679489661923;
constexpr double E        = 2.71828182845904523536;
constexpr double SQRT2    = 1.41421356237309504880;
constexpr float  EPSILON  = 1e-6f;
constexpr double EPSILOND = 1e-10;


// ============================================================================
// compare & clamp
// ============================================================================

template <typename T>
constexpr T minOf(T a, T b) { return a < b ? a : b; }

template <typename T>
constexpr T maxOf(T a, T b) { return a > b ? a : b; }

template <typename T>
constexpr T minOf(T a, T b, T c) { return minOf(a, minOf(b, c)); }

template <typename T>
constexpr T maxOf(T a, T b, T c) { return maxOf(a, maxOf(b, c)); }


template <typename T>
constexpr T clampToRange(T val, T lo, T hi) { return val < lo ? lo : (val > hi ? hi : val); }

template <typename T>
constexpr T clampToUint8(T val) { return clampToRange(val, T{0}, T{255}); }

template <typename T>
constexpr T clampToUint16(T val) { return clampToRange(val, T{0}, T{65535}); }


// ============================================================================
// calculate
// ============================================================================

template <typename T>
constexpr T abs(T val) { return val < T{0} ? -val : val; }

template <typename T>
constexpr T sqr(T val) { return val * val; }

template <typename T>
constexpr T cube(T val) { return val * val * val; }

// linear interpolation
template <typename T, typename U>
constexpr T lerp(T a, T b, U t) { return a + static_cast<T>((b - a) * t); }

template <typename T>
constexpr T deg2rad(T deg) { return deg * static_cast<T>(PI / 180.0); }

template <typename T>
constexpr T rad2deg(T rad) { return rad * static_cast<T>(180.0 / PI); }

inline double powd(double base, double exp) { return std::pow(base, exp); }
inline float  powf(float base, float exp)   { return std::pow(base, exp); }

inline double sqrtd(double val) { return std::sqrt(val); }
inline float  sqrtf(float val)  { return std::sqrt(val); }

inline float sinf(float rad) { return std::sin(rad); }
inline float cosf(float rad) { return std::cos(rad); }
inline float tanf(float rad) { return std::tan(rad); }

inline bool approxEqual(float a, float b, float eps = EPSILON)   { return std::abs(a - b) <= eps; }
inline bool approxEqual(double a, double b, double eps = EPSILOND) { return std::abs(a - b) <= eps; }


// ============================================================================
// range
// ============================================================================

template <typename T> constexpr bool isInRangeCC(T val, T lo, T hi) { return val >= lo && val <= hi; }
template <typename T> constexpr bool isInRangeOO(T val, T lo, T hi) { return val >  lo && val <  hi; }
template <typename T> constexpr bool isInRangeOC(T val, T lo, T hi) { return val >  lo && val <= hi; }
template <typename T> constexpr bool isInRangeCO(T val, T lo, T hi) { return val >= lo && val <  hi; }


// ============================================================================
// align
// ============================================================================

inline bool isAlignedTo(int num, int align)
{
    assert(align > 0 && "align must be positive");
    return (num % align) == 0;
}

inline int ceilTo(int num, int align)
{
    assert(align > 0 && "align must be positive");
    return ((num + align - 1) / align) * align;
}

inline int floorTo(int num, int align)
{
    assert(align > 0 && "align must be positive");
    return (num / align) * align;
}

constexpr bool isAlignedToOdd(int num) { return (num & 1) == 1; }
constexpr int  ceilToOdd(int num)      { return num | 1; }
constexpr int  floorToOdd(int num)     { return isAlignedToOdd(num) ? num : num - 1; }

constexpr bool isAlignedTo2(int num) { return (num & 1) == 0; }
constexpr int  ceilTo2(int num)      { return (num + 1) & ~1; }
constexpr int  floorTo2(int num)     { return num & ~1; }

constexpr bool isAlignedTo4(int num) { return (num & 3) == 0; }
constexpr int  ceilTo4(int num)      { return (num + 3) & ~3; }
constexpr int  floorTo4(int num)     { return num & ~3; }

constexpr bool isAlignedTo8(int num) { return (num & 7) == 0; }
constexpr int  ceilTo8(int num)      { return (num + 7) & ~7; }
constexpr int  floorTo8(int num)     { return num & ~7; }

constexpr bool isPowerOf2(int num) { return num > 0 && (num & (num - 1)) == 0; }

} // namespace math
} // namespace au

#endif // AURA_XMATH_H_
