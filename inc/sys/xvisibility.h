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
#endif
