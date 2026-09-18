$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$artifacts = Join-Path $root "_artifacts\tests\fsr-provider-runtime"
$upscaler = Join-Path $root "_artifacts\cs_fidelityfx_upscaler_dx12\Production_x64\cs_fidelityfx_upscaler_dx12.dll"
$frameGeneration = Join-Path $root "_artifacts\cs_fidelityfx_framegeneration_dx12\Production_x64\cs_fidelityfx_framegeneration_dx12.dll"
$officialUpscaler = Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll"
$officialFrameGeneration = Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\signedbin\amd_fidelityfx_framegeneration_dx12.dll"
$colorAdapterShader = Join-Path $root "_artifacts\shaders\fsr_color_conversion.cs"
$sdk = Join-Path $root "external\fidelityfx-sdk"
$sdkCommit = "60f4ea81909200d8542eca14dccb2628b763a9a3"
$rawUpscale = (git -C $sdk show "${sdkCommit}:Kits/FidelityFX/upscalers/include/ffx_upscale.h") -join "`n"
if ($LASTEXITCODE -ne 0 -or
    $rawUpscale -notmatch "FFX_UPSCALER_VERSION_MAJOR 4" -or
    $rawUpscale -notmatch "FFX_UPSCALER_VERSION_MINOR 1" -or
    $rawUpscale -notmatch "FFX_UPSCALER_VERSION_PATCH 1" -or
    $rawUpscale -notmatch "FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = \(1 << 1\)" -or
    $rawUpscale -notmatch "FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ   = \(1 << 2\)" -or
    $rawUpscale -match "GAMMA_2_2") {
    throw "Pinned unmodified FSR 4.1.1 color API does not match the expected sRGB/PQ-only contract."
}
$rawApiTypes = (git -C $sdk show "${sdkCommit}:Kits/FidelityFX/api/include/ffx_api_types.h") -join "`n"
if ($LASTEXITCODE -ne 0 -or
    $rawApiTypes -notmatch "FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB" -or
    $rawApiTypes -notmatch "FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ" -or
    $rawApiTypes -notmatch "FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB" -or
    $rawApiTypes -match "GAMMA_2_2") {
    throw "Pinned unmodified MLFG transfer API does not match the expected sRGB/PQ/scRGB-only contract."
}
$rawFrameGeneration = (git -C $sdk show "${sdkCommit}:Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h") -join "`n"
if ($LASTEXITCODE -ne 0 -or
    $rawFrameGeneration -notmatch "FFX_FRAMEGENERATION_VERSION_MAJOR 4" -or
    $rawFrameGeneration -notmatch "FFX_FRAMEGENERATION_VERSION_MINOR 0" -or
    $rawFrameGeneration -notmatch "FFX_FRAMEGENERATION_VERSION_PATCH 1" -or
    $rawFrameGeneration -notmatch "backbufferTransferFunction") {
    throw "Pinned unmodified MLFG 4.0.1 public dispatch contract was not found."
}
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "cl.exe is unavailable. Run this script from a Visual Studio developer shell."
}
if (-not (Test-Path -LiteralPath $upscaler -PathType Leaf) -or
    -not (Test-Path -LiteralPath $frameGeneration -PathType Leaf) -or
    -not (Test-Path -LiteralPath $colorAdapterShader -PathType Leaf)) {
    throw "Build the Production FidelityFX provider and shader targets before running this regression."
}

New-Item -ItemType Directory -Force $artifacts | Out-Null
$test = Join-Path $artifacts "fsr-provider-runtime-regression.exe"
& $compiler.Source /nologo /std:c++20 /EHsc /W4 /WX /DNOMINMAX `
    "/I$root" `
    (Join-Path $PSScriptRoot "fsr-provider-runtime-regression.cpp") `
    (Join-Path $root "source\plugins\sl.fsr.common\providerCapabilities.cpp") `
    d3d12.lib d3dcompiler.lib dxgi.lib user32.lib version.lib `
    "/Fo:$artifacts\\" "/Fe:$test"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build FidelityFX provider runtime regression test."
}

& $test $upscaler $frameGeneration $officialUpscaler $officialFrameGeneration `
    (Join-Path $PSScriptRoot "fsr-color-transfer-regression.hlsl") `
    $colorAdapterShader
if ($LASTEXITCODE -ne 0) {
    throw "FidelityFX provider runtime regression test failed."
}
