// Included by status_bar_visibility_tests.cpp after the offscreen surface fixture.
// Only DComp allocation/copy is substituted; layout, damage, glyph/icon drawing,
// fade masks, focus, hit testing and drop resolution are production code.
#include "status_bar_scroll_failure_cases.h"

void CheckPreparedTrayScrollText(ID2D1DeviceContext* context, IDWriteFactory* text,
    const StatusBarSnapshot& data, const std::vector<std::string>& pinned)
{
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> prepared, reference, readable;
    Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 64), nullptr, 0, properties, &prepared)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 64), nullptr, 0, properties, &reference)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 64), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                properties.pixelFormat), &readable)), "prepared text comparison initializes");
    StatusBarSettings settings; settings.pinnedTrayItems = pinned;
    auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
    StatusBarDrawResources resources;
    fluentFallbackForControl = true;
    Check(SUCCEEDED(resources.PrepareText(text, 1.f)) && !resources.iconFace &&
        SUCCEEDED(ArrangeStatusBarContent(text, resources.format.Get(), items, 640, 64, 1.f, true, false, scroll, true)),
        "layout captures the original fallback font state before surface drawing");
    const auto fontBuilds = resources.fontBuilds;
    const auto render = [&](ID2D1Bitmap1* target, auto& content, auto& scrolling, StatusBarDrawResources* cache, bool keep) {
        context->SetTarget(target); context->SetDpi(96, 96); context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const RECT client{0, 0, 640, 64};
        const auto result = DrawStatusBarContent(context, text, content, 640, 64, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, false, &scrolling,
            true, keep ? &client : nullptr, cache, keep);
        const auto ended = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(ended), "prepared text frame renders");
    };
    const auto pixels = [&](ID2D1Bitmap1* target) {
        Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "prepared text pixels copy");
        D2D1_MAPPED_RECT mapped{}; Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "prepared text pixels map");
        std::vector<std::uint32_t> result(640 * 64);
        for (unsigned y = 0; y < 64; ++y) std::memcpy(result.data() + y * 640, mapped.bits + y * mapped.pitch, 640 * 4);
        readable->Unmap(); return result;
    };
    fluentFallbackForControl = false; // The collection recovers after layout, within the same frame.
    render(prepared.Get(), items, scroll, &resources, true);
    Check(!resources.iconFace && resources.fontBuilds == fontBuilds, "drawing never retries a prepared fallback within the frame");
    fluentFallbackForControl = true;
    auto expected = BuildStatusBarItems(settings, data); StatusBarTrayScroll expectedScroll;
    render(reference.Get(), expected, expectedScroll, nullptr, false);
    Check(pixels(prepared.Get()) == pixels(reference.Get()), "prepared fallback pixels match the original complete fallback");
    fluentFallbackForControl = false;
    render(prepared.Get(), items, scroll, &resources, false);
    Check(resources.iconFace && resources.fontBuilds == fontBuilds + 1, "the next requested frame retries and recovers as before");
    render(reference.Get(), expected, expectedScroll, nullptr, false);
    Check(pixels(prepared.Get()) == pixels(reference.Get()), "recovered pixels match the original complete Fluent frame");
    std::cout << "Prepared scroll text preserves fallback pixels and next-frame recovery\n";
}

void CheckTrayScrollUpdates(ID2D1Device* device, ID2D1DeviceContext* context, IDWriteFactory* text,
    StatusBarSnapshot data, const std::vector<std::string>& pinned, bool originalRouteControl = false)
{
    for (std::size_t index = 0; index < data.tray.size(); ++index)
    {
        auto& icon = data.tray[index]; icon.width = icon.height = 12; icon.pixels.resize(144);
        for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 12; ++x)
            icon.pixels[y * 12 + x] = x == 0 || y == 0 || x == 11 || y == 11 ? 0u :
                0xff000000u | (static_cast<unsigned>(index * 41 + x * 13) % 256u) << 16 |
                (static_cast<unsigned>(index * 23 + y * 17) % 256u) << 8 | static_cast<unsigned>((index * 11u + x * y) % 256u);
    }
    if (!originalRouteControl)
    {
        CheckPreparedTrayScrollText(context, text, data, pinned);
        CheckTrayScrollFailureRecovery(context, text, data, pinned);
    }
    unsigned frames = 0, legacyDoubleFrames = 0;
    std::uint64_t updatedPixels = 0, referencePixels = 0;
    for (const bool alignedPhysicalOrigin : {true, false})
    for (const bool merged : {false, true}) for (const bool rightAligned : {false, true})
    for (const bool highContrast : {false, true}) for (const bool light : {false, true})
    for (const float scale : {1.f, 1.5f, 2.f}) for (const UINT dipWidth : {480u, 1280u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale), height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        const UINT dpi = static_cast<UINT>(96 * scale);
        const RECT client{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> retained, reference, legacy, readable, fullAtlas, uncached;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &retained)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &legacy)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &uncached)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width + 32, height + 24), nullptr, 0, properties, &fullAtlas)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    properties.pixelFormat), &readable)), "scroll comparison surfaces initialize");
        StatusBarSettings settings; settings.pinnedTrayItems = pinned;
        PersonalizationSettings appearance; appearance.contentTheme = light ? 1 : 0;
        const StatusBarPalette palette{highContrast, D2D1::ColorF(0xffffff), D2D1::ColorF(0),
            D2D1::ColorF(0x003399), D2D1::ColorF(0xffffff)};
        auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
        bool frameValid = false;
        const auto pixels = [&](ID2D1Bitmap1* target) {
            Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "scroll pixels copy");
            D2D1_MAPPED_RECT mapped{};
            Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "scroll pixels map");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
            for (UINT y = 0; y < height; ++y) std::memcpy(result.data() + static_cast<std::size_t>(y) * width,
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(width) * 4);
            readable->Unmap(); return result;
        };
        const auto full = [&](ID2D1Bitmap1* target, auto& content, auto& scrolling,
            std::optional<std::size_t> hover, std::size_t focus, const std::optional<RECT>& indicator, bool* reusable = nullptr) {
            context->SetTarget(fullAtlas.Get()); context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Translation(13.f, 9.f));
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(context, text, content, width, height, scale,
                appearance, palette, hover, true, focus, merged, rightAligned, &scrolling, true, nullptr, nullptr, false, reusable);
            DrawStatusBarDropIndicator(context, indicator, appearance, palette, dpi);
            const auto ended = context->EndDraw(); context->SetTarget(nullptr);
            Check(SUCCEEDED(result) && SUCCEEDED(ended), "complete scroll reference draws");
            const D2D1_RECT_U source{13, 9, 13 + width, 9 + height};
            Check(SUCCEEDED(target->CopyFromBitmap(nullptr, fullAtlas.Get(), &source)), "full reference preserves the physical raster origin");
        };
        ComPtr<AtlasSurface> legacySurface; legacySurface.Attach(new AtlasSurface(context, legacy.Get()));
        const auto legacyFull = [&](auto& content, auto& scrolling, std::size_t focus, const std::optional<RECT>& marker,
            bool prearranged = false, StatusBarDrawResources* resources = nullptr) {
            POINT origin{}; ComPtr<ID2D1DeviceContext> drawing;
            Check(SUCCEEDED(legacySurface->BeginDraw(nullptr, IID_PPV_ARGS(&drawing), &origin)), "old complete surface path begins");
            drawing->SetDpi(96, 96);
            drawing->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
            drawing->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            drawing->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            drawing->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(drawing.Get(), text, content, width, height, scale,
                appearance, palette, {}, true, focus, merged, rightAligned, &scrolling, true,
                prearranged ? &client : nullptr, resources, prearranged && resources != nullptr);
            DrawStatusBarDropIndicator(drawing.Get(), marker, appearance, palette, dpi); drawing.Reset();
            Check(SUCCEEDED(result) && SUCCEEDED(legacySurface->EndDraw()), "old complete surface path ends");
        };
        const auto equivalent = [&](ID2D1Bitmap1* observedTarget) {
            const auto observed = pixels(observedTarget), referenceFrame = pixels(reference.Get());
            for (std::size_t i = 0; i < observed.size(); ++i)
            {
                if (observed[i] == referenceFrame[i]) continue;
                const POINT p{static_cast<LONG>(i % width), static_cast<LONG>(i / width)};
                bool equal = !highContrast && PtInRect(&scroll.viewport, p);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    equal = equal && std::abs(static_cast<int>((observed[i] >> shift) & 255) -
                        static_cast<int>((referenceFrame[i] >> shift) & 255)) <= 2;
                Check(equal, "scroll/fade/focus/marker pixels match complete reference");
            }
        };
        full(retained.Get(), items, scroll, {}, 0, {}, &frameValid);
        Check(frameValid, "ordinary complete frames really qualify for retained scroll");
        StatusBarDrawResources retainedResources;
        auto arrangedItems = BuildStatusBarItems(settings, data); auto arrangedScroll = scroll;
        Check(SUCCEEDED(retainedResources.PrepareText(text, scale)) &&
            SUCCEEDED(ArrangeStatusBarContent(text, retainedResources.format.Get(), arrangedItems, width, height,
                scale, merged, rightAligned, arrangedScroll, true)), "complete fallback arranges before any BeginDraw");
        for (std::size_t i = 0; i < items.size(); ++i)
            Check(EqualRect(&items[i].bounds, &arrangedItems[i].bounds) &&
                EqualRect(&items[i].contentBounds, &arrangedItems[i].contentBounds), "independent arrangement matches full drawing");
        legacyFull(arrangedItems, arrangedScroll, 0, {}, true, &retainedResources);
        full(reference.Get(), items, scroll, {}, 0, {});
        equivalent(legacy.Get());
        if (!scroll.Maximum())
        {
            const auto unchanged = items;
            Check(!UpdateStatusBarTrayScrollLayout(items, scroll.offset + 1, scroll, true),
                "invalid prior offset cannot reuse a complete layout");
            for (std::size_t i = 0; i < items.size(); ++i)
                Check(EqualRect(&items[i].bounds, &unchanged[i].bounds), "rejected reuse leaves layout intact");
            continue;
        }
        ComPtr<AtlasSurface> surface; surface.Attach(new AtlasSurface(context, retained.Get(), alignedPhysicalOrigin));
        ComPtr<AtlasSurface> uncachedSurface; uncachedSurface.Attach(new AtlasSurface(context, uncached.Get(), alignedPhysicalOrigin));
        const std::array<LONG, 8> distances{4, 4, static_cast<LONG>(56 * scale), scroll.Maximum(),
            -4, -static_cast<LONG>(56 * scale), -scroll.Maximum(), 4};
        std::optional<RECT> indicator;
        for (const LONG distance : distances)
        {
            const auto before = items; auto oldScroll = scroll;
            if (!scroll.Move(distance)) continue;
            const auto first = std::find_if(before.begin(), before.end(), [](const auto& item) {
                return item.icon && !IsRectEmpty(&item.bounds);
            });
            Check(first != before.end(), "overflow has a visible tray target");
            const auto hover = frames % 2 ? std::size_t{0} : static_cast<std::size_t>(first - before.begin());
            const auto focus = frames % 3 ? static_cast<std::size_t>(first - before.begin()) : std::size_t{0};
            full(retained.Get(), items, oldScroll, hover, focus, indicator, &frameValid);
            Check(frameValid, "complete scroll source frame is reusable");
            auto uncachedItems = before; auto uncachedScroll = oldScroll;
            full(uncached.Get(), uncachedItems, uncachedScroll, hover, focus, indicator);
            // Full layout is an independent geometry reference. Reusing the old
            // geometry without shifting it must fail on at least the first cell.
            auto expected = BuildStatusBarItems(settings, data); auto expectedScroll = scroll;
            full(reference.Get(), expected, expectedScroll, {}, focus, {});
            Check(UpdateStatusBarTrayScrollLayout(items, oldScroll.offset, scroll, frameValid), "stable tray layout reflows");
            bool moved = false;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                Check(EqualRect(&items[i].bounds, &expected[i].bounds) &&
                    EqualRect(&items[i].contentBounds, &expected[i].contentBounds), "scroll geometry matches complete layout");
                if (items[i].icon && !EqualRect(&items[i].contentBounds, &before[i].contentBounds)) moved = true;
                if (!items[i].icon) Check(EqualRect(&items[i].bounds, &before[i].bounds), "scroll preserves other controls");
            }
            Check(moved && EqualRect(&scroll.viewport, &expectedScroll.viewport) && scroll.offset == expectedScroll.offset &&
                scroll.contentWidth == expectedScroll.contentWidth && scroll.wheelRemainder == expectedScroll.wheelRemainder,
                "negative unshifted layout differs while viewport and offset match full layout");
            for (LONG x = 0; x < static_cast<LONG>(width); ++x)
                for (const LONG y : {scroll.viewport.top, (scroll.viewport.top + scroll.viewport.bottom) / 2,
                    scroll.viewport.bottom - 1})
                    Check(HitTestStatusBarItems(items, {x, y}) == HitTestStatusBarItems(expected, {x, y}),
                        "all visible tray hit boundaries match complete layout");
            POINT point{std::clamp(first->contentBounds.left + (first->contentBounds.right - first->contentBounds.left) / 2 - 1,
                scroll.viewport.left, scroll.viewport.right - 1), (scroll.viewport.top + scroll.viewport.bottom) / 2};
            if (const auto* overflow = Find(items, "tray"); overflow && frames % 3 == 1 && !IsRectEmpty(&overflow->bounds))
                point = {(overflow->bounds.left + overflow->bounds.right) / 2, (overflow->bounds.top + overflow->bounds.bottom) / 2};
            const auto next = ResolveStatusBarTrayDrop(items, point, MulDiv(5, static_cast<int>(dpi), 96),
                std::max(2, MulDiv(2, static_cast<int>(dpi), 96)), scroll.viewport);
            const auto wanted = ResolveStatusBarTrayDrop(expected, point, MulDiv(5, static_cast<int>(dpi), 96),
                std::max(2, MulDiv(2, static_cast<int>(dpi), 96)), expectedScroll.viewport);
            Check(next.has_value() == wanted.has_value() && (!next || (next->before == wanted->before &&
                next->pinned == wanted->pinned && EqualRect(&next->indicator, &wanted->indicator))), "drop target uses new cells");
            const auto nextIndicator = next ? std::optional(next->indicator) : std::nullopt;
            full(reference.Get(), expected, expectedScroll, {}, focus, nextIndicator);
            // Actual old fault route: complete frame with the old marker, then
            // another complete frame if PreviewDrop changes it. These draw the
            // same production content and final marker, without new scroll code.
            auto legacyItems = BuildStatusBarItems(settings, data); auto legacyScroll = scroll;
            const auto oldUpdates = legacySurface->updates.size(), oldComplete = legacySurface->completeUpdates;
            legacyFull(legacyItems, legacyScroll, focus, indicator);
            const bool markerChanged = indicator.has_value() != nextIndicator.has_value() ||
                (indicator && !EqualRect(&*indicator, &*nextIndicator));
            unsigned legacySubmissions = 1;
            if (markerChanged) { legacyFull(legacyItems, legacyScroll, focus, nextIndicator); ++legacySubmissions; }
            if (legacySubmissions == 2) ++legacyDoubleFrames;
            Check(legacySurface->updates.size() == oldUpdates + legacySubmissions &&
                legacySurface->completeUpdates == oldComplete + legacySubmissions,
                "old route actually performs full surface BeginDraw and duplicate EndDraw when the marker changes");
            if (originalRouteControl)
            {
                std::cerr << "Original scroll control: complete BeginDraw=" << legacySurface->completeUpdates - oldComplete
                    << " EndDraw=" << legacySurface->updates.size() - oldUpdates << " marker-changed=" << markerChanged << '\n';
                Check(legacySurface->completeUpdates == oldComplete && legacySubmissions == 1,
                    "original scroll route violates bounded single-update presentation");
            }
            equivalent(legacy.Get());
            const auto region = StatusBarTrayScrollDamage(items, hover, scroll.viewport, indicator, nextIndicator, width, height);
            const auto start = surface->updates.size();
            Check(SUCCEEDED(DrawStatusBarContentUpdate(surface.Get(), text, items, region, width, height, scale,
                appearance, palette, {}, true, focus, merged, scroll, nextIndicator, dpi, &retainedResources)), "one retained scroll update draws");
            Check(surface->updates.size() == start + 1 && region.right - region.left < static_cast<LONG>(width),
                "scroll submits once and avoids a full strip update");
            Check(SUCCEEDED(DrawStatusBarContentUpdate(uncachedSurface.Get(), text, expected, region, width, height, scale,
                appearance, palette, {}, true, focus, merged, expectedScroll, nextIndicator, dpi)),
                "independent complete-layout uncached content draws at the same arbitrary atlas offset");
            Check(pixels(retained.Get()) == pixels(uncached.Get()), "cached retained scroll equals uncached drawing at identical physical coordinates");
            if (alignedPhysicalOrigin) equivalent(retained.Get());
            updatedPixels += static_cast<std::uint64_t>(region.right - region.left) * (region.bottom - region.top);
            referencePixels += static_cast<std::uint64_t>(width) * height;
            indicator = nextIndicator; ++frames;
        }
        const auto intact = items;
        auto broken = scroll; broken.contentWidth += 1;
        Check(!UpdateStatusBarTrayScrollLayout(items, scroll.offset, broken, true), "structure/content width change requires full layout");
        for (std::size_t i = 0; i < items.size(); ++i)
            Check(EqualRect(&items[i].bounds, &intact[i].bounds) && EqualRect(&items[i].contentBounds, &intact[i].contentBounds),
                "failed retained-layout validation does not leave partially shifted cells");
    }
    Check(frames && legacyDoubleFrames && updatedPixels < referencePixels, "old full/double-frame failure signals were exercised");
    std::cout << "Tray scroll reference frames=" << frames << " old duplicate frames=" << legacyDoubleFrames
        << " updated pixels=" << updatedPixels << " full reference pixels=" << referencePixels << '\n';

    ComPtr<IDCompositionDesktopDevice> composition; ComPtr<IDCompositionSurface> native;
    Check(SUCCEEDED(DCompositionCreateDevice2(device, IID_PPV_ARGS(&composition))) &&
        SUCCEEDED(composition->CreateSurface(640, 64, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &native)),
        "native tray scroll surface initializes");
    StatusBarSettings nativeSettings; nativeSettings.pinnedTrayItems = pinned;
    auto nativeItems = BuildStatusBarItems(nativeSettings, data); StatusBarTrayScroll nativeScroll;
    StatusBarDrawResources nativeResources;
    bool nativeFrameValid = false;
    Check(SUCCEEDED(nativeResources.PrepareText(text, 1.f)) &&
        SUCCEEDED(ArrangeStatusBarContent(text, nativeResources.format.Get(), nativeItems, 640, 64, 1.f,
            true, false, nativeScroll, true, &nativeFrameValid)), "native first frame arranges before surface initialization");
    const RECT nativeClient{0, 0, 640, 64};
    POINT offset{}; ComPtr<ID2D1DeviceContext> nativeContext;
    Check(SUCCEEDED(native->BeginDraw(nullptr, IID_PPV_ARGS(&nativeContext), &offset)), "native scroll first frame uses full initialization");
    nativeContext->SetDpi(96, 96);
    nativeContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    nativeContext->Clear(D2D1::ColorF(0, 0.f));
    const auto initial = DrawStatusBarContent(nativeContext.Get(), text, nativeItems, 640, 64, 1.f,
        PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, false, &nativeScroll, true, &nativeClient, &nativeResources, true, &nativeFrameValid);
    nativeContext.Reset();
    Check(SUCCEEDED(initial) && SUCCEEDED(native->EndDraw()), "native tray layout is initialized");
    const LONG oldOffset = nativeScroll.offset;
    Check(nativeScroll.Move(4) && UpdateStatusBarTrayScrollLayout(nativeItems, oldOffset, nativeScroll, nativeFrameValid), "native tray reflows before drawing");
    const auto damage = StatusBarTrayScrollDamage(nativeItems, {}, nativeScroll.viewport, {}, {}, 640, 64);
    Check(SUCCEEDED(DrawStatusBarContentUpdate(native.Get(), text, nativeItems, damage, 640, 64, 1.f,
        PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, nativeScroll, {}, 96, &nativeResources)) &&
        SUCCEEDED(composition->Commit()), "real DComp accepts retained tray scroll update and synchronous commit");
}
