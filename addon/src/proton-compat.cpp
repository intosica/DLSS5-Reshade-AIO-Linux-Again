#include "../include/proton-compat.hpp"

namespace protoncompat {
namespace {

HMODULE TryLoad(const std::wstring &path, std::wstring *out_used_path)
{
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module && out_used_path) *out_used_path = path;
    return module;
}

// Original Windows driver layout: walk DriverStore package folders looking
// for the newest _nvngx.dll. Only ever matches on a real Windows install
// where the NVIDIA installer has run.
HMODULE FindViaDriverStore(std::wstring *out_used_path)
{
    wchar_t system[MAX_PATH] = {}, pattern[MAX_PATH] = {};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return nullptr;
    swprintf_s(pattern, L"%s\\DriverStore\\FileRepository\\nv*.inf_amd64_*", system);

    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return nullptr;

    std::wstring best;
    FILETIME best_time = {};
    do
    {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        wchar_t candidate[MAX_PATH] = {};
        swprintf_s(candidate, L"%s\\DriverStore\\FileRepository\\%s\\_nvngx.dll", system, found.cFileName);
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExW(candidate, GetFileExInfoStandard, &attributes)) continue;
        if (best.empty() || CompareFileTime(&attributes.ftLastWriteTime, &best_time) > 0)
        {
            best = candidate;
            best_time = attributes.ftLastWriteTime;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);

    if (best.empty()) return nullptr;
    return TryLoad(best, out_used_path);
}

// Proton/Wine layout: current NVIDIA Linux driver packages install
// `_nvngx.dll` and `nvngx.dll` directly into the prefix's
// drive_c\windows\system32 -- there is no DriverStore package tree, since
// that is a Windows Update construct the Linux driver has no reason to
// replicate. Steam/Proton populates this from the host driver's
// /usr/lib{,/x86_64-linux-gnu}/nvidia/wine/ directory when the prefix is
// created, so a flat system32 lookup is the correct primary strategy here.
HMODULE FindFlatSystem32(std::wstring *out_used_path)
{
    wchar_t system[MAX_PATH] = {};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return nullptr;
    for (const wchar_t *name : {L"_nvngx.dll", L"nvngx.dll"})
    {
        wchar_t candidate[MAX_PATH] = {};
        swprintf_s(candidate, L"%s\\%s", system, name);
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
        if (const HMODULE module = TryLoad(candidate, out_used_path)) return module;
    }
    return nullptr;
}

// Last resort: beside the game executable and the current working
// directory -- covers prefixes where the distro's driver package does not
// auto-populate system32 and the user copied _nvngx.dll manually instead.
HMODULE FindBesideGame(std::wstring *out_used_path)
{
    std::vector<std::wstring> directories;

    wchar_t game_directory[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, game_directory, MAX_PATH) != 0)
        if (wchar_t *slash = wcsrchr(game_directory, L'\\'))
        {
            slash[1] = L'\0';
            directories.emplace_back(game_directory);
        }

    wchar_t working_directory[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, working_directory) != 0)
        directories.emplace_back(working_directory);

    for (const std::wstring &directory : directories)
        for (const wchar_t *name : {L"_nvngx.dll", L"nvngx.dll"})
        {
            wchar_t candidate[MAX_PATH] = {};
            swprintf_s(candidate, L"%ls\\%ls", directory.c_str(), name);
            if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
            if (const HMODULE module = TryLoad(candidate, out_used_path)) return module;
        }
    return nullptr;
}

} // namespace

HMODULE LoadInstalledNgxCoreProtonAware(std::wstring *out_used_path)
{
    if (const HMODULE loaded = GetModuleHandleW(L"_nvngx.dll"))
    {
        if (out_used_path) *out_used_path = L"(already loaded)";
        return loaded;
    }

    const bool wine = IsRunningUnderWine();

    // Skip the DriverStore walk under Wine on the first pass: it cannot
    // succeed there and only costs a FindFirstFile call plus a misleading
    // log line on every init.
    if (!wine)
        if (const HMODULE module = FindViaDriverStore(out_used_path)) return module;

    if (const HMODULE module = FindFlatSystem32(out_used_path)) return module;
    if (const HMODULE module = FindBesideGame(out_used_path)) return module;

    // Exhaust the Windows-native strategy too, just in case some future
    // driver package proves the "never exists under Wine" assumption wrong.
    if (wine)
        if (const HMODULE module = FindViaDriverStore(out_used_path)) return module;

    return nullptr;
}

SharedResourceResult CreateCrossApiSharedResource(
    ID3D11Device *device11, ID3D12Device *device12,
    UINT width, UINT height, DXGI_FORMAT format,
    bool force_cpu_fallback)
{
    SharedResourceResult result;
    if (!device11 || !device12 || width == 0 || height == 0) return result;

    if (!force_cpu_fallback)
    {
        // --- Tier 1: native NT-handle GPU-to-GPU share ----------------------
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
        HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
            &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource12));

        HANDLE shared = nullptr;
        if (SUCCEEDED(hr))
            hr = device12->CreateSharedHandle(resource12.Get(), nullptr, GENERIC_ALL, nullptr, &shared);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> resource11;
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<ID3D11Device1> device1;
            hr = device11->QueryInterface(IID_PPV_ARGS(&device1));
            if (SUCCEEDED(hr))
                hr = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&resource11));
        }
        if (shared) CloseHandle(shared);

        if (SUCCEEDED(hr))
        {
            result.transport = SharedResourceTransport::NtHandle;
            result.resource12 = resource12;
            result.dest_state = D3D12_RESOURCE_STATE_COMMON;
            return result;
        }
        // DXVK routinely fails this with E_INVALIDARG (0x80070057): it does
        // not implement NT-handle export/import for D3D11 resources across
        // processes or APIs. Expected here under Wine -- fall through.
    }

    // --- Tier 2: CPU staging copy (works identically everywhere) -----------
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging11;
    if (FAILED(device11->CreateTexture2D(&staging_desc, nullptr, &staging11)))
        return result;

    D3D12_RESOURCE_DESC desc12 = {};
    desc12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc12.Width = width;
    desc12.Height = height;
    desc12.DepthOrArraySize = 1;
    desc12.MipLevels = 1;
    desc12.Format = format;
    desc12.SampleDesc.Count = 1;
    desc12.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc12.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap12 = {};
    heap12.Type = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
    if (FAILED(device12->CreateCommittedResource(&heap12, D3D12_HEAP_FLAG_NONE, &desc12,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource12))))
        return result;

    UINT64 total_bytes = 0;
    device12->GetCopyableFootprints(&desc12, 0, 1, 0, &result.upload_footprint,
        &result.upload_num_rows, &result.upload_row_bytes, &total_bytes);
    if (total_bytes == 0) return result;

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc = {};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = total_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload12;
    if (FAILED(device12->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload12))))
        return result;

    const D3D12_RANGE no_read = {0, 0};
    void *mapped = nullptr;
    if (FAILED(upload12->Map(0, &no_read, &mapped)))
        return result;

    result.transport = SharedResourceTransport::CpuStaging;
    result.resource12 = resource12;
    result.staging11 = staging11;
    result.upload12 = upload12;
    result.upload_mapped = mapped;
    result.dest_state = D3D12_RESOURCE_STATE_COPY_DEST;
    return result;
}

bool PumpCpuStagingCopy(ID3D11DeviceContext *context11, ID3D11Resource *source11,
    SharedResourceResult &result, ID3D12GraphicsCommandList *upload_list)
{
    if (result.transport != SharedResourceTransport::CpuStaging) return false;
    if (!context11 || !source11 || !upload_list) return false;
    if (!result.staging11 || !result.upload12 || !result.upload_mapped) return false;

    context11->CopyResource(result.staging11.Get(), source11);

    D3D11_MAPPED_SUBRESOURCE mapped11 = {};
    // Map(READ) on an immediate context blocks until the CopyResource above
    // has actually landed, so no separate CPU/GPU sync object is needed here.
    if (FAILED(context11->Map(result.staging11.Get(), 0, D3D11_MAP_READ, 0, &mapped11)))
        return false;

    const auto *src_base = static_cast<const BYTE *>(mapped11.pData);
    auto *dst_base = static_cast<BYTE *>(result.upload_mapped);
    const UINT row_bytes = static_cast<UINT>(result.upload_row_bytes);
    const UINT dst_pitch = result.upload_footprint.Footprint.RowPitch;
    for (UINT row = 0; row < result.upload_num_rows; ++row)
        std::memcpy(dst_base + static_cast<size_t>(row) * dst_pitch,
            src_base + static_cast<size_t>(row) * mapped11.RowPitch, row_bytes);

    context11->Unmap(result.staging11.Get(), 0);

    TransitionSharedResource(upload_list, result, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = result.resource12.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = result.upload12.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = result.upload_footprint;

    upload_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return true;
}

void TransitionSharedResource(ID3D12GraphicsCommandList *list,
    SharedResourceResult &result, D3D12_RESOURCE_STATES after)
{
    if (!list || !result.resource12 || result.dest_state == after) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = result.resource12.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = result.dest_state;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
    result.dest_state = after;
}

} // namespace protoncompat
