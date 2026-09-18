# Streamline FSR integration

This fork exposes native D3D12 FSR Super Resolution and Frame Generation as
`sl::kFeatureFSR` and `sl::kFeatureFSR_G`. Their public ABI is declared in
`sl_fsr.h` and `sl_fsr_g.h`. The packaged implementation uses FSR 3.1.5,
Frame Generation 3.1.6, and the frame-generation swapchain 3.1.7 from the
pinned FidelityFX SDK source.

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
with `pow(max(c, 0), 1 / 2.2)`. Alpha is not transfer-converted. Use
`FSROptions::useAutoExposure`, `preExposure`, and an optional
`kBufferTypeExposure` tag according to the source image contract; do not
pre-convert gamma-2.2 input to sRGB.

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
