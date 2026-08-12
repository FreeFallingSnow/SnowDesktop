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
    std::span<const std::wstring_view> keywords;
};

inline constexpr std::array<std::size_t, 4> kFilesVisuals{
    16, 28, 29, 52 };
inline constexpr std::array<std::size_t, 4> kUtilitiesVisuals{
    27, 30, 31, 53 };
inline constexpr std::array<std::size_t, 4> kCreativeVisuals{
    17, 32, 33, 54 };
inline constexpr std::array<std::size_t, 4> kDevelopmentVisuals{
    25, 34, 35, 55 };
inline constexpr std::array<std::size_t, 4> kOfficeVisuals{
    18, 36, 37, 56 };
inline constexpr std::array<std::size_t, 4> kCommunicationVisuals{
    19, 38, 39, 57 };
inline constexpr std::array<std::size_t, 4> kMediaVisuals{
    20, 40, 41, 58 };
inline constexpr std::array<std::size_t, 4> kEngineeringVisuals{
    21, 42, 43, 59 };
inline constexpr std::array<std::size_t, 4> kAiVisuals{
    22, 44, 45, 60 };
inline constexpr std::array<std::size_t, 4> kGamingVisuals{
    23, 46, 47, 61 };
inline constexpr std::array<std::size_t, 4> kOperationsVisuals{
    24, 48, 49, 62 };
inline constexpr std::array<std::size_t, 4> kWebVisuals{
    26, 50, 51, 63 };

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
    Category{ L"files", "app.demo_category.files", kFilesVisuals, kFilesKeywords },
    Category{ L"utilities", "app.demo_category.utilities", kUtilitiesVisuals, kUtilitiesKeywords },
    Category{ L"creative", "app.demo_category.creative", kCreativeVisuals, kCreativeKeywords },
    Category{ L"development", "app.demo_category.development", kDevelopmentVisuals, kDevelopmentKeywords },
    Category{ L"office", "app.demo_category.office", kOfficeVisuals, kOfficeKeywords },
    Category{ L"communication", "app.demo_category.communication", kCommunicationVisuals, kCommunicationKeywords },
    Category{ L"media", "app.demo_category.media", kMediaVisuals, kMediaKeywords },
    Category{ L"engineering", "app.demo_category.engineering", kEngineeringVisuals, kEngineeringKeywords },
    Category{ L"ai", "app.demo_category.ai", kAiVisuals, kAiKeywords },
    Category{ L"gaming", "app.demo_category.gaming", kGamingVisuals, kGamingKeywords },
    Category{ L"operations", "app.demo_category.operations", kOperationsVisuals, kOperationsKeywords },
    Category{ L"web", "app.demo_category.web", kWebVisuals, kWebKeywords },
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

inline std::size_t VisualIndexForOrdinal(
    const Category& category, std::size_t ordinal) noexcept
{
    if (category.visualIndices.empty())
        return ordinal % demo_mode_rules::kVisualIdentities.size();
    return category.visualIndices[ordinal % category.visualIndices.size()];
}

inline std::size_t VisibleItemCount(
    const Category& category, std::size_t itemCount) noexcept
{
    return std::min(itemCount, category.visualIndices.size());
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
