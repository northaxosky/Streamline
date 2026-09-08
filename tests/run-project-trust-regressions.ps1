# Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$artifacts = Join-Path $root "_artifacts\tests\project-trust"
$fixtureRoot = Join-Path $artifacts "fixture"
$caseRoot = Join-Path $artifacts "cases"
$unmanifestedRoot = Join-Path $artifacts "unmanifested"
$missingRoot = Join-Path $artifacts "missing"
$tamperedSizeRoot = Join-Path $artifacts "tampered-size"
$tamperedHashRoot = Join-Path $artifacts "tampered-hash"
$keyRoot = Join-Path $artifacts "keys"
$qualificationRoot = Join-Path $root "_artifacts\runtime-qualification\develop-sdk"
$signingTool = Join-Path $root "tools\project-signing.ps1"
$physicalPathSource = Join-Path $root "source\core\sl.security\physicalFilePath.cpp"
$releaseId = "streamline-test-release-1"
$keyId = [uint32]117

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "cl.exe is unavailable. Run this script from a Visual Studio developer shell."
}
$physicalPathText = Get-Content -Raw $physicalPathSource
if (-not $physicalPathText.Contains("GetMappedFileNameW") -or
    $physicalPathText.Contains("GetFinalPathNameByHandleW")) {
    throw "Physical path binding must use mapped-file identity, not a rewritable final-path query."
}
$haveNvidiaFixture =
    (Test-Path (Join-Path $qualificationRoot "sl.interposer.dll") -PathType Leaf) -and
    (Test-Path (Join-Path $qualificationRoot "sl.common.dll") -PathType Leaf) -and
    (Test-Path (Join-Path $qualificationRoot "sl.dlss_g.dll") -PathType Leaf)

Remove-Item -LiteralPath $artifacts -Recurse -Force -ErrorAction SilentlyContinue
foreach ($directory in @(
    $fixtureRoot, $caseRoot, $unmanifestedRoot, $missingRoot,
    $tamperedSizeRoot, $tamperedHashRoot, $keyRoot)) {
    New-Item -ItemType Directory -Force $directory | Out-Null
}

$fixtureSource = Join-Path $PSScriptRoot "project-trust-fixture.cpp"
& $compiler.Source /nologo /std:c++20 /EHsc /LD /W4 /WX `
    $fixtureSource `
    "/Fe:$fixtureRoot\sl.interposer.dll" `
    "/Fo:$artifacts\fixture-interposer.obj" `
    "/link" "/pdb:$artifacts\fixture-interposer.pdb"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build sl.interposer.dll test fixture."
}
& $compiler.Source /nologo /std:c++20 /EHsc /LD /W4 /WX `
    /DPROJECT_TRUST_FIXTURE_VALUE=43 `
    $fixtureSource `
    "/Fe:$fixtureRoot\sl.common.dll" `
    "/Fo:$artifacts\fixture-common.obj" `
    "/link" "/pdb:$artifacts\fixture-common.pdb"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build sl.common.dll test fixture."
}

$privateKey = Join-Path $keyRoot "test-private.pem"
$publicKey = Join-Path $keyRoot "test-public.pem"
$trustHeader = Join-Path $keyRoot "projectTrustTestConfig.h"
& pwsh -NoProfile -File $signingTool -Mode GenerateKey `
    -KeyId $keyId -ReleaseId $releaseId `
    -PrivateKeyPath $privateKey -PublicKeyPath $publicKey `
    -PublicConfigPath $trustHeader
if ($LASTEXITCODE -ne 0) {
    throw "Failed to generate ephemeral project trust key."
}

$specPath = Join-Path $artifacts "valid-spec.json"
$specJson = @{
    releaseId = $releaseId
    keyId = $keyId
    files = @(
        @{ name = "sl.common.dll"; role = "ProjectCommon" }
        @{ name = "sl.interposer.dll"; role = "ProjectInterposer" }
    )
} | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText(
    $specPath, $specJson, [Text.UTF8Encoding]::new($false))

& pwsh -NoProfile -File $signingTool -Mode Sign `
    -SpecPath $specPath -InputDirectory $fixtureRoot `
    -PrivateKeyPath $privateKey `
    -ManifestPath (Join-Path $caseRoot "valid.bin") `
    -SignaturePath (Join-Path $caseRoot "valid.sig")
if ($LASTEXITCODE -ne 0) {
    throw "Failed to sign valid project fixture."
}
Copy-Item (Join-Path $caseRoot "valid.bin") `
    (Join-Path $fixtureRoot "sl.project-manifest.bin")
Copy-Item (Join-Path $caseRoot "valid.sig") `
    (Join-Path $fixtureRoot "sl.project-manifest.sig")

function Get-Sha256([string]$Path) {
    return [Security.Cryptography.SHA256]::HashData(
        [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($Path)))
}

function New-Entry(
    [string]$Name,
    [byte]$Role,
    [string]$Path,
    [Nullable[uint64]]$Size = $null,
    [byte[]]$Hash = $null) {
    $item = Get-Item -LiteralPath $Path
    return [pscustomobject]@{
        Name = $Name
        Role = $Role
        Size = if ($null -ne $Size) { [uint64]$Size } else { [uint64]$item.Length }
        Hash = if ($null -ne $Hash) { $Hash } else { Get-Sha256 $item.FullName }
    }
}

function Write-RawManifest(
    [string]$Path,
    [string]$Release,
    [uint32]$ManifestKeyId,
    [object[]]$Entries) {
    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($stream, [Text.Encoding]::UTF8, $true)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes("SLPMAN01"))
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]0)
        $writer.Write([uint32]$Entries.Count)
        $releaseBytes = [Text.Encoding]::ASCII.GetBytes($Release)
        $writer.Write([uint32]$releaseBytes.Length)
        $writer.Write([uint32]$ManifestKeyId)
        $writer.Write([uint32]0)
        $writer.Write($releaseBytes)
        foreach ($entry in $Entries) {
            $name = [Text.Encoding]::ASCII.GetBytes([string]$entry.Name)
            $writer.Write([uint16]$name.Length)
            $writer.Write([byte]$entry.Role)
            $writer.Write([byte]0)
            $writer.Write([uint64]$entry.Size)
            $writer.Write([byte[]]$entry.Hash)
            $writer.Write($name)
        }
        $writer.Flush()
        $bytes = $stream.ToArray()
        [BitConverter]::GetBytes([uint32]$bytes.Length).CopyTo($bytes, 12)
        [IO.File]::WriteAllBytes($Path, $bytes)
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Write-Signature(
    [string]$Manifest,
    [string]$Key,
    [string]$Signature) {
    $ecdsa = [Security.Cryptography.ECDsa]::Create()
    try {
        $ecdsa.ImportFromPem([IO.File]::ReadAllText($Key))
        $value = $ecdsa.SignData(
            [IO.File]::ReadAllBytes($Manifest),
            [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
        [IO.File]::WriteAllBytes($Signature, $value)
    }
    finally {
        $ecdsa.Dispose()
    }
}

function Write-Case(
    [string]$Name,
    [object[]]$Entries,
    [string]$Release = $releaseId,
    [uint32]$ManifestKeyId = $keyId,
    [string]$Key = $privateKey) {
    $manifest = Join-Path $caseRoot "$Name.bin"
    Write-RawManifest $manifest $Release $ManifestKeyId $Entries
    Write-Signature $manifest $Key (Join-Path $caseRoot "$Name.sig")
}

$interposer = Join-Path $fixtureRoot "sl.interposer.dll"
$common = Join-Path $fixtureRoot "sl.common.dll"
$standard = @(
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 1 $interposer
)

$wrongDigest = Get-Sha256 $interposer
$wrongDigest[0] = $wrongDigest[0] -bxor 0xff
Write-Case "wrong-digest" @(
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 1 $interposer -Hash $wrongDigest
)
Write-Case "wrong-size" @(
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 1 $interposer -Size ((Get-Item $interposer).Length + 1)
)
Write-Case "wrong-release" $standard "streamline-test-release-2"
Write-Case "wrong-key-id" $standard $releaseId 999
Write-Case "duplicate" @(
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.common.dll" 2 $common
)
Write-Case "case-collision" @(
    New-Entry "sl.Common.dll" 2 $common
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 1 $interposer
)
Write-Case "path-traversal" @(
    New-Entry "../evil.dll" 3 $interposer
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 1 $interposer
)
Write-Case "wrong-role" @(
    New-Entry "sl.common.dll" 2 $common
    New-Entry "sl.interposer.dll" 2 $interposer
)
Write-Case "wrong-basename" @(
    New-Entry "evil.dll" 1 $interposer
    New-Entry "sl.common.dll" 2 $common
)

$secondPrivate = Join-Path $keyRoot "wrong-private.pem"
& pwsh -NoProfile -File $signingTool -Mode GenerateKey `
    -KeyId 118 -ReleaseId $releaseId `
    -PrivateKeyPath $secondPrivate `
    -PublicKeyPath (Join-Path $keyRoot "wrong-public.pem") `
    -PublicConfigPath (Join-Path $keyRoot "wrong-trust.h")
Copy-Item (Join-Path $caseRoot "valid.bin") (Join-Path $caseRoot "wrong-key.bin")
Write-Signature (Join-Path $caseRoot "wrong-key.bin") $secondPrivate `
    (Join-Path $caseRoot "wrong-key.sig")
Copy-Item (Join-Path $caseRoot "valid.bin") (Join-Path $caseRoot "invalid-signature.bin")
[IO.File]::WriteAllBytes((Join-Path $caseRoot "invalid-signature.sig"), [byte[]]::new(64))

[IO.File]::WriteAllBytes((Join-Path $caseRoot "oversized.bin"), [byte[]]::new(65537))
[IO.File]::WriteAllBytes((Join-Path $caseRoot "oversized.sig"), [byte[]]::new(64))
$validBytes = [IO.File]::ReadAllBytes((Join-Path $caseRoot "valid.bin"))
[IO.File]::WriteAllBytes((Join-Path $caseRoot "truncated.bin"), $validBytes[0..19])
[IO.File]::WriteAllBytes((Join-Path $caseRoot "truncated.sig"), [byte[]]::new(64))
$trailing = [byte[]]::new($validBytes.Length + 1)
[Array]::Copy($validBytes, $trailing, $validBytes.Length)
$trailing[$trailing.Length - 1] = 0x5a
[IO.File]::WriteAllBytes((Join-Path $caseRoot "trailing.bin"), $trailing)
[IO.File]::WriteAllBytes((Join-Path $caseRoot "trailing.sig"), [byte[]]::new(64))
$unsupportedVersion = [byte[]]$validBytes.Clone()
$unsupportedVersion[8] = 2
[IO.File]::WriteAllBytes(
    (Join-Path $caseRoot "unsupported-version.bin"), $unsupportedVersion)
[IO.File]::WriteAllBytes(
    (Join-Path $caseRoot "unsupported-version.sig"), [byte[]]::new(64))

if ($haveNvidiaFixture) {
    Write-Case "mixed" @(
        New-Entry "sl.common.dll" 2 (Join-Path $qualificationRoot "sl.common.dll")
        New-Entry "sl.dlss_g.dll" 3 (Join-Path $qualificationRoot "sl.dlss_g.dll")
        New-Entry "sl.interposer.dll" 1 (Join-Path $qualificationRoot "sl.interposer.dll")
    )
}

foreach ($directory in @(
    $unmanifestedRoot, $missingRoot, $tamperedSizeRoot, $tamperedHashRoot)) {
    Copy-Item $interposer (Join-Path $directory "sl.interposer.dll")
}
foreach ($directory in @($tamperedSizeRoot, $tamperedHashRoot)) {
    Copy-Item $common (Join-Path $directory "sl.common.dll")
}
Copy-Item (Join-Path $caseRoot "valid.bin") `
    (Join-Path $missingRoot "sl.project-manifest.bin")
Copy-Item (Join-Path $caseRoot "valid.sig") `
    (Join-Path $missingRoot "sl.project-manifest.sig")
[IO.File]::AppendAllText(
    (Join-Path $tamperedSizeRoot "sl.interposer.dll"), "x",
    [Text.Encoding]::ASCII)
$sameSizeTamper = Join-Path $tamperedHashRoot "sl.interposer.dll"
$tamperedBytes = [IO.File]::ReadAllBytes($sameSizeTamper)
$tamperedBytes[$tamperedBytes.Length - 1] =
    $tamperedBytes[$tamperedBytes.Length - 1] -bxor 0x01
[IO.File]::WriteAllBytes($sameSizeTamper, $tamperedBytes)

$trustInclude = Split-Path -Parent $trustHeader
$testExe = Join-Path $artifacts "project-trust-regression.exe"
& $compiler.Source /nologo /std:c++20 /EHsc /W4 /WX /DNOMINMAX `
    /DSL_PRODUCTION /DSL_PROJECT_TRUST_TEST_CONFIG `
    "/I$root" "/I$trustInclude" `
    (Join-Path $PSScriptRoot "project-trust-regression.cpp") `
    $physicalPathSource `
    (Join-Path $root "source\core\sl.security\projectTrust.cpp") `
    (Join-Path $root "source\core\sl.security\secureLoadLibrary.cpp") `
    "/Fo:$artifacts\\" "/Fe:$testExe"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build project trust regression test."
}

$testArguments = @(
    $fixtureRoot, $caseRoot, $unmanifestedRoot, $missingRoot,
    $tamperedSizeRoot, $tamperedHashRoot)
if ($haveNvidiaFixture) {
    $testArguments += $qualificationRoot
}
else {
    Write-Warning "Signed NVIDIA fixture unavailable; skipping mixed and NVIDIA-only cases."
}
& $testExe @testArguments
if ($LASTEXITCODE -ne 0) {
    throw "Project trust regression test failed."
}
