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

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <windows.h>

namespace sl::security
{

inline constexpr wchar_t kProjectManifestName[] = L"sl.project-manifest.bin";
inline constexpr wchar_t kProjectManifestSignatureName[] = L"sl.project-manifest.sig";

enum class ProjectFileRole : uint8_t
{
    eProjectInterposer = 1,
    eProjectCommon = 2,
    eNvidiaModule = 3,
    eProjectPlugin = 4,
    eAmdModule = 5,
    eProjectVendorModule = 6,
};

enum class TrustFailure : uint32_t
{
    eOk = 0,
    eInvalidArgument,
    eManifestIncomplete,
    eManifestIo,
    eManifestTooLarge,
    eManifestMalformed,
    eManifestVersionUnsupported,
    eManifestUnknownKey,
    eManifestReleaseMismatch,
    eProjectTrustNotConfigured,
    eSignatureInvalid,
    eRequestedFileNotApproved,
    eResolutionFailed,
    eFileOpenFailed,
    eFileSizeMismatch,
    eFileHashMismatch,
    eNvidiaSignatureInvalid,
    eAmdSignatureInvalid,
    eLoadFailed,
};

struct ProjectTrustConfig
{
    bool configured{};
    uint32_t keyId{};
    // Raw P-256 affine coordinates, X followed by Y, 32-byte big-endian each.
    std::array<uint8_t, 64> publicKeyXY{};
    std::string_view expectedReleaseId{};
};

// The resolver is called once for every logical basename in the signed
// manifest. It must return the independently selected physical winner for that
// logical file. This permits a game adapter to resolve each entry through its
// own virtual-file-system helper instead of assuming a common physical parent.
using ResolveProjectFile = bool(*)(
    void* context,
    std::wstring_view logicalBasename,
    std::wstring& physicalPath);

// Intended for deterministic tests and diagnostics. It runs after all package
// files have been authenticated and while their restrictive handles remain
// open, immediately before LoadLibraryExW. Production callers should leave it
// null and must never execute untrusted code from this callback.
using BeforeProjectLoad = void(*)(void* context, std::wstring_view physicalPath);

struct ProjectLoadOptions
{
    std::wstring_view requestedBasename;
    std::wstring_view manifestPath;
    std::wstring_view signaturePath;
    ResolveProjectFile resolve{};
    void* resolveContext{};
    const ProjectTrustConfig* trust{};
    bool authenticateOnly{};
    BeforeProjectLoad beforeLoad{};
    void* beforeLoadContext{};
};

struct ProjectLoadResult
{
    TrustFailure failure{ TrustFailure::eInvalidArgument };
    DWORD systemError{};
    HMODULE module{};

    explicit operator bool() const { return failure == TrustFailure::eOk; }
};

// Authenticates the exact manifest bytes and every file bound by it. All file
// handles are retained until the requested DLL has been loaded (or until an
// authenticate-only request returns). Windows has no load-from-handle API; the
// restrictive handles close the authenticate/reopen replacement window while
// LoadLibraryExW opens the verified full physical path.
ProjectLoadResult authenticateAndLoadProjectLibrary(const ProjectLoadOptions& options);
ProjectLoadResult authenticateAndLoadNvidiaLibrary(
    std::wstring_view physicalPath,
    bool authenticateOnly = false);

const ProjectTrustConfig& getCompiledProjectTrust();
const char* getTrustFailureMessage(TrustFailure failure);
bool verifyNvidiaEmbeddedSignature(const wchar_t* fullPath);

}
