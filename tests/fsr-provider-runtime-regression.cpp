#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler_color.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "source/plugins/sl.fsr.common/colorConversion.h"
#include "source/plugins/sl.fsr.common/jitter.h"
#include "source/plugins/sl.fsr.common/providerCapabilities.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
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

    uint32_t gammaDispatch{};
    uint32_t srgbDispatch{};
    uint32_t gammaTransfer{};
    const bool hasProjectGammaDispatch =
        sl::fsr::getUpscaleColorDispatchFlags(
            sl::FSRColorSpace::eGamma22,
            true,
            gammaDispatch);
    const bool hasSrgbDispatch =
        sl::fsr::getUpscaleColorDispatchFlags(
            sl::FSRColorSpace::eSRGB,
            false,
            srgbDispatch);
    const bool hasProjectGammaTransfer =
        sl::fsr::getFrameGenerationTransferFunction(
            sl::FSRColorSpace::eGamma22,
            true,
            gammaTransfer);
    const uint32_t gammaInternalDispatch =
        ffxFsr3UpscalerGetDispatchColorFlags(gammaDispatch);
    const uint32_t srgbInternalDispatch =
        ffxFsr3UpscalerGetDispatchColorFlags(srgbDispatch);
    const bool projectFsr3MappingsValid =
        hasProjectGammaDispatch &&
        hasSrgbDispatch &&
        hasProjectGammaTransfer &&
        gammaDispatch == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_GAMMA_2_2 &&
        srgbDispatch == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB &&
        sl::fsr::getUpscaleColorCreateFlags(sl::FSRColorSpace::eGamma22) ==
            FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE &&
        gammaTransfer ==
            FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2;
    uint32_t officialUpscaleFlags{};
    uint32_t officialFrameGenerationTransfer{};
    const bool officialLinearMappingValid =
        sl::fsr::getUpscaleColorDispatchFlags(
            sl::FSRColorSpace::eLinear,
            false,
            officialUpscaleFlags) &&
        officialUpscaleFlags == 0 &&
        sl::fsr::getFrameGenerationTransferFunction(
            sl::FSRColorSpace::eLinear,
            false,
            officialFrameGenerationTransfer) &&
        officialFrameGenerationTransfer ==
            FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB &&
        !sl::fsr::getUpscaleColorDispatchFlags(
            sl::FSRColorSpace::eGamma22,
            false,
            officialUpscaleFlags) &&
        !sl::fsr::getFrameGenerationTransferFunction(
            sl::FSRColorSpace::eGamma22,
            false,
            officialFrameGenerationTransfer);

    ColorResults gamma{};
    ColorResults srgb{};
    if (!projectFsr3MappingsValid ||
        !officialLinearMappingValid ||
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

bool validatesGamma22LinearAdapter(
    ID3D12Device* device,
    const wchar_t* shaderPath)
{
    // Exercise the shipped conversion HLSL, PSO, barriers, and fence lifetime
    // independently of ML-provider support. This is not an FSR4/MLFG dispatch.
    std::ifstream shaderStream(
        shaderPath,
        std::ios::binary | std::ios::ate);
    if (!shaderStream) return false;
    const auto shaderSize = shaderStream.tellg();
    if (shaderSize <= 0) return false;
    shaderStream.seekg(0);
    std::vector<uint8_t> shader(static_cast<size_t>(shaderSize));
    if (!shaderStream.read(
            reinterpret_cast<char*>(shader.data()),
            shaderSize))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
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
    pipelineDesc.CS = { shader.data(), shader.size() };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(
            &pipelineDesc,
            IID_PPV_ARGS(pipeline.GetAddressOf()))))
    {
        return false;
    }

    constexpr UINT width = 2;
    constexpr UINT height = 2;
    const std::array<std::array<uint8_t, 4>, width * height> sourcePixels{{
        {{ 0, 64, 128, 0 }},
        {{ 192, 255, 32, 85 }},
        {{ 17, 111, 229, 170 }},
        {{ 255, 1, 200, 255 }}
    }};
    const auto makeTextureDesc = [](DXGI_FORMAT format, bool allowUav)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = allowUav ?
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS :
            D3D12_RESOURCE_FLAG_NONE;
        return desc;
    };
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto inputDesc =
        makeTextureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    const auto linearDesc =
        makeTextureDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    const auto outputDesc =
        makeTextureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    Microsoft::WRL::ComPtr<ID3D12Resource> input;
    Microsoft::WRL::ComPtr<ID3D12Resource> linear;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &inputDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(input.GetAddressOf()))) ||
        FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &linearDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(linear.GetAddressOf()))) ||
        FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &outputDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(output.GetAddressOf()))))
    {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT linearFootprint{};
    UINT rows{};
    UINT64 rowBytes{};
    UINT64 uploadBytes{};
    device->GetCopyableFootprints(
        &inputDesc,
        0,
        1,
        0,
        &footprint,
        &rows,
        &rowBytes,
        &uploadBytes);
    UINT linearRows{};
    UINT64 linearRowBytes{};
    UINT64 linearReadbackBytes{};
    device->GetCopyableFootprints(
        &linearDesc,
        0,
        1,
        0,
        &linearFootprint,
        &linearRows,
        &linearRowBytes,
        &linearReadbackBytes);
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> linearReadback;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(upload.GetAddressOf()))) ||
        FAILED(device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readback.GetAddressOf()))))
    {
        return false;
    }
    D3D12_RESOURCE_DESC linearReadbackDesc = bufferDesc;
    linearReadbackDesc.Width = linearReadbackBytes;
    if (FAILED(device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &linearReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(linearReadback.GetAddressOf()))))
    {
        return false;
    }
    void* uploadData{};
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(upload->Map(0, &noRead, &uploadData))) return false;
    for (UINT row = 0; row < height; ++row)
    {
        std::memcpy(
            static_cast<uint8_t*>(uploadData) +
                footprint.Offset +
                row * footprint.Footprint.RowPitch,
            sourcePixels.data() + row * width,
            width * sizeof(sourcePixels[0]));
    }
    D3D12_RANGE writtenUpload{ 0, uploadBytes };
    upload->Unmap(0, &writtenUpload);

    D3D12_RESOURCE_DESC constantsDesc = bufferDesc;
    constantsDesc.Width =
        2 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    Microsoft::WRL::ComPtr<ID3D12Resource> constants;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &constantsDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(constants.GetAddressOf()))))
    {
        return false;
    }
    struct ConversionConstants
    {
        uint32_t width;
        uint32_t height;
        uint32_t direction;
        uint32_t unused;
    };
    void* constantData{};
    if (FAILED(constants->Map(0, &noRead, &constantData))) return false;
    const ConversionConstants decode{ width, height, 0, 0 };
    const ConversionConstants encode{ width, height, 1, 0 };
    std::memcpy(constantData, &decode, sizeof(decode));
    std::memcpy(
        static_cast<uint8_t*>(constantData) +
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        &encode,
        sizeof(encode));
    D3D12_RANGE writtenConstants{ 0, constantsDesc.Width };
    constants->Unmap(0, &writtenConstants);

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.NumDescriptors = 4;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    if (FAILED(device->CreateDescriptorHeap(
            &descriptorHeapDesc,
            IID_PPV_ARGS(descriptorHeap.GetAddressOf()))))
    {
        return false;
    }
    const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpuStart =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpuHandle = [&](UINT index)
    {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            cpuStart.ptr + index * descriptorSize
        };
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC inputSrv{};
    inputSrv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    inputSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    inputSrv.Format = inputDesc.Format;
    inputSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        input.Get(),
        &inputSrv,
        cpuHandle(0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC linearUav{};
    linearUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    linearUav.Format = linearDesc.Format;
    device->CreateUnorderedAccessView(
        linear.Get(),
        nullptr,
        &linearUav,
        cpuHandle(1));
    D3D12_SHADER_RESOURCE_VIEW_DESC linearSrv = inputSrv;
    linearSrv.Format = linearDesc.Format;
    device->CreateShaderResourceView(
        linear.Get(),
        &linearSrv,
        cpuHandle(2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = linearUav;
    outputUav.Format = outputDesc.Format;
    device->CreateUnorderedAccessView(
        output.Get(),
        nullptr,
        &outputUav,
        cpuHandle(3));

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
            pipeline.Get(),
            IID_PPV_ARGS(commandList.GetAddressOf()))) ||
        FAILED(device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()))))
    {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION uploadLocation{};
    uploadLocation.pResource = upload.Get();
    uploadLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    uploadLocation.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION inputLocation{};
    inputLocation.pResource = input.Get();
    inputLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(
        &inputLocation,
        0,
        0,
        0,
        &uploadLocation,
        nullptr);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = input.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = linear.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(2, barriers);
    commandList->SetComputeRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[]{ descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    const auto gpuStart =
        descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuHandle = [&](UINT index)
    {
        return D3D12_GPU_DESCRIPTOR_HANDLE{
            gpuStart.ptr + index * descriptorSize
        };
    };
    commandList->SetComputeRootDescriptorTable(0, gpuHandle(0));
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(1));
    commandList->SetComputeRootConstantBufferView(
        2,
        constants->GetGPUVirtualAddress());
    commandList->Dispatch(1, 1, 1);

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = linear.Get();
    barriers[1].Transition.pResource = linear.Get();
    barriers[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(2, barriers);
    D3D12_TEXTURE_COPY_LOCATION linearLocation{};
    linearLocation.pResource = linear.Get();
    linearLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION linearReadbackLocation{};
    linearReadbackLocation.pResource = linearReadback.Get();
    linearReadbackLocation.Type =
        D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    linearReadbackLocation.PlacedFootprint = linearFootprint;
    commandList->CopyTextureRegion(
        &linearReadbackLocation,
        0,
        0,
        0,
        &linearLocation,
        nullptr);
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barriers[1]);
    D3D12_RESOURCE_BARRIER outputBarrier{};
    outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outputBarrier.Transition.pResource = output.Get();
    outputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    outputBarrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputBarrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &outputBarrier);
    commandList->SetComputeRootDescriptorTable(0, gpuHandle(2));
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(3));
    commandList->SetComputeRootConstantBufferView(
        2,
        constants->GetGPUVirtualAddress() +
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    commandList->Dispatch(1, 1, 1);
    outputBarrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputBarrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &outputBarrier);
    D3D12_TEXTURE_COPY_LOCATION outputLocation{};
    outputLocation.pResource = output.Get();
    outputLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION readbackLocation{};
    readbackLocation.pResource = readback.Get();
    readbackLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readbackLocation.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(
        &readbackLocation,
        0,
        0,
        0,
        &outputLocation,
        nullptr);
    if (FAILED(commandList->Close())) return false;
    ID3D12CommandList* lists[]{ commandList.Get() };
    queue->ExecuteCommandLists(1, lists);
    // Test-only readback drain. Production resources retire through their
    // graphics ordering or the swapchain's AddRef'd last-reader fence.
    if (FAILED(queue->Signal(fence.Get(), 1)) ||
        !waitForFence(fence.Get(), 1))
    {
        return false;
    }
    void* readbackData{};
    D3D12_RANGE readRange{ 0, uploadBytes };
    if (FAILED(readback->Map(0, &readRange, &readbackData)))
    {
        return false;
    }
    void* linearReadbackData{};
    D3D12_RANGE linearReadRange{ 0, linearReadbackBytes };
    if (FAILED(linearReadback->Map(
            0,
            &linearReadRange,
            &linearReadbackData)))
    {
        readback->Unmap(0, nullptr);
        return false;
    }
    bool valid = true;
    for (UINT row = 0; row < height; ++row)
    {
        const auto* actual =
            static_cast<const uint8_t*>(readbackData) +
            footprint.Offset +
            row * footprint.Footprint.RowPitch;
        const auto* expected = reinterpret_cast<const uint8_t*>(
            sourcePixels.data() + row * width);
        for (UINT i = 0; i < width * sizeof(sourcePixels[0]); ++i)
        {
            if (std::abs(
                    static_cast<int>(actual[i]) -
                    static_cast<int>(expected[i])) > 1)
            {
                valid = false;
            }
        }
        const auto* actualLinear =
            reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(linearReadbackData) +
                linearFootprint.Offset +
                row * linearFootprint.Footprint.RowPitch);
        const auto* source =
            sourcePixels.data() + row * width;
        for (UINT pixel = 0; pixel < width; ++pixel)
        {
            for (UINT channel = 0; channel < 4; ++channel)
            {
                const float sourceValue =
                    source[pixel][channel] / 255.0f;
                const float expectedLinear =
                    channel == 3 ?
                        sourceValue :
                        std::pow(sourceValue, 2.2f);
                const float actualValue =
                    DirectX::PackedVector::XMConvertHalfToFloat(
                        actualLinear[pixel * 4 + channel]);
                const float tolerance =
                    channel == 3 ?
                        0.0006f :
                        std::max(0.0002f, expectedLinear * 0.003f);
                if (std::abs(actualValue - expectedLinear) > tolerance)
                {
                    valid = false;
                }
            }
        }
    }
    D3D12_RANGE noWrite{ 0, 0 };
    readback->Unmap(0, &noWrite);
    linearReadback->Unmap(0, &noWrite);
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

struct ApiFunctions
{
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};

    explicit operator bool() const
    {
        return create && destroy && configure && query && dispatch;
    }
};

bool createTexture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    const HRESULT result = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        state,
        nullptr,
        IID_PPV_ARGS(resource.GetAddressOf()));
    if (FAILED(result))
    {
        std::cerr << "Texture creation failed for format "
            << static_cast<uint32_t>(format) << ": 0x" << std::hex
            << static_cast<uint32_t>(result) << std::dec << '\n';
    }
    return SUCCEEDED(result);
}

bool uploadTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* texture,
    const void* data,
    size_t rowBytes,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads)
{
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowSize{};
    UINT64 totalBytes{};
    device->GetCopyableFootprints(
        &desc,
        0,
        1,
        0,
        &footprint,
        &rows,
        &rowSize,
        &totalBytes);
    if (rowBytes > rowSize || rows != desc.Height)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = totalBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(upload.GetAddressOf()))))
    {
        return false;
    }

    void* mapped{};
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(upload->Map(0, &noRead, &mapped)))
    {
        return false;
    }
    for (UINT row = 0; row < rows; ++row)
    {
        std::memcpy(
            static_cast<uint8_t*>(mapped) +
                footprint.Offset +
                static_cast<size_t>(row) * footprint.Footprint.RowPitch,
            static_cast<const uint8_t*>(data) +
                static_cast<size_t>(row) * rowBytes,
            rowBytes);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(
        &destination,
        0,
        0,
        0,
        &source,
        nullptr);
    uploads.push_back(std::move(upload));
    return true;
}

bool executeAndWait(
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Fence* fence,
    uint64_t fenceValue)
{
    if (FAILED(commandList->Close()))
    {
        return false;
    }
    ID3D12CommandList* lists[]{ commandList };
    queue->ExecuteCommandLists(1, lists);
    return SUCCEEDED(queue->Signal(fence, fenceValue)) &&
        waitForFence(fence, fenceValue);
}

bool dispatchUpscaleSequence(
    const ApiFunctions& api,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Fence* fence,
    uint64_t versionId,
    ID3D12Resource* color,
    ID3D12Resource* depth,
    ID3D12Resource* motion,
    bool useStreamlineTranslation,
    uint64_t& fenceValue,
    std::vector<uint8_t>& outputBytes)
{
    constexpr uint32_t renderWidth = 64;
    constexpr uint32_t renderHeight = 64;
    constexpr uint32_t outputWidth = 96;
    constexpr uint32_t outputHeight = 96;
    constexpr size_t outputPixelBytes = sizeof(uint16_t) * 4;

    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    if (!createTexture(
            device,
            outputWidth,
            outputHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            output))
    {
        return false;
    }

    ffxOverrideVersion overrideVersion{
        { FFX_API_DESC_TYPE_OVERRIDE_VERSION },
        versionId
    };
    ffxCreateBackendDX12Desc backend{
        {
            FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
            &overrideVersion.header
        },
        device
    };
    ffxCreateContextDescUpscaleVersion apiVersion{
        {
            FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,
            &backend.header
        },
        FFX_UPSCALER_VERSION
    };
    ffxCreateContextDescUpscale create{
        {
            FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            &apiVersion.header
        },
        FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE,
        { renderWidth, renderHeight },
        { outputWidth, outputHeight }
    };
    ffxContext context{};
    const auto createResult =
        api.create(&context, &create.header, nullptr);
    if (createResult != FFX_API_RETURN_OK)
    {
        std::cerr << "FSR jitter regression context creation failed: "
            << createResult << '\n';
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    bool valid =
        SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.GetAddressOf()))) &&
        SUCCEEDED(device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(commandList.GetAddressOf())));

    const std::array<sl::float2, 2> streamlineJitter{
        sl::float2{ -0.4375f, 0.2407407463f },
        sl::float2{ 0.46875f, -0.092592597f }
    };
    for (size_t frame = 0; valid && frame < streamlineJitter.size(); ++frame)
    {
        if (frame != 0)
        {
            valid =
                SUCCEEDED(allocator->Reset()) &&
                SUCCEEDED(commandList->Reset(allocator.Get(), nullptr));
            if (!valid) break;
        }

        ffxDispatchDescUpscale dispatch{
            { FFX_API_DISPATCH_DESC_TYPE_UPSCALE }
        };
        dispatch.commandList = commandList.Get();
        dispatch.color = ffxApiGetResourceDX12(
            color,
            FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ffxApiGetResourceDX12(
            depth,
            FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ffxApiGetResourceDX12(
            motion,
            FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output = ffxApiGetResourceDX12(
            output.Get(),
            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = useStreamlineTranslation ?
            sl::fsr::getProviderJitterOffset(streamlineJitter[frame]) :
            FfxApiFloatCoords2D{
                streamlineJitter[frame].x,
                streamlineJitter[frame].y
            };
        dispatch.motionVectorScale = {
            static_cast<float>(renderWidth),
            static_cast<float>(renderHeight)
        };
        dispatch.renderSize = { renderWidth, renderHeight };
        dispatch.upscaleSize = { outputWidth, outputHeight };
        dispatch.frameTimeDelta = 16.6667f;
        dispatch.preExposure = 1.0f;
        dispatch.reset = frame == 0;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.0f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        const auto dispatchResult =
            api.dispatch(&context, &dispatch.header);
        if (dispatchResult != FFX_API_RETURN_OK)
        {
            std::cerr << "FSR jitter regression dispatch failed at frame "
                << frame << ": " << dispatchResult << '\n';
            valid = false;
        }
        else
        {
            valid = executeAndWait(
                queue,
                commandList.Get(),
                fence,
                ++fenceValue);
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowSize{};
    UINT64 totalBytes{};
    if (valid)
    {
        const auto outputDesc = output->GetDesc();
        device->GetCopyableFootprints(
            &outputDesc,
            0,
            1,
            0,
            &footprint,
            &rows,
            &rowSize,
            &totalBytes);
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        valid = SUCCEEDED(device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readback.GetAddressOf())));
    }
    if (valid)
    {
        valid =
            SUCCEEDED(allocator->Reset()) &&
            SUCCEEDED(commandList->Reset(allocator.Get(), nullptr));
    }
    if (valid)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output.Get();
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = output.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commandList->CopyTextureRegion(
            &destination,
            0,
            0,
            0,
            &source,
            nullptr);
        valid = executeAndWait(
            queue,
            commandList.Get(),
            fence,
            ++fenceValue);
    }
    if (valid)
    {
        outputBytes.resize(
            static_cast<size_t>(outputWidth) *
            outputHeight *
            outputPixelBytes);
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        void* mapped{};
        valid = SUCCEEDED(readback->Map(0, &readRange, &mapped));
        if (valid)
        {
            const size_t rowBytes = outputWidth * outputPixelBytes;
            for (UINT row = 0; row < rows; ++row)
            {
                std::memcpy(
                    outputBytes.data() +
                        static_cast<size_t>(row) * rowBytes,
                    static_cast<const uint8_t*>(mapped) +
                        footprint.Offset +
                        static_cast<size_t>(row) *
                            footprint.Footprint.RowPitch,
                    rowBytes);
            }
            D3D12_RANGE noWrite{ 0, 0 };
            readback->Unmap(0, &noWrite);
        }
    }

    api.destroy(&context, nullptr);
    return valid;
}

bool validatesJitterTranslation(
    const ApiFunctions& api,
    ID3D12Device* device,
    uint64_t versionId)
{
    constexpr uint32_t renderWidth = 64;
    constexpr uint32_t renderHeight = 64;
    std::vector<uint16_t> color(
        static_cast<size_t>(renderWidth) * renderHeight * 4);
    std::vector<float> depth(
        static_cast<size_t>(renderWidth) * renderHeight);
    std::vector<uint16_t> motion(
        static_cast<size_t>(renderWidth) * renderHeight * 2);
    for (uint32_t y = 0; y < renderHeight; ++y)
    {
        for (uint32_t x = 0; x < renderWidth; ++x)
        {
            const size_t pixel =
                static_cast<size_t>(y) * renderWidth + x;
            const float checker =
                ((x / 4 + y / 4) & 1) ? 0.85f : 0.05f;
            color[pixel * 4 + 0] =
                DirectX::PackedVector::XMConvertFloatToHalf(checker);
            color[pixel * 4 + 1] =
                DirectX::PackedVector::XMConvertFloatToHalf(
                    static_cast<float>(x) / (renderWidth - 1));
            color[pixel * 4 + 2] =
                DirectX::PackedVector::XMConvertFloatToHalf(
                    static_cast<float>(y) / (renderHeight - 1));
            color[pixel * 4 + 3] =
                DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            depth[pixel] = 0.2f +
                0.6f * static_cast<float>(y) / (renderHeight - 1);
            motion[pixel * 2 + 0] =
                DirectX::PackedVector::XMConvertFloatToHalf(0.0f);
            motion[pixel * 2 + 1] =
                DirectX::PackedVector::XMConvertFloatToHalf(0.0f);
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> colorTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> motionTexture;
    if (!createTexture(
            device,
            renderWidth,
            renderHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            colorTexture) ||
        !createTexture(
            device,
            renderWidth,
            renderHeight,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            depthTexture) ||
        !createTexture(
            device,
            renderWidth,
            renderHeight,
            DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            motionTexture))
    {
        std::cerr << "FSR jitter regression input texture creation failed\n";
        return false;
    }

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
            nullptr,
            IID_PPV_ARGS(commandList.GetAddressOf()))) ||
        FAILED(device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()))))
    {
        std::cerr << "FSR jitter regression command objects failed\n";
        return false;
    }

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploads;
    if (!uploadTexture(
            device,
            commandList.Get(),
            colorTexture.Get(),
            color.data(),
            renderWidth * sizeof(uint16_t) * 4,
            uploads) ||
        !uploadTexture(
            device,
            commandList.Get(),
            depthTexture.Get(),
            depth.data(),
            renderWidth * sizeof(float),
            uploads) ||
        !uploadTexture(
            device,
            commandList.Get(),
            motionTexture.Get(),
            motion.data(),
            renderWidth * sizeof(uint16_t) * 2,
            uploads))
    {
        std::cerr << "FSR jitter regression input upload setup failed\n";
        return false;
    }
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers{};
    const std::array<ID3D12Resource*, 3> resources{
        colorTexture.Get(),
        depthTexture.Get(),
        motionTexture.Get()
    };
    for (size_t i = 0; i < barriers.size(); ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = resources[i];
        barriers[i].Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        static_cast<UINT>(barriers.size()),
        barriers.data());
    uint64_t fenceValue = 1;
    if (!executeAndWait(
            queue.Get(),
            commandList.Get(),
            fence.Get(),
            fenceValue))
    {
        std::cerr << "FSR jitter regression input upload execution failed\n";
        return false;
    }
    uploads.clear();

    std::vector<uint8_t> directOutput;
    std::vector<uint8_t> translatedOutput;
    const bool directValid = dispatchUpscaleSequence(
        api,
        device,
        queue.Get(),
        fence.Get(),
        versionId,
        colorTexture.Get(),
        depthTexture.Get(),
        motionTexture.Get(),
        false,
        fenceValue,
        directOutput);
    const bool translatedValid = dispatchUpscaleSequence(
        api,
        device,
        queue.Get(),
        fence.Get(),
        versionId,
        colorTexture.Get(),
        depthTexture.Get(),
        motionTexture.Get(),
        true,
        fenceValue,
        translatedOutput);
    if (!directValid || !translatedValid ||
        directOutput.size() != translatedOutput.size())
    {
        std::cerr << "FSR jitter regression output failed: direct="
            << directValid << ", translated=" << translatedValid
            << ", direct-bytes=" << directOutput.size()
            << ", translated-bytes=" << translatedOutput.size() << '\n';
        return false;
    }
    size_t differences{};
    for (size_t i = 0; i < directOutput.size(); ++i)
    {
        differences += directOutput[i] != translatedOutput[i];
    }
    if (differences != 0)
    {
        std::cerr << "FSR production jitter translation changed "
            << differences << " GPU output byte(s)\n";
        return false;
    }
    return true;
}

ApiFunctions getApi(HMODULE module)
{
    return {
        reinterpret_cast<PfnFfxCreateContext>(
            GetProcAddress(module, "ffxCreateContext")),
        reinterpret_cast<PfnFfxDestroyContext>(
            GetProcAddress(module, "ffxDestroyContext")),
        reinterpret_cast<PfnFfxConfigure>(
            GetProcAddress(module, "ffxConfigure")),
        reinterpret_cast<PfnFfxQuery>(
            GetProcAddress(module, "ffxQuery")),
        reinterpret_cast<PfnFfxDispatch>(
            GetProcAddress(module, "ffxDispatch"))
    };
}

bool isAmdAdapter(ID3D12Device* device)
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    return device &&
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(
            device->GetAdapterLuid(),
            IID_PPV_ARGS(adapter.GetAddressOf()))) &&
        SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.VendorId == 0x1002;
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 7)
    {
        std::cerr << "usage: fsr-provider-runtime-regression "
            "<project-upscaler> <project-frame-generation> "
            "<official-upscaler> <official-frame-generation> "
            "<fsr3-color-transfer-shader> "
            "<ml-color-adapter-shader>\n";
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
    HMODULE officialUpscaler = LoadLibraryExW(
        argv[3],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    HMODULE officialFrameGeneration = LoadLibraryExW(
        argv[4],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!upscaler || !frameGeneration ||
        !officialUpscaler || !officialFrameGeneration)
    {
        std::cerr << "Failed to load FidelityFX runtime modules: "
            << GetLastError() << '\n';
        return 1;
    }

    const ApiFunctions upscalerApi = getApi(upscaler);
    const ApiFunctions frameGenerationApi = getApi(frameGeneration);
    const ApiFunctions officialUpscalerApi = getApi(officialUpscaler);
    const ApiFunctions officialFrameGenerationApi =
        getApi(officialFrameGeneration);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const HRESULT deviceResult = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(device.GetAddressOf()));
    uint64_t upscalerVersionId{};
    uint64_t swapchainVersionId{};
    const bool providersValid =
        upscalerApi &&
        frameGenerationApi &&
        officialUpscalerApi &&
        officialFrameGenerationApi &&
        SUCCEEDED(deviceResult) &&
        findVersion(
            upscalerApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            device.Get(),
            "3.1.5",
            &upscalerVersionId) &&
        findVersion(
            frameGenerationApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            device.Get(),
            "3.1.6") &&
        findVersion(
            frameGenerationApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12,
            device.Get(),
            "3.1.7") &&
        findVersion(
            frameGenerationApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
            device.Get(),
            "3.1.7",
            &swapchainVersionId);
    const bool fsr4Available =
        providersValid &&
        findVersion(
            officialUpscalerApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            device.Get(),
            "4.1.1");
    const bool mlfgAvailable =
        providersValid &&
        findVersion(
            officialFrameGenerationApi.query,
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            device.Get(),
            "4.0.1");
    const auto fsr4UnavailableReason =
        sl::fsr::getMachineLearningUnavailableReason(
            device.Get(),
            true,
            fsr4Available,
            false);
    const auto mlfgUnavailableReason =
        sl::fsr::getMachineLearningUnavailableReason(
            device.Get(),
            true,
            mlfgAvailable,
            true);
    const auto d3d12Runtime =
        sl::fsr::getD3D12RuntimeCapabilities(device.Get());
    sl::FSRCapabilities publicRuntime{};
    sl::fsr::setD3D12RuntimeCapabilities(
        device.Get(),
        publicRuntime);
    const bool shaderModel66 =
        d3d12Runtime.shaderModelMajor > 6 ||
        (d3d12Runtime.shaderModelMajor == 6 &&
         d3d12Runtime.shaderModelMinor >= 6);
    const bool runtimeCapabilitiesValid =
        d3d12Runtime.runtimeSource !=
            sl::FSRD3D12RuntimeSource::eUnknown &&
        d3d12Runtime.coreVersionMajor != 0 &&
        publicRuntime.windows11OrGreater ==
            (d3d12Runtime.windows11OrGreater ?
                sl::Boolean::eTrue :
                sl::Boolean::eFalse) &&
        publicRuntime.shaderModelMajor ==
            d3d12Runtime.shaderModelMajor &&
        publicRuntime.shaderModelMinor ==
            d3d12Runtime.shaderModelMinor &&
        publicRuntime.d3d12RuntimeSource ==
            d3d12Runtime.runtimeSource &&
        publicRuntime.d3d12CoreVersionMajor ==
            d3d12Runtime.coreVersionMajor &&
        publicRuntime.d3d12CoreVersionMinor ==
            d3d12Runtime.coreVersionMinor &&
        (!fsr4Available || shaderModel66) &&
        (!mlfgAvailable ||
         (d3d12Runtime.windows11OrGreater && shaderModel66));
    const auto isSpecificUnavailableReason =
        [](sl::FSRUnavailableReason reason)
        {
            return reason ==
                    sl::FSRUnavailableReason::eOperatingSystemUnsupported ||
                reason == sl::FSRUnavailableReason::eRuntimeUnsupported ||
                reason == sl::FSRUnavailableReason::eHardwareUnsupported;
        };
    const bool unavailableReasonValid =
        isAmdAdapter(device.Get()) ||
        ((fsr4Available ||
          isSpecificUnavailableReason(fsr4UnavailableReason)) &&
         (mlfgAvailable ||
          isSpecificUnavailableReason(mlfgUnavailableReason)));
    const bool colorValid =
        providersValid && validatesColorContract(device.Get(), argv[5]);
    const bool gamma22LinearAdapterValid =
        validatesGamma22LinearAdapter(device.Get(), argv[6]);
    const bool jitterTranslationValid =
        providersValid && validatesJitterTranslation(
            upscalerApi,
            device.Get(),
            upscalerVersionId);
    const bool completionValid =
        providersValid && validatesCompletionLifecycle(
            frameGenerationApi.create,
            frameGenerationApi.destroy,
            frameGenerationApi.query,
            device.Get(),
            swapchainVersionId);
    const bool valid =
        providersValid &&
        runtimeCapabilitiesValid &&
        unavailableReasonValid &&
        colorValid &&
        gamma22LinearAdapterValid &&
        completionValid &&
        jitterTranslationValid;

    FreeLibrary(officialFrameGeneration);
    FreeLibrary(officialUpscaler);
    FreeLibrary(frameGeneration);
    FreeLibrary(upscaler);
    if (!valid)
    {
        std::cerr << "Contract status: providers=" << providersValid
            << ", runtime-capabilities=" << runtimeCapabilitiesValid
            << ", unavailable-reason=" << unavailableReasonValid
            << ", fsr4=" << fsr4Available
            << ", mlfg=" << mlfgAvailable
            << ", fsr4-reason="
            << static_cast<uint32_t>(fsr4UnavailableReason)
            << ", mlfg-reason="
            << static_cast<uint32_t>(mlfgUnavailableReason)
            << ", color=" << colorValid
            << ", gamma22-linear-adapter="
            << gamma22LinearAdapterValid
            << ", completion=" << completionValid
            << ", jitter-translation=" << jitterTranslationValid << '\n';
        return 1;
    }
    std::cout << "FidelityFX direct provider discovery, ML capability gating, "
        "active D3D12 runtime, FSR3 color transfer, Gamma 2.2 linear adapter, "
        "completion, and production jitter translation passed. "
        "D3D12Core=" << d3d12Runtime.coreVersionMajor << '.'
        << d3d12Runtime.coreVersionMinor << '.'
        << d3d12Runtime.coreVersionPatch << '.'
        << d3d12Runtime.coreVersionRevision << ", SM="
        << d3d12Runtime.shaderModelMajor << '.'
        << d3d12Runtime.shaderModelMinor << ", requested SDK="
        << d3d12Runtime.requestedSDKVersion << ", FSR4="
        << fsr4Available << " (reason "
        << static_cast<uint32_t>(fsr4UnavailableReason)
        << "), MLFG=" << mlfgAvailable << " (reason "
        << static_cast<uint32_t>(mlfgUnavailableReason) << ").\n";
    return 0;
}
