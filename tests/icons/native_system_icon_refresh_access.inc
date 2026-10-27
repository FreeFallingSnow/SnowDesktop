    // Exercise the actual async system-icon completion route. Only Win32
    // presentation visibility is supplied by message-only fixture windows.
    static inline std::vector<HWND> systemIconRefreshCalls;
    static inline HWND systemIconRefreshPopup = nullptr;
    static inline decltype(&InvalidateRect) systemIconInvalidateOriginal = nullptr;
    static inline decltype(&IsWindowVisible) systemIconVisibleOriginal = nullptr;
    static BOOL WINAPI RecordSystemIconInvalidation(HWND window, const RECT* rect, BOOL erase)
    {
        systemIconRefreshCalls.push_back(window);
        return systemIconInvalidateOriginal(window, rect, erase);
    }
    static BOOL WINAPI SystemIconFixtureVisible(HWND window)
    {
        return window == systemIconRefreshPopup ? TRUE : systemIconVisibleOriginal(window);
    }
    static void RunSystemIconRefreshProbe(DesktopApp& app, ID2D1DeviceContext* context,
        int index, const std::function<HBITMAP(bool)>& referenceSource)
    {
        const auto check = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        struct Windows
        {
            std::vector<HWND> values;
            ~Windows() { for (const auto window : values) DestroyWindow(window); }
        } windows;
        const auto makeWindow = [&] {
            const auto window = CreateWindowExW(0, L"STATIC", L"System icon notification probe", 0,
                0, 0, 64, 64, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
            check(window != nullptr, "create message-only system-icon consumer");
            windows.values.push_back(window); return window;
        };
        const HWND desktop = makeWindow(), navigation = makeWindow(), dock = makeWindow(), popup = makeWindow();
        const auto savedDesktop = std::exchange(app.hwnd_, desktop);
        const auto savedNavigation = std::exchange(app.quickNavigationHwnd_, navigation);
        const auto savedPopup = std::exchange(app.floatingPopupHwnd_, popup);
        auto savedHosts = std::move(app.persistentDockHosts_);
        const auto savedFloating = std::exchange(app.floatingDockHostActive_, true);
        const auto savedPanel = app.luaWidgetPanelRequest_;
        const auto savedContext = app.d2dContext_;
        const auto savedStyle = app.iconBeautifySettings_;
        auto savedQuick = std::move(app.quickNavSysIconCache_);
        auto savedShared = std::move(app.placeholderIconCache_);
        auto savedGlass = std::move(app.iconGlassBackdrop_);
        auto savedBitmapStyles = std::move(app.iconBitmapStyles_);
        const auto savedMouseDown = app.mouseDown_;
        const auto savedBackdrop = app.desktopBackdropFullCollectionPending_;
        app.d2dContext_ = context;
        app.iconBeautifySettings_ = snowdesktop::icon_beautify::MakePreset(
            snowdesktop::IconBeautifyPreset::FrostedGlass);
        app.luaWidgetPanelRequest_.widgetId = L"system-icon-fixture";
        auto host = std::make_unique<DesktopApp::PersistentDockHost>();
        host->owner = &app; host->hwnd = dock; host->active = true;
        app.persistentDockHosts_.push_back(std::move(host));
        systemIconRefreshPopup = popup;
        const auto cleanup = [&] {
            app.shellVisualWork_.Cancel(L"quick-system:");
            app.shellVisualWork_.Cancel(L"system:");
            app.mouseDown_ = savedMouseDown;
            app.hwnd_ = savedDesktop;
            app.desktopBackdropFullCollectionPending_ = savedBackdrop;
            app.quickNavigationHwnd_ = savedNavigation;
            app.floatingPopupHwnd_ = savedPopup;
            app.persistentDockHosts_ = std::move(savedHosts);
            app.floatingDockHostActive_ = savedFloating;
            app.luaWidgetPanelRequest_ = savedPanel;
            app.d2dContext_ = savedContext;
            app.iconBeautifySettings_ = savedStyle;
            app.quickNavSysIconCache_ = std::move(savedQuick);
            app.placeholderIconCache_ = std::move(savedShared);
            app.iconGlassBackdrop_ = std::move(savedGlass);
            app.iconBitmapStyles_ = std::move(savedBitmapStyles);
            systemIconRefreshPopup = nullptr;
        };
        try
        {
            InputSafetyImport invalidation("InvalidateRect", reinterpret_cast<ULONG_PTR>(&RecordSystemIconInvalidation));
            systemIconInvalidateOriginal = reinterpret_cast<decltype(&InvalidateRect)>(invalidation.original);
            InputSafetyImport visibility("IsWindowVisible", reinterpret_cast<ULONG_PTR>(&SystemIconFixtureVisible));
            systemIconVisibleOriginal = reinterpret_cast<decltype(&IsWindowVisible)>(visibility.original);
            const auto key = [&](bool quick) {
                return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(index)) << 32) |
                    static_cast<std::uint64_t>(quick ? 64 : 129);
            };
            const auto clear = [&] {
                app.quickNavSysIconCache_.clear(); app.placeholderIconCache_.clear();
                app.desktopBackdropFullCollectionPending_ = false;
                systemIconRefreshCalls.clear();
                MSG message{};
                while (PeekMessageW(&message, app.hwnd_, kBackgroundShellReadyMessage,
                    kBackgroundShellReadyMessage, PM_REMOVE)) {}
            };
            // Waiting on the actual producer notification proves the requested
            // worker has completed, including failure paths with no cache entry.
            const auto waitReady = [&] {
                const auto deadline = GetTickCount64() + 10000;
                MSG message{};
                while (GetTickCount64() < deadline)
                {
                    if (PeekMessageW(&message, app.hwnd_, kBackgroundShellReadyMessage,
                        kBackgroundShellReadyMessage, PM_REMOVE)) return;
                    Sleep(1);
                }
                check(false, "system-icon worker must notify completion within the bounded fixture wait");
            };
            const auto count = [&](HWND window) {
                return std::count(systemIconRefreshCalls.begin(), systemIconRefreshCalls.end(), window);
            };
            constexpr const char* unrelatedRefreshFailure =
                "navigation-only system icons must not invalidate desktop, Dock or popup; shared icons retain all consumers";
            const auto verifyRefresh = [&](bool quick, std::uint64_t revision) {
                check(count(navigation) == 1, "an accepted system icon must request exactly one navigation redraw");
                check(count(app.hwnd_) == (quick ? 0 : 1) && count(dock) == (quick ? 0 : 1) &&
                    count(popup) == (quick ? 0 : 1),
                    unrelatedRefreshFailure);
                check(app.dragSession_.StaticSceneRevision() == revision + (quick ? 0 : 1) &&
                    app.desktopBackdropFullCollectionPending_ == !quick,
                    "only shared system icons invalidate drag pixels and request full desktop glass collection");
            };
            const auto renderPixels = [&](bool quick, ID2D1Bitmap* reference) {
                const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ComPtr<ID2D1Bitmap1> surface;
                check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(96, 96), nullptr, 0, &properties, &surface)),
                    "allocate the actual system-icon consumer surface");
                ComPtr<ID2D1Image> savedTarget;
                context->GetTarget(&savedTarget);
                D2D1_MATRIX_3X2_F savedTransform{};
                context->GetTransform(&savedTransform);
                const auto savedQuickPaint = app.quickNavCompositionPaintInProgress_;
                const auto savedPopupPaint = app.renderingFloatingPopup_;
                const auto restore = [&] {
                    context->SetTarget(savedTarget.Get()); context->SetTransform(savedTransform);
                    app.quickNavCompositionPaintInProgress_ = savedQuickPaint;
                    app.renderingFloatingPopup_ = savedPopupPaint;
                };
                app.quickNavCompositionPaintInProgress_ = quick;
                app.renderingFloatingPopup_ = !quick;
                context->SetTarget(surface.Get()); context->SetTransform(D2D1::Matrix3x2F::Identity());
                context->BeginDraw();
                context->Clear(D2D1::ColorF(0.2f, 0.3f, 0.4f, 0.7f));
                const RECT destination{12, 8, 76, 72};
                if (reference) app.DrawIconBitmap(context, reference, destination);
                else if (quick) app.DrawQuickNavSysIcon(context, index, destination);
                else app.DrawPlaceholderIcon(context, index, destination, 1.0f);
                const auto drawn = context->EndDraw();
                restore();
                check(SUCCEEDED(drawn), "complete the real system-icon consumer drawing");
                const auto readableProperties = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, surface->GetPixelFormat());
                ComPtr<ID2D1Bitmap1> readable;
                check(SUCCEEDED(context->CreateBitmap(surface->GetPixelSize(), nullptr, 0,
                    &readableProperties, &readable)) &&
                    SUCCEEDED(readable->CopyFromBitmap(nullptr, surface.Get(), nullptr)),
                    "copy the real consumer output for complete pixel comparison");
                D2D1_MAPPED_RECT mapped{};
                check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map the system-icon output pixels");
                std::vector<std::uint32_t> pixels(96 * 96);
                for (size_t row = 0; row < 96; ++row)
                    std::memcpy(pixels.data() + row * 96, mapped.bits + row * mapped.pitch, 96 * sizeof(std::uint32_t));
                check(SUCCEEDED(readable->Unmap()), "unmap the system-icon output pixels");
                return pixels;
            };
            for (const bool quick : {true, false})
            {
                clear();
                const auto revision = app.dragSession_.StaticSceneRevision();
                app.QueueSystemIconBitmap(index, 64, true, key(quick), quick);
                app.QueueSystemIconBitmap(index, 64, true, key(quick), quick);
                waitReady();
                app.DrainBackgroundShellWork();
                auto& cache = quick ? app.quickNavSysIconCache_ : app.placeholderIconCache_;
                check(cache.contains(key(quick)) && cache.size() == 1 &&
                    (quick ? app.placeholderIconCache_ : app.quickNavSysIconCache_).empty(),
                    "actual async completion publishes only its consumer cache and coalesces a pending duplicate");
                verifyRefresh(quick, revision);
                const auto actual = ReadIconPixelBitmap(app, context, cache.at(key(quick)).Get());
                const auto source = referenceSource(quick);
                check(source != nullptr, "obtain a full synchronous system-icon reference independent of completion routing");
                const auto referenceBitmap = app.CreateD2DBitmapFromHBitmap(source, true);
                DeleteObject(source);
                const auto reference = ReadIconPixelBitmap(app, context, referenceBitmap.Get());
                check(actual.buffer.width == reference.buffer.width && actual.buffer.height == reference.buffer.height &&
                    actual.buffer.pixels == reference.buffer.pixels && actual.glass == reference.glass,
                    "async system-icon cache must retain the full reference pixels, dimensions and glass metadata");
                check(renderPixels(quick, nullptr) == renderPixels(quick, referenceBitmap.Get()),
                    "actual navigation and shared-placeholder draw routes must match the complete reference surface pixels");
                verifyRefresh(quick, revision);
                if (quick)
                {
                    // Restore the original four excess completion actions on
                    // this real accepted navigation result. Its one navigation
                    // notification already ran; keep it unchanged so the
                    // oracle fails specifically on the unrelated-host refresh.
                    app.InvalidateDragStaticScene();
                    InvalidateRect(app.hwnd_, nullptr, FALSE);
                    app.InvalidateDockRects();
                    app.InvalidateFloatingPopupWindow(false);
                    check(count(navigation) == 1 && count(app.hwnd_) == 1 && count(dock) == 1 && count(popup) == 1 &&
                        app.dragSession_.StaticSceneRevision() == revision + 1 && app.desktopBackdropFullCollectionPending_,
                        "the original excess refresh actions must actually reach every old consumer and drag/glass state");
                    bool legacyRejected = false;
                    try { verifyRefresh(true, revision); }
                    catch (const std::runtime_error& error)
                    { legacyRejected = std::string_view(error.what()) == unrelatedRefreshFailure; }
                    check(legacyRejected,
                        "the same business oracle must reject the original excess completion actions at the unrelated-host assertion");
                    std::cout << "Original system-icon excess refresh actions were observed and rejected by the unrelated-host assertion\n";
                }
            }
            for (const bool quick : {true, false})
            {
                clear();
                const auto revision = app.dragSession_.StaticSceneRevision();
                app.QueueSystemIconBitmap(index, 64, true, key(quick), quick);
                waitReady();
                app.shellVisualWork_.Cancel(quick ? L"quick-system:" : L"system:");
                app.DrainBackgroundShellWork();
                check(app.quickNavSysIconCache_.empty() && app.placeholderIconCache_.empty() &&
                    systemIconRefreshCalls.empty() && app.dragSession_.StaticSceneRevision() == revision &&
                    !app.desktopBackdropFullCollectionPending_,
                    "cancelled ready tickets must neither publish a stale icon nor invalidate any consumer");
                app.mouseDown_ = true;
                app.QueueSystemIconBitmap(index, 64, true, key(quick), quick);
                waitReady();
                app.DrainBackgroundShellWork();
                check(app.quickNavSysIconCache_.empty() && app.placeholderIconCache_.empty() && systemIconRefreshCalls.empty(),
                    "the production pointer/drag delivery fence must still defer system-icon completion");
                app.mouseDown_ = false;
                app.DrainBackgroundShellWork();
                check((quick ? app.quickNavSysIconCache_ : app.placeholderIconCache_).contains(key(quick)),
                    "a fresh ticket after cancellation must publish when the existing delivery fence opens");
                verifyRefresh(quick, revision);
            }
            for (const bool quick : {true, false}) for (const bool gpuFailure : {false, true})
            {
                clear();
                const auto revision = app.dragSession_.StaticSceneRevision();
                app.QueueSystemIconBitmap(gpuFailure ? index : -1, 64, true, key(quick), quick);
                waitReady();
                if (gpuFailure) app.d2dContext_.Reset();
                app.DrainBackgroundShellWork();
                app.d2dContext_ = context;
                check(app.quickNavSysIconCache_.empty() && app.placeholderIconCache_.empty() &&
                    systemIconRefreshCalls.empty() && app.dragSession_.StaticSceneRevision() == revision &&
                    !app.desktopBackdropFullCollectionPending_,
                    "Shell extraction or device upload failure must preserve the no-publication/no-redraw behavior");
            }
            std::cout << "System-icon async routing, full reference pixels, coalescing, cancellation, failure and delivery fences passed\n";
        }
        catch (...) { cleanup(); throw; }
        cleanup();
    }
