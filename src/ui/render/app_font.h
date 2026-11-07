#pragma once

#include "app_font_types.h"

#include <dwrite_3.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <string_view>
#include <utility>

namespace snowdesktop::app_fonts
{
struct Resource
{
    Selection selection;
    std::wstring family;
    std::wstring xamlSource;
    // The isolated factory owns the local font loader. Retain it until all
    // selected glyphs have been consumed, including the startup draw thread.
    Microsoft::WRL::ComPtr<IDWriteFactory6> factory;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
};

extern std::atomic<std::shared_ptr<const Resource>> current;
extern std::atomic<std::uint64_t> revision;

// Text formats retain the collection. Explicit Lua font paths and all icon
// families bypass this helper and keep their original lookup behavior.
HRESULT CreateTextFormat(IDWriteFactory* factory, const wchar_t* systemFamily,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
    float size, const wchar_t* locale, IDWriteTextFormat** format);

void SetWeight(IDWriteTextLayout* layout, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_RANGE range);

template <typename Factory>
inline HRESULT CreateTextFormat(const Microsoft::WRL::ComPtr<Factory>& factory,
    const wchar_t* systemFamily, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
    DWRITE_FONT_STRETCH stretch, float size, const wchar_t* locale, IDWriteTextFormat** format)
{
    return CreateTextFormat(factory.Get(), systemFamily, weight, style, stretch, size, locale, format);
}

std::wstring GdiFamily(const wchar_t* systemFamily = L"Segoe UI");

std::wstring XamlFamily();

std::vector<Choice> List(const std::filesystem::path& assets, const std::filesystem::path& data);
bool Select(const Selection& selection, const std::filesystem::path& assets,
    const std::filesystem::path& data);
bool Import(const std::filesystem::path& source, const std::filesystem::path& data,
    std::vector<Choice>& choices, std::string& error);
std::filesystem::path ResolveXamlResource(std::wstring_view name,
    const std::filesystem::path& data);
} // namespace snowdesktop::app_fonts
