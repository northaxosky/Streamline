#include "source/core/sl.security/physicalFilePath.h"
#include "source/core/sl.security/projectTrust.h"
#include "source/core/sl.security/secureLoadLibrary.h"

#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sl::security;

namespace
{

int failures{};

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Resolver
{
    fs::path root;
};

bool resolveFromRoot(
    void* context,
    std::wstring_view basename,
    std::wstring& path)
{
    const auto& resolver = *static_cast<Resolver*>(context);
    path = (resolver.root / std::wstring(basename)).wstring();
    return true;
}

ProjectLoadResult run(
    const fs::path& root,
    const fs::path& manifest,
    const fs::path& signature,
    std::wstring_view requested,
    const ProjectTrustConfig& trust,
    bool authenticateOnly = true,
    BeforeProjectLoad beforeLoad = nullptr,
    void* beforeLoadContext = nullptr)
{
    Resolver resolver{ root };
    const std::wstring manifestString = manifest.wstring();
    const std::wstring signatureString = signature.wstring();
    ProjectLoadOptions options
    {
        requested,
        manifestString,
        signatureString,
        resolveFromRoot,
        &resolver,
        &trust,
        authenticateOnly,
        beforeLoad,
        beforeLoadContext
    };
    return authenticateAndLoadProjectLibrary(options);
}

void expectFailure(
    const fs::path& root,
    const fs::path& cases,
    const char* caseName,
    TrustFailure expected,
    std::wstring_view requested = L"sl.interposer.dll")
{
    const fs::path base = cases / caseName;
    ProjectLoadResult result = run(
        root, base.string() + ".bin", base.string() + ".sig",
        requested, getCompiledProjectTrust());
    if (result.failure != expected)
    {
        std::cerr << "FAIL: " << caseName << " expected "
            << getTrustFailureMessage(expected) << ", got "
            << getTrustFailureMessage(result.failure) << '\n';
        ++failures;
    }
}

struct ReplacementAttempt
{
    bool writeBlocked{};
    bool renameBlocked{};
    fs::path physicalPath;
};

void attemptReplacement(void* context, std::wstring_view physicalPath)
{
    auto& attempt = *static_cast<ReplacementAttempt*>(context);
    const std::wstring path(physicalPath);
    attempt.physicalPath = path;
    HANDLE write = CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    attempt.writeBlocked = write == INVALID_HANDLE_VALUE;
    if (write != INVALID_HANDLE_VALUE)
    {
        CloseHandle(write);
    }

    const std::wstring replacement = path + L".replacement-attempt";
    DeleteFileW(replacement.c_str());
    attempt.renameBlocked = !MoveFileExW(
        path.c_str(), replacement.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!attempt.renameBlocked)
    {
        MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

void checkDosAliasBinding(const fs::path& fixture)
{
    const fs::path absolute = fs::absolute(fixture);
    const std::wstring sourceDrive = absolute.root_name().wstring();
    if (sourceDrive.size() != 2 || sourceDrive[1] != L':')
    {
        return;
    }

    wchar_t aliasLetter{};
    for (const wchar_t candidate : { L'B', L'A' })
    {
        const std::wstring aliasRoot{ candidate, L':', L'\\' };
        if (candidate != sourceDrive[0] &&
            GetDriveTypeW(aliasRoot.c_str()) == DRIVE_NO_ROOT_DIR)
        {
            aliasLetter = candidate;
            break;
        }
    }
    if (!aliasLetter)
    {
        return;
    }

    std::wstring device(32768, L'\0');
    if (!QueryDosDeviceW(
        sourceDrive.c_str(), device.data(),
        static_cast<DWORD>(device.size())))
    {
        check(false, "source volume device name is available for alias test");
        return;
    }
    device.resize(std::wcslen(device.c_str()));
    const std::wstring target =
        device + absolute.parent_path().wstring().substr(2);
    const std::wstring alias{ aliasLetter, L':' };
    constexpr DWORD defineFlags =
        DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM;
    if (!DefineDosDeviceW(defineFlags, alias.c_str(), target.c_str()))
    {
        check(false, "hostile DOS alias can be installed for binding test");
        return;
    }

    HANDLE raw = CreateFileW(
        absolute.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring identityPath;
    std::wstring loadPath;
    HANDLE boundLoadHandle = INVALID_HANDLE_VALUE;
    DWORD systemError{};
    const bool resolved = raw != INVALID_HANDLE_VALUE &&
        getPhysicalFilePaths(
            raw, identityPath, loadPath,
            boundLoadHandle, systemError);
    if (boundLoadHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(boundLoadHandle);
    }
    if (raw != INVALID_HANDLE_VALUE)
    {
        CloseHandle(raw);
    }
    const DWORD removeFlags = defineFlags |
        DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE;
    check(
        DefineDosDeviceW(
            removeFlags, alias.c_str(), target.c_str()) != FALSE,
        "hostile DOS alias is removed after binding test");

    check(resolved, "physical path resolves while a hostile DOS alias exists");
    check(
        loadPath.size() < 2 ||
        towupper(loadPath[0]) != aliasLetter ||
        loadPath[1] != L':',
        "physical path does not use an attacker-controlled DOS alias");
    std::error_code equivalentError;
    check(
        resolved && fs::equivalent(loadPath, absolute, equivalentError) &&
        !equivalentError,
        "alias-resistant physical path retains the authenticated file identity");
}

void checkWinePathControls()
{
    std::wstring loadPath;
    check(
        detail::getWineMappedLoadPathForTests(
            L"\\??\\C:\\Games\\Fallout 4\\sl.common.dll",
            loadPath) &&
        loadPath == L"\\??\\C:\\Games\\Fallout 4\\sl.common.dll",
        "Wine mapped drive path is preserved exactly for loading");
    check(
        detail::getWineMappedLoadPathForTests(
            L"\\??\\z:\\home\\user\\game\\sl.common.dll",
            loadPath),
        "Wine mapped lowercase drive path is accepted");

    for (const std::wstring_view invalid : {
        L"\\Device\\HarddiskVolume1\\game\\sl.common.dll",
        L"\\??\\C:game\\sl.common.dll",
        L"\\??\\UNC\\server\\share\\sl.common.dll",
        L"\\??\\unix\\home\\user\\sl.common.dll",
        L"\\??\\GlobalRoot\\Device\\HarddiskVolume1\\sl.common.dll",
        L"\\??\\C:\\game\\..\\sl.common.dll",
        L"\\??\\C:\\game\\\\sl.common.dll",
        L"\\??\\C:\\game/sl.common.dll",
        L"\\??\\C:\\game\\sl.common.dll:stream",
        L"\\??\\C:\\game\\sl.*.dll",
        L"\\??\\C:\\game\\" })
    {
        check(
            !detail::getWineMappedLoadPathForTests(
                invalid, loadPath),
            "unsupported Wine mapped path fails closed");
    }

    FILE_ID_INFO expected{};
    expected.VolumeSerialNumber = 17;
    expected.FileId.Identifier[0] = 42;
    FILE_ID_INFO candidate = expected;
    check(
        detail::fileIdsMatchForTests(
            expected, candidate, true),
        "Wine identity accepts an exact volume and file ID match");

    candidate.VolumeSerialNumber = 18;
    check(
        !detail::fileIdsMatchForTests(
            expected, candidate, true),
        "Wine identity rejects a cross-volume file ID collision");
    candidate = expected;
    candidate.FileId.Identifier[0] = 43;
    check(
        !detail::fileIdsMatchForTests(
            expected, candidate, true),
        "Wine identity rejects a different file ID");
    expected.VolumeSerialNumber = 0;
    candidate = expected;
    check(
        detail::fileIdsMatchForTests(
            expected, candidate, false),
        "native identity comparison preserves existing zero-volume behavior");
    check(
        !detail::fileIdsMatchForTests(
            expected, candidate, true),
        "Wine identity fails closed without volume scope");
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 7 && argc != 8)
    {
        std::cerr << "usage: project-trust-regression <fixture-root> "
            "<case-root> <unmanifested-root> <missing-root> "
            "<tampered-size-root> <tampered-hash-root> "
            "[qualification-root]\n";
        return 2;
    }

    const fs::path fixtureRoot = argv[1];
    const fs::path cases = argv[2];
    const fs::path unmanifestedRoot = argv[3];
    const fs::path missingRoot = argv[4];
    const fs::path tamperedSizeRoot = argv[5];
    const fs::path tamperedHashRoot = argv[6];
    const fs::path qualificationRoot = argc == 8 ? argv[7] : L"";
    const auto& trust = getCompiledProjectTrust();
    check(trust.configured, "ephemeral test trust is compiled into the test executable");
    checkDosAliasBinding(fixtureRoot / "sl.interposer.dll");
    checkWinePathControls();

    const fs::path validManifest = cases / "valid.bin";
    const fs::path validSignature = cases / "valid.sig";
    TrustFailure wrapperFailure{};
    HMODULE routed = loadLibrary(
        (fixtureRoot / "sl.interposer.dll").c_str(), &wrapperFailure);
    check(
        routed && wrapperFailure == TrustFailure::eOk,
        "Production compatibility loader routes a valid community manifest through the gate");
    if (routed)
    {
        FreeLibrary(routed);
    }

    ProjectLoadResult valid = run(
        fixtureRoot, validManifest, validSignature,
        L"sl.interposer.dll", trust, false);
    check(static_cast<bool>(valid), "valid signed custom fixture loads");
    if (valid.module)
    {
        using FixtureFunction = int(*)();
        auto function = reinterpret_cast<FixtureFunction>(
            GetProcAddress(valid.module, "projectTrustFixture"));
        check(function && function() == 42, "loaded fixture is the authenticated DLL");
        FreeLibrary(valid.module);
    }

    ReplacementAttempt replacement;
    ProjectLoadResult held = run(
        fixtureRoot, validManifest, validSignature,
        L"sl.interposer.dll", trust, false,
        attemptReplacement, &replacement);
    check(static_cast<bool>(held), "DLL loads while its authenticated handle is held");
    check(replacement.writeBlocked, "held handle blocks write replacement");
    check(replacement.renameBlocked, "held handle blocks rename/delete replacement");
    std::error_code equivalentError;
    check(
        fs::equivalent(
            replacement.physicalPath,
            fixtureRoot / "sl.interposer.dll",
            equivalentError) &&
        !equivalentError,
        "load path names the backing file mapped from the authenticated handle");
    if (held.module)
    {
        FreeLibrary(held.module);
    }

    ProjectTrustConfig unconfigured{};
    ProjectLoadResult noTrust = run(
        fixtureRoot, validManifest, validSignature,
        L"sl.interposer.dll", unconfigured);
    check(
        noTrust.failure == TrustFailure::eProjectTrustNotConfigured,
        "present manifest fails closed when project trust is unconfigured");

    expectFailure(fixtureRoot, cases, "wrong-key", TrustFailure::eSignatureInvalid);
    expectFailure(fixtureRoot, cases, "invalid-signature", TrustFailure::eSignatureInvalid);
    expectFailure(fixtureRoot, cases, "oversized", TrustFailure::eManifestTooLarge);
    expectFailure(fixtureRoot, cases, "truncated", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "trailing", TrustFailure::eManifestMalformed);
    expectFailure(
        fixtureRoot, cases, "unsupported-version",
        TrustFailure::eManifestVersionUnsupported);
    expectFailure(fixtureRoot, cases, "duplicate", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "case-collision", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "path-traversal", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "wrong-role", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "wrong-basename", TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "wrong-digest", TrustFailure::eFileHashMismatch);
    expectFailure(fixtureRoot, cases, "wrong-size", TrustFailure::eFileSizeMismatch);
    expectFailure(
        fixtureRoot, cases, "unsigned-ngx",
        TrustFailure::eNvidiaSignatureInvalid);
    expectFailure(
        fixtureRoot.parent_path() / "unsigned-amd", cases, "unsigned-amd",
        TrustFailure::eAmdSignatureInvalid,
        L"amd_fidelityfx_loader_dx12.dll");
    expectFailure(
        fixtureRoot, cases, "wrong-project-plugin-role",
        TrustFailure::eManifestMalformed);
    expectFailure(
        fixtureRoot, cases, "wrong-project-vendor-role",
        TrustFailure::eManifestMalformed);
    expectFailure(
        fixtureRoot, cases, "wrong-project-vendor-basename",
        TrustFailure::eManifestMalformed);
    expectFailure(
        fixtureRoot, cases, "wrong-amd-role",
        TrustFailure::eManifestMalformed);
    expectFailure(
        fixtureRoot, cases, "wrong-amd-basename",
        TrustFailure::eManifestMalformed);
    expectFailure(fixtureRoot, cases, "wrong-release", TrustFailure::eManifestReleaseMismatch);
    expectFailure(fixtureRoot, cases, "wrong-key-id", TrustFailure::eManifestUnknownKey);
    expectFailure(
        fixtureRoot, cases, "valid", TrustFailure::eRequestedFileNotApproved,
        L"sl.reflex.dll");

    ProjectLoadResult projectPlugin = run(
        fixtureRoot, cases / "project-plugin.bin",
        cases / "project-plugin.sig", L"sl.fsr.dll", trust);
    check(
        static_cast<bool>(projectPlugin),
        "manifest-authorized project plugin authenticates");

    ProjectLoadResult projectVendor = run(
        fixtureRoot, cases / "project-vendor.bin",
        cases / "project-vendor.sig",
        L"amd_fidelityfx_framegeneration_dx12.dll", trust);
    check(
        static_cast<bool>(projectVendor),
        "manifest-authorized frame-generation provider authenticates without vendor Authenticode");

    ProjectLoadResult projectUpscaler = run(
        fixtureRoot, cases / "project-vendor.bin",
        cases / "project-vendor.sig",
        L"amd_fidelityfx_upscaler_dx12.dll", trust);
    check(
        static_cast<bool>(projectUpscaler),
        "manifest-authorized upscaler provider authenticates without vendor Authenticode");

    ProjectLoadResult amdModules = run(
        fixtureRoot, cases / "amd-modules.bin",
        cases / "amd-modules.sig",
        L"amd_fidelityfx_loader_dx12.dll", trust);
    check(
        static_cast<bool>(amdModules),
        "pinned signed AMD FidelityFX modules authenticate");

    ProjectLoadResult incomplete = run(
        fixtureRoot, validManifest, cases / "does-not-exist.sig",
        L"sl.interposer.dll", trust);
    check(
        incomplete.failure == TrustFailure::eManifestIncomplete,
        "one missing detached file fails closed");

    ProjectLoadResult missing = run(
        missingRoot, validManifest, validSignature,
        L"sl.interposer.dll", trust);
    check(
        missing.failure == TrustFailure::eFileOpenFailed,
        "missing manifest member fails closed");
    HMODULE missingRouted = loadLibrary(
        (missingRoot / "sl.interposer.dll").c_str(), &wrapperFailure);
    check(
        !missingRouted && wrapperFailure == TrustFailure::eFileOpenFailed,
        "present incomplete package never falls through to NVIDIA-only loading");
    if (missingRouted)
    {
        FreeLibrary(missingRouted);
    }

    ProjectLoadResult tamperedSize = run(
        tamperedSizeRoot, validManifest, validSignature,
        L"sl.interposer.dll", trust);
    check(
        tamperedSize.failure == TrustFailure::eFileSizeMismatch,
        "size-changing file tampering is rejected");

    ProjectLoadResult tamperedHash = run(
        tamperedHashRoot, validManifest, validSignature,
        L"sl.interposer.dll", trust);
    check(
        tamperedHash.failure == TrustFailure::eFileHashMismatch,
        "same-size file tampering is rejected");

    if (!qualificationRoot.empty())
    {
        ProjectLoadResult mixed = run(
            qualificationRoot, cases / "mixed.bin", cases / "mixed.sig",
            L"sl.interposer.dll", trust);
        check(static_cast<bool>(mixed), "mixed project and NVIDIA package authenticates");
    }

    HMODULE unsignedModule = loadLibrary(
        (unmanifestedRoot / "sl.interposer.dll").c_str(), &wrapperFailure);
    check(
        !unsignedModule &&
        wrapperFailure == TrustFailure::eNvidiaSignatureInvalid,
        "unsigned unmanifested custom DLL is rejected in Production");
    if (unsignedModule)
    {
        FreeLibrary(unsignedModule);
    }

    if (!qualificationRoot.empty())
    {
        HMODULE nvidiaModule = loadLibrary(
            (qualificationRoot / "sl.dlss_g.dll").c_str(), &wrapperFailure);
        if (!nvidiaModule)
        {
            const ProjectLoadResult diagnostic =
                authenticateAndLoadNvidiaLibrary(
                    (qualificationRoot / "sl.dlss_g.dll").wstring());
            std::cerr << "NVIDIA-only load failed with trust result "
                << getTrustFailureMessage(wrapperFailure)
                << " and system error " << diagnostic.systemError << '\n';
            if (diagnostic.module)
            {
                FreeLibrary(diagnostic.module);
            }
        }
        check(
            nvidiaModule && wrapperFailure == TrustFailure::eOk,
            "available original NVIDIA DLL is accepted without a project manifest");
        if (nvidiaModule)
        {
            FreeLibrary(nvidiaModule);
        }
    }

    if (failures)
    {
        std::cerr << failures << " project trust regression(s) failed\n";
        return 1;
    }
    std::cout << "Project trust regressions passed.\n";
    return 0;
}
