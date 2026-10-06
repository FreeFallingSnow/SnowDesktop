#pragma once

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
struct Selection
{
    std::string package = "system";
    std::string family;
    bool operator==(const Selection&) const = default;
};

struct Choice
{
    Selection selection;
    std::wstring name;
    std::wstring xamlSource;
};

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

inline std::atomic<std::shared_ptr<const Resource>> current;
inline std::atomic<std::uint64_t> revision{0};

// Text formats retain the collection. Explicit Lua font paths and all icon
// families bypass this helper and keep their original lookup behavior.
inline HRESULT CreateTextFormat(IDWriteFactory* factory, const wchar_t* systemFamily,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
    float size, const wchar_t* locale, IDWriteTextFormat** format)
{
    const auto resource = current.load();
    const HRESULT result = factory->CreateTextFormat(resource ? resource->family.c_str() : systemFamily,
        resource ? resource->collection.Get() : nullptr, weight, style, stretch, size, locale, format);
    if (SUCCEEDED(result) && format && *format)
    {
        Microsoft::WRL::ComPtr<IDWriteTextFormat3> variable;
        if (SUCCEEDED((*format)->QueryInterface(IID_PPV_ARGS(&variable))))
        {
            const DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<float>(weight)};
            variable->SetFontAxisValues(&axis, 1);
        }
    }
    return result;
}

inline void SetWeight(IDWriteTextLayout* layout, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_RANGE range)
{
    layout->SetFontWeight(weight, range);
    Microsoft::WRL::ComPtr<IDWriteTextLayout4> variable;
    if (SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(&variable))))
    {
        const DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<float>(weight)};
        variable->SetFontAxisValues(&axis, 1, range);
    }
}

template <typename Factory>
inline HRESULT CreateTextFormat(const Microsoft::WRL::ComPtr<Factory>& factory,
    const wchar_t* systemFamily, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
    DWRITE_FONT_STRETCH stretch, float size, const wchar_t* locale, IDWriteTextFormat** format)
{
    return CreateTextFormat(factory.Get(), systemFamily, weight, style, stretch, size, locale, format);
}

inline std::wstring GdiFamily(const wchar_t* systemFamily = L"Segoe UI")
{
    const auto resource = current.load();
    return resource ? resource->family : systemFamily;
}

inline std::wstring XamlFamily()
{
    const auto resource = current.load();
    return resource ? resource->xamlSource : L"Segoe UI Variable, Segoe UI";
}

std::vector<Choice> List(const std::filesystem::path& assets, const std::filesystem::path& data);
bool Select(const Selection& selection, const std::filesystem::path& assets,
    const std::filesystem::path& data);
bool Import(const std::filesystem::path& source, const std::filesystem::path& data,
    std::vector<Choice>& choices, std::string& error);
std::filesystem::path ResolveXamlResource(std::wstring_view name,
    const std::filesystem::path& data);
} // namespace snowdesktop::app_fonts
