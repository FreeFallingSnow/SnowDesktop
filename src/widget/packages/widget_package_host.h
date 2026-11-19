#pragma once

#include "widget_package_catalog_snapshot.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::widget { class WidgetPackageManager; }

namespace snowdesktop::widget_runtime
{
void TrimWidgetPackageHistory(widget::WidgetPackageManager& manager,
    std::string_view packageId = {});
widget::WidgetPackageManager& GetWidgetPackageManager();
std::wstring ResolveWidgetPath(const std::wstring& packageId);
bool RecoverWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& preferredVersion = std::nullopt,
    std::string_view rejectedVersion = {});
std::optional<widget::InstalledPackage> ResolveWidgetEntryPackage(const std::filesystem::path& path);
}
