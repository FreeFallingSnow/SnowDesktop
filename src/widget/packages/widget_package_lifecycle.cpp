#include "widget_package_manager.h"
#include "widget_package_limits.h"
#include "widget_package_read.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

namespace snowdesktop::widget
{
namespace
{
using detail::HasReparsePoint;
using detail::PathContainsReparsePoint;
using detail::PathUtf8;
using detail::Timestamp;
using detail::Utf8ToWide;

void RecordCleanupError(std::string& error, const std::filesystem::path& path,
    const std::string& detail)
{
    if (error.empty())
        error = "cannot clean component version " + PathUtf8(path) + ": " + detail;
}

bool IsSafeRemovalTree(const std::filesystem::path& root, std::error_code& ec)
{
    if (HasReparsePoint(root))
        return false;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (HasReparsePoint(it->path()))
            return false;
    }
    return !ec;
}

void RemoveRetiredVersions(const std::filesystem::path& root, std::string& error)
{
    if (PathContainsReparsePoint(root))
    {
        RecordCleanupError(error, root, "cleanup directory contains a reparse point");
        return;
    }
    std::vector<std::filesystem::path> tickets;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end;
        !ec && it != end; it.increment(ec))
    {
        const auto& path = it->path();
        if (path.parent_path() == root && WidgetPackageValidator::IsUuid(PathUtf8(path.filename())))
            tickets.push_back(path);
    }
    if (ec) RecordCleanupError(error, root, ec.message());
    for (const auto& ticket : tickets)
    {
        ec.clear();
        if (!IsSafeRemovalTree(ticket, ec))
        {
            RecordCleanupError(error, ticket, ec ? ec.message() : "directory contains a reparse point");
            continue;
        }
        std::filesystem::remove_all(ticket, ec);
        if (ec) RecordCleanupError(error, ticket, ec.message());
    }
}

void RestorePackageDirectory(const std::filesystem::path& quarantine,
    const std::filesystem::path& original, std::string& error)
{
    std::error_code restoreError;
    std::filesystem::rename(quarantine, original, restoreError);
    if (restoreError)
        error += "; cannot restore package directory: " + restoreError.message();
}
}

bool WidgetPackageManager::TrimInstalledVersions(std::string& error, std::string_view packageId)
{
    error.clear();
    struct History
    {
        bool hasCurrent = false;
        std::vector<const InstalledPackage*> restorable;
    };
    std::unordered_map<std::string, History> histories;
    for (const auto& package : *packages_)
    {
        if (package.builtin || package.development || package.unregistered ||
            (!packageId.empty() && package.manifest.id != packageId))
            continue;
        const auto entry = registry_.entries.find(package.manifest.id);
        if (entry == registry_.entries.end() || entry->second.source != package.source)
            continue;
        auto& history = histories[package.manifest.id];
        if (package.manifest.version == entry->second.activeVersion)
            history.hasCurrent = true;
        else
            history.restorable.push_back(&package);
    }

    std::vector<const InstalledPackage*> expired;
    for (auto& [id, history] : histories)
    {
        (void)id;
        auto& versions = history.restorable;
        if (!history.hasCurrent || versions.size() <= kMaxRestorablePackageVersions)
            continue;
        std::sort(versions.begin(), versions.end(), [](const auto* left, const auto* right) {
            return WidgetPackageValidator::IsNewerSemVer(left->manifest.version,
                right->manifest.version);
        });
        expired.insert(expired.end(),
            versions.begin() + static_cast<std::ptrdiff_t>(kMaxRestorablePackageVersions),
            versions.end());
    }

    std::error_code ec;
    const auto retiredRoot = std::filesystem::absolute(
        paths_.quarantine / L"expired-installed-versions", ec).lexically_normal();
    if (ec || PathContainsReparsePoint(retiredRoot))
    {
        RecordCleanupError(error, paths_.quarantine,
            ec ? ec.message() : "cleanup directory contains a reparse point");
        return false;
    }
    if (!expired.empty())
        std::filesystem::create_directory(retiredRoot, ec);
    const bool hasRetiredDirectory = !ec && std::filesystem::exists(retiredRoot, ec);
    if (ec) RecordCleanupError(error, retiredRoot, ec.message());
    if (!hasRetiredDirectory) return error.empty();

    std::unordered_set<std::filesystem::path> retired;
    for (const auto* package : expired)
    {
        const auto& id = package->manifest.id;
        const auto& version = package->manifest.version;
        const auto path = paths_.installed / Utf8ToWide(id) / Utf8ToWide(version);
        const auto manifestPath = path / L"widget.json";
        const auto ticket = GenerateUuid();
        if (!WidgetPackageValidator::IsUuid(id) || !WidgetPackageValidator::IsSemVer(version) ||
            !WidgetPackageValidator::IsUuid(ticket) || package->root != path ||
            PathContainsReparsePoint(manifestPath))
        {
            RecordCleanupError(error, path, "unsafe installed version path");
            continue;
        }
        PackageManifest manifest;
        ValidationReport report;
        if (!validator_.ReadManifest(manifestPath, manifest, report) ||
            manifest.id != id || manifest.version != version)
        {
            RecordCleanupError(error, path, "installed version identity changed before cleanup");
            continue;
        }
        std::filesystem::rename(path, retiredRoot / Utf8ToWide(ticket), ec);
        if (ec) RecordCleanupError(error, path, ec.message());
        else retired.insert(path);
    }
    if (!retired.empty())
    {
        auto remaining = std::make_shared<std::vector<InstalledPackage>>(*packages_);
        std::erase_if(*remaining, [&](const auto& package) {
            return retired.contains(package.root);
        });
        packages_ = std::move(remaining);
        catalogueRevision_.reset();
    }
    RemoveRetiredVersions(retiredRoot, error);
    return error.empty();
}

bool WidgetPackageManager::Rollback(const std::string& packageId,
    const std::string& version, std::string& error)
{
    if (!registry_.entries.contains(packageId))
    {
        error = "package has no installed version registry";
        return false;
    }
    const auto target = paths_.installed / Utf8ToWide(packageId) /
        Utf8ToWide(version);
    PackageManifest manifest;
    const auto report = validator_.ValidateDirectory(target, &manifest);
    if (!report.Ok() || manifest.id != packageId || manifest.version != version)
    {
        error = "requested rollback version is invalid or missing";
        return false;
    }
    auto pending = *this;
    pending.registry_.entries.at(packageId).activeVersion = version;
    if (!pending.Refresh(error, false)) return false;
    const bool foundInCatalog = std::any_of(pending.packages_->begin(),
        pending.packages_->end(), [&](const auto& package) {
            return package.selected && package.root == target &&
                package.manifest.id == packageId && package.manifest.version == version;
        });
    if (!foundInCatalog)
    {
        error = "requested rollback version is invalid or missing";
        return false;
    }
    if (!pending.registry_.Save(paths_.registry, error)) return false;
    *this = std::move(pending);
    return true;
}

bool WidgetPackageManager::QuarantineInactiveVersion(
    const InstalledPackage& package, std::string& error)
{
    const auto& id = package.manifest.id;
    const auto& version = package.manifest.version;
    const auto registered = registry_.entries.find(id);
    if (registered == registry_.entries.end() || registered->second.source != package.source ||
        registered->second.activeVersion == version ||
        !WidgetPackageValidator::IsUuid(id) || !WidgetPackageValidator::IsSemVer(version))
    {
        error = "cannot quarantine the active or unowned package version";
        return false;
    }
    const auto source = paths_.installed / Utf8ToWide(id) / Utf8ToWide(version);
    if (source != package.root || PathContainsReparsePoint(source) ||
        PathContainsReparsePoint(paths_.quarantine))
    {
        error = "cannot quarantine an unsafe package version path";
        return false;
    }
    const auto quarantine = paths_.quarantine /
        Utf8ToWide(id + "-" + version + "-rejected-" + GenerateUuid());
    auto pending = *this;
    std::error_code ec;
    std::filesystem::rename(source, quarantine, ec);
    if (ec)
    {
        error = "cannot quarantine rejected package version: " + ec.message();
        return false;
    }
    if (!pending.Refresh(error))
    {
        RestorePackageDirectory(quarantine, source, error);
        return false;
    }
    *this = std::move(pending);
    return true;
}

bool WidgetPackageManager::Uninstall(const std::string& packageId,
    std::string& error)
{
    const auto registered = registry_.entries.find(packageId);
    if (registered == registry_.entries.end())
    {
        error = "package is not installed";
        return false;
    }
    const auto source = paths_.installed / Utf8ToWide(packageId);
    const auto quarantine = paths_.quarantine /
        (Utf8ToWide(packageId) + L"-uninstalled-" + Utf8ToWide(Timestamp()));
    auto pending = *this;
    pending.registry_.entries.erase(packageId);
    if (!registered->second.enabled && std::any_of(
            packages_->begin(), packages_->end(), [&](const auto& package) {
                return package.development && package.manifest.id == packageId;
            }))
        pending.registry_.disabledDevelopmentIds.insert(packageId);

    std::error_code ec;
    std::filesystem::rename(source, quarantine, ec);
    if (ec)
    {
        error = "cannot move package to quarantine: " + ec.message();
        return false;
    }
    if (!pending.Refresh(error, false) ||
        !pending.registry_.Save(paths_.registry, error))
    {
        RestorePackageDirectory(quarantine, source, error);
        return false;
    }
    *this = std::move(pending);
    return true;
}
}
