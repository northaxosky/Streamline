/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
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

#include "source/core/sl.security/projectTrust.h"

#include <filesystem>
#include <string>

namespace sl
{

namespace security
{

namespace
{

namespace fs = std::filesystem;

struct DirectoryResolver
{
    fs::path directory;
    std::wstring requestedName;
    fs::path requestedPath;
};

bool resolveFromDirectory(
    void* context,
    std::wstring_view logicalBasename,
    std::wstring& physicalPath)
{
    auto& resolver = *static_cast<DirectoryResolver*>(context);
    physicalPath = logicalBasename == resolver.requestedName ?
        resolver.requestedPath.wstring() :
        (resolver.directory / std::wstring(logicalBasename)).wstring();
    return true;
}

bool isMissing(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND ||
        error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME;
}

bool pathExists(const fs::path& path, DWORD& error)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring lowerAscii(std::wstring value)
{
    for (wchar_t& c : value)
    {
        if (c >= L'A' && c <= L'Z')
        {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return value;
}

}

HMODULE loadLibrary(const wchar_t* path, TrustFailure* failureOut)
{
    if (failureOut)
    {
        *failureOut = TrustFailure::eInvalidArgument;
    }
    if (!path || !*path)
    {
        return nullptr;
    }

#ifdef SL_PRODUCTION
    std::error_code error;
    fs::path requestedPath = fs::absolute(path, error);
    if (error)
    {
        return nullptr;
    }
    requestedPath = requestedPath.lexically_normal();
    const fs::path directory = requestedPath.parent_path();
    const fs::path manifestPath = directory / kProjectManifestName;
    const fs::path signaturePath = directory / kProjectManifestSignatureName;
    DWORD manifestError{};
    DWORD signatureError{};
    const bool haveManifest = pathExists(manifestPath, manifestError);
    const bool haveSignature = pathExists(signaturePath, signatureError);

    if (!haveManifest && !haveSignature &&
        isMissing(manifestError) && isMissing(signatureError))
    {
        // Preserve the original Production behavior when no community package
        // is selected: only the existing NVIDIA Authenticode policy may load.
        const ProjectLoadResult result =
            authenticateAndLoadNvidiaLibrary(requestedPath.wstring());
        if (failureOut)
        {
            *failureOut = result.failure;
        }
        return result.module;
    }
    if (!haveManifest || !haveSignature)
    {
        if (failureOut)
        {
            *failureOut = TrustFailure::eManifestIncomplete;
        }
        return nullptr;
    }

    DirectoryResolver resolver
    {
        directory,
        lowerAscii(requestedPath.filename().wstring()),
        requestedPath
    };
    const std::wstring manifest = manifestPath.wstring();
    const std::wstring signature = signaturePath.wstring();
    const ProjectLoadOptions options
    {
        resolver.requestedName,
        manifest,
        signature,
        resolveFromDirectory,
        &resolver,
        &getCompiledProjectTrust()
    };
    ProjectLoadResult result = authenticateAndLoadProjectLibrary(options);
    if (failureOut)
    {
        *failureOut = result.failure;
    }
    return result.module;
#else
    HMODULE module = LoadLibraryW(path);
    if (failureOut)
    {
        *failureOut = module ? TrustFailure::eOk : TrustFailure::eLoadFailed;
    }
    return module;
#endif
}

}
}
