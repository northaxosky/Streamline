# Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet("GeneratePublicHeader", "Assemble", "Validate", "ValidateTag")]
    [string]$Mode,

    [string]$OutputPath,
    [string]$BuildRoot,
    [string]$CacheDirectory,
    [string]$CandidateDirectory,
    [string]$SourceCommit,
    [string]$Tag
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$configPath = Join-Path $root "config\project-release.psd1"
$config = Import-PowerShellDataFile $configPath

function Assert-Required([string]$Value, [string]$Name) {
    if (-not $Value) {
        throw "$Name is required for mode $Mode."
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force $parent | Out-Null
    }
    [IO.File]::WriteAllText(
        [IO.Path]::GetFullPath($Path),
        $Content,
        [Text.UTF8Encoding]::new($false))
}

function Get-PublicHeader {
    $keyBytes = [Convert]::FromHexString([string]$config.PublicKeyXY)
    if ($keyBytes.Length -ne 64) {
        throw "PublicKeyXY must contain exactly 64 bytes."
    }
    $initializer = ($keyBytes | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
    return @"
// Generated from config/project-release.psd1. Contains no private material.
#pragma once
#define SL_PROJECT_TRUST_CONFIGURED 1
#define SL_PROJECT_TRUST_KEY_ID $($config.KeyId)u
#define SL_PROJECT_TRUST_RELEASE_ID "$($config.ReleaseId)"
#define SL_PROJECT_TRUST_PUBLIC_KEY_XY { $initializer }
"@
}

function Assert-OfficialFile([string]$Path, [string]$ExpectedHash) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Pinned upstream file is missing: $Path"
    }
    $actual = Get-Sha256 $Path
    if ($actual -ne $ExpectedHash.ToLowerInvariant()) {
        throw "Pinned upstream file hash mismatch: $Path"
    }
}

function Get-InventoryEntries([string]$Directory, [string]$Pattern = "*") {
    return @(
        Get-ChildItem -LiteralPath $Directory -Filter $Pattern -File |
            Sort-Object Name |
            ForEach-Object {
                [ordered]@{
                    name = $_.Name
                    size = [uint64]$_.Length
                    sha256 = Get-Sha256 $_.FullName
                }
            })
}

switch ($Mode) {
    "GeneratePublicHeader" {
        Assert-Required $OutputPath "OutputPath"
        Write-Utf8NoBom $OutputPath (Get-PublicHeader)
    }
    "ValidateTag" {
        Assert-Required $Tag "Tag"
        if ($Tag -cne $config.ReleaseTag) {
            throw "Release tag '$Tag' must exactly match '$($config.ReleaseTag)'."
        }
    }
    "Assemble" {
        Assert-Required $BuildRoot "BuildRoot"
        Assert-Required $CacheDirectory "CacheDirectory"
        Assert-Required $CandidateDirectory "CandidateDirectory"
        Assert-Required $SourceCommit "SourceCommit"

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

        if (Test-Path -LiteralPath $CandidateDirectory) {
            throw "CandidateDirectory already exists: $CandidateDirectory"
        }
        $package = Join-Path $CandidateDirectory "package"
        $bin = Join-Path $package "bin\x64"
        $symbols = Join-Path $CandidateDirectory "symbols"
        $provenance = Join-Path $CandidateDirectory "provenance"
        New-Item -ItemType Directory -Force $bin,$symbols,$provenance | Out-Null

        $specFiles = @()
        foreach ($file in $config.Runtime) {
            $destination = Join-Path $bin $file.Name
            if ($file.ContainsKey("BuildProject")) {
                $sourceDirectory = Join-Path $BuildRoot "$($file.BuildProject)\Production_x64"
                Copy-Item -LiteralPath (Join-Path $sourceDirectory $file.Name) -Destination $destination
                Copy-Item -LiteralPath (Join-Path $sourceDirectory "$($file.BuildProject).pdb") -Destination $symbols
            }
            else {
                $source = Join-Path $upstream $file.ArchivePath
                Assert-OfficialFile $source $file.Sha256
                Copy-Item -LiteralPath $source -Destination $destination
            }
            $specFiles += [ordered]@{ name = $file.Name; role = $file.Role }
        }
        foreach ($license in $config.Licenses) {
            $source = Join-Path $upstream $license.ArchivePath
            $destination = Join-Path $package $license.OutputPath
            New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
        }
        Copy-Item -LiteralPath (Join-Path $root "3rd-party-licenses.md") -Destination $package

        $spec = [ordered]@{
            releaseId = $config.ReleaseId
            keyId = [uint32]$config.KeyId
            files = $specFiles
        }
        Write-Utf8NoBom (Join-Path $provenance "release-spec.json") (
            $spec | ConvertTo-Json -Depth 5)
        Write-Utf8NoBom (Join-Path $provenance "projectTrust.generated.h") (
            Get-PublicHeader)

        $components = @(
            foreach ($file in $config.Runtime) {
                $item = Get-Item -LiteralPath (Join-Path $bin $file.Name)
                $signature = Get-AuthenticodeSignature $item.FullName
                [ordered]@{
                    name = $file.Name
                    role = $file.Role
                    origin = if ($file.ContainsKey("BuildProject")) {
                        "rebuilt-source"
                    } else {
                        "official-nvidia-$($config.Upstream.Version)"
                    }
                    size = [uint64]$item.Length
                    sha256 = Get-Sha256 $item.FullName
                    fileVersion = $item.VersionInfo.FileVersion
                    authenticodeStatus = $signature.Status.ToString()
                }
            })
        $inventory = [ordered]@{
            schemaVersion = 1
            state = "unsigned-candidate"
            releaseId = $config.ReleaseId
            releaseTag = $config.ReleaseTag
            keyId = [uint32]$config.KeyId
            sourceCommit = $SourceCommit
            build = [ordered]@{
                configuration = "Production"
                platform = "x64"
                generator = "vs2022"
                publicConfigSha256 = Get-Sha256 $configPath
                generatedTrustHeaderSha256 =
                    Get-Sha256 (Join-Path $provenance "projectTrust.generated.h")
                upstreamVersion = $config.Upstream.Version
                upstreamArchiveSha256 = $config.Upstream.Sha256
            }
            components = $components
            symbols = Get-InventoryEntries $symbols "*.pdb"
            licenses = @(
                Get-ChildItem -LiteralPath $package -Recurse -File |
                    Where-Object Extension -in ".txt",".md" |
                    Sort-Object FullName |
                    ForEach-Object {
                        [ordered]@{
                            path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace("\","/")
                            size = [uint64]$_.Length
                            sha256 = Get-Sha256 $_.FullName
                        }
                    })
        }
        Write-Utf8NoBom (Join-Path $provenance "inventory.json") (
            $inventory | ConvertTo-Json -Depth 8)
    }
    "Validate" {
        Assert-Required $CandidateDirectory "CandidateDirectory"
        Assert-Required $SourceCommit "SourceCommit"
        $package = Join-Path $CandidateDirectory "package"
        $bin = Join-Path $package "bin\x64"
        $provenance = Join-Path $CandidateDirectory "provenance"
        $inventory = Get-Content -Raw -LiteralPath (
            Join-Path $provenance "inventory.json") | ConvertFrom-Json
        if ($inventory.releaseId -cne $config.ReleaseId -or
            $inventory.releaseTag -cne $config.ReleaseTag -or
            $inventory.sourceCommit -cne $SourceCommit -or
            $inventory.build.publicConfigSha256 -cne (Get-Sha256 $configPath)) {
            throw "Candidate provenance does not match the trusted release configuration."
        }
        $expectedNames = @($config.Runtime.Name | Sort-Object)
        $actualNames = @(Get-ChildItem -LiteralPath $bin -Filter "*.dll" -File |
            Select-Object -ExpandProperty Name | Sort-Object)
        if (Compare-Object $expectedNames $actualNames) {
            throw "Candidate runtime DLL set does not match the release configuration."
        }
        foreach ($file in $config.Runtime) {
            $path = Join-Path $bin $file.Name
            $record = $inventory.components | Where-Object name -CEQ $file.Name
            if (-not $record -or $record.sha256 -cne (Get-Sha256 $path) -or
                [uint64]$record.size -ne [uint64](Get-Item $path).Length) {
                throw "Candidate component does not match its inventory: $($file.Name)"
            }
            if (-not $file.ContainsKey("BuildProject") -and
                $record.sha256 -cne $file.Sha256) {
                throw "Candidate NVIDIA component does not match its upstream pin: $($file.Name)"
            }
        }
        $expectedSymbols = @("sl.common.pdb", "sl.interposer.pdb")
        $actualSymbols = @(Get-ChildItem -LiteralPath (
            Join-Path $CandidateDirectory "symbols") -Filter "*.pdb" -File |
            Select-Object -ExpandProperty Name | Sort-Object)
        if (Compare-Object $expectedSymbols $actualSymbols) {
            throw "Candidate symbol set does not match the release configuration."
        }
        foreach ($record in $inventory.symbols) {
            $path = Join-Path $CandidateDirectory "symbols\$($record.name)"
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                $record.sha256 -cne (Get-Sha256 $path) -or
                [uint64]$record.size -ne [uint64](Get-Item $path).Length) {
                throw "Candidate symbol does not match its inventory: $($record.name)"
            }
        }
        $expectedLicenses = @(
            $config.Licenses.OutputPath
            "3rd-party-licenses.md"
        ) | Sort-Object
        $actualLicenses = @(
            Get-ChildItem -LiteralPath $package -Recurse -File |
                Where-Object Extension -in ".txt",".md" |
                ForEach-Object {
                    [IO.Path]::GetRelativePath($package, $_.FullName).Replace("\","/")
                } | Sort-Object)
        if (Compare-Object $expectedLicenses $actualLicenses) {
            throw "Candidate license set does not match the release configuration."
        }
        foreach ($record in $inventory.licenses) {
            $path = Join-Path $package $record.path
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                $record.sha256 -cne (Get-Sha256 $path) -or
                [uint64]$record.size -ne [uint64](Get-Item $path).Length) {
                throw "Candidate license does not match its inventory: $($record.path)"
            }
        }
        $spec = Get-Content -Raw -LiteralPath (
            Join-Path $provenance "release-spec.json") | ConvertFrom-Json
        if ($spec.releaseId -cne $config.ReleaseId -or
            [uint32]$spec.keyId -ne [uint32]$config.KeyId) {
            throw "Candidate signing specification has the wrong release identity."
        }
        $expectedSpec = @($config.Runtime | ForEach-Object {
            "$($_.Name)|$($_.Role)"
        })
        $actualSpec = @($spec.files | ForEach-Object {
            "$($_.name)|$($_.role)"
        })
        if (Compare-Object $expectedSpec $actualSpec -CaseSensitive) {
            throw "Candidate signing specification does not match the release configuration."
        }
        $expectedHeader = Get-PublicHeader
        if ((Get-Content -Raw -LiteralPath (
            Join-Path $provenance "projectTrust.generated.h")) -cne $expectedHeader) {
            throw "Generated trust header does not match the public release configuration."
        }
    }
}
