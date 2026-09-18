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

#include "providerCapabilities.h"

#include <dxgi1_6.h>
#include <filesystem>
#include <tuple>
#include <vector>
#include <wrl/client.h>

namespace sl::fsr
{

namespace
{

bool isWindows11OrGreater()
{
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto getVersion = module ?
        reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(module, "RtlGetVersion")) :
        nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return getVersion &&
        getVersion(&version) == 0 &&
        (version.dwMajorVersion > 10 ||
         (version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000));
}

D3D_SHADER_MODEL getHighestShaderModel(ID3D12Device* device)
{
    if (!device) return D3D_SHADER_MODEL_5_1;
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{
        D3D_SHADER_MODEL_6_6
    };
    if (FAILED(device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL,
        &shaderModel,
        sizeof(shaderModel))))
    {
        return D3D_SHADER_MODEL_5_1;
    }
    return shaderModel.HighestShaderModel;
}

bool isAmdAdapter(ID3D12Device* device)
{
    if (!device) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(
            device->GetAdapterLuid(),
            IID_PPV_ARGS(adapter.GetAddressOf()))) &&
        SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.VendorId == 0x1002;
}

bool getFileVersion(
    const std::filesystem::path& path,
    D3D12RuntimeCapabilities& capabilities)
{
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return false;
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return false;
    }
    VS_FIXEDFILEINFO* version{};
    UINT versionSize{};
    if (!VerQueryValueW(
            data.data(),
            L"\\",
            reinterpret_cast<void**>(&version),
            &versionSize) ||
        !version ||
        versionSize < sizeof(*version))
    {
        return false;
    }
    capabilities.coreVersionMajor = HIWORD(version->dwFileVersionMS);
    capabilities.coreVersionMinor = LOWORD(version->dwFileVersionMS);
    capabilities.coreVersionPatch = HIWORD(version->dwFileVersionLS);
    capabilities.coreVersionRevision = LOWORD(version->dwFileVersionLS);
    return true;
}

bool isSystemD3D12Core(const std::filesystem::path& path)
{
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    const auto systemCore =
        std::filesystem::path(systemDirectory) / L"D3D12Core.dll";
    return _wcsicmp(path.c_str(), systemCore.c_str()) == 0;
}

bool isBelowMlfgAgilityFloor(
    const D3D12RuntimeCapabilities& capabilities)
{
    if (capabilities.runtimeSource !=
        FSRD3D12RuntimeSource::eAgilitySDK)
    {
        return false;
    }
    const auto version = std::tuple{
        capabilities.coreVersionMajor,
        capabilities.coreVersionMinor,
        capabilities.coreVersionPatch
    };
    return version < std::tuple{ 1u, 4u, 9u };
}

}

D3D12RuntimeCapabilities getD3D12RuntimeCapabilities(
    ID3D12Device* device)
{
    D3D12RuntimeCapabilities capabilities{};
    capabilities.windows11OrGreater = isWindows11OrGreater();

    const auto shaderModel = getHighestShaderModel(device);
    capabilities.shaderModelMajor =
        (static_cast<uint32_t>(shaderModel) >> 4) & 0xf;
    capabilities.shaderModelMinor =
        static_cast<uint32_t>(shaderModel) & 0xf;

    const auto executable = GetModuleHandleW(nullptr);
    if (const auto exportedVersion = executable ?
            GetProcAddress(executable, "D3D12SDKVersion") :
            nullptr)
    {
        capabilities.requestedSDKVersion =
            *reinterpret_cast<const uint32_t*>(exportedVersion);
    }

    const auto core = GetModuleHandleW(L"D3D12Core.dll");
    wchar_t corePath[MAX_PATH]{};
    if (core &&
        GetModuleFileNameW(core, corePath, MAX_PATH) > 0)
    {
        const std::filesystem::path path(corePath);
        capabilities.runtimeSource = isSystemD3D12Core(path) ?
            FSRD3D12RuntimeSource::eSystem :
            FSRD3D12RuntimeSource::eAgilitySDK;
        getFileVersion(path, capabilities);
    }
    return capabilities;
}

FSRUnavailableReason getMachineLearningUnavailableReason(
    ID3D12Device* device,
    bool moduleAvailable,
    bool providerAvailable,
    bool requiresWindows11)
{
    if (!moduleAvailable) return FSRUnavailableReason::eModuleUnavailable;
    const auto runtime = getD3D12RuntimeCapabilities(device);
    if (requiresWindows11 && !runtime.windows11OrGreater)
    {
        return FSRUnavailableReason::eOperatingSystemUnsupported;
    }
    if (runtime.shaderModelMajor < 6 ||
        (runtime.shaderModelMajor == 6 &&
         runtime.shaderModelMinor < 6) ||
        (requiresWindows11 && isBelowMlfgAgilityFloor(runtime)))
    {
        return FSRUnavailableReason::eRuntimeUnsupported;
    }
    if (providerAvailable) return FSRUnavailableReason::eNone;
    if (!isAmdAdapter(device))
    {
        return FSRUnavailableReason::eHardwareUnsupported;
    }
    return FSRUnavailableReason::eProviderUnavailable;
}

}
