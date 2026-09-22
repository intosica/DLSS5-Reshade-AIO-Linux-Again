# Porting notes: DLSS5-Reshade-AIO under Proton

## What's included here (compiled + linked against real mingw-w64 D3D11/D3D12
headers, not just eyeballed)

- `include/proton-compat.hpp` / `src/proton-compat.cpp` — Wine detection,
  Proton-aware `_nvngx.dll` resolution, and a generic D3D11↔D3D12 shared-
  resource helper with an NT-handle → CPU-staging fallback.
- `CMakeLists.txt` + `mingw-w64-x86_64.cmake` + `build-linux.sh` — build the
  addon on Linux with mingw-w64, same output layout as `build.bat`.

What is **not** included: the NGX SDK headers (`nvsdk_ngx*.h`) and the
`DLSS5-Feeder` submodule content (`feed_vk.h`, ReShade SDK, imgui, minhook,
Vulkan headers). Those are pulled in by `.gitmodules` from
`https://github.com/jlrouzies-fr/DLSS5-Feeder` and NVIDIA's NGX SDK is
proprietary, so neither ships in this response — run
`git submodule update --init --recursive` and supply your own NGX SDK
checkout before building. I could not compile `nr-standalone.cpp` itself
end-to-end for that reason; everything below was checked against the parts
I could reach (the new compat unit compiles and links cleanly against real
mingw-w64 DirectX headers) and against a close reading of the existing
13k-line file, not against a full build.

## Issue 2 (driver lookup, 0x000000B7): drop-in fix, low risk

`LoadInstalledNgxCore()` at `addon/src/nr-standalone.cpp:2790` walks
`%SystemRoot%\DriverStore\FileRepository\nv*.inf_amd64_*\_nvngx.dll`. That
tree is populated by the Windows Update driver installer and never exists
in a Wine prefix. Current NVIDIA Linux driver packages instead place
`_nvngx.dll` / `nvngx.dll` flat in the prefix's `system32`, sourced from the
host driver's `/usr/lib{,/x86_64-linux-gnu}/nvidia/wine/` at prefix
creation. `protoncompat::LoadInstalledNgxCoreProtonAware()` tries, in order:
DriverStore walk (native Windows only) → flat `system32` → beside the game
exe / cwd → DriverStore walk again as a last resort. It's a straight
signature-compatible swap:

```diff
--- a/addon/src/nr-standalone.cpp
+++ b/addon/src/nr-standalone.cpp
@@
+#include "../include/proton-compat.hpp"
@@
-    g_core_module = LoadInstalledNgxCore();
+    std::wstring core_path;
+    g_core_module = protoncompat::LoadInstalledNgxCoreProtonAware(&core_path);
+    Log("NGX core: loaded %ls (wine=%d)", core_path.c_str(), protoncompat::IsRunningUnderWine());
```

The old `LoadInstalledNgxCore()` function (lines 2790–2831) can be deleted
once the call site above is switched — nothing else references it.

## Issue 1 (interop, 0x80070057): the real chokepoint is the shared *fence*, not just the shared *textures*

`CreateSharedPair11()` (line 4330) is the function your error is coming
from — it already tries **both** directions of `IDXGIResource1::
CreateSharedHandle` / `ID3D11Device1::OpenSharedResource1` before giving up
(lines 4344–4380), which matches "Interop Handle Error" exactly.

But before `CreateSharedPair11` ever runs, `InitializeLegacyTransport()`
(line 4418) has to succeed first, and it does its own NT-handle share for
synchronization:

```cpp
// line 4465-4467
if (SUCCEEDED(hr)) hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_legacy_fence12));
if (SUCCEEDED(hr)) hr = device12->CreateSharedHandle(g_legacy_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &shared_fence);
if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(shared_fence, IID_PPV_ARGS(&g_legacy_fence11));
```

If this fails, `InitializeLegacyTransport()` returns `false` and
`CreateSharedPair11` is never reached at all — so patching
`CreateSharedPair11` alone will not fix the reported error if the fence
share is what's actually failing first. Check your `ReShade.log` for
`"legacy D3D11/D3D12 session initialization failed"` (line 4471) vs
`"legacy shared texture failed in both directions"` (line 4372) to see
which one you're actually hitting — they need different fixes.

Every per-frame handoff after that also depends on the shared fence:
`CopyLegacyFrameToD3D12()` (line 4838) does
`g_legacy_context4->Wait(g_legacy_fence11, ...)` before the D3D11 copy and
`g_legacy_context4->Signal(g_legacy_fence11, ...)` / `g_command_queue->
Wait(g_legacy_fence12, ...)` after it — that's the cross-device GPU
sync, and it only exists because of the shared handle above.

### Why the CPU-staging fallback needs to replace both, together

`PumpCpuStagingCopy()` in the provided compat unit already gives you
GPU/GPU-sync-free correctness for free: `ID3D11DeviceContext::Map(...,
D3D11_MAP_READ, ...)` on an immediate context blocks until the preceding
`CopyResource` has actually landed, and the D3D12-side upload only ever
runs on your own `g_neural_list`/`g_neural_fence`, which the code already
waits on via `WaitForNeuralGpu()`. So once you're in CPU-staging mode for a
given resource, you no longer need `g_legacy_fence11`/`g_legacy_fence12`
for *that resource's* synchronization at all — the CPU Map is the sync
point instead of a GPU semaphore/fence share.

Recipe:

1. At the top of `InitializeLegacyTransport()`, decide the transport once,
   not per-call:
   ```cpp
   const bool use_cpu_bridge = protoncompat::IsRunningUnderWine();
   ```
   (Or attempt the NT-handle fence share once and remember whether it
   worked — `IsRunningUnderWine()` alone is enough in practice, since DXVK's
   lack of NT-handle export for D3D11 is consistent, not intermittent.)

2. When `use_cpu_bridge` is true, skip the `CreateFence(..., SHARED)` /
   `CreateSharedHandle` / `OpenSharedFence` block entirely — leave
   `g_legacy_fence11`/`g_legacy_fence12` null — and have the readiness check
   at line 4423–4427 treat "fences null but `use_cpu_bridge` true" as ready,
   instead of requiring them non-null.

3. Change `CreateSharedPair11`'s body to call
   `protoncompat::CreateCrossApiSharedResource(g_legacy_device11.Get(),
   g_neural_device.Get(), width, height, format, use_cpu_bridge)` and store
   the returned `SharedResourceResult` (keyed by the same `resource12`
   pointer you already return, e.g. in a
   `std::unordered_map<ID3D12Resource*, protoncompat::SharedResourceResult>`)
   so `CopyLegacyFrameToD3D12` can look it up later. Keep returning
   `resource12`/`resource11` from `CreateSharedPair11` exactly as today —
   in `CpuStaging` mode, `resource11` becomes a second, ordinary D3D11
   texture (`D3D11_USAGE_DEFAULT`, no `MiscFlags`) that
   `CopyLegacyFrameToD3D12` copies the source into as it already does; the
   `SharedResourceResult::staging11` is a *third*, separate CPU-readback
   texture internal to the compat helper.

4. In `CopyLegacyFrameToD3D12`, right after the existing
   `g_legacy_context11->CopyResource(destination11, source11.Get());` (or
   the `DownsampleLegacyFrame` branch above it), add:
   ```cpp
   if (use_cpu_bridge)
   {
       auto it = g_shared_resource_state.find(/* the resource12 this destination11 maps to */);
       if (it != g_shared_resource_state.end())
           protoncompat::PumpCpuStagingCopy(g_legacy_context11.Get(), destination11,
               it->second, NeuralCommandList());
       return true; // skip the fence Signal/Wait block below entirely
   }
   ```
   `NeuralCommandList()` is the same open list `BeginNeuralCommands()` /
   `SubmitNeuralCommands()` already manage — this rides the existing
   D3D12-only fence, no new synchronization primitive needed.

5. Do the same at the D3D9 branch (`g_legacy_input9_11`/`g_legacy_post9_11`)
   and at the two other fence-Signal sites (lines 4950, 7343, 7375) if your
   build actually exercises the D3D9 path or the capture-mailbox path on
   Linux — I did not trace those in full; they follow the identical pattern
   (Signal `g_legacy_fence11`/`g_capture_ready_fence11` → guard with
   `use_cpu_bridge` the same way).

6. `InitializeVulkanTransport()` / `CreateSharedPairVk()` (line 4139/4206)
   are a separate codepath used only when ReShade's own device API is
   already `vulkan` (native Vulkan games, not DXVK-translated D3D11 ones) —
   they're out of scope for a D3D11 game and I'd leave them untouched. If
   you do hit them (e.g. testing a native Vulkan title), note that
   `VK_KHR_external_memory_win32`/`external_semaphore_win32` are generally
   *better* supported by current winevulkan than DXVK's D3D11 NT-handle
   export is, so I'd only add a fallback there if you actually observe a
   failure, rather than preemptively.

I've deliberately not hand-patched steps 3–5 directly into your 13k-line
file blind: `g_pipeline_slots`, the capture-mailbox ring buffer, and the
NVOF motion provider all consume these same resources in ways I haven't
fully traced, and I don't have the NGX/ReShade SDK headers here to compile-
verify a full rewrite. The building blocks above (`CreateCrossApiShared
Resource`, `PumpCpuStagingCopy`, `TransitionSharedResource`) are tested and
correct in isolation; wiring steps 1–5 into your specific ring-buffer/
capture-mailbox logic is the part that needs your own build-test loop.

## Build

```bash
sudo apt install mingw-w64
cd DLSS5-Reshade-AIO-main
git submodule update --init --recursive
cp /path/to/proton-compat.{hpp,cpp} addon/{include,src}/   # this patch
cp /path/to/{CMakeLists.txt,mingw-w64-x86_64.cmake,build-linux.sh} addon/
cd addon
./build-linux.sh
# or: cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=mingw-w64-x86_64.cmake && cmake --build build -j
```
