// DComp allocation/copy is the only drawing substitute. Both the candidate
// and the independent complete oracle use production layout, glyphs and pixels.
void CheckContentUpdates(ID2D1Device* device, ID2D1DeviceContext* context, IDWriteFactory* text,
    StatusBarSnapshot data, bool fullRouteControl = false)
{
    std::vector<std::string> pinned;
    while (data.tray.size() < 64)
    {
        tray::Icon icon;
        icon.key = icon.persistentKey = "content-" + std::to_string(data.tray.size());
        data.tray.push_back(std::move(icon));
    }
    for (std::size_t i = 0; i < data.tray.size(); ++i)
    {
        auto& icon = data.tray[i]; pinned.push_back(icon.persistentKey);
        icon.width = icon.height = 16;
        icon.pixels.assign(256, 0x08000008);
        icon.pixels[i % 2 ? 255 : 0] = 0xff33aa55;
    }
    data.inputMethod.label = L"中"; data.inputMethod.layoutLabel = L"拼";
    std::uint64_t updatedPixels = 0, legacyPixels = 0;
    unsigned frames = 0, emptyFrames = 0, fullFallbacks = 0;
    for (const bool merged : {false, true}) for (const bool right : {false, true})
    for (const bool hc : {false, true}) for (const bool light : {false, true})
    for (const float scale : {.75f, 1.f, 1.5f, 2.f}) for (const UINT dipWidth : {640u, 1920u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale);
        const UINT height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        const UINT dpi = static_cast<UINT>(96.f * scale);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> retained, reference, readable;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &retained)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    properties.pixelFormat), &readable)), "content comparison bitmaps initialize");
        ComPtr<AtlasSurface> surface, oracle;
        surface.Attach(new AtlasSurface(context, retained.Get(), true));
        oracle.Attach(new AtlasSurface(context, reference.Get(), true));
        StatusBarSettings settings; settings.cpu = settings.memory = settings.gpu = settings.traffic = true;
        settings.batteryPercentage = true; settings.pinnedTrayItems = pinned;
        auto items = BuildStatusBarItems(settings, data);
        StatusBarTrayScroll scroll;
        StatusBarDrawResources resources;
        PersonalizationSettings appearance; appearance.contentTheme = light ? 1 : 0;
        StatusBarPalette palette{hc, D2D1::ColorF(0xffffff), D2D1::ColorF(0),
            D2D1::ColorF(0x003399), D2D1::ColorF(0xffffff)};
        bool valid = false;
        const auto full = [&](IDCompositionSurface* output, IDWriteFactory* factory, auto& content,
            auto& scrolling, const StatusBarPalette& colors, StatusBarDrawResources* cache, bool* reusable,
            const std::optional<RECT>& marker = {}, std::optional<std::size_t> hover = 0) {
            POINT origin{}; ComPtr<ID2D1DeviceContext> drawing;
            Check(SUCCEEDED(output->BeginDraw(nullptr, IID_PPV_ARGS(&drawing), &origin)), "complete content surface begins");
            drawing->SetDpi(96, 96);
            drawing->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
            drawing->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            drawing->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            drawing->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(drawing.Get(), factory, content, width, height, scale,
                appearance, colors, hover, true, 1, merged, right, &scrolling, true, nullptr, cache, false, reusable);
            DrawStatusBarDropIndicator(drawing.Get(), marker, appearance, colors, dpi);
            drawing.Reset();
            Check(SUCCEEDED(result) && SUCCEEDED(output->EndDraw()), "complete content surface ends");
        };
        const auto pixels = [&](ID2D1Bitmap1* target) {
            Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "content pixels copy");
            D2D1_MAPPED_RECT mapped{}; Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "content pixels map");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
            for (UINT y = 0; y < height; ++y) std::memcpy(result.data() + static_cast<std::size_t>(y) * width,
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(width) * 4);
            readable->Unmap(); return result;
        };
        const auto compare = [&](const auto& expected, const auto& expectedScroll) {
            Check(items.size() == expected.size() && EqualRect(&scroll.viewport, &expectedScroll.viewport) &&
                scroll.offset == expectedScroll.offset && scroll.contentWidth == expectedScroll.contentWidth,
                "content update retains independent full layout and scroll state");
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                Check(EqualRect(&items[i].bounds, &expected[i].bounds) &&
                    EqualRect(&items[i].contentBounds, &expected[i].contentBounds), "content update preserves every hit/content rectangle");
                const auto& r = items[i].bounds;
                if (IsRectEmpty(&r)) continue;
                for (const POINT point : {POINT{r.left, r.top}, POINT{r.right - 1, r.bottom - 1},
                    POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2}, POINT{r.left - 1, r.top}})
                    Check(HitTestStatusBarItems(items, point) == HitTestStatusBarItems(expected, point),
                        "content update preserves center/edge/outside hits");
                for (const bool menu : {false, true})
                {
                    const auto actual = ResolveStatusBarInvocation(items, i, menu);
                    const auto wanted = ResolveStatusBarInvocation(expected, i, menu);
                    Check(actual.has_value() == wanted.has_value() && (!actual ||
                        (actual->action == wanted->action && actual->isTray == wanted->isTray &&
                            actual->trayKey == wanted->trayKey && actual->trayAction == wanted->trayAction &&
                            EqualRect(&actual->bounds, &wanted->bounds))), "content update preserves invocation identity and geometry");
                }
            }
            const auto actual = pixels(retained.Get()), wanted = pixels(reference.Get());
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                if (actual[i] == wanted[i]) continue;
                const POINT p{static_cast<LONG>(i % width), static_cast<LONG>(i / width)};
                bool equal = !palette.highContrast && PtInRect(&scroll.viewport, p);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    equal = equal && std::abs(static_cast<int>((actual[i] >> shift) & 255) -
                        static_cast<int>((wanted[i] >> shift) & 255)) <= 2;
                if (!equal)
                    std::cerr << "content mismatch merged=" << merged << " right=" << right << " hc=" << hc
                        << " scale=" << scale << " width=" << width << " pixel=" << i << '\n';
                Check(equal, "changed-item pixels differ from the independent complete frame");
            }
        };
        full(surface.Get(), text, items, scroll, palette, &resources, &valid);
        Check(valid, "initial content frame is complete and reliable");
        scroll.Move(scroll.Maximum() / 2 + (scroll.Maximum() ? 7 : 0));
        full(surface.Get(), text, items, scroll, palette, &resources, &valid);
        for (unsigned change = 0; change < 15; ++change)
        {
            const auto before = items;
            auto next = items;
            for (auto& item : next) { item.bounds = {}; item.contentBounds = {}; }
            const auto edit = [&](std::string_view key) -> StatusBarItem& {
                const auto* item = Find(next, key); Check(item != nullptr, "content target exists");
                return next[static_cast<std::size_t>(item - next.data())];
            };
            const auto iconIndex = [&](bool visible, bool edge) {
                std::size_t selected = next.size();
                for (std::size_t i = 0; i < before.size(); ++i) if (before[i].icon &&
                    (!IsRectEmpty(&before[i].bounds)) == visible)
                {
                    selected = i;
                    if (edge || (before[i].bounds.right - before[i].bounds.left >= static_cast<LONG>(28 * scale))) break;
                }
                if (selected == next.size())
                    for (std::size_t i = 0; i < before.size(); ++i) if (before[i].icon) { selected = i; break; }
                Check(selected != next.size(), "tray bitmap target exists"); return selected;
            };
            const auto recolor = [&](std::size_t index) {
                auto& image = next[index].icon->pixels;
                const bool first = (image[0] >> 24) >= 16;
                image.assign(256, 0x08080000);
                image[first ? 255 : 0] = 0xffaa7733;
            };
            switch (change)
            {
            case 0: recolor(iconIndex(true, false)); break;
            case 1: edit("cpu").text = L"CPU 29%"; break;
            case 2: edit("memory").text = L"RAM 91%"; break;
            case 3: edit("gpu").text = L"GPU 3%"; break;
            case 4: edit("clock").text = L"2026/10/23   16:26"; break;
            case 5: edit("traffic").text = L"↓ 8.2 GiB/s ↑ 0.1 B/s"; break;
            case 6:
                edit("controlCenter").batteryLevel = 9;
                edit("controlCenter").batteryTone = StatusBarBatteryTone::Charging;
                edit("controlCenter").batteryPercentageText = L"99%"; break;
            case 7:
                for (auto& item : next)
                {
                    item.tip = L"new metadata";
                    item.controlTips = {L"new network", L"new volume", L"new battery"};
                    if (item.icon) { item.icon->tip = L"new tray tip"; item.icon->application = L"new application"; item.text = item.icon->tip; }
                }
                break;
            case 8: edit("cpu").text = L"CPU 8%"; edit("clock").text = L"2026/10/23   16:27"; recolor(iconIndex(true, false)); break;
            case 9: recolor(iconIndex(false, false)); break;
            case 10: recolor(iconIndex(true, true)); break;
            case 11: edit("inputMethod").text = L"英"; break;
            case 12:
                edit("inputMethodLayout").inputIcon.emplace();
                edit("inputMethodLayout").inputIcon->width = edit("inputMethodLayout").inputIcon->height = 8;
                edit("inputMethodLayout").inputIcon->pixels.assign(64, 0xff5577aa); break;
            case 13: edit("inputMethodLayout").inputIcon->pixels[0] = 0xffaa7755; break;
            case 14: edit("cpu").text = L"CPU 37%"; recolor(iconIndex(true, false)); break;
            }
            const auto previousOffset = scroll.offset;
            if (change == 14) Check(scroll.Move(7), "combined content/scroll fixture really changes offset");
            std::optional<std::size_t> hover = 0;
            if (change == 14) hover.reset();
            const auto oldComplete = surface->completeUpdates, updateStart = surface->updates.size();
            const std::optional<RECT> marker = (change == 10 || change == 14) && !IsRectEmpty(&scroll.viewport) ?
                std::optional<RECT>(RECT{scroll.viewport.left, scroll.viewport.top + 5,
                    scroll.viewport.left + 2, scroll.viewport.bottom - 5}) : std::nullopt;
            // Establish the stable overlay before changing content, as the
            // controller does. Its pixels must survive an intersecting update.
            if (marker && change == 10)
            {
                auto original = before;
                full(surface.Get(), text, original, scroll, palette, &resources, &valid, marker);
            }
            const auto start = surface->updates.size(), completeStart = surface->completeUpdates;
            if (fullRouteControl)
            {
                full(surface.Get(), text, next, scroll, palette, &resources, &valid, marker);
                std::cout << "old content route BeginDraw(nullptr)=" << surface->completeUpdates - completeStart
                    << " EndDraw=" << surface->updates.size() - start << '\n';
                Check(surface->completeUpdates == completeStart, "stable changed content must not submit a complete strip");
            }
            Check(RestoreStatusBarContentLayout(next, before, merged, valid), "stable extent inputs reuse actual complete geometry");
            const bool partial = change != 14 || UpdateStatusBarTrayScrollLayout(next, previousOffset, scroll, valid);
            if (partial)
            {
                const auto extra = change == 14 ? StatusBarTrayScrollDamage(next, 0, scroll.viewport, {}, marker, width, height) : RECT{};
                const auto damage = StatusBarContentDamage(next, before, width, height, scale, scroll, extra);
                Check(SUCCEEDED(DrawStatusBarContentUpdates(surface.Get(), text, next, damage, width, height, scale,
                    appearance, palette, hover, true, 1, merged, scroll, marker, dpi, &resources, &valid)),
                    "production changed-content route draws its complete damage set");
                Check(surface->completeUpdates == completeStart, "content delta never upgrades to a complete strip");
            }
            else
            {
                // An empty tray viewport is an existing full-layout fallback.
                full(surface.Get(), text, next, scroll, palette, &resources, &valid, marker, hover);
                ++fullFallbacks;
            }
            if (change == 7 || change == 9)
            {
                Check(surface->updates.size() == start && SameStatusBarContent(next, before) == (change == 7),
                    "tooltip-only and hidden bitmap revisions have zero surface updates");
                ++emptyFrames;
            }
            for (std::size_t i = start; i < surface->updates.size(); ++i)
            {
                const auto& r = surface->updates[i];
                if (partial) Check(r.right - r.left < static_cast<LONG>(width), "changed-item regions exclude the complete strip");
                updatedPixels += static_cast<std::uint64_t>(r.right - r.left) * (r.bottom - r.top);
            }
            items = std::move(next);
            auto expected = items; auto expectedScroll = scroll;
            full(oracle.Get(), text, expected, expectedScroll, palette, nullptr, nullptr, marker, hover);
            compare(expected, expectedScroll);
            if (change == 7)
                for (const auto& item : items) Check(item.tip == L"new metadata" &&
                    (!item.icon || (item.icon->tip == L"new tray tip" && item.icon->application == L"new application")),
                    "retaining geometry never restores stale tooltip/application metadata");
            if (marker) full(surface.Get(), text, items, scroll, palette, &resources, &valid);
            Check(surface->completeUpdates >= oldComplete && surface->updates.size() >= updateStart, "surface observations remain monotonic");
            legacyPixels += static_cast<std::uint64_t>(width) * height;
            ++frames;
        }
        // Structural/reservation/palette/font changes route through the same
        // real complete drawing oracle, instead of reusing guessed extents.
        for (unsigned change = 0; change < 7; ++change)
        {
            auto next = items; auto nextPalette = palette;
            if (change == 0) next.erase(std::find_if(next.begin(), next.end(), [](const auto& item) { return item.icon.has_value(); }));
            if (change == 1) { auto i = std::find_if(next.begin(), next.end(), [](const auto& item) { return item.icon.has_value(); }); ++i->icon->width; i->icon->pixels.resize(i->icon->width * i->icon->height); }
            if (change == 2) for (auto& item : next) if (item.key == "clock") item.text = L"October 24, 2026   4:27 PM";
            if (change == 3) for (auto& item : next) if (item.key == "controlCenter") { item.controlGlyphs[2].clear(); item.batteryPercentageText.clear(); }
            if (change == 4) { nextPalette.highContrast = !palette.highContrast; nextPalette.foreground = D2D1::ColorF(0xaa2266); }
            if (change == 5) resources.Reset();
            if (change == 6) { auto i = std::find_if(next.begin(), next.end(), [](const auto& item) { return item.icon.has_value(); }); ++i->icon->identity.process; }
            const bool retain = SameStatusBarPalette(palette, nextPalette) && resources.TextMatches(text, scale) &&
                RestoreStatusBarContentLayout(next, items, merged, valid);
            Check(!retain, "structural/reservation/palette/font invalidation requires complete drawing");
            const auto completeStart = surface->completeUpdates;
            full(surface.Get(), text, next, scroll, nextPalette, &resources, &valid);
            items = std::move(next); palette = nextPalette;
            auto expected = items; auto expectedScroll = scroll;
            full(oracle.Get(), text, expected, expectedScroll, palette, nullptr, nullptr);
            compare(expected, expectedScroll);
            Check(surface->completeUpdates == completeStart + 1, "invalidation initializes one complete frame");
            ++fullFallbacks;
        }
    }
    Check(updatedPixels < legacyPixels, "content updates reduce actual surface pixel coverage");
    std::cout << "Content complete-reference frames=" << frames << " zero-surface frames=" << emptyFrames
        << " complete fallbacks=" << fullFallbacks << " updated pixels=" << updatedPixels
        << " old complete pixels=" << legacyPixels << '\n';

    // Observe a real unnecessary layout call with the existing DirectWrite
    // fault boundary. The candidate draws changed CPU text without measuring
    // its reserved string; the old whole-strip route really hits that probe.
    ComPtr<FailingStatusBarLayoutFactory> probe;
    probe.Attach(new FailingStatusBarLayoutFactory(text, L"CPU 100%"));
    probe->failures = 1;
    StatusBarSettings settings; settings.cpu = true;
    auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDCompositionSurface> native;
    Check(SUCCEEDED(DCompositionCreateDevice2(device, IID_PPV_ARGS(&composition))) &&
        SUCCEEDED(composition->CreateSurface(1920, 32, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &native)),
        "native content surface initializes");
    const auto nativeFull = [&](auto& content, bool* reusable) {
        POINT origin{}; ComPtr<ID2D1DeviceContext> drawing;
        Check(SUCCEEDED(native->BeginDraw(nullptr, IID_PPV_ARGS(&drawing), &origin)), "native content first draw is complete");
        drawing->SetDpi(96, 96);
        drawing->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
        drawing->Clear(D2D1::ColorF(0, 0.f));
        const auto result = DrawStatusBarContent(drawing.Get(), probe.Get(), content, 1920, 32, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, false, false, &scroll,
            true, nullptr, nullptr, false, reusable);
        drawing.Reset(); Check(SUCCEEDED(result) && SUCCEEDED(native->EndDraw()), "native complete content draws");
    };
    bool reusable = true; nativeFull(items, &reusable);
    Check(reusable, "native baseline measurement is reliable");
    auto next = items;
    for (auto& item : next) if (item.key == "cpu") item.text = L"CPU 29%";
    probe->failures = 0;
    Check(RestoreStatusBarContentLayout(next, items, false, reusable), "native content retains the complete layout");
    const auto damage = StatusBarContentDamage(next, items, 1920, 32, 1.f, scroll);
    Check(!damage.empty() && SUCCEEDED(DrawStatusBarContentUpdates(native.Get(), probe.Get(), next, damage,
        1920, 32, 1.f, PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, false, scroll, {}, 96,
        nullptr, &reusable)) && SUCCEEDED(composition->Commit()), "real DComp accepts changed CPU content");
    Check(probe->failures == 0, "changed content does not repeat reserved-width layout work");
    nativeFull(next, &reusable);
    Check(probe->failures == 1 && !reusable, "old complete route really executes the unnecessary reserved-width measurement");
    auto recovery = next;
    for (auto& item : recovery) if (item.key == "cpu") item.text = L"CPU 30%";
    Check(!RestoreStatusBarContentLayout(recovery, next, false, reusable), "observed measurement failure rejects content retention");
    nativeFull(recovery, &reusable);
    Check(reusable && Find(recovery, "cpu")->bounds.right - Find(recovery, "cpu")->bounds.left > 28,
        "complete recovery restores actual width after the real measurement failure");
    std::cout << "Native content updates, no-layout probe, old-route negative, and complete recovery passed\n";
}
