#include "winui/widget_package_review.h"
#include "test_temporary_directory.h"

#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct HeldHandle
{
    HANDLE value;
    ~HeldHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void RunReviewedPackageInstallationCases()
{
    using namespace snowdesktop::widget;
    using namespace snowdesktop::winui::widgets_page_backend_detail;
    using Status = ReviewedPackageInstallStatus;
    snowdesktop::test::TemporaryDirectory temporary;
    PackagePaths paths;
    paths.builtin = temporary.path / L"builtin";
    paths.installed = temporary.path / L"installed";
    paths.development = temporary.path / L"dev";
    paths.staging = temporary.path / L"staging";
    paths.quarantine = temporary.path / L"quarantine";
    paths.migrations = temporary.path / L"migrations";
    paths.registry = temporary.path / L"packages.json";
    WidgetPackageManager manager(paths);
    std::string error;
    Require(manager.Initialize(error), "review fixture package manager initializes");
    const auto source = temporary.path / L"source";
    std::filesystem::create_directory(source);
    const auto write = [](const auto& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        Require(static_cast<bool>(file), "review fixture writes its own file");
    };
    write(source / L"widget.json", R"json({
        "schemaVersion":2,"apiVersion":2,"dataVersion":1,
        "id":"1c60540b-d29b-4938-916c-e998f830de40","slug":"review-test",
        "name":"Review Test","version":"1.0.0","entry":"main.lua",
        "minHostVersion":"1.0.7.0","author":"Test","license":"MIT",
        "description":"Lock recovery fixture","defaultSize":{"columns":1,"rows":1},
        "permissions":[],"optionalPermissions":[],"requiredFeatures":[],"optionalFeatures":[]
    })json");
    const std::string lua = "return widget.define({model = {installed = true}})";
    write(source / L"main.lua", lua);
    const auto archive = paths.staging / L"review.snowwidget";
    PackageArtifact artifact;
    ValidationReport report;
    Require(manager.ExportDirectory(source, archive, artifact, report, error),
        "review fixture exports a real package");
    PackageManifest manifest;
    Require(manager.ValidateArchive(archive, &manifest).Ok(), "review fixture validates the real archive");
    const auto hash = WidgetPackageManager::Sha256File(archive);
    Require(!hash.empty(), "review fixture can read the package bytes");
    std::optional<ReviewedPackageFileIdentity> identity;
    {
        ScopedPackageIdentityLock lock(archive, paths.staging);
        Require(lock.MatchesPathIdentity(), "ordinary package review lock is available");
        identity = lock.Identity();
    }
    // Real Windows sharing conflict on the package: the previous review gate
    // fails although reading the archive and installing it remain possible.
    HeldHandle occupied{CreateFileW(archive.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(occupied.value != INVALID_HANDLE_VALUE, "package sharing-conflict fixture opens");
    Require(WidgetPackageManager::Sha256File(archive).empty() &&
        snowdesktop::widget::detail::HashPackageFile(archive, true) == hash,
        "the occupied package still supports real content reads");
    {
        ScopedPackageIdentityLock strict(archive, paths.staging);
        if (strict.Acquired() || strict.ErrorCode() != ERROR_SHARING_VIOLATION || strict.ErrorPath() != archive)
            std::wcerr << strict.Details() << L'\n';
        Require(!strict.Acquired() && strict.ErrorCode() == ERROR_SHARING_VIOLATION &&
            strict.ErrorPath() == archive,
            "strict review reproduces the package sharing failure with its real path and code");
    }
    unsigned calls = 0;
    InstalledPackage installed;
    const auto install = [&]() {
        ++calls;
        return manager.InstallArchive(archive, {"local", "review-test"}, false,
            installed, report, error);
    };
    auto result = InstallReviewedPackage(archive, paths, identity, hash, manifest, false, install);
    Require(result.status == Status::NeedsLockConfirmation && calls == 0 &&
        !manager.ContainsPackage(manifest.id) && !result.lockDetails.empty(),
        "unconfirmed lock failure preserves cancellation and does not install anything");
    result = InstallReviewedPackage(archive, paths, identity, hash, manifest, true, install);
    Require(result.status == Status::Installed && calls == 1 && manager.ContainsPackage(manifest.id),
        "confirmed recovery reaches the real installer exactly once despite the same sharing conflict");
    {
        std::ifstream entry(installed.root / L"main.lua", std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(entry)), {});
        Require(content == lua, "confirmed recovery commits the reviewed component content");
    }
    result = InstallReviewedPackage(archive, paths, identity, hash, manifest, false, install);
    Require(result.status == Status::NeedsLockConfirmation && calls == 1,
        "a previous recovery choice never grants consent to a subsequent operation");
    result = InstallReviewedPackage(archive, paths, identity, hash, manifest, true, [&]() {
        ++calls;
        return manager.InstallArchive(archive, {"another-source", "another-item"}, false,
            installed, report, error);
    });
    Require(result.status == Status::InstallFailed && calls == 2 &&
        error.find("source changes require explicit confirmation") != std::string::npos,
        "lock recovery retains the installer's source confirmation and its actual failure");
    write(archive, "changed after review");
    result = InstallReviewedPackage(archive, paths, identity, hash, manifest, true, install);
    Require(result.status == Status::Changed && calls == 2,
        "recovery cannot install content that differs from the reviewed package");
    result = InstallReviewedPackage(archive, paths, identity,
        snowdesktop::widget::detail::HashPackageFile(archive, true), manifest, true, install);
    Require(result.status == Status::Invalid && calls == 2 && !result.validationDetails.empty(),
        "recovery retains actual format failures and their diagnostics");
    std::filesystem::remove(archive);
    result = InstallReviewedPackage(archive, paths, identity, hash, manifest, true, install);
    Require(result.status == Status::Unreadable && calls == 2,
        "an unreadable package never reaches installation or reports success");
}
}

void TestReviewedPackageInstallation()
{
    try { RunReviewedPackageInstallationCases(); }
    catch (const std::exception& error)
    {
        // Unwind all fixture resources before reporting failure to the runner.
        std::cerr << "FAILED: " << error.what() << '\n';
        std::exit(1);
    }
}
