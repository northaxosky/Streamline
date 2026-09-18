$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$artifacts = Join-Path $root "_artifacts\tests\fsr-provider-runtime"
$upscaler = Join-Path $root "_artifacts\cs_fidelityfx_upscaler_dx12\Production_x64\amd_fidelityfx_upscaler_dx12.dll"
$frameGeneration = Join-Path $root "_artifacts\cs_fidelityfx_framegeneration_dx12\Production_x64\amd_fidelityfx_framegeneration_dx12.dll"
$loader = Join-Path $root "external\fidelityfx-sdk\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll"
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "cl.exe is unavailable. Run this script from a Visual Studio developer shell."
}
if (-not (Test-Path -LiteralPath $upscaler -PathType Leaf) -or
    -not (Test-Path -LiteralPath $frameGeneration -PathType Leaf)) {
    throw "Build the Production FidelityFX provider targets before running this regression."
}

New-Item -ItemType Directory -Force $artifacts | Out-Null
$test = Join-Path $artifacts "fsr-provider-runtime-regression.exe"
& $compiler.Source /nologo /std:c++20 /EHsc /W4 /WX /DNOMINMAX `
    "/I$root" `
    (Join-Path $PSScriptRoot "fsr-provider-runtime-regression.cpp") `
    d3d12.lib dxgi.lib user32.lib `
    "/Fo:$artifacts\\" "/Fe:$test"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build FidelityFX provider runtime regression test."
}

& $test $upscaler $frameGeneration $loader
if ($LASTEXITCODE -ne 0) {
    throw "FidelityFX provider runtime regression test failed."
}
