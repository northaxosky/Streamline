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
#include "sl_fsr.h"

namespace sl
{

enum class FSRGMode : uint32_t
{
    eOff,
    eOn,
    eCount
};

enum class FSRGCompletionMode : uint32_t
{
    //! No private frame-generation swapchain exists, so capability is not known yet.
    eNone,
    //! Host-input completion is exposed through completionFence and completionFenceValue.
    eFence,
    //! A private swapchain exists, but its provider does not expose host-input completion.
    eVendorCompletionUnavailable
};

// {BDE29A5F-0E62-4506-9196-5E61310C08BD}
SL_STRUCT_BEGIN(FSRGOptions, StructType({ 0xbde29a5f, 0xe62, 0x4506, { 0x91, 0x96, 0x5e, 0x61, 0x31, 0xc, 0x8, 0xbd } }), kStructVersion1)
    FSRGMode mode = FSRGMode::eOff;
    uint32_t displayWidth = INVALID_UINT;
    uint32_t displayHeight = INVALID_UINT;
    uint32_t maxRenderWidth = INVALID_UINT;
    uint32_t maxRenderHeight = INVALID_UINT;
    uint32_t backBufferFormat{};
    FSRColorSpace colorSpace = FSRColorSpace::eSRGB;
    float frameTimeDeltaMilliseconds{};
    float viewSpaceToMetersFactor = 1.0f;
    Boolean onlyPresentGenerated = Boolean::eFalse;
SL_STRUCT_END()

// {79D096DE-E8AF-453D-85D9-A344DF3FB48F}
SL_STRUCT_BEGIN(FSRGState, StructType({ 0x79d096de, 0xe8af, 0x453d, { 0x85, 0xd9, 0xa3, 0x44, 0xdf, 0x3f, 0xb4, 0x8f } }), kStructVersion1)
    uint64_t estimatedVRAMUsageInBytes{};
    uint32_t frameGenerationVersionMajor{};
    uint32_t frameGenerationVersionMinor{};
    uint32_t frameGenerationVersionPatch{};
    uint32_t swapchainVersionMajor{};
    uint32_t swapchainVersionMinor{};
    uint32_t swapchainVersionPatch{};
    FSRGCompletionMode completionMode = FSRGCompletionMode::eNone;
    //! AddRef'd ID3D12Fence covering every host-input read scheduled by the latest swapchain Present. The caller must Release it.
    void* completionFence{};
    //! Fence value paired with completionFence. Zero means the capability is supported but no host-input dependency has been submitted.
    uint64_t completionFenceValue{};
    Boolean active = Boolean::eFalse;
SL_STRUCT_END()

}

using PFun_slFSRGGetState = sl::Result(const sl::ViewportHandle& viewport, sl::FSRGState& state);
using PFun_slFSRGSetOptions = sl::Result(const sl::ViewportHandle& viewport, const sl::FSRGOptions& options);
//! Waits for pending generated presents and destroys active algorithm contexts before an owner switch.
using PFun_slFSRGQuiesce = sl::Result();

inline sl::Result slFSRGGetState(const sl::ViewportHandle& viewport, sl::FSRGState& state)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR_G, slFSRGGetState);
    return s_slFSRGGetState(viewport, state);
}

inline sl::Result slFSRGSetOptions(const sl::ViewportHandle& viewport, const sl::FSRGOptions& options)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR_G, slFSRGSetOptions);
    return s_slFSRGSetOptions(viewport, options);
}

inline sl::Result slFSRGQuiesce()
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR_G, slFSRGQuiesce);
    return s_slFSRGQuiesce();
}
