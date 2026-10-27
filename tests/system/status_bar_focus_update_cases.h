// Included by the existing production offscreen driver after its AtlasSurface.
class RejectFocusUpdateSurface final : public IDCompositionSurface
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) override
    { if (!value) return E_POINTER; *value = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE BeginDraw(const RECT*, REFIID, void** value, POINT*) override
    { if (value) *value = nullptr; ++attempts; return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE EndDraw() override { ++ends; return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE SuspendDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResumeDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Scroll(const RECT*, const RECT*, int, int) override { return E_NOTIMPL; }
    unsigned attempts = 0, ends = 0;
};

void CheckFocusFeedbackUpdates(ID2D1DeviceContext* context, IDWriteFactory* text,
    const StatusBarSnapshot& data, const std::vector<std::string>& pinned)
{
    unsigned transitions = 0, noops = 0;
    for (const bool merged : {false, true}) for (const bool highContrast : {false, true})
    for (const bool light : {false, true}) for (const float scale : {.75f, 1.f, 1.5f, 2.f})
    for (const UINT dipWidth : {640u, 1920u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale),
            height = static_cast<UINT>((merged ? 64.f : 32.f) * scale), dpi = static_cast<UINT>(96 * scale);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> retained, reference, readable, fullAtlas;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &retained)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width + 32, height + 24), nullptr, 0, properties, &fullAtlas)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    properties.pixelFormat), &readable)), "focus feedback surfaces initialize");
        StatusBarSettings settings; settings.pinnedTrayItems = pinned;
        PersonalizationSettings appearance; appearance.contentTheme = light ? 1 : 0;
        const StatusBarPalette palette{highContrast, D2D1::ColorF(0xffffff), D2D1::ColorF(0),
            D2D1::ColorF(0x003399), D2D1::ColorF(0xffffff)};
        auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
        const auto pixels = [&](ID2D1Bitmap1* target) {
            Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "focus pixels copy");
            D2D1_MAPPED_RECT mapped{};
            Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "focus pixels map");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
            for (UINT y = 0; y < height; ++y) std::memcpy(result.data() + static_cast<std::size_t>(y) * width,
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(width) * 4);
            readable->Unmap(); return result;
        };
        const auto full = [&](ID2D1Bitmap1* target, auto& content, auto& scrolling,
            const StatusBarInteractionVisualState& state) {
            context->SetTarget(fullAtlas.Get()); context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Translation(13.f, 9.f));
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(context, text, content, width, height, scale,
                appearance, palette, state.hovered, state.focused.has_value(), state.focused.value_or(0),
                merged, true, &scrolling);
            DrawStatusBarDropIndicator(context, state.indicator, appearance, palette, dpi);
            const auto ended = context->EndDraw(); context->SetTarget(nullptr);
            Check(SUCCEEDED(result) && SUCCEEDED(ended), "complete focus reference draws");
            const D2D1_RECT_U client{13, 9, 13 + width, 9 + height};
            Check(SUCCEEDED(target->CopyFromBitmap(nullptr, fullAtlas.Get(), &client)), "complete focus atlas copies to client");
        };
        StatusBarInteractionVisualState before;
        full(retained.Get(), items, scroll, before);
        scroll.Move(scroll.Maximum() / 2);
        full(retained.Get(), items, scroll, before);
        ComPtr<AtlasSurface> surface; surface.Attach(new AtlasSurface(context, retained.Get(), true));
        std::vector<std::size_t> visible;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (!IsRectEmpty(&items[i].bounds) && items[i].action != StatusBarAction::None &&
                items[i].action != StatusBarAction::ShowDesktop) visible.push_back(i);
        Check(visible.size() >= 2, "focus fixture has independent visible targets");
        const auto first = visible.front(), last = visible.back();
        std::vector<std::size_t> visibleTray;
        for (const auto index : visible) if (items[index].icon) visibleTray.push_back(index);
        Check(visibleTray.size() >= 2, "focus fixture has distinct visible tray cells");
        const auto firstTray = visibleTray.front(), lastTray = visibleTray.back();
        std::optional<RECT> indicator;
        if (!IsRectEmpty(&scroll.viewport))
            indicator = RECT{scroll.viewport.left + 3, scroll.viewport.top + 5,
                scroll.viewport.left + 5, scroll.viewport.bottom - 5};
        auto roundedIndicator = indicator;
        if (roundedIndicator) roundedIndicator->right = roundedIndicator->left + 28;
        const std::vector<StatusBarInteractionVisualState> expectedSequence{
            {first, {}, {}}, {first, last, {}}, {{}, last, indicator}, {{}, first, roundedIndicator},
            {{}, first, roundedIndicator}, {{}, {}, roundedIndicator}, {last, first, roundedIndicator}, {{}, first, {}}, {{}, {}, {}},
            {lastTray, firstTray, {}}, {lastTray, lastTray, {}}, {{}, {}, {}}};
        struct Input { std::optional<std::size_t> hover, focus; bool keyboard, actualFocus; std::optional<RECT> marker; };
        const std::vector<Input> sequence{
            {first, {}, false, false, {}}, {first, last, true, true, {}}, {{}, last, true, true, indicator},
            {{}, first, true, true, roundedIndicator}, {{}, first, true, true, roundedIndicator},
            {{}, first, true, false, roundedIndicator}, {last, first, true, true, roundedIndicator},
            {{}, first, true, true, {}}, {{}, first, false, true, {}},
            {lastTray, firstTray, true, true, {}}, {lastTray, lastTray, true, true, {}}, {{}, {}, false, false, {}}};
        for (std::size_t step = 0; step < sequence.size(); ++step)
        {
            const auto& next = sequence[step];
            StatusBarInteraction input; input.hovered = next.hover; input.focused = next.focus;
            const auto after = StatusBarInteractionVisual(input, next.keyboard, next.actualFocus, next.marker);
            const auto originalItems = items; const auto originalScroll = scroll;
            const auto start = surface->updates.size();
            const auto damage = StatusBarInteractionDamageRegions(items, before, after, width, height);
            if (!damage.empty())
                Check(SUCCEEDED(DrawStatusBarContentUpdates(surface.Get(), text, items, damage,
                    width, height, scale, appearance, palette, after.hovered, after.focused.has_value(),
                    after.focused.value_or(0), merged, scroll, after.indicator, dpi)), "focus retained update draws");
            auto expectedItems = BuildStatusBarItems(settings, data); auto expectedScroll = originalScroll;
            full(reference.Get(), expectedItems, expectedScroll, expectedSequence[step]);
            const auto observed = pixels(retained.Get()), expected = pixels(reference.Get());
            for (std::size_t i = 0; i < observed.size(); ++i)
            {
                if (observed[i] == expected[i]) continue;
                const POINT location{static_cast<LONG>(i % width), static_cast<LONG>(i / width)};
                bool equivalent = !highContrast && PtInRect(&scroll.viewport, location);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    equivalent = equivalent && std::abs(static_cast<int>((observed[i] >> shift) & 255) -
                        static_cast<int>((expected[i] >> shift) & 255)) <= 2;
                if (!equivalent)
                    std::cerr << "focus feedback mismatch merged=" << merged << " hc=" << highContrast
                        << " light=" << light << " scale=" << scale << " width=" << width << " step=" << step
                        << " x=" << location.x << " y=" << location.y << " observed=" << observed[i]
                        << " expected=" << expected[i] << '\n';
                Check(equivalent, "focus/hover/indicator pixels match complete reference");
            }
            for (std::size_t i = 0; i < items.size(); ++i)
                Check(EqualRect(&items[i].bounds, &originalItems[i].bounds) &&
                    EqualRect(&items[i].contentBounds, &originalItems[i].contentBounds) &&
                    EqualRect(&items[i].bounds, &expectedItems[i].bounds), "focus update retains reference hit geometry");
            Check(EqualRect(&scroll.viewport, &originalScroll.viewport) && scroll.offset == originalScroll.offset,
                "focus feedback retains scroll geometry");
            Check(RetainStatusBarPreparedLayout(items, originalItems, originalScroll, scroll, merged, true) &&
                !RetainStatusBarPreparedLayout(items, originalItems, originalScroll, scroll, merged, false),
                "prepared geometry reuse requires a successful measurement snapshot");
            if (damage.empty()) { Check(surface->updates.size() == start, "unchanged focus submits nothing"); ++noops; }
            else
            {
                Check(surface->updates.size() == start + damage.size(), "focus transition submits its disjoint damage set once");
                std::uint64_t changedPixels = 0;
                for (const auto& region : damage)
                    changedPixels += static_cast<std::uint64_t>(region.right - region.left) * (region.bottom - region.top);
                Check(changedPixels < static_cast<std::uint64_t>(width) * height / 2,
                    "distant focus/hover feedback preserves bounded disjoint updates");
            }
            before = after; ++transitions;
        }
    }
    Check(transitions && noops, "focus changes and stable-state noops executed");
    std::cout << "Focus feedback reference transitions=" << transitions << " stable noops=" << noops << '\n';
}

void CheckFocusNativeUpdates(ID2D1Device* device, IDWriteFactory* text, const StatusBarSnapshot& data,
    const std::vector<std::string>& pinned)
{
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDCompositionSurface> surface;
    Check(SUCCEEDED(DCompositionCreateDevice2(device, IID_PPV_ARGS(&composition))) &&
        SUCCEEDED(composition->CreateSurface(640, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)), "native focus surface initializes without an HWND");
    StatusBarSettings settings; settings.pinnedTrayItems = pinned;
    auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
    POINT origin{}; ComPtr<ID2D1DeviceContext> drawing;
    Check(SUCCEEDED(surface->BeginDraw(nullptr, IID_PPV_ARGS(&drawing), &origin)), "native focus complete initialization begins");
    drawing->SetDpi(96, 96);
    drawing->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
    drawing->Clear(D2D1::ColorF(0, 0.f));
    const auto result = DrawStatusBarContent(drawing.Get(), text, items, 640, 64, 1.f,
        PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, true, &scroll);
    drawing.Reset();
    Check(SUCCEEDED(result) && SUCCEEDED(surface->EndDraw()) && SUCCEEDED(composition->Commit()),
        "native focus complete frame initializes before incremental updates");
    const auto* menu = Find(items, "menu"); const auto* clock = Find(items, "clock");
    Check(menu && clock, "native feedback fixture has two distinct controls");
    const auto first = static_cast<std::size_t>(menu - items.data()), last = static_cast<std::size_t>(clock - items.data());
    const RECT marker{scroll.viewport.left + 2, 5, scroll.viewport.left + 30, 59};
    StatusBarInteractionVisualState before;
    for (const auto& after : std::vector<StatusBarInteractionVisualState>{
        {{}, first, {}}, {first, last, marker}, {{}, {}, marker}, {{}, {}, {}}})
    {
        const auto damage = StatusBarInteractionDamageRegions(items, before, after, 640, 64);
        Check(!damage.empty() && SUCCEEDED(DrawStatusBarContentUpdates(surface.Get(), text, items, damage,
            640, 64, 1.f, PersonalizationSettings{}, StatusBarPalette{}, after.hovered, after.focused.has_value(),
            after.focused.value_or(0), true, scroll, after.indicator, 96)) && SUCCEEDED(composition->Commit()),
            "real DComp presents combined focus/hover/marker feedback synchronously");
        before = after;
    }
    std::cout << "Native DComp focus/hover/marker transitions=4 passed\n";
}

// Use the production focus-return route with real item construction, complete
// layout and D2D presentation. Replace only the native foreground grant and the
// model snapshot delivery. The legacy control executes the former three paint
// boundaries and is run under the same one-presentation assertion.
void CheckFocusReturnRoutes(ID2D1DeviceContext* context, IDWriteFactory* text,
    StatusBarSnapshot data, std::vector<std::string> pinned, bool legacyControl)
{
    for (std::size_t i = 0; i < data.tray.size(); ++i)
    {
        const auto color = static_cast<unsigned>(i);
        data.tray[i].width = data.tray[i].height = 1;
        data.tray[i].pixels = {0xff000000u | ((color * 71u) % 256u) << 16 |
            ((color * 37u + 65u) % 256u) << 8 | ((color * 19u + 127u) % 256u)};
    }
    constexpr UINT width = 640, height = 32;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> candidate, reference, readable, fullAtlas;
    Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &candidate)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width + 32, height + 24), nullptr, 0, properties, &fullAtlas)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                properties.pixelFormat), &readable)), "focus return targets initialize");
    PersonalizationSettings appearance;
    const StatusBarPalette palette{};
    StatusBarSettings settings; settings.pinnedTrayItems = pinned;
    const auto pixels = [&](ID2D1Bitmap1* target) {
        Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "focus return pixels copy");
        D2D1_MAPPED_RECT mapped{};
        Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "focus return pixels map");
        std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
        for (UINT y = 0; y < height; ++y) std::memcpy(result.data() + static_cast<std::size_t>(y) * width,
            mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(width) * 4);
        readable->Unmap(); return result;
    };
    const auto draw = [&](ID2D1Bitmap1* target, auto& items, auto& scroll, const StatusBarInteraction& input,
        bool keyboard, StatusBarDrawResources* resources = nullptr, bool prepared = false) {
        context->SetTarget(fullAtlas.Get()); context->SetDpi(96, 96);
        context->SetTransform(D2D1::Matrix3x2F::Translation(13.f, 9.f));
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        const auto result = DrawStatusBarContent(context, text, items, width, height, 1.f,
            appearance, palette, input.hovered, keyboard && input.focused.has_value(), input.focused.value_or(0),
            false, true, &scroll, true, prepared ? &bounds : nullptr, resources, prepared);
        const auto ended = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(ended), "focus return full frame presents");
        const D2D1_RECT_U client{13, 9, 13 + width, 9 + height};
        Check(SUCCEEDED(target->CopyFromBitmap(nullptr, fullAtlas.Get(), &client)), "focus route atlas copies to client");
    };
    unsigned routes = 0, oldTriple = 0, denied = 0, partialRoutes = 0, completeRoutes = 0;
    for (unsigned scenario = 0; scenario < 12; ++scenario)
    {
        auto currentSettings = settings;
        auto items = BuildStatusBarItems(currentSettings, data); StatusBarTrayScroll scroll;
        StatusBarInteraction input; input.hovered = 0;
        bool keyboard = false;
        draw(candidate.Get(), items, scroll, input, keyboard);
        const auto beforeItems = items; const auto beforeScroll = scroll;
        const auto beforeVisual = StatusBarInteractionVisual(input, keyboard, true, {});
        ComPtr<AtlasSurface> candidateSurface; candidateSurface.Attach(new AtlasSurface(context, candidate.Get(), true));
        Check(scroll.Maximum() > 0, "focus return fixture overflows");
        const std::string key = scenario % 2 && scenario != 7 ? pinned.front() : pinned.back();
        if (scenario == 2 || scenario == 7) std::rotate(currentSettings.pinnedTrayItems.begin(),
            currentSettings.pinnedTrayItems.begin() + 3, currentSettings.pinnedTrayItems.end());
        if (scenario == 3 || scenario == 5) std::erase(currentSettings.pinnedTrayItems, key);
        if (scenario == 5 || scenario == 8) currentSettings.tray = false;
        auto nextData = data;
        nextData.clock = L"2026/10/23   16:26"; // A real simultaneous content update.
        auto expectedData = nextData;
        const bool grant = scenario != 4;
        const bool requestedKeyboard = scenario != 1;
        auto expectedItems = items; auto expectedScroll = scroll; auto expectedInput = input;
        bool expectedKeyboard = keyboard;
        unsigned legacyFrames = 0;
        const auto legacyPaint = [&] {
            auto prior = expectedItems;
            expectedItems = BuildStatusBarItems(currentSettings, expectedData);
            expectedInput.Reconcile(prior, expectedItems);
            draw(reference.Get(), expectedItems, expectedScroll, expectedInput, expectedKeyboard);
            ++legacyFrames;
            if (scenario == 7 && legacyFrames == 1)
                std::erase_if(expectedData.tray, [&](const auto& icon) { return icon.key == key; });
        };
        legacyPaint();
        if (scenario == 7)
            legacyPaint(); // Its synchronous preparation request settles before reveal too.
        if (scenario == 8)
        {
            expectedData.clock = L"2026/10/23   16:29";
            legacyPaint(); // The already-issued reentrant content paint.
        }
        if (scenario == 9)
        {
            std::erase_if(expectedData.tray, [&](const auto& icon) { return icon.key == key; });
            expectedData.clock = L"2026/10/23   16:30";
            legacyPaint(); // Reconcile a removed visible target before native restoration.
        }
        for (const auto& item : expectedItems)
            if (item.icon && item.icon->key == key && expectedScroll.Reveal(item.contentBounds))
            { legacyPaint(); break; }
        const auto expectedIndex = FindStatusBarTrayFocus(expectedItems, key);
        const auto expectedNativeTarget = expectedIndex ?
            std::optional{StatusBarTargetOf(expectedItems[*expectedIndex])} : std::nullopt;
        const RECT expectedNativeBounds = expectedIndex ? expectedItems[*expectedIndex].bounds : RECT{};
        if (expectedIndex && grant)
        {
            expectedInput.focused = expectedIndex; expectedKeyboard = requestedKeyboard; expectedInput.hovered.reset();
            if (scenario == 6)
            {
                std::erase_if(expectedData.tray, [&](const auto& icon) { return icon.key == key; });
                expectedData.clock = L"2026/10/23   16:27";
            }
            legacyPaint();
        }
        else if (expectedIndex)
        {
            expectedData.clock = L"2026/10/23   16:31";
            legacyPaint(); // A failed native grant can still synchronously request Paint.
        }
        if (legacyFrames == 3) ++oldTriple;
        unsigned frames = 0, restores = 0, leaves = 0, nestedDrawRequests = 0, paintRequests = 0;
        StatusBarDrawResources preparedResources;
        bool fallbackPrepared = false;
        StatusBarPresentationPreparation preparation;
        const auto activationPaint = [&] {
            ++paintRequests;
            if (preparation.Defer()) { ++nestedDrawRequests; return; }
            draw(candidate.Get(), items, scroll, input, keyboard); ++frames;
        };
        unsigned preparations = 0;
        const auto prepare = [&] {
            const auto prior = items;
            items = BuildStatusBarItems(currentSettings, nextData);
            input.Reconcile(prior, items);
            auto result = preparedResources.PrepareText(text, 1.f);
            if (SUCCEEDED(result)) result = ArrangeStatusBarContent(text, preparedResources.format.Get(), items,
                width, height, 1.f, false, true, scroll, true);
            if (scenario == 11 && !preparedResources.iconFace)
            {
                fallbackPrepared = true;
                fluentFallbackForControl = false; // It recovers at the old final Paint opportunity.
            }
            ++preparations;
            if (scenario == 7 && preparations == 1)
            {
                std::erase_if(nextData.tray, [&](const auto& icon) { return icon.key == key; });
                activationPaint(); // Controlled reentry at the fresh-snapshot boundary.
            }
            if (scenario == 8 && preparations == 1)
            {
                nextData.clock = L"2026/10/23   16:29";
                activationPaint(); // No focus target; the content request still needs sampling.
            }
            if (scenario == 9 && preparations == 1)
            {
                std::erase_if(nextData.tray, [&](const auto& icon) { return icon.key == key; });
                nextData.clock = L"2026/10/23   16:30";
                activationPaint();
            }
            Check(SUCCEEDED(result), "production focus layout prepares");
        };
        if (!legacyControl)
        {
            const auto dispatched = DispatchStatusBarTrayFocusReturn(key, requestedKeyboard, items, scroll, input, keyboard,
                preparation, prepare,
                [&] {
                    ++restores;
                    const auto visible = FindStatusBarTrayFocus(items, key);
                    const auto target = visible ? std::optional{StatusBarTargetOf(items[*visible])} : std::nullopt;
                    Check(target == expectedNativeTarget && (!visible ||
                        EqualRect(&items[*visible].bounds, &expectedNativeBounds)),
                        "reveal refresh sees the same native focus target before restoration");
                    if (scenario == 11)
                    {
                        preparedResources.Reset();
                        fluentFallbackForControl = true; // Real non-Fluent collection at the native activation sample.
                    }
                    if (grant) activationPaint();
                    else { nextData.clock = L"2026/10/23   16:31"; activationPaint(); }
                    if (grant && scenario == 6)
                    {
                        std::erase_if(nextData.tray, [&](const auto& icon) { return icon.key == key; });
                        nextData.clock = L"2026/10/23   16:27";
                    }
                    return grant;
                },
                [&] { ++leaves; input.hovered.reset(); },
                [&] {
                    if (scenario == 11)
                        Check(fallbackPrepared && preparedResources.iconFace && preparedResources.fontBuilds >= 3 &&
                            !fluentFallbackForControl, "native activation fallback recovers at final sampling before presentation");
                    if (scenario == 0)
                    {
                        RejectFocusUpdateSurface rejected;
                        const RECT damage{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
                        Check(DrawStatusBarContentUpdate(&rejected, text, items, damage, width, height, 1.f,
                            appearance, palette, input.hovered, keyboard && input.focused.has_value(),
                            input.focused.value_or(0), false, scroll, {}, 96) == E_INVALIDARG &&
                            rejected.attempts == 1 && rejected.ends == 0,
                            "real retained draw reaches the unsupported backend boundary");
                    }
                    if (scenario != 0 && scenario != 11 &&
                        RetainStatusBarPreparedLayout(items, beforeItems, beforeScroll, scroll, false, true))
                    {
                        const auto visual = StatusBarInteractionVisual(input, keyboard, true, {});
                        auto damage = StatusBarContentDamage(items, beforeItems, width, height, 1.f, scroll);
                        for (const auto& feedback : StatusBarInteractionDamageRegions(items, beforeVisual, visual, width, height))
                            AddStatusBarInteractionDamage(damage, feedback);
                        if (beforeScroll.offset != scroll.offset) AddStatusBarInteractionDamage(damage, scroll.viewport);
                        Check(!damage.empty() && SUCCEEDED(DrawStatusBarContentUpdates(candidateSurface.Get(), text,
                            items, damage, width, height, 1.f, appearance, palette, visual.hovered,
                            visual.focused.has_value(), visual.focused.value_or(0), false, scroll, {}, 96, &preparedResources)),
                            "prepared focus route draws actual combined content/interaction damage");
                        ++partialRoutes;
                    }
                    else { draw(candidate.Get(), items, scroll, input, keyboard, &preparedResources, true); ++completeRoutes; }
                    ++frames;
                });
            Check(dispatched == (expectedIndex.has_value() && grant),
                "production focus route records the same native restoration result");
        }
        else
        {
            // Executed old fault route under the candidate assertion, not a
            // source-text/parameter mutation or fabricated counter-only stub.
            auto controlData = nextData;
            const auto controlPaint = [&] {
                const auto prior = items;
                items = BuildStatusBarItems(currentSettings, controlData);
                input.Reconcile(prior, items);
                draw(candidate.Get(), items, scroll, input, keyboard); ++frames;
                if (scenario == 7 && frames == 1)
                    std::erase_if(controlData.tray, [&](const auto& icon) { return icon.key == key; });
            };
            controlPaint();
            if (scenario == 7) controlPaint();
            if (scenario == 8)
            {
                controlData.clock = L"2026/10/23   16:29";
                controlPaint();
            }
            if (scenario == 9)
            {
                std::erase_if(controlData.tray, [&](const auto& icon) { return icon.key == key; });
                controlData.clock = L"2026/10/23   16:30";
                controlPaint();
            }
            for (const auto& item : items)
                if (item.icon && item.icon->key == key && scroll.Reveal(item.contentBounds))
                { controlPaint(); break; }
            if (const auto index = FindStatusBarTrayFocus(items, key); index && grant)
            {
                input.focused = index; keyboard = requestedKeyboard; input.hovered.reset();
                if (scenario == 6)
                {
                    std::erase_if(controlData.tray, [&](const auto& icon) { return icon.key == key; });
                    controlData.clock = L"2026/10/23   16:27";
                }
                controlPaint();
            }
            else if (index)
            {
                controlData.clock = L"2026/10/23   16:31";
                controlPaint();
            }
        }
        const auto observed = pixels(candidate.Get()), expected = pixels(reference.Get());
        for (std::size_t pixel = 0; pixel < observed.size(); ++pixel)
        {
            if (observed[pixel] == expected[pixel]) continue;
            const POINT location{static_cast<LONG>(pixel % width), static_cast<LONG>(pixel / width)};
            bool equivalent = PtInRect(&scroll.viewport, location) != FALSE;
            for (unsigned shift = 0; shift < 32; shift += 8)
                equivalent = equivalent && std::abs(static_cast<int>((observed[pixel] >> shift) & 255) -
                    static_cast<int>((expected[pixel] >> shift) & 255)) <= 2;
            Check(equivalent, "focus return final pixels match old complete route");
        }
        Check(input.focused == expectedInput.focused && input.hovered == expectedInput.hovered && keyboard == expectedKeyboard &&
            scroll.offset == expectedScroll.offset && EqualRect(&scroll.viewport, &expectedScroll.viewport),
            "focus identity, hover, visibility and scrolling match the complete route");
        for (std::size_t i = 0; i < items.size(); ++i)
            Check(EqualRect(&items[i].bounds, &expectedItems[i].bounds) &&
                EqualRect(&items[i].contentBounds, &expectedItems[i].contentBounds), "focus return geometry matches complete route");
        if (!legacyControl)
        {
            Check(restores == static_cast<unsigned>(expectedIndex.has_value()), "only a visible tray/overflow target can request focus");
            Check(leaves == static_cast<unsigned>(expectedIndex.has_value() && grant), "Leave follows only successful focus restore");
            Check(nestedDrawRequests == paintRequests && nestedDrawRequests >= leaves && !preparation.Active(),
                "synchronous activation paint is collected only within the preparation boundary");
            Check(preparations == legacyFrames + static_cast<unsigned>(expectedIndex.has_value() && grant),
                "native activation sampling and explicit final sampling retain separate recovery opportunities");
            if (!grant) ++denied;
        }
        if (frames != 1)
            std::cerr << "focus return scenario=" << scenario << " actual frames=" << frames
                << " old complete frames=" << legacyFrames << " offset=" << scroll.offset << '\n';
        Check(frames == 1, "focus return must present once after reconcile/reveal/final interaction state");
        ++routes;
    }
    Check(routes == 12 && oldTriple && denied, "reorder/overflow/missing/denied, pending content and old triple-paint routes executed");
    if (!legacyControl)
    {
        Check(partialRoutes && completeRoutes, "focus routes execute retained drawing and complete fallbacks");
        auto items = BuildStatusBarItems(settings, data); StatusBarTrayScroll scroll;
        StatusBarInteraction input; input.hovered = 0; bool keyboard = false;
        draw(candidate.Get(), items, scroll, input, keyboard);
        unsigned restores = 0, prepares = 0, presents = 0;
        StatusBarPresentationPreparation preparation;
        const auto result = DispatchStatusBarTrayFocusReturn(pinned.front(), true, items, scroll, input, keyboard,
            preparation, [&] { ++prepares; /* Renderer unavailable: retain its previous geometry. */ },
            [&] { ++restores; return true; }, [&] { input.hovered.reset(); }, [&] { ++presents; });
        Check(result && restores == 1 && prepares == 2 && presents == 1 && keyboard && !input.hovered && input.focused &&
            items[*input.focused].icon && items[*input.focused].icon->key == pinned.front(),
            "unavailable rendering does not suppress or defer actual native focus restoration");
    }
    std::cout << "Focus return routes=" << routes << " old triple frames=" << oldTriple << " native focus denied=" << denied
        << " retained routes=" << partialRoutes << " complete routes=" << completeRoutes << '\n';
}
