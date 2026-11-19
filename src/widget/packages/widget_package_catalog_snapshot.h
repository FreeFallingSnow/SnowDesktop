#pragma once

#include "widget_package.h"
#include "widget_package_source_types.h"
#include "widget_package_validation_report.h"
#include "widget/permissions/widget_permission_state.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
struct InstalledPackage
{
    PackageManifest manifest;
    PackageSourceRef source;
    std::filesystem::path root;
    std::string sha256;
    PermissionDecisionState permissionState =
        PermissionDecisionState::LegacyImplicit;
    std::vector<std::string> grantedPermissions;
    std::vector<std::string> grantedNetworkDomains;
    bool builtin = false;
    bool development = false;
    bool enabled = true;
    // Selected within its own source. A development override can temporarily
    // make a selected managed package inactive without hiding its state from
    // management UI.
    bool selected = false;
    bool active = true;
    // Files discovered under installed/ without a registry owner are inactive
    // recovery candidates, not a locally installed source reservation.
    bool unregistered = false;
};

using InstalledPackageCatalog = std::shared_ptr<const std::vector<InstalledPackage>>;

std::string PackagePermissionReviewFingerprint(const InstalledPackage& package);

struct InvalidPackage
{
    std::string packageId;
    PackageManifest manifest;
    PackageSourceRef source;
    std::filesystem::path root;
    ValidationReport report;
    bool builtin = false;
    bool development = false;
    bool selected = false;
};
}
