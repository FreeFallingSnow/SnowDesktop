#pragma once

#include <cstdint>
#include <string>

namespace snowdesktop::winui
{

struct SettingsLanguageOption
{
    std::string code;
    std::wstring label;
};

enum class GeneralStartupConflictKind : std::uint8_t
{
    None,
    NonPackagedVersionOwnsStartup,
    InstalledVersionOwnsStartup,
};

/** Runtime-only startup ownership projected by the host. */
struct GeneralStartupConflict
{
    GeneralStartupConflictKind kind = GeneralStartupConflictKind::None;
    std::wstring ownerCommand;
};

} // namespace snowdesktop::winui
