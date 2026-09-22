// proton-compat.hpp
//
// Linux/Proton compatibility layer for the DLSS5-Reshade-AIO standalone addon.
//
// Two independent problems are addressed here:
//
//   1. IDXGIResource1::CreateSharedHandle() / D3D11_RESOURCE_MISC_SHARED_NTHANDLE
//      routinely fail under DXVK (observed as E_INVALIDARG / 0x80070057) because
//      DXVK's D3D11 layer does not implement NT-handle export/import for cross-
//      process or cross-API (D3D11<->D3D12) resource sharing. There is no DXVK
//      version check that reliably predicts this -- treat failure as an expected
//      outcome under Wine and fall back to a CPU staging copy, which only needs
//      ordinary Map/Unmap + CopyTextureRegion and therefore works identically on
//      DXVK, VKD3D-Proton, and native Windows.
//
//   2. NVIDIA's Windows driver installer lays the core NGX snippet out under
//      %SystemRoot%\System32\DriverStore\FileRepository\nv*.inf_amd64_*\_nvngx.dll.
//      That directory tree is a Windows Update / driver-store construct and is
//      never populated inside a Wine prefix. Recent NVIDIA Linux driver packages
//      instead place _nvngx.dll / nvngx.dll directly in the prefix's
//      drive_c\windows\system32 (Proton/Steam sources these from the host
//      driver's /usr/lib{,/x86_64-linux-gnu}/nvidia/wine/ directory when the
//      prefix is created). Detect Wine and prefer that flat layout instead of
//      walking a DriverStore tree that is guaranteed not to exist there.
//
// This header has no dependency on the NGX SDK or ReShade SDK headers, so it
// can be compiled and unit-tested in isolation from the rest of the addon.

#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

namespace protoncompat {

// ---------------------------------------------------------------------------
// Wine / Proton detection
// ---------------------------------------------------------------------------

// ntdll.dll exports `wine_get_version` only inside Wine (including every
// Proton build, since Proton is a Wine fork). It is never present on real
// Windows, which makes GetProcAddress() the standard, dependency-free way
// community tools (DXVK, vkd3d-proton, countless mods) detect Wine at
// runtime. Cheaper than parsing the registry or shelling out.
inline bool IsRunningUnderWine()
{
    static const bool cached = []() -> bool
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        return GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return cached;
}

// Returns e.g. L"9.0-staging" under Wine/Proton, or an empty string on
// native Windows / if the export is missing for some reason. Purely
// informational -- useful in log lines when triaging interop failures.
inline std::wstring WineVersionString()
{
    if (!IsRunningUnderWine()) return {};
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using wine_get_version_t = const char *(__cdecl *)();
    const auto fn = reinterpret_cast<wine_get_version_t>(GetProcAddress(ntdll, "wine_get_version"));
    if (!fn) return {};
    const char *version = fn();
    if (!version) return {};
    return std::wstring(version, version + std::strlen(version));
}

// ---------------------------------------------------------------------------
// NGX core module (_nvngx.dll) resolution
// ---------------------------------------------------------------------------

// Drop-in replacement for the addon's original LoadInstalledNgxCore(). Tries,
// in order: (native Windows only) DriverStore package walk, flat
// %SystemRoot%\System32\_nvngx.dll / nvngx.dll, then beside the game exe /
// the current working directory. Under Wine the DriverStore walk is skipped
// on the first pass (it cannot succeed there) and only retried as an absolute
// last resort in case some future driver package proves this assumption
// wrong. On success, `*out_used_path` (if non-null) receives the path that
// was actually loaded, for logging.
HMODULE LoadInstalledNgxCoreProtonAware(std::wstring *out_used_path = nullptr);

// ---------------------------------------------------------------------------
// Cross-API (D3D11 <-> D3D12) shared resource creation with CPU fallback
// ---------------------------------------------------------------------------

enum class SharedResourceTransport
{
    Failed,
    NtHandle,   // IDXGIResource1::CreateSharedHandle / ID3D11Device1::OpenSharedResource1
    CpuStaging, // ID3D11DeviceContext::CopyResource + Map/Unmap, then a D3D12 upload-heap copy
};

struct SharedResourceResult
{
    SharedResourceTransport transport = SharedResourceTransport::Failed;

    // Valid for both transports: the D3D12-side resource NGX will actually read from / write to.
    Microsoft::WRL::ComPtr<ID3D12Resource> resource12;

    // Only populated when transport == CpuStaging.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging11; // CPU-readback copy of the D3D11 source
    Microsoft::WRL::ComPtr<ID3D12Resource> upload12;    // persistently-mapped D3D12 upload heap
    void *upload_mapped = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_footprint = {};
    UINT upload_num_rows = 0;
    UINT64 upload_row_bytes = 0;
    D3D12_RESOURCE_STATES dest_state = D3D12_RESOURCE_STATE_COMMON;
};

// Attempts the native NT-handle GPU-to-GPU share first (unless
// `force_cpu_fallback` is set, e.g. because the caller already knows it is
// running under Wine and wants to skip the doomed-to-fail first attempt).
// On any failure of the NT-handle path, transparently builds the CPU
// staging resources instead. Only returns transport == Failed if even the
// CPU path's resource creation fails (out of memory, invalid format, etc).
SharedResourceResult CreateCrossApiSharedResource(
    ID3D11Device *device11, ID3D12Device *device12,
    UINT width, UINT height, DXGI_FORMAT format,
    bool force_cpu_fallback = false);

// Call once per frame when transport == CpuStaging, with an *already open*
// D3D12 graphics command list (the addon's existing neural command list is
// fine -- this just appends a barrier + CopyTextureRegion to it, it does not
// submit anything itself, so it composes with the addon's existing
// BeginNeuralCommands()/SubmitNeuralCommands() fencing). `source11` is read
// with CopyResource() + a blocking Map(D3D11_MAP_READ), so call this after
// the D3D11 side has finished producing the frame's color/depth/motion
// texture for this frame, not before.
bool PumpCpuStagingCopy(ID3D11DeviceContext *context11, ID3D11Resource *source11,
    SharedResourceResult &result, ID3D12GraphicsCommandList *upload_list);

// Transitions result.resource12 into `after` on `list`, tracking the
// resource's current state on `result` so repeated calls only emit a
// barrier when the state actually changes. Safe to call for both transports
// (a no-op barrier is cheap; callers do not need to special-case NtHandle
// vs CpuStaging when driving the resource into a shader-readable state).
void TransitionSharedResource(ID3D12GraphicsCommandList *list,
    SharedResourceResult &result, D3D12_RESOURCE_STATES after);

} // namespace protoncompat
