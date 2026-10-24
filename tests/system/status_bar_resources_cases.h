// The null-resource production branch is the pre-cache complete reference.
// Only the embedded font-resource loader is replaced with the same bundled
// Fluent TTF; DirectWrite glyphs, D2D pixels, layout and hit testing stay real.
void CheckResourceReuse(ID2D1Factory1* factory, IDXGIDevice* dxgi, ID2D1DeviceContext* initialContext,
    IDWriteFactory* text, bool uncachedControl)
{
    constexpr UINT width = 1024, height = 128;
    StatusBarDrawResources resources;
    auto* cached = uncachedControl ? nullptr : &resources;
    StatusBarSnapshot data; data.clock = L"2026/10/24   20:30";
    data.power.emplace(); data.power->available = true; data.power->batteryPercent = 58;
    tray::Icon trayIcon;
    trayIcon.key = trayIcon.persistentKey = "resource-icon";
    trayIcon.identity.window = 321; trayIcon.identity.id = 7; trayIcon.identity.process = 456;
    trayIcon.width = 8; trayIcon.height = 6; trayIcon.pixels.resize(48);
    trayIcon.pixels[0] = 0x0f00000f; // Visible fringe below the centering threshold.
    trayIcon.pixels[1 * 8 + 5] = 0xff33aa55;
    trayIcon.pixels[4 * 8 + 6] = 0x10001000; // Alpha == 16 participates in ink bounds.
    data.tray.push_back(trayIcon);
    data.inputMethod.profileKnown = true; data.inputMethod.label = L"EN";
    data.inputMethod.layoutIcon.emplace();
    data.inputMethod.layoutIcon->width = 6; data.inputMethod.layoutIcon->height = 8;
    data.inputMethod.layoutIcon->pixels.resize(48, 0x08000008); // No threshold ink: full image fallback.
    StatusBarSettings settings; settings.pinnedTrayItems = {trayIcon.persistentKey}; settings.batteryPercentage = true;
    auto source = BuildStatusBarItems(settings, data);
    // Include a deterministic input-method image even if the ambient locale
    // hides that control in the default settings.
    if (std::none_of(source.begin(), source.end(), [](const auto& item) { return item.inputIcon.has_value(); }))
    {
        StatusBarItem input; input.key = "resource-input"; input.action = StatusBarAction::None;
        input.inputIcon = data.inputMethod.layoutIcon; input.left = true;
        source.insert(source.begin(), input);
    }
    PersonalizationSettings appearance;
    StatusBarPalette palette;
    bool merged = false;
    float scale = 1;
    ID2D1DeviceContext* context = initialContext;
    struct Frame { std::vector<std::uint32_t> pixels; std::vector<StatusBarItem> items; StatusBarTrayScroll scroll; };
    const auto render = [&](StatusBarDrawResources* cache) {
        Frame frame; frame.items = source;
        ComPtr<ID2D1Bitmap1> target, readable;
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format), &target)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format), &readable)),
            "resource comparison targets initialize");
        context->SetTarget(target.Get()); context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const auto result = DrawStatusBarContent(context, text, frame.items, width, height, scale,
            appearance, palette, 0, true, 1, merged, true, &frame.scroll, true, nullptr, cache);
        const auto ended = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(ended), "resource production frame renders");
        Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target.Get(), nullptr)), "resource pixels copy");
        D2D1_MAPPED_RECT mapped{};
        Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "resource pixels map");
        frame.pixels.resize(static_cast<std::size_t>(width) * height);
        for (UINT row = 0; row < height; ++row)
            std::memcpy(frame.pixels.data() + static_cast<std::size_t>(row) * width,
                mapped.bits + static_cast<std::size_t>(row) * mapped.pitch, static_cast<std::size_t>(width) * 4);
        readable->Unmap();
        return frame;
    };
    unsigned comparisons = 0;
    const auto compare = [&] {
        const auto observed = render(cached), expected = render(nullptr);
        Check(observed.pixels == expected.pixels, "cached complete pixels equal the original uncached production frame");
        Check(observed.items.size() == expected.items.size() &&
            EqualRect(&observed.scroll.viewport, &expected.scroll.viewport) && observed.scroll.offset == expected.scroll.offset,
            "resource reuse retains full layout and tray viewport");
        for (std::size_t index = 0; index < observed.items.size(); ++index)
        {
            const auto& actual = observed.items[index]; const auto& wanted = expected.items[index];
            Check(EqualRect(&actual.bounds, &wanted.bounds) && EqualRect(&actual.contentBounds, &wanted.contentBounds),
                "cached resources preserve hit and scrolled content rectangles");
            if (!IsRectEmpty(&actual.bounds))
            {
                const POINT center{(actual.bounds.left + actual.bounds.right) / 2, (actual.bounds.top + actual.bounds.bottom) / 2};
                Check(HitTestStatusBarItems(observed.items, center) == HitTestStatusBarItems(expected.items, center),
                    "cached resources preserve item hits");
            }
        }
        ++comparisons;
    };
    context->SetDpi(96, 96);
    compare();
    Check(resources.iconFace || uncachedControl, "the actual bundled Fluent face is exercised");
    const auto beforeFonts = fluentFormatCreations;
    const auto beforeFaces = resources.faceLookups;
    const auto beforeBitmaps = resources.bitmapBuilds, beforeScans = resources.inkScans;
    (void)render(cached);
    Check(fluentFormatCreations == beforeFonts, "stable redraw must reuse Fluent text formats (old uncached path repeats creation)");
    Check(resources.faceLookups == beforeFaces && beforeFaces > 0, "stable redraw retains the Fluent font face without another family lookup");
    Check(resources.bitmapBuilds == beforeBitmaps && resources.inkScans == beforeScans && beforeBitmaps >= 2,
        "stable redraw reuses actual bitmaps and alpha bounds");
    const auto trayEntry = std::find_if(resources.bitmaps.begin(), resources.bitmaps.end(), [](const auto& entry) { return entry.identity.has_value(); });
    const auto inputEntry = std::find_if(resources.bitmaps.begin(), resources.bitmaps.end(), [](const auto& entry) { return !entry.identity; });
    const RECT expectedTrayInk{5, 1, 7, 5}, expectedInputInk{0, 0, 6, 8};
    Check(trayEntry != resources.bitmaps.end() && EqualRect(&trayEntry->bitmap.ink, &expectedTrayInk) &&
        inputEntry != resources.bitmaps.end() && EqualRect(&inputEntry->bitmap.ink, &expectedInputInk),
        "threshold-16 asymmetry and transparent full-image fallback retain independently specified bounds");
    for (const bool compact : {false, true}) for (const bool hc : {false, true})
    for (const int theme : {0, 1}) for (const float dpiScale : {1.f, 1.5f, 2.f})
    {
        merged = compact; palette.highContrast = hc; appearance.contentTheme = theme; scale = dpiScale;
        palette.foreground = D2D1::ColorF(0xffffff); palette.highlight = D2D1::ColorF(0x003399);
        palette.highlightText = D2D1::ColorF(0xffff00);
        compare();
    }
    auto builds = resources.bitmapBuilds;
    for (auto& item : source) if (item.icon) { item.icon->tip = L"metadata changed"; item.icon->application = L"updated app"; }
    compare(); Check(resources.bitmapBuilds == builds, "tooltip/application changes do not replace a visual resource");
    for (auto& item : source) if (item.inputIcon) item.inputIcon->pixels[0] = 0xffaa7733;
    compare(); Check(resources.bitmapBuilds == builds + 1, "input-method pixel changes rebuild their own image and bounds");
    for (auto& item : source) if (item.icon) { item.icon->width = 6; item.icon->height = 8; }
    builds = resources.bitmapBuilds; compare();
    Check(resources.bitmapBuilds == builds + 1, "dimension changes invalidate images even when the pixel count is unchanged");
    for (auto& item : source) if (item.icon) { item.icon->width = 8; item.icon->height = 6; }
    compare();
    for (auto& item : source) if (item.icon)
    { item.icon->pixels[1 * 8 + 5] = 0; item.icon->pixels[5 * 8 + 1] = 0xffaa5522; }
    builds = resources.bitmapBuilds; compare();
    Check(resources.bitmapBuilds == builds + 1, "one changed pixel revision replaces only its bitmap and ink bounds");
    for (auto& item : source) if (item.icon) ++item.icon->identity.process;
    builds = resources.bitmapBuilds; compare();
    Check(resources.bitmapBuilds == builds + 1, "re-registration with the same key and pixels replaces the previous owner resource");
    const auto registered = source;
    std::erase_if(source, [](const auto& item) { return item.icon || item.inputIcon; });
    compare(); Check(resources.bitmaps.empty(), "deleted tray/input-method resources are released in that draw");
    source = registered; compare();
    auto fontBuilds = resources.fontBuilds;
    app_fonts::revision.fetch_add(1); compare();
    Check(resources.fontBuilds == fontBuilds + 1, "font revision replaces text formats and Fluent face");
    const auto previousSelection = app_fonts::current.load();
    auto selection = std::make_shared<app_fonts::Resource>(); selection->family = L"Consolas";
    app_fonts::current.store(selection); fontBuilds = resources.fontBuilds; compare();
    Check(resources.fontBuilds == fontBuilds + 1, "font selection identity also invalidates formats");
    app_fonts::current.store(previousSelection); compare();
    ComPtr<IDWriteFactory> otherText;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(otherText.GetAddressOf()))), "replacement DirectWrite factory initializes");
    text = otherText.Get(); fontBuilds = resources.fontBuilds; compare();
    Check(resources.fontBuilds == fontBuilds + 1, "factory replacement invalidates font resources");
    builds = resources.bitmapBuilds; context->SetDpi(120, 120); compare();
    Check(resources.bitmapBuilds >= builds + 2, "context DPI changes invalidate image resources");
    ComPtr<ID2D1Device> originalDevice; context->GetDevice(&originalDevice);
    ComPtr<ID2D1DeviceContext> sibling;
    Check(SUCCEEDED(originalDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &sibling)), "sibling draw context initializes");
    sibling->SetDpi(120, 120); context = sibling.Get(); builds = resources.bitmapBuilds; compare();
    Check(resources.bitmapBuilds == builds, "new contexts on the same device retain compatible image resources");
    ComPtr<ID2D1Device> replacementDevice; ComPtr<ID2D1DeviceContext> replacement;
    Check(SUCCEEDED(factory->CreateDevice(dxgi, &replacementDevice)) &&
        SUCCEEDED(replacementDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &replacement)), "replacement D2D device initializes");
    replacement->SetDpi(120, 120); context = replacement.Get(); builds = resources.bitmapBuilds; compare();
    Check(resources.bitmapBuilds >= builds + 2, "device replacement rebuilds compatible bitmaps before drawing");
    resources.Reset();
    Check(!resources.device && !resources.factory && resources.bitmaps.empty() && !resources.iconFace && !resources.format,
        "explicit graphics-loss/window cleanup releases all retained device and font resources");
    compare();
    resources.Reset();
    fluentFallbackForControl = true;
    compare();
    Check(!resources.iconFace && resources.TextMatches(text, scale),
        "the first collection has no Fluent face while factory/revision/scale keys are valid");
    fluentFallbackForControl = false;
    fontBuilds = resources.fontBuilds;
    compare();
    Check(resources.iconFace && resources.fontBuilds == fontBuilds + 1,
        "a later requested draw refreshes stale formats and recovers the Fluent face without a key change");
    source.clear(); scale = 1;
    for (std::size_t index = 0; index < StatusBarDrawResources::MaximumBitmaps + 32; ++index)
    {
        StatusBarItem item; item.key = "bounded-" + std::to_string(index); item.icon = trayIcon; item.icon->key = item.key;
        source.push_back(std::move(item));
    }
    Check(SUCCEEDED(resources.Prepare(context, text, scale, source)), "bounded bitmap cache prepares");
    for (const auto& item : source) Check(resources.BitmapFor(context, item).image != nullptr, "eviction never drops the current draw image");
    Check(resources.bitmaps.size() == StatusBarDrawResources::MaximumBitmaps, "bitmap retention is bounded independently of input count");
    source.clear();
    Check(SUCCEEDED(resources.Prepare(context, text, scale, source)) && resources.bitmaps.empty(), "reconcile promptly releases the bounded cache");
    std::cout << "Resource reuse: " << comparisons << " exact uncached pixel/layout/hit comparisons; lifecycle and bounds passed\n";
}
