/*
* Copyright (c) 2026
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "include/sl.h"
#include "include/sl_consts.h"
#include "include/sl_fsr.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.file/file.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.fsr.common/colorConversion.h"
#include "source/plugins/sl.fsr.common/ffxRuntime.h"
#include "source/plugins/sl.fsr/versions.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "external/json/include/nlohmann/json.hpp"
#include "_artifacts/json/fsr_json.h"

using json = nlohmann::json;

namespace sl
{

namespace fsr_plugin
{

struct Viewport
{
    FSROptions options{};
    ffxContext context{};
    uint32_t createFlags{};
    FfxApiDimensions2D maxRenderSize{};
    FfxApiDimensions2D maxUpscaleSize{};
};

struct Context
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(Context);
    void onCreateContext() {}
    void onDestroyContext() {}

    fsr::Runtime runtime{};
    fsr::ProviderVersion provider{};
    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};
    common::ViewportIdFrameData<4, false> options = { "fsr" };
    std::map<uint32_t, Viewport> viewports{};
    ID3D12Device* device{};
};

}

static std::string JSON = std::string(fsr_json, &fsr_json[fsr_json_len]);

void updateEmbeddedJSON(json& config);

SL_PLUGIN_DEFINE("sl.fsr", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, fsr_plugin, Context)

namespace
{

uint32_t getQualityMode(FSRMode mode)
{
    switch (mode)
    {
        case FSRMode::eNativeAA: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
        case FSRMode::eQuality: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
        case FSRMode::eBalanced: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
        case FSRMode::ePerformance: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        case FSRMode::eUltraPerformance: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
        default: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    }
}

FfxApiDimensions2D getDimensions(const CommonResource& resource)
{
    const auto& extent = resource.getExtent();
    if (extent) return { extent.width, extent.height };
    const auto desc = static_cast<ID3D12Resource*>(resource.getNative())->GetDesc();
    return { static_cast<uint32_t>(desc.Width), desc.Height };
}

ID3D12GraphicsCommandList* getNativeCommandList(
    chi::CommandList commandList,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& native)
{
    auto* unknown = static_cast<IUnknown*>(commandList);
    if (unknown && SUCCEEDED(unknown->QueryInterface(
        __uuidof(StreamlineRetrieveBaseInterface),
        reinterpret_cast<void**>(native.ReleaseAndGetAddressOf()))))
    {
        return native.Get();
    }
    return static_cast<ID3D12GraphicsCommandList*>(commandList);
}

uint32_t getCreateFlags(const FSROptions& options, const Constants& constants, const FfxApiDimensions2D& renderSize, const FfxApiDimensions2D& motionSize)
{
    uint32_t flags = fsr::getUpscaleColorCreateFlags(options.colorSpace);
    if (options.useAutoExposure == Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (options.dynamicResolutionEnabled == Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (constants.depthInverted == Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (constants.cameraFar == 0.0f) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (constants.motionVectorsJittered == Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (motionSize.width > renderSize.width || motionSize.height > renderSize.height)
    {
        flags |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    }
    return flags;
}

void destroyViewport(fsr_plugin::Context& ctx, fsr_plugin::Viewport& viewport)
{
    if (viewport.context)
    {
        if (ctx.runtime.destroy(&viewport.context) != FFX_API_RETURN_OK)
        {
            SL_LOG_ERROR("Failed to destroy FSR context");
        }
        viewport.context = {};
    }
}

bool ensureContext(
    fsr_plugin::Context& ctx,
    fsr_plugin::Viewport& viewport,
    const Constants& constants,
    FfxApiDimensions2D renderSize,
    FfxApiDimensions2D motionSize,
    FfxApiDimensions2D outputSize)
{
    const auto& options = viewport.options;
    FfxApiDimensions2D maxRender = {
        options.maxRenderWidth == INVALID_UINT ? renderSize.width : options.maxRenderWidth,
        options.maxRenderHeight == INVALID_UINT ? renderSize.height : options.maxRenderHeight
    };
    FfxApiDimensions2D maxUpscale = {
        options.outputWidth == INVALID_UINT ? outputSize.width : options.outputWidth,
        options.outputHeight == INVALID_UINT ? outputSize.height : options.outputHeight
    };
    if (!renderSize.width || !renderSize.height || !outputSize.width || !outputSize.height ||
        maxRender.width < renderSize.width || maxRender.height < renderSize.height ||
        maxUpscale.width < outputSize.width || maxUpscale.height < outputSize.height)
    {
        return false;
    }
    const uint32_t flags = getCreateFlags(options, constants, renderSize, motionSize);
    if (viewport.context &&
        viewport.createFlags == flags &&
        viewport.maxRenderSize.width == maxRender.width &&
        viewport.maxRenderSize.height == maxRender.height &&
        viewport.maxUpscaleSize.width == maxUpscale.width &&
        viewport.maxUpscaleSize.height == maxUpscale.height)
    {
        return true;
    }

    destroyViewport(ctx, viewport);

    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, ctx.provider.id};
    ffxCreateBackendDX12Desc backend{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, &overrideVersion.header }, ctx.device};
    ffxCreateContextDescUpscaleVersion apiVersion{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &backend.header }, FFX_UPSCALER_VERSION};
    ffxCreateContextDescUpscale create{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &apiVersion.header }, flags, maxRender, maxUpscale};
    if (ctx.runtime.create(&viewport.context, &create.header) != FFX_API_RETURN_OK ||
        !ctx.runtime.validateProvider(viewport.context, ctx.provider))
    {
        destroyViewport(ctx, viewport);
        return false;
    }

    viewport.createFlags = flags;
    viewport.maxRenderSize = maxRender;
    viewport.maxUpscaleSize = maxUpscale;
    return true;
}

Result fsrEvaluate(chi::CommandList commandList, const common::EventData& event, const BaseStructure** inputs, uint32_t numInputs)
{
    auto& ctx = *fsr_plugin::getContext();
    FSROptions* options{};
    Constants* constants{};
    if (!ctx.options.get(event, &options) || options->mode == FSRMode::eOff) return Result::eErrorInvalidState;
    if (!common::getConsts(event, &constants)) return Result::eErrorMissingConstants;
    if (!std::isfinite(constants->cameraNear) || !std::isfinite(constants->cameraFar) ||
        !std::isfinite(constants->cameraFOV) || constants->cameraNear == constants->cameraFar ||
        constants->cameraFOV <= 0.0f || constants->cameraFOV > std::numbers::pi_v<float>)
    {
        SL_LOG_ERROR("FSR requires valid camera near, far, and vertical field-of-view values");
        return Result::eErrorMissingConstants;
    }

    CommonResource color{}, output{}, depth{}, motion{}, exposure{}, reactive{}, transparency{};
    SL_CHECK(getTaggedResource(kBufferTypeScalingInputColor, color, event.frame, event.id, false, inputs, numInputs));
    SL_CHECK(getTaggedResource(kBufferTypeScalingOutputColor, output, event.frame, event.id, false, inputs, numInputs));
    SL_CHECK(getTaggedResource(kBufferTypeDepth, depth, event.frame, event.id, false, inputs, numInputs));
    SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, motion, event.frame, event.id, false, inputs, numInputs));
    getTaggedResource(kBufferTypeExposure, exposure, event.frame, event.id, true, inputs, numInputs);
    getTaggedResource(kBufferTypeReactiveMaskHint, reactive, event.frame, event.id, true, inputs, numInputs);
    getTaggedResource(kBufferTypeTransparencyAndCompositionMaskHint, transparency, event.frame, event.id, true, inputs, numInputs);

    if (color.getState() == UINT_MAX || output.getState() == UINT_MAX ||
        depth.getState() == UINT_MAX || motion.getState() == UINT_MAX ||
        (exposure && exposure.getState() == UINT_MAX) ||
        (reactive && reactive.getState() == UINT_MAX) ||
        (transparency && transparency.getState() == UINT_MAX))
    {
        SL_LOG_ERROR("FSR requires explicit D3D12 resource states");
        return Result::eErrorMissingResourceState;
    }

    const auto renderSize = getDimensions(color);
    const auto outputSize = getDimensions(output);
    const auto motionSize = getDimensions(motion);
    auto& viewport = ctx.viewports[event.id];
    viewport.options = *options;
    if (!ensureContext(ctx, viewport, *constants, renderSize, motionSize, outputSize))
    {
        SL_LOG_ERROR("Failed to create the required FSR 3.1.5 context");
        return Result::eErrorComputeFailed;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> nativeCommandListReference;
    auto* nativeCommandList = getNativeCommandList(commandList, nativeCommandListReference);
    if (!nativeCommandList) return Result::eErrorMissingInputParameter;

    ffxDispatchDescUpscale dispatch{{ FFX_API_DISPATCH_DESC_TYPE_UPSCALE }};
    dispatch.commandList = nativeCommandList;
    dispatch.color = fsr::getResource(static_cast<ID3D12Resource*>(color.getNative()), static_cast<D3D12_RESOURCE_STATES>(color.getState()));
    dispatch.output = fsr::getResource(static_cast<ID3D12Resource*>(output.getNative()), static_cast<D3D12_RESOURCE_STATES>(output.getState()));
    dispatch.depth = fsr::getResource(static_cast<ID3D12Resource*>(depth.getNative()), static_cast<D3D12_RESOURCE_STATES>(depth.getState()));
    dispatch.motionVectors = fsr::getResource(static_cast<ID3D12Resource*>(motion.getNative()), static_cast<D3D12_RESOURCE_STATES>(motion.getState()));
    if (exposure) dispatch.exposure = fsr::getResource(static_cast<ID3D12Resource*>(exposure.getNative()), static_cast<D3D12_RESOURCE_STATES>(exposure.getState()));
    if (reactive) dispatch.reactive = fsr::getResource(static_cast<ID3D12Resource*>(reactive.getNative()), static_cast<D3D12_RESOURCE_STATES>(reactive.getState()));
    if (transparency) dispatch.transparencyAndComposition = fsr::getResource(static_cast<ID3D12Resource*>(transparency.getNative()), static_cast<D3D12_RESOURCE_STATES>(transparency.getState()));
    dispatch.jitterOffset = { -constants->jitterOffset.x, -constants->jitterOffset.y };
    dispatch.motionVectorScale = { constants->mvecScale.x * renderSize.width, constants->mvecScale.y * renderSize.height };
    dispatch.renderSize = renderSize;
    dispatch.upscaleSize = outputSize;
    dispatch.enableSharpening = options->sharpness > 0.0f;
    dispatch.sharpness = std::clamp(options->sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = options->frameTimeDeltaMilliseconds;
    dispatch.preExposure = options->preExposure;
    dispatch.reset = constants->reset == Boolean::eTrue;
    dispatch.cameraNear = constants->cameraNear;
    dispatch.cameraFar = constants->cameraFar;
    dispatch.cameraFovAngleVertical = constants->cameraFOV;
    dispatch.viewSpaceToMetersFactor = options->viewSpaceToMetersFactor;
    dispatch.flags = fsr::getUpscaleColorDispatchFlags(options->colorSpace);

    const auto result = ctx.runtime.dispatch(&viewport.context, &dispatch.header);
    if (result != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("FSR 3.1.5 dispatch failed");
        return Result::eErrorComputeFailed;
    }
    return Result::eOk;
}

}

Result slSetData(const BaseStructure* inputs, CommandBuffer*)
{
    auto* options = findStruct<const FSROptions>(inputs);
    auto* viewport = findStruct<const ViewportHandle>(inputs);
    if (!options || !viewport || options->mode >= FSRMode::eCount ||
        options->colorSpace >= FSRColorSpace::eCount ||
        !std::isfinite(options->sharpness) ||
        !std::isfinite(options->preExposure) || options->preExposure == 0.0f ||
        !std::isfinite(options->frameTimeDeltaMilliseconds) || options->frameTimeDeltaMilliseconds < 0.0f ||
        !std::isfinite(options->viewSpaceToMetersFactor) || options->viewSpaceToMetersFactor <= 0.0f)
    {
        return Result::eErrorInvalidParameter;
    }
    fsr_plugin::getContext()->options.set(0, *viewport, options);
    if (options->mode == FSRMode::eOff)
    {
        auto& ctx = *fsr_plugin::getContext();
        auto it = ctx.viewports.find(*viewport);
        if (it != ctx.viewports.end())
        {
            destroyViewport(ctx, it->second);
            ctx.viewports.erase(it);
        }
    }
    return Result::eOk;
}

Result slGetData(const BaseStructure* inputs, BaseStructure* output, CommandBuffer*)
{
    auto& ctx = *fsr_plugin::getContext();
    if (auto* settings = findStruct<FSROptimalSettings>(output))
    {
        auto* options = findStruct<const FSROptions>(inputs);
        if (!options || options->mode == FSRMode::eOff || options->mode >= FSRMode::eCount ||
            options->outputWidth == 0 || options->outputWidth == INVALID_UINT ||
            options->outputHeight == 0 || options->outputHeight == INVALID_UINT)
        {
            return Result::eErrorInvalidParameter;
        }
        ffxOverrideVersion overrideVersion{
            { FFX_API_DESC_TYPE_OVERRIDE_VERSION },
            ctx.provider.id
        };
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{
            {
                FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE,
                &overrideVersion.header
            },
            options->outputWidth,
            options->outputHeight,
            getQualityMode(options->mode),
            &settings->optimalRenderWidth,
            &settings->optimalRenderHeight
        };
        if (ctx.runtime.query(nullptr, &query.header) != FFX_API_RETURN_OK)
        {
            return Result::eErrorFeatureNotSupported;
        }
        settings->renderWidthMin = settings->optimalRenderWidth;
        settings->renderHeightMin = settings->optimalRenderHeight;
        settings->renderWidthMax = options->outputWidth;
        settings->renderHeightMax = options->outputHeight;
        return Result::eOk;
    }
    if (auto* state = findStruct<FSRState>(output))
    {
        auto* viewport = findStruct<const ViewportHandle>(inputs);
        if (!viewport) return Result::eErrorMissingInputParameter;
        state->providerVersionMajor = ctx.provider.major;
        state->providerVersionMinor = ctx.provider.minor;
        state->providerVersionPatch = ctx.provider.patch;
        state->estimatedVRAMUsageInBytes = 0;
        auto it = ctx.viewports.find(*viewport);
        if (it != ctx.viewports.end() && it->second.context)
        {
            FfxApiEffectMemoryUsage memory{};
            ffxQueryDescUpscaleGetGPUMemoryUsage query{{ FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE }, &memory};
            if (ctx.runtime.query(&it->second.context, &query.header) == FFX_API_RETURN_OK)
            {
                state->estimatedVRAMUsageInBytes = memory.totalUsageInBytes;
            }
        }
        return Result::eOk;
    }
    return Result::eErrorMissingInputParameter;
}

Result slAllocateResources(CommandBuffer*, Feature feature, const ViewportHandle&)
{
    return feature == kFeatureFSR ? Result::eOk : Result::eErrorInvalidParameter;
}

Result slFreeResources(Feature feature, const ViewportHandle& viewport)
{
    if (feature != kFeatureFSR) return Result::eErrorInvalidParameter;
    auto& ctx = *fsr_plugin::getContext();
    auto it = ctx.viewports.find(viewport);
    if (it == ctx.viewports.end()) return Result::eOk;
    destroyViewport(ctx, it->second);
    ctx.viewports.erase(it);
    return Result::eOk;
}

bool slOnPluginStartup(const char* jsonConfig, void* device)
{
    SL_PLUGIN_COMMON_STARTUP();
    auto& ctx = *fsr_plugin::getContext();
    ctx.device = static_cast<ID3D12Device*>(device);
    if (!ctx.device || !ctx.runtime.initialize(file::getModulePath(), true, false) ||
        !ctx.runtime.selectProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ctx.device, "3.1.5", ctx.provider))
    {
        return false;
    }
    if (!param::getPointerParam(api::getContext()->parameters, param::common::kPFunRegisterEvaluateCallbacks, &ctx.registerEvaluateCallbacks))
    {
        return false;
    }
    ctx.registerEvaluateCallbacks(kFeatureFSR, fsrEvaluate, nullptr);
    return true;
}

void slOnPluginShutdown()
{
    auto& ctx = *fsr_plugin::getContext();
    if (ctx.registerEvaluateCallbacks) ctx.registerEvaluateCallbacks(kFeatureFSR, nullptr, nullptr);
    for (auto& [id, viewport] : ctx.viewports) destroyViewport(ctx, viewport);
    ctx.viewports.clear();
    ctx.runtime.shutdown();
    plugin::onShutdown(api::getContext());
}

sl::Result slFSRGetOptimalSettings(const FSROptions& options, FSROptimalSettings& settings)
{
    return slGetData(&options, &settings, nullptr);
}

sl::Result slFSRGetState(const ViewportHandle& viewport, FSRState& state)
{
    return slGetData(&viewport, &state, nullptr);
}

sl::Result slFSRSetOptions(const ViewportHandle& viewport, const FSROptions& options)
{
    auto input = viewport;
    input.next = const_cast<FSROptions*>(&options);
    return slSetData(&input, nullptr);
}

sl::Result slIsSupported(const AdapterInfo&)
{
    return fsr_plugin::getContext()->runtime ? Result::eOk : Result::eErrorFeatureNotSupported;
}

void updateEmbeddedJSON(json& config)
{
    common::PFunUpdateCommonEmbeddedJSONConfig* update{};
    param::getPointerParam(api::getContext()->parameters, param::common::kPFunUpdateCommonEmbeddedJSONConfig, &update);
    if (!update) return;
    common::PluginInfo info{};
    info.minOS = Version(10, 0, 0);
    info.requiredTags = {
        { kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate },
        { kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate },
        { kBufferTypeDepth, ResourceLifecycle::eValidUntilEvaluate },
        { kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilEvaluate }
    };
    update(&config, info);
}

SL_EXPORT void* slGetPluginFunction(const char* functionName)
{
    SL_EXPORT_FUNCTION(slOnPluginLoad);
    SL_EXPORT_FUNCTION(slOnPluginShutdown);
    SL_EXPORT_FUNCTION(slOnPluginStartup);
    SL_EXPORT_FUNCTION(slSetData);
    SL_EXPORT_FUNCTION(slGetData);
    SL_EXPORT_FUNCTION(slAllocateResources);
    SL_EXPORT_FUNCTION(slFreeResources);
    SL_EXPORT_FUNCTION(slIsSupported);
    SL_EXPORT_FUNCTION(slFSRGetOptimalSettings);
    SL_EXPORT_FUNCTION(slFSRGetState);
    SL_EXPORT_FUNCTION(slFSRSetOptions);
    return nullptr;
}

}
