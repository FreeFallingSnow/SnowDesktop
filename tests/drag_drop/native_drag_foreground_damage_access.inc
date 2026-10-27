    // The supplied boundary is a private offscreen DComp surface backed by
    // real WARP pixels. Pointer routing, draw and commit remain production.
    static inline std::exception_ptr dragForegroundPaintError;
    static inline DesktopApp* dragForegroundPaintOwner = nullptr;
    static inline RECT dragForegroundPendingPaint{};
    static inline decltype(&::InvalidateRect) dragForegroundOriginalInvalidate = nullptr;
    static inline decltype(&::UpdateWindow) dragForegroundOriginalUpdate = nullptr;
    // A never-shown window is geometry only. Isolate its OS paint delivery,
    // retain the actual invalidation rectangle and execute production OnPaint
    // synchronously at production UpdateWindow's existing call boundary.
    static BOOL WINAPI DragForegroundInvalidate(HWND window, const RECT* dirty, BOOL erase)
    {
        if (!dragForegroundPaintOwner || window != dragForegroundPaintOwner->hwnd_)
            return dragForegroundOriginalInvalidate(window, dirty, erase);
        RECT client{}, clipped{};
        GetClientRect(window, &client);
        const RECT requested = dirty ? *dirty : client;
        if (IntersectRect(&clipped, &requested, &client))
        {
            if (IsRectEmpty(&dragForegroundPendingPaint)) dragForegroundPendingPaint = clipped;
            else UnionRect(&dragForegroundPendingPaint, &dragForegroundPendingPaint, &clipped);
        }
        return TRUE;
    }
    static BOOL WINAPI DragForegroundUpdate(HWND window)
    {
        if (!dragForegroundPaintOwner || window != dragForegroundPaintOwner->hwnd_)
            return dragForegroundOriginalUpdate(window);
        if (!IsRectEmpty(&dragForegroundPendingPaint))
        {
            const RECT dirty = dragForegroundPendingPaint;
            dragForegroundPendingPaint = {};
            ValidateRect(window, nullptr);
            try
            {
                if (!dragForegroundPaintOwner->OnPaint(&dirty) &&
                    !dragForegroundPaintOwner->compositionPaintInProgress_)
                    throw std::runtime_error("private UpdateWindow production OnPaint failed");
            }
            catch (...) { dragForegroundPaintError = std::current_exception(); }
        }
        return TRUE;
    }

    static void CheckDragForegroundDamage(DesktopApp& app, bool originalFullRequest = false)
    {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &app.d3dDevice_, nullptr, nullptr)), "create pointer foreground WARP device");
        ComPtr<IDXGIDevice> dxgi;
        check(SUCCEEDED(app.d3dDevice_.As(&dxgi)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&app.d2dFactory_))) &&
            SUCCEEDED(app.d2dFactory_->CreateDevice(dxgi.Get(), &app.d2dDevice_)) &&
            SUCCEEDED(app.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &app.d2dContext_)) &&
            SUCCEEDED(DCompositionCreateDevice2(app.d2dDevice_.Get(), IID_PPV_ARGS(&app.dcompDevice_))) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.desktopForegroundCompositionVisual_)) &&
            SUCCEEDED(app.dcompVisual_->AddVisual(app.desktopForegroundCompositionVisual_.Get(), TRUE, nullptr)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))),
            "initialize actual pointer foreground graphics");
        app.RecreateItemTextFormat();
        app.personalizationSettings_ = PersonalizationSettings::DarkPreset();
        app.personalizationSettings_.glassEnabled = false;
        app.personalizationSettings_.acrylicEnabled = false;
        app.dockSettings_.followComponentAppearance = true;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        app.startupInitializationComplete_ = true;
        app.generalSettings_.dockEnabled = false;
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        auto& page = app.gridPages_.front();
        page.bounds = page.workArea = page.visualWorkArea = {0, 0, 800, 600};
        page.columns = 8; page.rows = 6;
        app.savedPageColumns_[page.id] = page.columns;
        app.savedPageRows_[page.id] = page.rows;
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item; item.name = i ? L"B" : L"A";
            item.parsingName = item.layoutKey = item.name;
            const int row = i ? 1 : 0;
            item.gridCell = {app.gridPages_.front().id, i * 2, row};
            item.gridSpan = {1, 1}; item.selected = true;
            item.bounds = {10 + i * 200, 10 + row * 100, 90 + i * 200, 90 + row * 100};
            app.items_.push_back(std::move(item));
        }
        app.containers_.push_back(std::make_unique<DesktopGrid>(&app.gridPages_, &app.items_, &app));
        for (auto& item : app.items_)
            app.items_oo_.push_back(std::make_unique<DesktopIcon>(&item, app.containers_.front().get(), &app));
        const HWND savedWindow = app.hwnd_;
        const HWND window = CreateWindowExW(0, L"STATIC", L"R05 offscreen pointer", WS_POPUP,
            0, 0, 1200, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(window && !IsWindowVisible(window), "offscreen pointer fixture never shows a desktop host");
        struct Cleanup
        {
            DesktopApp& app; HWND saved; HWND window;
            ~Cleanup()
            {
                app.marqueeActive_ = false; app.widgetAction_ = DesktopApp::WidgetAction::None;
                app.EndDragSession();
                app.items_oo_.clear(); app.containers_.clear();
                app.ResetDesktopForegroundComposition();
                app.CancelUiAnimationFrame();
                app.uiAnimationScheduler_.Shutdown();
                app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
                app.iconGlassBackdrop_.clear(); app.iconBitmapStyles_.clear();
                app.d2dIconCache_.clear(); app.itemTextLayoutCache_.clear();
                app.itemTextEffectContext_.Reset(); app.itemTextFormat_.Reset();
                app.dcompSurface_.Reset(); app.dcompTarget_.Reset();
                app.dcompVisual_.Reset(); app.dcompDevice_.Reset();
                app.d2dContext_.Reset(); app.d2dDevice_.Reset();
                app.d2dFactory_.Reset(); app.d3dDevice_.Reset();
                app.dwriteFactory_.Reset();
                app.hwnd_ = saved; DestroyWindow(window);
            }
        } cleanup{app, savedWindow, window};
        app.hwnd_ = window;
        struct PaintBindingScope
        {
            ~PaintBindingScope()
            {
                dragForegroundPaintOwner = nullptr;
                dragForegroundPendingPaint = {};
            }
        } paintBinding;
        dragForegroundPaintOwner = &app;
        dragForegroundPendingPaint = {};
        InputSafetyImport invalidation("InvalidateRect", reinterpret_cast<ULONG_PTR>(&DragForegroundInvalidate));
        dragForegroundOriginalInvalidate = reinterpret_cast<decltype(dragForegroundOriginalInvalidate)>(invalidation.original);
        InputSafetyImport update("UpdateWindow", reinterpret_cast<ULONG_PTR>(&DragForegroundUpdate));
        dragForegroundOriginalUpdate = reinterpret_cast<decltype(dragForegroundOriginalUpdate)>(update.original);
        check(SUCCEEDED(app.dcompDevice_->CreateTargetForHwnd(window, FALSE, &app.dcompTarget_)) &&
            SUCCEEDED(app.dcompTarget_->SetRoot(app.dcompVisual_.Get())),
            "bind real composition commits to the never-shown fixture-owned target");
        check(app.uiAnimationScheduler_.Initialize(), "initialize the production commit timing boundary");
        dragForegroundPaintError = {};
        const RECT client{0, 0, 1200, 900};
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const auto readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> background, target, reference, readable;
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0, properties, &background)) &&
            SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0, properties, &target)) &&
            SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0, readProperties, &readable)),
            "allocate independent retained foreground and background pixels");
        ComPtr<ForegroundSurface> root, foreground;
        root.Attach(new ForegroundSurface(app.d2dContext_.Get(), background.Get()));
        foreground.Attach(new ForegroundSurface(app.d2dContext_.Get(), target.Get()));
        app.dcompSurface_ = root;
        app.compositionWidth_ = 1200; app.compositionHeight_ = 900;
        app.desktopForegroundCompositionSurface_ = foreground;
        app.desktopForegroundCompositionWidth_ = 1200; app.desktopForegroundCompositionHeight_ = 900;
        const auto pixels = [&](ID2D1Bitmap1* bitmap) {
            check(SUCCEEDED(readable->CopyFromBitmap(nullptr, bitmap, nullptr)), "copy retained pointer pixels");
            D2D1_MAPPED_RECT mapped{};
            check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "map retained pointer pixels");
            std::vector<unsigned char> value(1200 * 900 * 4);
            for (unsigned y = 0; y < 900; ++y)
                std::memcpy(value.data() + y * 1200 * 4, mapped.bits + y * mapped.pitch, 1200 * 4);
            readable->Unmap(); return value;
        };
        const auto frame = [&] {
            foreground->updates.clear(); root->updates.clear();
            app.shellHoverTraceActive_ = true; app.shellHoverTraceCount_ = 0; app.shellHoverTraceWriteIndex_ = 0;
            app.PresentPointerInteractionFrame(true);
            check(app.FlushPendingCompositionCommit(), "flush the existing end-of-message composition boundary");
            check(!app.graphicsDeviceRecovery_.Pending() && !app.compositionRenderRecoveryPending_,
                "a pointer frame must not hide graphics recovery behind a recorded commit event");
            if (dragForegroundPaintError) std::rethrow_exception(dragForegroundPaintError);
        };
        const auto commitCount = [&] {
            size_t count = 0;
            for (size_t i = 0; i < app.shellHoverTraceCount_; ++i)
                if (app.shellHoverTrace_[i].event == DesktopApp::ShellHoverTraceEvent::CommitFlushed) ++count;
            return count;
        };
        size_t comparedFrames = 0;
        const auto compare = [&](bool expectLocal) {
            check(!foreground->updates.empty(), "changed pointer feedback reaches actual foreground BeginDraw");
            check(commitCount() == 1, "changed visible pointer feedback must submit one synchronous composition commit");
            if (originalFullRequest && expectLocal)
            {
                // Restore the exact old full-client presentation boundary,
                // then apply the same area assertion used for the candidate.
                foreground->updates.clear();
                check(app.PresentDesktopForegroundComposition(client), "execute original full-client foreground request");
                std::cout << "R05 negative control executed original full-client foreground presentation\n";
            }
            std::cout << "R05 foreground frame " << ++comparedFrames << " requests:";
            for (const auto& rect : foreground->updates)
                std::cout << " [" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << ']';
            std::cout << "; background draws=" << root->updates.size() << "; commits=" << commitCount() << '\n';
            if (expectLocal)
                check(std::all_of(foreground->updates.begin(), foreground->updates.end(), [&](const RECT& rect) {
                    return !EqualRect(&rect, &client);
                }), "R05 changed pointer feedback still requests the entire client");
            const auto actual = pixels(target.Get());
            // Independent full raster oracle: a separate target, no partial
            // region, no modification of the candidate surface/paint validity.
            ForegroundSurface referenceSurface(app.d2dContext_.Get(), reference.Get());
            POINT offset{}; ID2D1DeviceContext* raw = nullptr;
            check(SUCCEEDED(referenceSurface.BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                reinterpret_cast<void**>(&raw), &offset)), "begin independent complete reference target");
            ComPtr<ID2D1DeviceContext> context; context.Attach(raw);
            context->Clear(D2D1::ColorF(0, 0, 0, 0));
            app.DrawDesktopForeground(context.Get(), app.desktopIconsHidden_);
            check(SUCCEEDED(referenceSurface.EndDraw()), "finish independent complete current reference");
            check(actual == pixels(reference.Get()), "retained local pointer pixels differ from the complete current reference");
        };
        // Use real DesktopIcon providers and fixed pixels; no source Item::Draw
        // or title layout is replaced. Begin() supplies the established gesture.
        for (auto& item : app.items_)
        {
            const std::vector<unsigned> iconPixels(32 * 32, 0xff39ad72);
            item.iconBitmap = CreateBitmap(32, 32, 1, 32, iconPixels.data());
            item.iconBitmapSize = {32, 32}; item.iconState = IconState::FullQuality;
        }
        app.RefreshDesktopItemIndexCache();
        check(std::all_of(app.items_oo_.begin(), app.items_oo_.end(), [](const auto& item) {
            return !item->GetTitle().empty() && item->GetIconBitmap() != nullptr;
        }), "real desktop source providers resolve the populated title and icon inputs");
        check(app.OnPaint(nullptr) && app.FlushPendingCompositionCommit(),
            "seed the actual complete pre-drag desktop reference");
        const std::array<RECT, 2> sourceCells{app.items_[0].bounds, app.items_[1].bounds};
        const auto hasSourcePixels = [](const std::vector<unsigned char>& data, const RECT& cell) {
            for (LONG y = cell.top; y < cell.bottom; ++y)
                for (LONG x = cell.left; x < cell.right; ++x)
                    if (data[(static_cast<size_t>(y) * 1200 + static_cast<size_t>(x)) * 4 + 3]) return true;
            return false;
        };
        const auto preDragBackground = pixels(background.Get());
        check(hasSourcePixels(preDragBackground, sourceCells[0]) && hasSourcePixels(preDragBackground, sourceCells[1]),
            "pre-drag reference must contain actual source-cell pixels on both rows");
        Begin(app, false, 2);
        app.dragSession_.UpdateActionFromMods(0);
        const auto move = [&](POINT point) {
            app.lastMousePoint_ = point;
            app.ResolveCurrentDragTargetAt(point);
        };
        move({150, 150}); frame();
        std::cout << "R05 initial layer migration root draws=" << root->updates.size()
            << "; foreground draws=" << foreground->updates.size() << "; commits=" << commitCount() << '\n';
        check(!root->updates.empty() && std::any_of(root->updates.begin(), root->updates.end(),
            [&](const RECT& dirty) { return EqualRect(&dirty, &client) != FALSE; }),
            "first item layer migration keeps the complete background path");
        const auto migratedBackground = pixels(background.Get());
        check(!hasSourcePixels(migratedBackground, sourceCells[0]) && !hasSourcePixels(migratedBackground, sourceCells[1]),
            "first item migration must erase every original source-cell pixel from the background");
        compare(false);
        for (POINT point : {POINT{250, 150}, POINT{150, 250}, POINT{50, 150}})
        {
            move(point);
            Container* const owner = app.dragSession_.TargetContainer();
            Slot* const slot = app.dragSession_.TargetSlot();
            const auto region = app.dragSession_.TargetRegion();
            frame();
            check(root->updates.empty() && commitCount() == 1,
                "subsequent item feedback stays synchronous with one foreground commit");
            check(app.dragSession_.TargetContainer() == owner && app.dragSession_.TargetSlot() == slot &&
                app.dragSession_.TargetRegion() == region && app.dragSession_.Items().size() == 2,
                "presenting local feedback preserves actual target, hit region and multi-source state");
            check(EqualRect(&sourceCells[0], &app.items_[0].bounds) &&
                EqualRect(&sourceCells[1], &app.items_[1].bounds),
                "pointer preview cannot commit or mutate the original source layout");
            compare(true);
        }
        frame();
        check(foreground->updates.empty() && root->updates.empty() && commitCount() == 0,
            "unchanged logical item feedback keeps the original zero-present path");
        app.EndDragSession(); ValidateRect(window, nullptr);
        dragForegroundPendingPaint = {};
        // Unknown/empty retained component context keeps the complete path.
        // Real payload acceptance/commit semantics stay in the existing native
        // source matrices; no file transfer is claimed by this visual fixture.
        DesktopWidget componentSource;
        componentSource.id = L"r05-component-source";
        componentSource.type = DesktopWidgetType::Collection;
        componentSource.bounds = {600, 500, 850, 750};
        auto sourceObject = CreateWidget(&componentSource, &app);
        auto* sourceContainer = dynamic_cast<WidgetContainer*>(sourceObject.get());
        check(sourceContainer != nullptr, "construct actual component source container");
        app.dragSession_.Begin(sourceContainer, {}, {}, {50, 50}, {50, 50});
        app.dragSession_.UpdateTarget(app.containers_.front().get(),
            app.containers_.front()->GetSlots().front().get(), HitRegion::Empty);
        frame(); compare(false);
        app.dragSession_.UpdatePoint({150, 150}); frame();
        check(!root->updates.empty(), "component-source drag retains the complete background presentation path");
        compare(false);
        app.EndDragSession(); ValidateRect(window, nullptr);
        dragForegroundPendingPaint = {};
        app.marqueeActive_ = false; app.marqueeFullPresentPending_ = false;
        app.marqueeAnchorPoint_ = {305, 270};
        // Supply an actual native single-cell entrance. This fixture observes
        // its foreground selection pixels; retained child surfaces have their
        // own production handoff test and remain outside this pixel oracle.
        app.customDesktopVisible_ = false;
        app.widgets_.reserve(3);
        DesktopWidget entrance;
        entrance.id = L"r05-native-entrance"; entrance.type = DesktopWidgetType::Collection;
        entrance.singleCellIconMode = true; entrance.keepWhenDesktopHidden = true;
        entrance.gridCell = {app.gridPages_.front().id, 1, 1}; entrance.gridSpan = {1, 1};
        entrance.bounds = GetGridRect(app.gridPages_, entrance.gridCell, entrance.gridSpan);
        entrance.title = L"Native component entrance\nwith a long selected title\nthat continues below\nits original grid cell";
        app.widgets_.push_back(std::move(entrance));
        // A legal filename length still expands far below its grid cell.
        app.items_[0].name = std::wstring(200, L'W') + L".lnk";
        check(app.OnPaint(nullptr) && app.FlushPendingCompositionCommit(),
            "seed the real expanded-title background before marquee layer migration");
        const auto preMarqueeBackground = pixels(background.Get());
        bool expandedBackgroundTitle = false;
        for (size_t y = 274; y < 900 && !expandedBackgroundTitle; ++y)
            for (size_t x = 0; x < 100; ++x)
                if (preMarqueeBackground[(y * 1200 + x) * 4 + 3])
                { expandedBackgroundTitle = true; break; }
        check(expandedBackgroundTitle,
            "pre-marquee reference must contain real expanded title pixels beyond the future marquee");
        app.marqueeActive_ = true; app.marqueeFullPresentPending_ = true;
        for (POINT point : {POINT{5, 5}, POINT{205, 5}, POINT{305, 160}, POINT{5, 260}})
        {
            app.UpdateMarqueeSelection(point);
            std::vector<bool> selection;
            for (const auto& item : app.items_) selection.push_back(item.selected);
            const bool first = app.marqueeFullPresentPending_;
            frame();
            check(!app.marqueeFullPresentPending_, "first marquee frame completes the original layer migration");
            check(first ? std::any_of(root->updates.begin(), root->updates.end(),
                [&](const RECT& dirty) { return EqualRect(&dirty, &client) != FALSE; }) : root->updates.empty(),
                "only the first marquee frame repaints the background");
            if (first)
            {
                const auto selectedPixels = pixels(target.Get());
                bool titleOutsideMarquee = false;
                for (LONG y = app.marqueeRect_.bottom + 4; y < 900 && !titleOutsideMarquee; ++y)
                    for (LONG x = 0; x < 100; ++x)
                        if (selectedPixels[(static_cast<size_t>(y) * 1200 + x) * 4 + 3])
                        { titleOutsideMarquee = true; break; }
                check(titleOutsideMarquee,
                    "long-title input must produce real selected pixels beyond the marquee rectangle");
                const auto migratedTitleBackground = pixels(background.Get());
                bool oldTitleRemains = false;
                for (size_t y = 274; y < 900 && !oldTitleRemains; ++y)
                    for (size_t x = 0; x < 100; ++x)
                        if (migratedTitleBackground[(y * 1200 + x) * 4 + 3])
                        { oldTitleRemains = true; break; }
                check(!oldTitleRemains,
                    "first marquee migration must remove the expanded title from the background layer");
            }
            for (size_t i = 0; i < selection.size(); ++i)
                check(app.items_[i].selected == selection[i], "local presentation changes selection state");
            compare(!first);
        }
        app.desktopIconsHidden_ = true;
        app.marqueeAnchorPoint_ = {105, 105};
        app.UpdateMarqueeSelection({195, 195});
        check(app.widgets_.front().selected, "hidden desktop selection retains the native entrance contract");
        frame(); compare(false);
        const auto entrancePixels = pixels(target.Get());
        bool entranceTitleVisible = false;
        for (size_t y = 220; y < 650 && !entranceTitleVisible; ++y)
            for (size_t x = 110; x < 190; ++x)
                if (entrancePixels[(y * 1200 + x) * 4 + 3])
                { entranceTitleVisible = true; break; }
        check(entranceTitleVisible,
            "native entrance selected title must produce actual pixels beyond the hidden marquee rectangle");
        app.desktopIconsHidden_ = false;
        app.marqueeActive_ = false;
        // Keep native-preview data addresses stable through fixture teardown.
        app.widgets_.front().selected = false;
        app.widgets_.front().bounds = {};
        app.widgets_.front().gridCell.pageId.clear();
        app.customDesktopVisible_ = true;
        DesktopWidget source;
        source.id = L"r05-move-source"; source.type = DesktopWidgetType::Collection;
        source.gridCell = {app.gridPages_.front().id, 0, 0}; source.gridSpan = {1, 1};
        source.bounds = GetGridRect(app.gridPages_, source.gridCell, source.gridSpan);
        app.widgets_.push_back(std::move(source));
        app.savedPageIds_ = {app.gridPages_.front().id, L"r05-other-page"};
        app.savedPageNames_[L"r05-other-page"] = L"R05 retained navigation page";
        check(app.MaxPageOffset() == 1, "navigation transition fixture retains an actual second page");
        app.mouseDownWidgetIndex_ = 1;
        app.widgetAction_ = DesktopApp::WidgetAction::Move;
        app.widgetPreviewSpan_ = {1, 1};
        app.widgetPreviewCell_ = {app.gridPages_.front().id, 0, 1};
        frame(); compare(false);
        for (const bool hidden : {false, true})
        {
            app.desktopIconsHidden_ = hidden;
            app.widgetPreviewCell_.column = hidden ? 2 : 1;
            const RECT originalBounds = app.widgets_[1].bounds;
            frame(); compare(!hidden);
            check(EqualRect(&originalBounds, &app.widgets_[1].bounds),
                "foreground feedback moves the preview without committing widget geometry");
        }
        app.widgetPreviewCell_.column = 3;
        frame(); compare(true);
        app.desktopIconsHidden_ = false;
        DesktopWidget group;
        group.id = L"r05-group-target"; group.type = DesktopWidgetType::CollectionGroup;
        group.bounds = {500, 120, 760, 350};
        app.widgets_.push_back(std::move(group));
        app.widgetCollectionGroupTargetIndex_ = 2;
        frame(); compare(false);
        app.widgetCollectionGroupTargetIndex_ = static_cast<size_t>(-1);
        frame(); compare(true);
        app.widgets_[2].type = DesktopWidgetType::Collection;
        app.widgetPairTargetIndex_ = 2;
        app.widgetPairAction_ = snowdesktop::widget_pair_drop::Action::CreateCollectionGroup;
        frame(); compare(true);
        app.widgetPairTargetIndex_ = static_cast<size_t>(-1);
        app.widgetPairAction_ = snowdesktop::widget_pair_drop::Action::None;
        frame(); compare(true);
        app.widgetAction_ = DesktopApp::WidgetAction::Resize;
        app.widgetPreviewCell_ = {app.gridPages_.front().id, 0, 0};
        app.widgetPreviewSpan_ = {2, 2};
        frame(); compare(false);
        frame();
        check(foreground->updates.empty() && root->updates.empty() && commitCount() == 0,
            "unchanged logical widget feedback keeps the original zero-present path");
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.hoverEffect = 0;
        app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.showWindowsButton = false;
        auto dock = std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{500, 500, 850, 620});
        app.widgetDockTargetContainer_ = dock.get();
        app.containers_.push_back(std::move(dock));
        app.widgetDockTarget_ = true; app.widgetDockInsertIndex_ = 0;
        app.widgetAction_ = DesktopApp::WidgetAction::Move;
        frame(); compare(false);
        const auto dockInsertionPixels = pixels(target.Get());
        app.widgetDockTarget_ = false; app.widgetDockTargetContainer_ = nullptr;
        frame(); compare(true);
        const auto withoutDockInsertion = pixels(target.Get());
        bool dockInsertionChanged = false;
        for (size_t y = 500; y < 620 && !dockInsertionChanged; ++y)
            for (size_t x = 500; x < 850; ++x)
            {
                const size_t offset = (y * 1200 + x) * 4;
                if (std::memcmp(dockInsertionPixels.data() + offset, withoutDockInsertion.data() + offset, 4) != 0)
                { dockInsertionChanged = true; break; }
            }
        check(dockInsertionChanged, "Dock target input must produce and erase a real insertion indicator");
        check(app.RenderDesktopForegroundComposition(nullptr, false) &&
            app.presentedPointerInteractionKind_ == DesktopApp::PointerInteractionKind::None,
            "an actual non-pointer foreground transaction invalidates the old interaction footprint");
        frame();
        check(foreground->updates.empty() && root->updates.empty() && commitCount() == 0 &&
            app.presentedPointerInteractionKind_ == DesktopApp::PointerInteractionKind::None,
            "unchanged feedback cannot invent a newly painted reliable footprint after invalidation");
        app.widgetPreviewCell_.column = 1;
        frame();
        check(foreground->updates.size() == 1 && EqualRect(&foreground->updates.front(), &client),
            "the first pointer update after another transaction retains a complete recovery frame");
        compare(false);
        app.widgetPreviewCell_.column = 2;
        frame(); compare(true);
        app.desktopForegroundCompositionSurface_.Reset();
        check(SUCCEEDED(app.CreateOrResizeDesktopForegroundCompositionSurface()) &&
            app.presentedPointerInteractionKind_ == DesktopApp::PointerInteractionKind::None &&
            app.desktopForegroundCompositionSurface_.Get() != foreground.Get(),
            "actual same-size foreground allocation invalidates the pointer footprint");
        check(app.RenderDesktopForegroundComposition(nullptr, false),
            "actual replacement surface can receive a complete non-pointer repaint");
        ComPtr<ID2D1Bitmap1> recovered;
        check(SUCCEEDED(app.d2dContext_->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0,
            properties, &recovered)), "supply a fresh observable retained target after real replacement");
        target = recovered;
        foreground.Attach(new ForegroundSurface(app.d2dContext_.Get(), target.Get()));
        app.desktopForegroundCompositionSurface_ = foreground;
        app.widgetPreviewCell_.column = 0;
        frame();
        check(foreground->updates.size() == 1 && EqualRect(&foreground->updates.front(), &client),
            "old envelopes from a replaced same-size surface force one complete pointer update");
        compare(false);
        app.widgetPreviewCell_.column = 1;
        frame(); compare(true);
        app.widgetAction_ = DesktopApp::WidgetAction::None;
        app.InvalidateDragStaticScene();
        check(app.presentedPointerInteractionKind_ == DesktopApp::PointerInteractionKind::None,
            "global static invalidation discards any unreliable old pointer envelope");
        check(!app.dragPreviewHwnd_ && !app.inputHwnd_ && !app.controlHwnd_ && !app.hintHwnd_,
            "foreground checks never initialize desktop input, control, hint or drag preview windows");
    }
