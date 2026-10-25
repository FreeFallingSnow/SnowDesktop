    // Include after native_foreground_culling_access.inc.
    // The DComp allocation boundary uses R06's retained WARP surface; input,
    // DrawDesktopForeground, tooltip repair and commit routing remain real.
    static void CheckDockHoverFrameRouting(DesktopApp& app, bool legacy)
    {
        const auto check = [](bool ok, const std::string& message) {
            if (!ok) throw std::runtime_error("R08: " + message);
        };
        struct MotionScope
        {
            int mode = snowdesktop::animation::detail::mode.load();
            int popup = snowdesktop::animation::detail::popupEffect.load();
            int speed = snowdesktop::animation::detail::speed.load();
            int limit = snowdesktop::animation::detail::frameLimit.load();
            int effect = snowdesktop::animation::detail::windowEffect.load();
            int position = snowdesktop::animation::detail::dockPosition.load();
            bool energy = snowdesktop::animation::detail::energySaver.load();
            bool battery = snowdesktop::animation::detail::onBattery.load();
            ~MotionScope()
            {
                snowdesktop::animation::SetRuntimePreferences(mode, popup, speed, limit,
                    energy, battery, effect, position);
            }
        } motionScope;
        snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
            snowdesktop::animation::Scale, 1, 0, false, false, 1);
        const HWND previousWindow = app.hwnd_;
        const HWND geometry = CreateWindowExW(0, L"STATIC", L"R08 geometry",
            WS_POPUP, -32000, -32000, 3600, 800, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        check(geometry && !IsWindowVisible(geometry), "create never-shown test-owned geometry window");
        struct GeometryScope
        {
            DesktopApp& app; HWND previous; HWND window;
            ~GeometryScope()
            {
                app.shellHoverTraceActive_ = false;
                app.uiAnimationScheduler_.CancelAll();
                app.dockMagnificationAnimationFrameToken_ = 0;
                app.persistentDockHosts_.clear();
                app.hwnd_ = previous;
                DestroyWindow(window);
            }
        } geometryScope{app, previousWindow, geometry};
        app.hwnd_ = geometry;
        RECT client{}; GetClientRect(geometry, &client);
        const UINT width = static_cast<UINT>(client.right);
        const UINT height = static_cast<UINT>(client.bottom);
        check(width == 3600 && height == 800, "private client geometry must match the explicit multi-screen scene");
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.desktopForegroundCompositionVisual_)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))), "initialize D2D/DWrite/DComp");
        check(SUCCEEDED(app.dcompDevice_->CreateTargetForHwnd(geometry, FALSE, &app.dcompTarget_)) &&
            SUCCEEDED(app.dcompTarget_->SetRoot(app.dcompVisual_.Get())) &&
            SUCCEEDED(app.dcompVisual_->AddVisual(app.desktopForegroundCompositionVisual_.Get(), TRUE, nullptr)),
            "attach composition to the never-shown test-owned window");
        check(app.uiAnimationScheduler_.Initialize(), "initialize production animation scheduler");
        ComPtr<ID2D1DeviceContext> referenceContext;
        check(SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &referenceContext)),
            "create independent complete-reference context");
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> retained, complete;
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &retained)) &&
            SUCCEEDED(referenceContext->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &complete)),
            "allocate independent retained and full-reference pixel targets");
        ComPtr<ForegroundSurface> surface;
        surface.Attach(new ForegroundSurface(app.d2dContext_.Get(), retained.Get()));
        app.desktopForegroundCompositionSurface_ = surface;
        app.desktopForegroundCompositionWidth_ = width;
        app.desktopForegroundCompositionHeight_ = height;
        app.personalizationSettings_ = PersonalizationSettings::DarkPreset();
        app.personalizationSettings_.glassEnabled = app.personalizationSettings_.acrylicEnabled = false;
        app.collectionPopupAppearance_ = app.personalizationSettings_;
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.followComponentAppearance = true;
        app.dockSettings_.showWindowsButton = false;
        app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showFrequentItems = false;
        app.dockSettings_.showRunningApps = false;
        app.dockSettings_.showOnlyWhenSummoned = false;
        app.dockSettings_.hoverScale = 2.0f;
        app.customDesktopVisible_ = true;
        app.desktopItemsReady_ = true;
        app.floatingDockHostActive_ = false;
        app.gridPages_.front().bounds = app.gridPages_.front().workArea =
            app.gridPages_.front().visualWorkArea = client;
        app.layoutWorkArea_ = client;
        app.virtualWidth_ = static_cast<int>(width);
        app.virtualHeight_ = static_cast<int>(height);
        app.iconWork_.Stop();
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        app.widgets_.clear(); app.dockEntries_.clear();
        for (int index = 0; index < 3; ++index)
        {
            DesktopItem item;
            item.name = L"R08 long title " + std::to_wstring(index) + std::wstring(40, L'W');
            item.parsingName = L"r08-private-item-" + std::to_wstring(index);
            item.layoutKey = ToUpperInvariant(item.parsingName);
            item.gridCell = {kDockPageId, index, 0};
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = -16;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            item.iconBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            check(item.iconBitmap && pixels, "allocate opaque test-owned Dock icon");
            std::fill_n(static_cast<std::uint32_t*>(pixels), 256,
                0xff4070a0u + static_cast<std::uint32_t>(index) * 0x00102010u);
            item.iconBitmapSize = {16, 16}; item.iconState = IconState::FullQuality;
            DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = item.layoutKey;
            app.items_.push_back(std::move(item)); app.dockEntries_.push_back(std::move(entry));
        }
        app.RefreshDesktopItemIndexCache();
        const std::array<RECT, 3> areas{{{100, 180, 900, 680}, {1350, 180, 2150, 680}, {2600, 180, 3400, 680}}};
        const POINT outside{1100, 40};
        const auto read = [&](ID2D1DeviceContext* context, ID2D1Bitmap1* bitmap) {
            auto readProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                bitmap->GetPixelFormat(), 96, 96);
            ComPtr<ID2D1Bitmap1> cpu;
            check(SUCCEEDED(context->CreateBitmap(bitmap->GetPixelSize(), nullptr, 0, &readProperties, &cpu)) &&
                SUCCEEDED(cpu->CopyFromBitmap(nullptr, bitmap, nullptr)), "copy real rendered pixels");
            D2D1_MAPPED_RECT mapped{};
            check(SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map pixel reference");
            std::vector<std::uint32_t> result(static_cast<size_t>(width) * height);
            for (UINT row = 0; row < height; ++row)
                std::memcpy(result.data() + static_cast<size_t>(row) * width,
                    mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(width) * 4);
            check(SUCCEEDED(cpu->Unmap()), "unmap pixel reference");
            return result;
        };
        const auto paintedBounds = [&](const std::vector<std::uint32_t>& pixels, bool greenOnly = false) {
            RECT bounds{static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
            for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const auto pixel = pixels[static_cast<size_t>(y) * width + x];
                const auto red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
                if ((pixel >> 24) != 0 && (!greenOnly || (green > red + 12 && green > blue + 12)))
                {
                    bounds.left = std::min(bounds.left, static_cast<LONG>(x));
                    bounds.top = std::min(bounds.top, static_cast<LONG>(y));
                    bounds.right = std::max(bounds.right, static_cast<LONG>(x + 1));
                    bounds.bottom = std::max(bounds.bottom, static_cast<LONG>(y + 1));
                }
            }
            return bounds;
        };
        std::array<DockContainer*, 3> docks{};
        const auto reference = [&] {
            std::array<DockContainer::PresentationState, 3> before;
            for (size_t index = 0; index < docks.size(); ++index) before[index] = docks[index]->CapturePresentationState();
            referenceContext->SetTarget(complete.Get());
            referenceContext->SetDpi(96, 96); referenceContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            referenceContext->SetTransform(D2D1::Matrix3x2F::Identity());
            referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
            app.brushCache_.clear(); app.brushCacheContext_ = referenceContext.Get();
            app.DrawDesktopForeground(referenceContext.Get(), app.desktopIconsHidden_);
            check(SUCCEEDED(referenceContext->EndDraw()), "draw independent complete foreground reference");
            referenceContext->SetTarget(nullptr);
            app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
            const auto actualPixels = read(app.d2dContext_.Get(), retained.Get());
            const auto completePixels = read(referenceContext.Get(), complete.Get());
            if (actualPixels != completePixels)
            {
                const auto mismatch = std::mismatch(actualPixels.begin(), actualPixels.end(), completePixels.begin());
                const size_t index = static_cast<size_t>(mismatch.first - actualPixels.begin());
                std::cerr << "R08 pixel mismatch x=" << index % width << " y=" << index / width
                    << " actual=" << *mismatch.first << " reference=" << *mismatch.second
                    << " pointer=" << app.lastMousePoint_.x << ',' << app.lastMousePoint_.y
                    << " effect=" << app.dockSettings_.hoverEffect << " position=" << static_cast<int>(app.dockSettings_.position) << '\n';
            }
            check(actualPixels == completePixels,
                "retained partial foreground must equal the complete pixel reference");
            for (size_t index = 0; index < docks.size(); ++index)
            {
                const auto after = docks[index]->CapturePresentationState();
                check(EqualRect(&before[index].reservedArea, &after.reservedArea) &&
                    EqualRect(&before[index].focusRect, &after.focusRect) && before[index].scrollOffset == after.scrollOffset &&
                    before[index].entry.IsAnimating() == after.entry.IsAnimating() &&
                    before[index].entry.FocusScale(2.0f) == after.entry.FocusScale(2.0f) &&
                    before[index].single.IsAnimating() == after.single.IsAnimating() &&
                    before[index].single.Scale() == after.single.Scale() &&
                    before[index].single.PointerAxis() == after.single.PointerAxis() &&
                    EqualRect(&before[index].single.CurrentRect(), &after.single.CurrentRect()),
                    "full-reference rendering cannot change focus, geometry or animation state");
            }
        };
        const auto clearTrace = [&] {
            app.shellHoverTraceActive_ = true;
            app.shellHoverTraceCount_ = app.shellHoverTraceWriteIndex_ = 0;
            app.shellHoverTraceMenuEndTick_ = 0;
        };
        const auto commitCount = [&](DesktopApp::ShellHoverTraceEvent event) {
            size_t count = 0;
            for (size_t index = 0; index < app.shellHoverTraceCount_; ++index)
                if (app.shellHoverTrace_[index].event == event) ++count;
            return count;
        };
        const auto reset = [&] {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.dockAnimationDesktopDirty_ = {};
            app.lastMousePoint_ = outside; app.desktopForegroundTooltipDamage_ = {};
            for (auto* dock : docks)
            {
                auto state = dock->CapturePresentationState();
                state.focusRect = {}; state.entry = {}; state.single = {};
                check(dock->RestorePresentationState(state), "reset the existing production Dock timeline");
            }
            check(app.RenderDesktopForegroundComposition(nullptr, app.desktopIconsHidden_), "initialize complete scene pixels");
            surface->updates.clear(); clearTrace();
        };
        const auto point = [&](size_t owner, size_t slot) {
            const auto& slots = docks[owner]->GetSlots();
            check(slot < slots.size(), "real Dock slots must contain the requested entry");
            const RECT bounds = slots[slot]->GetBounds();
            return POINT{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        };
        const auto scope = [&](bool allowSecond, bool allowThird) {
            for (const RECT& update : surface->updates)
            {
                RECT overlap{};
                const RECT second = docks[1]->GetBounds(), third = docks[2]->GetBounds();
                check(allowSecond || !IntersectRect(&overlap, &update, &second),
                    "unrelated screen must not be included in the hover update");
                check(allowThird || !IntersectRect(&overlap, &update, &third),
                    "independent host must not be included in the desktop hover update");
            }
        };
        bool loggedSample = false;
        const auto move = [&](POINT sample, bool allowSecond, bool allowThird, bool presents) {
            surface->updates.clear(); clearTrace();
            app.OnMouseMoveAt(0, sample);
            if (legacy)
            {
                // Frozen pre-R08 damage-routing block, replayed after the real
                // input has supplied the same hover/model state. It is only a
                // negative control, never the complete-reference expectation.
                surface->updates.clear(); clearTrace();
                std::cout << "R08 negative: executing frozen all-Dock hover damage route\n";
                RECT dirty{};
                for (const auto& container : app.containers_)
                {
                    const auto* dock = dynamic_cast<const DockContainer*>(container.get());
                    if (!dock) continue;
                    const RECT envelope = snowdesktop::floating_dock_rules::ExpandHostForTitleLayer(
                        dock->GetInteractiveBounds(), app.dockSettings_.position);
                    if (IsRectEmpty(&dirty)) dirty = envelope;
                    else UnionRect(&dirty, &dirty, &envelope);
                }
                InflateRect(&dirty, 4, 4);
                check(app.PresentDesktopForegroundComposition(dirty), "execute the frozen original damage route");
            }
            if (!loggedSample && !surface->updates.empty())
            {
                const RECT observed = surface->updates.front();
                std::cout << "R08 " << (legacy ? "legacy" : "candidate") << " effect=" << app.dockSettings_.hoverEffect
                    << " position=" << static_cast<int>(app.dockSettings_.position) << " update=["
                    << observed.left << ',' << observed.top << ',' << observed.right << ',' << observed.bottom
                    << "] draws=" << surface->updates.size() << " queued="
                    << commitCount(DesktopApp::ShellHoverTraceEvent::CommitQueued) << " flushed="
                    << commitCount(DesktopApp::ShellHoverTraceEvent::CommitFlushed) << '\n';
                loggedSample = true;
            }
            scope(allowSecond, allowThird);
            check(presents ? !surface->updates.empty() : surface->updates.empty(),
                "input must update only when its visual state requires presentation");
            check(commitCount(DesktopApp::ShellHoverTraceEvent::CommitQueued) == static_cast<size_t>(presents) &&
                commitCount(DesktopApp::ShellHoverTraceEvent::CommitFlushed) == static_cast<size_t>(presents) &&
                !app.compositionCommitPending_, "hover must retain one synchronous queue/flush boundary");
            check(!app.compositionRenderRecoveryPending_ && !app.graphicsDeviceRecovery_.Pending() &&
                app.desktopForegroundCompositionSurface_.Get() == surface.Get(),
                "hover presentation must succeed without reset or graphics recovery");
            check(app.lastMousePoint_.x == sample.x && app.lastMousePoint_.y == sample.y &&
                app.dockEntries_.size() == 3 && !app.dragSession_.IsActive(), "input preserves pointer/model/gesture state");
            reference();
        };
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {0, 1, 2})
        {
            app.persistentDockHosts_.clear(); app.containers_.clear();
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = position == DockPosition::Top;
            loggedSample = false;
            app.desktopIconsHidden_ = false; app.dockSettings_.keepWhenDesktopHidden = false;
            for (size_t index = 0; index < docks.size(); ++index)
            {
                auto dock = std::make_unique<DockContainer>(&app, &app.dockEntries_, areas[index]);
                docks[index] = dock.get(); app.containers_.push_back(std::move(dock));
            }
            auto host = std::make_unique<DesktopApp::PersistentDockHost>();
            host->owner = &app; host->container = docks[2]; host->active = true;
            host->guideOcclusionRect = app.usageGuideFrame_;
            app.persistentDockHosts_.push_back(std::move(host));
            reset();
            const auto restingPixels = read(app.d2dContext_.Get(), retained.Get());
            const POINT first = point(0, 0);
            move(first, false, false, true);
            const auto* firstEntry = docks[0]->EntryAtPoint(first);
            check(app.GetDockContainerAtPoint(first) == docks[0] && firstEntry &&
                firstEntry->GetEntryIndex() == 0 && firstEntry->GetTitle() == app.items_.front().name,
                "known first entry and its nonempty long title must reach the real hit and indexed title providers");
            const auto hoverPixels = read(app.d2dContext_.Get(), retained.Get());
            RECT visibleTitle{}, title = docks[0]->GetHoveredTitleBounds(first);
            check(IntersectRect(&visibleTitle, &title, &client) != FALSE,
                "the hovered long title must have visible geometry");
            bool titlePixelsChanged = false;
            for (LONG y = visibleTitle.top; y < visibleTitle.bottom; ++y)
            for (LONG x = visibleTitle.left; x < visibleTitle.right; ++x)
            {
                const size_t index = static_cast<size_t>(y) * width + static_cast<size_t>(x);
                titlePixelsChanged = titlePixelsChanged || restingPixels[index] != hoverPixels[index];
            }
            check(restingPixels != hoverPixels && titlePixelsChanged,
                "real hover and long-title pixels must differ from the complete resting scene");
            move(first, false, false, false);
            auto settled = docks[0]->CapturePresentationState();
            settled.focusRect = docks[0]->GetSlots().front()->GetBounds();
            const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
            if (effect == 1)
            {
                settled.single.SetTarget(settled.focusRect, now - 1000, 1.0,
                    position == DockPosition::Left || position == DockPosition::Right ? first.y : first.x);
                settled.single.Advance(now);
            }
            else if (effect == 2) { settled.entry.SetHovered(true, now - 1000, 1.0); settled.entry.Advance(now); }
            check(docks[0]->RestorePresentationState(settled), "supply an already-expanded production timeline");
            move({first.x + 1, first.y + 1}, false, false, true);
            move(point(0, 2), false, false, true);
            const RECT interaction = docks[0]->GetInteractiveBounds();
            POINT retainedPoint = point(0, 2), nextRetained = retainedPoint;
            switch (position)
            {
            case DockPosition::Bottom: retainedPoint.y = interaction.top - 1; nextRetained = retainedPoint; --nextRetained.y; break;
            case DockPosition::Top: retainedPoint.y = interaction.bottom; nextRetained = retainedPoint; ++nextRetained.y; break;
            case DockPosition::Left: retainedPoint.x = interaction.right; nextRetained = retainedPoint; ++nextRetained.x; break;
            case DockPosition::Right: retainedPoint.x = interaction.left - 1; nextRetained = retainedPoint; --nextRetained.x; break;
            }
            move(retainedPoint, false, false, true);
            move(nextRetained, false, false, true);
            move(outside, false, false, true);
            if (effect == 1)
            {
                check(docks[0]->IsMagnificationAnimating() && app.dockMagnificationAnimationFrameToken_,
                    "leaving single magnification starts the real exit track");
                surface->updates.clear();
                const ULONGLONG deadline = GetTickCount64() + 2000;
                while (app.dockMagnificationAnimationFrameToken_ && GetTickCount64() < deadline)
                {
                    check(WaitForSingleObject(app.uiAnimationScheduler_.WaitHandle(), 50) != WAIT_FAILED,
                        "wait on the actual bounded animation scheduler");
                    app.uiAnimationScheduler_.DispatchDue();
                }
                check(!app.dockMagnificationAnimationFrameToken_ && !docks[0]->IsMagnificationAnimating() &&
                    !surface->updates.empty(), "stationary leave must present the terminal clearing frame");
                scope(false, false); reference();
            }
            reset(); move(first, false, false, true);
            const POINT second = point(1, 1);
            move(second, true, false, true);
            check(app.GetDockContainerAtPoint(second) == docks[1], "cross-screen input transfers hit ownership");
            if (effect == 1)
            {
                reset();
                auto exiting = docks[1]->CapturePresentationState();
                const RECT base = docks[1]->GetSlots().front()->GetBounds();
                const double exitNow = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                const int axis = position == DockPosition::Left || position == DockPosition::Right
                    ? (base.top + base.bottom) / 2 : (base.left + base.right) / 2;
                exiting.single.SetTarget(base, exitNow - 200, 1.0, axis);
                exiting.single.Advance(exitNow - 100);
                exiting.single.SetTarget({}, exitNow - 40, 1.0, axis);
                exiting.single.Advance(exitNow);
                check(docks[1]->RestorePresentationState(exiting) && docks[1]->IsMagnificationAnimating(),
                    "supply a separate Dock's real mid-exit distribution");
                move(first, true, false, true);
                const RECT secondBounds = docks[1]->GetBounds();
                check(std::any_of(surface->updates.begin(), surface->updates.end(), [&](const RECT& update) {
                    RECT overlap{}; return IntersectRect(&overlap, &update, &secondBounds) != FALSE;
                }), "a separate live exit track must retain its clearing presentation");
            }
            reset(); move(point(2, 0), false, false, false);
            app.persistentDockHosts_.front()->active = false;
            reset(); move(point(2, 0), false, true, true);
            app.persistentDockHosts_.front()->active = true;
            app.desktopIconsHidden_ = true;
            reset(); move(first, false, false, false);
            app.dockSettings_.keepWhenDesktopHidden = true;
            reset(); move(first, false, false, true);

            // Starting and releasing a sort must retain the existing hover,
            // including a half-entered timeline. Exercise the real Dock model,
            // visual hit test and D2D insertion renderer without desktop input.
            for (const bool external : {false, true})
            {
                app.desktopIconsHidden_ = false;
                reset();
                app.lastMousePoint_ = first;
                const RECT base = docks[0]->GetSlots().front()->GetBounds();
                (void)docks[0]->GetElementVisualRect(base, first);
                auto hovering = docks[0]->CapturePresentationState();
                const double hoverNow = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                if (effect == 1)
                {
                    hovering.single = {};
                    hovering.single.SetTarget(base, hoverNow - 40, 1.0,
                        position == DockPosition::Left || position == DockPosition::Right ? first.y : first.x);
                    hovering.single.Advance(hoverNow);
                }
                else if (effect == 2)
                {
                    hovering.entry = {};
                    hovering.entry.SetHovered(true, hoverNow - 80, 1.0);
                    hovering.entry.Advance(hoverNow);
                }
                check(docks[0]->RestorePresentationState(hovering), "restore partial pre-drag hover");
                const RECT beforeDrag = docks[0]->GetElementVisualRect(base, first);
                auto* source = docks[0]->GetSlots()[2]->GetItem();
                app.dragSession_.Begin(external ? nullptr : docks[0],
                    external ? std::vector<Item*>{} : std::vector<Item*>{source}, {}, first, first);
                const RECT duringDrag = docks[0]->GetElementVisualRect(base, first);
                check(EqualRect(&beforeDrag, &duringDrag) &&
                    docks[0]->IsMagnificationAnimating() == (effect != 0),
                    "internal and external drag entry must preserve partial hover geometry and its timeline");
                const RECT dragTitle = docks[0]->GetHoveredTitleBounds(first);
                check(IsRectEmpty(&dragTitle),
                    "drag hover must not put a title over the insertion feedback");
                docks[0]->AdvanceMagnificationAnimation(hoverNow + 1000);
                const RECT expanded = docks[0]->GetElementVisualRect(base, first);
                check(effect == 0 ? EqualRect(&base, &expanded) != FALSE :
                    docks[0]->GetElementIconSize(base, expanded) > docks[0]->GetElementIconSize(base, base),
                    "stationary drag hover must finish the configured growth effect");
                const RECT hidden = docks[0]->GetSlots().back()->GetBounds();
                const RECT hiddenVisual = docks[0]->GetElementVisualRect(hidden, first);
                check(IsRectEmpty(&hidden) && EqualRect(&hidden, &hiddenVisual),
                    "a hidden Search insertion boundary must remain empty and undisplaced during hover");
                POINT tip = first;
                if (effect != 0)
                {
                    switch (position)
                    {
                    case DockPosition::Bottom: tip.y = base.top - 1; break;
                    case DockPosition::Top: tip.y = base.bottom; break;
                    case DockPosition::Left: tip.x = base.right; break;
                    case DockPosition::Right: tip.x = base.left - 1; break;
                    }
                }
                app.lastMousePoint_ = tip;
                Slot* hit = nullptr;
                const auto region = docks[0]->HitTestDrag(tip, hit);
                check(hit == docks[0]->GetSlots().front().get() &&
                    (region == HitRegion::SortBefore || region == HitRegion::SortAfter),
                    "drag hit testing must accept the visible grown icon outside its base cell");

                referenceContext->SetTarget(complete.Get());
                referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                docks[0]->DrawInsertionPreview(referenceContext.Get(), 1);
                check(SUCCEEDED(referenceContext->EndDraw()), "render the real magnified insertion line");
                referenceContext->SetTarget(nullptr);
                const auto indicator = read(referenceContext.Get(), complete.Get());
                const RECT next = docks[0]->GetElementVisualRect(docks[0]->GetSlots()[1]->GetBounds(), tip);
                const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
                const LONG axis = vertical ? next.top : next.left;
                const POINT line = vertical ? POINT{first.x, axis} : POINT{axis, first.y};
                check(line.x >= 0 && line.y >= 0 && line.x < static_cast<LONG>(width) && line.y < static_cast<LONG>(height) &&
                    indicator[static_cast<size_t>(line.y) * width + static_cast<size_t>(line.x)] != 0,
                    "the blue insertion line must follow the animated next-slot edge rather than its original pitch");
                const RECT linePixels = paintedBounds(indicator);
                const LONG lineLength = vertical ? linePixels.right - linePixels.left : linePixels.bottom - linePixels.top;
                const LONG baseLength = (vertical ? base.right - base.left : base.bottom - base.top) - 20;
                check(effect == 0 ? std::abs(lineLength - baseLength) <= 2 : lineLength > baseLength,
                    "the insertion line must lengthen with either adjacent hover owner, including slight single-icon growth");
                check(effect != 2 || (vertical ? linePixels.bottom - linePixels.top : linePixels.right - linePixels.left) > 4,
                    "wave hover must also enlarge insertion-line thickness");

                referenceContext->SetTarget(complete.Get());
                referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                app.brushCache_.clear(); app.brushCacheContext_ = referenceContext.Get();
                app.dragSession_.UpdateTarget(docks[0], docks[0]->GetSlots().front().get(), HitRegion::Handoff);
                app.DrawDesktopForeground(referenceContext.Get(), false);
                check(SUCCEEDED(referenceContext->EndDraw()), "render green Dock handoff through the actual scene entry");
                referenceContext->SetTarget(nullptr);
                app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
                const RECT greenPixels = paintedBounds(read(referenceContext.Get(), complete.Get()), true);
                const RECT focusedVisual = docks[0]->GetElementVisualRect(base, tip);
                check(greenPixels.left <= focusedVisual.left + 1 && greenPixels.top <= focusedVisual.top + 1 &&
                    greenPixels.right >= focusedVisual.right - 1 && greenPixels.bottom >= focusedVisual.bottom - 1,
                    "the green handoff frame must enclose the full animated icon rather than its original cell");
                const RECT beforeRelease = docks[0]->GetElementVisualRect(base, tip);
                app.dragSession_.End();
                const RECT released = docks[0]->GetElementVisualRect(base, tip);
                check(EqualRect(&beforeRelease, &released), "release must not restart or collapse hover growth");
                app.lastMousePoint_ = outside;
                (void)docks[0]->GetElementVisualRect(base, outside);
                app.lastMousePoint_ = first;
                app.dragSession_.Begin(nullptr, {}, {}, first, first);
                const RECT reentered = docks[0]->GetElementVisualRect(base, first);
                check(effect == 0 || docks[0]->IsMagnificationAnimating() ||
                    docks[0]->GetElementIconSize(base, reentered) > docks[0]->GetElementIconSize(base, base),
                    "re-entering during a drag must resume the configured hover animation");
                app.dragSession_.End();
            }

            for (const bool reconcile : {false, true})
            {
                const RECT base = docks[0]->GetSlots().front()->GetBounds();
                app.lastMousePoint_ = first;
                auto menuHover = docks[0]->CapturePresentationState();
                menuHover.focusRect = base; menuHover.entry = {}; menuHover.single = {};
                const double menuNow = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                if (effect == 1)
                {
                    menuHover.single.SetTarget(base, menuNow - 200, 1.0,
                        position == DockPosition::Left || position == DockPosition::Right ? first.y : first.x);
                    menuHover.single.Advance(menuNow);
                }
                else if (effect == 2)
                { menuHover.entry.SetHovered(true, menuNow - 200, 1.0); menuHover.entry.Advance(menuNow); }
                check(docks[0]->RestorePresentationState(menuHover), "supply settled hover before menu capture");
                app.shellPopupMenuLayerDepth_ = 1;
                check(!app.IsPersistentDockHostWindow(nullptr),
                    "an uninitialized Dock host must not claim an absent mouse-capture handle");
                // The fixture's HWND is never shown and remains offscreen.
                // A live cursor cannot be on this Dock; no user window is operated.
                if (reconcile) app.ReconcileDesktopHoverState(snowdesktop::desktop_hover_rules::ReconcileMode::DeactivateOnly);
                else app.OnMouseLeave();
                check(app.lastMousePoint_.x == LONG_MIN && app.lastMousePoint_.y == LONG_MIN &&
                    app.shellPopupMenuLayerDepth_ == 1,
                    "menu capture must clear departed Dock hover before the menu closes, including fallback reconciliation");
                // STATIC supplies geometry only; query the real Dock paint
                // resolver to start its exit track before advancing the clock.
                (void)docks[0]->GetElementVisualRect(base, app.lastMousePoint_);
                docks[0]->AdvanceMagnificationAnimation(menuNow + 1000);
                const RECT cleared = docks[0]->GetElementVisualRect(base, app.lastMousePoint_);
                check(EqualRect(&cleared, &base) && !docks[0]->IsMagnificationAnimating(),
                    "hover must reach resting geometry while the context-menu session is still active");
                app.shellPopupMenuLayerDepth_ = 0;
            }
        }
        // The real release may replace the complete Dock runtime tree. Run it
        // after all rendering/menu scenes, and never reuse the fixture's old
        // Dock/Slot pointers afterwards. Double-click mode exposes an unwanted
        // click as pending state without launching an application.
        const POINT selfPoint = point(0, 0);
        app.lastMousePoint_ = selfPoint;
        auto* self = docks[0]->GetSlots().front()->GetItem();
        app.dragSession_.Begin(docks[0], {self}, app.BuildDragSourceList({self}, docks[0]), selfPoint, selfPoint);
        app.mouseDown_ = true; app.mouseDownPoint_ = selfPoint;
        app.dockPressedContainer_ = docks[0]; app.dockPressedEntry_ = 0;
        app.dockPressedWindowAction_ = snowdesktop::dock_window_rules::DockClickAction::Launch;
        app.dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
        const bool singleClick = app.dockSettings_.singleClickLaunchItems;
        app.dockSettings_.singleClickLaunchItems = false;
        check(!app.HandleDockClickRelease(selfPoint) && app.dragSession_.IsActive(),
            "an active drag must never enter Dock click execution even with stale pressed state");
        // Live window discovery is an independent maintenance boundary, not
        // part of this private model's click/drop contract.
        app.dockRunningWindowsRefreshTick_ = GetTickCount() | 1u;
        app.OnLeftButtonUpAt(0, selfPoint, false);
        check(!app.dragSession_.HasContext() && app.dockPendingDoubleClickEntry_ == static_cast<size_t>(-1) &&
            app.dockEntries_.size() == 3 && app.dockEntries_.front().reference == app.items_.front().layoutKey,
            "the actual button-up route must end a self drop without arming a click or changing Dock order");
        app.dockSettings_.singleClickLaunchItems = singleClick;
        app.shellHoverTraceActive_ = false;
        app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
        app.persistentDockHosts_.clear(); app.containers_.clear();
        app.CancelUiAnimationFrame();
        app.uiAnimationScheduler_.Shutdown();
        app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
        app.ResetDesktopForegroundComposition();
        app.iconGlassBackdrop_.clear(); app.iconBitmapStyles_.clear(); app.d2dIconCache_.clear();
        app.itemTextEffectContext_.Reset();
        app.dcompTarget_.Reset(); app.dcompVisual_.Reset(); app.dcompDevice_.Reset();
        app.dwriteFactory_.Reset();
        app.d2dContext_.Reset(); app.d2dDevice_.Reset(); app.d2dFactory_.Reset(); app.d3dDevice_.Reset();
        std::cout << "R08 actual pointer/foreground routing, retained/full WARP pixels, hits, state, commits and exit frames passed\n";
    }
