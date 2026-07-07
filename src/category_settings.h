/**
 * @file category_settings.h
 * @brief 桌面文件分类设置
 */

#pragma once

#include <string>
#include <vector>

struct CategoryRule
{
    std::wstring id;
    std::wstring label;
    std::wstring extensions;
};

struct CategorySettings
{
    float tabFontSize = 14.0f;
    std::vector<CategoryRule> rules;

    static CategorySettings Defaults();
};

std::wstring GetCategorySettingsPath();
bool LoadCategorySettings(const wchar_t* path, CategorySettings& settings);
bool SaveCategorySettings(const wchar_t* path, const CategorySettings& settings);
std::vector<std::wstring> ParseCategoryExtensionList(const std::wstring& text);
std::wstring NormalizeCategoryExtensionText(const std::wstring& text);
std::vector<std::wstring> GetCategoryOrder(const CategorySettings& settings);
std::wstring GetCategoryLabel(const CategorySettings& settings, const std::wstring& categoryId);
std::wstring CategoryIdForExtension(const CategorySettings& settings, const std::wstring& extensionUpper);
