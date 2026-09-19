#include "source/core/sl.security/projectTrust.h"
#include "include/sl.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace sl::security;

namespace
{

struct DirectoryResolver
{
    fs::path directory;
};

bool resolveFromDirectory(
    void* context,
    std::wstring_view basename,
    std::wstring& path)
{
    const auto& resolver = *static_cast<DirectoryResolver*>(context);
    path = (resolver.directory / std::wstring(basename)).wstring();
    return true;
}

Microsoft::WRL::ComPtr<ID3D12Device> createD3D12Device()
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (SUCCEEDED(D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.GetAddressOf()))))
    {
        return device;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) &&
        SUCCEEDED(factory->EnumWarpAdapter(
            IID_PPV_ARGS(warpAdapter.GetAddressOf()))) &&
        SUCCEEDED(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.ReleaseAndGetAddressOf()))))
    {
        return device;
    }
    return {};
}

bool verifyEvaluationRoutes(
    PFun_slSetFeatureLoaded* setFeatureLoaded,
    PFun_slSetD3DDevice* setD3DDevice,
    PFun_slGetNewFrameToken* getNewFrameToken,
    PFun_slEvaluateFeature* evaluate)
{
    const auto device = createD3D12Device();
    if (!device)
    {
        std::cerr << "A D3D12 hardware or WARP device is unavailable\n";
        return false;
    }
    const auto disableDLSSG =
        setFeatureLoaded(sl::kFeatureDLSS_G, false);
    if (disableDLSSG != sl::Result::eOk)
    {
        std::cerr << "Failed to disable DLSS frame generation before the FSR "
            "frame-generation route check (SDK "
            << static_cast<int>(disableDLSSG) << ")\n";
        return false;
    }
    const auto deviceResult = setD3DDevice(device.Get());
    if (deviceResult != sl::Result::eOk)
    {
        std::cerr << "Streamline D3D12 device registration failed (SDK "
            << static_cast<int>(deviceResult) << ")\n";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(commandList.GetAddressOf()))))
    {
        std::cerr << "Failed to create the D3D12 evaluation command list\n";
        return false;
    }

    uint32_t frameIndex = 1;
    sl::FrameToken* frame{};
    const auto frameResult = getNewFrameToken(frame, &frameIndex);
    if (frameResult != sl::Result::eOk || !frame)
    {
        std::cerr << "Failed to obtain a Streamline frame token (SDK "
            << static_cast<int>(frameResult) << ")\n";
        return false;
    }

    const sl::ViewportHandle viewport{ 0u };
    const sl::BaseStructure* inputs[]{ &viewport };
    bool valid = true;
    for (const auto feature : { sl::kFeatureFSR, sl::kFeatureFSR_G })
    {
        const auto result = evaluate(
            feature,
            *frame,
            inputs,
            static_cast<uint32_t>(std::size(inputs)),
            commandList.Get());
        if (result != sl::Result::eErrorInvalidState)
        {
            std::cerr << "FSR public evaluation route did not reach feature "
                << feature << " (SDK " << static_cast<int>(result) << ")\n";
            valid = false;
        }
    }
    return valid;
}

bool initializePlugins(HMODULE module, const fs::path& directory)
{
    const auto initialize = reinterpret_cast<PFun_slInit*>(
        GetProcAddress(module, "slInit"));
    const auto shutdown = reinterpret_cast<PFun_slShutdown*>(
        GetProcAddress(module, "slShutdown"));
    const auto getRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(
        GetProcAddress(module, "slGetFeatureRequirements"));
    const auto setFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(
        GetProcAddress(module, "slSetFeatureLoaded"));
    const auto setD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(
        GetProcAddress(module, "slSetD3DDevice"));
    const auto getNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(
        GetProcAddress(module, "slGetNewFrameToken"));
    const auto evaluate = reinterpret_cast<PFun_slEvaluateFeature*>(
        GetProcAddress(module, "slEvaluateFeature"));
    if (!initialize || !shutdown || !getRequirements || !setFeatureLoaded ||
        !setD3DDevice || !getNewFrameToken || !evaluate)
    {
        std::cerr << "Streamline initialization exports are missing\n";
        return false;
    }

    const std::array features{
        sl::kFeatureDLSS, sl::kFeatureDLSS_G,
        sl::kFeatureFSR, sl::kFeatureFSR_G,
        sl::kFeaturePCL, sl::kFeatureReflex
    };
    const auto path = directory.wstring();
    const wchar_t* paths[]{ path.c_str() };
    sl::Preferences preferences{};
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0.0";
    preferences.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.logLevel = sl::LogLevel::eOff;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.logMessageCallback = [](sl::LogType, const char* message) {
        std::cerr << message;
    };
    const auto result = initialize(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk)
    {
        std::cerr << "Streamline plugin initialization failed (SDK "
            << static_cast<int>(result) << ")\n";
        return false;
    }

    bool valid = true;
    for (const auto feature : { sl::kFeatureFSR, sl::kFeatureFSR_G })
    {
        sl::FeatureRequirements requirements{};
        const auto query = getRequirements(feature, requirements);
        if (query != sl::Result::eOk ||
            !(requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported))
        {
            std::cerr << "FSR plugin metadata was not registered for feature "
                << feature << " (SDK " << static_cast<int>(query) << ")\n";
            valid = false;
        }
    }
    if (valid &&
        !verifyEvaluationRoutes(
            setFeatureLoaded,
            setD3DDevice,
            getNewFrameToken,
            evaluate))
    {
        valid = false;
    }
    const auto stopped = shutdown();
    if (stopped != sl::Result::eOk)
    {
        std::cerr << "Streamline shutdown failed (SDK "
            << static_cast<int>(stopped) << ")\n";
        valid = false;
    }
    if (valid)
    {
        std::cout << "Streamline plugin initialization, FSR metadata, and "
            "public evaluation routes passed\n";
    }
    return valid;
}

}

int wmain(int argc, wchar_t** argv)
{
    const bool initialize =
        argc == 3 && std::wstring_view(argv[2]) == L"--initialize";
    if (argc != 2 && !initialize)
    {
        std::cerr << "usage: project-release-verifier <bin-directory> [--initialize]\n";
        return 2;
    }

    const fs::path directory = fs::absolute(argv[1]);
    DirectoryResolver resolver{ directory };
    const std::wstring manifest =
        (directory / kProjectManifestName).wstring();
    const std::wstring signature =
        (directory / kProjectManifestSignatureName).wstring();
    const ProjectLoadOptions options
    {
        L"sl.interposer.dll",
        manifest,
        signature,
        resolveFromDirectory,
        &resolver,
        &getCompiledProjectTrust(),
        !initialize
    };
    const ProjectLoadResult result =
        authenticateAndLoadProjectLibrary(options);
    if (!result)
    {
        std::cerr << getTrustFailureMessage(result.failure)
            << " (system error " << result.systemError << ")\n";
        return 1;
    }
    return !initialize || initializePlugins(result.module, directory) ? 0 : 1;
}
