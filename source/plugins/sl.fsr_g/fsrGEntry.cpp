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

#include <cmath>
#include <map>
#include <mutex>
#include <numbers>

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
#include "source/plugins/sl.fsr.common/ffxRuntime.h"
#include "source/plugins/sl.fsr_g/versions.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "external/json/include/nlohmann/json.hpp"
#include "_artifacts/json/fsr_g_json.h"

using json = nlohmann::json;

namespace sl
{

namespace fsr_g
{

struct Viewport
{
    FSRGOptions options{};
    ffxContext context{};
    uint32_t createFlags{};
    FfxApiDimensions2D maxRenderSize{};
    FfxApiDimensions2D displaySize{};
    uint32_t backBufferFormat{};
};

struct Context
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(Context);
    void onCreateContext() {}
    void onDestroyContext() {}

    fsr::Runtime runtime{};
    fsr::ProviderVersion frameGenerationProvider{};
    fsr::ProviderVersion swapchainProvider{};
    fsr::ProviderVersion swapchainHwndProvider{};
    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};
    common::ViewportIdFrameData<4, false> options = { "fsr_g" };
    std::map<uint32_t, Viewport> viewports{};
    ID3D12Device* device{};
    ID3D12CommandQueue* gameQueue{};
    IDXGISwapChain4* swapchain{};
    ffxContext swapchainContext{};
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
    if (ctx.runtime.dispatch(&ctx.swapchainContext, &wait.header) != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("FidelityFX frame-generation swapchain wait failed");
    }
}

void destroyViewport(fsr_g::Context& ctx, fsr_g::Viewport& viewport)
{
    if (!viewport.context) return;
    ffxConfigureDescFrameGeneration disable{{ FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION }};
    disable.swapChain = ctx.swapchain;
    disable.frameGenerationEnabled = false;
    disable.allowAsyncWorkloads = false;
    ctx.runtime.configure(&viewport.context, &disable.header);
    waitForPresents(ctx);
    if (ctx.runtime.destroy(&viewport.context) != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("Failed to destroy FSR frame-generation context");
    }
    viewport.context = {};
}

void destroySwapchainContext(fsr_g::Context& ctx)
{
    if (!ctx.swapchainContext) return;
    waitForPresents(ctx);
    if (ctx.runtime.destroy(&ctx.swapchainContext) != FFX_API_RETURN_OK)
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
    const uint32_t backBufferFormat = ffxApiGetSurfaceFormatDX12(static_cast<DXGI_FORMAT>(options.backBufferFormat));
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
    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, ctx.frameGenerationProvider.id};
    ffxCreateBackendDX12Desc backend{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12, &overrideVersion.header }, ctx.device};
    ffxCreateContextDescFrameGenerationVersion apiVersion{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &backend.header }, FFX_FRAMEGENERATION_VERSION};
    ffxCreateContextDescFrameGeneration create{{ FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &apiVersion.header }, flags, displaySize, maxRenderSize, backBufferFormat};
    if (ctx.runtime.create(&viewport.context, &create.header) != FFX_API_RETURN_OK ||
        !ctx.runtime.validateProvider(viewport.context, ctx.frameGenerationProvider))
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

ffxReturnCode_t frameGenerationCallback(ffxDispatchDescFrameGeneration* desc, void* user)
{
    auto* viewport = static_cast<fsr_g::Viewport*>(user);
    if (!viewport || !viewport->context) return FFX_API_RETURN_ERROR_PARAMETER;
    desc->backbufferTransferFunction =
        fsr::getFrameGenerationTransferFunction(viewport->options.colorSpace);
    return fsr_g::getContext()->runtime.dispatch(&viewport->context, &desc->header);
}

Result fsrGPrepare(chi::CommandList commandList, const common::EventData& event, const BaseStructure** inputs, uint32_t numInputs)
{
    auto& ctx = *fsr_g::getContext();
    std::scoped_lock lock(ctx.mutex);
    FSRGOptions* options{};
    Constants* constants{};
    if (!ctx.options.get(event, &options) || options->mode == FSRGMode::eOff) return Result::eErrorInvalidState;
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
    viewport.options = *options;
    if (!ensureFrameGenerationContext(ctx, viewport, *constants, renderSize, motionSize))
    {
        SL_LOG_ERROR("Failed to create the required FSR frame-generation 3.1.6 context");
        return Result::eErrorComputeFailed;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> nativeCommandListReference;
    auto* nativeCommandList = getNativeCommandList(commandList, nativeCommandListReference);
    if (!nativeCommandList) return Result::eErrorMissingInputParameter;

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{
        { FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 },
        ui ? fsr::getResource(static_cast<ID3D12Resource*>(ui.getNative()), static_cast<D3D12_RESOURCE_STATES>(ui.getState())) : FfxApiResource{},
        0
    };
    if (ctx.runtime.configure(&ctx.swapchainContext, &uiConfig.header) != FFX_API_RETURN_OK)
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
        configure.HUDLessColor = fsr::getResource(static_cast<ID3D12Resource*>(hudless.getNative()), static_cast<D3D12_RESOURCE_STATES>(hudless.getState()));
    }
    configure.onlyPresentGenerated = options->onlyPresentGenerated == Boolean::eTrue;
    configure.generationRect = { 0, 0, static_cast<int32_t>(options->displayWidth), static_cast<int32_t>(options->displayHeight) };
    configure.frameID = event.frame;
    if (ctx.runtime.configure(&viewport.context, &configure.header) != FFX_API_RETURN_OK)
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
    const auto result = ctx.runtime.dispatch(&viewport.context, &prepare.header);
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
    if (ctx.runtime.create(&ctx.swapchainContext, &create.header) != FFX_API_RETURN_OK ||
        !ctx.runtime.validateProvider(ctx.swapchainContext, ctx.swapchainProvider))
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
    if (ctx.runtime.create(&ctx.swapchainContext, &create.header) != FFX_API_RETURN_OK ||
        !ctx.runtime.validateProvider(ctx.swapchainContext, ctx.swapchainHwndProvider))
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
    auto* viewport = findStruct<const ViewportHandle>(inputs);
    if (!options || !viewport || options->mode >= FSRGMode::eCount ||
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
    ctx.options.set(0, *viewport, options);
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
    state->frameGenerationVersionMajor = ctx.frameGenerationProvider.major;
    state->frameGenerationVersionMinor = ctx.frameGenerationProvider.minor;
    state->frameGenerationVersionPatch = ctx.frameGenerationProvider.patch;
    state->swapchainVersionMajor = ctx.swapchainProvider.major;
    state->swapchainVersionMinor = ctx.swapchainProvider.minor;
    state->swapchainVersionPatch = ctx.swapchainProvider.patch;
    auto it = ctx.viewports.find(*viewport);
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
        if (ctx.runtime.query(&ctx.swapchainContext, &completion.header) == FFX_API_RETURN_OK &&
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
        if (ctx.runtime.query(&it->second.context, &query.header) == FFX_API_RETURN_OK)
        {
            state->estimatedVRAMUsageInBytes = memory.totalUsageInBytes;
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
    if (!ctx.device || !ctx.runtime.initialize(file::getModulePath(), false, true) ||
        !ctx.runtime.selectProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, ctx.device, "3.1.6", ctx.frameGenerationProvider) ||
    !ctx.runtime.selectProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12, ctx.device, "3.1.7", ctx.swapchainProvider) ||
    !ctx.runtime.selectProvider(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, ctx.device, "3.1.7", ctx.swapchainHwndProvider))
    {
        return false;
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
        ctx.runtime.shutdown();
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

sl::Result slIsSupported(const AdapterInfo&)
{
    return fsr_g::getContext()->runtime ? Result::eOk : Result::eErrorFeatureNotSupported;
}

void updateEmbeddedJSON(json& config)
{
    common::PFunUpdateCommonEmbeddedJSONConfig* update{};
    param::getPointerParam(api::getContext()->parameters, param::common::kPFunUpdateCommonEmbeddedJSONConfig, &update);
    if (!update) return;
    common::PluginInfo info{};
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
    SL_EXPORT_FUNCTION(slFSRGQuiesce);
    SL_EXPORT_FUNCTION(slHookCreateSwapChain);
    SL_EXPORT_FUNCTION(slHookCreateSwapChainForHwnd);
    SL_EXPORT_FUNCTION(slHookSwapchainDestroyed);
    return nullptr;
}

}
