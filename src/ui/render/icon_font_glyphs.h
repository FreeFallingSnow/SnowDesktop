#pragma once

#include <dwrite_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::icon_fonts
{
enum class BundledFont
{
    FluentRegular,
    FontAwesomeSolid,
};

// Read the same file used by the XAML FontFamily URI. A family-name lookup
// can silently select a system font in the independent settings process.
// Missing or invalid assets must produce an empty list, never fallback glyphs.
inline std::vector<std::uint32_t> PrivateUseCodepoints(
    const std::filesystem::path& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
        return {};

    ComPtr<IDWriteFontFile> file;
    if (FAILED(factory->CreateFontFileReference(path.c_str(), nullptr, &file)))
        return {};
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType{};
    DWRITE_FONT_FACE_TYPE faceType{};
    UINT32 faceCount = 0;
    if (FAILED(file->Analyze(&supported, &fileType, &faceType, &faceCount)) ||
        !supported || faceCount != 1)
        return {};

    IDWriteFontFile* files[]{file.Get()};
    ComPtr<IDWriteFontFace> face;
    if (FAILED(factory->CreateFontFace(faceType, 1, files, 0,
            DWRITE_FONT_SIMULATIONS_NONE, &face)))
        return {};
    ComPtr<IDWriteFontFace1> unicodeFace;
    if (FAILED(face.As(&unicodeFace)))
        return {};

    UINT32 rangeCount = 0;
    const HRESULT counted = unicodeFace->GetUnicodeRanges(0, nullptr, &rangeCount);
    if ((FAILED(counted) && counted != E_NOT_SUFFICIENT_BUFFER) || !rangeCount)
        return {};
    std::vector<DWRITE_UNICODE_RANGE> ranges(rangeCount);
    if (FAILED(unicodeFace->GetUnicodeRanges(rangeCount, ranges.data(), &rangeCount)))
        return {};
    ranges.resize(rangeCount);

    constexpr std::array<DWRITE_UNICODE_RANGE, 3> privateUse{{
        {0xE000, 0xF8FF}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD}}};
    std::vector<std::uint32_t> codepoints;
    for (const auto& range : ranges)
    {
        for (const auto& area : privateUse)
        {
            const UINT32 first = (std::max)(range.first, area.first);
            const UINT32 last = (std::min)(range.last, area.last);
            for (UINT32 codepoint = first; codepoint <= last; ++codepoint)
                codepoints.push_back(codepoint);
        }
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

// UI callers select a fixed resource; file paths and font-file IO stay in
// the rendering service rather than the settings presenter.
inline std::vector<std::uint32_t> BundledPrivateUseCodepoints(
    std::wstring_view executableDirectory, BundledFont font)
{
    const wchar_t* filename = nullptr;
    switch (font)
    {
    case BundledFont::FluentRegular:
        filename = L"FluentSystemIcons-Regular.ttf";
        break;
    case BundledFont::FontAwesomeSolid:
        filename = L"fa-solid-900.ttf";
        break;
    default:
        return {};
    }
    return PrivateUseCodepoints(
        std::filesystem::path(std::wstring(executableDirectory)) /
        L"Assets" / L"Fonts" / filename);
}
} // namespace snowdesktop::icon_fonts
