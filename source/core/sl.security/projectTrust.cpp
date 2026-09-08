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

#include "include/sl_security.h"
#include "source/core/sl.security/physicalFilePath.h"

#include <algorithm>
#include <bcrypt.h>
#include <limits>
#include <memory>
#include <vector>

#ifdef SL_PROJECT_TRUST_TEST_CONFIG
#include "projectTrustTestConfig.h"
#elif defined(SL_PROJECT_TRUST_CONFIG_HEADER)
#include SL_PROJECT_TRUST_CONFIG_HEADER
#else
#include "source/core/sl.security/projectTrustConfig.h"
#endif

#pragma comment(lib, "bcrypt.lib")

namespace sl::security
{
namespace
{

constexpr std::array<uint8_t, 8> kManifestMagic{ 'S', 'L', 'P', 'M', 'A', 'N', '0', '1' };
constexpr uint16_t kManifestVersion = 1;
constexpr uint16_t kManifestHeaderSize = 32;
constexpr size_t kMaximumManifestSize = 64 * 1024;
constexpr uint32_t kMaximumEntryCount = 32;
constexpr uint32_t kMaximumReleaseIdLength = 64;
constexpr uint16_t kMaximumBasenameLength = 64;
constexpr size_t kSignatureSize = 64;

struct HandleCloser
{
    void operator()(void* handle) const
    {
        if (handle && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct AlgorithmCloser
{
    void operator()(void* handle) const
    {
        if (handle)
        {
            BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
        }
    }
};
using UniqueAlgorithm = std::unique_ptr<void, AlgorithmCloser>;

struct HashCloser
{
    void operator()(void* handle) const
    {
        if (handle)
        {
            BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
        }
    }
};
using UniqueHash = std::unique_ptr<void, HashCloser>;

struct KeyCloser
{
    void operator()(void* handle) const
    {
        if (handle)
        {
            BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle));
        }
    }
};
using UniqueKey = std::unique_ptr<void, KeyCloser>;

struct ManifestEntry
{
    std::string basename;
    ProjectFileRole role{};
    uint64_t size{};
    std::array<uint8_t, 32> digest{};
};

struct ParsedManifest
{
    uint32_t keyId{};
    std::string releaseId;
    std::vector<ManifestEntry> entries;
};

struct OpenedFile
{
    UniqueHandle handle;
    UniqueHandle boundLoadHandle;
    std::wstring identityPath;
    std::wstring loadPath;
};

ProjectLoadResult fail(TrustFailure failure, DWORD systemError = 0)
{
    return { failure, systemError, nullptr };
}

bool ntSuccess(NTSTATUS status)
{
    return status >= 0;
}

uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p)
{
    uint64_t value{};
    for (size_t i = 0; i != 8; ++i)
    {
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return value;
}

bool isReleaseIdValid(std::string_view value)
{
    if (value.empty() || value.size() > kMaximumReleaseIdLength)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '.' || c == '_' || c == '-';
        });
}

bool isCanonicalBasename(std::string_view value)
{
    if (value.empty() || value.size() > kMaximumBasenameLength ||
        value.size() < 5 || !value.ends_with(".dll"))
    {
        return false;
    }
    if (value == "." || value == ".." || value.find("..") != std::string_view::npos)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '_' || c == '.';
        });
}

bool isKnownNvidiaModule(std::string_view basename)
{
    constexpr std::array<std::string_view, 15> names
    {
        "nvlowlatencyvk.dll",
        "nvngx_deepdvc.dll",
        "nvngx_dlss.dll",
        "nvngx_dlssd.dll",
        "nvngx_dlssg.dll",
        "sl.deepdvc.dll",
        "sl.directsr.dll",
        "sl.dlss.dll",
        "sl.dlss_d.dll",
        "sl.dlss_g.dll",
        "sl.imgui.dll",
        "sl.nis.dll",
        "sl.nvperf.dll",
        "sl.pcl.dll",
        "sl.reflex.dll",
    };
    return std::find(names.begin(), names.end(), basename) != names.end();
}

bool roleMatchesBasename(ProjectFileRole role, std::string_view basename)
{
    switch (role)
    {
    case ProjectFileRole::eProjectInterposer:
        return basename == "sl.interposer.dll";
    case ProjectFileRole::eProjectCommon:
        return basename == "sl.common.dll";
    case ProjectFileRole::eNvidiaModule:
        return isKnownNvidiaModule(basename);
    default:
        return false;
    }
}

TrustFailure parseManifest(const std::vector<uint8_t>& bytes, ParsedManifest& parsed)
{
    if (bytes.size() > kMaximumManifestSize)
    {
        return TrustFailure::eManifestTooLarge;
    }
    if (bytes.size() < kManifestHeaderSize ||
        !std::equal(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin()))
    {
        return TrustFailure::eManifestMalformed;
    }
    const auto* data = bytes.data();
    if (readU16(data + 8) != kManifestVersion)
    {
        return TrustFailure::eManifestVersionUnsupported;
    }
    if (readU16(data + 10) != kManifestHeaderSize ||
        readU32(data + 12) != bytes.size() ||
        readU32(data + 28) != 0)
    {
        return TrustFailure::eManifestMalformed;
    }

    const uint32_t entryCount = readU32(data + 16);
    const uint32_t releaseLength = readU32(data + 20);
    parsed.keyId = readU32(data + 24);
    if (entryCount == 0 || entryCount > kMaximumEntryCount ||
        releaseLength == 0 || releaseLength > kMaximumReleaseIdLength)
    {
        return TrustFailure::eManifestMalformed;
    }

    size_t offset = kManifestHeaderSize;
    if (releaseLength > bytes.size() - offset)
    {
        return TrustFailure::eManifestMalformed;
    }
    parsed.releaseId.assign(
        reinterpret_cast<const char*>(data + offset),
        reinterpret_cast<const char*>(data + offset + releaseLength));
    if (!isReleaseIdValid(parsed.releaseId))
    {
        return TrustFailure::eManifestMalformed;
    }
    offset += releaseLength;

    parsed.entries.clear();
    parsed.entries.reserve(entryCount);
    std::string previousName;
    bool haveInterposer = false;
    bool haveCommon = false;
    for (uint32_t i = 0; i != entryCount; ++i)
    {
        constexpr size_t fixedEntrySize = 44;
        if (offset > bytes.size() || fixedEntrySize > bytes.size() - offset)
        {
            return TrustFailure::eManifestMalformed;
        }
        const uint16_t nameLength = readU16(data + offset);
        const auto role = static_cast<ProjectFileRole>(data[offset + 2]);
        if (nameLength == 0 || nameLength > kMaximumBasenameLength ||
            data[offset + 3] != 0)
        {
            return TrustFailure::eManifestMalformed;
        }

        ManifestEntry entry;
        entry.role = role;
        entry.size = readU64(data + offset + 4);
        std::copy_n(data + offset + 12, entry.digest.size(), entry.digest.begin());
        offset += fixedEntrySize;
        if (nameLength > bytes.size() - offset)
        {
            return TrustFailure::eManifestMalformed;
        }
        entry.basename.assign(
            reinterpret_cast<const char*>(data + offset),
            reinterpret_cast<const char*>(data + offset + nameLength));
        offset += nameLength;

        if (!isCanonicalBasename(entry.basename) ||
            !roleMatchesBasename(role, entry.basename) ||
            (!previousName.empty() && entry.basename <= previousName))
        {
            return TrustFailure::eManifestMalformed;
        }
        previousName = entry.basename;
        haveInterposer |= role == ProjectFileRole::eProjectInterposer;
        haveCommon |= role == ProjectFileRole::eProjectCommon;
        parsed.entries.push_back(std::move(entry));
    }
    if (offset != bytes.size() || !haveInterposer || !haveCommon)
    {
        return TrustFailure::eManifestMalformed;
    }
    return TrustFailure::eOk;
}

TrustFailure openRestricted(std::wstring_view path, OpenedFile& opened, DWORD& systemError)
{
    std::wstring nullTerminatedPath(path);
    HANDLE raw = CreateFileW(
        nullTerminatedPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (raw == INVALID_HANDLE_VALUE)
    {
        systemError = GetLastError();
        return TrustFailure::eFileOpenFailed;
    }
    opened.handle.reset(raw);

    HANDLE boundLoadHandle = INVALID_HANDLE_VALUE;
    if (!getPhysicalFilePaths(
        raw, opened.identityPath, opened.loadPath,
        boundLoadHandle, systemError))
    {
        return TrustFailure::eFileOpenFailed;
    }
    opened.boundLoadHandle.reset(boundLoadHandle);
    return TrustFailure::eOk;
}

TrustFailure readBoundedFile(
    OpenedFile& opened,
    size_t maximumSize,
    std::vector<uint8_t>& bytes,
    DWORD& systemError)
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(opened.handle.get(), &size))
    {
        systemError = GetLastError();
        return TrustFailure::eManifestIo;
    }
    if (size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > maximumSize)
    {
        return TrustFailure::eManifestTooLarge;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset != bytes.size())
    {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read{};
        if (!ReadFile(opened.handle.get(), bytes.data() + offset, chunk, &read, nullptr))
        {
            systemError = GetLastError();
            return TrustFailure::eManifestIo;
        }
        if (read == 0)
        {
            return TrustFailure::eManifestIo;
        }
        offset += read;
    }
    return TrustFailure::eOk;
}

TrustFailure hashBuffer(
    const uint8_t* data,
    size_t size,
    std::array<uint8_t, 32>& digest)
{
    BCRYPT_ALG_HANDLE rawAlgorithm{};
    if (!ntSuccess(BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
    {
        return TrustFailure::eManifestIo;
    }
    UniqueAlgorithm algorithm(rawAlgorithm);

    DWORD objectLength{};
    DWORD resultLength{};
    if (!ntSuccess(BCryptGetProperty(
        rawAlgorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &resultLength, 0)))
    {
        return TrustFailure::eManifestIo;
    }
    std::vector<uint8_t> object(objectLength);
    BCRYPT_HASH_HANDLE rawHash{};
    if (!ntSuccess(BCryptCreateHash(
        rawAlgorithm, &rawHash, object.data(), objectLength, nullptr, 0, 0)))
    {
        return TrustFailure::eManifestIo;
    }
    UniqueHash hash(rawHash);
    size_t offset = 0;
    while (offset != size)
    {
        const ULONG chunk = static_cast<ULONG>((std::min<size_t>)(
            size - offset, std::numeric_limits<ULONG>::max()));
        if (!ntSuccess(BCryptHashData(
            rawHash, const_cast<PUCHAR>(data + offset), chunk, 0)))
        {
            return TrustFailure::eManifestIo;
        }
        offset += chunk;
    }
    if (!ntSuccess(BCryptFinishHash(
        rawHash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
    {
        return TrustFailure::eManifestIo;
    }
    return TrustFailure::eOk;
}

TrustFailure hashFile(
    HANDLE file,
    std::array<uint8_t, 32>& digest,
    DWORD& systemError)
{
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
    {
        systemError = GetLastError();
        return TrustFailure::eFileOpenFailed;
    }

    BCRYPT_ALG_HANDLE rawAlgorithm{};
    if (!ntSuccess(BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
    {
        return TrustFailure::eFileHashMismatch;
    }
    UniqueAlgorithm algorithm(rawAlgorithm);
    DWORD objectLength{};
    DWORD resultLength{};
    if (!ntSuccess(BCryptGetProperty(
        rawAlgorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &resultLength, 0)))
    {
        return TrustFailure::eFileHashMismatch;
    }
    std::vector<uint8_t> object(objectLength);
    BCRYPT_HASH_HANDLE rawHash{};
    if (!ntSuccess(BCryptCreateHash(
        rawAlgorithm, &rawHash, object.data(), objectLength, nullptr, 0, 0)))
    {
        return TrustFailure::eFileHashMismatch;
    }
    UniqueHash hash(rawHash);
    std::array<uint8_t, 64 * 1024> buffer{};
    for (;;)
    {
        DWORD read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            systemError = GetLastError();
            return TrustFailure::eFileHashMismatch;
        }
        if (read == 0)
        {
            break;
        }
        if (!ntSuccess(BCryptHashData(rawHash, buffer.data(), read, 0)))
        {
            return TrustFailure::eFileHashMismatch;
        }
    }
    if (!ntSuccess(BCryptFinishHash(
        rawHash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
    {
        return TrustFailure::eFileHashMismatch;
    }
    SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
    return TrustFailure::eOk;
}

TrustFailure verifyManifestSignature(
    const std::vector<uint8_t>& manifest,
    const std::vector<uint8_t>& signature,
    const ProjectTrustConfig& trust)
{
    if (signature.size() != kSignatureSize)
    {
        return TrustFailure::eSignatureInvalid;
    }
    std::array<uint8_t, 32> digest{};
    const TrustFailure hashFailure = hashBuffer(
        manifest.data(), manifest.size(), digest);
    if (hashFailure != TrustFailure::eOk)
    {
        return hashFailure;
    }

    struct PublicBlob
    {
        BCRYPT_ECCKEY_BLOB header;
        std::array<uint8_t, 64> xy;
    } blob
    {
        { BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32 },
        trust.publicKeyXY
    };

    BCRYPT_ALG_HANDLE rawAlgorithm{};
    if (!ntSuccess(BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
    {
        return TrustFailure::eSignatureInvalid;
    }
    UniqueAlgorithm algorithm(rawAlgorithm);
    BCRYPT_KEY_HANDLE rawKey{};
    if (!ntSuccess(BCryptImportKeyPair(
        rawAlgorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &rawKey,
        reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0)))
    {
        return TrustFailure::eSignatureInvalid;
    }
    UniqueKey key(rawKey);
    const NTSTATUS status = BCryptVerifySignature(
        rawKey, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(signature.data()),
        static_cast<ULONG>(signature.size()), 0);
    return ntSuccess(status) ? TrustFailure::eOk : TrustFailure::eSignatureInvalid;
}

std::wstring widenAscii(std::string_view value)
{
    return std::wstring(value.begin(), value.end());
}

}

const ProjectTrustConfig& getCompiledProjectTrust()
{
#if SL_PROJECT_TRUST_CONFIGURED
    static constexpr ProjectTrustConfig trust
    {
        true,
        SL_PROJECT_TRUST_KEY_ID,
        SL_PROJECT_TRUST_PUBLIC_KEY_XY,
        SL_PROJECT_TRUST_RELEASE_ID
    };
#else
    static constexpr ProjectTrustConfig trust{};
#endif
    return trust;
}

bool verifyNvidiaEmbeddedSignature(const wchar_t* fullPath)
{
    return fullPath && verifyEmbeddedSignature(fullPath);
}

ProjectLoadResult authenticateAndLoadNvidiaLibrary(
    std::wstring_view physicalPath,
    bool authenticateOnly)
{
    if (physicalPath.empty())
    {
        return fail(TrustFailure::eInvalidArgument);
    }
    DWORD systemError{};
    OpenedFile opened;
    TrustFailure failure = openRestricted(physicalPath, opened, systemError);
    if (failure != TrustFailure::eOk)
    {
        return fail(failure, systemError);
    }
    if (!verifyNvidiaEmbeddedSignature(opened.identityPath.c_str()))
    {
        return fail(TrustFailure::eNvidiaSignatureInvalid);
    }
    if (authenticateOnly)
    {
        return { TrustFailure::eOk, 0, nullptr };
    }
    HMODULE module = LoadLibraryExW(
        opened.loadPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    {
        return fail(TrustFailure::eLoadFailed, GetLastError());
    }
    return { TrustFailure::eOk, 0, module };
}

ProjectLoadResult authenticateAndLoadProjectLibrary(const ProjectLoadOptions& options)
{
    if (options.requestedBasename.empty() ||
        options.manifestPath.empty() ||
        options.signaturePath.empty() ||
        !options.resolve ||
        !options.trust)
    {
        return fail(TrustFailure::eInvalidArgument);
    }
    if (!options.trust->configured)
    {
        return fail(TrustFailure::eProjectTrustNotConfigured);
    }

    DWORD systemError{};
    OpenedFile manifestFile;
    TrustFailure failure = openRestricted(
        options.manifestPath, manifestFile, systemError);
    if (failure != TrustFailure::eOk)
    {
        return fail(TrustFailure::eManifestIncomplete, systemError);
    }
    OpenedFile signatureFile;
    failure = openRestricted(
        options.signaturePath, signatureFile, systemError);
    if (failure != TrustFailure::eOk)
    {
        return fail(TrustFailure::eManifestIncomplete, systemError);
    }

    std::vector<uint8_t> manifestBytes;
    failure = readBoundedFile(
        manifestFile, kMaximumManifestSize, manifestBytes, systemError);
    if (failure != TrustFailure::eOk)
    {
        return fail(failure, systemError);
    }
    std::vector<uint8_t> signatureBytes;
    failure = readBoundedFile(
        signatureFile, kSignatureSize, signatureBytes, systemError);
    if (failure != TrustFailure::eOk)
    {
        return fail(
            failure == TrustFailure::eManifestTooLarge ?
                TrustFailure::eSignatureInvalid : failure,
            systemError);
    }

    ParsedManifest parsed;
    failure = parseManifest(manifestBytes, parsed);
    if (failure != TrustFailure::eOk)
    {
        return fail(failure);
    }
    if (parsed.keyId != options.trust->keyId)
    {
        return fail(TrustFailure::eManifestUnknownKey);
    }
    if (parsed.releaseId != options.trust->expectedReleaseId)
    {
        return fail(TrustFailure::eManifestReleaseMismatch);
    }
    failure = verifyManifestSignature(
        manifestBytes, signatureBytes, *options.trust);
    if (failure != TrustFailure::eOk)
    {
        return fail(failure);
    }

    std::string requested;
    requested.reserve(options.requestedBasename.size());
    for (wchar_t c : options.requestedBasename)
    {
        if (c > 0x7f)
        {
            return fail(TrustFailure::eRequestedFileNotApproved);
        }
        requested.push_back(static_cast<char>(c));
    }
    if (!isCanonicalBasename(requested))
    {
        return fail(TrustFailure::eRequestedFileNotApproved);
    }

    const ManifestEntry* requestedEntry{};
    std::vector<OpenedFile> openedFiles;
    openedFiles.reserve(parsed.entries.size());
    for (const ManifestEntry& entry : parsed.entries)
    {
        std::wstring resolvedPath;
        const std::wstring logicalName = widenAscii(entry.basename);
        if (!options.resolve(
            options.resolveContext, logicalName, resolvedPath) ||
            resolvedPath.empty())
        {
            return fail(TrustFailure::eResolutionFailed);
        }

        OpenedFile opened;
        failure = openRestricted(resolvedPath, opened, systemError);
        if (failure != TrustFailure::eOk)
        {
            return fail(failure, systemError);
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(opened.handle.get(), &fileSize))
        {
            return fail(TrustFailure::eFileOpenFailed, GetLastError());
        }
        if (fileSize.QuadPart < 0 ||
            static_cast<uint64_t>(fileSize.QuadPart) != entry.size)
        {
            return fail(TrustFailure::eFileSizeMismatch);
        }
        std::array<uint8_t, 32> digest{};
        failure = hashFile(opened.handle.get(), digest, systemError);
        if (failure != TrustFailure::eOk)
        {
            return fail(failure, systemError);
        }
        if (digest != entry.digest)
        {
            return fail(TrustFailure::eFileHashMismatch);
        }
        if (entry.role == ProjectFileRole::eNvidiaModule &&
            !verifyNvidiaEmbeddedSignature(opened.identityPath.c_str()))
        {
            return fail(TrustFailure::eNvidiaSignatureInvalid);
        }
        if (entry.basename == requested)
        {
            requestedEntry = &entry;
        }
        openedFiles.push_back(std::move(opened));
    }
    if (!requestedEntry)
    {
        return fail(TrustFailure::eRequestedFileNotApproved);
    }
    if (options.authenticateOnly)
    {
        return { TrustFailure::eOk, 0, nullptr };
    }

    const auto index = static_cast<size_t>(
        requestedEntry - parsed.entries.data());
    const std::wstring& requestedPath = openedFiles[index].loadPath;
    if (options.beforeLoad)
    {
        options.beforeLoad(options.beforeLoadContext, requestedPath);
    }
    HMODULE module = LoadLibraryExW(
        requestedPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    {
        return fail(TrustFailure::eLoadFailed, GetLastError());
    }
    return { TrustFailure::eOk, 0, module };
}

const char* getTrustFailureMessage(TrustFailure failure)
{
    switch (failure)
    {
    case TrustFailure::eOk: return "success";
    case TrustFailure::eInvalidArgument: return "invalid trust request";
    case TrustFailure::eManifestIncomplete: return "manifest or detached signature is missing";
    case TrustFailure::eManifestIo: return "manifest I/O or hashing failed";
    case TrustFailure::eManifestTooLarge: return "manifest exceeds its size bound";
    case TrustFailure::eManifestMalformed: return "manifest is malformed or incomplete";
    case TrustFailure::eManifestVersionUnsupported: return "manifest version is unsupported";
    case TrustFailure::eManifestUnknownKey: return "manifest key ID is not trusted";
    case TrustFailure::eManifestReleaseMismatch: return "manifest release ID is not accepted";
    case TrustFailure::eProjectTrustNotConfigured: return "project signing trust is not configured";
    case TrustFailure::eSignatureInvalid: return "detached project signature is invalid";
    case TrustFailure::eRequestedFileNotApproved: return "requested DLL is not approved by the manifest";
    case TrustFailure::eResolutionFailed: return "a manifest file could not be physically resolved";
    case TrustFailure::eFileOpenFailed: return "a manifest file could not be opened safely";
    case TrustFailure::eFileSizeMismatch: return "a manifest file size does not match";
    case TrustFailure::eFileHashMismatch: return "a manifest file hash does not match";
    case TrustFailure::eNvidiaSignatureInvalid: return "NVIDIA embedded signature verification failed";
    case TrustFailure::eLoadFailed: return "authenticated DLL load failed";
    default: return "unknown trust failure";
    }
}

}
