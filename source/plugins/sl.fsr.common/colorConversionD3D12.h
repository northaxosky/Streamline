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

#include "source/platforms/sl.chi/compute.h"

namespace sl::fsr
{

struct LinearColorTexture
{
    chi::Resource resource{};
    uint32_t width{};
    uint32_t height{};
};

class ColorConversionD3D12
{
public:
    bool initialize(chi::ICompute* compute);
    void shutdown();
    explicit operator bool() const { return m_compute && m_kernel; }

    bool ensureLinearTexture(
        LinearColorTexture& texture,
        uint32_t width,
        uint32_t height,
        const char* debugName);
    void release(
        LinearColorTexture& texture,
        uint32_t frameDelay = 3);

    bool decodeGamma22(
        chi::CommandList commandList,
        ID3D12Resource* source,
        D3D12_RESOURCE_STATES sourceState,
        LinearColorTexture& destination,
        uint32_t width,
        uint32_t height);
    bool encodeGamma22(
        chi::CommandList commandList,
        const LinearColorTexture& source,
        ID3D12Resource* destination,
        D3D12_RESOURCE_STATES destinationState,
        uint32_t width,
        uint32_t height);

private:
    enum class Direction : uint32_t
    {
        eGamma22ToLinear,
        eLinearToGamma22
    };

    bool dispatch(
        chi::CommandList commandList,
        chi::Resource source,
        chi::Resource destination,
        uint32_t width,
        uint32_t height,
        Direction direction);

    chi::ICompute* m_compute{};
    chi::Kernel m_kernel{};
};

}
