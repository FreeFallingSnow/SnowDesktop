#include "widget_draw_context.h"

#include "common/utils.h"
#include "ui/render/app_font.h"

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

WidgetDrawContext::WidgetDrawContext() = default;
WidgetDrawContext::~WidgetDrawContext() = default;

std::uint32_t DecodeIconGlyph(std::string_view glyph) noexcept
{
    if (glyph.empty() || glyph.size() > 4) return 0;
    wchar_t utf16[2]{};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        glyph.data(), static_cast<int>(glyph.size()), utf16, 2);
    if (count == 1) return utf16[0];
    if (count != 2 || utf16[0] < 0xD800 || utf16[0] > 0xDBFF ||
        utf16[1] < 0xDC00 || utf16[1] > 0xDFFF)
        return 0;
    return 0x10000u + (std::uint32_t(utf16[0] - 0xD800) << 10) +
        std::uint32_t(utf16[1] - 0xDC00);
}

bool WidgetDrawContext::DrawCenteredGlyph(std::uint32_t codepoint,
    bool fluent, float cx, float cy, float size, ID2D1Brush& brush,
    D2D1_RECT_F* paintedBounds)
{
    if (!ctx || !dwrite || codepoint == 0 || size <= 0.0f) return false;
    ComPtr<ID2D1Factory> factory;
    ctx->GetFactory(&factory);
    if (!factory) return false;
    if (factory.Get() != glyphFactory_.Get())
    {
        glyphOutlines_.clear();
        glyphFactory_ = factory;
    }
    const auto key = codepoint | (fluent ? 0x200000u : 0u);
    if (glyphOutlines_.size() >= 512 && !glyphOutlines_.contains(key))
        glyphOutlines_.clear();
    const auto [entry, inserted] = glyphOutlines_.try_emplace(key);
    auto& outline = entry->second;
    if (inserted)
    {
        auto& face = fluent ? fluentFace_ : fontAwesomeFace_;
        if (!face)
        {
            auto* format = TextFormat(20.0f, DWRITE_FONT_WEIGHT_NORMAL,
                false, DWRITE_WORD_WRAPPING_NO_WRAP, !fluent, false, fluent, nullptr);
            if (!format) return false;
            std::wstring name(format->GetFontFamilyNameLength() + 1, L'\0');
            ComPtr<IDWriteFontCollection> collection;
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteFont> font;
            UINT32 familyIndex = 0;
            BOOL found = FALSE;
            if (FAILED(format->GetFontFamilyName(name.data(), static_cast<UINT32>(name.size()))) ||
                FAILED(format->GetFontCollection(&collection)) || !collection ||
                FAILED(collection->FindFamilyName(name.c_str(), &familyIndex, &found)) || !found ||
                FAILED(collection->GetFontFamily(familyIndex, &family)) ||
                FAILED(family->GetFirstMatchingFont(format->GetFontWeight(),
                    format->GetFontStretch(), format->GetFontStyle(), &font)) ||
                FAILED(font->CreateFontFace(&face)))
                return false;
        }
        UINT16 glyph = 0;
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        D2D1_RECT_F ink{};
        if (FAILED(face->GetGlyphIndices(&codepoint, 1, &glyph)) || glyph == 0 ||
            FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink)) ||
            FAILED(face->GetGlyphRunOutline(1.0f, &glyph, nullptr, nullptr, 1,
                FALSE, FALSE, sink.Get())) || FAILED(sink->Close()) ||
            FAILED(geometry->GetBounds(nullptr, &ink)) ||
            ink.right <= ink.left || ink.bottom <= ink.top)
            return false;
        outline.geometry = std::move(geometry);
        outline.ink = ink;
    }
    if (!outline.geometry) return false;
    const auto& ink = outline.ink;
    const float halfWidth = (ink.right - ink.left) * size * 0.5f;
    const float halfHeight = (ink.bottom - ink.top) * size * 0.5f;
    const float x = cx - halfWidth;
    const float y = cy - halfHeight;
    D2D1_MATRIX_3X2_F transform{};
    ctx->GetTransform(&transform);
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(size, size) *
        D2D1::Matrix3x2F::Translation(x - ink.left * size, y - ink.top * size) * transform);
    ctx->FillGeometry(outline.geometry.Get(), &brush);
    ctx->SetTransform(transform);
    if (paintedBounds)
        *paintedBounds = D2D1::RectF(x, y, cx + halfWidth, cy + halfHeight);
    return true;
}

WidgetDrawContext::PrivateFontResource* WidgetDrawContext::LoadPrivateFont(
    const std::wstring& path)
{
    if (!dwrite || path.empty()) return nullptr;
    if (auto found = privateFonts_.find(path);
        found != privateFonts_.end())
        return &found->second;
    ComPtr<IDWriteFactory3> factory;
    if (FAILED(dwrite->QueryInterface(IID_PPV_ARGS(&factory))) ||
        !factory)
        return nullptr;
    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(dwrite->CreateFontFileReference(
            path.c_str(), nullptr, &fontFile)) || !fontFile)
        return nullptr;
    ComPtr<IDWriteFontSetBuilder> baseBuilder;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory->CreateFontSetBuilder(&baseBuilder)) || !baseBuilder ||
        FAILED(baseBuilder.As(&builder)) || !builder ||
        FAILED(builder->AddFontFile(fontFile.Get())))
        return nullptr;
    ComPtr<IDWriteFontSet> fontSet;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(baseBuilder->CreateFontSet(&fontSet)) || !fontSet ||
        FAILED(factory->CreateFontCollectionFromFontSet(
            fontSet.Get(), &collection)) || !collection ||
        collection->GetFontFamilyCount() == 0)
        return nullptr;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(collection->GetFontFamily(0, &family)) || !family ||
        FAILED(family->GetFamilyNames(&names)) || !names ||
        names->GetCount() == 0)
        return nullptr;
    UINT32 nameIndex = 0;
    BOOL exists = FALSE;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
        names->FindLocaleName(locale, &nameIndex, &exists);
    if (!exists)
        names->FindLocaleName(L"en-US", &nameIndex, &exists);
    if (!exists) nameIndex = 0;
    UINT32 nameLength = 0;
    if (FAILED(names->GetStringLength(nameIndex, &nameLength)) ||
        nameLength == 0 || nameLength > 255)
        return nullptr;
    std::wstring familyName(nameLength + 1, L'\0');
    if (FAILED(names->GetString(nameIndex, familyName.data(),
            nameLength + 1)))
        return nullptr;
    familyName.resize(nameLength);
    auto [inserted, added] = privateFonts_.emplace(path,
        PrivateFontResource{ std::move(collection), std::move(familyName) });
    return added ? &inserted->second : nullptr;
}

ID2D1SolidColorBrush* WidgetDrawContext::Brush(int color, float alpha)
{
    if (!ctx) return nullptr;
    if (brushContext_ != ctx)
    {
        brushes_.clear();
        brushContext_ = ctx;
    }

    const auto alphaByte = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(std::lround(alpha * 255.0f)), 0, 255));
    const std::uint32_t key =
        (static_cast<std::uint32_t>(color) & 0x00FFFFFFu) | (alphaByte << 24);
    if (auto found = brushes_.find(key); found != brushes_.end())
        return found->second.Get();

    if (brushes_.size() >= 512)
        brushes_.clear();

    ComPtr<ID2D1SolidColorBrush> brush;
    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >> 8) & 0xFF) / 255.0f;
    const float b = (color & 0xFF) / 255.0f;
    if (FAILED(ctx->CreateSolidColorBrush(
        D2D1::ColorF(r, g, b, alphaByte / 255.0f), &brush)) || !brush)
        return nullptr;
    return brushes_.emplace(key, std::move(brush)).first->second.Get();
}

IDWriteTextFormat* WidgetDrawContext::TextFormat(float size,
    DWRITE_FONT_WEIGHT weight, bool centered, DWRITE_WORD_WRAPPING wrapping,
    bool fontAwesome, bool verticallyCentered, bool fluent,
    const std::wstring* privateFontPath)
{
    if (!dwrite) return nullptr;
    const auto sizeKey = static_cast<std::uint64_t>(std::clamp(
        static_cast<int>(std::lround(size * 100.0f)), 1, 0xFFFFFF));
    const std::uint64_t key = sizeKey |
        (static_cast<std::uint64_t>(weight) << 24) |
        (static_cast<std::uint64_t>(centered) << 36) |
        (static_cast<std::uint64_t>(wrapping) << 37) |
        (static_cast<std::uint64_t>(fontAwesome) << 40) |
        (static_cast<std::uint64_t>(verticallyCentered) << 41) |
        (static_cast<std::uint64_t>(fluent) << 42);
    if (privateFontPath)
    {
        const std::wstring privateKey = *privateFontPath + L"#" +
            std::to_wstring(key);
        if (auto found = privateTextFormats_.find(privateKey);
            found != privateTextFormats_.end())
            return found->second.Get();
        PrivateFontResource* resource = LoadPrivateFont(*privateFontPath);
        if (!resource) return nullptr;
        ComPtr<IDWriteTextFormat> format;
        dwrite->CreateTextFormat(resource->familyName.c_str(),
            resource->collection.Get(), weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (!format) return nullptr;
        format->SetTextAlignment(centered
            ? DWRITE_TEXT_ALIGNMENT_CENTER
            : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(centered || verticallyCentered
            ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
            : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(wrapping);
        if (privateTextFormats_.size() >= 128)
            privateTextFormats_.clear();
        return privateTextFormats_.emplace(
            privateKey, std::move(format)).first->second.Get();
    }
    if (auto found = textFormats_.find(key);
        found != textFormats_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    if (fluent)
    {
        format.Attach(CreateFluentTextFormat(dwrite, size));
    }
    else if (fontAwesome)
    {
        format.Attach(CreateFaTextFormat(dwrite, size));
    }
    else
    {
        snowdesktop::app_fonts::CreateTextFormat(dwrite, L"Segoe UI", weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    }
    if (!format) return nullptr;
    format->SetTextAlignment(centered
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(centered || verticallyCentered
        ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
        : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(wrapping);
    if (textFormats_.size() >= 128)
        textFormats_.clear();
    return textFormats_.emplace(key, std::move(format)).first->second.Get();
}

bool WidgetDrawContext::PreloadPrivateFont(const std::wstring& path)
{
    return LoadPrivateFont(path) != nullptr;
}

bool WidgetDrawContext::HasPrivateFont(const std::wstring& path) const
{
    return privateFonts_.contains(path);
}

void WidgetDrawContext::ResetBrushes()
{
    brushes_.clear();
    brushContext_ = nullptr;
}

void WidgetDrawContext::ClearPrivateTextFormats()
{
    privateTextFormats_.clear();
}

WidgetDrawContext::Counts WidgetDrawContext::ResourceCounts() const noexcept
{
    return { brushes_.size(), textFormats_.size(), privateTextFormats_.size(), privateFonts_.size() };
}

ID2D1SolidColorBrush* GetCachedBrush(WidgetDrawContext* state, int color, float alpha)
{
    return state ? state->Brush(color, alpha) : nullptr;
}

bool GetTextLayoutInkBounds(IDWriteTextLayout& layout, D2D1_RECT_F& bounds) noexcept
{
    DWRITE_OVERHANG_METRICS overhang{};
    if (FAILED(layout.GetOverhangMetrics(&overhang))) return false;
    bounds = { -overhang.left, -overhang.top,
        layout.GetMaxWidth() + overhang.right,
        layout.GetMaxHeight() + overhang.bottom };
    return true;
}

IDWriteTextFormat* GetCachedTextFormat(WidgetDrawContext* state, float size,
    DWRITE_FONT_WEIGHT weight, bool centered, DWRITE_WORD_WRAPPING wrapping,
    bool fontAwesome, bool verticallyCentered, bool fluent, const std::wstring* privateFontPath)
{
    return state ? state->TextFormat(size, weight, centered, wrapping,
        fontAwesome, verticallyCentered, fluent, privateFontPath) : nullptr;
}

std::string_view CurrentWidgetSurface(const WidgetDrawContext* state) noexcept
{
    return state && state->surfaceKind && *state->surfaceKind
        ? std::string_view(state->surfaceKind)
        : std::string_view("desktop");
}

void DrawHostRect(WidgetDrawContext* state, float x, float y, float width, float height,
    int color, float radius, float alpha)
{
    if (!state || !state->ctx) return;
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color, alpha);
    if (!brush) return;
    D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + x, state->widgetRect.top + y,
        state->widgetRect.left + x + width, state->widgetRect.top + y + height);
    if (radius > 0)
        state->ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    else
        state->ctx->FillRectangle(rect, brush);
}

void DrawHostText(WidgetDrawContext* state, const std::wstring& text,
    float x, float y, float width, float height, float size, int color)
{
    if (!state || !state->ctx || !state->dwrite) return;
    const float scale = CalculateWidgetCellScale(state->gridCellW, state->gridCellH);
    const float scaledSize = std::max(9.0f, size * scale);
    IDWriteTextFormat* format = GetCachedTextFormat(state, scaledSize,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, true, DWRITE_WORD_WRAPPING_NO_WRAP);
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color);
    if (!format || !brush) return;
    state->ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        D2D1::RectF(state->widgetRect.left + x, state->widgetRect.top + y,
            state->widgetRect.left + x + width, state->widgetRect.top + y + height),
        brush);
}

void DrawHostStrokeRect(WidgetDrawContext* state, float x, float y, float width, float height,
    int color, float radius, float thickness, float alpha)
{
    if (!state || !state->ctx) return;
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color, alpha);
    if (!brush) return;
    D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + x, state->widgetRect.top + y,
        state->widgetRect.left + x + width, state->widgetRect.top + y + height);
    if (radius > 0)
        state->ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, thickness);
    else
        state->ctx->DrawRectangle(rect, brush, thickness);
}

ComPtr<IDWriteTextLayout> CreateHostSingleLineTextLayout(
    WidgetDrawContext* state, const std::wstring& text, float size,
    float width, float height)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!state || !state->dwrite)
        return layout;
    IDWriteTextFormat* format = GetCachedTextFormat(state,
        std::max(9.0f, size), DWRITE_FONT_WEIGHT_NORMAL, false,
        DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    if (!format)
        return layout;
    const std::wstring layoutText = text.empty() ? L" " : text;
    if (FAILED(state->dwrite->CreateTextLayout(layoutText.c_str(),
        static_cast<UINT32>(layoutText.size()), format,
        std::max(1.0f, width), std::max(1.0f, height),
        &layout)))
        layout.Reset();
    return layout;
}

ComPtr<IDWriteTextLayout> CreateHostMultilineTextLayout(
    WidgetDrawContext* state, const std::wstring& text, float size, float width)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!state || !state->dwrite)
        return layout;
    IDWriteTextFormat* format = GetCachedTextFormat(state,
        std::max(9.0f, size), DWRITE_FONT_WEIGHT_NORMAL, false,
        DWRITE_WORD_WRAPPING_WRAP, false, false);
    if (!format)
        return layout;
    // DirectWrite does not expose an empty final line for a trailing
    // newline. Keep a layout-only space there so the caret can occupy it.
    std::wstring layoutText = text;
    if (layoutText.empty() ||
        layoutText.back() == L'\n' ||
        layoutText.back() == L'\r')
        layoutText.push_back(L' ');
    if (FAILED(state->dwrite->CreateTextLayout(layoutText.c_str(),
        static_cast<UINT32>(layoutText.size()), format,
        std::max(1.0f, width), 100000.0f, &layout)))
        layout.Reset();
    return layout;
}

void DrawHostTextSelection(WidgetDrawContext* state,
    IDWriteTextLayout* textLayout, size_t selectionStart,
    size_t selectionEnd, float originX, float originY,
    int color)
{
    if (!state || !textLayout ||
        selectionStart >= selectionEnd ||
        selectionStart >
            static_cast<size_t>(std::numeric_limits<UINT32>::max()))
        return;
    const UINT32 start = static_cast<UINT32>(selectionStart);
    const size_t boundedLength = std::min(
        selectionEnd - selectionStart,
        static_cast<size_t>(
            std::numeric_limits<UINT32>::max() - start));
    if (boundedLength == 0)
        return;

    UINT32 count = 0;
    textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        nullptr, 0, &count);
    if (count == 0)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        metrics.data(), count, &count)))
        return;
    // DirectWrite may return one metric per glyph/run. Drawing every metric
    // independently makes adjacent selection pieces overlap, especially for
    // proportional text, and produces visible rounded blue boxes while the
    // selection is being dragged. Merge contiguous pieces on each line and
    // draw one flat highlight per line so the selection remains stable.
    std::vector<D2D1_RECT_F> selectionRects;
    selectionRects.reserve(count);
    for (UINT32 i = 0; i < count; ++i)
    {
        const auto& hit = metrics[i];
        const D2D1_RECT_F rect = D2D1::RectF(
            hit.left, hit.top,
            hit.left + std::max(1.5f, hit.width),
            hit.top + std::max(1.0f, hit.height));
        if (!selectionRects.empty())
        {
            auto& previous = selectionRects.back();
            const bool sameLine =
                std::abs(previous.top - rect.top) < 0.5f &&
                std::abs(previous.bottom - rect.bottom) < 0.5f;
            const bool contiguous = rect.left <= previous.right + 0.5f;
            if (sameLine && contiguous)
            {
                previous.left = std::min(previous.left, rect.left);
                previous.right = std::max(previous.right, rect.right);
                previous.top = std::min(previous.top, rect.top);
                previous.bottom = std::max(previous.bottom, rect.bottom);
                continue;
            }
        }
        selectionRects.push_back(rect);
    }

    for (const auto& rect : selectionRects)
    {
        DrawHostRect(state, originX + rect.left,
            originY + rect.top, rect.right - rect.left,
            rect.bottom - rect.top, color, 0.0f, 0.32f);
    }
}

HostInputDisplayText BuildHostInputDisplayText(
    const std::wstring& text, size_t cursor,
    size_t selectionAnchor, const std::wstring& compositionText,
    size_t compositionCursor)
{
    HostInputDisplayText result;
    result.text = text;
    result.cursor = std::min(cursor, result.text.size());
    if (compositionText.empty())
        return result;

    const size_t anchor = std::min(
        selectionAnchor, result.text.size());
    const size_t selectionStart = std::min(result.cursor, anchor);
    const size_t selectionEnd = std::max(result.cursor, anchor);
    result.text.erase(
        selectionStart, selectionEnd - selectionStart);
    result.text.insert(selectionStart, compositionText);
    result.compositionStart = selectionStart;
    result.compositionLength = compositionText.size();
    result.cursor = selectionStart + std::min(
        compositionCursor, compositionText.size());
    return result;
}

void DrawHostCompositionUnderline(WidgetDrawContext* state,
    IDWriteTextLayout* textLayout, size_t compositionStart,
    size_t compositionLength, float originX, float originY,
    int color)
{
    if (!state || !textLayout || compositionLength == 0 ||
        compositionStart >
            static_cast<size_t>(std::numeric_limits<UINT32>::max()))
        return;
    const UINT32 start =
        static_cast<UINT32>(compositionStart);
    const size_t boundedLength = std::min(
        compositionLength,
        static_cast<size_t>(
            std::numeric_limits<UINT32>::max() - start));
    if (boundedLength == 0)
        return;

    UINT32 count = 0;
    textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        nullptr, 0, &count);
    if (count == 0)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(textLayout->HitTestTextRange(start,
            static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
            metrics.data(), count, &count)))
        return;
    for (UINT32 i = 0; i < count; ++i)
    {
        const auto& hit = metrics[i];
        DrawHostRect(state, originX + hit.left,
            originY + hit.top + std::max(1.0f, hit.height - 1.5f),
            std::max(1.5f, hit.width), 1.5f,
            color, 0.0f, 0.92f);
    }
}

void DrawSimulatedPreviewIcon(WidgetDrawContext* state,
    const std::wstring& identity, const std::wstring& title,
    float x, float y, float size, float alpha)
{
    if (!state || !state->ctx || size <= 0.0f) return;
    static constexpr int colors[] = {
        0x3B82F6, 0x8B5CF6, 0xEC4899, 0xF97316,
        0x14B8A6, 0x22C55E, 0xEAB308, 0x6366F1,
    };
    std::uint32_t hash = 2166136261u;
    for (wchar_t value : identity)
    {
        hash ^= static_cast<std::uint32_t>(value);
        hash *= 16777619u;
    }
    const int background = colors[hash % std::size(colors)];
    ID2D1SolidColorBrush* fill = GetCachedBrush(
        state, background, std::clamp(alpha, 0.0f, 1.0f));
    ID2D1SolidColorBrush* border = GetCachedBrush(
        state, 0xFFFFFF, std::clamp(alpha * 0.42f, 0.0f, 1.0f));
    ID2D1SolidColorBrush* text = GetCachedBrush(
        state, 0xFFFFFF, std::clamp(alpha, 0.0f, 1.0f));
    const D2D1_RECT_F rect = D2D1::RectF(
        x + state->widgetRect.left, y + state->widgetRect.top,
        x + state->widgetRect.left + size,
        y + state->widgetRect.top + size);
    const float radius = std::max(3.0f, size * 0.22f);
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(rect, radius, radius);
    if (fill) state->ctx->FillRoundedRectangle(rounded, fill);
    if (border)
        state->ctx->DrawRoundedRectangle(
            rounded, border, std::max(1.0f, size / 24.0f));

    std::wstring letter;
    for (wchar_t value : title)
    {
        if (value != L' ' && value != L'\t' &&
            value != L'\r' && value != L'\n')
        {
            letter.assign(1, value);
            break;
        }
    }
    if (letter.empty()) letter = L"?";
    CharUpperBuffW(letter.data(), static_cast<DWORD>(letter.size()));
    IDWriteTextFormat* format = GetCachedTextFormat(
        state, std::max(11.0f, size * 0.46f),
        DWRITE_FONT_WEIGHT_BOLD, true,
        DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    if (format && text)
    {
        state->ctx->DrawTextW(letter.c_str(),
            static_cast<UINT32>(letter.size()), format,
            rect, text, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void PopWidgetClips(WidgetDrawContext& draw) noexcept
{
    while (draw.widgetClipDepth > 0)
    {
        draw.ctx->PopAxisAlignedClip();
        --draw.widgetClipDepth;
    }
}

WidgetCommandCapture::~WidgetCommandCapture()
{
    if (draw_) Abort();
}

WidgetCommandCapture::BeginResult WidgetCommandCapture::Begin(
    WidgetDrawContext& draw, std::vector<D2D1_RECT_F>& clipRects, ID2D1Device* device,
    Microsoft::WRL::ComPtr<ID2D1DeviceContext>& recordingContext)
{
    if (!draw.ctx || !device) return BeginResult::NoDevice;
    if (!recordingContext && FAILED(device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recordingContext)))
        return BeginResult::NoRecordingContext;
    if (!recordingContext ||
        FAILED(recordingContext->CreateCommandList(&commands_)) || !commands_)
        return BeginResult::NoCommandList;

    display_ = draw.ctx;
    recording_ = recordingContext;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    display_->GetDpi(&dpiX, &dpiY);
    recording_->SetDpi(dpiX, dpiY);
    recording_->SetUnitMode(display_->GetUnitMode());
    recording_->SetAntialiasMode(display_->GetAntialiasMode());
    recording_->SetTextAntialiasMode(display_->GetTextAntialiasMode());
    recording_->SetPrimitiveBlend(display_->GetPrimitiveBlend());
    recording_->SetTransform(D2D1::Matrix3x2F::Identity());
    recording_->SetTarget(commands_.Get());
    recording_->BeginDraw();
    draw.ctx = recording_.Get();
    draw.widgetClipDepth = 0;
    clipRects.clear();
    clipRects_ = &clipRects;
    draw_ = &draw;
    return BeginResult::Started;
}

HRESULT WidgetCommandCapture::FinishDraw() noexcept
{
    if (!draw_) return E_UNEXPECTED;
    PopWidgetClips(*draw_);
    const HRESULT result = recording_->EndDraw();
    recording_->SetTarget(nullptr);
    draw_->ctx = display_;
    clipRects_->clear();
    draw_ = nullptr;
    return result;
}

HRESULT WidgetCommandCapture::Close() noexcept
{
    return commands_ ? commands_->Close() : E_UNEXPECTED;
}

void WidgetCommandCapture::Abort() noexcept
{
    (void)FinishDraw();
    (void)Close();
}

Microsoft::WRL::ComPtr<ID2D1CommandList> WidgetCommandCapture::TakeCommands() noexcept
{
    return std::move(commands_);
}
}
