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

#include <d3d12.h>

#include "include/sl_fsr.h"

namespace sl::fsr
{

struct D3D12RuntimeCapabilities
{
    bool windows11OrGreater{};
    uint32_t shaderModelMajor{};
    uint32_t shaderModelMinor{};
    FSRD3D12RuntimeSource runtimeSource =
        FSRD3D12RuntimeSource::eUnknown;
    uint32_t coreVersionMajor{};
    uint32_t coreVersionMinor{};
    uint32_t coreVersionPatch{};
    uint32_t coreVersionRevision{};
    uint32_t requestedSDKVersion{};
};

//! Describes the D3D12 runtime which actually created device. Merely staging an
//! Agility SDK does not affect this result.
D3D12RuntimeCapabilities getD3D12RuntimeCapabilities(
    ID3D12Device* device);

template<typename T>
void setD3D12RuntimeCapabilities(
    ID3D12Device* device,
    T& capabilities)
{
    if (capabilities.structVersion < kStructVersion2) return;
    const auto runtime = getD3D12RuntimeCapabilities(device);
    capabilities.windows11OrGreater = runtime.windows11OrGreater ?
        Boolean::eTrue :
        Boolean::eFalse;
    capabilities.shaderModelMajor = runtime.shaderModelMajor;
    capabilities.shaderModelMinor = runtime.shaderModelMinor;
    capabilities.d3d12RuntimeSource = runtime.runtimeSource;
    capabilities.d3d12CoreVersionMajor = runtime.coreVersionMajor;
    capabilities.d3d12CoreVersionMinor = runtime.coreVersionMinor;
    capabilities.d3d12CoreVersionPatch = runtime.coreVersionPatch;
    capabilities.d3d12CoreVersionRevision = runtime.coreVersionRevision;
    capabilities.requestedD3D12SDKVersion =
        runtime.requestedSDKVersion;
}

//! Converts the result of an actual ML provider query into a stable public
//! reason. Positive support requires both the documented OS/runtime features
//! and exact provider enumeration on the supplied device.
FSRUnavailableReason getMachineLearningUnavailableReason(
    ID3D12Device* device,
    bool moduleAvailable,
    bool providerAvailable,
    bool requiresWindows11);

}
