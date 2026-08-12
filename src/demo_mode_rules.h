#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snowdesktop::demo_mode_rules
{

struct VisualIdentity
{
    std::wstring_view title;
    std::wstring_view glyph;
    std::uint32_t backgroundRgb;
};

// These are generic Fluent System Icons rather than product or brand logos.
// Their titles are localized at the presentation boundary, while the stable
// identity hash keeps the same real shortcut visually consistent everywhere.
inline constexpr std::array<VisualIdentity, 16> kVisualIdentities{
    VisualIdentity{ L"Northstar Projects", L"\uE1EB", 0x5968D8 },
    VisualIdentity{ L"Luma Canvas", L"\uF357", 0xC75B7A },
    VisualIdentity{ L"QuietFocus", L"\uF07D", 0xD2873E },
    VisualIdentity{ L"Orbit Workspace", L"\uE487", 0x477FC1 },
    VisualIdentity{ L"Quill Notes", L"\uEED9", 0x7A68B8 },
    VisualIdentity{ L"Cedar Vault", L"\uE066", 0x66758C },
    VisualIdentity{ L"Vista Player", L"\uEDF0", 0xA85291 },
    VisualIdentity{ L"Courier Mail", L"\uF506", 0x3F8EAE },
    VisualIdentity{ L"Daymark Calendar", L"\uE238", 0xC5634F },
    VisualIdentity{ L"Pulse Utilities", L"\uF82F", 0x4D8C65 },
    VisualIdentity{ L"Sparkboard", L"\uEB34", 0xB48A34 },
    VisualIdentity{ L"Wayfinder", L"\uE6B2", 0x3D8D88 },
    VisualIdentity{ L"Facet Photos", L"\uE719", 0xB65C65 },
    VisualIdentity{ L"Tempo Music", L"\uE855", 0x7562A8 },
    VisualIdentity{ L"Branchline IDE", L"\uEFBE", 0x3F78A8 },
    VisualIdentity{ L"Atlas Data", L"\uEECC", 0x54854D },
};

// Indices 16..114 are reserved for collection categories. Categories own
// disjoint subject icons, and the larger pools cover the entries exposed by
// inline collection slots plus the compact "all" mosaic before badges repeat
// a subject. The array order mirrors the contiguous RCDATA resource IDs.
inline constexpr std::array<VisualIdentity, 99> kCategoryVisualIdentities{
    VisualIdentity{ L"Atlas Library", L"\uE8A5", 0x397FE7 },
    VisualIdentity{ L"Luma Present", L"\uE650", 0xE98A24 },
    VisualIdentity{ L"Ledger Pro", L"\uEA44", 0x20A384 },
    VisualIdentity{ L"Relay Chat", L"\uE8BD", 0x4B8EDB },
    VisualIdentity{ L"ReelView", L"\uF4B0", 0x835BC8 },
    VisualIdentity{ L"Axis Draft", L"\uE15F", 0xD88A24 },
    VisualIdentity{ L"Mosaic AI", L"\uE128", 0x5051A5 },
    VisualIdentity{ L"Arcade Hub", L"\uE16B", 0x4A58B5 },
    VisualIdentity{ L"Harbor Ops", L"\uF1BE", 0x3A596C },
    VisualIdentity{ L"Branchline Terminal", L"\uE756", 0x454953 },
    VisualIdentity{ L"Orbit Browser", L"\uE774", 0x238ED2 },
    VisualIdentity{ L"CloudGate", L"\uE753", 0x6983DD },
    VisualIdentity{ L"Cedar Files", L"\uE8A5", 0x397FE7 },
    VisualIdentity{ L"Safekeep Backup", L"\uE8B7", 0x2467C9 },
    VisualIdentity{ L"Pulse Monitor", L"\uE9D9", 0x354A66 },
    VisualIdentity{ L"Sentinel Guard", L"\uEA18", 0x4255C8 },
    VisualIdentity{ L"Luma Sketch", L"\uE70F", 0xD76685 },
    VisualIdentity{ L"FrameForge", L"\uE768", 0x7654C8 },
    VisualIdentity{ L"Northstar IDE", L"\uE943", 0x276EBE },
    VisualIdentity{ L"MergePath", L"\uE8AB", 0x566BC6 },
    VisualIdentity{ L"Papertrail Reader", L"\uE8A5", 0xD9554F },
    VisualIdentity{ L"Taskboard", L"\uE762", 0x258B7A },
    VisualIdentity{ L"Huddle Meet", L"\uE8AA", 0x3A87D6 },
    VisualIdentity{ L"PeopleHub", L"\uE716", 0x2F9DB6 },
    VisualIdentity{ L"Echo Player", L"\uE768", 0x714DB4 },
    VisualIdentity{ L"Airwave", L"\uE720", 0xA64683 },
    VisualIdentity{ L"ForgeCAD", L"\uE71A", 0x2B72B9 },
    VisualIdentity{ L"CircuitLab", L"\uE950", 0x315875 },
    VisualIdentity{ L"Neural Desk", L"\uE945", 0x765BCC },
    VisualIdentity{ L"FlowMind", L"\uE8ED", 0x4F82BF },
    VisualIdentity{ L"Pixel Deck", L"\uE7FC", 0x4B54B7 },
    VisualIdentity{ L"Apex Racing", L"\uE7FC", 0x343941 },
    VisualIdentity{ L"Network Watch", L"\uE968", 0x27537B },
    VisualIdentity{ L"LogScope", L"\uE9D9", 0x455369 },
    VisualIdentity{ L"WebCanvas", L"\uE774", 0x2C91CE },
    VisualIdentity{ L"Cloud Portal", L"\uE753", 0x6E81D4 },
    VisualIdentity{ L"Dropzone", L"\uE896", 0x2C7FD2 },
    VisualIdentity{ L"Relay Remote", L"\uE8AF", 0x3A78BD },
    VisualIdentity{ L"Chroma Studio", L"\uE790", 0xC96B63 },
    VisualIdentity{ L"DataForge", L"\uE8CE", 0x4766B9 },
    VisualIdentity{ L"Quill Writer", L"\uE70F", 0x397FD2 },
    VisualIdentity{ L"Postbox Mail", L"\uE715", 0x347FC2 },
    VisualIdentity{ L"Prism Photos", L"\uEB9F", 0x7065BD },
    VisualIdentity{ L"MeshFlow", L"\uE9D2", 0x3479BB },
    VisualIdentity{ L"Prompt Harbor", L"\uE8BD", 0x7C57C7 },
    VisualIdentity{ L"Quest Compass", L"\uE81B", 0x71449B },
    VisualIdentity{ L"Stackyard", L"\uE7B8", 0x315274 },
    VisualIdentity{ L"Skyfetch", L"\uE896", 0x3C8DD1 },
    VisualIdentity{ L"Harbor Archive", L"\uE8A5", 0x356FC1 },
    VisualIdentity{ L"Papertrail Vault", L"\uE72E", 0x365AA5 },
    VisualIdentity{ L"Northstar Drive", L"\uE8A5", 0x4C75BB },
    VisualIdentity{ L"Clearpath Sync", L"\uE895", 0x5B59C6 },
    VisualIdentity{ L"Fieldnote Docs", L"\uE8A5", 0x5872B1 },
    VisualIdentity{ L"Waypoint Cabinet", L"\uE8A5", 0x4D6298 },
    VisualIdentity{ L"Luma Transfer", L"\uE8AB", 0x566BC6 },
    VisualIdentity{ L"TuneUp Center", L"\uF82F", 0x5647AA },
    VisualIdentity{ L"Beacon Tools", L"\uE74D", 0x9A526B },
    VisualIdentity{ L"Canvas North", L"\uF357", 0x8C59BF },
    VisualIdentity{ L"Storyline", L"\uE768", 0xC35C8A },
    VisualIdentity{ L"Motion Deck", L"\uE714", 0x3B79BE },
    VisualIdentity{ L"Code Harbor", L"\uE968", 0x426DB6 },
    VisualIdentity{ L"Stackline", L"\uE90F", 0x394EAA },
    VisualIdentity{ L"Debug Desk", L"\uE7BA", 0x4B6599 },
    VisualIdentity{ L"Buildcraft", L"\uF166", 0x7854B8 },
    VisualIdentity{ L"Commit Grove", L"\uE9D9", 0x386EA6 },
    VisualIdentity{ L"Runtime Lab", L"\uE768", 0x7654C8 },
    VisualIdentity{ L"SourcePilot", L"\uE950", 0x456BAA },
    VisualIdentity{ L"Northstar Sheets", L"\uEA44", 0x287EB5 },
    VisualIdentity{ L"Briefcase", L"\uE821", 0x6151B2 },
    VisualIdentity{ L"Meeting Notes", L"\uE70B", 0x3876A8 },
    VisualIdentity{ L"Slideworks", L"\uE650", 0xD3705F },
    VisualIdentity{ L"Signal Room", L"\uE8BD", 0x4B8EDB },
    VisualIdentity{ L"Courier Desk", L"\uE8AA", 0x3A87D6 },
    VisualIdentity{ L"Talkline", L"\uE720", 0x5968D8 },
    VisualIdentity{ L"Tempo Video", L"\uE768", 0x7654C8 },
    VisualIdentity{ L"Soundstage", L"\uE720", 0xA64683 },
    VisualIdentity{ L"Framebox", L"\uEDB9", 0x7259B3 },
    VisualIdentity{ L"Vector Works", L"\uE15F", 0x3979B4 },
    VisualIdentity{ L"Model Harbor", L"\uE9D2", 0x3F6CA5 },
    VisualIdentity{ L"Simulate Pro", L"\uF1C3", 0x6556AE },
    VisualIdentity{ L"Blueprint Desk", L"\uE8A5", 0x2E76B8 },
    VisualIdentity{ L"Measure Lab", L"\uE9D2", 0x475EA5 },
    VisualIdentity{ L"Assembly Bay", L"\uE90F", 0x5063A8 },
    VisualIdentity{ L"Field Solver", L"\uE945", 0x3A78B5 },
    VisualIdentity{ L"Design Grid", L"\uE950", 0x315875 },
    VisualIdentity{ L"Prototype Room", L"\uF1C3", 0x5E56B2 },
    VisualIdentity{ L"Model Studio", L"\uE128", 0x6C53C0 },
    VisualIdentity{ L"Insight Engine", L"\uE8B6", 0x4F74B8 },
    VisualIdentity{ L"Reasoning Lab", L"\uE945", 0x765BCC },
    VisualIdentity{ L"Playforge", L"\uE7FC", 0x6B4DB7 },
    VisualIdentity{ L"Game Harbor", L"\uE7FC", 0x4A58B5 },
    VisualIdentity{ L"Level Studio", L"\uE81B", 0x71449B },
    VisualIdentity{ L"Party Relay", L"\uE716", 0x7654C8 },
    VisualIdentity{ L"Victory Lane", L"\uE7FC", 0x3A69A8 },
    VisualIdentity{ L"Server Desk", L"\uE968", 0x355B79 },
    VisualIdentity{ L"Deploy Lane", L"\uE895", 0x5953A7 },
    VisualIdentity{ L"Cluster View", L"\uE968", 0x3A6C8C },
    VisualIdentity{ L"Service Monitor", L"\uE9D9", 0x455369 },
    VisualIdentity{ L"Runbook", L"\uE8A5", 0x315274 },
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
