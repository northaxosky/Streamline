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
    //! Pure power-law gamma 2.2. Project FSR 3 handles this in its patched
    //! shaders. For official FSR 4, the plugin decodes to linear RGBA16F,
    //! dispatches the unmodified provider without a nonlinear flag, and
    //! encodes the output back to Gamma 2.2.
    eGamma22 = 3,
    eCount = 4
};

//! FidelityFX upscaling implementation selected for this viewport.
enum class FSRAlgorithm : uint32_t
{
    //! Source-built FSR 3.1.5 provider.
    eFSR3,
    //! Official signed FSR 4.1.1 machine-learning provider.
    eFSR4,
    eCount
};

//! Why a requested FidelityFX algorithm cannot be activated.
enum class FSRUnavailableReason : uint32_t
{
    eNone,
    //! The authenticated module was not present or could not be loaded.
    eModuleUnavailable,
    //! The current operating system does not meet the documented requirement.
    eOperatingSystemUnsupported,
    //! The D3D12 runtime/device does not expose the required shader model.
    eRuntimeUnsupported,
    //! The provider rejected the current GPU. This is reported only after an
    //! actual provider-version query, never from PCI identification alone.
    eHardwareUnsupported,
    //! The exact requested provider version was not returned for this device.
    eProviderUnavailable
};

//! D3D12 runtime which created the device passed to the FidelityFX provider.
enum class FSRD3D12RuntimeSource : uint32_t
{
    eUnknown,
    //! The active D3D12Core.dll is the Windows system runtime.
    eSystem,
    //! The active D3D12Core.dll is outside System32, including devices created
    //! through the Agility SDK export or ID3D12SDKConfiguration1 factory paths.
    eAgilitySDK
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
    //! Must describe the actual scaling-input signal. eGamma22 uses the
    //! plugin's linear RGBA16F adapter for official FSR 4 and is never sent
    //! to that provider as an invented nonlinear dispatch flag.
    FSRColorSpace colorSpace = FSRColorSpace::eSRGB;
    Boolean useAutoExposure = Boolean::eFalse;
    Boolean dynamicResolutionEnabled = Boolean::eFalse;
SL_STRUCT_END()

//! Optional structure chained to FSROptions. Omit it to retain FSR 3 behavior.
// {10D1CFA1-BC23-4B70-917C-ACD4EE1A84D3}
SL_STRUCT_BEGIN(FSRAlgorithmOptions, StructType({ 0x10d1cfa1, 0xbc23, 0x4b70, { 0x91, 0x7c, 0xac, 0xd4, 0xee, 0x1a, 0x84, 0xd3 } }), kStructVersion1)
    FSRAlgorithm algorithm = FSRAlgorithm::eFSR3;
SL_STRUCT_END()

// {62FFB716-61EC-47EA-97F2-3CAD898B7A26}
SL_STRUCT_BEGIN(FSRCapabilities, StructType({ 0x62ffb716, 0x61ec, 0x47ea, { 0x97, 0xf2, 0x3c, 0xad, 0x89, 0x8b, 0x7a, 0x26 } }), kStructVersion2)
    FSRAlgorithm algorithm = FSRAlgorithm::eFSR3;
    Boolean available = Boolean::eFalse;
    FSRUnavailableReason unavailableReason = FSRUnavailableReason::eProviderUnavailable;
    uint32_t providerVersionMajor{};
    uint32_t providerVersionMinor{};
    uint32_t providerVersionPatch{};
    // kStructVersion2
    Boolean windows11OrGreater = Boolean::eFalse;
    uint32_t shaderModelMajor{};
    uint32_t shaderModelMinor{};
    FSRD3D12RuntimeSource d3d12RuntimeSource =
        FSRD3D12RuntimeSource::eUnknown;
    uint32_t d3d12CoreVersionMajor{};
    uint32_t d3d12CoreVersionMinor{};
    uint32_t d3d12CoreVersionPatch{};
    uint32_t d3d12CoreVersionRevision{};
    //! Value exported as D3D12SDKVersion by the main executable. Zero when the
    //! export is absent; an ID3D12SDKConfiguration1 device-factory activation
    //! is instead identified by the active non-system D3D12Core version.
    uint32_t requestedD3D12SDKVersion{};
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
SL_STRUCT_BEGIN(FSRState, StructType({ 0x2cb38576, 0x2b67, 0x42cb, { 0x90, 0x98, 0x14, 0xb5, 0x8d, 0xe2, 0x1d, 0x50 } }), kStructVersion2)
    uint64_t estimatedVRAMUsageInBytes{};
    uint32_t providerVersionMajor{};
    uint32_t providerVersionMinor{};
    uint32_t providerVersionPatch{};
    // kStructVersion2
    FSRAlgorithm algorithm = FSRAlgorithm::eFSR3;
    Boolean available = Boolean::eFalse;
    FSRUnavailableReason unavailableReason = FSRUnavailableReason::eProviderUnavailable;
SL_STRUCT_END()

}

using PFun_slFSRGetOptimalSettings = sl::Result(const sl::FSROptions& options, sl::FSROptimalSettings& settings);
using PFun_slFSRGetState = sl::Result(const sl::ViewportHandle& viewport, sl::FSRState& state);
using PFun_slFSRSetOptions = sl::Result(const sl::ViewportHandle& viewport, const sl::FSROptions& options);
using PFun_slFSRGetCapabilities = sl::Result(sl::FSRAlgorithm algorithm, sl::FSRCapabilities& capabilities);

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

inline sl::Result slFSRGetCapabilities(sl::FSRAlgorithm algorithm, sl::FSRCapabilities& capabilities)
{
    SL_FEATURE_FUN_IMPORT_STATIC(sl::kFeatureFSR, slFSRGetCapabilities);
    return s_slFSRGetCapabilities(algorithm, capabilities);
}
