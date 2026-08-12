#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snowdesktop::demo_mode_rules
{

struct VisualIdentity
{
    const char* titleKey;
    std::wstring_view glyph;
    std::uint32_t backgroundRgb;
};

// These are generic Fluent System Icons rather than product or brand logos.
// Their titles are localized at the presentation boundary, while the stable
// identity hash keeps the same real shortcut visually consistent everywhere.
inline constexpr std::array<VisualIdentity, 16> kVisualIdentities{
    VisualIdentity{ "app.demo_identity.projects", L"\uE1EB", 0x5968D8 },
    VisualIdentity{ "app.demo_identity.design", L"\uF357", 0xC75B7A },
    VisualIdentity{ "app.demo_identity.focus", L"\uF07D", 0xD2873E },
    VisualIdentity{ "app.demo_identity.workspace", L"\uE487", 0x477FC1 },
    VisualIdentity{ "app.demo_identity.notes", L"\uEED9", 0x7A68B8 },
    VisualIdentity{ "app.demo_identity.archive", L"\uE066", 0x66758C },
    VisualIdentity{ "app.demo_identity.media", L"\uEDF0", 0xA85291 },
    VisualIdentity{ "app.demo_identity.mail", L"\uF506", 0x3F8EAE },
    VisualIdentity{ "app.demo_identity.calendar", L"\uE238", 0xC5634F },
    VisualIdentity{ "app.demo_identity.tools", L"\uF82F", 0x4D8C65 },
    VisualIdentity{ "app.demo_identity.ideas", L"\uEB34", 0xB48A34 },
    VisualIdentity{ "app.demo_identity.explore", L"\uE6B2", 0x3D8D88 },
    VisualIdentity{ "app.demo_identity.photos", L"\uE719", 0xB65C65 },
    VisualIdentity{ "app.demo_identity.music", L"\uE855", 0x7562A8 },
    VisualIdentity{ "app.demo_identity.code", L"\uEFBE", 0x3F78A8 },
    VisualIdentity{ "app.demo_identity.data", L"\uEECC", 0x54854D },
};

inline constexpr std::array<VisualIdentity, 12> kCategoryVisualIdentities{
    VisualIdentity{ "app.demo_identity.notes", L"\uE8A5", 0x397FE7 },
    VisualIdentity{ "app.demo_identity.projects", L"\uE650", 0xE98A24 },
    VisualIdentity{ "app.demo_identity.data", L"\uEA44", 0x20A384 },
    VisualIdentity{ "app.demo_identity.mail", L"\uE8BD", 0x4B8EDB },
    VisualIdentity{ "app.demo_identity.media", L"\uF4B0", 0x835BC8 },
    VisualIdentity{ "app.demo_identity.tools", L"\uE15F", 0xD88A24 },
    VisualIdentity{ "app.demo_identity.ideas", L"\uE128", 0x5051A5 },
    VisualIdentity{ "app.demo_identity.explore", L"\uE16B", 0x4A58B5 },
    VisualIdentity{ "app.demo_identity.tools", L"\uF1BE", 0x3A596C },
    VisualIdentity{ "app.demo_identity.code", L"\uE756", 0x454953 },
    VisualIdentity{ "app.demo_identity.explore", L"\uE774", 0x238ED2 },
    VisualIdentity{ "app.demo_identity.archive", L"\uE753", 0x6983DD },
};

inline constexpr std::size_t kDemoIconAssetCount =
    kVisualIdentities.size() + kCategoryVisualIdentities.size();

inline const VisualIdentity& VisualIdentityAt(
    std::size_t index) noexcept
{
    if (index < kVisualIdentities.size())
        return kVisualIdentities[index];
    const std::size_t categoryIndex = index - kVisualIdentities.size();
    return categoryIndex < kCategoryVisualIdentities.size()
        ? kCategoryVisualIdentities[categoryIndex]
        : kVisualIdentities.front();
}

inline constexpr bool ShouldMaskApplication(
    bool demoModeEnabled, bool isApplicationShortcut) noexcept
{
    return demoModeEnabled && isApplicationShortcut;
}

inline std::uint64_t StableIdentityHash(std::wstring_view identity) noexcept
{
    // FNV-1a over the UTF-16 code-unit bytes used by Windows. This avoids the
    // implementation-defined/randomized behavior of std::hash and makes the
    // mapping repeatable across launches and builds.
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t character : identity)
    {
        const auto codeUnit = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(codeUnit & 0xFFU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint8_t>((codeUnit >> 8U) & 0xFFU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::size_t VisualIdentityIndex(std::wstring_view identity) noexcept
{
    return static_cast<std::size_t>(
        StableIdentityHash(identity) % kVisualIdentities.size());
}

inline const VisualIdentity& ResolveVisualIdentity(
    std::wstring_view identity) noexcept
{
    return kVisualIdentities[VisualIdentityIndex(identity)];
}

} // namespace snowdesktop::demo_mode_rules
