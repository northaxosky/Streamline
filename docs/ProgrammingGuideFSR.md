# Streamline FSR integration

This fork exposes native D3D12 FSR Super Resolution and Frame Generation as
`sl::kFeatureFSR` and `sl::kFeatureFSR_G`. Their public ABI is declared in
`sl_fsr.h` and `sl_fsr_g.h`.

The default remains the source-built FSR 3 path: FSR Upscaling 3.1.5, Frame
Generation 3.1.6, and the patched Frame Generation Swapchain 3.1.7. Chain
`FSRAlgorithmOptions` or `FSRGAlgorithmOptions` to the existing options
structure to request the official signed FSR Upscaling 4.1.1 or FSR Frame
Generation 4.0.1 provider. Omitting the algorithm structure selects FSR 3 and
preserves the original ABI.

Call `slFSRGetCapabilities` or `slFSRGGetCapabilities` before changing the
active algorithm. Capability is based on the exact versions returned by the
provider for the current D3D12 device. A failed request returns
`eErrorFeatureNotSupported` without replacing the previously stored working
options. Never report FSR 3 as FSR 4 when the exact ML provider is absent.

Value-initialize public outputs with the header used by the caller, for example
`FSRState state{}` or `FSRGCapabilities capabilities{}`. Version-1 state
callers receive only the original fields. Version-2 state callers additionally
receive `algorithm`, `available`, and `unavailableReason`. Capability version 1
contains algorithm availability and exact provider versions; capability
version 2 appends Windows, shader-model, active-D3D12Core, and requested
Agility SDK diagnostics.

`available == eTrue` is paired with `unavailableReason == eNone` only after the
documented runtime checks and exact provider enumeration succeed. When the
official algorithm is unavailable, `available` is false, the reason identifies
the failed admission stage, and provider version fields remain zero if the
exact provider was not returned. In `FSRGState`, `active` means that a viewport
algorithm context currently exists; it does not replace the availability
result. Before viewport activation, state reports the selected/default
algorithm and its current admission result.

The runtime package keeps the official AMD modules intact:

* `amd_fidelityfx_upscaler_dx12.dll`
* `amd_fidelityfx_framegeneration_dx12.dll`

The source-built FSR 3 providers use the truthful, distinct names
`cs_fidelityfx_upscaler_dx12.dll` and
`cs_fidelityfx_framegeneration_dx12.dll`. Each algorithm context is called
only through the five-function public FFX API exported by the module that
created it. The official ML modules are not renamed, patched, or copied into a
project provider.

Creation, configuration, query, dispatch, and destruction of an algorithm
context always use the function table of the module that created that context.
FSR 3 contexts therefore remain owned by `cs_fidelityfx_*`, while FSR 4
contexts remain owned by `amd_fidelityfx_*`; contexts are never passed across
those function tables. The sole cross-provider seam is the public
frame-generation callback and opaque swapchain pointer described below. The
project 3.1.7 swapchain context itself remains owned exclusively by
`cs_fidelityfx_framegeneration_dx12.dll`.

Both official effect DLLs directly export `ffxCreateContext`,
`ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, and `ffxDispatch`, and neither
imports `amd_fidelityfx_loader_dx12.dll`. The plugin resolves those exports
from each authenticated effect module, so the generic AMD loader is not part
of this runtime closure.

### Pinned official API evidence

The pinned SDK identifies the signed algorithms and their public ABI in these
exact-revision anchors:

* `Kits/FidelityFX/docs/techniques/super-resolution-ml.md` identifies FSR
  Upscaling 4.1.1, requires the signed binary distribution, and directs
  non-linear inputs through the public upscale API.
* The
  [unmodified 4.1.1 upscale header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX/upscalers/include/ffx_upscale.h)
  defines only the official sRGB and PQ non-linear dispatch flags.
* The
  [unmodified ML frame-generation guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX/docs/techniques/frame-interpolation-ml.md)
  identifies Frame Generation 4.0.1, requires the public configure,
  `PrepareV2`, and generation-dispatch sequence, lists only optional sRGB/PQ
  conversion, and reserves additional color spaces for future support.
* The
  [unmodified 4.0.1 frame-generation header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h)
  uses `FfxApiBackbufferTransferFunction`; the
  [unmodified public API types](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/60f4ea81909200d8542eca14dccb2628b763a9a3/Kits/FidelityFX/api/include/ffx_api_types.h)
  define only sRGB, PQ, and scRGB values.

The prepared SDK in this repository is intentionally patched. The tracked
`patches/fidelityfx-sdk-2.3.0-fg-completion.patch` adds the pure Gamma 2.2
upscale flag and frame-generation transfer value together with the matching
source-built FSR 3 shader conversions. Those additions are project ABI for the
`cs_fidelityfx_*` providers only. The official ML adapter never sends those
values to the intact AMD DLLs.

The frame-generation algorithm/swapchain seam is likewise public and
algorithm-provider-independent within the pinned API.
`ffx_framegeneration.h` defines
`FfxApiFrameGenerationDispatchFunc` and carries that exact callback type plus
an opaque swapchain pointer in `ffxConfigureDescFrameGeneration`.
`frame-interpolation-api.md` specifies that the swapchain invokes the callback
with a complete `ffxDispatchDescFrameGeneration`, and that the callback calls
`ffxDispatch` on its algorithm context. The source-built 3.1.7 swapchain stores
the same public callback type in
`framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.h`; it does not
inspect or own the official 4.0.1 algorithm context. This is why the callback
can dispatch through the official module's function table while the
project-built swapchain remains the presentation and completion owner.

## Color contract

Set `FSROptions::colorSpace` and `FSRGOptions::colorSpace` to the transfer
function of the tagged color resources and swapchain:

| Value | Transfer function |
| --- | --- |
| `eLinear` | Linear light |
| `eSRGB` | IEC sRGB piecewise transfer |
| `ePQ` | Perceptual quantizer |
| `eGamma22` | Pure power-law gamma 2.2 |

`eGamma22` is not sRGB. It decodes RGB with `pow(max(c, 0), 2.2)` and encodes
with `pow(max(c, 0), 1 / 2.2)`. Alpha is not transfer-converted. The
source-built FSR 3 providers implement this project extension.

The unmodified official FSR 4 and ML frame-generation APIs do not define a
pure Gamma 2.2 value. For `eGamma22`, both plugins use one shared D3D12
conversion implementation rather than relabeling the signal or sending an
unknown flag:

* FSR 4 decodes the tagged input RGB to linear RGBA16F, preserves alpha,
  dispatches the official upscaler with no nonlinear flag into a linear
  RGBA16F output, then encodes RGB into the caller's Gamma 2.2 output.
* MLFG decodes the callback `presentColor` into per-frame linear RGBA16F,
  dispatches the official algorithm with the public scRGB transfer value and
  linear RGBA16F output targets, then encodes every generated output back into
  the swapchain-owned Gamma 2.2 target before returning from the callback.
* Optional HUD-less color is decoded into the same retained per-frame linear
  set before configure/PrepareV2. UI remains owned by the patched 3.1.7
  swapchain. Because generated outputs return to Gamma 2.2 before composition,
  both generated and real frames use the existing Gamma 2.2 UI/final
  presentation path.

The conversion changes RGB only. Alpha, exposure, pre-exposure, automatic
exposure, generation rectangles, and output-space ownership retain their
existing contracts. `eLinear`, `eSRGB`, and `ePQ` continue to use the official
values directly.

FSR 4 uses two full-resolution conversion dispatches. MLFG uses one source
conversion plus one conversion for each generated output, and an additional
conversion when HUD-less color is supplied. The adapter uses RGBA16F
intermediates. FSR scratch follows graphics-queue ordering; MLFG scratch is
retained per frame and is not recycled until the patched swapchain's AddRef'd
last-reader completion fence reaches the value covering that Present. This
keeps all callback, HUD-less, generated-output, UI-composition, and final
presentation readers inside the existing asynchronous retirement boundary.

## D3D12 resources and recording

Every tagged `sl::Resource` must contain the native `ID3D12Resource*`, its
actual D3D12 resource state, and the applicable extent. Supply current
`sl::Constants` and a frame token for each frame.

FSR Super Resolution requires:

| Tag | Lifetime |
| --- | --- |
| `kBufferTypeScalingInputColor` | `eValidUntilEvaluate` |
| `kBufferTypeScalingOutputColor` | `eValidUntilEvaluate` |
| `kBufferTypeDepth` | `eValidUntilEvaluate` |
| `kBufferTypeMotionVectors` | `eValidUntilEvaluate` |

`kBufferTypeExposure`, `kBufferTypeReactiveMaskHint`, and
`kBufferTypeTransparencyAndCompositionMaskHint` are optional. Call
`slFSRSetOptions`, then record `slEvaluateFeature(sl::kFeatureFSR, ...)` on an
open native D3D12 graphics command list. Submit that list before consuming the
output, and do not reuse tagged resources until the submitted GPU work
completes.

FSR Frame Generation requires `kBufferTypeDepth` and
`kBufferTypeMotionVectors` with `eValidUntilPresent`.
`kBufferTypeHUDLessColor` and `kBufferTypeUIColorAndAlpha` are optional and
must also remain valid through the frame-generation readers when supplied.
The frame sequence is:

1. Create the application swapchain while `sl.fsr_g` owns the Streamline
   frame-generation hooks.
2. Call `slFSRGSetOptions`, submit current constants and tags, and record
   `slEvaluateFeature(sl::kFeatureFSR_G, ...)` on the frame's native D3D12
   graphics command list.
3. Execute the preparation command list before the corresponding swapchain
   `Present`.
4. Call `Present` once. The private swapchain schedules interpolation, UI
   composition, and final presentation asynchronously.

ML frame generation uses the same patched source-built swapchain and the
official signed 4.0.1 algorithm context. Its required order is configure,
`ffxDispatchDescFrameGenerationPrepareV2`, then the algorithm dispatch invoked
by the swapchain during `Present`. `PrepareV2` includes the validated camera
position and full up/right/forward basis, vertical FOV in radians, near/far
planes, and view-space-to-meters factor.

## Completion and resource retirement

Call `slFSRGGetState` after private swapchain creation, before admitting the
borrowed-input path:

| Mode | Meaning |
| --- | --- |
| `eNone` | No private frame-generation swapchain exists; capability is not known. |
| `eFence` | `completionFence` is an AddRef'd `ID3D12Fence`; the caller must `Release` it. |
| `eVendorCompletionUnavailable` | A private swapchain exists, but the provider cannot report host-input completion. |

With `eFence`, value `0` is valid and means the capability is available but no
host-input dependency has been submitted. After `Present`, the returned fence
and value cover all interpolation, HUD-less, UI-composition, and final-present
reads scheduled for the latest application frame. `Present` returning, or the
application preparation list completing, does not retire those inputs.

Retire asynchronously: queue a GPU wait on the returned vendor fence, then
signal the application's existing retirement timeline. Do not add a per-frame
CPU wait. Do not admit borrowed FG inputs when the mode is
`eVendorCompletionUnavailable`.

The completion query always belongs to the patched source-built swapchain
context, including when the selected algorithm is official ML frame
generation. The official algorithm records into the swapchain-provided command
list through the public callback ABI; the swapchain's completion point
therefore remains the retirement authority for algorithm, HUD-less,
UI-composition, and final-present reads.

## ML capability requirements

FSR Upscaling 4 requires a supported AMD RX 7000-series discrete GPU, RX
9000-series GPU, or later supported hardware. FSR Frame Generation 4 requires
Windows 11, an RX 9000-series GPU or later, and a DirectX 12 Agility SDK/runtime
that supports Shader Model 6.6 (the vendor documentation specifies Agility SDK
1.4.9 or later). The pinned SDK 2.3.0 sample uses Agility SDK 1.616.1 and
`D3D12SDKVersion` 616.

The plugin does not enable ML algorithms from PCI identification alone. It
checks the documented OS requirement and the actual device's Shader Model 6.6
support, then asks the official provider module to enumerate the exact 4.1.1
or 4.0.1 version for that `ID3D12Device`. PCI identification is used only to
classify a failed provider query. Context creation is pinned to the enumerated
provider ID and validated by querying the created context. An unsupported
adapter such as an RTX 4090 keeps FSR 3 and DLSS available and reports
`eHardwareUnsupported` for the ML request.

Version-2 `FSRCapabilities` and `FSRGCapabilities` report the active
`D3D12Core.dll` file version and whether it came from the Windows system
runtime or a non-system Agility SDK path. They also report the actual device
shader model, Windows 11 status, and the main executable's requested
`D3D12SDKVersion` when that export exists. A zero requested version does not
exclude the typed `ID3D12SDKConfiguration1` device-factory path; the active
core and device capabilities remain authoritative.

Merely placing `D3D12Core.dll` beside a plugin does not activate it. With the
classic activation path, `D3D12SDKVersion` and `D3D12SDKPath` must be exported
by the main executable. A plugin DLL cannot supply those exports for
`Fallout4.exe`. The typed alternative must run before device creation:
obtain `ID3D12SDKConfiguration1` through `D3D12GetInterface`, call
`CreateDeviceFactory` with the pinned SDK version and path, and create the
device through the returned `ID3D12DeviceFactory` (or apply that factory to
global state before normal device creation). Neither path can retrofit an
already-created device.

The Fallout 4 host uses the typed path before creating its private D3D12
device. It looks for the exact 1.616.1 runtime at
`Streamline/D3D12/D3D12Core.dll`, dynamically resolves `D3D12GetInterface`,
obtains `ID3D12SDKConfiguration1`, and calls `CreateDeviceFactory` with SDK
version 616 and the absolute UTF-8 `Streamline/D3D12/` directory. The private
device is created through that `ID3D12DeviceFactory`, which remains alive for
the device lifetime. This does not require executable exports or
`ApplyToGlobalState`. If the exact runtime, API, factory, or device creation is
unavailable, the host falls back to the system D3D12 path before a device is
published; the capability result for the device that was actually created
remains authoritative.

Successful compilation and contract tests prove loading, ABI selection, color
flags, and retirement behavior. They are not evidence that ML rendering works
on supported AMD hardware; that requires Windows 11 testing on qualifying RX
hardware with the required Agility runtime.

## Ownership and shutdown

`sl.fsr_g` and `sl.dlss_g` are runtime-mutually-exclusive swapchain owners.
Select exactly one before creating the private presentation chain. To switch
owners, resize, or tear down, stop new FG preparation, call `slFSRGQuiesce`,
retire outstanding inputs, disable the old owner, and recreate the underlying
swapchain for the new owner.

When FG is off, use the plain D3D12 presentation path. Skip FG-only capture,
tags, configuration, evaluation, and latency work; setting an interpolation
weight or equivalent to zero is not the FG-off fast path. Temporary menu
suspension may retain the selected chain, but must not prepare unnecessary FG
work.
