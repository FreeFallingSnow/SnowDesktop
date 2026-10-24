    static inline HWND runningRefreshOutput = nullptr;
    static inline decltype(&InvalidateRect) runningRefreshInvalidate = nullptr;
    static inline unsigned runningRefreshLegacyCalls = 0;
    static BOOL WINAPI RunningRefreshLegacyDamage(HWND window, const RECT* rect, BOOL erase)
    {
        if (window == runningRefreshOutput)
        {
            ++runningRefreshLegacyCalls;
            std::cerr << "R07 negative control: production invalidation escalated to full desktop\n";
            return runningRefreshInvalidate(window, nullptr, erase);
        }
        return runningRefreshInvalidate(window, rect, erase);
    }
    // OS discovery and Win32 damage are production paths. The fixture owns
    // the input HWND and replaces only icon pixels and the output surface.
    static void CheckRunningRefresh(DesktopApp& app, HWND window,
        ID2D1DeviceContext* context, bool forceLegacy = false)
    {
        const auto check = [](bool ok, const char* text) {
            if (!ok) throw std::runtime_error(text);
        };
        const int mode = snowdesktop::animation::detail::mode.exchange(
            snowdesktop::animation::Disabled);
        const auto moduleMask = snowdesktop::runtime_modules::policyMask.exchange(0);
        const HWND output = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"Dock running damage fixture", WS_POPUP,
            -16000, -16000, 1200, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(output != nullptr, "create hidden, owned damage receiver without a desktop host");
        const auto cleanup = [&] {
            app.dockIconWork_.Stop(); app.shellVisualWork_.Stop();
            for (auto& item : app.dockUnpinnedRunningApps_)
                if (item.iconBitmap) { app.EraseD2DIconCacheForBitmap(item.iconBitmap); DeleteObject(item.iconBitmap); }
            app.dockUnpinnedRunningApps_.clear();
            app.containers_.clear(); app.items_oo_.clear(); app.items_.clear();
            app.d2dIconCache_.clear(); app.d2dContext_.Reset(); app.dwriteFactory_.Reset();
            app.hwnd_ = completionWindow;
            DestroyWindow(output);
            snowdesktop::animation::detail::mode.store(mode);
            snowdesktop::runtime_modules::policyMask.store(moduleMask);
            runningRefreshOutput = nullptr;
        };
        try
        {
            std::unique_ptr<InputSafetyImport> legacyDamage;
            wchar_t legacyFlag[2]{};
            if (forceLegacy || GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_R07_LEGACY_DAMAGE", legacyFlag, 2))
            {
                runningRefreshLegacyCalls = 0;
                runningRefreshOutput = output;
                legacyDamage = std::make_unique<InputSafetyImport>("InvalidateRect",
                    reinterpret_cast<ULONG_PTR>(&RunningRefreshLegacyDamage));
                runningRefreshInvalidate = reinterpret_cast<decltype(&InvalidateRect)>(legacyDamage->original);
            }
            app.items_oo_.clear(); app.items_.clear(); app.containers_.clear();
            app.mouseDown_ = false;
            app.generalSettings_.dockEnabled = true;
            app.dockSettings_.showWindowsButton = false;
            app.dockSettings_.showQuickSearchButton = false;
            app.dockSettings_.showFrequentItems = false;
            app.dockSettings_.showRunningApps = true;
            app.dockSettings_.hoverEffect = 0;
            app.dockSettings_.showOnlyWhenSummoned = false;
            app.dockSettings_.edgeAttached = false;
            app.dockSettings_.position = DockPosition::Bottom;
            app.dockEntries_.clear();
            app.gridPages_.front().bounds = app.gridPages_.front().workArea =
                app.gridPages_.front().visualWorkArea = RECT{0, 0, 1200, 800};
            app.hwnd_ = output;
            app.d2dContext_ = context;
            check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.ReleaseAndGetAddressOf()))),
                "create real title measurement and glyph renderer");
            app.containers_.push_back(std::make_unique<DockContainer>(&app,
                &app.dockEntries_, RECT{100, 600, 1100, 700}));
            auto& dock = *static_cast<DockContainer*>(app.containers_.back().get());
            app.RefreshDockRunningWindows(false);
            const auto tracked = [&]() -> DockRunningAppInfo& {
                const auto found = std::find_if(app.dockUnpinnedRunningApps_.begin(),
                    app.dockUnpinnedRunningApps_.end(), [window](const auto& item) {
                        return std::find(item.trackedWindows.begin(), item.trackedWindows.end(), window)
                            != item.trackedWindows.end();
                    });
                if (found == app.dockUnpinnedRunningApps_.end())
                    throw std::runtime_error("actual discovery must retain owned external task window");
                return *found;
            };
            (void)tracked();
            // Deterministic icon pixels replace asynchronous Shell artwork only.
            // Discovery, identity ordering, title/state, layout and drawing stay real.
            const auto supplyIcons = [&] {
                for (auto& item : app.dockUnpinnedRunningApps_)
                {
                    if (item.iconRequestedSize == 32767) continue;
                    if (item.iconBitmap) { app.EraseD2DIconCacheForBitmap(item.iconBitmap); DeleteObject(item.iconBitmap); }
                    std::vector<DWORD> pixels(32 * 32, 0xff4080c0);
                    item.iconBitmap = CreateBitmap(32, 32, 1, 32, pixels.data());
                    check(item.iconBitmap != nullptr, "create isolated stable running icon pixels");
                    item.iconBitmapSize = {32, 32}; item.iconRequestedSize = 32767;
                }
            };
            supplyIcons();
            const auto focus = [&] {
                for (const auto& slot : dock.GetSlots())
                    if (const auto* item = dynamic_cast<DockRunningItem*>(slot->GetItem());
                        item && item->GetIdentityKey() == tracked().identityKey)
                    {
                        const RECT bounds = slot->GetBounds();
                        app.lastMousePoint_ = {(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
                        return;
                    }
                throw std::runtime_error("actual Dock slots must expose the discovered task");
            };
            focus();
            const RECT visibleTitle = dock.GetHoveredTitleBounds(app.lastMousePoint_);
            check(!IsRectEmpty(&visibleTitle), "real title measurement must expose the fixture's hovered title");
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1Bitmap1> partial, full;
            check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(1200, 800), nullptr, 0, &props, &partial)) &&
                SUCCEEDED(context->CreateBitmap(D2D1::SizeU(1200, 800), nullptr, 0, &props, &full)),
                "allocate independent old/partial and full-reference targets");
            const auto draw = [&](ID2D1Bitmap1* target, const RECT* dirty) {
                context->SetTarget(target); context->BeginDraw();
                context->SetTransform(D2D1::Matrix3x2F::Identity());
                if (dirty) context->PushAxisAlignedClip(D2D1::RectF(
                    static_cast<float>(dirty->left), static_cast<float>(dirty->top),
                    static_cast<float>(dirty->right), static_cast<float>(dirty->bottom)),
                    D2D1_ANTIALIAS_MODE_ALIASED);
                context->Clear(D2D1::ColorF(0, 0, 0, 0));
                dock.DrawChrome(context, app.lastMousePoint_); dock.DrawContents(context);
                if (dirty) context->PopAxisAlignedClip();
                check(SUCCEEDED(context->EndDraw()), "render actual Dock chrome, contents, indicators and title");
                context->SetTarget(nullptr);
            };
            const auto read = [&](ID2D1Bitmap1* target) {
                ComPtr<ID2D1Bitmap1> cpu;
                const auto cpuProps = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, props.pixelFormat);
                check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(1200, 800), nullptr, 0, &cpuProps, &cpu)) &&
                    SUCCEEDED(cpu->CopyFromBitmap(nullptr, target, nullptr)), "copy actual Dock reference pixels");
                D2D1_MAPPED_RECT mapped{};
                check(SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map actual Dock pixels");
                std::vector<std::uint8_t> pixels(1200 * 800 * 4);
                for (size_t y = 0; y < 800; ++y)
                    std::memcpy(pixels.data() + y * 1200 * 4, mapped.bits + y * mapped.pitch, 1200 * 4);
                cpu->Unmap(); return pixels;
            };
            const auto identities = [&] {
                std::vector<std::wstring> values;
                for (const auto& item : app.dockUnpinnedRunningApps_) values.push_back(item.identityKey);
                return values;
            };
            const auto refresh = [&](bool visualChange, bool layoutChange) {
                ValidateRect(output, nullptr);
                const RECT before = dock.GetInteractiveBounds();
                const auto order = identities();
                app.RefreshDockRunningWindows();
                supplyIcons();
                RECT damage{};
                const bool invalidated = GetUpdateRect(output, &damage, FALSE) != FALSE;
                check(invalidated == visualChange, "unchanged discovery submits zero invalidation; changed content submits damage");
                if (visualChange)
                {
                    check(damage.top > 100 && damage.bottom <= 800,
                        "running-window changes must not invalidate unrelated desktop pixels (old full route fails here)");
                    const RECT after = dock.GetInteractiveBounds();
                    check(damage.left <= before.left && damage.top <= before.top &&
                        damage.right >= before.right && damage.bottom >= before.bottom &&
                        damage.left <= after.left && damage.top <= after.top &&
                        damage.right >= after.right && damage.bottom >= after.bottom,
                        "actual damage must cover old and new geometry, including shrink/growth");
                    draw(partial.Get(), &damage);
                }
                draw(full.Get(), nullptr);
                check(read(partial.Get()) == read(full.Get()),
                    "old complete pixels plus actual partial refresh must equal an independent complete Dock draw");
                if (!layoutChange)
                {
                    const RECT after = dock.GetInteractiveBounds();
                    check(order == identities() && EqualRect(&before, &after),
                        "content updates preserve actual identity order and Dock geometry");
                    check(dock.RunningItemAtPoint(app.lastMousePoint_) != nullptr,
                        "content updates retain the actual running-item hit target");
                }
            };
            draw(partial.Get(), nullptr);
            refresh(false, false);
            tracked().title = L"stale cached title with a different width";
            draw(partial.Get(), nullptr);
            refresh(true, false);
            wchar_t title[512]{}; GetWindowTextW(window, title, 512);
            check(tracked().title == title, "real discovery restores the actual external window title");
            tracked().foreground = !tracked().foreground;
            tracked().minimized = !tracked().minimized;
            draw(partial.Get(), nullptr);
            refresh(true, false);
            check(!tracked().selected && tracked().minimized == (IsIconic(window) != FALSE),
                "real state refresh preserves selection and current minimized feedback");
            tracked().selected = true;
            draw(partial.Get(), nullptr);
            refresh(false, false);
            check(tracked().selected, "unchanged discovery preserves an actual selected running item");
            DockRunningAppInfo departed;
            departed.identityKey = L"owned-departed-running-fixture";
            departed.title = L"departed";
            departed.presence.SetVisible(true, 0, false);
            app.dockUnpinnedRunningApps_.push_back(std::move(departed));
            dock.InvalidateSlots(); draw(partial.Get(), nullptr);
            refresh(true, true);
            check(std::none_of(app.dockUnpinnedRunningApps_.begin(), app.dockUnpinnedRunningApps_.end(),
                [](const auto& item) { return item.identityKey == L"owned-departed-running-fixture"; }),
                "departed presentation reaches zero and leaves the actual running model");
            refresh(false, false);
            for (auto& item : app.dockUnpinnedRunningApps_)
                if (item.iconBitmap) { app.EraseD2DIconCacheForBitmap(item.iconBitmap); DeleteObject(item.iconBitmap); }
            app.dockUnpinnedRunningApps_.clear();
            dock.InvalidateSlots(); draw(partial.Get(), nullptr);
            refresh(true, true);
            check(tracked().presence.Amount() == 1.0f, "new discovered task reaches its full disabled-motion geometry");
            refresh(false, false);
            check(!IsWindowVisible(output), "pixel regression must never reveal a desktop host");
            std::cout << "R07 actual discovery, unchanged/title/state/shrink/growth damage, hits and WARP full/partial pixels passed\n";
        }
        catch (...) { cleanup(); throw; }
        cleanup();
    }
