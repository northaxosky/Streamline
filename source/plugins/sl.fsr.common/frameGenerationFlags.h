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

#include "include/sl_fsr_g.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api_types.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"

namespace sl::fsr
{

inline uint32_t getFrameGenerationColorCreateFlags(FSRColorSpace colorSpace)
{
    // eGamma22 is decoded to SDR linear by the plugin adapter before dispatch.
    const bool hdr =
        colorSpace == FSRColorSpace::ePQ ||
        colorSpace == FSRColorSpace::eLinear;
    return hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
}

inline uint32_t getUiCompositionFlags(const FSRGOptions& options)
{
    uint32_t flags{};
    if (options.structVersion < kStructVersion2) return flags;
    if (options.uiPremultipliedAlpha == Boolean::eTrue)
    {
        flags |= FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA;
    }
    if (options.uiDoubleBuffering == Boolean::eTrue)
    {
        flags |=
            FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    }
    return flags;
}

// FFX normalizes scRGB as (v - min/80) / ((max - min)/80); {0, 0} yields inf.
inline void applyFrameGenerationLuminance(
    const FSRGOptions& options,
    bool convertedGamma22,
    float (&minMaxLuminance)[2])
{
    if (convertedGamma22)
    {
        // Decoded SDR linear has 1.0 as white; scRGB defines 1.0 as 80 nits.
        minMaxLuminance[0] = 0.0f;
        minMaxLuminance[1] = 80.0f;
    }
    else if (
        options.structVersion >= kStructVersion2 &&
        options.maxLuminance > options.minLuminance)
    {
        minMaxLuminance[0] = options.minLuminance;
        minMaxLuminance[1] = options.maxLuminance;
    }
}

}
