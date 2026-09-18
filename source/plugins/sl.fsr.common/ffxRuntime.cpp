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

#include "ffxRuntime.h"

#include "source/core/sl.log/log.h"
#include "source/core/sl.security/secureLoadLibrary.h"

namespace sl::fsr
{

namespace
{

uint32_t getFfxState(D3D12_RESOURCE_STATES state)
{
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    if (state & D3D12_RESOURCE_STATE_RENDER_TARGET) return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    if (state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) return FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
    if (state & D3D12_RESOURCE_STATE_COPY_DEST) return FFX_API_RESOURCE_STATE_COPY_DEST;
    if (state & D3D12_RESOURCE_STATE_COPY_SOURCE) return FFX_API_RESOURCE_STATE_COPY_SRC;
    if (state & D3D12_RESOURCE_STATE_PRESENT) return FFX_API_RESOURCE_STATE_PRESENT;
    if (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        return state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            ? FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ
            : FFX_API_RESOURCE_STATE_PIXEL_READ;
    }
    if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (state & D3D12_RESOURCE_STATE_GENERIC_READ) return FFX_API_RESOURCE_STATE_GENERIC_READ;
    return FFX_API_RESOURCE_STATE_COMMON;
}

template<typename T>
bool resolve(HMODULE module, const char* name, T& function)
{
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!function)
    {
        SL_LOG_ERROR("FidelityFX loader is missing export '%s'", name);
        return false;
    }
    return true;
}

}

Runtime::~Runtime()
{
    shutdown();
}

bool Runtime::initialize(const std::filesystem::path& directory, bool upscaler, bool frameGeneration)
{
    shutdown();

    std::vector<const wchar_t*> providerNames;
    if (upscaler) providerNames.push_back(L"amd_fidelityfx_upscaler_dx12.dll");
    if (frameGeneration) providerNames.push_back(L"amd_fidelityfx_framegeneration_dx12.dll");
    for (const auto* name : providerNames)
    {
        const auto path = directory / name;
        HMODULE provider = security::loadLibrary(path.c_str());
        if (!provider)
        {
            SL_LOG_ERROR("Failed to authenticate and load FidelityFX provider '%S'", path.c_str());
            shutdown();
            return false;
        }
        m_providers.push_back(provider);
    }

    const auto loaderPath = directory / L"amd_fidelityfx_loader_dx12.dll";
    m_loader = security::loadLibrary(loaderPath.c_str());
    if (!m_loader)
    {
        SL_LOG_ERROR("Failed to authenticate and load FidelityFX loader '%S'", loaderPath.c_str());
        shutdown();
        return false;
    }

    if (!resolve(m_loader, "ffxCreateContext", m_create) ||
        !resolve(m_loader, "ffxDestroyContext", m_destroy) ||
        !resolve(m_loader, "ffxConfigure", m_configure) ||
        !resolve(m_loader, "ffxQuery", m_query) ||
        !resolve(m_loader, "ffxDispatch", m_dispatch))
    {
        shutdown();
        return false;
    }
    return true;
}

void Runtime::shutdown()
{
    m_create = {};
    m_destroy = {};
    m_configure = {};
    m_query = {};
    m_dispatch = {};
    if (m_loader)
    {
        FreeLibrary(m_loader);
        m_loader = {};
    }
    for (auto it = m_providers.rbegin(); it != m_providers.rend(); ++it)
    {
        FreeLibrary(*it);
    }
    m_providers.clear();
}

bool Runtime::selectProvider(
    ffxStructType_t createDescType,
    ID3D12Device* device,
    const char* expectedVersion,
    ProviderVersion& provider) const
{
    if (!m_query || !device || !expectedVersion) return false;

    uint64_t count{};
    ffxQueryDescGetVersions countQuery{{ FFX_API_QUERY_DESC_TYPE_GET_VERSIONS }, createDescType, device, &count};
    if (m_query(nullptr, &countQuery.header) != FFX_API_RETURN_OK || count == 0)
    {
        SL_LOG_ERROR("No FidelityFX providers were returned for descriptor 0x%llx", createDescType);
        return false;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    ffxQueryDescGetVersions versionsQuery{{ FFX_API_QUERY_DESC_TYPE_GET_VERSIONS }, createDescType, device, &count, ids.data(), names.data()};
    if (m_query(nullptr, &versionsQuery.header) != FFX_API_RETURN_OK)
    {
        SL_LOG_ERROR("FidelityFX provider enumeration failed for descriptor 0x%llx", createDescType);
        return false;
    }

    if (chooseProviderVersion(ids, names, expectedVersion, provider))
    {
        return true;
    }

    SL_LOG_ERROR("Required FidelityFX provider version '%s' is unavailable", expectedVersion);
    return false;
}

bool Runtime::validateProvider(ffxContext context, const ProviderVersion& provider) const
{
    if (!m_query || !context) return false;
    ffxQueryGetProviderVersion query{{ FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION }};
    if (m_query(&context, &query.header) != FFX_API_RETURN_OK ||
        !query.versionName ||
        query.versionId != provider.id ||
        query.versionName != provider.name)
    {
        SL_LOG_ERROR("FidelityFX created a provider other than requested version '%s'", provider.name.c_str());
        return false;
    }
    return true;
}

ffxReturnCode_t Runtime::create(ffxContext* context, ffxCreateContextDescHeader* desc) const
{
    return m_create ? m_create(context, desc, nullptr) : FFX_API_RETURN_ERROR;
}

ffxReturnCode_t Runtime::destroy(ffxContext* context) const
{
    return m_destroy ? m_destroy(context, nullptr) : FFX_API_RETURN_ERROR;
}

ffxReturnCode_t Runtime::configure(ffxContext* context, const ffxConfigureDescHeader* desc) const
{
    return m_configure ? m_configure(context, desc) : FFX_API_RETURN_ERROR;
}

ffxReturnCode_t Runtime::query(ffxContext* context, ffxQueryDescHeader* desc) const
{
    return m_query ? m_query(context, desc) : FFX_API_RETURN_ERROR;
}

ffxReturnCode_t Runtime::dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const
{
    return m_dispatch ? m_dispatch(context, desc) : FFX_API_RETURN_ERROR;
}

Runtime::operator bool() const
{
    return m_create && m_destroy && m_configure && m_query && m_dispatch;
}

FfxApiResource getResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    return ffxApiGetResourceDX12(resource, getFfxState(state));
}

}
