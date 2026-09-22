#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef long (*SehGuardedFn)(void *context);

void SehShimInit(void);

// Call once, on unload (e.g. DllMain's DLL_PROCESS_DETACH), to unregister
// the handler installed by SehShimInit(). Skipping this is not just a
// leak: if this DLL is later unloaded and reloaded (which this addon does
// during startup -- observed loading/unloading standalone-dlssnr.addon64
// three times before settling), the OLD handler pointer from the previous
// load stays registered in the process-wide vectored handler chain. When
// Wine later dispatches ANY unrelated exception (even a completely benign
// one, like UE4's thread-naming pseudo-exception), it walks that chain and
// calls the stale pointer -- which now points into unmapped/repurposed
// memory -- causing an immediate access violation.
void SehShimShutdown(void);

long SehGuardedCall(SehGuardedFn fn, void *context, unsigned long *exception_code);

#ifdef __cplusplus
}
#endif
