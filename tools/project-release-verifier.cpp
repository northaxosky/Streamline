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

constexpr int kGpuUnavailableExitCode = 77;
constexpr UINT kAmdVendorId = 0x1002;
constexpr UINT kNvidiaVendorId = 0x10de;
constexpr UINT kIntelVendorId = 0x8086;
constexpr UINT kNvidiaAcpiVendorId = 0x4144564e;

enum class VerificationMode
{
    ePortable,
    eInitialize,
    eInitializeIfSupported
};

enum class HardwareStatus
{
    eAvailable,
    eUnavailable,
    eError
};

struct DirectoryResolver
{
    fs::path directory;
};

struct HardwareDevice
{
    HardwareStatus status{ HardwareStatus::eError };
    Microsoft::WRL::ComPtr<ID3D12Device> device;
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

bool isSupportedPhysicalAdapter(const DXGI_ADAPTER_DESC1& description)
{
    if (description.Flags &
        (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE))
    {
        return false;
    }
    return description.VendorId == kIntelVendorId ||
        description.VendorId == kAmdVendorId ||
        description.VendorId == kNvidiaVendorId ||
        description.VendorId == kNvidiaAcpiVendorId;
}

HardwareDevice createHardwareD3D12Device()
{
    HardwareDevice hardware;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    const auto factoryResult =
        CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(factoryResult))
    {
        std::cerr << "DXGI adapter factory creation failed (HRESULT "
            << factoryResult << ")\n";
        return hardware;
    }

    for (UINT index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto enumerationResult =
            factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enumerationResult == DXGI_ERROR_NOT_FOUND)
        {
            hardware.status = HardwareStatus::eUnavailable;
            return hardware;
        }
        if (FAILED(enumerationResult))
        {
            std::cerr << "DXGI adapter enumeration failed (HRESULT "
                << enumerationResult << ")\n";
            return hardware;
        }

        DXGI_ADAPTER_DESC1 description{};
        const auto descriptionResult = adapter->GetDesc1(&description);
        if (FAILED(descriptionResult))
        {
            std::cerr << "DXGI adapter description query failed (HRESULT "
                << descriptionResult << ")\n";
            return hardware;
        }
        if (!isSupportedPhysicalAdapter(description))
        {
            continue;
        }

        const auto deviceResult = D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(hardware.device.GetAddressOf()));
        if (FAILED(deviceResult))
        {
            std::cerr << "D3D12 device creation failed for a supported "
                "physical GPU (HRESULT " << deviceResult << ")\n";
            return hardware;
        }
        hardware.status = HardwareStatus::eAvailable;
        return hardware;
    }
}

bool parseMode(int argc, wchar_t** argv, VerificationMode& mode)
{
    if (argc == 2)
    {
        mode = VerificationMode::ePortable;
        return true;
    }
    if (argc != 3)
    {
        return false;
    }
    const std::wstring_view option = argv[2];
    if (option == L"--initialize")
    {
        mode = VerificationMode::eInitialize;
        return true;
    }
    if (option == L"--initialize-if-supported")
    {
        mode = VerificationMode::eInitializeIfSupported;
        return true;
    }
    return false;
}

bool verifyEvaluationRoutes(
    PFun_slSetFeatureLoaded* setFeatureLoaded,
    PFun_slSetD3DDevice* setD3DDevice,
    PFun_slGetNewFrameToken* getNewFrameToken,
    PFun_slEvaluateFeature* evaluate,
    ID3D12Device* device)
{
    const auto disableDLSSG =
        setFeatureLoaded(sl::kFeatureDLSS_G, false);
    if (disableDLSSG != sl::Result::eOk)
    {
        std::cerr << "Failed to disable DLSS frame generation before the FSR "
            "frame-generation route check (SDK "
            << static_cast<int>(disableDLSSG) << ")\n";
        return false;
    }
    const auto deviceResult = setD3DDevice(device);
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

bool initializePlugins(
    HMODULE module,
    const fs::path& directory,
    ID3D12Device* device)
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
            evaluate,
            device))
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
    VerificationMode mode{};
    if (!parseMode(argc, argv, mode))
    {
        std::cerr << "usage: project-release-verifier <bin-directory> "
            "[--initialize|--initialize-if-supported]\n";
        return 2;
    }

    const bool initialize = mode != VerificationMode::ePortable;
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
    if (!initialize)
    {
        std::cout << "Authenticated manifest, signature, and package payloads "
            "passed\n";
        return 0;
    }

    const auto hardware = createHardwareD3D12Device();
    if (hardware.status == HardwareStatus::eUnavailable)
    {
        if (mode == VerificationMode::eInitializeIfSupported)
        {
            std::cerr << "SKIPPED: authenticated package passed; GPU "
                "initialization not run because no supported physical Intel, "
                "AMD, or NVIDIA GPU is available\n";
            return kGpuUnavailableExitCode;
        }
        std::cerr << "Streamline GPU initialization requires a supported "
            "physical Intel, AMD, or NVIDIA GPU\n";
        return 1;
    }
    if (hardware.status == HardwareStatus::eError)
    {
        return 1;
    }
    return initializePlugins(result.module, directory, hardware.device.Get())
        ? 0
        : 1;
}
