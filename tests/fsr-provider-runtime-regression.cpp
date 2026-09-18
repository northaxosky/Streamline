#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{

bool findVersion(
    PfnFfxQuery query,
    ffxStructType_t createType,
    ID3D12Device* device,
    std::string_view expected,
    uint64_t* versionId = nullptr)
{
    uint64_t count{};
    ffxQueryDescGetVersions countQuery{
        { FFX_API_QUERY_DESC_TYPE_GET_VERSIONS },
        createType,
        device,
        &count
    };
    if (query(nullptr, &countQuery.header) != FFX_API_RETURN_OK || !count)
    {
        return false;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    ffxQueryDescGetVersions versionsQuery{
        { FFX_API_QUERY_DESC_TYPE_GET_VERSIONS },
        createType,
        device,
        &count,
        ids.data(),
        names.data()
    };
    if (query(nullptr, &versionsQuery.header) != FFX_API_RETURN_OK)
    {
        return false;
    }
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] && names[i] == expected)
        {
            if (versionId) *versionId = ids[i];
            return true;
        }
    }
    return false;
}

bool hasIdleCompletion(
    PfnFfxCreateContext create,
    PfnFfxDestroyContext destroy,
    PfnFfxQuery query,
    ID3D12Device* device,
    uint64_t versionId)
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf()))))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        return false;
    }

    HWND window = CreateWindowExW(
        0,
        L"STATIC",
        L"FidelityFX completion regression",
        WS_OVERLAPPED,
        0,
        0,
        64,
        64,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!window)
    {
        return false;
    }

    IDXGISwapChain4* swapchain{};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ffxOverrideVersion overrideVersion{{ FFX_API_DESC_TYPE_OVERRIDE_VERSION }, versionId};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 apiVersion{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12, &overrideVersion.header },
        FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION
    };
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createDesc{
        { FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, &apiVersion.header },
        &swapchain,
        window,
        &desc,
        nullptr,
        factory.Get(),
        queue.Get()
    };
    ffxContext context{};
    const bool created = create &&
        create(&context, &createDesc.header, nullptr) == FFX_API_RETURN_OK &&
        swapchain;

    void* completionFence{};
    uint64_t completionValue = UINT64_MAX;
    bool supported{};
    if (created)
    {
        ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1 completion{
            { FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 },
            &completionFence,
            &completionValue
        };
        supported =
            query(&context, &completion.header) == FFX_API_RETURN_OK &&
            completionFence &&
            completionValue == 0;
    }

    if (completionFence) static_cast<ID3D12Fence*>(completionFence)->Release();
    if (created && destroy) destroy(&context, nullptr);
    if (swapchain) swapchain->Release();
    DestroyWindow(window);
    return supported;
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: fsr-provider-runtime-regression "
            "<project-upscaler> <project-frame-generation> <official-loader>\n";
        return 2;
    }

    HMODULE upscaler = LoadLibraryExW(
        argv[1],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HMODULE frameGeneration = LoadLibraryExW(
        argv[2],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HMODULE loader = LoadLibraryExW(
        argv[3],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!upscaler || !frameGeneration || !loader)
    {
        std::cerr << "Failed to load FidelityFX runtime modules: "
            << GetLastError() << '\n';
        return 1;
    }

    const auto query = reinterpret_cast<PfnFfxQuery>(
        GetProcAddress(loader, "ffxQuery"));
    const auto create = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(loader, "ffxCreateContext"));
    const auto destroy = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(loader, "ffxDestroyContext"));
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const HRESULT deviceResult = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(device.GetAddressOf()));
    uint64_t swapchainVersionId{};
    const bool providersValid =
        query &&
        create &&
        destroy &&
        SUCCEEDED(deviceResult) &&
        findVersion(
            query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            device.Get(),
            "3.1.5") &&
        findVersion(
            query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            device.Get(),
            "3.1.6") &&
        findVersion(
            query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12,
            device.Get(),
            "3.1.7") &&
        findVersion(
            query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
            device.Get(),
            "3.1.7",
            &swapchainVersionId);
    const bool valid =
        providersValid &&
        hasIdleCompletion(
            create,
            destroy,
            query,
            device.Get(),
            swapchainVersionId);

    FreeLibrary(loader);
    FreeLibrary(frameGeneration);
    FreeLibrary(upscaler);
    if (!valid)
    {
        std::cerr << "The project providers or idle completion contract failed.\n";
        return 1;
    }
    std::cout << "FidelityFX project provider discovery and idle completion passed.\n";
    return 0;
}
