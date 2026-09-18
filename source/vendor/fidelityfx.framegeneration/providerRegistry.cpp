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

#include "_ffx/Kits/FidelityFX/api/internal/ffx_provider.h"
#include "_ffx/Kits/FidelityFX/framegeneration/fsr3/include/ffx_provider_fsr3framegeneration.h"
#include "_ffx/Kits/FidelityFX/framegeneration/fsr3/include/ffx_provider_fsr3framegenerationswapchain.h"

#include <array>

namespace
{

std::array<ffxProvider*, 2> getProviders()
{
    return {
        &ffxProvider_Fsr3FrameGeneration::GetInstance(),
        &ffxProvider_Fsr3FrameGenerationSwapChain::GetInstance(),
    };
}

}

ffxProvider* GetProvider(
    ffxStructType_t descType,
    uint64_t overrideId,
    void* device,
    std::optional<ffxProviderExternal>& externalProvider)
{
    const auto providers = getProviders();
    return GetProvider(descType, overrideId, device, externalProvider, providers);
}

uint64_t GetProviderVersions(
    ffxStructType_t descType,
    void* device,
    uint64_t capacity,
    uint64_t* versionIds,
    const char** versionNames,
    std::optional<ffxProviderExternal>& externalProvider)
{
    const auto providers = getProviders();
    return GetProviderVersions(
        descType,
        device,
        capacity,
        versionIds,
        versionNames,
        externalProvider,
        providers);
}
