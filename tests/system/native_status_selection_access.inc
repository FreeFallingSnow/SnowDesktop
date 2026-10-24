    static void RunStatusSelection(DesktopApp& app, ID2D1Factory1* factory,
        ID2D1Device* device, ID2D1DeviceContext* context, ID2D1Bitmap1* target)
    {
        const auto require = [](bool value, const char* message) {
            if (!value)
            {
                std::cerr << "R13 assertion: " << message << std::endl;
                throw std::runtime_error(message);
            }
        };
        // Only test-owned windows on an inactive desktop receive invalidations.
        // No DesktopApp::Run, Shell host, real status bar or physical input.
        struct Windows
        {
            DesktopApp& app;
            HWND saved;
            std::array<HWND, 4> values{};
            explicit Windows(DesktopApp& owner) : app(owner), saved(owner.hwnd_)
            {
                for (auto& window : values)
                {
                    window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"R13 isolated invalidation owner",
                        WS_POPUP | WS_VISIBLE, 0, 0, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                    if (!window) throw std::runtime_error("create R13 test-owned window");
                }
                app.hwnd_ = values[0];
                app.floatingPopupHwnd_ = values[3];
                for (size_t i = 1; i <= 2; ++i)
                {
                    auto host = std::make_unique<DesktopApp::PersistentDockHost>();
                    host->owner = &app; host->hwnd = values[i]; host->active = true;
                    app.persistentDockHosts_.push_back(std::move(host));
                }
            }
            void Validate() const { for (auto window : values) ValidateRect(window, nullptr); }
            bool Dirty(size_t index) const { return GetUpdateRect(values[index], nullptr, FALSE) != FALSE; }
            ~Windows()
            {
                app.persistentDockHosts_.clear(); app.floatingPopupHwnd_ = nullptr; app.hwnd_ = saved;
                for (auto window : values) if (window) DestroyWindow(window);
            }
        } windows(app);
        InitListTitleRendering(app, device, context);
        app.d2dFactory_ = factory;
        app.RecreateItemTextFormat();
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item;
            item.layoutKey = L"R13-desktop-" + std::to_wstring(i); item.name = L"R13 Desktop";
            item.gridCell = {app.gridPages_[0].id, i, 0};
            item.bounds = {40 + i * 200, 40, 140 + i * 200, 140};
            app.items_.push_back(std::move(item));
        }
        app.RefreshDesktopItemIndexCache();
        app.containers_.push_back(std::make_unique<DesktopGrid>(&app.gridPages_, &app.items_, &app));
        for (auto& item : app.items_)
            app.items_oo_.push_back(std::make_unique<DesktopIcon>(&item, app.containers_[0].get(), &app));
        app.ClearSelection();
        app.dockFolderPopupOpen_ = true;
        app.dockFolderPopupWidget_ = {};
        app.dockFolderPopupWidget_.type = DesktopWidgetType::FolderMapping;
        app.dockFolderPopupWidget_.id = L"R13-private-popup";
        FolderEntry temporary; temporary.name = L"Temporary local selection"; temporary.selected = true;
        app.dockFolderPopupWidget_.folderEntries.push_back(std::move(temporary));
        app.popupAnimation_.ShowImmediately();
        app.popupRect_ = {500, 40, 950, 400};
        app.popupWidgetIndex_ = static_cast<size_t>(-1);

        const auto press = [&](bool legacy = false, bool right = false, bool blank = false, bool doubleClick = false) {
            snowdesktop::StatusBarInteraction interaction;
            snowdesktop::StatusBarItem item;
            item.key = "taskView"; item.action = snowdesktop::StatusBarAction::TaskView;
            item.bounds = {0, 0, 40, 30};
            const std::vector<snowdesktop::StatusBarItem> items{item};
            unsigned notifications = 0;
            const HWND focus = GetFocus();
            const auto action = interaction.Press(items, blank ? POINT{80, 10} : POINT{10, 10}, right, doubleClick, [&] {
                ++notifications;
                if (legacy)
                {
                    // Execute the exact original R4 callback, including its real
                    // HWND invalidation boundaries, as the negative control.
                    app.ClearSelection();
                    InvalidateRect(app.hwnd_, nullptr, FALSE);
                    app.InvalidatePersistentDockHosts();
                    app.InvalidateFloatingPopupWindow();
                }
                else app.HandleStatusBarPointerPressed();
            });
            require(notifications == 1 && GetFocus() == focus,
                "status Press must clear once before early returns without moving focus");
            require(action == (!right && blank ? snowdesktop::StatusBarAction::Dismiss : snowdesktop::StatusBarAction::None),
                "status blank, context and Task View double-click decisions remain unchanged");
            const auto released = interaction.Release(items, blank ? POINT{80, 10} : POINT{10, 10}, right);
            require(released.accepted == !(!right && (blank || doubleClick)),
                "status release and context-menu handoff preserve their original acceptance");
            require(!app.keyboardNavInsideWidget_ && app.keyboardNavWidgetIndex_ == static_cast<size_t>(-1) &&
                app.keyboardNavMemberIndex_ == -1 && !app.keyboardNavVisualFocus_ && !app.keyboardNavSearchBox_ &&
                !app.keyboardNavCollectionGroupTabs_ && !app.keyboardNavFileGroupCategoryTabs_,
                "every status Press resets the original complete keyboard navigation context");
        };
        windows.Validate();
        press(true);
        require(windows.Dirty(0) && windows.Dirty(1) && windows.Dirty(2) && windows.Dirty(3),
            "old R4 callback must actually invalidate every test host with no changed selection");
        const auto requireClean = [&] {
            require(!windows.Dirty(0) && !windows.Dirty(1) && !windows.Dirty(2) && !windows.Dirty(3),
                "unchanged selection must leave every presentation owner clean");
        };
        bool oldRejected = false;
        try { requireClean(); }
        catch (const std::runtime_error& error)
        {
            if (std::string_view(error.what()) != "unchanged selection must leave every presentation owner clean") throw;
            oldRejected = true;
        }
        require(oldRejected, "the original callback must fail the same zero-update assertion used for the candidate");
        std::cout << "R13 old-behavior failure signal: no selection still updates desktop, both Docks and popup\n";
        for (const bool right : {false, true}) for (const bool blank : {false, true}) for (const bool twice : {false, true})
        {
            windows.Validate();
            press(false, right, blank, twice);
            requireClean();
            require(app.dockFolderPopupWidget_.folderEntries[0].selected,
                "status clear preserves the pre-existing temporary Dock-folder selection contract");
        }

        const auto pixels = [&] {
            ComPtr<ID2D1Bitmap1> readable;
            auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                target->GetPixelFormat());
            require(SUCCEEDED(context->CreateBitmap(target->GetPixelSize(), nullptr, 0, &properties, &readable)) &&
                SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "read R13 real WARP output");
            D2D1_MAPPED_RECT mapped{};
            require(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map R13 WARP output");
            const auto size = target->GetPixelSize();
            std::vector<std::uint32_t> result(static_cast<size_t>(size.width) * size.height);
            for (UINT y = 0; y < size.height; ++y)
                std::memcpy(result.data() + static_cast<size_t>(y) * size.width,
                    mapped.bits + static_cast<size_t>(y) * mapped.pitch, static_cast<size_t>(size.width) * 4);
            require(SUCCEEDED(readable->Unmap()), "unmap R13 WARP output");
            return result;
        };
        const auto paint = [&](const RECT* dirty, const std::function<void()>& draw) {
            context->BeginDraw(); context->SetTransform(D2D1::Matrix3x2F::Identity());
            if (dirty) context->PushAxisAlignedClip(DesktopApp::ToD2DRect(*dirty), D2D1_ANTIALIAS_MODE_ALIASED);
            context->Clear(D2D1::ColorF(.12f, .18f, .24f, 1.f));
            draw();
            if (dirty) context->PopAxisAlignedClip();
            require(SUCCEEDED(context->EndDraw()), "R13 production drawing must finish successfully");
            return pixels();
        };
        const auto compare = [&](const char* scenario, const std::function<void()>& draw,
            size_t owner, bool completeDesktop = false) {
            const auto before = paint(nullptr, draw);
            windows.Validate();
            press();
            RECT dirty{}, client{};
            require(GetUpdateRect(windows.values[owner], &dirty, FALSE) != FALSE,
                "changed selection must invalidate its actual presentation owner");
            for (size_t i = 0; i < windows.values.size(); ++i)
                require(i == owner || !windows.Dirty(i), "R13 excludes unchanged presentation owners");
            GetClientRect(windows.values[owner], &client);
            require(owner != 0 || (EqualRect(&dirty, &client) != FALSE) == completeDesktop,
                "desktop icons retain their full fallback while widget and Dock selection stay local");
            const auto partial = paint(&dirty, draw);
            const auto full = paint(nullptr, draw);
            require(before != full, "R13 fixture must contain real visible selection pixels");
            if (partial != full)
            {
                RECT different{LONG_MAX, LONG_MAX, 0, 0};
                size_t count = 0;
                const auto width = target->GetPixelSize().width;
                for (size_t i = 0; i < partial.size(); ++i)
                {
                    if (partial[i] == full[i]) continue;
                    const LONG x = static_cast<LONG>(i % width), y = static_cast<LONG>(i / width);
                    different.left = std::min(different.left, x); different.top = std::min(different.top, y);
                    different.right = std::max(different.right, x + 1); different.bottom = std::max(different.bottom, y + 1);
                    ++count;
                }
                std::cout << "R13 mismatch " << scenario << ": pixels=" << count
                    << " bounds=" << different.left << ',' << different.top << ',' << different.right << ',' << different.bottom
                    << " dirty=" << dirty.left << ',' << dirty.top << ',' << dirty.right << ',' << dirty.bottom << std::endl;
            }
            require(partial == full, "R13 local update must equal the complete production pixel reference including old titles");
            windows.Validate(); press();
            require(!windows.Dirty(0) && !windows.Dirty(1) && !windows.Dirty(2) && !windows.Dirty(3),
                "second status Press after clearing must perform zero extra host updates");
            std::cout << "R13 pixel reference passed: " << scenario << std::endl;
        };

        app.items_[0].name = L"A deliberately long selected desktop title that extends across several lines below its cell";
        app.items_[0].bounds = {40, 40, 140, 140};
        app.items_[1].bounds = {40, 180, 140, 280};
        app.items_[0].selected = true;
        const RECT originalBounds = app.items_[0].bounds;
        compare("desktop long title full fallback", [&] { app.DrawStaticBackground(context, nullptr, false); }, 0, true);
        require(EqualRect(&originalBounds, &app.items_[0].bounds) && !app.items_[0].selected,
            "desktop selection refresh preserves geometry while clearing the original item");
        require(app.dockFolderPopupWidget_.folderEntries[0].selected, "unrelated popup selection remains untouched");
        app.items_[0].selected = app.items_[1].selected = true;
        compare("desktop multi-selection full fallback", [&] { app.DrawStaticBackground(context, nullptr, false); }, 0, true);
        require(!app.items_[0].selected && !app.items_[1].selected,
            "one status click clears all selected desktop objects including overlapping title layers");

        app.items_[0].largeIcon.emplace();
        auto& large = *app.items_[0].largeIcon;
        large.effect = 1; large.amplitude = 2; large.shadow = true; large.shadowStrength = 1;
        large.backgroundStyle = kAppearancePresetCustom; large.material = 0;
        app.items_[0].bounds = {220, 100, 920, 700}; app.items_[0].selected = true;
        auto& largeRuntime = app.largeIconRuntime_[app.items_[0].layoutKey];
        largeRuntime.motion.tiltX = largeRuntime.motion.tiltY = 1;
        compare("projected large icon full fallback", [&] { app.DrawLargeIcon(context, app.items_[0], app.items_[0].bounds, app.items_[0].selected ? 2 : 0); }, 0, true);
        std::cout << "R13 large-icon complete fallback pixel reference; card projection resources="
            << static_cast<bool>(largeRuntime.cardResources) << '\n';
        app.items_[0].largeIcon.reset(); app.largeIconRuntime_.clear();

        app.dockFolderPopupOpen_ = false;
        auto& group = SeedListTitleScene(app, 3, true, false);
        app.widgets_[0].folderEntries[1].selected = true;
        const RECT groupFrame = group.GetFrameRect();
        compare("grouped file selection", [&] { group.DrawContent(context, group.GetBodyRect()); }, 0);
        const RECT afterGroupFrame = group.GetFrameRect();
        require(!app.widgets_[0].folderEntries[1].selected && EqualRect(&groupFrame, &afterGroupFrame),
            "grouped source selection clears through its real parent without changing geometry");
        windows.Validate();
        app.keyboardNavInsideWidget_ = app.keyboardNavVisualFocus_ = app.keyboardNavSearchBox_ = true;
        app.keyboardNavWidgetIndex_ = 1;
        press();
        require(windows.Dirty(0) && !windows.Dirty(1) && !windows.Dirty(2) && !windows.Dirty(3),
            "keyboard-only widget focus loss refreshes its host even with no selected items");

        app.items_oo_.clear(); app.containers_.clear(); app.widgets_.clear(); app.items_.clear();
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.showWindowsButton = app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showFrequentItems = app.dockSettings_.showRunningApps = false;
        app.dockSettings_.hoverEffect = 0;
        DesktopItem dockItem;
        dockItem.layoutKey = L"R13-private-dock-item"; dockItem.name = L"Private Dock item";
        dockItem.gridCell = {kDockPageId, 0, 0}; app.items_.push_back(std::move(dockItem));
        app.RefreshDesktopItemIndexCache();
        DockEntry selectedEntry;
        selectedEntry.type = DockEntryType::DesktopItem;
        selectedEntry.reference = app.items_[0].layoutKey;
        selectedEntry.selected = true;
        app.dockEntries_ = {std::move(selectedEntry)};
        auto dock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{0, 650, 1200, 800});
        auto* dockView = dock.get(); app.containers_.push_back(std::move(dock));
        (void)dockView->GetSlots();
        const RECT dockFrame = dockView->GetBounds();
        compare("desktop Dock selection", [&] { dockView->DrawChrome(context, app.lastMousePoint_); dockView->DrawContents(context); }, 0);
        const RECT afterDockFrame = dockView->GetBounds();
        require(EqualRect(&dockFrame, &afterDockFrame), "status click preserves final Dock hit geometry");
        app.persistentDockHosts_[0]->container = dockView;
        app.dockEntries_[0].selected = true;
        compare("persistent Dock selection", [&] { dockView->DrawChrome(context, app.lastMousePoint_); dockView->DrawContents(context); }, 1);

        // A shared DesktopItem popup is a regular Collection popup. The
        // temporary Dock-folder route intentionally enumerates folderEntries.
        app.dockFolderPopupOpen_ = false;
        DesktopWidget collection;
        collection.id = L"R13-shared-popup"; collection.type = DesktopWidgetType::Collection;
        app.widgets_.push_back(std::move(collection));
        app.popupWidgetIndex_ = 0;
        DesktopItem shared; shared.layoutKey = L"R13-shared-popup-item"; shared.name = L"Shared"; shared.selected = true;
        app.items_.push_back(std::move(shared)); app.RefreshDesktopItemIndexCache();
        app.widgets_[0].itemKeys = {app.items_.back().layoutKey};
        windows.Validate(); press();
        require(!windows.Dirty(0) && !windows.Dirty(1) && !windows.Dirty(2) && windows.Dirty(3) && !app.items_.back().selected,
            "popup's shared desktop selection refreshes its real floating host alone");
        app.dockFolderPopupOpen_ = false; app.popupWidgetIndex_ = static_cast<size_t>(-1);
        app.persistentDockHosts_[0]->container = nullptr;
        ClearListTitleRendering(app);
        std::cout << "R13 production Press, host routing, final state/geometry and WARP pixel comparisons passed\n";
    }
