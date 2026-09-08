#include "source/core/sl.security/projectTrust.h"

#include <filesystem>
#include <iostream>
#include <string>

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

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: project-release-verifier <bin-directory>\n";
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
        true
    };
    const ProjectLoadResult result =
        authenticateAndLoadProjectLibrary(options);
    if (!result)
    {
        std::cerr << getTrustFailureMessage(result.failure)
            << " (system error " << result.systemError << ")\n";
        return 1;
    }
    return 0;
}
