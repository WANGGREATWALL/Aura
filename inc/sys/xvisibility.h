#ifndef AURA_XVISIBILITY_H_
#define AURA_XVISIBILITY_H_

// AURA_STATIC is set for static Aura; AURA_EXPORTS only for a shared producer.
#if defined(_WIN32)
#if defined(AURA_STATIC)
#define AU_API
#elif defined(AURA_EXPORTS)
#define AU_API __declspec(dllexport)
#else
#define AU_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#if defined(AURA_STATIC)
#define AU_API
#else
#define AU_API __attribute__((visibility("default")))
#endif
#else
#define AU_API
#endif

// Keep consumer-emitted inline/template symbols local to their shared object.
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define AU_LOCAL __attribute__((visibility("hidden")))
#else
#define AU_LOCAL
#endif

#endif
