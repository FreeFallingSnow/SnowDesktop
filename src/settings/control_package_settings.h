#pragma once
#include "control_settings.h"
#include "winui/windows_compat.h"
#include "winui/widgets_page_backend.h"

namespace snowdesktop::control_settings
{
struct PackageSettingsContext
{
    winui::IWidgetsPageBackend& backend;
    std::uint64_t generation;
};
bool IsPackageSettingsTarget(const Target& target) noexcept;
Value HandlePackageSettings(std::string_view method, const Value& params,
    const Target& target, const PackageSettingsContext& context);
}
