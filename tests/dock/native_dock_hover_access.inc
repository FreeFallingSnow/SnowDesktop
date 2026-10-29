    // Include after native_foreground_culling_access.inc.
    // The DComp allocation boundary uses R06's retained WARP surface; input,
    // DrawDesktopForeground, tooltip repair and commit routing remain real.
    static inline POINT dockHoverHandoffScreenPoint{};
    static inline HWND dockHoverHandoffWindow = nullptr;
    static BOOL WINAPI DockHoverHandoffCursor(POINT* point)
    {
        if (!point) return FALSE;
        *point = dockHoverHandoffScreenPoint;
        return TRUE;
    }
    static HWND WINAPI DockHoverHandoffHit(POINT)
    {
        return dockHoverHandoffWindow;
    }
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
                // A side wave can extend past this finite bitmap (CDB: x=-24).
                // Every visible edge remains strict; off-image pixels cannot
                // be observed by a readback starting at client x/y = 0.
                RECT visibleFocus{};
                check(IntersectRect(&visibleFocus, &focusedVisual, &client) && !IsRectEmpty(&greenPixels) &&
                    greenPixels.left <= visibleFocus.left + 1 && greenPixels.top <= visibleFocus.top + 1 &&
                    greenPixels.right >= visibleFocus.right - 1 && greenPixels.bottom >= visibleFocus.bottom - 1,
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

            // Component handles use WidgetAction, with no DragSession items.
            // Route real input and scene rendering through that second owner.
            for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FolderMapping})
            {
                reset();
                DesktopWidget widget;
                widget.id = L"r08-moving-component"; widget.type = type;
                widget.gridCell = {app.gridPages_.front().id, 5, 1}; widget.gridSpan = {1, 1};
                widget.bounds = {1020, 80, 1100, 160};
                app.widgets_.push_back(std::move(widget));
                app.mouseDownWidgetIndex_ = 0; app.mouseDown_ = true;
                app.mouseDownPoint_ = outside;
                app.widgetDragOriginalCell_ = app.widgets_.front().gridCell;
                app.widgetDragOriginalSpan_ = app.widgets_.front().gridSpan;
                app.widgetAction_ = DesktopApp::WidgetAction::Move;
                const auto input = [&](POINT sample) {
                    // Suppress only the optional native tooltip allocation.
                    // The actual target/gesture route and D2D scene stay real.
                    auto factory = std::move(app.dwriteFactory_);
                    app.OnMouseMoveAt(MK_LBUTTON, sample);
                    app.dwriteFactory_ = std::move(factory);
                };
                input(first);
                check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == docks[0] &&
                    !app.dragSession_.HasContext(), "component input must reach Dock insertion without an item drag");
                check(app.RenderDesktopForegroundComposition(nullptr, false), "render actual component-to-Dock scene");
                app.EnsureUiAnimationFrame();
                check(effect == 0 || (docks[0]->IsMagnificationAnimating() && app.dockMagnificationAnimationFrameToken_),
                    "component entry must start the configured Dock animation and its frame scheduler");
                const double componentNow = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                docks[0]->AdvanceMagnificationAnimation(componentNow + 1000);
                const RECT base = docks[0]->GetSlots().front()->GetBounds();
                const RECT expanded = docks[0]->GetElementVisualRect(base, first);
                const RECT componentTitle = docks[0]->GetHoveredTitleBounds(first);
                check((effect == 0 ? EqualRect(&expanded, &base) != FALSE :
                    docks[0]->GetElementIconSize(base, expanded) > docks[0]->GetElementIconSize(base, base)) &&
                    IsRectEmpty(&componentTitle), "component drag must grow Dock icons while keeping insertion feedback free of titles");
                if (effect != 0)
                {
                    POINT tip = first;
                    switch (position)
                    {
                    case DockPosition::Bottom: tip.y = base.top - 1; break;
                    case DockPosition::Top: tip.y = base.bottom; break;
                    case DockPosition::Left: tip.x = base.right; break;
                    case DockPosition::Right: tip.x = base.left - 1; break;
                    }
                    input(tip);
                    check(app.widgetDockTarget_ && app.GetDockContainerAtPoint(tip) == docks[0],
                        "component insertion must retain the visible magnified Dock outside its base cell");
                }
                referenceContext->SetTarget(complete.Get());
                referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                docks[0]->DrawInsertionPreview(referenceContext.Get(), app.widgetDockInsertIndex_);
                check(SUCCEEDED(referenceContext->EndDraw()), "render component insertion with hover geometry");
                referenceContext->SetTarget(nullptr);
                const RECT line = paintedBounds(read(referenceContext.Get(), complete.Get()));
                check(!IsRectEmpty(&line), "component Dock landing must expose a visible insertion line");
                input(outside);
                (void)docks[0]->GetElementVisualRect(base, outside);
                docks[0]->AdvanceMagnificationAnimation(componentNow + 2000);
                const RECT exited = docks[0]->GetElementVisualRect(base, outside);
                check(!app.widgetDockTarget_ && EqualRect(&exited, &base),
                    "leaving the Dock during a component move must complete its hover exit");
                app.widgetAction_ = DesktopApp::WidgetAction::Resize;
                app.lastMousePoint_ = first;
                const RECT resizing = docks[0]->GetElementVisualRect(base, first);
                check(EqualRect(&resizing, &base), "component resize must continue to suppress Dock hover");
                app.widgetAction_ = DesktopApp::WidgetAction::None;
                app.mouseDown_ = false; app.mouseDownWidgetIndex_ = static_cast<size_t>(-1);
                app.widgets_.clear(); app.widgetDockTargetContainer_ = nullptr;
                app.widgetCollectionGroupTargetIndex_ = app.widgetPairTargetIndex_ = static_cast<size_t>(-1);
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
        // Commit actual item/component pins, then rebuild the real Dock tree.
        // A stationary pointer on the grown edge is outside the new base
        // cells; loss of the old focus rectangle must not replay entry.
        const auto baselineEntries = app.dockEntries_;
        DesktopWidget insertedWidget;
        insertedWidget.id = L"r08-committed-component";
        insertedWidget.type = DesktopWidgetType::Collection;
        insertedWidget.gridCell = {kDockPageId, 0, 0}; insertedWidget.gridSpan = {1, 1};
        app.widgets_.push_back(std::move(insertedWidget));
        app.dockAreas_.assign(areas.begin(), areas.end());
        const auto rebuild = [&] {
            // Deny native Host allocation only. The production model rebuild,
            // presentation capture/restore and Dock geometry remain real.
            auto device = std::move(app.dcompDevice_);
            app.RebuildContainersAndItems();
            app.dcompDevice_ = std::move(device);
            size_t index = 0;
            for (const auto& container : app.containers_)
                if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                {
                    check(index < docks.size(), "rebuild keeps only the three test-owned Docks");
                    docks[index++] = dock;
                }
            check(index == docks.size(), "rebuild restores every Dock model without native Host allocation");
        };
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {1, 2})
        for (const bool component : {false, true})
        for (const bool settled : {false, true})
        for (const bool newTarget : {true, false})
        {
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = false;
            app.dockEntries_ = baselineEntries;
            if (!component) app.dockEntries_.pop_back(); // A real existing item is the unpinned source.
            app.RefreshCollectedKeysCache();
            rebuild(); reset();
            const RECT base = docks[0]->GetSlots().front()->GetBounds();
            POINT pointer = point(0, 0);
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            // Preserve the exact center-minus-10 sample that failed on the
            // original implementation, and run it before other commit scenes.
            // The retained-old-item case also covers the original leading edge.
            if (vertical) pointer.y = newTarget ? pointer.y - 10 : base.top;
            else pointer.x = newTarget ? pointer.x - 10 : base.left;
            app.lastMousePoint_ = pointer;
            (void)docks[0]->GetElementVisualRect(base, pointer);
            auto state = docks[0]->CapturePresentationState();
            state.focusRect = base; state.entry = {}; state.single = {};
            const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
            if (effect == 1)
            {
                state.single.SetTarget(base, now - (settled ? 200 : 40), 1.0, vertical ? pointer.y : pointer.x);
                state.single.Advance(now);
            }
            else
            {
                state.entry.SetHovered(true, now - (settled ? 200 : 80), 1.0);
                state.entry.Advance(now);
            }
            check(docks[0]->RestorePresentationState(state), "supply real pre-commit hover progress");
            switch (position)
            {
            case DockPosition::Bottom: pointer.y = base.top - 1; break;
            case DockPosition::Top: pointer.y = base.bottom; break;
            case DockPosition::Left: pointer.x = base.right; break;
            case DockPosition::Right: pointer.x = base.left - 1; break;
            }
            app.lastMousePoint_ = pointer;
            (void)docks[0]->GetElementVisualRect(base, pointer);
            const float maximum = snowdesktop::dock_magnification::ResolveFocusScale(effect, app.dockSettings_.hoverScale, true);
            const auto amplitude = [&] {
                const auto current = docks[0]->CapturePresentationState();
                return effect == 1 ? current.single.Scale() : current.entry.FocusScale(maximum);
            };
            const float before = amplitude();
            check(before > 1.0f, "commit begins with actual visible hover growth");
            const size_t insertIndex = newTarget ? 0 : app.dockEntries_.size();
            const std::wstring expectedFocus = newTarget
                ? (component ? app.widgets_.front().id : app.items_.back().layoutKey)
                : app.dockEntries_.front().reference;
            if (component)
            {
                Widget source(&app.widgets_.front(), &app);
                app.CommitDockDrop({&source}, nullptr, docks[0], insertIndex, 0);
            }
            else
            {
                DesktopIcon source(&app.items_.back(), nullptr, &app);
                app.CommitDockDrop({&source}, nullptr, docks[0], insertIndex, 0);
            }
            check(app.dockEntries_.size() == baselineEntries.size() + static_cast<size_t>(component),
                "the production commit actually inserts the item or component");
            auto sample = [&] {
                const RECT next = docks[0]->GetSlots().front()->GetBounds();
                (void)docks[0]->GetElementVisualRect(next, pointer);
                const auto presentation = docks[0]->CapturePresentationState();
                const auto* focusedEntry = dynamic_cast<DockEntryItem*>(docks[0]->GetSlots().front()->GetItem());
                check(EqualRect(&presentation.focusRect, &next) &&
                    focusedEntry && focusedEntry->GetReference() == expectedFocus,
                    "the real hovered entry must follow the pointer after insertion, including newly inserted content");
                check(amplitude() + 0.0001f >= before && (!settled || !docks[0]->IsMagnificationAnimating()),
                    "committed insertion must retain hover amplitude instead of starting a new entry animation");
            };
            sample(); // Invalidated slots before the runtime-tree handoff.
            rebuild(); sample();
            {
                // Replace only OS cursor sampling with this process's own
                // never-shown HWND. The production leave route must retain
                // the logical Dock after capture/Host handoff, without moving
                // the user's cursor or operating any desktop host window.
                dockHoverHandoffScreenPoint = pointer;
                check(ClientToScreen(geometry, &dockHoverHandoffScreenPoint) != FALSE,
                    "convert test-owned Dock pointer to screen coordinates");
                dockHoverHandoffWindow = geometry;
                InputSafetyImport cursor("GetCursorPos", reinterpret_cast<ULONG_PTR>(&DockHoverHandoffCursor));
                InputSafetyImport hit("WindowFromPoint", reinterpret_cast<ULONG_PTR>(&DockHoverHandoffHit));
                app.OnMouseLeave();
                check(app.lastMousePoint_.x == pointer.x && app.lastMousePoint_.y == pointer.y,
                    "a post-insertion HWND leave must retain the live pointer on the newly hovered Dock item");
                sample();
            }
            docks[0]->AdvanceMagnificationAnimation(now + (effect == 1 ? 42 : 82));
            check(!docks[0]->IsMagnificationAnimating() && std::abs(amplitude() - maximum) < 0.0001f,
                "an in-progress entry finishes at its original deadline across insertion and rebuild");
        }
        app.dockEntries_ = baselineEntries;
        std::erase_if(app.containers_, [](const auto& container) {
            return dynamic_cast<WidgetContainer*>(container.get()) != nullptr;
        });
        app.widgets_.clear(); app.RefreshCollectedKeysCache(); rebuild();
        app.dockAreas_.clear(); // The later real release must retain the fixture's no-Host boundary.

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

        // The first pin has no fixed Slot to anchor its indicator. Exercise
        // the real component input route and D2D pixels with running apps on
        // the other side, including Start/Search and every hover orientation.
        app.persistentDockHosts_.clear();
        app.widgets_.clear();
        DesktopWidget firstPinWidget;
        firstPinWidget.id = L"first-pin-component";
        firstPinWidget.type = DesktopWidgetType::Collection;
        firstPinWidget.gridCell = {app.gridPages_.front().id, 5, 1};
        firstPinWidget.gridSpan = {1, 1};
        firstPinWidget.bounds = {1020, 80, 1100, 160};
        app.widgets_.push_back(std::move(firstPinWidget));

        // Independent paint oracle for the reported left/right asymmetry.
        // Calibrate the icon's actual unscaled padding from its opaque DIB,
        // then recover the animated cell edges from DrawContents pixels.
        // Do not derive the expected line from GetElementVisualRect: that is
        // also used by the product marker and would hide a shared mistake.
        app.iconBeautifySettings_.enabled = false;
        app.iconOverrides_.clear();
        for (auto& item : app.items_) item.selected = false;
        std::array<wchar_t, 32768> executablePath{};
        check(GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size())) > 0,
            "locate the native fixture's private evidence directory");
        const auto paintEvidence = std::filesystem::path(executablePath.data()).parent_path() /
            (L"dock-insertion-paint-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        check(std::filesystem::create_directory(paintEvidence), "exclusively create test-owned paint evidence directory");
        const auto iconPaintBounds = [&](const std::vector<std::uint32_t>& pixels) {
            RECT result{static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
            for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const auto pixel = pixels[static_cast<size_t>(y) * width + x];
                if ((pixel >> 24) < 250 || (pixel & 0xffffffu) != 0x4070a0u) continue;
                result.left = std::min(result.left, static_cast<LONG>(x));
                result.top = std::min(result.top, static_cast<LONG>(y));
                result.right = std::max(result.right, static_cast<LONG>(x + 1));
                result.bottom = std::max(result.bottom, static_cast<LONG>(y + 1));
            }
            return result;
        };
        size_t paintChecks = 0, paintFailures = 0;
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {1, 2})
        for (const size_t generated : {size_t{0}, size_t{3}})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear(); app.dockUnpinnedRunningApps_.clear();
            app.dockEntries_.assign(baselineEntries.begin(), baselineEntries.begin() + 1);
            for (size_t index = 0; index < generated; ++index)
            {
                DockRunningAppInfo running;
                running.identityKey = L"paint-running-" + std::to_wstring(index);
                running.title = L"Running application"; running.presence.SetVisible(true, 0, false);
                app.dockUnpinnedRunningApps_.push_back(std::move(running));
            }
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = false;
            app.dockSettings_.showWindowsButton = true; app.dockSettings_.showQuickSearchButton = true;
            app.dockSettings_.showRunningApps = true;
            app.gridPages_.front().dpiX = app.gridPages_.front().dpiY = 120;
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{100, 20, 1300, 780});
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            const RECT base = dock->GetSlots().front()->GetBounds();
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const auto leading = [&](RECT rect) { return vertical ? rect.top : rect.left; };
            const auto trailing = [&](RECT rect) { return vertical ? rect.bottom : rect.right; };
            const auto drawPixels = [&](bool marker, size_t boundary) {
                referenceContext->SetTarget(complete.Get());
                referenceContext->SetTransform(D2D1::Matrix3x2F::Identity());
                referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                app.brushCache_.clear(); app.brushCacheContext_ = referenceContext.Get();
                if (marker) dock->DrawInsertionPreview(referenceContext.Get(), boundary);
                else dock->DrawContents(referenceContext.Get());
                check(SUCCEEDED(referenceContext->EndDraw()), "draw actual animated Dock contents or insertion pixels");
                referenceContext->SetTarget(nullptr);
                app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
                return read(referenceContext.Get(), complete.Get());
            };
            app.lastMousePoint_ = outside;
            app.widgetAction_ = DesktopApp::WidgetAction::None;
            const auto baselinePixels = drawPixels(false, 0);
            const RECT baselinePaint = iconPaintBounds(baselinePixels);
            check(!IsRectEmpty(&baselinePaint), "the real opaque fixture icon must be observable before hover");
            const LONG leadingPadding = leading(baselinePaint) - leading(base);
            const LONG trailingPadding = trailing(base) - trailing(baselinePaint);
            check(leadingPadding >= 0 && trailingPadding >= 0,
                "measured unscaled icon padding must lie inside its actual slot");
            for (const int phase : {0, 1, 2})
            for (const size_t boundary : {size_t{0}, size_t{1}})
            {
                auto state = dock->CapturePresentationState();
                state.focusRect = {}; state.single = {}; state.entry = {};
                check(dock->RestorePresentationState(state), "reset independent paint sample's real hover timeline");
                POINT pointer{(base.left + base.right) / 2, (base.top + base.bottom) / 2};
                const LONG sampleAxis = leading(base) + (trailing(base) - leading(base)) * (boundary == 0 ? 1 : 3) / 4;
                if (vertical) pointer.y = sampleAxis; else pointer.x = sampleAxis;
                app.mouseDownWidgetIndex_ = 0; app.mouseDown_ = true; app.mouseDownPoint_ = outside;
                app.widgetDragOriginalCell_ = app.widgets_.front().gridCell;
                app.widgetDragOriginalSpan_ = app.widgets_.front().gridSpan;
                app.widgetAction_ = DesktopApp::WidgetAction::Move;
                auto factory = std::move(app.dwriteFactory_);
                app.OnMouseMoveAt(MK_LBUTTON, pointer); app.dwriteFactory_ = std::move(factory);
                check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == dock &&
                    app.widgetDockInsertIndex_ == boundary,
                    "real component pointer must select each side of the same fixed item");
                state = dock->CapturePresentationState(); state.focusRect = base; state.single = {}; state.entry = {};
                const double start = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                if (effect == 1)
                {
                    state.single.SetTarget(base, start, 1.0, sampleAxis);
                    state.single.Advance(start + phase * snowdesktop::dock_magnification::SingleFocusAnimation::kDurationMilliseconds / 2.0);
                }
                else
                {
                    state.entry.SetHovered(true, start, 1.0);
                    state.entry.Advance(start + phase * snowdesktop::dock_magnification::HoverEntryAnimation::kDurationMilliseconds / 2.0);
                }
                check(dock->RestorePresentationState(state), "supply initial, intermediate and completed native hover frames");
                const auto contentPixels = drawPixels(false, boundary);
                const RECT icon = iconPaintBounds(contentPixels);
                check(!IsRectEmpty(&icon), "animated target must retain actual opaque icon pixels");
                const LONG expectedAxis = boundary == 0 ? leading(icon) - leadingPadding : trailing(icon) + trailingPadding;
                const auto markerPixels = drawPixels(true, boundary);
                const RECT marker = paintedBounds(markerPixels);
                const LONG actualAxis = vertical ? (marker.top + marker.bottom) / 2 : (marker.left + marker.right) / 2;
                const bool matches = !IsRectEmpty(&marker) && std::abs(actualAxis - expectedAxis) <= 1;
                ++paintChecks;
                std::cout << "Dock actual paint position=" << static_cast<int>(position) << " effect=" << effect
                    << " running=" << generated << " phase=" << phase << " side=" << boundary
                    << " pointer=" << pointer.x << ',' << pointer.y << " icon=" << icon.left << ',' << icon.top << ',' << icon.right << ',' << icon.bottom
                    << " padding=" << leadingPadding << ',' << trailingPadding << " expected=" << expectedAxis << " marker=" << actualAxis
                    << " matches=" << matches << '\n';
                if (!matches)
                {
                    ++paintFailures;
                    const auto name = L"position-" + std::to_wstring(static_cast<int>(position)) + L"-effect-" + std::to_wstring(effect) +
                        L"-running-" + std::to_wstring(generated) + L"-phase-" + std::to_wstring(phase) + L"-side-" + std::to_wstring(boundary);
                    std::string error;
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-contents.png"),
                        static_cast<int>(width), static_cast<int>(height), contentPixels, error), "save actual contents pixels: " + error);
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-marker.png"),
                        static_cast<int>(width), static_cast<int>(height), markerPixels, error), "save actual insertion pixels: " + error);
                }
                app.widgetAction_ = DesktopApp::WidgetAction::None; app.mouseDown_ = false;
                app.mouseDownWidgetIndex_ = static_cast<size_t>(-1);
                app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
            }
        }
        std::cout << "Independent actual Dock paint checks=" << paintChecks << " mismatches=" << paintFailures
            << " evidence=" << paintEvidence.string() << '\n';
        check(paintFailures == 0, "insertion line must follow independently painted item edges at every hover phase on both sides");

        // File components use a different source classification and the
        // opposite lane. Measure the real component/control plate, rather
        // than its small preview thumbnails whose inset grows with the icon.
        const auto platePaintBounds = [&](const std::vector<std::uint32_t>& pixels, RECT anchor, size_t fromTrailing, bool tracePerpendicular = false) {
            std::vector<std::uint8_t> visited(pixels.size());
            std::vector<size_t> pending;
            std::vector<RECT> plates;
            const bool vertical = app.dockSettings_.position == DockPosition::Left || app.dockSettings_.position == DockPosition::Right;
            const auto white = [&](size_t index) {
                // read() copies PREMULTIPLIED BGRA, not the straight-alpha
                // colour returned by a PNG decoder. White fill/outline have
                // RGB equal to alpha (within one UNORM quantization step).
                const auto pixel = pixels[index], alpha = pixel >> 24;
                return alpha > 0 && std::abs(static_cast<int>((pixel >> 16) & 255) - static_cast<int>(alpha)) <= 1 &&
                    std::abs(static_cast<int>((pixel >> 8) & 255) - static_cast<int>(alpha)) <= 1 &&
                    std::abs(static_cast<int>(pixel & 255) - static_cast<int>(alpha)) <= 1;
            };
            std::array<size_t, 256> alphaCounts{};
            for (size_t index = 0; index < pixels.size(); ++index)
                if (white(index)) ++alphaCounts[pixels[index] >> 24];
            const auto fillAlpha = static_cast<std::uint32_t>(std::distance(alphaCounts.begin(),
                std::max_element(alphaCounts.begin() + 1, alphaCounts.end())));
            const auto fill = [&](size_t index) { return white(index) && (pixels[index] >> 24) == fillAlpha; };
            for (size_t seed = 0; seed < pixels.size(); ++seed)
            {
                // The alpha-uniform fill interiors remain separate when two
                // enlarged white outlines touch. Do not join those outlines
                // into one object and thereby mistake Start for FileGroup.
                if (visited[seed] || !fill(seed)) continue;
                pending.clear(); pending.push_back(seed); visited[seed] = 1;
                RECT region{static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
                const auto append = [&](size_t index) {
                    if (!visited[index] && fill(index)) { visited[index] = 1; pending.push_back(index); }
                };
                for (size_t next = 0; next < pending.size(); ++next)
                {
                    const size_t index = pending[next];
                    const auto x = static_cast<LONG>(index % width), y = static_cast<LONG>(index / width);
                    region.left = std::min(region.left, x); region.top = std::min(region.top, y);
                    region.right = std::max(region.right, x + 1); region.bottom = std::max(region.bottom, y + 1);
                    if (x > 0) append(index - 1);
                    if (x + 1 < static_cast<LONG>(width)) append(index + 1);
                    if (y > 0) append(index - width);
                    if (y + 1 < static_cast<LONG>(height)) append(index + width);
                }
                const LONG minimumSide = std::max(20L, std::min(anchor.right - anchor.left, anchor.bottom - anchor.top) * 3 / 4);
                if (region.right - region.left < minimumSide || region.bottom - region.top < minimumSide) continue;
                const LONG first = vertical ? region.top : region.left;
                const LONG end = vertical ? region.bottom : region.right;
                const LONG axisCenter = (first + end) / 2;
                const LONG crossFirst = vertical ? region.left : region.top;
                const LONG crossEnd = vertical ? region.right : region.bottom;
                const LONG crossLimit = static_cast<LONG>(vertical ? width : height);
                const auto pixelIndex = [&](LONG cross) { return vertical ? static_cast<size_t>(axisCenter) * width + cross : static_cast<size_t>(cross) * width + axisCenter; };
                LONG outerFirst = crossFirst, outerEnd = crossEnd;
                const auto firstRaw = pixels[pixelIndex(crossFirst)], endRaw = pixels[pixelIndex(crossEnd - 1)];
                const bool firstWhite = white(pixelIndex(crossFirst)), endWhite = white(pixelIndex(crossEnd - 1));
                if (firstWhite) while (outerFirst > 0 && white(pixelIndex(outerFirst - 1))) --outerFirst;
                if (endWhite) while (outerEnd < crossLimit && white(pixelIndex(outerEnd))) ++outerEnd;
                const bool firstUsable = firstWhite && outerFirst > 0;
                const bool endUsable = endWhite && outerEnd < crossLimit;
                double strokeWidth = 0.0;
                int observedSides = 0;
                if (firstUsable) { strokeWidth += crossFirst - outerFirst; ++observedSides; }
                if (endUsable) { strokeWidth += outerEnd - crossEnd; ++observedSides; }
                if (tracePerpendicular)
                    std::cout << "Footer plate perpendicular raw=0x" << std::hex << firstRaw << ",0x" << endRaw << std::dec
                        << " core=" << crossFirst << ',' << crossEnd << " outer=" << outerFirst << ',' << outerEnd
                        << " limit=" << crossLimit << " white=" << firstWhite << ',' << endWhite
                        << " usable=" << firstUsable << ',' << endUsable << " adoptedSides=" << observedSides << '\n';
                if (!observedSides) continue;
                // Measure the local stroke perpendicular to the lane, where
                // this card cannot touch its neighbour. Ignore clipped sides.
                // Never scan through a connected outline into another card.
                const double halfStroke = strokeWidth / (2.0 * observedSides);
                const LONG pathFirst = static_cast<LONG>(std::lround(first - halfStroke));
                const LONG pathEnd = static_cast<LONG>(std::lround(end + halfStroke));
                if (vertical) { region.top = pathFirst; region.bottom = pathEnd; }
                else { region.left = pathFirst; region.right = pathEnd; }
                plates.push_back(region);
            }
            std::sort(plates.begin(), plates.end(), [&](RECT left, RECT right) {
                return vertical ? left.top + left.bottom < right.top + right.bottom : left.left + left.right < right.left + right.right;
            });
            if (plates.size() != fromTrailing + 2)
                std::cerr << "File fill-component observation count=" << plates.size() << " expected=" << fromTrailing + 2
                    << " rawFillAlpha=" << fillAlpha << " anchor=" << anchor.left << ',' << anchor.top << ',' << anchor.right << ',' << anchor.bottom << '\n';
            // Actual paint order is Start, optional FileGroup, then Search or
            // Trash. A trailing wave can move the target centre farther than
            // its neighbour, so nearest-base distance is not its identity.
            return fromTrailing < plates.size() ? plates[plates.size() - 1 - fromTrailing] : RECT{};
        };
        DesktopWidget fileSource;
        fileSource.id = L"paint-file-source"; fileSource.type = DesktopWidgetType::FolderMapping;
        fileSource.gridCell = {app.gridPages_.front().id, 5, 1}; fileSource.gridSpan = {1, 1};
        fileSource.bounds = {1020, 80, 1100, 160};
        app.widgets_.push_back(std::move(fileSource));
        DesktopWidget fileTarget;
        fileTarget.id = L"paint-file-target"; fileTarget.type = DesktopWidgetType::FileGroup;
        fileTarget.gridCell = {kDockPageId, 1, 0}; fileTarget.gridSpan = {1, 1};
        app.widgets_.push_back(std::move(fileTarget));
        const auto drawFilePixels = [&](DockContainer* dock, bool marker, size_t boundary) {
            referenceContext->SetTarget(complete.Get());
            referenceContext->SetTransform(D2D1::Matrix3x2F::Identity());
            referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
            app.brushCache_.clear(); app.brushCacheContext_ = referenceContext.Get();
            if (marker) dock->DrawInsertionPreview(referenceContext.Get(), boundary);
            else dock->DrawContents(referenceContext.Get());
            check(SUCCEEDED(referenceContext->EndDraw()), "draw actual file-lane contents or insertion pixels");
            referenceContext->SetTarget(nullptr); app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
            return read(referenceContext.Get(), complete.Get());
        };
        size_t filePaintChecks = 0, filePaintFailures = 0;
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {1, 2})
        for (const bool attached : {false, true})
        for (const size_t folders : {size_t{0}, size_t{1}})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear(); app.dockUnpinnedRunningApps_.clear();
            app.dockEntries_.assign(baselineEntries.begin(), baselineEntries.begin() + 1);
            if (folders)
            {
                DockEntry entry; entry.type = DockEntryType::FileGroup; entry.reference = L"paint-file-target";
                app.dockEntries_.push_back(std::move(entry));
            }
            app.widgets_[1].type = folders ? DesktopWidgetType::FileGroup : DesktopWidgetType::FolderMapping;
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = attached;
            app.dockSettings_.showWindowsButton = true; app.dockSettings_.showQuickSearchButton = true;
            app.gridPages_.front().dpiX = app.gridPages_.front().dpiY = 120;
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{100, 20, 1300, 780});
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            const RECT base = folders ? dock->GetSlots()[1]->GetBounds() : dock->GetSearchRect();
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const auto leading = [&](RECT rect) { return vertical ? rect.top : rect.left; };
            const auto trailing = [&](RECT rect) { return vertical ? rect.bottom : rect.right; };
            app.lastMousePoint_ = outside; app.widgetAction_ = DesktopApp::WidgetAction::None;
            const auto baselineFilePixels = drawFilePixels(dock, false, 0);
            check(complete->GetPixelFormat().alphaMode == D2D1_ALPHA_MODE_PREMULTIPLIED,
                "file paint oracle reads the actual premultiplied target format");
            const auto sample = baselineFilePixels[static_cast<size_t>((base.top + base.bottom) / 2) * width + (base.left + base.right) / 2];
            std::cout << "File baseline raw BGRA=0x" << std::hex << sample << std::dec << " alphaMode=PREMULTIPLIED\n";
            if (position == DockPosition::Bottom && effect == 1 && !attached && !folders)
            {
                std::string error;
                check(snowdesktop::preview_png::Save(paintEvidence / L"file-baseline-first.png", static_cast<int>(width),
                    static_cast<int>(height), baselineFilePixels, error), "save first file baseline raw target: " + error);
            }
            const RECT baselinePlate = platePaintBounds(baselineFilePixels, base, folders ? 1 : 0);
            check(!IsRectEmpty(&baselinePlate), "real file component/Search plate must be observable before hover");
            const LONG leadingPadding = leading(baselinePlate) - leading(base);
            const LONG trailingPadding = trailing(base) - trailing(baselinePlate);
            check(leadingPadding >= 0 && trailingPadding >= 0, "actual file plate lies within its unscaled slot");
            for (const int phase : {0, 1, 2})
            for (const size_t side : {size_t{0}, size_t{1}})
            {
                auto state = dock->CapturePresentationState(); state.focusRect = {}; state.single = {}; state.entry = {};
                check(dock->RestorePresentationState(state), "reset file-lane real hover timeline");
                POINT pointer{(base.left + base.right) / 2, (base.top + base.bottom) / 2};
                const LONG sampleAxis = leading(base) + (trailing(base) - leading(base)) * (side == 0 ? 1 : 3) / 4;
                if (vertical) pointer.y = sampleAxis; else pointer.x = sampleAxis;
                app.mouseDownWidgetIndex_ = 1; app.mouseDown_ = true; app.mouseDownPoint_ = outside;
                app.widgetDragOriginalCell_ = app.widgets_[1].gridCell; app.widgetDragOriginalSpan_ = app.widgets_[1].gridSpan;
                app.widgetAction_ = DesktopApp::WidgetAction::Move;
                auto factory = std::move(app.dwriteFactory_);
                app.OnMouseMoveAt(MK_LBUTTON, pointer); app.dwriteFactory_ = std::move(factory);
                const size_t boundary = 1 + (folders ? side : 0);
                check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == dock && app.widgetDockInsertIndex_ == boundary,
                    "real FolderMapping/FileGroup source must target the file partition on both sides or its first empty position");
                state = dock->CapturePresentationState(); state.focusRect = base; state.single = {}; state.entry = {};
                const double start = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                if (effect == 1)
                {
                    state.single.SetTarget(base, start, 1.0, sampleAxis);
                    state.single.Advance(start + phase * snowdesktop::dock_magnification::SingleFocusAnimation::kDurationMilliseconds / 2.0);
                }
                else
                {
                    state.entry.SetHovered(true, start, 1.0);
                    state.entry.Advance(start + phase * snowdesktop::dock_magnification::HoverEntryAnimation::kDurationMilliseconds / 2.0);
                }
                check(dock->RestorePresentationState(state), "supply file-lane initial/intermediate/completed real hover frame");
                const auto contentPixels = drawFilePixels(dock, false, boundary);
                const RECT plate = platePaintBounds(contentPixels, base, folders ? 1 : 0);
                check(!IsRectEmpty(&plate), "animated file target retains an independently observable plate");
                const LONG expected = folders && side ? trailing(plate) + trailingPadding : leading(plate) - leadingPadding;
                const auto markerPixels = drawFilePixels(dock, true, boundary);
                const RECT marker = paintedBounds(markerPixels);
                const LONG actual = vertical ? (marker.top + marker.bottom) / 2 : (marker.left + marker.right) / 2;
                const bool matches = !IsRectEmpty(&marker) && std::abs(actual - expected) <= 1;
                ++filePaintChecks;
                std::cout << "File actual paint position=" << static_cast<int>(position) << " effect=" << effect
                    << " attached=" << attached << " folders=" << folders << " phase=" << phase << " side=" << side
                    << " pointer=" << pointer.x << ',' << pointer.y << " plate=" << plate.left << ',' << plate.top << ',' << plate.right << ',' << plate.bottom
                    << " padding=" << leadingPadding << ',' << trailingPadding << " expected=" << expected << " marker=" << actual
                    << " matches=" << matches << '\n';
                if (!matches)
                {
                    ++filePaintFailures;
                    const auto name = L"file-position-" + std::to_wstring(static_cast<int>(position)) + L"-effect-" + std::to_wstring(effect) +
                        L"-attached-" + std::to_wstring(attached) + L"-folders-" + std::to_wstring(folders) + L"-phase-" + std::to_wstring(phase) + L"-side-" + std::to_wstring(side);
                    std::string error;
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-contents.png"),
                        static_cast<int>(width), static_cast<int>(height), contentPixels, error), "save file contents pixels: " + error);
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-marker.png"),
                        static_cast<int>(width), static_cast<int>(height), markerPixels, error), "save file insertion pixels: " + error);
                }
                app.widgetAction_ = DesktopApp::WidgetAction::None; app.mouseDown_ = false;
                app.mouseDownWidgetIndex_ = static_cast<size_t>(-1); app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
            }
        }
        std::cout << "Independent actual file-lane paint checks=" << filePaintChecks << " mismatches=" << filePaintFailures << '\n';
        check(filePaintFailures == 0, "file leading/trailing and first-component markers follow real painted plates in floating and attached lanes");

        // Search can be hidden: the first file component then belongs before
        // Trash, after the last floating item, or in the attached lane's own
        // far-end boundary. A zero Search rectangle is never an origin anchor.
        const auto previousRecycleKey = app.items_[2].layoutKey;
        const auto previousRecycleClsid = app.items_[2].desktopIconClsid;
        app.items_[2].layoutKey = kDesktopIconClsidRecycleBin;
        app.items_[2].desktopIconClsid = kDesktopIconClsidRecycleBin;
        app.RefreshDesktopItemIndexCache();
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {1, 2})
        for (const bool attached : {false, true})
        for (const bool trash : {false, true})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear(); app.dockUnpinnedRunningApps_.clear();
            app.dockEntries_.assign(baselineEntries.begin(), baselineEntries.begin() + 1);
            if (trash)
            {
                DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = kDesktopIconClsidRecycleBin;
                app.dockEntries_.push_back(std::move(entry));
            }
            app.widgets_[1].type = DesktopWidgetType::FileCategories;
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = attached;
            app.dockSettings_.showWindowsButton = true; app.dockSettings_.showQuickSearchButton = false;
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{100, 20, 1300, 780});
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            const RECT panel = dock->GetBounds();
            const RECT base = dock->GetSlots()[trash ? 1 : 0]->GetBounds();
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const auto leading = [&](RECT rect) { return vertical ? rect.top : rect.left; };
            const auto trailing = [&](RECT rect) { return vertical ? rect.bottom : rect.right; };
            app.lastMousePoint_ = outside; app.widgetAction_ = DesktopApp::WidgetAction::None;
            const auto baselinePixels = drawFilePixels(dock, false, 1);
            const RECT baselinePaint = trash ? platePaintBounds(baselinePixels, base, 0, true) : iconPaintBounds(baselinePixels);
            check(!IsRectEmpty(&baselinePaint), "hidden-Search fallback neighbour has actual painted pixels");
            const LONG padding = trash ? leading(baselinePaint) - leading(base) : trailing(base) - trailing(baselinePaint);
            POINT pointer{(base.left + base.right) / 2, (base.top + base.bottom) / 2};
            if (!trash && attached)
            {
                if (vertical) pointer.y = panel.bottom - 2; else pointer.x = panel.right - 2;
            }
            app.mouseDownWidgetIndex_ = 1; app.mouseDown_ = true; app.mouseDownPoint_ = outside;
            app.widgetDragOriginalCell_ = app.widgets_[1].gridCell; app.widgetDragOriginalSpan_ = app.widgets_[1].gridSpan;
            app.widgetAction_ = DesktopApp::WidgetAction::Move;
            auto factory = std::move(app.dwriteFactory_);
            app.OnMouseMoveAt(MK_LBUTTON, pointer); app.dwriteFactory_ = std::move(factory);
            check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == dock && app.widgetDockInsertIndex_ == 1,
                "real FileCategories source targets the first empty file lane with Search hidden");
            dock->AdvanceMagnificationAnimation(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 1000);
            const auto contentPixels = drawFilePixels(dock, false, 1);
            const RECT painted = trash ? platePaintBounds(contentPixels, base, 0, true) : iconPaintBounds(contentPixels);
            const int halfSpacing = std::max(1, static_cast<int>(std::round(kDockSpacing * ResolveDockScale(app.dockSettings_, false)))) / 2;
            const LONG expected = !trash && attached ? trailing(panel) - halfSpacing :
                trash ? leading(painted) - padding : trailing(painted) + padding;
            const auto markerPixels = drawFilePixels(dock, true, 1);
            const RECT marker = paintedBounds(markerPixels);
            const LONG actual = vertical ? (marker.top + marker.bottom) / 2 : (marker.left + marker.right) / 2;
            const auto footerName = L"footer-position-" + std::to_wstring(static_cast<int>(position)) + L"-effect-" + std::to_wstring(effect) +
                L"-attached-" + std::to_wstring(attached) + L"-trash-" + std::to_wstring(trash);
            std::string footerError;
            check(snowdesktop::preview_png::Save(paintEvidence / (footerName + L"-baseline.png"),
                static_cast<int>(width), static_cast<int>(height), baselinePixels, footerError), "save footer baseline pixels: " + footerError);
            check(snowdesktop::preview_png::Save(paintEvidence / (footerName + L"-contents.png"),
                static_cast<int>(width), static_cast<int>(height), contentPixels, footerError), "save footer contents pixels: " + footerError);
            check(snowdesktop::preview_png::Save(paintEvidence / (footerName + L"-marker.png"),
                static_cast<int>(width), static_cast<int>(height), markerPixels, footerError), "save footer marker pixels: " + footerError);
            std::cout << "Empty file hidden Search position=" << static_cast<int>(position) << " effect=" << effect
                << " attached=" << attached << " trash=" << trash << " expected=" << expected << " marker=" << actual << '\n';
            check(!IsRectEmpty(&marker) && std::abs(actual - expected) <= 1,
                "first file component uses actual Trash/floating neighbour or its attached trailing lane, never an empty Search origin");
            app.widgetAction_ = DesktopApp::WidgetAction::None; app.mouseDown_ = false;
            app.mouseDownWidgetIndex_ = static_cast<size_t>(-1); app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
        }
        app.items_[2].layoutKey = previousRecycleKey;
        app.items_[2].desktopIconClsid = previousRecycleClsid;
        app.RefreshDesktopItemIndexCache();
        std::cout << "Empty file hidden-Search/Trash matrix: 32 real-source cases passed\n";

        // A genuinely scrolling lane still clips insertion lines. Prove the
        // overflow through its real wheel route, then paint both boundaries
        // while the last logical item is outside the reserved bar.
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {1, 2})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear(); app.dockUnpinnedRunningApps_.clear(); app.dockEntries_ = baselineEntries;
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = false; app.dockSettings_.showWindowsButton = true;
            app.dockSettings_.showQuickSearchButton = true;
            app.widgetAction_ = DesktopApp::WidgetAction::None; app.lastMousePoint_ = outside;
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{100, 100, 600, 600});
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            const RECT panel = dock->GetBounds();
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const POINT pointer{(panel.left + panel.right) / 2, (panel.top + panel.bottom) / 2};
            check(dock->ScrollByWheelDelta(pointer, -WHEEL_DELTA) && dock->CapturePresentationState().scrollOffset > 0,
                "real overflow wheel route must move a nonzero scroll offset");
            const RECT first = dock->GetSlots().front()->GetBounds();
            app.lastMousePoint_ = {(first.left + first.right) / 2, (first.top + first.bottom) / 2};
            (void)dock->GetElementVisualRect(first, app.lastMousePoint_);
            dock->AdvanceMagnificationAnimation(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 1000);
            for (const size_t boundary : {size_t{0}, baselineEntries.size()})
            {
                const RECT marker = paintedBounds(drawFilePixels(dock, true, boundary));
                const LONG actual = vertical ? (marker.top + marker.bottom) / 2 : (marker.left + marker.right) / 2;
                check(!IsRectEmpty(&marker) && actual >= (vertical ? panel.top : panel.left) &&
                    actual <= (vertical ? panel.bottom : panel.right),
                    "true overflow insertion remains inside the scrolling bar instead of following offscreen logical items");
            }
        }
        std::cout << "True-overflow insertion clip: 4 positions x 2 effects x 2 boundaries passed\n";

        // These older matrices deliberately exercise a lane that fits. Their
        // original seven 136px cells did not fit the 760px vertical reserve.
        // Derive the fixture scale from real page icon metrics and the full
        // cell/gap/maximum-interaction budget. Leave the animation reserve
        // outside the Dock area but inside the real pixel target as well.
        const auto fitLegacyScene = [&](DockPosition position, size_t cellCount) {
            app.dockSettings_.thicknessScale = 1.0f;
            const int icon = app.GetDockPageItemIconSize(app.gridPages_.front());
            const int cells = static_cast<int>(cellCount);
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const int axis = vertical ? 760 : 1200;
            const float maximumScale = snowdesktop::dock_magnification::ResolveFocusScale(
                app.dockSettings_.hoverEffect, app.dockSettings_.hoverScale, snowdesktop::animation::RuntimeAnimationsEnabled());
            const auto envelope = [&](int pixels) {
                return snowdesktop::dock_magnification::ExpandInteractionBounds(RECT{0, 0, 1, 1}, position,
                    pixels, maximumScale, app.dockSettings_.hoverEffect == 1);
            };
            const RECT unscaled = envelope(icon);
            const int animationReserve = vertical ? unscaled.bottom - unscaled.top - 1 : unscaled.right - unscaled.left - 1;
            constexpr int maximumSeparators = 3;
            const int roundingTerms = cells + maximumSeparators + 7;
            const double budget = cells * (icon + kDockSpacing) + maximumSeparators * kDockSeparatorGap + 3 * kDockSpacing + animationReserve;
            app.dockSettings_.thicknessScale = ClampDockScale(std::min(1.0f,
                static_cast<float>((axis - roundingTerms) / budget)));
            const RECT scaled = envelope(std::max(1, static_cast<int>(std::round(icon * app.dockSettings_.thicknessScale))));
            RECT reserve{100, 20, 1300, 780};
            if (vertical) { reserve.top -= scaled.top; reserve.bottom -= scaled.bottom - 1; }
            else { reserve.left -= scaled.left; reserve.right -= scaled.right - 1; }
            return reserve;
        };
        const auto requireNoOverflow = [&](DockContainer* dock) {
            // A fresh container has not built its slots yet. Restore a large
            // presentation offset, then let the real BuildSlots clamp it to
            // GetMaxScrollOffset; zero proves the actual layout fits without
            // relying on a wheel event at an arbitrary non-scrollable point.
            const auto initial = dock->CapturePresentationState();
            auto probe = initial; probe.scrollOffset = std::numeric_limits<int>::max();
            check(dock->RestorePresentationState(probe), "set private fixture's max-scroll observation");
            (void)dock->GetSlots();
            const int maximum = dock->CapturePresentationState().scrollOffset;
            std::cout << "Legacy fixture scale=" << app.dockSettings_.thicknessScale << " actualMaxScroll=" << maximum << '\n';
            check(dock->RestorePresentationState(initial), "restore zero-scroll fixture presentation");
            check(maximum == 0, "legacy geometry matrix must prove actual no-overflow capacity");
            const RECT envelope = dock->GetInteractiveBounds();
            const bool vertical = app.dockSettings_.position == DockPosition::Left || app.dockSettings_.position == DockPosition::Right;
            std::cout << "Legacy maximum interaction=" << envelope.left << ',' << envelope.top << ',' << envelope.right << ',' << envelope.bottom << '\n';
            check(vertical ? envelope.top >= 0 && envelope.bottom <= static_cast<LONG>(height) :
                envelope.left >= 0 && envelope.right <= static_cast<LONG>(width),
                "legacy maximum animated axis must be observable inside the actual target");
        };
        size_t legacyFixedFailures = 0, legacyEmptyFailures = 0;

        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {0, 1, 2})
        for (const bool controls : {false, true})
        for (const size_t fixedCount : {size_t{0}, size_t{1}, size_t{3}})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear();
            app.dockEntries_.assign(baselineEntries.begin(), baselineEntries.begin() + fixedCount);
            app.dockUnpinnedRunningApps_.clear();
            for (size_t index = 0; index < 2; ++index)
            {
                DockRunningAppInfo running;
                running.identityKey = L"first-pin-running-" + std::to_wstring(index);
                running.title = L"Running application";
                running.presence.SetVisible(true, 0, false);
                app.dockUnpinnedRunningApps_.push_back(std::move(running));
            }
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = position == DockPosition::Top;
            app.dockSettings_.showRunningApps = true;
            app.dockSettings_.showWindowsButton = controls;
            app.dockSettings_.showQuickSearchButton = controls;
            app.gridPages_.front().dpiX = app.gridPages_.front().dpiY =
                fixedCount == 3 ? 144u : controls ? 120u : 96u;
            const RECT reserve = fitLegacyScene(position, fixedCount + 2 + (controls ? 2 : 0));
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, reserve);
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            app.lastMousePoint_ = outside;
            requireNoOverflow(dock);
            const auto& slots = dock->GetSlots();
            const RECT lastRunning = slots[fixedCount + 1]->GetBounds();
            const POINT pointer{(lastRunning.left + lastRunning.right) / 2,
                (lastRunning.top + lastRunning.bottom) / 2};
            const RECT panel = dock->GetBounds();
            std::cout << "Legacy fixed case position=" << static_cast<int>(position) << " effect=" << effect
                << " controls=" << controls << " fixed=" << fixedCount << " dpi=" << app.gridPages_.front().dpiX
                << " pointer=" << pointer.x << ',' << pointer.y << " running=" << lastRunning.left << ',' << lastRunning.top << ',' << lastRunning.right << ',' << lastRunning.bottom
                << " bar=" << panel.left << ',' << panel.top << ',' << panel.right << ',' << panel.bottom << '\n';
            check(PtInRect(&panel, pointer) != FALSE && dock->RunningItemAtPoint(pointer),
                "legacy fixed source point must be inside the bar and a real scrollable running hit");
            check(!dock->ScrollByWheelDelta(pointer, -WHEEL_DELTA),
                "wheel over the confirmed running scroll area must report no overflow");
            app.mouseDownWidgetIndex_ = 0; app.mouseDown_ = true;
            app.mouseDownPoint_ = outside;
            app.widgetDragOriginalCell_ = app.widgets_.front().gridCell;
            app.widgetDragOriginalSpan_ = app.widgets_.front().gridSpan;
            app.widgetAction_ = DesktopApp::WidgetAction::Move;
            auto factory = std::move(app.dwriteFactory_);
            app.OnMouseMoveAt(MK_LBUTTON, pointer);
            app.dwriteFactory_ = std::move(factory);
            if (!app.widgetDockTarget_ || app.widgetDockTargetContainer_ != dock || app.widgetDockInsertIndex_ != fixedCount)
                std::cerr << "Legacy fixed input target=" << app.widgetDockTarget_ << " sameContainer=" << (app.widgetDockTargetContainer_ == dock)
                    << " actualIndex=" << app.widgetDockInsertIndex_ << " expectedIndex=" << fixedCount << '\n';
            check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == dock &&
                app.widgetDockInsertIndex_ == fixedCount,
                "component over running apps must target the fixed partition's final boundary");
            dock->AdvanceMagnificationAnimation(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 1000);
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            for (const size_t boundary : fixedCount == 0 ? std::vector<size_t>{0} : std::vector<size_t>{0, fixedCount})
            {
                const size_t anchor = fixedCount == 0 ? 0 : boundary == 0 ? 0 : fixedCount - 1;
                const RECT visual = dock->GetElementVisualRect(slots[anchor]->GetBounds(), pointer);
                LONG expectedAxis = vertical
                    ? (boundary > 0 ? visual.bottom : visual.top)
                    : (boundary > 0 ? visual.right : visual.left);
                // This matrix fits without scrolling. DrawContents does not
                // clip its animation to the base viewport in this case.
                referenceContext->SetTarget(complete.Get());
                referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                dock->DrawInsertionPreview(referenceContext.Get(), boundary);
                check(SUCCEEDED(referenceContext->EndDraw()), "render real first/last fixed insertion indicator");
                referenceContext->SetTarget(nullptr);
                const auto markerFrame = read(referenceContext.Get(), complete.Get());
                const RECT pixels = paintedBounds(markerFrame);
                const LONG actualAxis = vertical ? (pixels.top + pixels.bottom) / 2 : (pixels.left + pixels.right) / 2;
                const bool matches = !IsRectEmpty(&pixels) && std::abs(actualAxis - expectedAxis) <= 1;
                std::cout << "Legacy fixed pixels position=" << static_cast<int>(position) << " effect=" << effect
                    << " controls=" << controls << " fixed=" << fixedCount << " boundary=" << boundary
                    << " expected=" << expectedAxis << " marker=" << actualAxis << " bbox=" << pixels.left << ',' << pixels.top << ',' << pixels.right << ',' << pixels.bottom
                    << " matches=" << matches << '\n';
                bool oldAnchorRejected = true;
                if (fixedCount == 0)
                {
                    const RECT search = slots.back()->GetBounds();
                    const LONG oldAxis = vertical ? search.top : search.left;
                    oldAnchorRejected = std::abs(actualAxis - oldAxis) > 10;
                }
                if (!matches || !oldAnchorRejected)
                {
                    if (!oldAnchorRejected) std::cerr << "Legacy fixed old Search anchor was not rejected\n";
                    ++legacyFixedFailures;
                    const auto name = L"legacy-fixed-position-" + std::to_wstring(static_cast<int>(position)) + L"-effect-" + std::to_wstring(effect) +
                        L"-controls-" + std::to_wstring(controls) + L"-fixed-" + std::to_wstring(fixedCount) + L"-boundary-" + std::to_wstring(boundary);
                    const auto contents = drawFilePixels(dock, false, boundary);
                    std::string error;
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-contents.png"), static_cast<int>(width), static_cast<int>(height), contents, error), "save legacy fixed contents: " + error);
                    check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-marker.png"), static_cast<int>(width), static_cast<int>(height), markerFrame, error), "save legacy fixed marker: " + error);
                }
            }
            app.widgetAction_ = DesktopApp::WidgetAction::None;
            app.mouseDown_ = false; app.mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
        }
        std::cout << "First-pin D2D boundary matrix: 72 cases; 96/120/144 DPI; pixel failures=" << legacyFixedFailures << '\n';

        // Model slots keep all file/Trash entries before generated slots,
        // even though their presentation is trailing. Cover that distinction
        // and the zero-length viewport when no generated entries exist.
        DesktopWidget files;
        files.id = L"first-pin-files"; files.title = L"Files";
        files.type = DesktopWidgetType::FolderMapping;
        files.gridCell = {kDockPageId, 0, 0}; files.gridSpan = {1, 1};
        app.widgets_.push_back(std::move(files));
        enum class EmptyScene { FilesRunning, RunningUtilities, FilesOnly, Utilities, StartOnly, RecycleOnly, Empty };
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int effect : {0, 1, 2})
        for (const bool attached : {false, true})
        for (const auto scene : {EmptyScene::FilesRunning, EmptyScene::RunningUtilities, EmptyScene::FilesOnly, EmptyScene::Utilities,
                 EmptyScene::StartOnly, EmptyScene::RecycleOnly, EmptyScene::Empty})
        {
            app.uiAnimationScheduler_.CancelAll(); app.dockMagnificationAnimationFrameToken_ = 0;
            app.containers_.clear(); app.dockEntries_.clear(); app.dockUnpinnedRunningApps_.clear();
            const bool folder = scene == EmptyScene::FilesRunning || scene == EmptyScene::FilesOnly;
            const bool generated = scene == EmptyScene::FilesRunning || scene == EmptyScene::RunningUtilities;
            const bool start = scene != EmptyScene::RecycleOnly && scene != EmptyScene::Empty;
            const bool search = folder || scene == EmptyScene::Utilities || scene == EmptyScene::RunningUtilities;
            const bool recycle = folder || scene == EmptyScene::Utilities || scene == EmptyScene::RunningUtilities || scene == EmptyScene::RecycleOnly;
            if (folder)
            {
                DockEntry entry; entry.type = DockEntryType::FolderMapping; entry.reference = L"first-pin-files";
                app.dockEntries_.push_back(std::move(entry));
            }
            if (recycle)
            {
                DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = kDesktopIconClsidRecycleBin;
                app.dockEntries_.push_back(std::move(entry));
            }
            if (generated)
                for (size_t index = 0; index < 2; ++index)
                {
                    DockRunningAppInfo running;
                    running.identityKey = L"empty-partition-running-" + std::to_wstring(index);
                    running.title = L"Running application"; running.presence.SetVisible(true, 0, false);
                    app.dockUnpinnedRunningApps_.push_back(std::move(running));
                }
            app.dockSettings_.position = position; app.dockSettings_.hoverEffect = effect;
            app.dockSettings_.edgeAttached = attached;
            app.dockSettings_.showWindowsButton = start; app.dockSettings_.showQuickSearchButton = search;
            app.gridPages_.front().dpiX = app.gridPages_.front().dpiY = 120;
            const RECT reserve = fitLegacyScene(position, static_cast<size_t>(folder) + static_cast<size_t>(recycle) + static_cast<size_t>(start) +
                static_cast<size_t>(search) + (generated ? 2 : 0));
            auto ownedDock = std::make_unique<DockContainer>(&app, &app.dockEntries_, reserve);
            auto* dock = ownedDock.get(); app.containers_.push_back(std::move(ownedDock));
            app.lastMousePoint_ = outside;
            requireNoOverflow(dock);
            const auto& slots = dock->GetSlots();
            const size_t count = app.dockEntries_.size();
            const RECT startBase = dock->GetWindowsButtonRect();
            const RECT baseBounds = dock->GetBounds();
            const RECT hit = generated ? slots[count + 1]->GetBounds() : start ? startBase :
                recycle ? slots[count - 1]->GetBounds() : baseBounds;
            const POINT pointer{(hit.left + hit.right) / 2, (hit.top + hit.bottom) / 2};
            std::cout << "Legacy empty case position=" << static_cast<int>(position) << " effect=" << effect
                << " attached=" << attached << " scene=" << static_cast<int>(scene) << " dpi=" << app.gridPages_.front().dpiX
                << " pointer=" << pointer.x << ',' << pointer.y << " hit=" << hit.left << ',' << hit.top << ',' << hit.right << ',' << hit.bottom
                << " bar=" << baseBounds.left << ',' << baseBounds.top << ',' << baseBounds.right << ',' << baseBounds.bottom << '\n';
            check(PtInRect(&baseBounds, pointer) != FALSE, "legacy empty source point must be inside the reserved bar");
            if (generated)
            {
                const RECT running = slots[count]->GetBounds();
                const POINT scrollPoint{(running.left + running.right) / 2, (running.top + running.bottom) / 2};
                check(dock->RunningItemAtPoint(scrollPoint), "empty-group wheel sample must hit a real scrollable running item");
                check(!dock->ScrollByWheelDelta(scrollPoint, -WHEEL_DELTA), "confirmed running area must have no overflow");
            }
            else if (folder && !attached)
            {
                const RECT file = slots.front()->GetBounds();
                const POINT scrollPoint{(file.left + file.right) / 2, (file.top + file.bottom) / 2};
                check(dock->EntryAtPoint(scrollPoint), "floating file wheel sample must hit its real scrollable entry");
                check(!dock->ScrollByWheelDelta(scrollPoint, -WHEEL_DELTA), "confirmed floating file area must have no overflow");
            }
            app.mouseDownWidgetIndex_ = 0; app.mouseDown_ = true; app.mouseDownPoint_ = outside;
            app.widgetDragOriginalCell_ = app.widgets_.front().gridCell;
            app.widgetDragOriginalSpan_ = app.widgets_.front().gridSpan;
            app.widgetAction_ = DesktopApp::WidgetAction::Move;
            auto factory = std::move(app.dwriteFactory_);
            app.OnMouseMoveAt(MK_LBUTTON, pointer); app.dwriteFactory_ = std::move(factory);
            if (!app.widgetDockTarget_ || app.widgetDockTargetContainer_ != dock || app.widgetDockInsertIndex_ != 0)
                std::cerr << "Legacy empty input target=" << app.widgetDockTarget_ << " sameContainer=" << (app.widgetDockTargetContainer_ == dock)
                    << " actualIndex=" << app.widgetDockInsertIndex_ << " expectedIndex=0\n";
            check(app.widgetDockTarget_ && app.widgetDockTargetContainer_ == dock && app.widgetDockInsertIndex_ == 0,
                "files, running apps and utility-only surfaces must preserve the empty fixed partition's index zero");
            dock->AdvanceMagnificationAnimation(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 1000);
            const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
            const auto leading = [&](RECT rect) { return vertical ? rect.top : rect.left; };
            const auto trailing = [&](RECT rect) { return vertical ? rect.bottom : rect.right; };
            LONG expectedAxis = 0;
            if (generated || (!attached && folder))
            {
                const RECT anchor = slots[generated ? count : 0]->GetBounds();
                expectedAxis = leading(dock->GetElementVisualRect(anchor, pointer));
            }
            else if (!attached)
            {
                const RECT anchor = search ? slots.back()->GetBounds() : recycle ? slots[count - 1]->GetBounds() : start ? startBase : slots.back()->GetBounds();
                const RECT visual = IsRectEmpty(&anchor) ? anchor : dock->GetElementVisualRect(anchor, pointer);
                expectedAxis = start && !search && !recycle ? trailing(visual) : leading(visual);
            }
            else
            {
                const float scale = ResolveDockScale(app.dockSettings_, false);
                const int spacing = std::max(1, static_cast<int>(std::round(kDockSpacing * scale)));
                const int gap = std::max(1, static_cast<int>(std::round(kDockSeparatorGap * scale)));
                // Fixed pins occupy the leading section of an attached bar.
                // Trailing files/Search/Trash never move this section to the
                // opposite end; only Start's live growth shifts its boundary.
                expectedAxis = start ? trailing(dock->GetElementVisualRect(startBase, pointer)) +
                    ((folder || search || recycle) ? gap : 0) : leading(baseBounds) + spacing / 2;
            }
            referenceContext->SetTarget(complete.Get());
            referenceContext->BeginDraw(); referenceContext->Clear(D2D1::ColorF(0, 0, 0, 0));
            dock->DrawInsertionPreview(referenceContext.Get(), 0);
            check(SUCCEEDED(referenceContext->EndDraw()), "render empty fixed partition with trailing files and utility controls");
            referenceContext->SetTarget(nullptr);
            const auto markerFrame = read(referenceContext.Get(), complete.Get());
            const RECT pixels = paintedBounds(markerFrame);
            const LONG actualAxis = vertical ? (pixels.top + pixels.bottom) / 2 : (pixels.left + pixels.right) / 2;
            const bool matches = !IsRectEmpty(&pixels) && std::abs(actualAxis - expectedAxis) <= 1;
            std::cout << "Legacy empty pixels position=" << static_cast<int>(position) << " effect=" << effect << " attached=" << attached
                << " scene=" << static_cast<int>(scene) << " expected=" << expectedAxis << " marker=" << actualAxis
                << " bbox=" << pixels.left << ',' << pixels.top << ',' << pixels.right << ',' << pixels.bottom << " matches=" << matches << '\n';
            if (!matches)
            {
                ++legacyEmptyFailures;
                const auto name = L"legacy-empty-position-" + std::to_wstring(static_cast<int>(position)) + L"-effect-" + std::to_wstring(effect) +
                    L"-attached-" + std::to_wstring(attached) + L"-scene-" + std::to_wstring(static_cast<int>(scene));
                const auto contents = drawFilePixels(dock, false, 0);
                std::string error;
                check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-contents.png"), static_cast<int>(width), static_cast<int>(height), contents, error), "save legacy empty contents: " + error);
                check(snowdesktop::preview_png::Save(paintEvidence / (name + L"-marker.png"), static_cast<int>(width), static_cast<int>(height), markerFrame, error), "save legacy empty marker: " + error);
            }
            app.widgetAction_ = DesktopApp::WidgetAction::None; app.mouseDown_ = false;
            app.mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
        }
        std::cout << "Empty fixed file/utility D2D matrix: 168 cases; pixel failures=" << legacyEmptyFailures << '\n';
        check(legacyFixedFailures + legacyEmptyFailures == 0,
            "all 240 valid legacy scenes must match their real partition boundaries (see every mismatch frame)");
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
#include "native_dock_frame_access.inc"
