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

#include "sl.h"
#include "sl_consts.h"
#include "sl_core_types.h"

namespace sl
{

enum class FSRMode : uint32_t
{
    eOff,
    eNativeAA,
    eQuality,
    eBalanced,
    ePerformance,
    eUltraPerformance,
    eCount
};

enum class FSRColorSpace : uint32_t
{
    eLinear = 0,
    eSRGB = 1,
    ePQ = 2,
    //! Pure power-law gamma 2.2: decode pow(c, 2.2), encode pow(c, 1 / 2.2). Distinct from IEC sRGB.
    eGamma22 = 3,
    eCount = 4
};

// {DA762981-E158-4E5E-96CC-BF16BAAF369C}
SL_STRUCT_BEGIN(FSROptions, StructType({ 0xda762981, 0xe158, 0x4e5e, { 0x96, 0xcc, 0xbf, 0x16, 0xba, 0xaf, 0x36, 0x9c } }), kStructVersion1)
    FSRMode mode = FSRMode::eOff;
    uint32_t outputWidth = INVALID_UINT;
    uint32_t outputHeight = INVALID_UINT;
    uint32_t maxRenderWidth = INVALID_UINT;
    uint32_t maxRenderHeight = INVALID_UINT;
    float sharpness{};
    float preExposure = 1.0f;
    float frameTimeDeltaMilliseconds{};
    float viewSpaceToMetersFactor = 1.0f;
    FSRColorSpace colorSpace = FSRColorSpace::eSRGB;
    Boolean useAutoExposure = Boolean::eFalse;
    Boolean dynamicResolutionEnabled = Boolean::eFalse;
SL_STRUCT_END()

// {B8D650DE-C557-4293-AD50-7C0A17CACF56}
SL_STRUCT_BEGIN(FSROptimalSettings, StructType({ 0xb8d650de, 0xc557, 0x4293, { 0xad, 0x50, 0x7c, 0xa, 0x17, 0xca, 0xcf, 0x56 } }), kStructVersion1)
    uint32_t optimalRenderWidth{};
    uint32_t optimalRenderHeight{};
    uint32_t renderWidthMin{};
    uint32_t renderHeightMin{};
    uint32_t renderWidthMax{};
    uint32_t renderHeightMax{};
SL_STRUCT_END()

// {2CB38576-2B67-42CB-9098-14B58DE21D50}
SL_STRUCT_BEGIN(FSRState, StructType({ 0x2cb38576, 0x2b67, 0x42cb, { 0x90, 0x98, 0x14, 0xb5, 0x8d, 0xe2, 0x1d, 0x50 } }), kStructVersion1)
    uint64_t estimatedVRAMUsageInBytes{};
    uint32_t providerVersionMajor{};
    uint32_t providerVersionMinor{};
    uint32_t providerVersionPatch{};
SL_STRUCT_END()

}

using PFun_slFSRGetOptimalSettings = sl::Result(const sl::FSROptions& options, sl::FSROptimalSettings& settings);
using PFun_slFSRGetState = sl::Result(const sl::ViewportHandle& viewport, sl::FSRState& state);
using PFun_slFSRSetOptions = sl::Result(const sl::ViewportHandle& viewport, const sl::FSROptions& options);

inline sl::Result slFSRGetOptimalSettings(const sl::FSROptions& options, sl::FSROptimalSettings& settings)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR, slFSRGetOptimalSettings);
    return s_slFSRGetOptimalSettings(options, settings);
}

inline sl::Result slFSRGetState(const sl::ViewportHandle& viewport, sl::FSRState& state)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR, slFSRGetState);
    return s_slFSRGetState(viewport, state);
}

inline sl::Result slFSRSetOptions(const sl::ViewportHandle& viewport, const sl::FSROptions& options)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR, slFSRSetOptions);
    return s_slFSRSetOptions(viewport, options);
}
