#include "widget_package_manager.h"
#include "widget_package_read.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace snowdesktop::widget
{
using detail::Utf8ToWide;

bool WidgetPackageManager::CreateDevelopmentProject(
    const std::string& packageId, std::filesystem::path& projectRoot,
    std::string& error)
{
    projectRoot.clear();
    const auto existingDevelopment = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId;
        });
    if (existingDevelopment != packages_->end())
    {
        error = "a development project already exists for this component";
        return false;
    }

    const auto installed = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.manifest.id == packageId && package.active &&
                !package.builtin && !package.development;
        });
    if (installed == packages_->end())
    {
        error = "the installed component is unavailable";
        return false;
    }

    PackageManifest sourceManifest;
    const ValidationReport sourceReport = validator_.ValidateDirectory(
        installed->root, &sourceManifest);
    if (!sourceReport.Ok() || sourceManifest.id != packageId)
    {
        error = "the installed component failed validation";
        return false;
    }

    const std::filesystem::path staging =
        CreateStagingPath("development");
    std::error_code ec;
    auto discardStaging = [&]
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
    };
    if (!CopyPackageTree(installed->root, staging, error))
    {
        discardStaging();
        return false;
    }

    PackageManifest copiedManifest;
    const ValidationReport copiedReport = validator_.ValidateDirectory(
        staging, &copiedManifest);
    if (!copiedReport.Ok() || copiedManifest.id != sourceManifest.id ||
        copiedManifest.version != sourceManifest.version)
    {
        discardStaging();
        error = "the development copy failed validation";
        return false;
    }

    const std::wstring baseName = Utf8ToWide(
        copiedManifest.slug.empty() ? copiedManifest.id : copiedManifest.slug);
    std::filesystem::path destination = paths_.development / baseName;
    for (unsigned suffix = 2; std::filesystem::exists(destination, ec);
        ++suffix)
    {
        if (suffix > 10000)
        {
            destination = paths_.development /
                (baseName + L"-" + Utf8ToWide(GenerateUuid()));
            break;
        }
        destination = paths_.development /
            (baseName + L"-" + std::to_wstring(suffix));
    }
    if (ec)
    {
        discardStaging();
        error = "cannot inspect the development workspace: " + ec.message();
        return false;
    }

    std::filesystem::rename(staging, destination, ec);
    if (ec)
    {
        discardStaging();
        error = "cannot create the development project: " + ec.message();
        return false;
    }
    if (!Refresh(error))
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(destination, cleanupError);
        std::string refreshError;
        Refresh(refreshError);
        return false;
    }

    const auto created = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId &&
                package.root == destination;
        });
    if (created == packages_->end())
    {
        std::filesystem::remove_all(destination, ec);
        std::string refreshError;
        Refresh(refreshError);
        error = "the development project could not be loaded";
        return false;
    }
    projectRoot = destination;
    return true;
}
}
