    static void InitListTitleRendering(DesktopApp& app, ID2D1Device* device, ID2D1DeviceContext* context)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))))
            throw std::runtime_error("create actual DirectWrite factory for list rendering");
        app.d2dDevice_ = device; app.d2dContext_ = context;
        app.listItemFontSizeCu_ = 16;
        app.RecreateComponentListTextFormat();
        if (!app.uiAnimationScheduler_.Initialize()) throw std::runtime_error("initialize actual native animation scheduler");
        app.gridPages_[0].bounds = app.gridPages_[0].workArea = app.gridPages_[0].visualWorkArea = {0, 0, 1200, 900};
    }
    static ScrollingItemWidget& SeedListTitleScene(DesktopApp& app, int columns, bool grouped, bool popup)
    {
        app.ClearListTitleHover();
        app.containers_.clear(); app.items_oo_.clear(); app.items_.clear(); app.widgets_.clear();
        DesktopWidget source;
        source.id = L"hover-source"; source.type = DesktopWidgetType::FolderMapping;
        source.cellScale = 2; source.bounds = {30, 30, 1100, 850};
        source.gridCell.pageId = app.gridPages_[0].id;
        source.gridSpan = {4, 6};
        source.listMode = true; source.listColumns = columns;
        source.showSearchBox = source.showFileCategories = false;
        source.dateHeaders = source.privacyMode = false;
        source.detailShowModified = source.detailShowType = source.detailShowSize = false;
        for (int i = 0; i < 12; ++i)
        {
            FolderEntry entry;
            entry.name = L"Title-" + std::to_wstring(i) + L"-" + std::wstring(100, L'W') + L".txt";
            entry.fullPath = L"C:\\Isolated-List-Fixture\\" + entry.name;
            source.folderEntries.push_back(std::move(entry));
        }
        app.widgets_.reserve(2);
        app.widgets_.push_back(std::move(source));
        if (grouped)
        {
            auto group = app.widgets_[0];
            group.id = L"hover-group"; group.type = DesktopWidgetType::FileGroup;
            group.folderEntries.clear(); group.childWidgetIds = {L"hover-source"};
            group.activeCategoryId = L"hover-source";
            app.widgets_.push_back(std::move(group));
            app.containers_.push_back(std::make_unique<FileGroup>(&app.widgets_[1], &app));
        }
        else app.containers_.push_back(std::make_unique<FolderMapping>(&app.widgets_[0], &app));
        app.popupRect_ = {30, 30, 1100, 850};
        app.popupWidgetIndex_ = popup ? app.widgets_.size() - 1 : static_cast<size_t>(-1);
        app.popupAnimation_.ShowImmediately();
        auto& renderer = *static_cast<ScrollingItemWidget*>(app.containers_.back().get());
        if (popup)
        {
            const RECT frame = app.GetCategorizedPopupFrame(app.popupRect_);
            renderer.SetPopupFrame(&frame);
        }
        return renderer;
    }
    static void PaintListTitles(DesktopApp& app, ScrollingItemWidget& renderer,
        ID2D1DeviceContext* context, POINT point, bool clearCache = false)
    {
        app.lastMousePoint_ = point;
        app.UpdateListTitleHover();
        if (clearCache) { app.componentListTextLayoutCache_.clear(); app.componentListTextShadowCache_.clear(); }
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        renderer.DrawContent(context, renderer.GetBodyRect());
        if (FAILED(context->EndDraw())) throw std::runtime_error("actual native list draw must complete");
    }
    static std::vector<std::vector<float>> ListTitleFonts(const DesktopApp& app)
    {
        std::vector<std::vector<float>> result(12);
        for (const auto& [key, layout] : app.componentListTextLayoutCache_)
            for (int i = 0; i < 12; ++i)
                if (key.find(L"Title-" + std::to_wstring(i) + L"-") != std::wstring::npos)
                {
                    float size = 0;
                    if (FAILED(layout->GetFontSize(0, &size))) throw std::runtime_error("read actual list glyph size");
                    result[i].push_back(size);
                }
        return result;
    }
    static double ListTitleStart(const DesktopApp& app) { return app.listTitleHoverStarted_; }
    static float ListTitleOffset(DesktopApp& app)
    {
        const auto offset = app.ResolveListTitleHover(app.listTitleHoverOwner_, app.listTitleHoverKey_,
            app.listTitleHoverRect_, app.listTitleHoverPopup_, 1600, 100, 1);
        return offset.value_or(-1);
    }
    static auto ListTitleToken(const DesktopApp& app) { return app.listTitleHoverFrameToken_; }
    static size_t ScrollingListLayouts(const DesktopApp& app)
    {
        return static_cast<size_t>(std::count_if(app.componentListTextLayoutCache_.begin(),
            app.componentListTextLayoutCache_.end(), [](const auto& entry) { return entry.first.ends_with(L"\x1fscroll"); }));
    }
    static const auto& ListTitleKey(const DesktopApp& app) { return app.listTitleHoverKey_; }
    static RECT ListTitleHoverRect(const DesktopApp& app) { return app.listTitleHoverRect_; }
    static void ListTitleElapsed(DesktopApp& app, double elapsed)
    { app.listTitleHoverStarted_ = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - elapsed; }
    static void ListTitlePressed(DesktopApp& app, bool pressed) { app.mouseDown_ = pressed; }
    static void SelectListTitle(DesktopApp& app, int index)
    { app.widgets_[0].folderEntries.at(index).selected = true; }
    static void ShortenListTitle(DesktopApp& app, int index)
    { app.widgets_[0].folderEntries.at(index).name = L"a"; }
    static void MoveListTitlePointer(DesktopApp& app, POINT point)
    { app.lastMousePoint_ = point; app.UpdateListTitleHover(); }
    static void DispatchListTitleFrame(DesktopApp& app) { app.uiAnimationScheduler_.DispatchDue(); }
    static void CloseListTitlePopup(DesktopApp& app) { app.FinalizeCloseCollectionPopup(); }
    static void ClearListTitleRendering(DesktopApp& app)
    {
        app.CancelUiAnimationFrame(); app.uiAnimationScheduler_.Shutdown();
        app.d2dContext_.Reset(); app.d2dDevice_.Reset(); app.itemTextEffectContext_.Reset();
        app.componentListTextLayoutCache_.clear(); app.componentListTextShadowCache_.clear();
        app.componentListTextFormat_.Reset(); app.dwriteFactory_.Reset();
    }
    static void SeedPopupDisplayMenu(DesktopApp& app, const std::filesystem::path& folder, bool mapped)
    {
        SeedListTitleScene(app, 2, false, true);
        app.widgets_[0].sourceFolderPath = folder.wstring();
        app.dockFolderPopupWidget_ = app.widgets_[0];
        app.dockFolderPopupWidget_.id = kDockFolderPopupWidgetId;
        app.dockFolderPopupWidget_.gridCell = {kDockPageId, 0, 0};
        app.dockFolderPopupWidget_.gridSpan = {1, 1};
        app.dockFolderPopupWidget_.listMode = false;
        app.dockFolderPopupWidget_.dateHeaders = false;
        DockEntry entry;
        entry.type = mapped ? DockEntryType::FolderMapping : DockEntryType::DesktopItem;
        entry.reference = mapped ? app.widgets_[0].id : folder.wstring();
        app.dockEntries_ = {entry};
        app.dockFolderPopupMappingWidgetId_ = mapped ? entry.reference : L"";
        app.dockFolderPopupSourceId_ = std::to_wstring(static_cast<int>(entry.type)) + L":" + ToUpperInvariant(entry.reference);
        app.dockFolderPopupOpen_ = app.dockFolderPopupAvailable_ = true;
        app.popupAnchoredToDock_ = true; app.popupWidgetIndex_ = static_cast<size_t>(-1);
        app.dockFolderPopupContainer_ = std::make_unique<FolderMapping>(&app.dockFolderPopupWidget_, &app);
        app.desktopItemsReady_ = true;
        // An actual opened popup has already classified its Dock source.
        // Keep the real asynchronous provider and delivery in this fixture.
        const auto deadline = GetTickCount64() + 10000;
        while (!app.IsFolderDockEntry(app.dockEntries_.front()))
        {
            if (GetTickCount64() >= deadline) throw std::runtime_error("classify the real isolated Dock folder before menu commands");
            app.shellVisualWork_.Drain();
            Sleep(1);
        }
    }
    static void AppendPopupDisplayMenu(DesktopApp& app, HMENU menu) { app.AppendDockFolderPopupDisplayMenu(menu); }
    static bool PopupDisplayCommand(DesktopApp& app, UINT command) { return app.ExecuteDockFolderPopupDisplayCommand(command); }
    static const DesktopWidget& PopupDisplay(const DesktopApp& app) { return app.dockFolderPopupWidget_; }
    static FolderMapping* DatePopupView(DesktopApp& app) { return app.GetDateGroupedPopupView(app.popupRect_); }
    static RECT PopupDisplayItemRect(DesktopApp& app, size_t index)
    { return app.GetCollectionPopupItemRect(app.popupRect_, index); }
    static void ClearPopupMenuIcons(DesktopApp& app) { app.ClearMenuIcons(); }
