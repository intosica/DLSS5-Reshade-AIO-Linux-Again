#include "../include/seh-shim.h"
#include <windows.h>
#include <setjmp.h>

static __thread jmp_buf *g_active_jmp_buf;
static __thread unsigned long g_last_exception_code;
static void *g_handler_handle;

static LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *info)
{
    if (!g_active_jmp_buf)
        return EXCEPTION_CONTINUE_SEARCH;

    g_last_exception_code = info->ExceptionRecord->ExceptionCode;
    longjmp(*g_active_jmp_buf, 1);
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
    jmp_buf *previous = g_active_jmp_buf;
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
