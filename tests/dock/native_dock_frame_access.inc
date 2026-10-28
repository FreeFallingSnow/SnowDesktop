    // Exercise the real summon/paint/commit/layer route on test-owned windows.
    // Only the external foreground and native positioning calls are observed.
    static inline DesktopApp* dockFrameApp = nullptr;
    static inline HWND dockFrameWindow = nullptr, dockFrameForeground = nullptr;
    static inline std::uint64_t dockFramePreviousRevision = 0;
    static inline unsigned dockFrameExposures = 0, dockFrameStaleExposures = 0;
    static inline decltype(&SetWindowPos) dockFrameSetPosition = nullptr;
    static inline decltype(&DeferWindowPos) dockFrameDeferPosition = nullptr;

    static void ObserveDockFrameExposure(HWND window, HWND after, UINT flags)
    {
        if (!dockFrameApp || window != dockFrameWindow || (flags & SWP_HIDEWINDOW)) return;
        const bool reveal = (flags & SWP_SHOWWINDOW) != 0;
        const bool promote = !(flags & SWP_NOZORDER) && after == HWND_TOPMOST &&
            IsWindowVisible(window) && !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST);
        if (!reveal && !promote) return;
        ++dockFrameExposures;
        const auto* host = dockFrameApp->FindPersistentDockHost(window);
        const bool complete = host && host->presentationRevision > dockFramePreviousRevision &&
            host->frameReady && !host->compositionPaintInProgress &&
            !host->compositionRenderRecoveryPending && !dockFrameApp->compositionCommitPending_ &&
            !dockFrameApp->graphicsDeviceRecovery_.Pending();
        if (!complete) ++dockFrameStaleExposures;
    }
    static BOOL WINAPI ObserveDockFrameSetPosition(HWND window, HWND after,
        int x, int y, int width, int height, UINT flags)
    {
        ObserveDockFrameExposure(window, after, flags);
        return dockFrameSetPosition(window, after, x, y, width, height, flags);
    }
    static HDWP WINAPI ObserveDockFrameDeferPosition(HDWP batch, HWND window, HWND after,
        int x, int y, int width, int height, UINT flags)
    {
        ObserveDockFrameExposure(window, after, flags);
        return dockFrameDeferPosition(batch, window, after, x, y, width, height, flags);
    }
    static HWND WINAPI DockFrameForeground() { return dockFrameForeground; }

    static DesktopApp& CreateDockFrameScene(const std::filesystem::path& root)
    {
        // Match the existing native fixture's process-lifetime ownership;
        // Run() and global Shell shutdown are outside this isolated test.
        auto* app = new DesktopApp();
        app->gridPages_.emplace_back();
        ConfigureModel(*app, root);
        return *app;
    }
    static void CheckDockFrameTransitions(DesktopApp& app)
    {
        const auto check = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(std::string("Dock frame transition: ") + message);
        };
        const int previousMode = snowdesktop::animation::detail::mode.load();
        snowdesktop::animation::detail::mode.store(snowdesktop::animation::Disabled);
        app.instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = app.instance_; wc.lpfnWndProc = DesktopApp::FloatingDockWndProc;
        wc.lpszClassName = kFloatingDockWindowClassName;
        const bool registered = RegisterClassExW(&wc) != 0;
        check(registered, "register the production Dock window procedure on the private desktop");
        app.hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Dock frame geometry",
            WS_POPUP, 0, 0, 1200, 800, nullptr, nullptr, app.instance_, nullptr);
        dockFrameForeground = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Dock frame foreground",
            WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, app.instance_, nullptr);
        check(app.hwnd_ && dockFrameForeground, "create private geometry and foreground windows");
        struct Cleanup
        {
            DesktopApp& app; int mode; bool registered;
            ~Cleanup()
            {
                dockFrameApp = nullptr; dockFrameWindow = nullptr;
                app.DestroyFloatingDockWindow();
                if (dockFrameForeground) DestroyWindow(dockFrameForeground);
                dockFrameForeground = nullptr;
                if (app.hwnd_) DestroyWindow(app.hwnd_);
                app.hwnd_ = nullptr;
                if (registered) UnregisterClassW(kFloatingDockWindowClassName, app.instance_);
                app.uiAnimationScheduler_.Shutdown();
                app.containers_.clear();
                app.dcompDevice_.Reset(); app.dwriteFactory_.Reset();
                app.d2dContext_.Reset(); app.d2dDevice_.Reset(); app.d2dFactory_.Reset(); app.d3dDevice_.Reset();
                snowdesktop::animation::detail::mode.store(mode);
            }
        } cleanup{app, previousMode, registered};
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create isolated WARP graphics");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))) &&
            app.uiAnimationScheduler_.Initialize(), "initialize real D2D/DComp painting and scheduling");
        app.gridPages_.front().bounds = app.gridPages_.front().workArea =
            app.gridPages_.front().visualWorkArea = app.layoutWorkArea_ = {0, 0, 1200, 800};
        app.virtualLeft_ = app.virtualTop_ = 0; app.virtualWidth_ = 1200; app.virtualHeight_ = 800;
        app.generalSettings_.dockEnabled = true;
        app.startupInitializationComplete_ = true;
        app.desktopStartupPresentationPending_ = false;
        app.customDesktopVisible_ = true; app.desktopItemsReady_ = true;
        app.personalizationSettings_ = PersonalizationSettings::DarkPreset();
        app.personalizationSettings_.glassEnabled = app.personalizationSettings_.acrylicEnabled = false;
        app.dockSettings_.followComponentAppearance = true;
        app.dockSettings_.showWindowsButton = app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showFrequentItems = false; app.dockSettings_.showRunningApps = true;
        app.dockSettings_.hoverEffect = 0;
        app.floatingDockPromotionInputUpdateActive_ = true; // Input capability acquisition is outside this paint contract.
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        app.dockEntries_.clear(); app.containers_.clear();
        const auto running = [&](size_t count) {
            app.dockUnpinnedRunningApps_.clear();
            for (size_t index = 0; index < count; ++index)
            {
                DockRunningAppInfo item;
                item.identityKey = L"frame-running-" + std::to_wstring(index);
                item.title = L"Frame running application";
                item.presence.SetVisible(true, 0, false);
                app.dockUnpinnedRunningApps_.push_back(std::move(item));
            }
            app.InvalidateDockContainers();
        };
        dockFrameSetPosition = reinterpret_cast<decltype(dockFrameSetPosition)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowPos"));
        dockFrameDeferPosition = reinterpret_cast<decltype(dockFrameDeferPosition)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "DeferWindowPos"));
        check(dockFrameSetPosition && dockFrameDeferPosition, "resolve native position boundaries");
        InputSafetyImport foreground("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&DockFrameForeground));
        InputSafetyImport setPosition("SetWindowPos", reinterpret_cast<ULONG_PTR>(&ObserveDockFrameSetPosition));
        InputSafetyImport deferPosition("DeferWindowPos", reinterpret_cast<ULONG_PTR>(&ObserveDockFrameDeferPosition));
        for (const auto position : {DockPosition::Bottom, DockPosition::Left})
        for (const bool summonOnly : {false, true})
        {
            app.DestroyFloatingDockWindow(); app.containers_.clear();
            app.dockSettings_.position = position; app.dockSettings_.showOnlyWhenSummoned = summonOnly;
            running(1);
            auto container = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{100, 100, 1100, 700});
            auto* dock = container.get(); app.containers_.push_back(std::move(container));
            check(app.SyncPersistentDockHosts() && app.floatingDockHost_, "prepare the real persistent Dock host");
            auto& host = *app.floatingDockHost_;
            check(host.frameReady && (IsWindowVisible(host.hwnd) != FALSE) == !summonOnly,
                "initial Dock presentation matches the ordinary or summon-only mode");
            for (const size_t count : {size_t{3}, size_t{1}, size_t{3}})
            {
                const RECT before = dock->GetBounds();
                const RECT beforeCell = dock->GetSlots().front()->GetBounds();
                const int pitch = position == DockPosition::Left
                    ? beforeCell.bottom - beforeCell.top : beforeCell.right - beforeCell.left;
                const auto previousCount = app.dockUnpinnedRunningApps_.size();
                running(count);
                const RECT expected = dock->GetBounds();
                const bool vertical = position == DockPosition::Left;
                const int beforeLength = vertical ? before.bottom - before.top : before.right - before.left;
                const int expectedLength = vertical ? expected.bottom - expected.top : expected.right - expected.left;
                check(expectedLength - beforeLength == (static_cast<int>(count) - static_cast<int>(previousCount)) * pitch,
                    "running-area change has an independently known axis-length delta");
                dockFrameApp = &app; dockFrameWindow = host.hwnd;
                dockFramePreviousRevision = host.presentationRevision;
                dockFrameExposures = dockFrameStaleExposures = 0;
                app.InvalidateFloatingDockWindow(host, false);
                app.ShowFloatingDock(host.monitor, DockRevealSource::Hotkey);
                dockFrameApp = nullptr;
                std::cout << "Dock first-frame mode=" << summonOnly << " vertical=" << vertical << " running=" << count
                    << " exposures=" << dockFrameExposures << " stale=" << dockFrameStaleExposures << '\n';
                check(dockFrameExposures > 0, "observe a real native promotion or reveal before checking frame order");
                check(dockFrameStaleExposures == 0,
                    "a visible promotion must commit the current complete Dock before exposing its retained older-width frame");
                check(host.frameReady && !host.compositionRenderRecoveryPending && !app.graphicsDeviceRecovery_.Pending(),
                    "the prepared Dock frame completes without recovery");
                app.CloseFloatingDock(host, FloatingDockCloseFocusPolicy::PreserveCurrent);
            }
        }
    }
