#include "widget_package.h"
#include "widget_package_read.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace snowdesktop::widget
{
namespace
{
using detail::PathContainsReparsePoint;
using detail::Timestamp;
using detail::Utf8ToWide;

void RestorePackageDirectory(const std::filesystem::path& quarantine,
    const std::filesystem::path& original, std::string& error)
{
    std::error_code restoreError;
    std::filesystem::rename(quarantine, original, restoreError);
    if (restoreError)
        error += "; cannot restore package directory: " + restoreError.message();
}
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
