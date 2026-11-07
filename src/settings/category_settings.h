#pragma once

#include "category_settings_types.h"
#include "layout/category_collection_rules.h"

std::wstring GetCategorySettingsPath();
bool LoadCategorySettings(const wchar_t* path, CategorySettings& settings);
bool SaveCategorySettings(const wchar_t* path, const CategorySettings& settings);
void NormalizeCategorySettings(CategorySettings& settings);
bool IsBuiltinCategoryRuleId(const std::wstring& categoryId);
bool IsCategoryRuleEnabled(const CategorySettings& settings, const std::wstring& categoryId);
std::vector<std::wstring> ParseCategoryExtensionList(const std::wstring& text);
std::wstring NormalizeCategoryExtensionText(const std::wstring& text);
std::vector<std::wstring> GetCategoryOrder(const CategorySettings& settings);
std::wstring GetCategoryLabel(const CategorySettings& settings, const std::wstring& categoryId);
std::wstring CategoryIdForExtension(const CategorySettings& settings, const std::wstring& extensionUpper);
std::wstring CategoryIdForItemType(const CategorySettings& settings, const std::wstring& extensionUpper,
    const snowdesktop::category_collection_rules::ShortcutTarget& target);
std::vector<std::wstring> GetProgramCategoryExtensions(const CategorySettings& settings);
