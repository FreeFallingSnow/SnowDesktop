    // Real RemoveDockMappingAt -> rebuild -> Dock layout/render/scheduler.
    // Only the input model, clock samples and WARP device are fixture-owned.
    static void CheckDockEntryRemoval(DesktopApp& app, bool discardRetention = false)
    {
        std::string scenario = "timeline";
        const auto check = [&](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(std::string("Dock removal [") + scenario + "]: " + message);
        };
        snowdesktop::tests::dock_entry_removal::Run(check);
        snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
            snowdesktop::animation::Scale, 1, 0, false, false, 0);
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create isolated WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)),
            "create real renderer resources without a presentation window");
        app.generalSettings_.dockEnabled = true;
        app.desktopItemsReady_ = true;
        app.dockSettings_.hoverEffect = 0;
        app.dockSettings_.showRunningApps = false;
        app.dockSettings_.showWindowsButton = app.dockSettings_.showQuickSearchButton = false;
        app.iconBeautifySettings_.enabled = false;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        app.gridPages_.front().bounds = app.gridPages_.front().workArea =
            app.gridPages_.front().visualWorkArea = {0, 0, 800, 600};
        app.dockAreas_ = {{0, 0, 800, 600}};
        check(app.uiAnimationScheduler_.Initialize(), "initialize production scheduler");
        ComPtr<ID2D1Bitmap1> target, readback;
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, properties, &target)),
            "allocate render target");
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, properties, &readback)),
            "allocate pixel readback");
        const auto redPixels = [&](DockContainer& dock, bool anyColor = false) {
            app.d2dContext_->SetTarget(target.Get());
            app.d2dContext_->BeginDraw();
            app.d2dContext_->Clear(D2D1::ColorF(0, 0, 0, 0));
            dock.DrawContents(app.d2dContext_.Get());
            check(SUCCEEDED(app.d2dContext_->EndDraw()), "render actual Dock contents");
            app.d2dContext_->SetTarget(nullptr);
            check(SUCCEEDED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)), "copy actual pixels");
            D2D1_MAPPED_RECT mapped{};
            check(SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "read actual pixels");
            size_t red = 0;
            for (size_t y = 0; y < 600; ++y)
                for (size_t x = 0; x < 800; ++x)
                {
                    const auto* pixel = mapped.bits + y * mapped.pitch + x * 4;
                    if (anyColor ? pixel[3] > 0 :
                        pixel[2] > 40 && pixel[2] > pixel[1] * 2 && pixel[2] > pixel[0] * 2) ++red;
                }
            readback->Unmap();
            return red;
        };
        const auto shortcut = [&](const std::filesystem::path& path, const std::filesystem::path& destination) {
            ComPtr<IShellLinkW> link;
            ComPtr<IPersistFile> persisted;
            check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
                SUCCEEDED(link->SetPath(destination.c_str())) && SUCCEEDED(link.As(&persisted)) &&
                SUCCEEDED(persisted->Save(path.c_str(), TRUE)), "create an actual private folder shortcut");
        };
        for (const bool edge : {false, true})
        for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
        for (const int sourceKind : {0, 1, 2}) // file, directory, directory shortcut
        for (const int removalKind : {0, 1, 2, 3}) // map menu, Delete, fixed drag, mapping drag
        {
            scenario = "edge=" + std::to_string(edge) + " position=" + std::to_string(static_cast<int>(position)) +
                " source=" + std::to_string(sourceKind) + " operation=" + std::to_string(removalKind);
            const bool returnToDesktop = removalKind == 1;
            const bool dragging = removalKind >= 2;
            const bool mapping = removalKind == 0 || removalKind == 3;
            app.dockSettings_.position = position;
            app.dockSettings_.edgeAttached = edge;
            app.d2dIconCache_.clear();
            app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.dockEntries_.clear();
            for (int i = 0; i < 3; ++i)
            {
                const auto path = app.initializationExperimentDirectory_ /
                    (std::wstring(L"pin-") + std::to_wstring(i) +
                        (sourceKind == 1 ? L"-folder" : sourceKind == 2 ? L".lnk" : L".txt"));
                if (sourceKind == 1) std::filesystem::create_directories(path);
                else if (sourceKind == 2)
                {
                    const auto destination = app.initializationExperimentDirectory_ / L"shortcut-target";
                    std::filesystem::create_directories(destination);
                    shortcut(path, destination);
                }
                else { std::ofstream file(path); file << "retained source"; }
                auto item = FileItem(path);
                item.isShortcut = sourceKind == 2;
                DeleteObject(item.iconBitmap);
                const std::vector<std::uint32_t> pixels(64 * 64, i == 1 ? 0xffff0000u : 0xff0000ffu);
                item.iconBitmap = CreateBitmap(64, 64, 1, 32, pixels.data());
                item.iconBitmapSize = {64, 64};
                check(item.iconBitmap != nullptr, "create distinctive source artwork");
                app.dockEntries_.push_back({DockEntryType::DesktopItem, item.layoutKey, mapping});
                item.gridCell = {mapping ? L"__page:1" : std::wstring(kDockPageId), i, 0};
                app.items_.push_back(std::move(item));
            }
            app.RefreshDesktopItemIndexCache();
            for (const auto& entry : app.dockEntries_) (void)app.ResolveDockFolderTarget(entry, nullptr, true);
            app.RebuildContainersAndItems();
            auto* dock = app.GetDockContainer();
            check(dock != nullptr, "create actual Dock model");
            const RECT original = dock->GetBounds();
            const auto before = redPixels(*dock);
            check(before > 100, "source has visible red artwork");
            const auto source = app.items_[1].parsingName;
            if (dragging)
            {
                Item* item = dock->GetSlots()[1]->GetItem();
                const POINT point{10, 10};
                app.dragSession_.Begin(dock, {item}, app.BuildDragSourceList({item}, dock), point, point);
                app.dragSession_.DeactivateForDrop();
                if (mapping) check(app.RemoveDockDragOutItems({item}), "execute actual mapping drag-out commit");
                else app.MoveDockItemsToDesktop({item}, {L"__page:1", 0, 0});
                app.EndDragSession();
            }
            else if (returnToDesktop)
            {
                dock->GetSlots()[1]->GetItem()->SetSelected(true);
                check(app.OnKeyDown(VK_DELETE), "execute actual fixed-pin Delete-key route");
            }
            else check(app.RemoveDockMappingAt(1), "execute actual mapping-removal route");
            dock = app.GetDockContainer();
            check(dock && app.dockEntries_.size() == 2 && std::filesystem::exists(source) && app.items_.size() == 3,
                "commit only the Dock reference and preserve its source file/model");
            if (dragging)
            {
                check(!dock->IsEntryRemovalAnimating() && dock->CapturePresentationState().removals->Entries().empty() &&
                    redPixels(*dock) == 0,
                    "fixed and mapping drag-out must immediately drop artwork/slot without a deferred exit animation");
                app.CancelUiAnimationFrame();
                continue;
            }
            if (discardRetention)
            {
                // Restore the original missing-retention behavior at the real
                // rebuild handoff; the same user route and assertions run.
                auto lost = dock->CapturePresentationState();
                lost.removals = std::make_shared<DockContainer::EntryRemovalTimeline>();
                check(dock->RestorePresentationState(lost), "apply missing-retention negative control");
                dock->InvalidateSlots();
                std::cout << "Dock removal negative: discard outgoing snapshot at runtime-tree handoff\n";
            }
            check(dock->IsEntryRemovalAnimating(), "actual removal route must retain the outgoing presentation");
            const RECT initial = dock->GetBounds();
            const auto initialPixels = redPixels(*dock);
            if (!EqualRect(&initial, &original) || initialPixels <= 100)
                std::cout << "First frame original=" << original.left << ',' << original.top << ',' << original.right << ',' << original.bottom
                    << " actual=" << initial.left << ',' << initial.top << ',' << initial.right << ',' << initial.bottom
                    << " redPixels=" << initialPixels << " before=" << before << '\n';
            check(EqualRect(&initial, &original), "first removal frame preserves the original panel geometry");
            check(initialPixels > 100, "first removal frame preserves outgoing artwork across runtime-tree replacement");
            check(app.dockRunningAnimationFrameToken_ != 0,
                "the production removal route must arm its scheduler without pointer re-entry or test assistance");
            const auto state = dock->CapturePresentationState();
            const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
            check(dock->AdvanceEntryRemovalAnimation(now + 100.0), "exit remains active midway");
            dock->InvalidateSlots();
            const auto midway = redPixels(*dock);
            check(midway > 0 && midway < before, "departing snapshot visibly shrinks and fades");
            check(!dock->AdvanceEntryRemovalAnimation(now + 1000.0), "exit completes on the shared timeline");
            dock->InvalidateSlots();
            check(redPixels(*dock) == 0 && state.removals->Entries().empty(),
                "final frame removes all departed pixels and releases the detached snapshot");
            const RECT final = dock->GetBounds();
            if (!edge)
                check(position == DockPosition::Left || position == DockPosition::Right
                    ? final.bottom - final.top < original.bottom - original.top
                    : final.right - final.left < original.right - original.left,
                    "island panel contracts along its configured axis");
            app.CancelUiAnimationFrame();
        }
        for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
            DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
        {
            scenario = "widget=" + std::to_string(static_cast<int>(type));
            app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.widgets_.clear(); app.dockEntries_.clear();
            DesktopWidget widget;
            widget.id = L"outgoing-native-widget"; widget.type = type;
            widget.title = L"Outgoing widget"; widget.gridCell = {kDockPageId, 0, 0};
            widget.autoCollect = false;
            widget.sourceFolderPath = app.initializationExperimentDirectory_.wstring();
            app.widgets_.push_back(std::move(widget));
            app.dockEntries_.push_back({DockEntryTypeForWidget(type), app.widgets_.front().id});
            app.RebuildContainersAndItems();
            auto* dock = app.GetDockContainer();
            check(dock && redPixels(*dock, true) > 0, "native widget entrance renders before removal");
            // Supply the same committed model deletion as the menu. The
            // capture/rebuild/automatic scheduling/rendering path stays real.
            dock->PrepareEntryRemoval(0);
            app.widgets_.clear(); app.dockEntries_.clear();
            app.RebuildContainersAndItems();
            dock = app.GetDockContainer();
            check(dock && dock->IsEntryRemovalAnimating() && app.dockRunningAnimationFrameToken_ != 0 &&
                redPixels(*dock, true) > 0, "widget deletion must retain artwork and self-arm without pointer movement");
            dock->AdvanceEntryRemovalAnimation(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() + 1000.0);
            dock->InvalidateSlots();
            check(redPixels(*dock, true) == 0, "deleted widget artwork retires after its shared exit timeline");
            app.CancelUiAnimationFrame();
        }
        // Keep real Shell queuing/completion and missing-pin confirmation.
        // An empty item enumeration is supplied only after physical deletion.
        scenario = "Shell success and failure";
        app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.dockEntries_.clear();
        app.dockSettings_.position = DockPosition::Bottom;
        app.dockSettings_.edgeAttached = false;
        const auto deleted = app.initializationExperimentDirectory_ / L"deleted-fixed.txt";
        { std::ofstream file(deleted); file << "private deletion fixture"; }
        app.items_.push_back(FileItem(deleted));
        app.dockEntries_.push_back({DockEntryType::DesktopItem, app.items_.back().layoutKey, false});
        app.RefreshDesktopItemIndexCache();
        (void)app.ResolveDockFolderTarget(app.dockEntries_.front(), nullptr, true);
        app.RebuildContainersAndItems();
        const auto deleteFile = [&] {
            bool done = false, succeeded = false;
            std::vector<snowdesktop::ShellFileOperationStep> steps{{FO_DELETE, {deleted.wstring()}, {},
                static_cast<FILEOP_FLAGS>(FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI)}};
            check(app.QueueShellFileOperation(std::move(steps), [&](bool result) {
                done = true; succeeded = result;
            }), "queue the real fixed-file deletion");
            auto* dock = app.GetDockContainer();
            check(dock && !dock->IsEntryRemovalAnimating() &&
                dock->CapturePresentationState().removals->Entries().size() == 1,
                "queued deletion retains artwork but does not animate before commit");
            const auto started = GetTickCount64();
            while (!done)
            {
                check(GetTickCount64() - started < 8000, "bounded Shell deletion completion");
                MSG message{};
                if (PeekMessageW(&message, completionWindow, kShellFileOperationCompletedMessage,
                        kShellFileOperationCompletedMessage, PM_REMOVE))
                    app.OnShellFileOperationCompleted(message.lParam);
                else MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            return succeeded;
        };
        HANDLE locked = CreateFileW(deleted.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "lock the private file against deletion");
        const bool lockedDelete = deleteFile();
        CloseHandle(locked);
        check(!lockedDelete && std::filesystem::exists(deleted) && app.dockEntries_.size() == 1 &&
            app.GetDockContainer()->CapturePresentationState().removals->Entries().empty(),
            "failed deletion keeps its source/pin and cancels its pending snapshot");
        check(deleteFile() && !std::filesystem::exists(deleted), "actual Shell deletion removes the private source");
        app.items_.clear(); app.RefreshDesktopItemIndexCache();
        app.PruneDockShellMetadata();
        const auto pruneStarted = GetTickCount64();
        while (!app.dockEntries_.empty())
        {
            check(GetTickCount64() - pruneStarted < 8000, "bounded production missing-pin confirmation");
            app.DrainBackgroundShellWork();
            if (!app.dockEntries_.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        check(app.GetDockContainer() && app.GetDockContainer()->IsEntryRemovalAnimating(),
            "confirmed deleted fixed file retains its outgoing artwork after Shell pruning and rebuild");
        app.CancelUiAnimationFrame();
        for (const int sourceKind : {0, 1, 2})
        {
            scenario = "externally deleted original=" + std::to_string(sourceKind);
            app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.dockEntries_.clear();
            app.d2dIconCache_.clear();
            const auto original = app.initializationExperimentDirectory_ /
                (sourceKind == 0 ? L"original-file.txt" : sourceKind == 1 ? L"original-folder" : L"original-folder.lnk");
            const auto destination = app.initializationExperimentDirectory_ / L"original-shortcut-target";
            if (sourceKind == 0) { std::ofstream file(original); file << "original mapped file"; }
            else if (sourceKind == 1) std::filesystem::create_directories(original);
            else { std::filesystem::create_directories(destination); shortcut(original, destination); }
            auto item = FileItem(original);
            DeleteObject(item.iconBitmap);
            const std::vector<std::uint32_t> pixels(64 * 64, 0xffff0000u);
            item.iconBitmap = CreateBitmap(64, 64, 1, 32, pixels.data()); item.iconBitmapSize = {64, 64};
            item.isShortcut = sourceKind == 2;
            app.dockEntries_.push_back({DockEntryType::DesktopItem, item.layoutKey, true});
            app.items_.push_back(std::move(item)); app.RefreshDesktopItemIndexCache();
            (void)app.ResolveDockFolderTarget(app.dockEntries_.front(), nullptr, true);
            app.RebuildContainersAndItems();
            check(sourceKind == 1 ? RemoveDirectoryW(original.c_str()) != FALSE : DeleteFileW(original.c_str()) != FALSE,
                "delete the original source outside the application's file-operation queue");
            snowdesktop::shell_refresh::Snapshot snapshot;
            snapshot.desktopComplete = true;
            app.LoadDesktopItems(&snapshot, false);
            check(app.dockEntries_.size() == 1 && !app.GetDockContainer()->IsEntryRemovalAnimating() &&
                app.GetDockContainer()->CapturePresentationState().removals->Entries().size() == 1 &&
                redPixels(*app.GetDockContainer()) > 0,
                "original-source disappearance captures artwork but waits for independent deletion confirmation");
            app.PruneDockShellMetadata();
            const auto started = GetTickCount64();
            while (!app.dockEntries_.empty())
            {
                check(GetTickCount64() - started < 8000, "bounded original-source deletion confirmation");
                app.DrainBackgroundShellWork();
                if (!app.dockEntries_.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            auto* dock = app.GetDockContainer();
            check(dock && app.dockRunningAnimationFrameToken_ != 0 && dock->IsEntryRemovalAnimating() && redPixels(*dock) > 0,
                "deleting a mapped original file/folder/shortcut self-arms an exit using artwork captured before reload");
            if (sourceKind == 2) check(std::filesystem::exists(destination), "deleting a folder shortcut preserves its target");
            app.CancelUiAnimationFrame();
        }
        app.CancelUiAnimationFrame(); app.uiAnimationScheduler_.Shutdown();
        app.containers_.clear(); app.items_oo_.clear();
        app.d2dIconCache_.clear(); app.iconGlassBackdrop_.clear(); app.iconBitmapStyles_.clear();
        app.d2dContext_.Reset(); app.d2dDevice_.Reset(); app.d2dFactory_.Reset(); app.d3dDevice_.Reset();
        std::cout << "Dock removal: 96 file/folder/shortcut mapping, Delete-key and drag-out scenes; 5 widget lifetimes; Shell failure/success; 3 external original-source deletions; automatic scheduling and snapshot retirement passed\n";
    }
