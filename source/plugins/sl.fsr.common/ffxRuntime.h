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

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <d3d12.h>

#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "providerVersion.h"

namespace sl::fsr
{

class Runtime
{
public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    bool initialize(const std::filesystem::path& directory, bool upscaler, bool frameGeneration);
    void shutdown();

    bool selectProvider(
        ffxStructType_t createDescType,
        ID3D12Device* device,
        const char* expectedVersion,
        ProviderVersion& provider) const;
    bool validateProvider(ffxContext context, const ProviderVersion& provider) const;

    ffxReturnCode_t create(ffxContext* context, ffxCreateContextDescHeader* desc) const;
    ffxReturnCode_t destroy(ffxContext* context) const;
    ffxReturnCode_t configure(ffxContext* context, const ffxConfigureDescHeader* desc) const;
    ffxReturnCode_t query(ffxContext* context, ffxQueryDescHeader* desc) const;
    ffxReturnCode_t dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const;

    explicit operator bool() const;

private:
    HMODULE m_loader{};
    std::vector<HMODULE> m_providers{};
    PfnFfxCreateContext m_create{};
    PfnFfxDestroyContext m_destroy{};
    PfnFfxConfigure m_configure{};
    PfnFfxQuery m_query{};
    PfnFfxDispatch m_dispatch{};
};

FfxApiResource getResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

}
