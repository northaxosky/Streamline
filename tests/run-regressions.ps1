# Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$artifacts = Join-Path $root "_artifacts\tests"
$swapChainSource = Join-Path $root "source\core\sl.interposer\dxgi\dxgiSwapchain.cpp"
$genericSource = Join-Path $root "source\platforms\sl.chi\generic.cpp"

$swapChainText = Get-Content -Raw $swapChainSource
$presentWiring = [regex]::Matches($swapChainText, "return invokePresent\(").Count
if ($presentWiring -ne 2) {
    throw "Expected Present and Present1 to use invokePresent; found $presentWiring call(s)."
}
$presentRequirements = @(
    "return m_base->Present(SyncInterval, Flags);",
    "return static_cast<IDXGISwapChain1*>(m_base)->Present1(SyncInterval, PresentFlags, pPresentParameters);",
    "((PFunPresentAfter*)hook)(Flags)",
    "((PFunPresentAfter*)hook)(PresentFlags)"
)
foreach ($requirement in $presentRequirements) {
    if (-not $swapChainText.Contains($requirement)) {
        throw "DXGISwapChain present wiring is missing: $requirement"
    }
}

$genericText = Get-Content -Raw $genericSource
$vendorStart = $genericText.IndexOf("ComputeStatus Generic::getVendorId")
$vendorEnd = $genericText.IndexOf("ComputeStatus Generic::startTrackingResource", $vendorStart)
if ($vendorStart -lt 0 -or $vendorEnd -lt 0) {
    throw "Could not locate Generic::getVendorId."
}
$vendorBody = $genericText.Substring($vendorStart, $vendorEnd - $vendorStart)
$vendorRequirements = @(
    "Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice",
    "QueryInterface(dxgiDevice.GetAddressOf())",
    "Microsoft::WRL::ComPtr<IDXGIAdapter> adapter",
    "GetAdapter(adapter.GetAddressOf())",
    "adapter->GetDesc(&desc)"
)
foreach ($requirement in $vendorRequirements) {
    if (-not $vendorBody.Contains($requirement)) {
        throw "Generic::getVendorId is missing lifetime guard: $requirement"
    }
}
if ($vendorBody.Contains("->Release()")) {
    throw "Generic::getVendorId releases a queried interface before its last use."
}

New-Item -ItemType Directory -Force $artifacts | Out-Null
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "cl.exe is unavailable. Run this script from a Visual Studio developer shell."
}

& $compiler.Source /nologo /std:c++20 /EHsc /W4 /WX `
    /I $root `
    (Join-Path $PSScriptRoot "present-result-regression.cpp") `
    "/Fo:$artifacts\present-result-regression.obj" `
    "/Fe:$artifacts\present-result-regression.exe"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build present-result-regression.cpp."
}

& "$artifacts\present-result-regression.exe"
if ($LASTEXITCODE -ne 0) {
    throw "Present result regression test failed."
}

$colorTest = Join-Path $artifacts "fsr-color-contract-regression.exe"
& $compiler.Source /nologo /std:c++20 /EHsc /W4 /WX `
    /I $root `
    (Join-Path $PSScriptRoot "fsr-color-contract-regression.cpp") `
    "/Fo:$artifacts\fsr-color-contract-regression.obj" `
    "/Fe:$colorTest"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build fsr-color-contract-regression.cpp."
}

& $colorTest `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\upscalers\fsr3\internal\ffx_provider_fsr3upscale.cpp") `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\upscalers\fsr3\include\gpu\fsr3upscaler\ffx_fsr3upscaler_callbacks_hlsl.h") `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\api\internal\gpu\ffx_core_gpu_common.h") `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\framegeneration\fsr3\include\gpu\frameinterpolation\ffx_frameinterpolation_common.h") `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\framegeneration\fsr3\include\gpu\opticalflow\ffx_opticalflow_prepare_luma.h") `
    (Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\framegeneration\fsr3\dx12\FrameInterpolationSwapchainDX12.cpp") `
    (Join-Path $root "source\plugins\sl.fsr\fsrEntry.cpp") `
    (Join-Path $root "source\plugins\sl.fsr_g\fsrGEntry.cpp")
if ($LASTEXITCODE -ne 0) {
    throw "FSR color contract regression test failed."
}

& (Join-Path $PSScriptRoot "run-project-trust-regressions.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "Project trust regressions failed."
}

& (Join-Path $PSScriptRoot "run-fsr-provider-runtime-regression.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "FidelityFX provider runtime regression failed."
}

Write-Host "Streamline regression tests passed."
