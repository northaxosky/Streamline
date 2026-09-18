#include "source/core/sl.security/projectTrust.h"
#include "include/sl.h"

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

bool initializePlugins(HMODULE module, const fs::path& directory)
{
    const auto initialize = reinterpret_cast<PFun_slInit*>(
        GetProcAddress(module, "slInit"));
    const auto shutdown = reinterpret_cast<PFun_slShutdown*>(
        GetProcAddress(module, "slShutdown"));
    const auto getRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(
        GetProcAddress(module, "slGetFeatureRequirements"));
    if (!initialize || !shutdown || !getRequirements)
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
    const auto stopped = shutdown();
    if (stopped != sl::Result::eOk)
    {
        std::cerr << "Streamline shutdown failed (SDK "
            << static_cast<int>(stopped) << ")\n";
        valid = false;
    }
    if (valid)
    {
        std::cout << "Streamline plugin initialization and FSR metadata passed\n";
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
