    static inline UINT previewTestModifiers = 0;
    static SHORT WINAPI PreviewTestKeys(int key)
    {
        const bool pressed = (key == VK_CONTROL && (previewTestModifiers & MOD_CONTROL)) ||
            (key == VK_MENU && (previewTestModifiers & MOD_ALT)) ||
            (key == VK_SHIFT && (previewTestModifiers & MOD_SHIFT)) ||
            (key == VK_LWIN && (previewTestModifiers & MOD_WIN));
        return pressed ? static_cast<SHORT>(0x8000) : 0;
    }

    static void CheckFilePreview(DesktopApp& app, const std::filesystem::path& root)
    {
        namespace preview = snowdesktop::file_preview;
        const auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        const auto await = [&](auto predicate, const char* message) {
            const auto deadline = GetTickCount64() + 5000;
            while (!predicate()) { require(GetTickCount64() < deadline, message); Sleep(2); }
        };
        require(NoPresentationWindows(app), "preview routing uses only an isolated message-only model owner");
        struct Capture
        {
            std::mutex mutex;
            preview::Availability available{{{preview::Kind::QuickLook, {}, L"test-provider"}}};
            std::vector<std::pair<preview::Kind, std::wstring>> requests;
        };
        const auto capture = std::make_shared<Capture>();
        app.filePreview_ = preview::Service({
            [capture] { std::lock_guard lock(capture->mutex); return capture->available; },
            [capture](const auto& provider, const std::wstring& path, const preview::PublishSession& publish) {
                // Only the external program boundary is replaced. Host key
                // routing, visible selection and async revalidation stay real.
                publish(preview::IdentifyProcess(GetCurrentProcessId()));
                std::lock_guard lock(capture->mutex);
                capture->requests.emplace_back(provider.kind, path); return true;
            }});
        struct Stop { preview::Service& service; ~Stop() { service.Stop(); } } stop{app.filePreview_};
        app.filePreview_.Start();
        await([&] { return app.filePreview_.Matches({}); }, "initial host preview detection must complete");
        previewTestModifiers = 0;
        InputSafetyImport keys("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&PreviewTestKeys));
        const auto count = [&] { std::lock_guard lock(capture->mutex); return capture->requests.size(); };
        const auto first = root / L"files/one 中文.txt", second = root / L"files/two.txt";
        std::filesystem::create_directories(first.parent_path());
        std::ofstream(first) << "first"; std::ofstream(second) << "second";
        app.generalSettings_.pageNavigationKeyboardEnabled = false;
        PopulateModel(app, {first, second}, 0);
        app.LayoutItems();
        app.items_[1].selected = false;
        require(app.GetSelectedFilePreviewPath() == first.wstring(), "the production desktop selection resolves its real file path");
        app.HandleInputMessage(completionWindow, WM_KEYDOWN, VK_SPACE, 1);
        require(app.typeToSelectSuppressChar_, "the native key dispatcher consumes the preview key and its following WM_CHAR");
        app.HandleInputMessage(completionWindow, WM_CHAR, L' ', 1);
        require(app.items_[0].selected && !app.items_[1].selected, "preview Space cannot enter type-to-select or change the selection");
        await([&] { return count() == 1; }, "the real native key path delivers one provider request");
        {
            std::lock_guard lock(capture->mutex);
            require(capture->requests[0].first == preview::Kind::QuickLook &&
                capture->requests[0].second == first.wstring(), "the host delivers the exact current selection to the selected provider");
        }
        require(app.OnKeyDown(VK_SPACE, true), "repeat handling consumes without another preview invocation");
        app.renameController_.BeginDesktopItem(0);
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "active rename retains Space for text input");
        app.renameController_.Reset();
        app.typeToSelectImeComposing_ = true;
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "IME composition cannot activate external preview");
        app.typeToSelectImeComposing_ = false;
        app.quickNavigationOpen_ = true;
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "quick navigation retains its keyboard context");
        app.quickNavigationOpen_ = false;
        app.luaWidgetPanelRequest_.widgetId = L"isolated-lua-input";
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "a Lua panel retains its keyboard context");
        app.luaWidgetPanelRequest_.widgetId.clear();
        app.mouseDown_ = true;
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "a held pointer/drag cannot start a preview");
        app.mouseDown_ = false;
        app.items_[0].desktopIconClsid = L"::{virtual-system-item}";
        require(app.GetSelectedFilePreviewPath().empty(), "virtual shell icons cannot be passed as ordinary files");
        app.items_[0].desktopIconClsid.clear();
        app.ClearSelection();
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "empty selection leaves the key available to normal input");

        const HWND previewWindow = CreateWindowExW(0, L"STATIC", L"preview test focus", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(previewWindow != nullptr, "create a second message-only preview identity");
        app.items_[0].selected = true;
        app.ClearSelectionForExternalWindow(previewWindow);
        const bool retained = app.items_[0].selected;
        app.filePreview_.RetainsSelection(nullptr);
        app.ClearSelectionForExternalWindow(previewWindow);
        const bool released = !app.items_[0].selected;
        DestroyWindow(previewWindow);
        require(retained && released, "only this preview session exempts external focus from clearing selection");

        const auto resetModel = [&] {
            app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
            app.dockEntries_.clear(); app.ClearSelection();
        };
        for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
                DesktopWidgetType::FolderMapping})
        {
            resetModel();
            SeedMemberModel(app, {first, second}, type, false);
            for (auto& item : app.items_) item.selected = item.parsingName == second.wstring();
            for (auto& entry : app.widgets_[0].folderEntries) entry.selected = entry.fullPath == second.wstring();
            require(app.GetSelectedFilePreviewPath() == second.wstring(),
                "native collection, desktop-files and folder-mapping views resolve their selected member");
            for (auto& container : app.containers_)
                if (auto* search = dynamic_cast<ScrollingItemWidget*>(container.get()))
                {
                    search->SetSearchFocused(true);
                    require(!app.TryHandleFilePreviewKey(VK_SPACE), "native widget search retains its Space input");
                    search->SetSearchFocused(false);
                }
        }
        for (const auto child : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories})
        {
            resetModel();
            SeedGroupedMembers(app, {first, second}, false, child);
            for (auto& item : app.items_) item.selected = item.parsingName == second.wstring();
            require(app.GetSelectedFilePreviewPath() == second.wstring(),
                "native collection groups and file groups use their visible member projection");
        }
        resetModel();
        PopulateModel(app, {first, second}, 0); app.LayoutItems();
        app.dockFolderPopupOpen_ = true;
        app.dockFolderPopupWidget_.id = L"preview-popup";
        app.dockFolderPopupWidget_.type = DesktopWidgetType::FolderMapping;
        FolderEntry entry; entry.name = second.filename().wstring(); entry.fullPath = second.wstring(); entry.selected = true;
        app.dockFolderPopupWidget_.folderEntries.push_back(std::move(entry));
        app.popupRect_ = {0, 0, 400, 300};
        app.popupAnimation_.ShowImmediately();
        require(app.GetSelectedFilePreviewPath() == second.wstring(), "the active Dock folder popup wins over stale desktop selection");
        app.dockFolderPopupWidget_.folderEntries[0].selected = false;
        require(app.GetSelectedFilePreviewPath().empty(), "an empty active popup never falls through to files behind it");
        app.dockFolderPopupOpen_ = false;
        app.dockFolderPopupWidget_ = {};
        app.popupAnimation_.ResetHidden();

        {
            std::lock_guard lock(capture->mutex);
            capture->available.providers = {{preview::Kind::Peek, {'P', MOD_ALT}, L"isolated-peek"}};
        }
        app.filePreview_.Refresh();
        await([&] { return app.filePreview_.Matches({'P', MOD_ALT}); }, "follow external provider key changes");
        app.items_[0].selected = true; app.items_[1].selected = false;
        previewTestModifiers = MOD_ALT;
        const auto beforeAlt = count();
        app.HandleInputMessage(completionWindow, WM_SYSKEYDOWN, 'P', LPARAM{1} << 29);
        app.HandleInputMessage(completionWindow, WM_SYSCHAR, L'p', LPARAM{1} << 29);
        await([&] { return count() == beforeAlt + 1; }, "custom Alt binding dispatches through the actual system-key route");
        require(app.typeToSelectSuppressChar_, "WM_SYSCHAR cannot replay an already handled preview shortcut");
        previewTestModifiers = 0;
        require(!app.TryHandleFilePreviewKey(VK_SPACE), "the old shortcut no longer belongs to Peek");
        require(NoPresentationWindows(app), "preview integration never initialized the desktop host or a visible preview window");
        std::cout << "Native file preview input, visible selection, text guards and focus lease passed.\n";
    }
