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
policy in addition to their signed manifest size and digest. Streamline plugins
retain the nested Streamline-key check. NGX binaries, which do not carry that
nested signature, must pass Windows' strong primary Authenticode policy and the
pinned NVIDIA NGX signer public-key check while their exact bytes remain
authorized by the project-signed manifest. There is no wildcard role, and any
other `sl.*.dll` beside a community manifest is refused.

The verifier calls its physical resolver independently for every logical
basename. A future game adapter should resolve each winner through its own
virtual-filesystem/USVFS-aware helper. The SDK default resolver uses ordinary
full paths in the requested DLL's directory. Every target is opened with read sharing only, hashed through that held handle,
and kept open through `LoadLibraryExW`. The same held-handle rule applies to the
manifest-free NVIDIA-only fallback. Windows has no load-from-handle API;
retaining restrictive handles closes the writable/delete replacement window
while the loader reopens the verified physical backing file. Its native backing
identity is derived from a read-only mapping of the same retained handle with
`GetMappedFileNameW`, avoiding final-path queries that virtual-file systems can
rewrite back to a logical alias. Local authentication uses the mount manager's
volume-GUID path; loading uses a mount-manager path that is independently
reopened, file-ID matched, and retained through `LoadLibraryExW`. UNC paths are
likewise reopened and identity matched. Per-session DOS aliases are never used
to select the load path, and unsupported device identities fail closed.

Wine and Proton report mapped files as their original NT-DOS
`\??\X:\...` names rather than Windows device paths. When Wine is positively
identified through its `ntdll` export, the verifier accepts only a canonical
absolute drive form, reopens that exact loader-compatible name, requires a
nonzero volume identity and matching file ID, and retains both restrictive
handles through the load. This provides package authentication, checked file
identity, and direct target-file locking inside a trusted Wine prefix. It does
not provide end-to-end namespace binding against another same-user Wine or
native process that concurrently rewrites drive mappings, ancestor paths, or
the prefix filesystem. Unsupported Wine namespaces and identities fail closed
without changing the stronger native Windows path policy. Actual
`WinVerifyTrust`, `LoadLibraryExW`, and USVFS/MO2 behavior under Proton remains a
runtime qualification requirement. The initial bounded
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

## Wine qualification

Run `tests/run-regressions.ps1` from a Visual Studio developer shell to create
the ignored `_artifacts/tests/project-trust` fixture bundle. From that directory
a Wine tester can exercise the real combined authentication and DLL-load path:

```text
wine project-trust-regression.exe fixture cases unmanifested missing tampered-size tampered-hash
```

Record the Wine/Proton version, whether MO2/USVFS is active, GPU, runtime folder,
test output, and Streamline authentication logs. A passing fixture demonstrates
the verifier and loader gate only; DLSS and frame-generation behavior require
separate game-level testing. MO2 qualification must also confirm that the
selected logical winner reaches the expected authenticated backing file.

## Release automation

`config/project-release.psd1` is the single public source for the release ID,
key ID, public key, upstream archive, and exact NVIDIA binary pins.
`tools/project-release.ps1 -Mode GeneratePublicHeader` produces
`_artifacts/project-release/include/projectTrust.generated.h` for SDK and
consumer builds. `.github/workflows/project-release.yml` accepts only the exact
configured tag on trusted `main`: a secretless job builds, tests, and assembles
the candidate before the protected `streamline-release` environment uses the
single base64-encoded PKCS#8 PEM secret `STREAMLINE_SIGNING_KEY`. The key exists
only in a restricted temporary file during the signing step and is deleted
before verification and publication.

## Security scope

This provides package authenticity and tamper resistance only while the trusted
game/host and verifier binaries remain trusted and the private key remains
protected. It is not DRM and cannot defend against replacement of the entire
trusted host/verifier. Version 1 binds an exact release ID but has no
anti-rollback guarantee without a separately trusted release floor. The initial
policy is intentionally offline and has no updater, revocation, or network
dependency.
