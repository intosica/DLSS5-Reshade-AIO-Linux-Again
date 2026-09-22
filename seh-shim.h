#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef long (*SehGuardedFn)(void *context);

// Call once, early (e.g. addon init / DllMain), before any SehGuardedCall.
void SehShimInit(void);

// Runs fn(context) with crash protection. On a structured exception
// (access violation, etc.) inside fn, returns 0x7fffffff and sets
// *exception_code to the Win32 exception code instead of crashing the
// process -- replacement for __try / __except(EXCEPTION_EXECUTE_HANDLER),
// which GCC does not implement (its C++ front end silently treats __try as
// plain try and then has nothing to match __except against; its C front
// end doesn't implement the MS SEH extension at all, unlike Clang).
//
// Caveat vs real SEH: unwinds via setjmp/longjmp from a vectored exception
// handler, which does NOT run C++ destructors for stack objects between the
// fault site and this frame. Verified correct (tested under Wine, catches
// a real access violation, process survives) for thin wrappers with only
// POD locals -- which is what every current call site is. Don't reuse it to
// guard code with non-trivial stack objects without checking that first.
long SehGuardedCall(SehGuardedFn fn, void *context, unsigned long *exception_code);

#ifdef __cplusplus
}
#endif
