/*++

    Copyright (c) OpenDevicePartnership. All rights Reserved

Module Name:

    km_wil_result_macros.h

Abstract:

    This header includes an implementation of a few WIL result-handling macros that are ifdef'd out in kernel mode,
    presumably because some WIL configurations that use them leverage logging facilities that are not available in
    kernel mode. These are adapted from the following WIL headers with minor adjustments to make them work in kernel mode:
      - wil/common.h
      - wil/nt_result_macros.h

Environment:

    Kernel-mode Driver Framework

--*/

#pragma once

#include <wdm.h>
#include <wil/result_macros.h>

#ifndef WIL_KERNEL_MODE
#error You are not in kernel-mode, so this header is unnecessary. Use the WIL implementation from wil/result_macros.h instead.
#endif

namespace wilkm
{
namespace detail
{
void
LogFailure(const char *expression, const char *file, int line, NTSTATUS ntstatus);
} // namespace detail

#pragma region From wil/common.h

/** Verify that `status` is an NTSTATUS value.
Other types will generate an intentional compilation error.  Note that this will accept any `long` value as that is the
underlying typedef behind NTSTATUS.
//!
Note that occasionally you might run into an NTSTATUS which is directly defined with a `#define`, such as:
@code
#define STATUS_NOT_SUPPORTED             0x1
@endcode
Though this looks like an `NTSTATUS`, this is actually an `unsigned long` (the hex specification forces this).  When
these are encountered and they are NOT in the public SDK (have not yet shipped to the public), then you should change
their definition to match the manner in which `NTSTATUS` constants are defined in ntstatus.h:
@code
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BBL)
@endcode
When these are encountered in the public SDK, their type should not be changed and you should use a static_cast
to use this value in a macro that utilizes `verify_ntstatus`, for example:
@code
NT_RETURN_IF_FALSE(static_cast<NTSTATUS>(STATUS_NOT_SUPPORTED), (dispatch->Version == HKE_V1_0));
@endcode
@param status The NTSTATUS returning expression
@return An NTSTATUS representing the evaluation of `val`. */
template <typename T> _Post_satisfies_(return == status) inline long verify_ntstatus(T status)
{
    // Note: Written in terms of 'long' as NTSTATUS is actually:  typedef _Return_type_success_(return >= 0) long NTSTATUS
    static_assert(wistd::is_same<T, long>::value, "Wrong Type: NTSTATUS expected");
    return status;
}

#pragma endregion

#pragma region From wil/nt_result_macros.h
// Force the compiler to evaluate a call to 'wprintf' to verify the format string & args and produce warnings if there
// are any issues. The short-circuit 'and' will prevent the call and strings used from making it into the binary.
// Note that this requires using a string literal for the format string. If you don't, you'll get the following compiler
// error: error C2146: syntax error: missing ')' before identifier '...'
#if !defined(wprintf) && !defined(WIL_NO_MSG_FORMAT_CHECKS)
#define __WI_CHECK_MSG_FMT(fmt, ...) (0 && ::wprintf(L"" fmt, ##__VA_ARGS__)) ? nullptr : fmt, ##__VA_ARGS__
#else
#define __WI_CHECK_MSG_FMT(fmt, ...) fmt, ##__VA_ARGS__
#endif

#define __NT_RETURN_NTSTATUS(status, str)                                                                                          \
    {                                                                                                                              \
        NTSTATUS __status = (status);                                                                                              \
        if (!NT_SUCCESS(__status))                                                                                                 \
        {                                                                                                                          \
            wilkm::detail::LogFailure(str, __FILE__, __LINE__, __status);                                                          \
        }                                                                                                                          \
        return __status;                                                                                                           \
    }

#define NT_RETURN_NTSTATUS(status) __NT_RETURN_NTSTATUS(wilkm::verify_ntstatus(status), #status)

// Conditionally returns failures (NTSTATUS) - always logs failures
#define NT_RETURN_IF_NTSTATUS_FAILED(status)                                                                                       \
    {                                                                                                                              \
        const auto __statusRet = wilkm::verify_ntstatus(status);                                                                   \
        if (!NT_SUCCESS(__statusRet))                                                                                              \
        {                                                                                                                          \
            __NT_RETURN_NTSTATUS(__statusRet, #status);                                                                            \
        }                                                                                                                          \
    }

#pragma endregion

} // namespace wilkm