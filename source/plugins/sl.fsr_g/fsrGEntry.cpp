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
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "include/sl.h"
#include "include/sl_consts.h"
#include "include/sl_fsr_g.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.file/file.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.fsr.common/colorConversion.h"
#include "source/plugins/sl.fsr.common/colorConversionD3D12.h"
#include "source/plugins/sl.fsr.common/ffxRuntime.h"
#include "source/plugins/sl.fsr.common/providerCapabilities.h"
#include "source/plugins/sl.fsr_g/versions.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "external/json/include/nlohmann/json.hpp"
#include "_artifacts/json/fsr_g_json.h"
#include "_artifacts/gitVersion.h"

using json = nlohmann::json;

namespace sl
{

namespace fsr_g
{

struct ConversionFrame
{
    uint64_t frameID = ~0ull;
    fsr::LinearColorTexture presentColor{};
    fsr::LinearColorTexture hudlessColor{};
    fsr::LinearColorTexture outputs[4]{};
    Microsoft::WRL::ComPtr<ID3D12Fence> completionFence{};
    uint64_t completionValue{};
};

struct Viewport
{
    FSRGOptions options{};
    FSRGAlgorithm algorithm = FSRGAlgorithm::eFSR3;
    ffxContext context{};
    uint32_t createFlags{};
    FfxApiDimensions2D maxRenderSize{};
    FfxApiDimensions2D displaySize{};
    uint32_t backBufferFormat{};
    std::mutex conversionFramesMutex{};
    std::vector<std::unique_ptr<ConversionFrame>> conversionFrames{};
};

struct Context
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(Context);
    void onCreateContext() {}
    void onDestroyContext() {}

    fsr::Runtime legacyRuntime{};
    fsr::Runtime mlRuntime{};
    fsr::ProviderVersion fsr3FrameGenerationProvider{};
    fsr::ProviderVersion fsr4FrameGenerationProvider{};
    FSRUnavailableReason fsr4UnavailableReason =
        FSRUnavailableReason::eModuleUnavailable;
    fsr::ProviderVersion swapchainProvider{};
    fsr::ProviderVersion swapchainHwndProvider{};
    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};
    common::ViewportIdFrameData<4, false> options = { "fsr_g" };
    std::map<uint32_t, Viewport> viewports{};
    ID3D12Device* device{};
    ID3D12CommandQueue* gameQueue{};
    IDXGISwapChain4* swapchain{};
    ffxContext swapchainContext{};
    chi::ICompute* compute{};
    fsr::ColorConversionD3D12 colorConversion{};
    std::mutex mutex{};
};

}

static std::string JSON = std::string(fsr_g_json, &fsr_g_json[fsr_g_json_len]);

void updateEmbeddedJSON(json& config);

SL_PLUGIN_DEFINE("sl.fsr_g", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, fsr_g, Context)

namespace
{

bool finite3(const float3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool nonzero3(const float3& value)
{
    return value.x * value.x + value.y * value.y + value.z * value.z > 0.0f;
}

fsr::Runtime& getAlgorithmRuntime(
    fsr_g::Context& ctx,
    FSRGAlgorithm algorithm)
{
    return algorithm == FSRGAlgorithm::eFSR4 ?
        ctx.mlRuntime :
        ctx.legacyRuntime;
}

const fsr::ProviderVersion& getAlgorithmProvider(
    const fsr_g::Context& ctx,
    FSRGAlgorithm algorithm)
{
    return algorithm == FSRGAlgorithm::eFSR4 ?
        ctx.fsr4FrameGenerationProvider :
        ctx.fsr3FrameGenerationProvider;
}

FSRUnavailableReason getUnavailableReason(
    const fsr_g::Context& ctx,
    FSRGAlgorithm algorithm)
{
    return algorithm == FSRGAlgorithm::eFSR4 ?
        ctx.fsr4UnavailableReason :
        FSRUnavailableReason::eNone;
}

bool usesGamma22Adapter(
    FSRGAlgorithm algorithm,
    FSRColorSpace colorSpace)
{
    return algorithm == FSRGAlgorithm::eFSR4 &&
        colorSpace == FSRColorSpace::eGamma22;
}

FfxApiDimensions2D getDimensions(const CommonResource& resource)
{
    const auto& extent = resource.getExtent();
    if (extent) return { extent.width, extent.height };
    const auto desc = static_cast<ID3D12Resource*>(resource.getNative())->GetDesc();
    return { static_cast<uint32_t>(desc.Width), desc.Height };
}

uint64_t getLinearTextureBytes(const fsr::LinearColorTexture& texture)
{
    return texture.resource ?
        static_cast<uint64_t>(texture.width) * texture.height * 8 :
        0;
}

uint64_t getConversionFrameBytes(
    const fsr_g::ConversionFrame& frame)
{
    uint64_t bytes =
        getLinearTextureBytes(frame.presentColor) +
        getLinearTextureBytes(frame.hudlessColor);
    for (const auto& output : frame.outputs)
    {
        bytes += getLinearTextureBytes(output);
    }
    return bytes;
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

uint32_t getCreateFlags(const Constants& constants, FfxApiDimensions2D renderSize, FfxApiDimensions2D motionSize)
{
    uint32_t flags{ FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT };
    if (constants.depthInverted == Boolean::eTrue) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (constants.cameraFar == 0.0f) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (constants.motionVectorsJittered == Boolean::eTrue) flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (motionSize.width > renderSize.width || motionSize.height > renderSize.height)
    {
        flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    }
    return flags;
}

void waitForPresents(fsr_g::Context& ctx)
{
    if (!ctx.swapchainContext) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{{ FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 }};
    if (ctx.legacyRuntime.dispatch(
            &ctx.swapchainContext,
            &wait.header) != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("FidelityFX frame-generation swapchain wait failed");
    }
}

void releaseConversionFrame(
    fsr_g::Context& ctx,
    fsr_g::ConversionFrame& frame)
{
    ctx.colorConversion.release(frame.presentColor, 0);
    ctx.colorConversion.release(frame.hudlessColor, 0);
    for (auto& output : frame.outputs)
    {
        ctx.colorConversion.release(output, 0);
    }
    frame = {};
}

void releaseConversionFrames(
    fsr_g::Context& ctx,
    fsr_g::Viewport& viewport)
{
    std::scoped_lock lock(viewport.conversionFramesMutex);
    for (auto& frame : viewport.conversionFrames)
    {
        releaseConversionFrame(ctx, *frame);
    }
    viewport.conversionFrames.clear();
}

void destroyViewport(fsr_g::Context& ctx, fsr_g::Viewport& viewport)
{
    if (viewport.context)
    {
        ffxConfigureDescFrameGeneration disable{{ FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION }};
        disable.swapChain = ctx.swapchain;
        disable.frameGenerationEnabled = false;
        disable.allowAsyncWorkloads = false;
        auto& runtime = getAlgorithmRuntime(ctx, viewport.algorithm);
        runtime.configure(&viewport.context, &disable.header);
        waitForPresents(ctx);
        if (runtime.destroy(&viewport.context) != FFX_API_RETURN_OK)
        {
            SL_LOG_ERROR("Failed to destroy FSR frame-generation context");
        }
        viewport.context = {};
    }
    releaseConversionFrames(ctx, viewport);
}

void destroySwapchainContext(fsr_g::Context& ctx)
{
    if (!ctx.swapchainContext) return;
    waitForPresents(ctx);
    if (ctx.legacyRuntime.destroy(&ctx.swapchainContext) != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("Failed to destroy FSR frame-generation swapchain context");
    }
    ctx.swapchainContext = {};
    ctx.swapchain = {};
    ctx.gameQueue = {};
}

bool ensureFrameGenerationContext(
    fsr_g::Context& ctx,
    fsr_g::Viewport& viewport,
    const Constants& constants,
    FfxApiDimensions2D renderSize,
    FfxApiDimensions2D motionSize)
{
    const auto& options = viewport.options;
    auto& runtime = getAlgorithmRuntime(ctx, viewport.algorithm);
    const auto& provider = getAlgorithmProvider(ctx, viewport.algorithm);
    FfxApiDimensions2D displaySize{ options.displayWidth, options.displayHeight };
    FfxApiDimensions2D maxRenderSize{
        options.maxRenderWidth == INVALID_UINT ? renderSize.width : options.maxRenderWidth,
        options.maxRenderHeight == INVALID_UINT ? renderSize.height : options.maxRenderHeight
    };
    if (!renderSize.width || !renderSize.height ||
        maxRenderSize.width < renderSize.width || maxRenderSize.height < renderSize.height)
    {
        return false;
    }
    const uint32_t flags = getCreateFlags(constants, renderSize, motionSize);
    const auto providerBackBufferFormat =
        usesGamma22Adapter(viewport.algorithm, options.colorSpace) ?
            DXGI_FORMAT_R16G16B16A16_FLOAT :
            static_cast<DXGI_FORMAT>(options.backBufferFormat);
    const uint32_t backBufferFormat =
        ffxApiGetSurfaceFormatDX12(providerBackBufferFormat);
    if (viewport.context &&
        viewport.createFlags == flags &&
        viewport.displaySize.width == displaySize.width &&
        viewport.displaySize.height == displaySize.height &&
        viewport.maxRenderSize.width == maxRenderSize.width &&
        viewport.maxRenderSize.height == maxRenderSize.height &&
        viewport.backBufferFormat == backBufferFormat)
    {
        return true;
    }

    destroyViewport(ctx, viewport);
    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, provider.id};
    ffxCreateBackendDX12Desc backend{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, &overrideVersion.header }, ctx.device};
    ffxCreateContextDescFrameGenerationVersion apiVersion{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &backend.header }, FFX_FRAMEGENERATION_VERSION};
    ffxCreateContextDescFrameGeneration create{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &apiVersion.header }, flags, displaySize, maxRenderSize, backBufferFormat};
    if (runtime.create(&viewport.context, &create.header) != FFX_API_RETURN_OK ||
        !runtime.validateProvider(viewport.context, provider))
    {
        destroyViewport(ctx, viewport);
        return false;
    }
    viewport.createFlags = flags;
    viewport.displaySize = displaySize;
    viewport.maxRenderSize = maxRenderSize;
    viewport.backBufferFormat = backBufferFormat;
    return true;
}

void updateConversionCompletions(
    fsr_g::Context& ctx,
    fsr_g::Viewport& viewport,
    uint64_t currentFrameID)
{
    if (!ctx.swapchainContext || viewport.conversionFrames.empty()) return;
    ID3D12Fence* rawFence{};
    uint64_t fenceValue{};
    ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1 completion{
        { FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 },
        reinterpret_cast<void**>(&rawFence),
        &fenceValue
    };
    const auto result = ctx.legacyRuntime.query(
        &ctx.swapchainContext,
        &completion.header);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    fence.Attach(rawFence);
    if (result != FFX_API_RETURN_OK || !fence || !fenceValue) return;
    for (auto& frame : viewport.conversionFrames)
    {
        if (frame->frameID != ~0ull &&
            frame->frameID != currentFrameID &&
            !frame->completionValue)
        {
            frame->completionFence = fence;
            frame->completionValue = fenceValue;
        }
    }
}

bool isReusable(const fsr_g::ConversionFrame& frame)
{
    return frame.frameID == ~0ull ||
        (frame.completionFence &&
         frame.completionValue &&
         frame.completionFence->GetCompletedValue() >=
             frame.completionValue);
}

fsr_g::ConversionFrame* acquireConversionFrame(
    fsr_g::Context& ctx,
    fsr_g::Viewport& viewport,
    uint64_t frameID,
    uint32_t width,
    uint32_t height,
    bool needsHudlessColor)
{
    std::scoped_lock lock(viewport.conversionFramesMutex);
    updateConversionCompletions(ctx, viewport, frameID);
    fsr_g::ConversionFrame* selected{};
    for (auto& frame : viewport.conversionFrames)
    {
        if (frame->frameID == frameID)
        {
            selected = frame.get();
            break;
        }
        if (!selected && isReusable(*frame))
        {
            selected = frame.get();
        }
    }
    if (!selected)
    {
        auto frame = std::make_unique<fsr_g::ConversionFrame>();
        selected = frame.get();
        viewport.conversionFrames.emplace_back(std::move(frame));
    }
    selected->frameID = frameID;
    selected->completionFence.Reset();
    selected->completionValue = 0;
    if (!ctx.colorConversion.ensureLinearTexture(
            selected->presentColor,
            width,
            height,
            "sl.fsr_g.mlfg.linear_present"))
    {
        return nullptr;
    }
    for (auto& output : selected->outputs)
    {
        if (!ctx.colorConversion.ensureLinearTexture(
                output,
                width,
                height,
                "sl.fsr_g.mlfg.linear_output"))
        {
            return nullptr;
        }
    }
    if (needsHudlessColor &&
        !ctx.colorConversion.ensureLinearTexture(
            selected->hudlessColor,
            width,
            height,
            "sl.fsr_g.mlfg.linear_hudless"))
    {
        return nullptr;
    }
    return selected;
}

fsr_g::ConversionFrame* findConversionFrame(
    fsr_g::Viewport& viewport,
    uint64_t frameID)
{
    for (auto& frame : viewport.conversionFrames)
    {
        if (frame->frameID == frameID) return frame.get();
    }
    return nullptr;
}

ffxReturnCode_t frameGenerationCallback(ffxDispatchDescFrameGeneration* desc, void* user)
{
    auto* viewport = static_cast<fsr_g::Viewport*>(user);
    if (!viewport || !viewport->context) return FFX_API_RETURN_ERROR_PARAMETER;
    auto& ctx = *fsr_g::getContext();
    const bool convertGamma22 =
        usesGamma22Adapter(
            viewport->algorithm,
            viewport->options.colorSpace);
    if (convertGamma22 && !ctx.colorConversion)
    {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    std::unique_lock conversionLock(
        viewport->conversionFramesMutex,
        std::defer_lock);
    if (convertGamma22) conversionLock.lock();
    FfxApiResource originalPresentColor{};
    FfxApiResource originalOutputs[4]{};
    fsr_g::ConversionFrame* conversionFrame{};
    if (convertGamma22)
    {
        conversionFrame =
            findConversionFrame(*viewport, desc->frameID);
        if (!conversionFrame ||
            desc->numGeneratedFrames > std::size(originalOutputs))
        {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        originalPresentColor = desc->presentColor;
        std::copy_n(
            desc->outputs,
            desc->numGeneratedFrames,
            originalOutputs);
        if (!ctx.colorConversion.decodeGamma22(
                desc->commandList,
                static_cast<ID3D12Resource*>(
                    originalPresentColor.resource),
                fsr::getD3D12ResourceState(
                    originalPresentColor.state),
                conversionFrame->presentColor,
                viewport->displaySize.width,
                viewport->displaySize.height))
        {
            return FFX_API_RETURN_ERROR;
        }
        desc->presentColor = fsr::getResource(
            static_cast<ID3D12Resource*>(
                conversionFrame->presentColor.resource->native),
            static_cast<D3D12_RESOURCE_STATES>(
                conversionFrame->presentColor.resource->state));
        for (uint32_t i = 0; i < desc->numGeneratedFrames; ++i)
        {
            desc->outputs[i] = fsr::getResource(
                static_cast<ID3D12Resource*>(
                    conversionFrame->outputs[i].resource->native),
                static_cast<D3D12_RESOURCE_STATES>(
                    conversionFrame->outputs[i].resource->state));
        }
    }
    const auto providerColorSpace =
        convertGamma22 ? FSRColorSpace::eLinear : viewport->options.colorSpace;
    if (!fsr::getFrameGenerationTransferFunction(
            providerColorSpace,
            viewport->algorithm == FSRGAlgorithm::eFSR3,
            desc->backbufferTransferFunction))
    {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const auto result =
        getAlgorithmRuntime(ctx, viewport->algorithm).dispatch(
        &viewport->context,
        &desc->header);
    if (convertGamma22)
    {
        desc->presentColor = originalPresentColor;
        std::copy_n(
            originalOutputs,
            desc->numGeneratedFrames,
            desc->outputs);
        if (result != FFX_API_RETURN_OK) return result;
        for (uint32_t i = 0; i < desc->numGeneratedFrames; ++i)
        {
            if (!ctx.colorConversion.encodeGamma22(
                    desc->commandList,
                    conversionFrame->outputs[i],
                    static_cast<ID3D12Resource*>(
                        originalOutputs[i].resource),
                    fsr::getD3D12ResourceState(
                        originalOutputs[i].state),
                    viewport->displaySize.width,
                    viewport->displaySize.height))
            {
                return FFX_API_RETURN_ERROR;
            }
        }
    }
    return result;
}

Result fsrGPrepare(chi::CommandList commandList, const common::EventData& event, const BaseStructure** inputs, uint32_t numInputs)
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    FSRGOptions* options{};
    FSRGAlgorithmOptions* algorithmOptions{};
    Constants* constants{};
    if (!ctx.options.get(event, &options, &algorithmOptions) ||
        !algorithmOptions ||
        options->mode == FSRGMode::eOff)
    {
        return Result::eErrorInvalidState;
    }
    const bool convertGamma22 =
        usesGamma22Adapter(
            algorithmOptions->algorithm,
            options->colorSpace);
    if (convertGamma22 && !ctx.colorConversion)
    {
        return Result::eErrorFeatureNotSupported;
    }
    if (!ctx.swapchain || !ctx.swapchainContext) return Result::eErrorNotInitialized;
    if (!common::getConsts(event, &constants)) return Result::eErrorMissingConstants;
    if (!std::isfinite(constants->cameraNear) || !std::isfinite(constants->cameraFar) ||
        !std::isfinite(constants->cameraFOV) || constants->cameraNear == constants->cameraFar ||
        constants->cameraFOV <= 0.0f || constants->cameraFOV > std::numbers::pi_v<float> ||
        !finite3(constants->cameraPos) || !finite3(constants->cameraUp) ||
        !finite3(constants->cameraRight) || !finite3(constants->cameraFwd) ||
        !nonzero3(constants->cameraUp) || !nonzero3(constants->cameraRight) ||
        !nonzero3(constants->cameraFwd))
    {
        SL_LOG_ERROR("FSR frame generation requires complete finite camera data");
        return Result::eErrorMissingConstants;
    }

    CommonResource depth{}, motion{}, hudless{}, ui{};
    SL_CHECK(getTaggedResource(kBufferTypeDepth, depth, event.frame, event.id, false, inputs, numInputs));
    SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, motion, event.frame, event.id, false, inputs, numInputs));
    getTaggedResource(kBufferTypeHUDLessColor, hudless, event.frame, event.id, true, inputs, numInputs);
    getTaggedResource(kBufferTypeUIColorAndAlpha, ui, event.frame, event.id, true, inputs, numInputs);
    if (depth.getState() == UINT_MAX || motion.getState() == UINT_MAX ||
        (hudless && hudless.getState() == UINT_MAX) ||
        (ui && ui.getState() == UINT_MAX))
    {
        return Result::eErrorMissingResourceState;
    }

    const auto renderSize = getDimensions(depth);
    const auto motionSize = getDimensions(motion);
    auto& viewport = ctx.viewports[event.id];
    if (viewport.context && viewport.algorithm != algorithmOptions->algorithm)
    {
        destroyViewport(ctx, viewport);
    }
    viewport.options = *options;
    viewport.algorithm = algorithmOptions->algorithm;
    if (!ensureFrameGenerationContext(ctx, viewport, *constants, renderSize, motionSize))
    {
        const auto& provider =
            getAlgorithmProvider(ctx, viewport.algorithm);
        SL_LOG_ERROR(
            "Failed to create the required FSR frame-generation %u.%u.%u context",
            provider.major,
            provider.minor,
            provider.patch);
        return Result::eErrorComputeFailed;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> nativeCommandListReference;
    auto* nativeCommandList = getNativeCommandList(commandList, nativeCommandListReference);
    if (!nativeCommandList) return Result::eErrorMissingInputParameter;

    fsr_g::ConversionFrame* conversionFrame{};
    if (convertGamma22)
    {
        conversionFrame = acquireConversionFrame(
            ctx,
            viewport,
            event.frame,
            options->displayWidth,
            options->displayHeight,
            static_cast<bool>(hudless));
        if (!conversionFrame ||
            (hudless &&
             !ctx.colorConversion.decodeGamma22(
                 commandList,
                 static_cast<ID3D12Resource*>(hudless.getNative()),
                 static_cast<D3D12_RESOURCE_STATES>(hudless.getState()),
                 conversionFrame->hudlessColor,
                 options->displayWidth,
                 options->displayHeight)))
        {
            return Result::eErrorComputeFailed;
        }
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{
        { FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 },
        ui ? fsr::getResource(static_cast<ID3D12Resource*>(ui.getNative()), static_cast<D3D12_RESOURCE_STATES>(ui.getState())) : FfxApiResource{},
        0
    };
    if (ctx.legacyRuntime.configure(
            &ctx.swapchainContext,
            &uiConfig.header) != FFX_API_RETURN_OK)
    {
        return Result::eErrorComputeFailed;
    }

    ffxConfigureDescFrameGeneration configure{{ FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION }};
    configure.swapChain = ctx.swapchain;
    configure.frameGenerationCallback = frameGenerationCallback;
    configure.frameGenerationCallbackUserContext = &viewport;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = true;
    if (hudless)
    {
        configure.HUDLessColor = convertGamma22 ?
            fsr::getResource(
                static_cast<ID3D12Resource*>(
                    conversionFrame->hudlessColor.resource->native),
                static_cast<D3D12_RESOURCE_STATES>(
                    conversionFrame->hudlessColor.resource->state)) :
            fsr::getResource(
                static_cast<ID3D12Resource*>(hudless.getNative()),
                static_cast<D3D12_RESOURCE_STATES>(hudless.getState()));
    }
    configure.onlyPresentGenerated = options->onlyPresentGenerated == Boolean::eTrue;
    configure.generationRect = { 0, 0, static_cast<int32_t>(options->displayWidth), static_cast<int32_t>(options->displayHeight) };
    configure.frameID = event.frame;
    if (getAlgorithmRuntime(ctx, viewport.algorithm).configure(
            &viewport.context,
            &configure.header) != FFX_API_RETURN_OK)
    {
        return Result::eErrorComputeFailed;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prepare{{ FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 }};
    prepare.frameID = event.frame;
    prepare.commandList = nativeCommandList;
    prepare.renderSize = renderSize;
    prepare.jitterOffset = { -constants->jitterOffset.x, -constants->jitterOffset.y };
    prepare.motionVectorScale = { constants->mvecScale.x * renderSize.width, constants->mvecScale.y * renderSize.height };
    prepare.frameTimeDelta = options->frameTimeDeltaMilliseconds;
    prepare.reset = constants->reset == Boolean::eTrue;
    prepare.cameraNear = constants->cameraNear;
    prepare.cameraFar = constants->cameraFar;
    prepare.cameraFovAngleVertical = constants->cameraFOV;
    prepare.viewSpaceToMetersFactor = options->viewSpaceToMetersFactor;
    prepare.depth = fsr::getResource(static_cast<ID3D12Resource*>(depth.getNative()), static_cast<D3D12_RESOURCE_STATES>(depth.getState()));
    prepare.motionVectors = fsr::getResource(static_cast<ID3D12Resource*>(motion.getNative()), static_cast<D3D12_RESOURCE_STATES>(motion.getState()));
    prepare.cameraPosition[0] = constants->cameraPos.x;
    prepare.cameraPosition[1] = constants->cameraPos.y;
    prepare.cameraPosition[2] = constants->cameraPos.z;
    prepare.cameraUp[0] = constants->cameraUp.x;
    prepare.cameraUp[1] = constants->cameraUp.y;
    prepare.cameraUp[2] = constants->cameraUp.z;
    prepare.cameraRight[0] = constants->cameraRight.x;
    prepare.cameraRight[1] = constants->cameraRight.y;
    prepare.cameraRight[2] = constants->cameraRight.z;
    prepare.cameraForward[0] = constants->cameraFwd.x;
    prepare.cameraForward[1] = constants->cameraFwd.y;
    prepare.cameraForward[2] = constants->cameraFwd.z;
    const auto result =
        getAlgorithmRuntime(ctx, viewport.algorithm).dispatch(
            &viewport.context,
            &prepare.header);
    if (result != FFX_API_RETURN_OK)
    {
        return Result::eErrorComputeFailed;
    }
    return Result::eOk;
}

HRESULT createSwapchain(
    IDXGIFactory* factory,
    ID3D12CommandQueue* queue,
    DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** output)
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    if (ctx.swapchainContext) return DXGI_ERROR_INVALID_CALL;

    IDXGISwapChain4* swapchain{};
    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, ctx.swapchainProvider.id};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 apiVersion{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12, &overrideVersion.header },
        FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION
    };
    ffxCreateContextDescFrameGenerationSwapChainNewDX12 create{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12, &apiVersion.header },
        &swapchain,
        desc,
        factory,
        queue
    };
    if (ctx.legacyRuntime.create(
            &ctx.swapchainContext,
            &create.header) != FFX_API_RETURN_OK ||
        !ctx.legacyRuntime.validateProvider(
            ctx.swapchainContext,
            ctx.swapchainProvider))
    {
        destroySwapchainContext(ctx);
        return E_FAIL;
    }
    ctx.swapchain = swapchain;
    ctx.gameQueue = queue;
    const HRESULT result = swapchain->QueryInterface(IID_PPV_ARGS(output));
    swapchain->Release();
    return result;
}

HRESULT createSwapchainForHwnd(
    IDXGIFactory2* factory,
    ID3D12CommandQueue* queue,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    IDXGISwapChain1** output)
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    if (ctx.swapchainContext) return DXGI_ERROR_INVALID_CALL;

    IDXGISwapChain4* swapchain{};
    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, ctx.swapchainHwndProvider.id};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 apiVersion{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12, &overrideVersion.header },
        FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION
    };
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, &apiVersion.header },
        &swapchain,
        hwnd,
        const_cast<DXGI_SWAP_CHAIN_DESC1*>(desc),
        const_cast<DXGI_SWAP_CHAIN_FULLSCREEN_DESC*>(fullscreenDesc),
        factory,
        queue
    };
    if (ctx.legacyRuntime.create(
            &ctx.swapchainContext,
            &create.header) != FFX_API_RETURN_OK ||
        !ctx.legacyRuntime.validateProvider(
            ctx.swapchainContext,
            ctx.swapchainHwndProvider))
    {
        destroySwapchainContext(ctx);
        return E_FAIL;
    }
    ctx.swapchain = swapchain;
    ctx.gameQueue = queue;
    const HRESULT result = swapchain->QueryInterface(IID_PPV_ARGS(output));
    swapchain->Release();
    return result;
}

}

Result slSetData(const BaseStructure* inputs, CommandBuffer*)
{
    auto* options = findStruct<const FSRGOptions>(inputs);
    auto* algorithmOptions =
        findStruct<const FSRGAlgorithmOptions>(inputs);
    auto* viewport = findStruct<const ViewportHandle>(inputs);
    FSRGAlgorithmOptions defaultAlgorithm{};
    if (!algorithmOptions) algorithmOptions = &defaultAlgorithm;
    if (!options || !viewport || options->mode >= FSRGMode::eCount ||
        algorithmOptions->algorithm >= FSRGAlgorithm::eCount ||
        options->colorSpace >= FSRColorSpace::eCount ||
        !std::isfinite(options->frameTimeDeltaMilliseconds) || options->frameTimeDeltaMilliseconds < 0.0f ||
        !std::isfinite(options->viewSpaceToMetersFactor) || options->viewSpaceToMetersFactor <= 0.0f ||
        (options->mode == FSRGMode::eOn &&
            (options->displayWidth == 0 || options->displayWidth == INVALID_UINT ||
             options->displayHeight == 0 || options->displayHeight == INVALID_UINT ||
             options->backBufferFormat == DXGI_FORMAT_UNKNOWN)))
    {
        return Result::eErrorInvalidParameter;
    }
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    if (options->mode != FSRGMode::eOff &&
        (getUnavailableReason(ctx, algorithmOptions->algorithm) !=
             FSRUnavailableReason::eNone ||
         (usesGamma22Adapter(
              algorithmOptions->algorithm,
              options->colorSpace) &&
          !ctx.colorConversion)))
    {
        return Result::eErrorFeatureNotSupported;
    }
    ctx.options.set(0, *viewport, options, algorithmOptions);
    if (options->mode == FSRGMode::eOff)
    {
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
    auto* viewport = findStruct<const ViewportHandle>(inputs);
    auto* state = findStruct<FSRGState>(output);
    if (!viewport || !state) return Result::eErrorMissingInputParameter;
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    auto it = ctx.viewports.find(*viewport);
    const auto algorithm = it != ctx.viewports.end() ?
        it->second.algorithm :
        FSRGAlgorithm::eFSR3;
    const auto& frameGenerationProvider =
        getAlgorithmProvider(ctx, algorithm);
    state->frameGenerationVersionMajor = frameGenerationProvider.major;
    state->frameGenerationVersionMinor = frameGenerationProvider.minor;
    state->frameGenerationVersionPatch = frameGenerationProvider.patch;
    if (state->structVersion >= kStructVersion2)
    {
        const auto reason = getUnavailableReason(ctx, algorithm);
        state->algorithm = algorithm;
        state->available =
            reason == FSRUnavailableReason::eNone ?
                Boolean::eTrue :
                Boolean::eFalse;
        state->unavailableReason = reason;
    }
    state->swapchainVersionMajor = ctx.swapchainProvider.major;
    state->swapchainVersionMinor = ctx.swapchainProvider.minor;
    state->swapchainVersionPatch = ctx.swapchainProvider.patch;
    state->active = it != ctx.viewports.end() && it->second.context ? Boolean::eTrue : Boolean::eFalse;
    state->completionMode = FSRGCompletionMode::eNone;
    state->completionFence = nullptr;
    state->completionFenceValue = 0;
    state->estimatedVRAMUsageInBytes = 0;
    if (ctx.swapchainContext)
    {
        ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1 completion{
            { FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 },
            &state->completionFence,
            &state->completionFenceValue
        };
        if (ctx.legacyRuntime.query(
                &ctx.swapchainContext,
                &completion.header) == FFX_API_RETURN_OK &&
            state->completionFence)
        {
            state->completionMode = FSRGCompletionMode::eFence;
        }
        else
        {
            if (state->completionFence)
            {
                static_cast<ID3D12Fence*>(state->completionFence)->Release();
            }
            state->completionFence = nullptr;
            state->completionFenceValue = 0;
            state->completionMode = FSRGCompletionMode::eVendorCompletionUnavailable;
        }
    }

    if (it != ctx.viewports.end() && it->second.context)
    {
        FfxApiEffectMemoryUsage memory{};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage query{{ FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE }, &memory};
        if (getAlgorithmRuntime(ctx, algorithm).query(
                &it->second.context,
                &query.header) == FFX_API_RETURN_OK)
        {
            state->estimatedVRAMUsageInBytes = memory.totalUsageInBytes;
        }
        std::scoped_lock conversionLock(
            it->second.conversionFramesMutex);
        for (const auto& frame : it->second.conversionFrames)
        {
            state->estimatedVRAMUsageInBytes +=
                getConversionFrameBytes(*frame);
        }
    }
    return Result::eOk;
}

Result slFSRGQuiesce()
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    for (auto& [id, viewport] : ctx.viewports) destroyViewport(ctx, viewport);
    ctx.viewports.clear();
    waitForPresents(ctx);
    return Result::eOk;
}

Result slAllocateResources(CommandBuffer*, Feature feature, const ViewportHandle&)
{
    return feature == kFeatureFSR_G ? Result::eOk : Result::eErrorInvalidParameter;
}

Result slFreeResources(Feature feature, const ViewportHandle& viewport)
{
    if (feature != kFeatureFSR_G) return Result::eErrorInvalidParameter;
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    auto it = ctx.viewports.find(viewport);
    if (it == ctx.viewports.end()) return Result::eOk;
    destroyViewport(ctx, it->second);
    ctx.viewports.erase(it);
    return Result::eOk;
}

HRESULT slHookCreateSwapChain(
    IDXGIFactory* factory,
    IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** output,
    bool& skip)
{
    ID3D12CommandQueue* queue{};
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return E_INVALIDARG;
    const HRESULT result = createSwapchain(factory, queue, desc, output);
    queue->Release();
    skip = SUCCEEDED(result);
    return result;
}

HRESULT slHookCreateSwapChainForHwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    IDXGIOutput*,
    IDXGISwapChain1** output,
    bool& skip)
{
    ID3D12CommandQueue* queue{};
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return E_INVALIDARG;
    const HRESULT result = createSwapchainForHwnd(factory, queue, hwnd, desc, fullscreenDesc, output);
    queue->Release();
    skip = SUCCEEDED(result);
    return result;
}

void slHookSwapchainDestroyed(IDXGISwapChain* swapchain)
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    if (ctx.swapchain != swapchain) return;
    for (auto& [id, viewport] : ctx.viewports) destroyViewport(ctx, viewport);
    ctx.viewports.clear();
    destroySwapchainContext(ctx);
}

bool slOnPluginStartup(const char* jsonConfig, void* device)
{
    SL_PLUGIN_COMMON_STARTUP();
    auto& ctx = *fsr_g::getContext();
    ctx.device = static_cast<ID3D12Device*>(device);
    const std::filesystem::path moduleDirectory =
        file::getModulePath();
    if (!ctx.device ||
        !ctx.legacyRuntime.initialize(
            moduleDirectory / L"cs_fidelityfx_framegeneration_dx12.dll") ||
        !ctx.legacyRuntime.selectProvider(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            ctx.device,
            "3.1.6",
            ctx.fsr3FrameGenerationProvider) ||
        !ctx.legacyRuntime.selectProvider(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12,
            ctx.device,
            "3.1.7",
            ctx.swapchainProvider) ||
        !ctx.legacyRuntime.selectProvider(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
            ctx.device,
            "3.1.7",
            ctx.swapchainHwndProvider))
    {
        return false;
    }
    const bool fsr4ModuleAvailable = ctx.mlRuntime.initialize(
        moduleDirectory / L"amd_fidelityfx_framegeneration_dx12.dll");
    const bool fsr4ProviderAvailable =
        fsr4ModuleAvailable &&
        ctx.mlRuntime.selectProvider(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            ctx.device,
            "4.0.1",
            ctx.fsr4FrameGenerationProvider,
            false);
    ctx.fsr4UnavailableReason =
        fsr::getMachineLearningUnavailableReason(
            ctx.device,
            fsr4ModuleAvailable,
            fsr4ProviderAvailable,
            true);
    if (!param::getPointerParam(
            api::getContext()->parameters,
            sl::param::common::kComputeAPI,
            &ctx.compute) ||
        !ctx.colorConversion.initialize(ctx.compute))
    {
        SL_LOG_WARN(
            "MLFG Gamma 2.2 conversion is unavailable; linear, sRGB and PQ remain usable");
    }
    if (!param::getPointerParam(api::getContext()->parameters, param::common::kPFunRegisterEvaluateCallbacks, &ctx.registerEvaluateCallbacks))
    {
        return false;
    }
    ctx.registerEvaluateCallbacks(kFeatureFSR_G, fsrGPrepare, nullptr);
    return true;
}

void slOnPluginShutdown()
{
    auto& ctx = *fsr_g::getContext();
    if (ctx.registerEvaluateCallbacks) ctx.registerEvaluateCallbacks(kFeatureFSR_G, nullptr, nullptr);
    slFSRGQuiesce();
    {
        std::scoped_lock lock(ctx.mutex);
        destroySwapchainContext(ctx);
        ctx.colorConversion.shutdown();
        ctx.mlRuntime.shutdown();
        ctx.legacyRuntime.shutdown();
    }
    plugin::onShutdown(api::getContext());
}

sl::Result slFSRGGetState(const ViewportHandle& viewport, FSRGState& state)
{
    return slGetData(&viewport, &state, nullptr);
}

sl::Result slFSRGSetOptions(const ViewportHandle& viewport, const FSRGOptions& options)
{
    auto input = viewport;
    input.next = const_cast<FSRGOptions*>(&options);
    return slSetData(&input, nullptr);
}

sl::Result slFSRGGetCapabilities(
    FSRGAlgorithm algorithm,
    FSRGCapabilities& capabilities)
{
    if (algorithm >= FSRGAlgorithm::eCount)
    {
        return Result::eErrorInvalidParameter;
    }
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    const auto reason = getUnavailableReason(ctx, algorithm);
    const auto& provider = getAlgorithmProvider(ctx, algorithm);
    capabilities.algorithm = algorithm;
    capabilities.available =
        reason == FSRUnavailableReason::eNone ?
            Boolean::eTrue :
            Boolean::eFalse;
    capabilities.unavailableReason = reason;
    capabilities.frameGenerationVersionMajor = provider.major;
    capabilities.frameGenerationVersionMinor = provider.minor;
    capabilities.frameGenerationVersionPatch = provider.patch;
    capabilities.swapchainVersionMajor = ctx.swapchainProvider.major;
    capabilities.swapchainVersionMinor = ctx.swapchainProvider.minor;
    capabilities.swapchainVersionPatch = ctx.swapchainProvider.patch;
    fsr::setD3D12RuntimeCapabilities(ctx.device, capabilities);
    return Result::eOk;
}

sl::Result slIsSupported(const AdapterInfo&)
{
    return fsr_g::getContext()->legacyRuntime ?
        Result::eOk :
        Result::eErrorFeatureNotSupported;
}

void updateEmbeddedJSON(json& config)
{
    common::PFunUpdateCommonEmbeddedJSONConfig* update{};
    param::getPointerParam(api::getContext()->parameters, param::common::kPFunUpdateCommonEmbeddedJSONConfig, &update);
    if (!update) return;
    common::PluginInfo info{};
    info.SHA = GIT_LAST_COMMIT_SHORT;
    info.minOS = Version(10, 0, 0);
    info.requiredTags = {
        { kBufferTypeDepth, ResourceLifecycle::eValidUntilPresent },
        { kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilPresent }
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
    SL_EXPORT_FUNCTION(slFSRGGetState);
    SL_EXPORT_FUNCTION(slFSRGSetOptions);
    SL_EXPORT_FUNCTION(slFSRGGetCapabilities);
    SL_EXPORT_FUNCTION(slFSRGQuiesce);
    SL_EXPORT_FUNCTION(slHookCreateSwapChain);
    SL_EXPORT_FUNCTION(slHookCreateSwapChainForHwnd);
    SL_EXPORT_FUNCTION(slHookSwapchainDestroyed);
    return nullptr;
}

}
