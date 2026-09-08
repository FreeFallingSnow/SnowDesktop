// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::steam_bridge
{
struct WidgetLocalization
{
    std::string locale;
    std::string title;
    std::string description;
};

struct SteamWorkshopLocalization
{
    std::string language;
    std::string title;
    std::string description;
};

std::optional<std::string> SteamApiLanguageForLocale(
    std::string_view locale);

std::vector<SteamWorkshopLocalization> BuildSteamWorkshopLocalizations(
    std::string_view fallbackTitle, std::string_view fallbackDescription,
    const std::vector<WidgetLocalization>& localizations);
}
