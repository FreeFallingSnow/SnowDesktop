#include "app.h"
#include "../demo_mode_rules.h"
#include "../demo_collection_rules.h"
#include <commoncontrols.h>
#include <cstdlib>

// Shell-icon decoration, privacy placeholders and quick-navigation icons.

bool DesktopApp::ShouldUseDemoIdentity(const DesktopItem& item) const
{
    return snowdesktop::demo_mode_rules::ShouldMaskApplication(
        generalSettings_.demoModeEnabled && AreDemoIdentityAssetsAvailable(),
        item.isApplicationShortcut);
}

bool DesktopApp::ShouldUseDemoCollectionIdentity(
    const DesktopWidget* collection) const
{
    return generalSettings_.demoModeEnabled &&
        AreDemoIdentityAssetsAvailable() && collection &&
        collection->type == DesktopWidgetType::Collection;
}

namespace
{
template <typename Cache>
const typename Cache::mapped_type& ResolveDemoCollectionIdentityCached(
    const DesktopWidget& collection,
    std::span<const DesktopWidget> widgets,
    Cache& cache)
{
    const auto* category = &snowdesktop::demo_collection_rules::
        ResolveCategory(collection.demoIconCategory,
            collection.title, collection.itemKeys);
    std::uint64_t signature = snowdesktop::demo_mode_rules::
        StableIdentityHash(category->id);
    auto mix = [&](std::wstring_view value) {
        signature ^= snowdesktop::demo_mode_rules::StableIdentityHash(value) +
            0x9e3779b97f4a7c15ULL + (signature << 6U) +
            (signature >> 2U);
    };
    mix(collection.title);
    mix(collection.gridCell.pageId);
    signature ^= static_cast<std::uint64_t>(collection.gridCell.column) << 8U;
    signature ^= static_cast<std::uint64_t>(collection.gridCell.row) << 16U;
    signature ^= static_cast<std::uint64_t>(collection.gridSpan.columns) << 24U;
    signature ^= static_cast<std::uint64_t>(collection.gridSpan.rows) << 32U;
    for (const auto& itemKey : collection.itemKeys)
        mix(itemKey);

    std::size_t subjectSlotOffset = 0;
    for (const auto& peer : widgets)
    {
        if (peer.type != DesktopWidgetType::Collection ||
            peer.gridCell.pageId != collection.gridCell.pageId)
            continue;
        const auto& peerCategory = snowdesktop::demo_collection_rules::
            ResolveCategory(peer.demoIconCategory,
                peer.title, peer.itemKeys);
        if (peerCategory.id != category->id)
            continue;

        mix(peer.id);
        signature ^= static_cast<std::uint64_t>(peer.itemKeys.size()) << 40U;
        signature ^= static_cast<std::uint64_t>(peer.gridCell.column) << 48U;
        signature ^= static_cast<std::uint64_t>(peer.gridCell.row) << 56U;

        const bool precedes = peer.gridCell.row < collection.gridCell.row ||
            (peer.gridCell.row == collection.gridCell.row &&
                (peer.gridCell.column < collection.gridCell.column ||
                    (peer.gridCell.column == collection.gridCell.column &&
                        peer.id < collection.id)));
        if (precedes)
            subjectSlotOffset += snowdesktop::demo_collection_rules::
                ExposedItemCount(peer.itemKeys.size(),
                    peer.gridSpan.columns, peer.gridSpan.rows);
    }

    auto& entry = cache[collection.id];
    if (entry.category != category || entry.signature != signature)
    {
        entry.category = category;
        entry.signature = signature;
        entry.subjectSlotOffset = subjectSlotOffset;
        entry.identitySlots.clear();
        entry.identitySlots.reserve(collection.itemKeys.size());
        for (std::size_t index = 0; index < collection.itemKeys.size(); ++index)
            entry.identitySlots.try_emplace(
                ToUpperInvariant(collection.itemKeys[index]), index);
    }
    return entry;
}

template <typename Entry>
snowdesktop::demo_collection_rules::IdentityPresentation
DemoCollectionPresentation(const Entry& entry,
    std::wstring_view identity)
{
    const auto found = entry.identitySlots.find(
        ToUpperInvariant(std::wstring(identity)));
    const std::size_t slot = found != entry.identitySlots.end()
        ? found->second
        : static_cast<std::size_t>(
            snowdesktop::demo_mode_rules::StableIdentityHash(identity));
    return snowdesktop::demo_collection_rules::PresentationForSlot(
        *entry.category, slot, entry.subjectSlotOffset);
}
}

const std::filesystem::path& DesktopApp::GetDemoIdentityIconDirectory() const
{
    if (demoIdentityIconDirectoryResolved_)
        return demoIdentityIconDirectory_;

    demoIdentityIconDirectoryResolved_ = true;
    std::filesystem::path configured;
    if (const wchar_t* value = _wgetenv(
            snowdesktop::demo_asset_paths::kEnvironmentVariable);
        value && *value)
        configured = value;
    demoIdentityIconDirectory_ = snowdesktop::demo_asset_paths::
        ResolveDirectory(GetExecutableDirectoryPath(), configured);
    demoIdentityIconPaths_ = snowdesktop::demo_asset_paths::
        EnumerateIcons<snowdesktop::demo_mode_rules::kDemoIconAssetCount>(
            demoIdentityIconDirectory_);
    return demoIdentityIconDirectory_;
}

bool DesktopApp::AreDemoIdentityAssetsAvailable() const
{
    GetDemoIdentityIconDirectory();
    return snowdesktop::demo_asset_paths::HasRequiredIcons(
        demoIdentityIconPaths_);
}

const std::filesystem::path& DesktopApp::GetDemoIdentityIconPath(
    size_t visualIndex) const
{
    GetDemoIdentityIconDirectory();
    static const std::filesystem::path empty;
    return visualIndex < demoIdentityIconPaths_.size()
        ? demoIdentityIconPaths_[visualIndex] : empty;
}

std::wstring DesktopApp::GetDemoIdentityTitle(
    std::wstring_view identity) const
{
    const auto& visual = snowdesktop::demo_mode_rules::
        ResolveVisualIdentity(identity);
    return std::wstring(visual.title);
}

std::wstring DesktopApp::GetDemoCollectionIdentityTitle(
    const DesktopWidget& collection, std::wstring_view identity) const
{
    const auto& entry = ResolveDemoCollectionIdentityCached(
        collection, widgets_, demoCollectionIdentityCache_);
    return std::wstring(DemoCollectionPresentation(
        entry, identity).title);
}

std::wstring DesktopApp::GetDemoCollectionCategoryTitle(
    const DesktopWidget& collection) const
{
    const auto& entry = ResolveDemoCollectionIdentityCached(
        collection, widgets_, demoCollectionIdentityCache_);
    return _LW(entry.category->titleKey);
}

ID2D1Bitmap1* DesktopApp::GetDemoIdentityBitmap(size_t visualIndex)
{
    if (!d2dContext_ || visualIndex >= demoIdentityIconBitmaps_.size())
        return nullptr;

    ComPtr<ID2D1Bitmap1>& cached =
        demoIdentityIconBitmaps_[visualIndex];
    if (cached)
        return cached.Get();

    const auto& iconPath = GetDemoIdentityIconPath(visualIndex);
    if (iconPath.empty())
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!demoIdentityWicFactory_ &&
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&demoIdentityWicFactory_))))
        return nullptr;
    if (FAILED(demoIdentityWicFactory_->CreateDecoderFromFilename(
            iconPath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(demoIdentityWicFactory_->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
    {
        cached.Reset();
        return nullptr;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) ||
        width == 0 || height == 0 ||
        width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max()))
        return nullptr;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels)
    {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    const UINT stride = width * 4U;
    const HRESULT copyResult = converter->CopyPixels(
        nullptr, stride, stride * height, static_cast<BYTE*>(pixels));
    if (SUCCEEDED(copyResult))
        cached = CreateD2DBitmapFromHBitmap(
            bitmap, iconBeautifySettings_.enabled);
    DeleteObject(bitmap);
    if (!cached)
        return nullptr;
    return cached.Get();
}

void DesktopApp::PreloadDemoIdentityBitmaps()
{
    if (!d2dContext_ || !generalSettings_.demoModeEnabled ||
        !AreDemoIdentityAssetsAvailable())
        return;
    for (size_t index = 0; index < demoIdentityIconBitmaps_.size(); ++index)
        GetDemoIdentityBitmap(index);
}

void DesktopApp::DrawDemoCollectionIdentityIcon(
    ID2D1RenderTarget* context, const DesktopWidget& collection,
    std::wstring_view identity, RECT iconRect, float opacity)
{
    if (!context || IsRectEmptyRect(iconRect) || opacity <= 0.0f)
        return;
    const auto& entry = ResolveDemoCollectionIdentityCached(
        collection, widgets_, demoCollectionIdentityCache_);
    const auto presentation = DemoCollectionPresentation(
        entry, identity);
    if (ID2D1Bitmap1* bitmap = GetDemoIdentityBitmap(
            presentation.visualIndex))
    {
        DrawIconBitmap(context, bitmap, iconRect, opacity);
        DrawDemoIdentityVariantBadge(
            context, presentation.variantIndex, iconRect, opacity);
        return;
    }
    DrawDemoIdentityIcon(context, identity, iconRect, opacity);
}

void DesktopApp::DrawDemoIdentityVariantBadge(
    ID2D1RenderTarget* context, size_t variantIndex,
    RECT iconRect, float opacity)
{
    if (!context || variantIndex == 0 || opacity <= 0.0f ||
        IsRectEmptyRect(iconRect))
        return;

    static constexpr std::array<std::wstring_view, 5> kVariantGlyphs{
        L"", L"\uE73E", L"\uE73A", L"\uE8FB", L"\uE945" };
    variantIndex %= kVariantGlyphs.size();
    if (variantIndex == 0)
        return;

    const int shortSide = std::max(1L, std::min(
        iconRect.right - iconRect.left,
        iconRect.bottom - iconRect.top));
    const int badgeSize = std::clamp(
        static_cast<int>(std::round(shortSide * 0.34f)),
        10, std::max(10, shortSide));
    const int inset = std::max(1,
        static_cast<int>(std::round(shortSide * 0.03f)));
    const RECT badgeRect = MakeRect(
        iconRect.right - badgeSize - inset,
        iconRect.bottom - badgeSize - inset,
        iconRect.right - inset,
        iconRect.bottom - inset);
    DrawD2DRoundedRectangle(context, badgeRect, badgeSize * 0.34f,
        D2D1::ColorF(0.96f, 0.97f, 0.99f, 0.96f * opacity),
        D2D1::ColorF(0.13f, 0.18f, 0.28f, 0.30f * opacity),
        std::max(1.0f, shortSide * 0.012f));
    if (fluentIconTextFormat_)
        DrawD2DText(context, std::wstring(kVariantGlyphs[variantIndex]),
            badgeRect, fluentIconTextFormat_.Get(),
            D2D1::ColorF(0.14f, 0.26f, 0.48f, 0.94f * opacity));
}

void DesktopApp::DrawDemoIdentityIcon(ID2D1RenderTarget* context,
    std::wstring_view identity, RECT iconRect, float opacity)
{
    if (!context || IsRectEmptyRect(iconRect) || opacity <= 0.0f)
        return;

    const size_t visualIndex = snowdesktop::demo_mode_rules::
        VisualIdentityIndex(identity);
    const auto& visual = snowdesktop::demo_mode_rules::
        VisualIdentityAt(visualIndex);
    if (ID2D1Bitmap1* bitmap = GetDemoIdentityBitmap(visualIndex))
    {
        DrawIconBitmap(context, bitmap, iconRect, opacity);
        return;
    }
    const float red = static_cast<float>(
        (visual.backgroundRgb >> 16U) & 0xFFU) / 255.0f;
    const float green = static_cast<float>(
        (visual.backgroundRgb >> 8U) & 0xFFU) / 255.0f;
    const float blue = static_cast<float>(
        visual.backgroundRgb & 0xFFU) / 255.0f;
    const int shortSide = std::max(1L, std::min(
        iconRect.right - iconRect.left,
        iconRect.bottom - iconRect.top));
    const float radius = std::max(3.0f, shortSide * 0.22f);
    DrawD2DRoundedRectangle(context, iconRect, radius,
        D2D1::ColorF(red, green, blue, opacity),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f * opacity),
        std::max(1.0f, shortSide * 0.018f));

    if (!fluentIconTextFormat_)
        return;

    D2D1_MATRIX_3X2_F previousTransform{};
    context->GetTransform(&previousTransform);
    const float glyphScale = std::clamp(
        static_cast<float>(shortSide) * 0.50f / 14.0f,
        0.72f, 3.0f);
    const D2D1_POINT_2F center = D2D1::Point2F(
        (iconRect.left + iconRect.right) * 0.5f,
        (iconRect.top + iconRect.bottom) * 0.5f);
    context->SetTransform(
        D2D1::Matrix3x2F::Scale(glyphScale, glyphScale, center) *
        previousTransform);
    DrawD2DText(context, std::wstring(visual.glyph), iconRect,
        fluentIconTextFormat_.Get(),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f * opacity));
    context->SetTransform(previousTransform);
}

void DesktopApp::DrawShortcutArrowOverlay(ID2D1RenderTarget* ctx, RECT iconRect, float alpha)
{
    if (!ctx) return;

    if (iconBeautifySettings_.enabled)
    {
        const int iconHeight = std::max(1, static_cast<int>(iconRect.bottom - iconRect.top));
        const float scale = static_cast<float>(iconHeight) / 64.0f;
        const int pad = std::max(1, static_cast<int>(std::round(2.0f * scale)));
        int badgeSz = static_cast<int>(std::round(17.0f * scale));
        const int iconWidth = std::max(1, static_cast<int>(iconRect.right - iconRect.left));
        badgeSz = std::clamp(badgeSz, 9, std::max(9, iconWidth));
        RECT badgeRect = MakeRect(
            iconRect.left + pad,
            iconRect.bottom - badgeSz - pad,
            iconRect.left + pad + badgeSz,
            iconRect.bottom - pad);

        ComPtr<ID2D1SolidColorBrush> badgeFillBrush;
        ComPtr<ID2D1SolidColorBrush> badgeStrokeBrush;
        if (FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.86f, 0.89f, 0.94f, 0.96f * alpha), &badgeFillBrush)) || !badgeFillBrush ||
            FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.54f, 0.61f, 0.72f, 0.58f * alpha), &badgeStrokeBrush)) || !badgeStrokeBrush)
        {
            return;
        }

        const float left = static_cast<float>(badgeRect.left);
        const float top = static_cast<float>(badgeRect.top);
        const float right = static_cast<float>(badgeRect.right);
        const float bottom = static_cast<float>(badgeRect.bottom);
        const float sz = right - left;
        const D2D1_ELLIPSE badgeEllipse = D2D1::Ellipse(
            D2D1::Point2F((left + right) * 0.5f, (top + bottom) * 0.5f),
            sz * 0.5f,
            sz * 0.5f);

        ComPtr<ID2D1SolidColorBrush> arrowBrush;
        if (FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.18f, 0.30f, 0.48f, 0.92f * alpha), &arrowBrush)) || !arrowBrush)
        {
            return;
        }

        ctx->FillEllipse(badgeEllipse, badgeFillBrush.Get());
        ctx->DrawEllipse(badgeEllipse, badgeStrokeBrush.Get(), std::max(1.0f, 1.1f * scale));

        const float stroke = std::max(1.0f, sz * 0.11f);
        const D2D1_POINT_2F start = D2D1::Point2F(left + sz * 0.30f, top + sz * 0.70f);
        const D2D1_POINT_2F end = D2D1::Point2F(left + sz * 0.70f, top + sz * 0.30f);
        ctx->DrawLine(start, end, arrowBrush.Get(), stroke);
        ctx->DrawLine(end, D2D1::Point2F(left + sz * 0.46f, top + sz * 0.30f), arrowBrush.Get(), stroke);
        ctx->DrawLine(end, D2D1::Point2F(left + sz * 0.70f, top + sz * 0.54f), arrowBrush.Get(), stroke);
        return;
    }

    auto createArrowBitmap = [&](ComPtr<ID2D1Bitmap>& outBitmap, SIZE& outSize) -> bool {
        if (outBitmap)
            return true;

        SHSTOCKICONINFO sii{};
        sii.cbSize = sizeof(sii);
        if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
            return false;

        int w = GetSystemMetrics(SM_CXICON);
        int h = GetSystemMetrics(SM_CYICON);
        if (w <= 0) w = 32;
        if (h <= 0) h = 32;

        SIZE bitmapSize{};
        HBITMAP dib = CreateAlphaBitmapFromIcon(sii.hIcon, w, h, bitmapSize);
        if (!dib)
        {
            DestroyIcon(sii.hIcon);
            return false;
        }

        DIBSECTION ds{};
        GetObjectW(dib, sizeof(ds), &ds);

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        ComPtr<ID2D1Bitmap> bitmap;
        HRESULT hr = ctx->CreateBitmap(D2D1::SizeU(w, h), ds.dsBm.bmBits,
            static_cast<UINT32>(ds.dsBm.bmWidthBytes), props, &bitmap);

        DeleteObject(dib);
        DestroyIcon(sii.hIcon);

        if (FAILED(hr) || !bitmap)
            return false;

        outBitmap = std::move(bitmap);
        outSize = bitmapSize;
        return true;
    };

    ID2D1Bitmap* arrowBitmap = nullptr;

    ComPtr<ID2D1DeviceContext> deviceContext;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&deviceContext))) && deviceContext)
    {
        if (!createArrowBitmap(shortcutArrowBitmap_, shortcutArrowBitmapSize_))
            return;
        arrowBitmap = shortcutArrowBitmap_.Get();
    }
    else
    {
        // 非 device-context 渲染目标已不再使用；快捷导航走 DComp 后 ctx 必为 device context。
        return;
    }

    if (!arrowBitmap) return;

    float scale = static_cast<float>(iconRect.bottom - iconRect.top) / 64.0f;
    int arrowSz = static_cast<int>(30.0f * scale + 0.5f);
    if (arrowSz < 10)
        arrowSz = 10;
    int arrowX = iconRect.left;
    int arrowY = iconRect.bottom - arrowSz;

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(arrowX),
        static_cast<float>(arrowY),
        static_cast<float>(arrowX + arrowSz),
        static_cast<float>(arrowY + arrowSz));

    ctx->DrawBitmap(arrowBitmap, dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void DesktopApp::DrawBeautifiedIconPlate(ID2D1RenderTarget* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F border, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    ComPtr<ID2D1Factory> factory;
    ctx->GetFactory(&factory);
    if (!factory) return;

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry ||
        FAILED(geometry->Open(&sink)) || !sink)
        return;

    const float left = static_cast<float>(rect.left);
    const float top = static_cast<float>(rect.top);
    const float right = static_cast<float>(rect.right);
    const float bottom = static_cast<float>(rect.bottom);
    const float width = right - left;
    const float height = bottom - top;
    const auto shape = iconBeautifySettings_.enabled
        ? iconBeautifySettings_.shape
        : snowdesktop::IconBeautifyShape::LegacyRounded;
    const auto& outline = snowdesktop::icon_beautify::ShapeOutline(shape);
    if (outline.empty())
        return;
    const auto toPoint = [&](const snowdesktop::icon_beautify::ShapePoint& point) {
        return D2D1::Point2F(
            left + point.x * width,
            top + point.y * height);
    };
    sink->BeginFigure(toPoint(outline.front()), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t index = 1; index < outline.size(); ++index)
        sink->AddLine(toPoint(outline[index]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;

    ComPtr<ID2D1SolidColorBrush> fillBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(fill, &fillBrush)) && fillBrush)
        ctx->FillGeometry(geometry.Get(), fillBrush.Get());
    if (strokeWidth > 0.0f && border.a > 0.0f &&
        SUCCEEDED(ctx->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        ctx->DrawGeometry(geometry.Get(), borderBrush.Get(), strokeWidth);
}

void DesktopApp::DrawPrivacyFaIcon(
    ID2D1DeviceContext* ctx, RECT rect, bool directory)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    ComPtr<ID2D1Bitmap1>& cached = directory
        ? privacyFolderIconBitmap_ : privacyFileIconBitmap_;
    if (!cached)
    {
        constexpr int bitmapSize = kIconBitmapSize;
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = bitmapSize;
        bitmapInfo.bmiHeader.biHeight = -bitmapSize;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP source = CreateDIBSection(screenDc, &bitmapInfo,
            DIB_RGB_COLORS, &bits, nullptr, 0);
        if (source && bits)
        {
            std::fill_n(static_cast<std::uint32_t*>(bits),
                bitmapSize * bitmapSize, 0u);
            HDC memoryDc = CreateCompatibleDC(screenDc);
            if (memoryDc)
            {
                HGDIOBJ oldBitmap = SelectObject(memoryDc, source);
                HFONT font = CreateFontW(-static_cast<int>(bitmapSize * 0.68f),
                    0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Font Awesome 6 Free Solid");
                HGDIOBJ oldFont = font ? SelectObject(memoryDc, font) : nullptr;
                SetBkMode(memoryDc, TRANSPARENT);
                SetTextColor(memoryDc, RGB(255, 255, 255));
                RECT glyphRect{ 0, 0, bitmapSize, bitmapSize };
                const wchar_t* glyph = L"";
                DrawTextW(memoryDc, glyph, -1, &glyphRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                if (oldFont) SelectObject(memoryDc, oldFont);
                if (font) DeleteObject(font);
                SelectObject(memoryDc, oldBitmap);
                DeleteDC(memoryDc);

                constexpr int glyphR = 0xff;
                constexpr int glyphG = 0xdb;
                constexpr int glyphB = 0x76;
                auto* pixels = static_cast<std::uint32_t*>(bits);
                for (int i = 0; i < bitmapSize * bitmapSize; ++i)
                {
                    const std::uint32_t pixel = pixels[i];
                    const int alpha = static_cast<int>(std::max({
                        pixel & 0xffu, (pixel >> 8) & 0xffu,
                        (pixel >> 16) & 0xffu }));
                    pixels[i] = static_cast<std::uint32_t>(alpha) << 24 |
                        static_cast<std::uint32_t>((glyphR * alpha + 127) / 255) << 16 |
                        static_cast<std::uint32_t>((glyphG * alpha + 127) / 255) << 8 |
                        static_cast<std::uint32_t>((glyphB * alpha + 127) / 255);
                }
                cached = CreateD2DBitmapFromHBitmap(source, true);
            }
            DeleteObject(source);
        }
        ReleaseDC(nullptr, screenDc);
    }

    if (cached)
    {
        ctx->DrawBitmap(cached.Get(), ToD2DRect(rect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
        return;
    }

    const float luminance = iconBeautifySettings_.backgroundStartR * 0.2126f +
        iconBeautifySettings_.backgroundStartG * 0.7152f +
        iconBeautifySettings_.backgroundStartB * 0.0722f;
    const D2D1_COLOR_F fill = D2D1::ColorF(
        iconBeautifySettings_.backgroundStartR,
        iconBeautifySettings_.backgroundStartG,
        iconBeautifySettings_.backgroundStartB,
        iconBeautifySettings_.backgroundOpacity);
    const D2D1_COLOR_F border = luminance > 0.58f
        ? D2D1::ColorF(0.62f, 0.66f, 0.72f, iconBeautifySettings_.backgroundOpacity)
        : D2D1::ColorF(0.78f, 0.82f, 0.90f, iconBeautifySettings_.backgroundOpacity);
    DrawBeautifiedIconPlate(ctx, rect, fill, border, 1.0f);
    ComPtr<IDWriteTextFormat> format;
    format.Attach(CreateFaTextFormat(dwriteFactory_.Get(),
        static_cast<float>(std::min(rect.right - rect.left, rect.bottom - rect.top)) * 0.52f));
    if (format)
        DrawD2DText(ctx, L"", rect, format.Get(),
            D2D1::ColorF(1.0f, 219.0f / 255.0f, 118.0f / 255.0f, 0.94f));
}

void DesktopApp::DrawPlaceholderIcon(ID2D1RenderTarget* ctx, int sysIconIndex,
    RECT iconRect, float alpha, bool allowBeautify)
{
    if (!ctx || sysIconIndex < 0) return;

    // 快捷导航改走 DComp 后，ctx 必为 ID2D1DeviceContext（与桌面同源 d2dDevice_）。
    // 非 device-context 路径已废弃，直接返回以避免在错误设备上创建位图。
    ComPtr<ID2D1DeviceContext> deviceContext;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&deviceContext))) || !deviceContext || !d2dContext_)
        return;
    auto& cache = placeholderIconCache_;
    const bool beautify = allowBeautify && iconBeautifySettings_.enabled;
    const int targetSize = std::max(
        iconRect.right - iconRect.left,
        iconRect.bottom - iconRect.top);
    const int sourceSize = snowdesktop::icon_render_rules::
        SourcePixelsForTarget(targetSize);
    const std::uint64_t cacheKey =
        (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(sysIconIndex)) << 32) |
        (static_cast<std::uint64_t>(sourceSize) << 1) |
        static_cast<std::uint64_t>(beautify ? 1 : 0);

    auto cached = cache.find(cacheKey);
    if (cached == cache.end())
    {
        ComPtr<IImageList> imageList;
        HRESULT hr = SHGetImageList(SHIL_JUMBO, IID_IImageList,
            reinterpret_cast<void**>(imageList.GetAddressOf()));
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_LARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList)
            return;

        HICON icon = nullptr;
        if (FAILED(imageList->GetIcon(sysIconIndex,
                ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) || !icon)
            return;

        SIZE bitmapSize{};
        HBITMAP alphaBitmap = CreateAlphaBitmapFromIcon(
            icon, sourceSize, sourceSize, bitmapSize);
        DestroyIcon(icon);
        if (!alphaBitmap)
            return;

        ComPtr<ID2D1Bitmap1> iconBitmap = CreateD2DBitmapFromHBitmap(alphaBitmap, beautify);
        DeleteObject(alphaBitmap);
        if (!iconBitmap)
        {
            return;
        }

        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(iconBitmap.As(&bitmap)) || !bitmap)
            return;

        cached = cache.emplace(cacheKey, std::move(bitmap)).first;
    }

    DrawIconBitmap(ctx, cached->second.Get(), iconRect, alpha);
}

void DesktopApp::DrawQuickNavSysIcon(ID2D1RenderTarget* ctx, int sysIconIndex, RECT dstRect)
{
    if (!ctx || sysIconIndex < 0) return;
    // 仅 ID2D1DeviceContext（与桌面同源 d2dDevice_）才支持 CreateBitmap/共享。
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&dc))) || !dc) return;

    const int targetSize = std::max(
        dstRect.right - dstRect.left,
        dstRect.bottom - dstRect.top);
    const int sourceSize = snowdesktop::icon_render_rules::
        SourcePixelsForTarget(targetSize);
    const std::uint64_t cacheKey =
        (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(sysIconIndex)) << 32) |
        static_cast<std::uint64_t>(sourceSize);
    auto cached = quickNavSysIconCache_.find(cacheKey);
    if (cached == quickNavSysIconCache_.end())
    {
        // EXTRALARGE avoids the transparent padding used by some JUMBO icons;
        // DrawIconEx then rasterizes it into the layout-aware source bucket.
        ComPtr<IImageList> imageList;
        HRESULT hr = SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
            reinterpret_cast<void**>(imageList.GetAddressOf()));
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_LARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList) return;

        HICON icon = nullptr;
        if (FAILED(imageList->GetIcon(sysIconIndex,
                ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) || !icon)
            return;

        SIZE bitmapSize{};
        HBITMAP alphaBitmap = CreateAlphaBitmapFromIcon(
            icon, sourceSize, sourceSize, bitmapSize);
        DestroyIcon(icon);
        if (!alphaBitmap) return;

        ComPtr<ID2D1Bitmap1> iconBitmap = CreateD2DBitmapFromHBitmap(
            alphaBitmap, iconBeautifySettings_.enabled);
        DeleteObject(alphaBitmap);
        if (!iconBitmap)
        {
            return;
        }

        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(iconBitmap.As(&bitmap)) || !bitmap)
            return;

        cached = quickNavSysIconCache_.emplace(
            cacheKey, std::move(bitmap)).first;
    }

    DrawIconBitmap(ctx, cached->second.Get(), dstRect);
}

/**
 * @brief 触发换页通知（记录文本与时间戳，启动重绘定时器）。
 * @param text 通知文本（如"第3页"）。
 */
