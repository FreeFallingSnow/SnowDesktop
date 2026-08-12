#pragma once

#include "demo_mode_rules.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace snowdesktop::demo_collection_rules
{

struct Category
{
    std::wstring_view id;
    const char* titleKey;
    std::span<const std::size_t> visualIndices;
    std::span<const std::wstring_view> identityTitles;
    std::span<const std::wstring_view> keywords;
};

struct IdentityPresentation
{
    std::size_t visualIndex = 0;
    std::size_t variantIndex = 0;
    std::wstring_view title;
};

inline constexpr std::size_t kIdentityVariantCount = 5;
inline constexpr std::size_t kIdentityCountPerCategory = 20;

inline constexpr std::array<std::size_t, 11> kFilesVisuals{
    16, 28, 29, 52, 64, 65, 66, 67, 68, 69, 70 };
inline constexpr std::array<std::size_t, 6> kUtilitiesVisuals{
    27, 30, 31, 53, 71, 72 };
inline constexpr std::array<std::size_t, 7> kCreativeVisuals{
    17, 32, 33, 54, 73, 74, 75 };
inline constexpr std::array<std::size_t, 11> kDevelopmentVisuals{
    25, 34, 35, 55, 76, 77, 78, 79, 80, 81, 82 };
inline constexpr std::array<std::size_t, 8> kOfficeVisuals{
    18, 36, 37, 56, 83, 84, 85, 86 };
inline constexpr std::array<std::size_t, 7> kCommunicationVisuals{
    19, 38, 39, 57, 87, 88, 89 };
inline constexpr std::array<std::size_t, 7> kMediaVisuals{
    20, 40, 41, 58, 90, 91, 92 };
inline constexpr std::array<std::size_t, 13> kEngineeringVisuals{
    21, 42, 43, 59, 93, 94, 95, 96, 97, 98, 99, 100, 101 };
inline constexpr std::array<std::size_t, 7> kAiVisuals{
    22, 44, 45, 60, 102, 103, 104 };
inline constexpr std::array<std::size_t, 9> kGamingVisuals{
    23, 46, 47, 61, 105, 106, 107, 108, 109 };
inline constexpr std::array<std::size_t, 9> kOperationsVisuals{
    24, 48, 49, 62, 110, 111, 112, 113, 114 };
inline constexpr std::array<std::size_t, 4> kWebVisuals{
    26, 50, 51, 63 };

inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kFilesTitles{
        L"Atlas Library", L"Cedar Files", L"Safekeep Backup", L"Dropzone",
        L"Harbor Archive", L"Papertrail Vault", L"Northstar Drive", L"Clearpath Sync",
        L"Fieldnote Docs", L"Waypoint Cabinet", L"Luma Transfer", L"Bridgebox",
        L"QuietStore", L"Parcel Desk", L"Openleaf Records", L"Homebase Files",
        L"Bluegate Share", L"Kindred Archive", L"Daymark Storage", L"Relay Folders" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kUtilitiesTitles{
        L"CloudGate", L"Pulse Monitor", L"Sentinel Guard", L"Relay Remote",
        L"TuneUp Center", L"Beacon Tools", L"Northstar Control", L"QuickPanel",
        L"Clearview Monitor", L"Switchboard", L"Tasklight", L"Keysmith",
        L"Orbit Settings", L"Portwatch", L"QuietMode", L"Signal Desk",
        L"Device Harbor", L"App Compass", L"System Lens", L"Control Bay" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kCreativeTitles{
        L"Luma Present", L"Luma Sketch", L"FrameForge", L"Chroma Studio",
        L"Canvas North", L"Storyline", L"Motion Deck", L"Palette Works",
        L"Render Room", L"Studio Relay", L"Facet Design", L"Inkstone",
        L"SceneCraft", L"Colorway", L"Layout Lab", L"Clipmaker",
        L"Visual Foundry", L"Storyboard", L"Shape House", L"Creative Bay" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kDevelopmentTitles{
        L"Branchline Terminal", L"Northstar IDE", L"MergePath", L"DataForge",
        L"Code Harbor", L"Stackline", L"Debug Desk", L"Buildcraft",
        L"Commit Grove", L"Runtime Lab", L"SourcePilot", L"Dev Relay",
        L"Module Works", L"Compile Bay", L"Syntax Studio", L"Testbench",
        L"Package Yard", L"API Compass", L"Query Desk", L"Release Lane" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kOfficeTitles{
        L"Ledger Pro", L"Papertrail Reader", L"Taskboard", L"Quill Writer",
        L"Northstar Sheets", L"Briefcase", L"Meeting Notes", L"Slideworks",
        L"Daymark Planner", L"Formcraft", L"Office Relay", L"Draftroom",
        L"Report Studio", L"Tableline", L"Agenda Desk", L"Document Bay",
        L"Worklog", L"Outline Pro", L"Review Board", L"Project Ledger" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kCommunicationTitles{
        L"Relay Chat", L"Huddle Meet", L"PeopleHub", L"Postbox Mail",
        L"Signal Room", L"Courier Desk", L"Talkline", L"Team Beacon",
        L"Message Bay", L"Inbox Pilot", L"Callbridge", L"Contact Grove",
        L"Meeting Point", L"Channel Desk", L"Voice Relay", L"Mailroom",
        L"GroupLink", L"Conversation Hub", L"Connect Center", L"Briefing Room" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kMediaTitles{
        L"ReelView", L"Echo Player", L"Airwave", L"Prism Photos",
        L"Tempo Video", L"Soundstage", L"Framebox", L"Media Harbor",
        L"Playlist Room", L"Screenlight", L"Audio Grove", L"Clip Library",
        L"Photo Desk", L"Stream Relay", L"Waveform", L"Gallery Works",
        L"Playback Studio", L"Album House", L"Cinema Bay", L"Studio Player" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kEngineeringTitles{
        L"Axis Draft", L"ForgeCAD", L"CircuitLab", L"MeshFlow",
        L"Vector Works", L"Model Harbor", L"Simulate Pro", L"Blueprint Desk",
        L"Measure Lab", L"Assembly Bay", L"Field Solver", L"Design Grid",
        L"Prototype Room", L"Structure Studio", L"Motion Analysis", L"Toolpath",
        L"Material Desk", L"System Model", L"Test Rig", L"Engineering Hub" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kAiTitles{
        L"Mosaic AI", L"Neural Desk", L"FlowMind", L"Prompt Harbor",
        L"Model Studio", L"Insight Engine", L"Reasoning Lab", L"Agent Grove",
        L"Context Desk", L"Vector Mind", L"Inference Bay", L"Data Pilot",
        L"Tensor Works", L"Idea Relay", L"Research Copilot", L"Knowledge Loom",
        L"Pattern Lab", L"Signal AI", L"Model Foundry", L"Prompt Studio" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kGamingTitles{
        L"Arcade Hub", L"Pixel Deck", L"Apex Racing", L"Quest Compass",
        L"Playforge", L"Game Harbor", L"Level Studio", L"Party Relay",
        L"Victory Lane", L"Checkpoint", L"Circuit Play", L"Arena Desk",
        L"Worldbuilder", L"Controller Bay", L"Match Point", L"Replay Room",
        L"Adventure Map", L"Co-op Corner", L"Game Library", L"Launcher Works" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kOperationsTitles{
        L"Harbor Ops", L"Network Watch", L"LogScope", L"Stackyard",
        L"Server Desk", L"Deploy Lane", L"Cluster View", L"Service Monitor",
        L"Runbook", L"Incident Room", L"Metric Bay", L"Pipeline Ops",
        L"Host Control", L"Tracepoint", L"Container Yard", L"Uptime Desk",
        L"Release Watch", L"System Harbor", L"Cloud Operations", L"Response Center" };
inline constexpr std::array<std::wstring_view, kIdentityCountPerCategory>
    kWebTitles{
        L"Orbit Browser", L"WebCanvas", L"Cloud Portal", L"Skyfetch",
        L"Pagecraft", L"Link Harbor", L"Site Studio", L"Tabspace",
        L"Bookmark Bay", L"Web Relay", L"Portal Desk", L"Cloudline",
        L"Browse North", L"Launch Page", L"Domain Works", L"Request Lab",
        L"Web Compass", L"Online Studio", L"Gateway", L"Frontend Desk" };

inline constexpr std::array<std::wstring_view, 13> kFilesKeywords{
    L"\u6587\u4ef6", L"\u4e0b\u8f7d", L"\u6587\u6863", L"\u56fe\u7247", L"\u89c6\u9891", L"\u97f3\u4e50",
    L"file", L"folder", L"download", L"document", L"photo", L"archive", L"cloud" };
inline constexpr std::array<std::wstring_view, 12> kUtilitiesKeywords{
    L"\u5de5\u5177", L"\u63a7\u5236", L"\u7f51\u76d8", L"\u4e91", L"tool", L"utility", L"control",
    L"wallpaper", L"everything", L"wiztree", L"utools", L"vhub" };
inline constexpr std::array<std::wstring_view, 15> kCreativeKeywords{
    L"\u521b\u9020", L"\u521b\u4f5c", L"\u8bbe\u8ba1", L"\u89c6\u9891", L"\u56fe\u50cf", L"photoshop", L"premiere",
    L"afterfx", L"encoder", L"obs", L"blender", L"davinci", L"design", L"creative", L"edit" };
inline constexpr std::array<std::wstring_view, 17> kDevelopmentKeywords{
    L"\u7f16\u7a0b", L"\u5f00\u53d1", L"\u4ee3\u7801", L"code", L"studio", L"pycharm", L"qt creator",
    L"arduino", L"git", L"mongodb", L"heidisql", L"android", L"vmware", L"twincat",
    L"developer", L"ide", L"hbuilder" };
inline constexpr std::array<std::wstring_view, 12> kOfficeKeywords{
    L"\u529e\u516c", L"\u6587\u6863", L"\u6d4f\u89c8\u5668", L"word", L"powerpoint", L"excel", L"wps",
    L"zotero", L"obsidian", L"typora", L"office", L"draw.io" };
inline constexpr std::array<std::wstring_view, 12> kCommunicationKeywords{
    L"\u901a\u8baf", L"\u901a\u4fe1", L"\u4f1a\u8bae", L"\u90ae\u7bb1", L"\u5fae\u4fe1", L"\u98de\u4e66", L"qq", L"mail",
    L"meeting", L"chat", L"message", L"communication" };
inline constexpr std::array<std::wstring_view, 12> kMediaKeywords{
    L"\u5f71\u97f3", L"\u5a92\u4f53", L"\u97f3\u4e50", L"\u89c6\u9891", L"\u6296\u97f3", L"\u54d4\u54e9", L"music", L"media",
    L"video", L"player", L"potplayer", L"bilibili" };
inline constexpr std::array<std::wstring_view, 15> kEngineeringKeywords{
    L"\u5de5\u7a0b", L"\u4eff\u771f", L"solidworks", L"creo", L"caxa", L"cad", L"keyshot", L"matlab",
    L"workbench", L"fluent", L"cura", L"syslab", L"engineering", L"ansys", L"mworks" };
inline constexpr std::array<std::wstring_view, 11> kAiKeywords{
    L"\u4eba\u5de5\u667a\u80fd", L"\u667a\u80fd", L"ai", L"chatgpt", L"claude", L"comfyui", L"lm studio",
    L"opencode", L"inference", L"model", L"cherry studio" };
inline constexpr std::array<std::wstring_view, 13> kGamingKeywords{
    L"\u6e38\u620f", L"\u52a0\u901f\u5668", L"steam", L"epic", L"minecraft", L"pcl", L"cyberpunk",
    L"cities skylines", L"paradox", L"kook", L"game", L"gaming", L"black myth" };
inline constexpr std::array<std::wstring_view, 13> kOperationsKeywords{
    L"\u8fd0\u7ef4", L"\u670d\u52a1\u5668", L"\u8fdc\u7a0b", L"docker", L"winscp", L"netlimiter", L"reqable",
    L"server", L"operations", L"devops", L"ssh", L"terminal", L"package manager" };
inline constexpr std::array<std::wstring_view, 11> kWebKeywords{
    L"\u7f51\u9875", L"\u6d4f\u89c8\u5668", L"\u4e92\u8054\u7f51", L"edge", L"chrome", L"browser", L"web",
    L"internet", L"website", L"cloud", L"online" };

inline constexpr std::array<Category, 12> kCategories{
    Category{ L"files", "app.demo_category.files", kFilesVisuals, kFilesTitles, kFilesKeywords },
    Category{ L"utilities", "app.demo_category.utilities", kUtilitiesVisuals, kUtilitiesTitles, kUtilitiesKeywords },
    Category{ L"creative", "app.demo_category.creative", kCreativeVisuals, kCreativeTitles, kCreativeKeywords },
    Category{ L"development", "app.demo_category.development", kDevelopmentVisuals, kDevelopmentTitles, kDevelopmentKeywords },
    Category{ L"office", "app.demo_category.office", kOfficeVisuals, kOfficeTitles, kOfficeKeywords },
    Category{ L"communication", "app.demo_category.communication", kCommunicationVisuals, kCommunicationTitles, kCommunicationKeywords },
    Category{ L"media", "app.demo_category.media", kMediaVisuals, kMediaTitles, kMediaKeywords },
    Category{ L"engineering", "app.demo_category.engineering", kEngineeringVisuals, kEngineeringTitles, kEngineeringKeywords },
    Category{ L"ai", "app.demo_category.ai", kAiVisuals, kAiTitles, kAiKeywords },
    Category{ L"gaming", "app.demo_category.gaming", kGamingVisuals, kGamingTitles, kGamingKeywords },
    Category{ L"operations", "app.demo_category.operations", kOperationsVisuals, kOperationsTitles, kOperationsKeywords },
    Category{ L"web", "app.demo_category.web", kWebVisuals, kWebTitles, kWebKeywords },
};

inline bool EqualsAsciiInsensitive(
    std::wstring_view left, std::wstring_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (std::towlower(left[index]) != std::towlower(right[index]))
            return false;
    }
    return true;
}

inline bool ContainsInsensitive(
    std::wstring_view text, std::wstring_view keyword) noexcept
{
    if (keyword.empty() || keyword.size() > text.size()) return false;
    for (std::size_t start = 0; start + keyword.size() <= text.size(); ++start)
    {
        if (EqualsAsciiInsensitive(text.substr(start, keyword.size()), keyword))
            return true;
    }
    return false;
}

inline const Category* FindCategory(std::wstring_view id) noexcept
{
    for (const auto& category : kCategories)
        if (EqualsAsciiInsensitive(category.id, id))
            return &category;
    return nullptr;
}

inline const Category& InferCategory(
    std::wstring_view title,
    std::span<const std::wstring> itemKeys) noexcept
{
    const Category* best = &kCategories.front();
    int bestScore = 0;
    for (const auto& category : kCategories)
    {
        int score = 0;
        for (const auto keyword : category.keywords)
        {
            if (ContainsInsensitive(title, keyword)) score += 8;
            for (const auto& itemKey : itemKeys)
                if (ContainsInsensitive(itemKey, keyword)) ++score;
        }
        if (score > bestScore)
        {
            best = &category;
            bestScore = score;
        }
    }
    return *best;
}

inline const Category& ResolveCategory(
    std::wstring_view binding,
    std::wstring_view title,
    std::span<const std::wstring> itemKeys) noexcept
{
    if (const Category* explicitCategory = FindCategory(binding))
        return *explicitCategory;
    return InferCategory(title, itemKeys);
}

inline std::size_t VisualIndex(
    const Category& category, std::wstring_view identity) noexcept
{
    if (category.visualIndices.empty())
        return demo_mode_rules::VisualIdentityIndex(identity);
    return category.visualIndices[
        demo_mode_rules::StableIdentityHash(identity) %
        category.visualIndices.size()];
}

inline IdentityPresentation PresentationForSlot(
    const Category& category, std::size_t slot,
    std::size_t subjectOffset = 0) noexcept
{
    const std::size_t titleCount = category.identityTitles.empty()
        ? 1 : category.identityTitles.size();
    slot %= titleCount;
    const std::size_t visualCount = category.visualIndices.empty()
        ? demo_mode_rules::kVisualIdentities.size()
        : category.visualIndices.size();
    const std::size_t subjectSlot = slot + subjectOffset;
    const std::size_t visualIndex = category.visualIndices.empty()
        ? subjectSlot % visualCount
        : category.visualIndices[subjectSlot % visualCount];
    return {
        visualIndex,
        (subjectSlot / visualCount) % kIdentityVariantCount,
        category.identityTitles.empty()
            ? demo_mode_rules::VisualIdentityAt(visualIndex).title
            : category.identityTitles[slot]
    };
}

inline std::size_t ExposedItemCount(
    std::size_t itemCount, int columns, int rows) noexcept
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns <= 1 && rows <= 1)
        return std::min<std::size_t>(itemCount, 4);

    const std::size_t inlineCapacity =
        static_cast<std::size_t>(columns * rows - 1);
    if (itemCount <= inlineCapacity)
        return itemCount;
    return std::min(itemCount, inlineCapacity + 4);
}

inline std::size_t ItemOrdinal(
    std::span<const std::wstring> itemKeys,
    std::wstring_view identity) noexcept
{
    for (std::size_t index = 0; index < itemKeys.size(); ++index)
        if (EqualsAsciiInsensitive(itemKeys[index], identity))
            return index;
    return static_cast<std::size_t>(-1);
}

} // namespace snowdesktop::demo_collection_rules
