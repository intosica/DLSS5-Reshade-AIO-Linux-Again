#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef long (*SehGuardedFn)(void *context);

void SehShimInit(void);

long SehGuardedCall(SehGuardedFn fn, void *context, unsigned long *exception_code);

#ifdef __cplusplus
}
#endif
