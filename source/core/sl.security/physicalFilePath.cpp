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

#include "source/core/sl.security/physicalFilePath.h"

#include <cstring>
#include <memory>
#include <psapi.h>
#include <string_view>

#pragma comment(lib, "psapi.lib")

namespace sl::security
{
namespace
{

struct HandleCloser
{
    void operator()(void* handle) const
    {
        if (handle)
        {
            CloseHandle(handle);
        }
    }
};

struct ViewCloser
{
    void operator()(void* view) const
    {
        if (view)
        {
            UnmapViewOfFile(view);
        }
    }
};

struct VolumeSearchCloser
{
    void operator()(void* search) const
    {
        if (search && search != INVALID_HANDLE_VALUE)
        {
            FindVolumeClose(search);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;
using UniqueView = std::unique_ptr<void, ViewCloser>;
using UniqueVolumeSearch = std::unique_ptr<void, VolumeSearchCloser>;

bool startsWithPathPrefix(std::wstring_view path, std::wstring_view prefix)
{
    return path.size() > prefix.size() &&
        path[prefix.size()] == L'\\' &&
        _wcsnicmp(path.data(), prefix.data(), prefix.size()) == 0;
}

bool isWine()
{
    static const bool detected = []()
        {
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll &&
                GetProcAddress(ntdll, "wine_get_version") != nullptr;
        }();
    return detected;
}

bool getWineMappedLoadPath(
    std::wstring_view native,
    std::wstring& loadPath)
{
    constexpr std::wstring_view prefix = L"\\??\\";
    if (!native.starts_with(prefix) ||
        native.size() <= prefix.size() + 3)
    {
        return false;
    }

    const size_t drive = prefix.size();
    const wchar_t letter = native[drive];
    if (!((letter >= L'A' && letter <= L'Z') ||
          (letter >= L'a' && letter <= L'z')) ||
        native[drive + 1] != L':' ||
        native[drive + 2] != L'\\')
    {
        return false;
    }

    size_t component = drive + 3;
    for (size_t i = component; i <= native.size(); ++i)
    {
        if (i != native.size() && native[i] != L'\\')
        {
            const wchar_t c = native[i];
            if (c < L' ' || c == L'/' || c == L':' ||
                c == L'"' || c == L'<' || c == L'>' ||
                c == L'|' || c == L'*' || c == L'?')
            {
                return false;
            }
            continue;
        }

        const std::wstring_view part =
            native.substr(component, i - component);
        if (part.empty() || part == L"." || part == L"..")
        {
            return false;
        }
        component = i + 1;
    }

    loadPath.assign(native);
    return true;
}

bool fileIdsMatch(
    const FILE_ID_INFO& expected,
    const FILE_ID_INFO& candidate,
    bool requireVolumeIdentity)
{
    if (requireVolumeIdentity &&
        (!expected.VolumeSerialNumber ||
         !candidate.VolumeSerialNumber))
    {
        return false;
    }
    return expected.VolumeSerialNumber == candidate.VolumeSerialNumber &&
        std::memcmp(
            expected.FileId.Identifier,
            candidate.FileId.Identifier,
            sizeof(expected.FileId.Identifier)) == 0;
}

bool bindSameIdentity(
    HANDLE expected,
    const std::wstring& path,
    HANDLE& bound,
    DWORD& systemError,
    bool requireVolumeIdentity = false)
{
    UniqueHandle candidate(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!candidate)
    {
        systemError = GetLastError();
        return false;
    }

    FILE_ID_INFO expectedId{};
    FILE_ID_INFO candidateId{};
    if (!GetFileInformationByHandleEx(
        expected, FileIdInfo, &expectedId, sizeof(expectedId)) ||
        !GetFileInformationByHandleEx(
            candidate.get(), FileIdInfo, &candidateId, sizeof(candidateId)))
    {
        systemError = GetLastError();
        return false;
    }
    if (!fileIdsMatch(
        expectedId, candidateId, requireVolumeIdentity))
    {
        systemError =
            requireVolumeIdentity &&
            (!expectedId.VolumeSerialNumber ||
             !candidateId.VolumeSerialNumber) ?
            ERROR_NOT_SUPPORTED : ERROR_FILE_INVALID;
        return false;
    }
    bound = static_cast<HANDLE>(candidate.release());
    return true;
}

bool convertLocalDevicePath(
    std::wstring_view native,
    HANDLE file,
    std::wstring& identityPath,
    std::wstring& loadPath,
    HANDLE& boundLoadHandle,
    DWORD& systemError)
{
    std::wstring volume(32768, L'\0');
    HANDLE rawSearch = FindFirstVolumeW(
        volume.data(), static_cast<DWORD>(volume.size()));
    if (rawSearch == INVALID_HANDLE_VALUE)
    {
        systemError = GetLastError();
        return false;
    }
    UniqueVolumeSearch search(rawSearch);

    std::wstring device(32768, L'\0');
    for (;;)
    {
        const size_t volumeLength = std::wcslen(volume.c_str());
        if (volumeLength > 5 &&
            volume.starts_with(L"\\\\?\\") &&
            volume[volumeLength - 1] == L'\\')
        {
            const std::wstring queryName =
                volume.substr(4, volumeLength - 5);
            const DWORD deviceLength = QueryDosDeviceW(
                queryName.c_str(), device.data(),
                static_cast<DWORD>(device.size()));
            if (deviceLength)
            {
                for (const wchar_t* candidate = device.data(); *candidate;
                    candidate += std::wcslen(candidate) + 1)
                {
                    const std::wstring_view prefix(candidate);
                    if (startsWithPathPrefix(native, prefix))
                    {
                        volume.resize(volumeLength);
                        const std::wstring relative(
                            native.substr(prefix.size() + 1));
                        identityPath = volume + relative;

                        DWORD pathLength{};
                        GetVolumePathNamesForVolumeNameW(
                            volume.c_str(), nullptr, 0, &pathLength);
                        if (!pathLength &&
                            GetLastError() != ERROR_MORE_DATA)
                        {
                            systemError = GetLastError();
                            return false;
                        }
                        std::wstring paths(pathLength, L'\0');
                        if (!GetVolumePathNamesForVolumeNameW(
                            volume.c_str(), paths.data(), pathLength,
                            &pathLength))
                        {
                            systemError = GetLastError();
                            return false;
                        }
                        for (const wchar_t* mount = paths.data(); *mount;
                            mount += std::wcslen(mount) + 1)
                        {
                            const std::wstring mountPathCandidate =
                                std::wstring(mount) + relative;
                            DWORD candidateError{};
                            HANDLE candidateHandle = INVALID_HANDLE_VALUE;
                            if (bindSameIdentity(
                                file, mountPathCandidate, candidateHandle,
                                candidateError))
                            {
                                loadPath = mountPathCandidate;
                                boundLoadHandle = candidateHandle;
                                return true;
                            }
                        }
                        systemError = ERROR_FILE_INVALID;
                        return false;
                    }
                }
            }
        }

        volume.assign(32768, L'\0');
        if (!FindNextVolumeW(
            rawSearch, volume.data(), static_cast<DWORD>(volume.size())))
        {
            const DWORD error = GetLastError();
            systemError = error == ERROR_NO_MORE_FILES ?
                ERROR_NOT_SUPPORTED : error;
            return false;
        }
    }
}

}

bool getPhysicalFilePaths(
    HANDLE file,
    std::wstring& identityPath,
    std::wstring& loadPath,
    HANDLE& boundLoadHandle,
    DWORD& systemError)
{
    identityPath.clear();
    loadPath.clear();
    boundLoadHandle = INVALID_HANDLE_VALUE;
    systemError = ERROR_SUCCESS;
    if (!file || file == INVALID_HANDLE_VALUE)
    {
        systemError = ERROR_INVALID_HANDLE;
        return false;
    }

    UniqueHandle mapping(CreateFileMappingW(
        file, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping)
    {
        systemError = GetLastError();
        return false;
    }
    UniqueView view(MapViewOfFile(
        mapping.get(), FILE_MAP_READ, 0, 0, 1));
    if (!view)
    {
        systemError = GetLastError();
        return false;
    }

    std::wstring native(32768, L'\0');
    const DWORD length = GetMappedFileNameW(
        GetCurrentProcess(), view.get(), native.data(),
        static_cast<DWORD>(native.size()));
    if (!length)
    {
        systemError = GetLastError();
        return false;
    }
    if (length >= native.size())
    {
        systemError = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    native.resize(length);

    if (isWine())
    {
        if (!getWineMappedLoadPath(native, loadPath))
        {
            systemError = ERROR_NOT_SUPPORTED;
            return false;
        }
        identityPath = loadPath;
        return bindSameIdentity(
            file, loadPath, boundLoadHandle,
            systemError, true);
    }

    constexpr std::wstring_view networkPrefix = L"\\Device\\Mup";
    if (startsWithPathPrefix(native, networkPrefix))
    {
        identityPath = L"\\\\";
        identityPath.append(native.substr(networkPrefix.size() + 1));
        loadPath = identityPath;
        return bindSameIdentity(
            file, loadPath, boundLoadHandle, systemError);
    }
    return convertLocalDevicePath(
        native, file, identityPath, loadPath,
        boundLoadHandle, systemError);
}

#if defined(SL_PHYSICAL_FILE_PATH_TESTS)
namespace detail
{

bool getWineMappedLoadPathForTests(
    std::wstring_view native,
    std::wstring& loadPath)
{
    return getWineMappedLoadPath(native, loadPath);
}

bool fileIdsMatchForTests(
    const FILE_ID_INFO& expected,
    const FILE_ID_INFO& candidate,
    bool requireVolumeIdentity)
{
    return fileIdsMatch(
        expected, candidate, requireVolumeIdentity);
}

}
#endif

}
