# Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.

# Assembles the runtime package from Production build output and pinned vendor binaries.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$config = Import-PowerShellDataFile (Join-Path $root "config\project-release.psd1")

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Copy-PinnedFile([string]$Source, [string]$ExpectedHash, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Pinned vendor file is missing: $Source"
    }
    if ((Get-Sha256 $Source) -ne $ExpectedHash.ToLowerInvariant()) {
        throw "Pinned vendor file hash mismatch: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination
}

$archive = Join-Path $CacheDirectory "streamline-sdk-$($config.Upstream.Version).zip"
$upstream = Join-Path $CacheDirectory "streamline-sdk-$($config.Upstream.Version)"
New-Item -ItemType Directory -Force $CacheDirectory | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    Invoke-WebRequest -UseBasicParsing -Uri $config.Upstream.Url -OutFile $archive
}
if ((Get-Sha256 $archive) -ne $config.Upstream.Sha256.ToLowerInvariant()) {
    throw "Pinned upstream archive hash mismatch."
}
if (-not (Test-Path -LiteralPath $upstream -PathType Container)) {
    Expand-Archive -LiteralPath $archive -DestinationPath $upstream
}

if (Test-Path -LiteralPath $OutputDirectory) {
    throw "OutputDirectory already exists: $OutputDirectory"
}
$package = Join-Path $OutputDirectory "package"
$bin = Join-Path $package "bin\x64"
$symbols = Join-Path $OutputDirectory "symbols"
New-Item -ItemType Directory -Force $bin, $symbols | Out-Null

foreach ($file in $config.Runtime) {
    $destination = Join-Path $bin $file.Name
    if ($file.ContainsKey("BuildProject")) {
        $sourceDirectory = Join-Path $BuildRoot "$($file.BuildProject)\Production_x64"
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $file.Name) -Destination $destination
        $symbolName = if ($file.ContainsKey("SymbolName")) { $file.SymbolName } else { "$($file.BuildProject).pdb" }
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $symbolName) -Destination $symbols
    }
    elseif ($file.ContainsKey("SourcePath")) {
        Copy-PinnedFile (Join-Path $root $file.SourcePath) $file.Sha256 $destination
    }
    else {
        Copy-PinnedFile (Join-Path $upstream $file.ArchivePath) $file.Sha256 $destination
    }
}
foreach ($license in $config.Licenses) {
    $source = if ($license.ContainsKey("SourcePath")) {
        Join-Path $root $license.SourcePath
    } else {
        Join-Path $upstream $license.ArchivePath
    }
    $destination = Join-Path $package $license.OutputPath
    New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $root "3rd-party-licenses.md") -Destination $package
Copy-Item -LiteralPath (Join-Path $root "include") -Destination $package -Recurse
