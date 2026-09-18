#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler_color.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "source/plugins/sl.fsr.common/colorConversion.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

class RelativeInclude final : public ID3DInclude
{
public:
    explicit RelativeInclude(const std::filesystem::path& sourcePath) :
        m_sourceDirectory(sourcePath.parent_path())
    {
    }

    HRESULT __stdcall Open(
        D3D_INCLUDE_TYPE,
        LPCSTR fileName,
        LPCVOID parentData,
        LPCVOID* data,
        UINT* bytes) override
    {
        if (!fileName || !data || !bytes)
        {
            return E_INVALIDARG;
        }

        auto directory = m_sourceDirectory;
        if (parentData)
        {
            const auto parent = m_paths.find(parentData);
            if (parent == m_paths.end())
            {
                return E_FAIL;
            }
            directory = parent->second.parent_path();
        }

        const auto path = (directory / fileName).lexically_normal();
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return E_FAIL;
        }
        const auto size = stream.tellg();
        if (size < 0 ||
            static_cast<uint64_t>(size) > std::numeric_limits<UINT>::max())
        {
            return E_FAIL;
        }
        stream.seekg(0);

        auto contents = std::make_unique<char[]>(static_cast<size_t>(size));
        if (!stream.read(contents.get(), size))
        {
            return E_FAIL;
        }
        *bytes = static_cast<UINT>(size);
        *data = contents.release();
        m_paths.emplace(*data, path);
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID data) override
    {
        const auto entry = m_paths.find(data);
        if (entry == m_paths.end())
        {
            return E_INVALIDARG;
        }
        m_paths.erase(entry);
        delete[] static_cast<const char*>(data);
        return S_OK;
    }

private:
    std::filesystem::path m_sourceDirectory;
    std::unordered_map<LPCVOID, std::filesystem::path> m_paths;
};

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

bool waitForFence(ID3D12Fence* fence, uint64_t value)
{
    if (fence->GetCompletedValue() >= value)
    {
        return true;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
    {
        return false;
    }
    const bool completed =
        SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
        WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return completed;
}

using ColorResults = std::array<std::array<float, 4>, 3>;

bool runColorTransfer(
    ID3D12Device* device,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    uint32_t constantBufferSize,
    uint32_t transferFunctionOffset,
    uint32_t transferFunction,
    ColorResults& results)
{
    constexpr uint32_t resultCount = 3;
    constexpr uint32_t resultStride = sizeof(float) * 4;
    constexpr uint32_t resultBytes = resultCount * resultStride;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC resultDesc{};
    resultDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resultDesc.Width = resultBytes;
    resultDesc.Height = 1;
    resultDesc.DepthOrArraySize = 1;
    resultDesc.MipLevels = 1;
    resultDesc.SampleDesc.Count = 1;
    resultDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &resultDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(output.GetAddressOf()))))
    {
        return false;
    }

    resultDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &resultDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readback.GetAddressOf()))))
    {
        return false;
    }

    const uint32_t alignedConstantBufferSize =
        (constantBufferSize + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
        ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC constantDesc = resultDesc;
    constantDesc.Width = alignedConstantBufferSize;
    Microsoft::WRL::ComPtr<ID3D12Resource> constants;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &constantDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(constants.GetAddressOf()))))
    {
        return false;
    }

    void* mappedConstants{};
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(constants->Map(0, &noRead, &mappedConstants)))
    {
        return false;
    }
    std::memset(mappedConstants, 0, alignedConstantBufferSize);
    std::memcpy(
        static_cast<uint8_t*>(mappedConstants) + transferFunctionOffset,
        &transferFunction,
        sizeof(transferFunction));
    D3D12_RANGE writtenConstants{
        transferFunctionOffset,
        transferFunctionOffset + sizeof(transferFunction)
    };
    constants->Unmap(0, &writtenConstants);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    if (FAILED(device->CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(descriptorHeap.GetAddressOf()))))
    {
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.NumElements = resultCount;
    uavDesc.Buffer.StructureByteStride = resultStride;
    device->CreateUnorderedAccessView(
        output.Get(),
        nullptr,
        &uavDesc,
        descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(
            &queueDesc,
            IID_PPV_ARGS(queue.GetAddressOf()))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            pipelineState,
            IID_PPV_ARGS(commandList.GetAddressOf()))) ||
        FAILED(device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()))))
    {
        return false;
    }

    D3D12_RESOURCE_BARRIER toUav{};
    toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav.Transition.pResource = output.Get();
    toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &toUav);
    commandList->SetComputeRootSignature(rootSignature);
    ID3D12DescriptorHeap* heaps[]{ descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootDescriptorTable(
        0,
        descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootConstantBufferView(
        1,
        constants->GetGPUVirtualAddress());
    commandList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = output.Get();
    commandList->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = output.Get();
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, resultBytes);
    if (FAILED(commandList->Close()))
    {
        return false;
    }

    ID3D12CommandList* lists[]{ commandList.Get() };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1)) || !waitForFence(fence.Get(), 1))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, resultBytes };
    void* mappedResults{};
    if (FAILED(readback->Map(0, &readRange, &mappedResults)))
    {
        return false;
    }
    std::memcpy(results.data(), mappedResults, resultBytes);
    D3D12_RANGE noWrite{ 0, 0 };
    readback->Unmap(0, &noWrite);
    return true;
}

bool validatesColorContract(ID3D12Device* device, const wchar_t* shaderPath)
{
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    RelativeInclude includeResolver(shaderPath);
    const HRESULT compileResult = D3DCompileFromFile(
        shaderPath,
        nullptr,
        &includeResolver,
        "main",
        "cs_5_1",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        shader.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(compileResult))
    {
        if (errors)
        {
            std::cerr.write(
                static_cast<const char*>(errors->GetBufferPointer()),
                static_cast<std::streamsize>(errors->GetBufferSize()));
        }
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    if (FAILED(D3DReflect(
            shader->GetBufferPointer(),
            shader->GetBufferSize(),
            IID_PPV_ARGS(reflection.GetAddressOf()))))
    {
        return false;
    }

    const auto constantBuffer = reflection->GetConstantBufferByName("cbFSR3Upscaler");
    D3D12_SHADER_BUFFER_DESC constantBufferDesc{};
    const auto transferFunctionVariable =
        constantBuffer->GetVariableByName("uInputColorTransferFunction");
    D3D12_SHADER_VARIABLE_DESC transferFunctionDesc{};
    if (FAILED(constantBuffer->GetDesc(&constantBufferDesc)) ||
        FAILED(transferFunctionVariable->GetDesc(&transferFunctionDesc)) ||
        transferFunctionDesc.Size != sizeof(uint32_t))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = _countof(parameters);
    rootDesc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> serializedRoot;
    Microsoft::WRL::ComPtr<ID3DBlob> rootErrors;
    if (FAILED(D3D12SerializeRootSignature(
            &rootDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRoot.GetAddressOf(),
            rootErrors.GetAddressOf())))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    if (FAILED(device->CreateRootSignature(
            0,
            serializedRoot->GetBufferPointer(),
            serializedRoot->GetBufferSize(),
            IID_PPV_ARGS(rootSignature.GetAddressOf()))))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature.Get();
    pipelineDesc.CS = {
        shader->GetBufferPointer(),
        shader->GetBufferSize()
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    if (FAILED(device->CreateComputePipelineState(
            &pipelineDesc,
            IID_PPV_ARGS(pipelineState.GetAddressOf()))))
    {
        return false;
    }

    const uint32_t gammaDispatch =
        sl::fsr::getUpscaleColorDispatchFlags(sl::FSRColorSpace::eGamma22);
    const uint32_t srgbDispatch =
        sl::fsr::getUpscaleColorDispatchFlags(sl::FSRColorSpace::eSRGB);
    const uint32_t gammaInternalDispatch =
        ffxFsr3UpscalerGetDispatchColorFlags(gammaDispatch);
    const uint32_t srgbInternalDispatch =
        ffxFsr3UpscalerGetDispatchColorFlags(srgbDispatch);
    const bool mappingsValid =
        gammaDispatch == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_GAMMA_2_2 &&
        srgbDispatch == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB &&
        sl::fsr::getUpscaleColorCreateFlags(sl::FSRColorSpace::eGamma22) ==
            FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE &&
        sl::fsr::getFrameGenerationTransferFunction(sl::FSRColorSpace::eGamma22) ==
            FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2;

    ColorResults gamma{};
    ColorResults srgb{};
    if (!mappingsValid ||
        !runColorTransfer(
            device,
            rootSignature.Get(),
            pipelineState.Get(),
            constantBufferDesc.Size,
            transferFunctionDesc.StartOffset,
            ffxFsr3UpscalerGetInputColorTransferFunction(gammaInternalDispatch),
            gamma) ||
        !runColorTransfer(
            device,
            rootSignature.Get(),
            pipelineState.Get(),
            constantBufferDesc.Size,
            transferFunctionDesc.StartOffset,
            ffxFsr3UpscalerGetInputColorTransferFunction(srgbInternalDispatch),
            srgb))
    {
        return false;
    }

    const auto isNear = [](float actual, float expected, float tolerance)
    {
        return std::isfinite(actual) &&
            std::fabs(actual - expected) <= tolerance;
    };
    const bool valid =
        isNear(gamma[0][0], 0.217638f, 0.0001f) &&
        isNear(srgb[0][0], 0.214041f, 0.0001f) &&
        std::fabs(gamma[0][0] - srgb[0][0]) > 0.003f &&
        isNear(gamma[1][0], 0.5f, 0.0001f) &&
        isNear(srgb[1][0], 0.5f, 0.0001f) &&
        isNear(gamma[2][0], 0.0f, 0.0001f) &&
        std::isnan(srgb[2][0]);
    if (!valid)
    {
        std::cerr << "Unexpected FSR transfer results: gamma="
            << gamma[0][0] << ", srgb=" << srgb[0][0]
            << ", gamma round-trip=" << gamma[1][0]
            << ", srgb round-trip=" << srgb[1][0]
            << ", gamma negative=" << gamma[2][0]
            << ", srgb negative=" << srgb[2][0] << '\n';
    }
    return valid;
}

bool validatesCompletionLifecycle(
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

    Microsoft::WRL::ComPtr<ID3D12Fence> idleFence;
    uint64_t idleValue = UINT64_MAX;
    bool supported{};
    if (created)
    {
        void* completionFence{};
        ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1 completion{
            { FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 },
            &completionFence,
            &idleValue
        };
        supported =
            query(&context, &completion.header) == FFX_API_RETURN_OK &&
            completionFence &&
            idleValue == 0;
        idleFence.Attach(static_cast<ID3D12Fence*>(completionFence));
    }

    if (supported)
    {
        const HRESULT presentResult = swapchain->Present(0, 0);
        void* completionFence{};
        uint64_t completionValue{};
        ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1 completion{
            { FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 },
            &completionFence,
            &completionValue
        };
        Microsoft::WRL::ComPtr<ID3D12Fence> presentedFence;
        if (query(&context, &completion.header) == FFX_API_RETURN_OK)
        {
            presentedFence.Attach(static_cast<ID3D12Fence*>(completionFence));
        }
        supported =
            SUCCEEDED(presentResult) &&
            presentedFence &&
            presentedFence.Get() == idleFence.Get() &&
            completionValue > idleValue &&
            waitForFence(presentedFence.Get(), completionValue);
        if (!supported)
        {
            std::cerr << "Completion lifecycle failed: present=0x"
                << std::hex << static_cast<unsigned long>(presentResult)
                << std::dec << ", idle=" << idleValue
                << ", submitted=" << completionValue
                << ", fence=" << static_cast<bool>(presentedFence) << '\n';
        }
    }

    if (created && destroy) destroy(&context, nullptr);
    if (swapchain) swapchain->Release();
    DestroyWindow(window);
    return supported;
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5)
    {
        std::cerr << "usage: fsr-provider-runtime-regression "
            "<project-upscaler> <project-frame-generation> <official-loader> "
            "<color-transfer-shader>\n";
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
    const bool colorValid =
        providersValid && validatesColorContract(device.Get(), argv[4]);
    const bool completionValid =
        providersValid && validatesCompletionLifecycle(
            create,
            destroy,
            query,
            device.Get(),
            swapchainVersionId);
    const bool valid = providersValid && colorValid && completionValid;

    FreeLibrary(loader);
    FreeLibrary(frameGeneration);
    FreeLibrary(upscaler);
    if (!valid)
    {
        std::cerr << "Contract status: providers=" << providersValid
            << ", color=" << colorValid
            << ", completion=" << completionValid << '\n';
        return 1;
    }
    std::cout << "FidelityFX provider discovery, color transfer, and completion passed.\n";
    return 0;
}
