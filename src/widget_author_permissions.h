#pragma once

#include "widget_package.h"

#include <string>

namespace snowdesktop::widget_authoring
{

struct PermissionReport
{
    bool ok = false;
    std::string json;
};

/** Build a machine-readable report from the shared permission/API catalogs. */
PermissionReport BuildPermissionReport(
    const snowdesktop::widget::PackageManifest& manifest);

} // namespace snowdesktop::widget_authoring
