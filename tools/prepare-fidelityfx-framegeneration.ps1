param(
    [switch]$ShortPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root "external\fidelityfx-sdk"
$sdkAlias = Join-Path $root "_ffx"
if (-not $ShortPath) {
    if (Test-Path -LiteralPath $sdkAlias) {
        $alias = Get-Item -LiteralPath $sdkAlias
        if ($alias.LinkType -ne "Junction") {
            throw "The FidelityFX source alias exists but is not a junction: $sdkAlias"
        }
    }
    else {
        New-Item -ItemType Junction -Path $sdkAlias -Target $sdk | Out-Null
    }
}
if (-not $ShortPath -and $root.Length -gt 80) {
    $drive = [char[]]([char]'S'..[char]'Z') |
        Where-Object { -not (Test-Path "$_`:\") } |
        Select-Object -First 1
    if (-not $drive) {
        throw "A short drive path is required to generate FidelityFX shaders."
    }
    & subst "$drive`:" $root
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create a short FidelityFX build path."
    }
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$drive`:\tools\prepare-fidelityfx-framegeneration.ps1" -ShortPath
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        exit 0
    }
    finally {
        & subst "$drive`:" /d
    }
}

$expectedCommit = "60f4ea81909200d8542eca14dccb2628b763a9a3"
$patch = Join-Path $root "patches\fidelityfx-sdk-2.3.0-fg-completion.patch"
$buildRoot = Join-Path $root "_artifacts\fidelityfx-sdk-build"
$archive = Join-Path $buildRoot "FidelityFX-SDK-v1.1.4.zip"
$toolRoot = Join-Path $buildRoot "sdk-1.1.4"
$tool = Join-Path $toolRoot "sdk\tools\binary_store\FidelityFX_SC.exe"
$pixInclude = Join-Path $toolRoot "sdk\libs\pix"
$generated = Join-Path $root "_ffxgen"
$stamp = Join-Path $generated "completion-build-inputs.txt"
$archiveUrl = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip"
$archiveSha256 = "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952"

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $sha256.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

$actualCommit = (& git -C $sdk rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -cne $expectedCommit) {
    throw "FidelityFX SDK must be pinned to $expectedCommit; found '$actualCommit'."
}

$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
& git -C $sdk apply --unidiff-zero --reverse --check $patch *> $null
$patchApplied = $LASTEXITCODE -eq 0
$ErrorActionPreference = $previousErrorAction
if (-not $patchApplied) {
    Invoke-Checked git @("-C", $sdk, "apply", "--unidiff-zero", "--check", $patch)
    Invoke-Checked git @("-C", $sdk, "apply", "--unidiff-zero", $patch)
}

New-Item -ItemType Directory -Force $buildRoot | Out-Null
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-WebRequest -Uri $archiveUrl -OutFile $archive
    }
    $actualArchiveSha256 = Get-Sha256 $archive
    if ($actualArchiveSha256 -cne $archiveSha256) {
        throw "FidelityFX build-tool archive hash mismatch: $actualArchiveSha256"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $toolRoot -Force
}
if (-not (Test-Path -LiteralPath $tool -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $pixInclude "pix3.h") -PathType Leaf)) {
    throw "The pinned FidelityFX build tools are incomplete."
}

$frameInterpolationShaders = @(
    "ffx_frameinterpolation_reconstruct_and_dilate_pass",
    "ffx_frameinterpolation_disocclusion_mask_pass",
    "ffx_frameinterpolation_reconstruct_previous_depth_pass",
    "ffx_frameinterpolation_setup_pass",
    "ffx_frameinterpolation_game_motion_vector_field_pass",
    "ffx_frameinterpolation_optical_flow_vector_field_pass",
    "ffx_frameinterpolation_pass",
    "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass",
    "ffx_frameinterpolation_compute_inpainting_pyramid_pass",
    "ffx_frameinterpolation_inpainting_pass",
    "ffx_frameinterpolation_debug_view_pass"
)
$opticalFlowShaders = @(
    "ffx_opticalflow_compute_luminance_pyramid_pass",
    "ffx_opticalflow_compute_optical_flow_advanced_pass_v5",
    "ffx_opticalflow_compute_scd_divergence_pass",
    "ffx_opticalflow_filter_optical_flow_pass_v5",
    "ffx_opticalflow_generate_scd_histogram_pass",
    "ffx_opticalflow_prepare_luma_pass",
    "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"
)
$upscalerShaders = @(
    "ffx_fsr3upscaler_autogen_reactive_pass",
    "ffx_fsr3upscaler_accumulate_pass",
    "ffx_fsr3upscaler_luma_pyramid_pass",
    "ffx_fsr3upscaler_prepare_reactivity_pass",
    "ffx_fsr3upscaler_prepare_inputs_pass",
    "ffx_fsr3upscaler_shading_change_pass",
    "ffx_fsr3upscaler_rcas_pass",
    "ffx_fsr3upscaler_shading_change_pyramid_pass",
    "ffx_fsr3upscaler_luma_instability_pass",
    "ffx_fsr3upscaler_debug_view_pass"
)
$variants = @(
    @{ Suffix = ""; ShaderModel = "62"; Profile = "cs_6_2"; Wave = @(); Half = "0" },
    @{ Suffix = "_wave64"; ShaderModel = "66"; Profile = "cs_6_6"; Wave = @('-DFFX_PREFER_WAVE64=[WaveSize(64)]'); Half = "0" },
    @{ Suffix = "_16bit"; ShaderModel = "62"; Profile = "cs_6_2"; Wave = @(); Half = "1" },
    @{ Suffix = "_wave64_16bit"; ShaderModel = "66"; Profile = "cs_6_6"; Wave = @('-DFFX_PREFER_WAVE64=[WaveSize(64)]'); Half = "1" }
)

$toolHash = Get-Sha256 $tool
$patchHash = Get-Sha256 $patch
$expectedStamp = "$expectedCommit`n$patchHash`n$toolHash`npermutation-entry-name-v2"
$requiredHeaders = foreach ($shader in $frameInterpolationShaders + $opticalFlowShaders + $upscalerShaders) {
    foreach ($variant in $variants) {
        Join-Path $generated "$shader$($variant.Suffix)_permutations.h"
    }
}
$upToDate = (Test-Path -LiteralPath $stamp -PathType Leaf) -and
    ((Get-Content -Raw -LiteralPath $stamp).TrimEnd() -ceq $expectedStamp) -and
    -not ($requiredHeaders | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($upToDate) {
    return
}

New-Item -ItemType Directory -Force $generated | Out-Null
$fidelityFx = Join-Path $sdk "Kits\FidelityFX"
$includeArguments = @(
    "-I", (Join-Path $fidelityFx "api\internal\include\gpu"),
    "-I", (Join-Path $fidelityFx "framegeneration\fsr3\include\gpu"),
    "-I", (Join-Path $fidelityFx "upscalers\fsr3\include\gpu")
)
$commonArguments = @(
    "-Zs",
    "-reflection",
    "-deps=gcc",
    "-DFFX_GPU=1",
    "-DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0",
    "-embed-arguments",
    "-E", "CS",
    "-Wno-for-redefinition",
    "-Wno-ambig-lit-shift",
    "-DFFX_HLSL=1"
)

function Build-Shaders(
    [string[]]$Names,
    [string]$SourceDirectory,
    [string[]]$EffectArguments) {
    foreach ($shader in $Names) {
        $source = Join-Path $SourceDirectory "$shader.hlsl"
        foreach ($variant in $variants) {
            $arguments = @($commonArguments + $EffectArguments)
            $arguments += "-name=$shader$($variant.Suffix)"
            $arguments += "-DFFX_HALF=$($variant.Half)"
            if ($variant.Half -eq "1") {
                $arguments += "-enable-16bit-types"
            }
            $arguments += $variant.Wave
            $arguments += "-DFFX_HLSL_SM=$($variant.ShaderModel)"
            $arguments += @("-T", $variant.Profile)
            $arguments += $includeArguments
            $arguments += "-output=$generated"
            $arguments += $source
            Invoke-Checked $tool $arguments
        }
    }
}

function Normalize-GeneratedHeaders([string[]]$Headers) {
    foreach ($header in $Headers) {
        $content = Get-Content -Raw -LiteralPath $header
        $content = $content -replace
            '(?m)(\s+const unsigned char\* blobData;\r?\n)',
            ('$1    const char*          entryName;' + [Environment]::NewLine)
        $content = $content -replace
            '(?m)(\{\s*g_[^,\r\n]+_size,\s*g_[^,\r\n]+_data,)',
            '$1 "CS",'
        Set-Content -LiteralPath $header -Value $content -Encoding ASCII
    }
}

Build-Shaders `
    $frameInterpolationShaders `
    (Join-Path $fidelityFx "framegeneration\fsr3\internal\shaders") `
    @(
    "-DFFX_FRAMEINTERPOLATION_EMBED_ROOTSIG=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}",
    "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}",
    "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}"
)
Build-Shaders `
    $opticalFlowShaders `
    (Join-Path $fidelityFx "framegeneration\fsr3\internal\shaders") `
    @(
    "-DFFX_OPTICALFLOW_EMBED_ROOTSIG=0",
    "-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}"
)
Build-Shaders `
    $upscalerShaders `
    (Join-Path $fidelityFx "upscalers\fsr3\internal\shaders") `
    @(
    "-DFFX_FSR3UPSCALER_EMBED_ROOTSIG=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
)
Normalize-GeneratedHeaders $requiredHeaders

[IO.File]::WriteAllText($stamp, "$expectedStamp`n", [Text.UTF8Encoding]::new($false))
