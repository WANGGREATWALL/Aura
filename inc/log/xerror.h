#ifndef XERROR_H
#define XERROR_H

/**
 * @file xerror.h
 * @brief Unified error code definitions for the caddies library.
 *
 * Convention
 * ----------
 *  - kSuccess (0)   : operation completed successfully.
 *  - Negative values: all error conditions (POSIX style).
 *  - All constants are plain int; return them directly from any int-returning
 *    function without a cast.
 *
 * Usage
 * -----
 *  #include "log/xerror.h"
 *
 *  int load(const std::string& path) {
 *      if (path.empty()) return err::kErrorInvalidParam;
 *      if (!mHandle)     return err::kErrorNotInitialized;
 *      ...
 *      return err::kSuccess;
 *  }
 *
 *  if (err::isError(obj.load(path))) { ... }
 */

namespace err {

/**
 * @def XERROR_MAP(XX)
 * @brief X-Macro defining all error codes, their numerical values, strings, and descriptions.
 */
// clang-format off
#define XERROR_MAP(XX) \
    /* --- Success --- */ \
    XX(kSuccess              ,     0, "Success"              , "Operation completed successfully"                               ) \
    /* --- Common & State Errors (-1000 ~ -1999) --- */ \
    XX(kErrorNotInitialized  , -1001, "ErrorNotInitialized"  , "init()/open() not yet called; object is not usable"             ) \
    XX(kErrorAlreadyInited   , -1002, "ErrorAlreadyInited"   , "init()/open() called again on an already-live object"           ) \
    XX(kErrorNotReady        , -1003, "ErrorNotReady"        , "Setup has started (async) but is not yet complete"              ) \
    XX(kErrorBusy            , -1004, "ErrorBusy"            , "Resource is locked / in use; caller should retry later"         ) \
    XX(kErrorStateInvalid    , -1005, "ErrorStateInvalid"    , "State machine rejects this call in the current state"           ) \
    XX(kErrorInvalidParam    , -1006, "ErrorInvalidParam"    , "Argument violates precondition (wrong value / combination)"     ) \
    XX(kErrorNullPointer     , -1007, "ErrorNullPointer"     , "A required non-null pointer argument is nullptr"                ) \
    XX(kErrorInvalidHandle   , -1008, "ErrorInvalidHandle"   , "Handle is null, already released, or from a different owner"    ) \
    XX(kErrorOutOfRange      , -1009, "ErrorOutOfRange"      , "Index, offset, or numeric value exceeds valid bounds"           ) \
    XX(kErrorSizeMismatch    , -1010, "ErrorSizeMismatch"    , "Two related sizes (lengths, dimensions, counts) do not agree"   ) \
    XX(kErrorBadFormat       , -1011, "ErrorBadFormat"       , "Magic number, header signature, or data layout is unexpected"   ) \
    XX(kErrorVersionMismatch , -1012, "ErrorVersionMismatch" , "API, protocol, or binary format version is incompatible"        ) \
    XX(kErrorOverflow        , -1013, "ErrorOverflow"        , "Arithmetic result or container capacity would overflow"         ) \
    XX(kErrorNotSupported    , -1014, "ErrorNotSupported"    , "Feature or code path is not implemented on this platform/build" ) \
    XX(kErrorAlreadyExists   , -1015, "ErrorAlreadyExists"   , "Duplicate creation/registration rejected"                       ) \
    XX(kErrorTimeout         , -1016, "ErrorTimeout"         , "Operation did not complete within the allotted time"            ) \
    XX(kErrorAborted         , -1017, "ErrorAborted"         , "Operation was cancelled by the caller or an external signal"    ) \
    XX(kErrorExpired         , -1018, "ErrorExpired"         , "License, token, session, or time-limited resource has expired"  ) \
    XX(kErrorUnknown         , -1099, "ErrorUnknown"         , "Catch-all; always prefer a specific code over this"             ) \
    /* --- File & IO Errors (-2000 ~ -2999) --- */ \
    XX(kErrorFileNotFound    , -2001, "ErrorFileNotFound"    , "File path does not exist"                                       ) \
    XX(kErrorDirNotFound     , -2002, "ErrorDirNotFound"     , "Directory path does not exist"                                  ) \
    XX(kErrorOpenFailed      , -2003, "ErrorOpenFailed"      , "fopen/open/CreateFile failed"                                   ) \
    XX(kErrorReadFailed      , -2004, "ErrorReadFailed"      , "fread/read returned an error or unexpected byte count"          ) \
    XX(kErrorWriteFailed     , -2005, "ErrorWriteFailed"     , "fwrite/write returned an error or unexpected byte count"        ) \
    XX(kErrorFileSizeMismatch, -2006, "ErrorFileSizeMismatch", "Caller-provided buffer size differs from the file's actual size") \
    XX(kErrorPermissionDenied, -2007, "ErrorPermissionDenied", "Process lacks required filesystem permissions"                  ) \
    XX(kErrorDiskFull        , -2008, "ErrorDiskFull"        , "Target filesystem has no space left"                            ) \
    /* --- Dynamic Library & Platform Errors (-3000 ~ -3999) --- */ \
    XX(kErrorDlibOpenFailed  , -3001, "ErrorDlibOpenFailed"  , "dlopen/LoadLibrary failed"                                      ) \
    XX(kErrorDlibSymNotFound , -3002, "ErrorDlibSymNotFound" , "dlsym/GetProcAddress returned null"                             ) \
    XX(kErrorDlibCloseFailed , -3003, "ErrorDlibCloseFailed" , "dlclose/FreeLibrary failed"                                     ) \
    XX(kErrorPlatformAPI     , -3004, "ErrorPlatformAPI"     , "Underlying OS/platform API call failed"                         ) \
    /* --- Memory & Buffer Errors (-4000 ~ -4999) --- */ \
    XX(kErrorNoMemory        , -4001, "ErrorNoMemory"        , "malloc/new returned null; system out of memory"                 ) \
    XX(kErrorInvalidAddr     , -4002, "ErrorInvalidAddr"     , "Pointer is not owned by this allocator/pool"                    ) \
    XX(kErrorInvalidSize     , -4003, "ErrorInvalidSize"     , "Allocation size is zero or exceeds the pool's block limit"      ) \
    XX(kErrorAlignmentFault  , -4004, "ErrorAlignmentFault"  , "Buffer or allocation does not satisfy required alignment"       ) \
    XX(kErrorBufferUnderflow , -4005, "ErrorBufferUnderflow" , "Buffer has insufficient data; read would exceed available bytes")
// clang-format on

// Generate constexpr int definitions
#define XX_ENUM(name, val, str, desc) constexpr int name = val;  ///< desc
XERROR_MAP(XX_ENUM)
#undef XX_ENUM

/**
 * @brief Check if the return value represents an error.
 */
inline bool isError(int code) { return code < 0; }

/**
 * @brief Convert an error code to a readable string.
 */
inline const char* getErrorStr(int code)
{
    switch (code) {
#define XX_CASE(name, val, str, desc) \
    case name: return str;
        XERROR_MAP(XX_CASE)
#undef XX_CASE
        default: return "UnknownErrorCode";
    }
}

}  // namespace err

// Aura modules use au::err; retain the existing global err namespace for
// source compatibility with older callers.
namespace au {
namespace err = ::err;
}

#endif  // XERROR_H
