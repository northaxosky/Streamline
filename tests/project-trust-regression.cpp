#include "source/core/sl.security/projectTrust.h"
#include "source/core/sl.security/secureLoadLibrary.h"

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
};

void attemptReplacement(void* context, std::wstring_view physicalPath)
{
    auto& attempt = *static_cast<ReplacementAttempt*>(context);
    const std::wstring path(physicalPath);
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
    expectFailure(fixtureRoot, cases, "wrong-release", TrustFailure::eManifestReleaseMismatch);
    expectFailure(fixtureRoot, cases, "wrong-key-id", TrustFailure::eManifestUnknownKey);
    expectFailure(
        fixtureRoot, cases, "valid", TrustFailure::eRequestedFileNotApproved,
        L"sl.reflex.dll");

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
