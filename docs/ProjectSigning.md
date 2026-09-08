# Project-owned Streamline package signing

This repository contains a private, source-level Windows trust gate in
`source/core/sl.security/projectTrust.h`. It lets a separately built host use
the same code to authenticate `sl.interposer.dll` before any DLL code runs.
The current SDK plugin loader uses the gate in `Production` builds.

## Trust and role policy

The detached files are named `sl.project-manifest.bin` and
`sl.project-manifest.sig`. The signature is ECDSA P-256 over the exact manifest
bytes using SHA-256. Its encoding is always 64-byte IEEE-P1363 `r || s`.
Verification uses Windows CNG (`BCryptVerifySignature`); no cryptographic
primitive is implemented here.

The public key is compiled configuration. It is never loaded beside the
manifest. The checked-in configuration deliberately has project trust disabled.
When both detached files are absent, `Production` retains the original
NVIDIA-only embedded-signature path. If either community file is present, any
missing, malformed, unknown-key, wrong-release, invalid-signature, incomplete,
or mismatched package fails closed.

The project signature can authorize only:

* `sl.interposer.dll` with role `ProjectInterposer`
* `sl.common.dll` with role `ProjectCommon`

Both entries are required. The `NvidiaModule` role accepts only the shipped
`nvlowlatencyvk`, `nvngx_deepdvc`, `nvngx_dlss`, `nvngx_dlssd`,
`nvngx_dlssg`, `sl.deepdvc`, `sl.directsr`, `sl.dlss`, `sl.dlss_d`,
`sl.dlss_g`, `sl.imgui`, `sl.nis`, `sl.nvperf`, `sl.pcl`, and `sl.reflex`
DLL basenames. These files must pass the existing NVIDIA embedded-signature
verifier in addition to their signed manifest size and digest. There is no
wildcard role, and any other `sl.*.dll` beside a community manifest is refused.

The verifier calls its physical resolver independently for every logical
basename. A future game adapter should resolve each winner through its own
virtual-filesystem/USVFS-aware helper. The SDK default resolver uses ordinary
full paths in the requested DLL's directory. Every target is opened with read sharing only, hashed through that held handle,
and kept open through `LoadLibraryExW`. The same held-handle rule applies to the
manifest-free NVIDIA-only fallback. Windows has no load-from-handle API;
retaining restrictive handles closes the writable/delete replacement window
while the loader reopens the verified full physical path. The initial bounded
implementation revalidates the selected package for each plugin load rather
than retaining a process-lifetime cache; this keeps resolver and file-identity
semantics simple at the cost of additional startup-only I/O.

## Binary manifest version 1

All integers are fixed little-endian. Bounds are enforced before allocation:
64 KiB total, 32 entries, 64-byte release ID, and 64-byte basename.

| Field | Size |
| --- | ---: |
| Magic `SLPMAN01` | 8 |
| Format version (`1`) | 2 |
| Header size (`32`) | 2 |
| Exact total byte size | 4 |
| Entry count | 4 |
| Release ID byte length | 4 |
| Public-key ID | 4 |
| Reserved zero | 4 |
| ASCII release ID (`A-Z a-z 0-9 . _ -`) | variable |

Each sorted entry is:

| Field | Size |
| --- | ---: |
| Basename byte length | 2 |
| Role (`1`, `2`, or `3`) | 1 |
| Reserved zero | 1 |
| File size | 8 |
| SHA-256 digest | 32 |
| Lowercase ASCII DLL basename | variable |

Names must be strictly sorted and unique, lowercase, and contain no path
separator, drive marker, `..`, absolute path, or case ambiguity. Unknown
versions, roles, key IDs, trailing bytes, truncation, and overflow are rejected.
The compiled trust policy also requires one exact release ID.

## Offline key ceremony and signing

Use PowerShell 7/.NET offline. Never put the release private key in this
repository or under a game/SDK directory. Keep it access-controlled, offline,
and backed up. One-time key generation (choose external protected paths):

```powershell
pwsh ./tools/project-signing.ps1 -Mode GenerateKey `
  -KeyId 1 -ReleaseId sdk-2.12.0-project-1 `
  -PrivateKeyPath X:\offline\streamline-release-private.pk8.pem `
  -PublicKeyPath .\_artifacts\signing\streamline-release-public.spki.pem `
  -PublicConfigPath .\_artifacts\signing\projectTrust.generated.h
```

This exports an ECDSA P-256 PKCS#8 private PEM, an SPKI public PEM, and a
public-only build configuration. Supply that reviewed configuration at compile
time as a quoted include path through `SL_PROJECT_TRUST_CONFIG_HEADER`. Do not
enable it until the release public key, key ID, and release ID have been
reviewed. No placeholder or test key is trusted by default.

Create an explicit JSON spec:

```json
{
  "releaseId": "sdk-2.12.0-project-1",
  "keyId": 1,
  "files": [
    { "name": "sl.common.dll", "role": "ProjectCommon" },
    { "name": "sl.interposer.dll", "role": "ProjectInterposer" },
    { "name": "sl.dlss_g.dll", "role": "NvidiaModule" }
  ]
}
```

Sign exact files from one explicit input directory:

```powershell
pwsh ./tools/project-signing.ps1 -Mode Sign `
  -SpecPath X:\offline\release-spec.json `
  -InputDirectory X:\offline\staged-sdk `
  -PrivateKeyPath X:\offline\streamline-release-private.pk8.pem `
  -ManifestPath X:\offline\out\sl.project-manifest.bin `
  -SignaturePath X:\offline\out\sl.project-manifest.sig
```

The tool does not discover, store, or log private keys. It refuses to overwrite
outputs and hashes only the exact allowlisted basenames in the explicit input
directory.

## Security scope

This provides package authenticity and tamper resistance only while the trusted
game/host and verifier binaries remain trusted and the private key remains
protected. It is not DRM and cannot defend against replacement of the entire
trusted host/verifier. Version 1 binds an exact release ID but has no
anti-rollback guarantee without a separately trusted release floor. The initial
policy is intentionally offline and has no updater, revocation, or network
dependency.
