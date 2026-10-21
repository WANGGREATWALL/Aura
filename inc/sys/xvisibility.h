#ifndef AURA_SYS_XVISIBILITY_H_
#define AURA_SYS_XVISIBILITY_H_

/* Header-only SDK facades and binding callbacks must stay in their consumer's
 * shared object too. Private library flags alone do not hide inline/template
 * instantiations emitted by a consumer with default symbol visibility. */
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define AU_LOCAL __attribute__((visibility("hidden")))
#else
#define AU_LOCAL
#endif
/* C-compatible public ABI visibility (xsystem.h is C++ only). */
#ifndef AURA_API
#if defined(_WIN32) && !defined(AURA_STATIC)
#if defined(AURA_EXPORTS)
#define AURA_API __declspec(dllexport)
#else
#define AURA_API __declspec(dllimport)
#endif
#elif (defined(__GNUC__) || defined(__clang__)) && !defined(AURA_STATIC)
#define AURA_API __attribute__((visibility("default")))
#else
#define AURA_API
#endif
#endif
#endif
