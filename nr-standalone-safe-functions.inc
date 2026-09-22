// Compile-checked in isolation (see notes) -- mirrors the structure of the
// 5 original SafeCreate/SafeEvaluate/SafeRelease/SafeCreateFg/SafeEvaluateFg
// functions, just routed through SehGuardedCall instead of __try/__except.

#include "../include/seh-shim.h"

namespace {

struct SafeCreateContext
{
    bool nr;
    NVSDK_NGX_Handle **target;
};

long SafeCreateTrampoline(void *raw)
{
    auto *ctx = static_cast<SafeCreateContext *>(raw);
    const NVSDK_NGX_Result result = g_bridge_create(
        ctx->nr ? g_nr_create : g_sr_create, NeuralCommandList(),
        ctx->nr ? kFeatureDlssNr : NVSDK_NGX_Feature_SuperSampling,
        g_ngx_params, ctx->target);
    return static_cast<long>(result);
}

struct SafeEvaluateContext
{
    bool nr;
    NVSDK_NGX_Handle *handle;
};

long SafeEvaluateTrampoline(void *raw)
{
    auto *ctx = static_cast<SafeEvaluateContext *>(raw);
    const NVSDK_NGX_Result result = g_bridge_evaluate(
        ctx->nr ? g_nr_evaluate : g_sr_evaluate, NeuralCommandList(),
        ctx->handle, g_ngx_params, nullptr);
    return static_cast<long>(result);
}

struct SafeReleaseContext
{
    NgxReleaseFeature release;
    NVSDK_NGX_Handle *handle;
};

long SafeReleaseTrampoline(void *raw)
{
    auto *ctx = static_cast<SafeReleaseContext *>(raw);
    const NVSDK_NGX_Result result = g_bridge_release(ctx->release, ctx->handle);
    return static_cast<long>(result);
}

long SafeCreateFgTrampoline(void * /*unused*/)
{
    const NVSDK_NGX_Result result = g_bridge_create(g_fg_create, NeuralCommandList(),
        NVSDK_NGX_Feature_FrameGeneration, g_ngx_params, &g_fg_feature);
    return static_cast<long>(result);
}

long SafeEvaluateFgTrampoline(void * /*unused*/)
{
    const NVSDK_NGX_Result result = g_bridge_evaluate(g_fg_evaluate, NeuralCommandList(),
        g_fg_feature, g_ngx_params, nullptr);
    return static_cast<long>(result);
}

} // namespace

static NVSDK_NGX_Result SafeCreate(bool nr, DWORD *exception,
    NVSDK_NGX_Handle **nr_target = nullptr)
{
    SafeCreateContext ctx{ nr, nr ? (nr_target ? nr_target : &g_nr_feature) : &g_sr_feature };
    unsigned long code = 0;
    const long result = SehGuardedCall(&SafeCreateTrampoline, &ctx, &code);
    *exception = code;
    return static_cast<NVSDK_NGX_Result>(result);
}

static NVSDK_NGX_Result SafeEvaluate(bool nr, DWORD *exception,
    NVSDK_NGX_Handle *nr_handle = nullptr)
{
    SafeEvaluateContext ctx{ nr, nr ? (nr_handle ? nr_handle : g_nr_feature) : g_sr_feature };
    unsigned long code = 0;
    const long result = SehGuardedCall(&SafeEvaluateTrampoline, &ctx, &code);
    *exception = code;
    return static_cast<NVSDK_NGX_Result>(result);
}

static NVSDK_NGX_Result SafeRelease(NgxReleaseFeature release, NVSDK_NGX_Handle *handle, DWORD *exception)
{
    SafeReleaseContext ctx{ release, handle };
    unsigned long code = 0;
    const long result = SehGuardedCall(&SafeReleaseTrampoline, &ctx, &code);
    *exception = code;
    return static_cast<NVSDK_NGX_Result>(result);
}

static NVSDK_NGX_Result SafeCreateFg(DWORD *exception)
{
    unsigned long code = 0;
    const long result = SehGuardedCall(&SafeCreateFgTrampoline, nullptr, &code);
    *exception = code;
    return static_cast<NVSDK_NGX_Result>(result);
}

static NVSDK_NGX_Result SafeEvaluateFg(DWORD *exception)
{
    unsigned long code = 0;
    const long result = SehGuardedCall(&SafeEvaluateFgTrampoline, nullptr, &code);
    *exception = code;
    return static_cast<NVSDK_NGX_Result>(result);
}
