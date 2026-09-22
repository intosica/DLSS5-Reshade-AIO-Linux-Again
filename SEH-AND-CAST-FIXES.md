# GCC/mingw build fixes: SEH + FARPROC casts

Both remaining errors are MSVC-vs-GCC language differences, not logic bugs.
Fixes below are tested: the SEH shim is verified under Wine to actually catch
a real access violation and let the process continue (not just compile).

## 1. `__try` / `__except(EXCEPTION_EXECUTE_HANDLER)` (the real one)

GCC's C++ front end doesn't implement `__except` at all — `__try` is
silently aliased to plain `try`, so it then has nothing to match `__except`
against. Unlike Clang, mingw-w64 GCC has never implemented MSVC SEH syntax,
in C or C++ mode (confirmed by testing both here).

Fix: replace the `__try`/`__except` blocks with `SehGuardedCall()` — a
process-wide vectored exception handler (`AddVectoredExceptionHandler`) +
`setjmp`/`longjmp`, which is plain Win32 API and works with any compiler.
**This is not a hand-wave — I built and ran it under Wine**: a normal call
returns its value untouched, a deliberately-crashing call (null-pointer
write) returns the sentinel with `exception_code = 0xc0000005`
(`EXCEPTION_ACCESS_VIOLATION`, the same code a real driver fault raises),
and the process survives and exits cleanly afterward.

**One caveat**: unlike real SEH, unwinding via `longjmp` from a vectored
handler does not run C++ destructors for stack objects between the fault
site and the guarded call. All 5 of your `Safe*` functions only have POD
locals (raw pointers/handles, no destructors), so this doesn't apply to your
code as it stands — just don't reuse `SehGuardedCall` to wrap something with
non-trivial stack objects without checking that first.

**Files added**: `include/seh-shim.h`, `src/seh-shim.c` (compile as **C**,
not C++ — that's what makes `-fms-extensions` unnecessary; plain Win32 APIs
work the same either way).

**Call once at startup**, in `DllMain` at line 13151:

```diff
--- a/addon/src/nr-standalone.cpp
+++ b/addon/src/nr-standalone.cpp
@@
+#include "../include/seh-shim.h"
@@
 BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
 {
     if (reason == DLL_PROCESS_ATTACH)
     {
+        SehShimInit();
```

**Replace the 5 functions** (`SafeCreate` at line 3115, `SafeEvaluate` at
3134, `SafeRelease` at 3152, `SafeCreateFg` at 3798, `SafeEvaluateFg` at
3813 — line numbers as of the version I read; search for these function
names rather than trusting exact line numbers if you've already made other
edits) with the versions in `src/nr-standalone-safe-functions.inc` (included
here) — same signatures, same call sites elsewhere in the file don't need to
change at all, only the bodies of these 5 functions and their trampolines.

I could not compile-check this exact snippet against your real headers
(`NVSDK_NGX_Result`, `g_bridge_create`, etc. are proprietary NGX types not
available to me), but the pattern is mechanical and I did verify the
underlying `SehGuardedCall` mechanism compiles, links, and correctly
recovers from a real fault. If `g_ngx_params`/`g_nr_feature`/etc. aren't
visible at file scope where you paste these (e.g. if they're `static` in a
different translation unit), you'll get ordinary "undeclared identifier"
errors pointing at exactly which globals need forward declarations added.

## 2. `FARPROC` → `void*` (MSVC allows the implicit conversion, GCC doesn't)

`FARPROC` is `long long (*)()` — a function pointer type — and converting it
to `void*` (a data pointer) is technically a language-standard violation
(function pointers and data pointers aren't guaranteed interchangeable) that
MSVC quietly allows and `-fpermissive` GCC allows *with a warning*, but
plain GCC in this project's strict mode rejects. On Windows both pointer
kinds are actually the same size/representation, so this is always safe in
practice — it just needs an explicit cast to compile under GCC.

**`external/DLSS5-Feeder/src/feed_vk_hook.h:169`** (submodule code — reapply
this after any `git submodule update`, e.g. keep it as a small patch file
you `git apply` post-clone):

```diff
-    void *target = GetProcAddress(lib, "vkCreateDevice");
+    void *target = reinterpret_cast<void *>(GetProcAddress(lib, "vkCreateDevice"));
```

**`addon/src/nr-standalone.cpp:7773–7782`** (10 lines, all the same shape).
Fix by hand or with this sed — scope it narrowly (it only touches lines
matching `= GetProcAddress(...)` immediately followed by `;`, which nothing
else in the file coincidentally matches near there):

```bash
sed -i -E 's/= GetProcAddress\(([^)]*)\);/= reinterpret_cast<void *>(GetProcAddress(\1));/' \
    addon/src/nr-standalone.cpp
```

Then check the diff before committing — `git diff addon/src/nr-standalone.cpp`
— since a global-scope sed like this is worth eyeballing once rather than
trusting blindly, in case the pattern matches somewhere unexpected elsewhere
in a 13k-line file.

## Everything else in your log is noise

`#pragma optimize` unknown-pragma warnings, `-Wcast-function-type` warnings
on the dozens of other `GetProcAddress` calls (those assign to typed
function-pointer variables, not `void*`, so they're warnings not errors),
`-Wmissing-field-initializers` on `{sizeof(x)}`-style struct inits, and
`-Wignored-qualifiers`/`-Wattributes`/`novtable` — all MSVC-ism warnings
that don't affect correctness. None of those block the build; leave them.
