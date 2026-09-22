// Replacement for __try/__except(EXCEPTION_EXECUTE_HANDLER): GCC's C++
// front end does not implement __except (only C++ try/catch; __try is
// silently aliased to try), and mingw-w64 GCC's C front end doesn't
// implement it either -- unlike Clang, which does support it as an MS
// extension. Rather than pull in a second compiler, this reimplements the
// same "catch a structured exception, don't crash the process" behaviour
// with plain Win32 APIs: a process-wide vectored exception handler plus
// setjmp/longjmp to unwind back to the guarded call.
#include "../include/seh-shim.h"
#include <windows.h>
#include <setjmp.h>

static __thread jmp_buf *g_active_jmp_buf;
static __thread unsigned long g_last_exception_code;
static void *g_handler_handle;

static LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *info)
{
    // Only intercept if we're actually inside a guarded call on this
    // thread; otherwise let normal handling (real crash, debugger, etc.)
    // proceed as if this handler weren't installed.
    if (!g_active_jmp_buf)
        return EXCEPTION_CONTINUE_SEARCH;

    g_last_exception_code = info->ExceptionRecord->ExceptionCode;
    longjmp(*g_active_jmp_buf, 1);
    // unreachable
}

void SehShimInit(void)
{
    if (!g_handler_handle)
        g_handler_handle = AddVectoredExceptionHandler(1, VectoredHandler);
}

long SehGuardedCall(SehGuardedFn fn, void *context, unsigned long *exception_code)
{
    *exception_code = 0;

    jmp_buf buf;
    jmp_buf *previous = g_active_jmp_buf; // supports nested guarded calls
    g_active_jmp_buf = &buf;

    long result;
    if (setjmp(buf) == 0)
    {
        result = fn(context);
    }
    else
    {
        *exception_code = g_last_exception_code;
        result = 0x7fffffffL;
    }

    g_active_jmp_buf = previous;
    return result;
}
