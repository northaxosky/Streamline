# Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.
#
# Offline helper for the project-owned Streamline manifest format. This script
# never discovers, persists, or prints a private key except at the exact
# -PrivateKeyPath supplied by the operator.

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet("GenerateKey", "Sign")]
    [string]$Mode,

    [string]$SpecPath,
    [string]$InputDirectory,
    [string]$PrivateKeyPath,
    [string]$PublicKeyPath,
    [string]$PublicConfigPath,
    [string]$ManifestPath,
    [string]$SignaturePath,
    [uint32]$KeyId,
    [string]$ReleaseId
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$knownRoles = @{
    ProjectInterposer = @{ Value = 1; Names = @("sl.interposer.dll") }
    ProjectCommon     = @{ Value = 2; Names = @("sl.common.dll") }
    NvidiaModule     = @{
        Value = 3
        Names = @(
            "nvlowlatencyvk.dll",
            "nvngx_deepdvc.dll",
            "nvngx_dlss.dll",
            "nvngx_dlssd.dll",
            "nvngx_dlssg.dll",
            "sl.deepdvc.dll",
            "sl.directsr.dll",
            "sl.dlss.dll",
            "sl.dlss_d.dll",
            "sl.dlss_g.dll",
            "sl.imgui.dll",
            "sl.nis.dll",
            "sl.nvperf.dll",
            "sl.pcl.dll",
            "sl.reflex.dll"
        )
    }
}

function Assert-ReleaseId([string]$Value) {
    if (-not $Value -or $Value.Length -gt 64 -or
        $Value -cnotmatch '^[A-Za-z0-9._-]+$') {
        throw "ReleaseId must be 1-64 ASCII letters, digits, '.', '_', or '-'."
    }
}

function Assert-OutputDoesNotExist([string]$Path, [string]$Label) {
    if (-not $Path) {
        throw "$Label is required."
    }
    if (Test-Path -LiteralPath $Path) {
        throw "$Label already exists; refusing to overwrite it: $Path"
    }
    $parent = Split-Path -Parent $Path
    if ($parent) {
        [IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($parent)) | Out-Null
    }
}

function ConvertTo-HexInitializer([byte[]]$Bytes) {
    return (($Bytes | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", ")
}

if ($Mode -eq "GenerateKey") {
    if (-not $PrivateKeyPath -or -not $PublicKeyPath -or
        -not $PublicConfigPath -or $KeyId -eq 0) {
        throw "GenerateKey requires nonzero -KeyId, -ReleaseId, -PrivateKeyPath, -PublicKeyPath, and -PublicConfigPath."
    }
    Assert-ReleaseId $ReleaseId
    Assert-OutputDoesNotExist $PrivateKeyPath "PrivateKeyPath"
    Assert-OutputDoesNotExist $PublicKeyPath "PublicKeyPath"
    Assert-OutputDoesNotExist $PublicConfigPath "PublicConfigPath"

    $ecdsa = [Security.Cryptography.ECDsa]::Create(
        [Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try {
        $privatePem = $ecdsa.ExportPkcs8PrivateKeyPem()
        $publicPem = $ecdsa.ExportSubjectPublicKeyInfoPem()
        $parameters = $ecdsa.ExportParameters($false)
        if ($parameters.Q.X.Length -ne 32 -or $parameters.Q.Y.Length -ne 32) {
            throw "Generated key is not a P-256 key."
        }
        $xy = [byte[]]::new(64)
        [Array]::Copy($parameters.Q.X, 0, $xy, 0, 32)
        [Array]::Copy($parameters.Q.Y, 0, $xy, 32, 32)

        # Private material is written only to the explicit operator-selected
        # path and is never echoed.
        [IO.File]::WriteAllText(
            [IO.Path]::GetFullPath($PrivateKeyPath),
            $privatePem,
            [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(
            [IO.Path]::GetFullPath($PublicKeyPath),
            $publicPem,
            [Text.UTF8Encoding]::new($false))

        $config = @"
// Generated public project trust configuration. Contains no private material.
#pragma once
#define SL_PROJECT_TRUST_CONFIGURED 1
#define SL_PROJECT_TRUST_KEY_ID $($KeyId)u
#define SL_PROJECT_TRUST_RELEASE_ID "$ReleaseId"
#define SL_PROJECT_TRUST_PUBLIC_KEY_XY { $(ConvertTo-HexInitializer $xy) }
"@
        [IO.File]::WriteAllText(
            [IO.Path]::GetFullPath($PublicConfigPath),
            $config,
            [Text.UTF8Encoding]::new($false))
    }
    finally {
        $ecdsa.Dispose()
    }

    Write-Warning "The PKCS#8 private key must remain offline, access-controlled, and backed up. Do not add it to this repository."
    Write-Host "Generated one P-256 private key, SPKI public key, and public build configuration at the explicitly supplied paths."
    exit 0
}

if (-not $SpecPath -or -not $InputDirectory -or -not $PrivateKeyPath -or
    -not $ManifestPath -or -not $SignaturePath) {
    throw "Sign requires -SpecPath, -InputDirectory, -PrivateKeyPath, -ManifestPath, and -SignaturePath."
}
if (-not (Test-Path -LiteralPath $PrivateKeyPath -PathType Leaf)) {
    throw "PrivateKeyPath does not name a file."
}
if (-not (Test-Path -LiteralPath $InputDirectory -PathType Container)) {
    throw "InputDirectory does not name a directory."
}
Assert-OutputDoesNotExist $ManifestPath "ManifestPath"
Assert-OutputDoesNotExist $SignaturePath "SignaturePath"

$spec = Get-Content -LiteralPath $SpecPath -Raw | ConvertFrom-Json
$specKeyId = [uint32]$spec.keyId
$specReleaseId = [string]$spec.releaseId
Assert-ReleaseId $specReleaseId
if ($specKeyId -eq 0) {
    throw "The manifest keyId must be nonzero."
}

$entries = @($spec.files)
if ($entries.Count -lt 1 -or $entries.Count -gt 32) {
    throw "The manifest must contain 1-32 files."
}
$normalized = @()
foreach ($entry in $entries) {
    $name = [string]$entry.name
    $roleName = [string]$entry.role
    if (-not $knownRoles.ContainsKey($roleName)) {
        throw "Unknown role '$roleName'."
    }
    if ($name.Length -lt 5 -or $name.Length -gt 64 -or
        $name -cnotmatch '^[a-z0-9_.]+\.dll$' -or $name.Contains("..") -or
        $knownRoles[$roleName].Names -cnotcontains $name) {
        throw "Role '$roleName' cannot authorize basename '$name'."
    }
    $source = Join-Path $InputDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Input file is missing: $source"
    }
    $item = Get-Item -LiteralPath $source
    $hash = [Security.Cryptography.SHA256]::HashData(
        [IO.File]::ReadAllBytes($item.FullName))
    $normalized += [pscustomobject]@{
        Name = $name
        Role = [byte]$knownRoles[$roleName].Value
        Size = [uint64]$item.Length
        Hash = $hash
    }
}
$byName = @{}
foreach ($entry in $normalized) {
    if ($byName.ContainsKey($entry.Name)) {
        throw "Duplicate manifest basename '$($entry.Name)'."
    }
    $byName[$entry.Name] = $entry
}
$names = [string[]]@($normalized | ForEach-Object { $_.Name })
[Array]::Sort($names, [StringComparer]::Ordinal)
$normalized = @($names | ForEach-Object { $byName[$_] })
if ($normalized.Name -cnotcontains "sl.interposer.dll" -or
    $normalized.Name -cnotcontains "sl.common.dll") {
    throw "A project manifest must contain both sl.interposer.dll and sl.common.dll."
}

$stream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($stream, [Text.Encoding]::UTF8, $true)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes("SLPMAN01"))
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]0) # Patched after serialization.
    $writer.Write([uint32]$normalized.Count)
    $releaseBytes = [Text.Encoding]::ASCII.GetBytes($specReleaseId)
    $writer.Write([uint32]$releaseBytes.Length)
    $writer.Write([uint32]$specKeyId)
    $writer.Write([uint32]0)
    $writer.Write($releaseBytes)
    foreach ($entry in $normalized) {
        $nameBytes = [Text.Encoding]::ASCII.GetBytes($entry.Name)
        $writer.Write([uint16]$nameBytes.Length)
        $writer.Write([byte]$entry.Role)
        $writer.Write([byte]0)
        $writer.Write([uint64]$entry.Size)
        $writer.Write([byte[]]$entry.Hash)
        $writer.Write($nameBytes)
    }
    $writer.Flush()
    $manifest = $stream.ToArray()
    [BitConverter]::GetBytes([uint32]$manifest.Length).CopyTo($manifest, 12)
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

$ecdsa = [Security.Cryptography.ECDsa]::Create()
try {
    $privatePem = [IO.File]::ReadAllText(
        [IO.Path]::GetFullPath($PrivateKeyPath))
    $ecdsa.ImportFromPem($privatePem)
    if ($ecdsa.KeySize -ne 256) {
        throw "PrivateKeyPath must contain an ECDSA P-256 PKCS#8 PEM key."
    }
    $signature = $ecdsa.SignData(
        $manifest,
        [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signature.Length -ne 64) {
        throw "Unexpected ECDSA signature encoding."
    }
}
finally {
    $ecdsa.Dispose()
}

[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($ManifestPath), $manifest)
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($SignaturePath), $signature)
Write-Host "Wrote deterministic manifest bytes and a 64-byte IEEE-P1363 ECDSA P-256/SHA-256 signature."
