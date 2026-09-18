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

#include "include/sl_fsr.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api_types.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

namespace sl::fsr
{

inline uint32_t getUpscaleColorCreateFlags(FSRColorSpace colorSpace)
{
    uint32_t flags{};
    if (colorSpace == FSRColorSpace::ePQ)
    {
        flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    if (colorSpace != FSRColorSpace::eLinear)
    {
        flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    }
    return flags;
}

inline bool getUpscaleColorDispatchFlags(
    FSRColorSpace colorSpace,
    bool allowProjectGamma22,
    uint32_t& flags)
{
    switch (colorSpace)
    {
        case FSRColorSpace::eLinear:
            flags = 0;
            return true;
        case FSRColorSpace::eSRGB:
            flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
            return true;
        case FSRColorSpace::ePQ:
            flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ;
            return true;
        case FSRColorSpace::eGamma22:
            if (!allowProjectGamma22) return false;
            flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_GAMMA_2_2;
            return true;
        default:
            return false;
    }
}

inline bool getFrameGenerationTransferFunction(
    FSRColorSpace colorSpace,
    bool allowProjectGamma22,
    uint32_t& transferFunction)
{
    switch (colorSpace)
    {
        case FSRColorSpace::eLinear:
            transferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
            return true;
        case FSRColorSpace::eSRGB:
            transferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            return true;
        case FSRColorSpace::ePQ:
            transferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
            return true;
        case FSRColorSpace::eGamma22:
            if (!allowProjectGamma22) return false;
            transferFunction =
                FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2;
            return true;
        default:
            return false;
    }
}

}
