#ifndef AURA_MM_XACCESS_API_H_
#define AURA_MM_XACCESS_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define AU_MM_NOEXCEPT noexcept
#else
#define AU_MM_NOEXCEPT
#endif

enum au_mm_access_mode { AU_MM_READ = 1, AU_MM_WRITE = 2, AU_MM_READ_WRITE = 3 };
#define AURA_MM_ACCESS_ABI_VERSION 1u
typedef struct au_mm_range {
    const void* mData;
    size_t mSize;
    uint32_t mMode;
} au_mm_range;
/* One image may have four planes. Larger sets use multiple descriptors. */
typedef struct au_mm_access_set {
    uint32_t mStructSize;
    au_mm_range mRanges[4];
    uint32_t mCount;
    int mCode;
} au_mm_access_set;

/* begin copies descriptors. Successful finish consumes the scope, including on
 * a cleanup error. A scope must end before submitting device work on its data. */
int au_mm_cpu_begin(const au_mm_access_set* sets, size_t count, void** out) AU_MM_NOEXCEPT;
int au_mm_cpu_finish(void* scope, int bodyCode) AU_MM_NOEXCEPT;
uint32_t au_mm_access_version(void) AU_MM_NOEXCEPT;

typedef int (*au_mm_owner_retain)(void*);
typedef void (*au_mm_owner_release)(void*);
typedef struct au_mm_external_memory {
    uint32_t mStructSize;
    void* mData;
    size_t mSize;
    int mFd;                  /* -1 for ordinary CPU storage. */
    uint32_t mType;            /* au::mm::MemType: 0 Pss, 1/2 DMA. */
    void* mOwner;
    au_mm_owner_retain mRetain; /* Both callbacks present, or both null (borrowed). */
    au_mm_owner_release mRelease;
} au_mm_external_memory;

/* Registrations own an fd duplicate, never the caller's fd. Borrowed CPU maps
 * remain the caller's responsibility. Unregister retires discovery immediately;
 * accepted internal leases retain the map owner until their last use. */
int au_mm_register(const au_mm_external_memory* memory, void** out) AU_MM_NOEXCEPT;
int au_mm_unregister(void* registration) AU_MM_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef AU_MM_NOEXCEPT
#endif
