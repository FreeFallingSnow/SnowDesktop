#include "widget_text_layout_cache.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_1.h>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace
{
using namespace snowdesktop::widget_runtime;
using Microsoft::WRL::ComPtr;
using Cache = WidgetTextLayoutCache;

void Check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "FAILED: text layout cache: " << message << '\n';
    std::exit(1);
}
void Ok(HRESULT result, const char* message)
{
    Check(SUCCEEDED(result), message);
}

struct Fixture
{
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextFormat> format;
    Fixture()
    {
        Ok(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), &factory), "create DirectWrite factory");
        format = Format(17);
    }
    ComPtr<IDWriteTextFormat> Format(float size)
    {
        ComPtr<IDWriteTextFormat> result;
        Ok(factory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &result), "create font");
        return result;
    }
    ComPtr<IDWriteTextLayout> Build(const Cache::Options& o,
        std::wstring_view text, std::wstring_view locale = L"en-US")
    {
        ComPtr<IDWriteTextLayout> result;
        Ok(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
            format.Get(), o.width, o.height, &result), "create text layout");
        const DWRITE_TEXT_RANGE range{ 0, static_cast<UINT32>(text.size()) };
        result->SetLocaleName(std::wstring(locale).c_str(), range);
        result->SetTextAlignment(o.alignment);
        result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        result->SetReadingDirection(o.direction == ViewTextDirection::RightToLeft
            ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
            : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        result->SetFontStyle(o.fontStyle == ViewFontStyle::Italic
            ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, range);
        result->SetUnderline(o.underline, range);
        if (o.lineHeight) result->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            *o.lineHeight, *o.lineHeight * 0.8f);
        if (o.letterSpacing != 0)
        {
            ComPtr<IDWriteTextLayout1> advanced;
            Ok(result.As(&advanced), "query letter spacing support");
            advanced->SetCharacterSpacing(o.letterSpacing / 2,
                o.letterSpacing / 2, 0, range);
        }
        if (o.overflow == ViewTextOverflow::Ellipsis)
        {
            ComPtr<IDWriteInlineObject> sign;
            Ok(factory->CreateEllipsisTrimmingSign(format.Get(), &sign), "create ellipsis");
            const DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            result->SetTrimming(&trim, sign.Get());
        }
        return result;
    }
};

struct Raster
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    Raster()
    {
        Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr), "create WARP device");
        ComPtr<IDXGIDevice> dxgi;
        Ok(d3d.As(&dxgi), "query DXGI");
        Ok(D2D1CreateDevice(dxgi.Get(), nullptr, &device), "create D2D device");
        Ok(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context), "create context");
    }
    std::vector<std::uint32_t> Draw(IDWriteTextLayout* layout, float dpi, bool alternate)
    {
        const auto size = D2D1::SizeU(static_cast<UINT32>(256 * dpi / 96),
            static_cast<UINT32>(128 * dpi / 96));
        const auto pixel = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> target, readable;
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixel, dpi, dpi);
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &target), "create target");
        context->SetTarget(target.Get());
        context->SetDpi(dpi, dpi);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetTransform(alternate ? D2D1::Matrix3x2F::Translation(7, 3)
            : D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> brush;
        Ok(context->CreateSolidColorBrush(D2D1::ColorF(alternate ? 0x33bbdd : 0xffffff,
            alternate ? 0.6f : 1.0f), &brush), "create brush");
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0x102030));
        context->DrawTextLayout(D2D1::Point2F(9, 7), layout, brush.Get());
        Ok(context->EndDraw(), "render layout");
        context->SetTarget(nullptr);
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &readable), "create readback");
        Ok(readable->CopyFromBitmap(nullptr, target.Get(), nullptr), "copy pixels");
        D2D1_MAPPED_RECT mapped{};
        Ok(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped), "map pixels");
        std::vector<std::uint32_t> pixels(size.width * size.height);
        for (UINT32 row = 0; row < size.height; ++row)
            std::copy_n(reinterpret_cast<const std::uint32_t*>(mapped.bits + row * mapped.pitch),
                size.width, pixels.begin() + row * size.width);
        Ok(readable->Unmap(), "unmap pixels");
        return pixels;
    }
};
}

void RunWidgetTextLayoutCacheTests()
{
    Fixture f;
    Cache cache;
    Cache::Options options;
    options.width = 175;
    options.height = 64;
    std::wstring text = L"Now playing / 正在播放";
    std::string locale = "en-US";
    int builds = 0;
    const auto resolve = [&](std::wstring_view owner = L"music",
                             std::string_view surface = "desktop") {
        return cache.Resolve(owner, surface, "title", f.format.Get(), options,
            text, locale, [&] { ++builds; return f.Build(options, text); });
    };
    auto first = resolve();
    auto hit = resolve();
    Check(first && first.Get() == hit.Get() && builds == 1, "same input reuses the layout");
    const auto changed = [&](const char* message) {
        const auto previous = hit;
        const int before = builds;
        hit = resolve();
        Check(hit && hit.Get() != previous.Get() && builds == before + 1, message);
        Check(resolve().Get() == hit.Get() && builds == before + 1, "replacement also reuses");
        Check(cache.Size() == 1, "changing content replaces rather than retaining history");
    };
    text = L"Next song"; changed("track text invalidates");
    options.width = 120; changed("width invalidates");
    options.height = 48; changed("height invalidates");
    options.fontStyle = ViewFontStyle::Italic; changed("font style invalidates");
    options.alignment = DWRITE_TEXT_ALIGNMENT_TRAILING; changed("alignment invalidates");
    options.direction = ViewTextDirection::RightToLeft; changed("direction invalidates");
    options.lineHeight = 21.0f; changed("line spacing invalidates");
    options.letterSpacing = 1.5f; changed("character spacing invalidates");
    options.underline = true; changed("link underline invalidates");
    options.overflow = ViewTextOverflow::Clip; changed("trimming invalidates");
    locale = "ar-SA"; changed("locale invalidates");
    auto oldFormat = f.format;
    f.format = f.Format(23); changed("font identity invalidates");
    Check(resolve(L"other").Get() != hit.Get() && resolve(L"music", "panel").Get() != hit.Get(),
        "owner and surface cannot share mutable identities");
    cache.Erase(L"music", "panel");
    Check(cache.Size() == 2, "closing a panel retains desktop and other owner");
    cache.Erase(L"music");
    Check(cache.Size() == 1, "unload or hide removes all owner layouts");
    cache.Clear();
    Check(cache.Size() == 0 && cache.RetainedInputBytes() == 0, "clear releases accounted inputs");

    for (std::size_t i = 0; i < Cache::MaximumEntries + 8; ++i)
    {
        const auto key = std::to_string(i);
        cache.Resolve(L"music", "desktop", key, f.format.Get(), options, L"a", "en-US",
            [&] { return f.Build(options, L"a"); });
    }
    Check(cache.Size() == Cache::MaximumEntries, "entry bound limits new node identities");
    cache.Clear();
    text.assign(Cache::MaximumTextLength, L'x');
    for (int i = 0; i < 40; ++i)
        cache.Resolve(L"music", "desktop", std::to_string(i), f.format.Get(), options, text, locale,
            [&] { return f.Build(options, text); });
    Check(cache.RetainedInputBytes() <= Cache::MaximumInputBytes && cache.Size() < 40,
        "text budget evicts older retained content");
    cache.Clear();
    text.push_back(L'x');
    const auto oversized = resolve();
    Check(oversized && cache.Size() == 0, "oversized text still draws without retention");
    text = L"valid";
    resolve();
    auto failed = cache.Resolve(L"music", "desktop", "title", f.format.Get(), options,
        L"replacement", locale, [] { return ComPtr<IDWriteTextLayout>{}; });
    Check(!failed && cache.Size() == 0, "failed replacement never presents stale content");

    // Cache and fresh DirectWrite layouts must produce identical pixels across
    // DPI, script, formatting, brush and origin changes, without caching brushes.
    Raster raster;
    options = {};
    options.width = 175;
    options.height = 64;
    for (const auto* sample : { L"Song title and artist", L"歌曲标题与歌手",
                               L"مرحبا بالعالم", L"A long title with wrapping and ellipsis" })
    {
        text = sample;
        for (const bool styled : { false, true })
        {
            options.fontStyle = styled ? ViewFontStyle::Italic : ViewFontStyle::Normal;
            options.underline = styled;
            options.letterSpacing = styled ? 0.7f : 0;
            options.direction = styled ? ViewTextDirection::RightToLeft : ViewTextDirection::LeftToRight;
            auto cached = resolve();
            Check(resolve().Get() == cached.Get(), "pixel comparison uses an actual cache hit");
            auto direct = f.Build(options, text);
            for (const float dpi : { 96.0f, 144.0f, 192.0f })
                for (const bool alternate : { false, true })
                {
                    const auto a = raster.Draw(cached.Get(), dpi, alternate);
                    const auto b = raster.Draw(direct.Get(), dpi, alternate);
                    Check(a == b, "cached and direct layout pixels match");
                    Check(std::adjacent_find(a.begin(), a.end(), std::not_equal_to<>()) != a.end(),
                        "pixel comparison contains rendered glyphs");
                }
        }
    }
}
