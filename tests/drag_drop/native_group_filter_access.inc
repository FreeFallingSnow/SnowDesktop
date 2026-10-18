    static void SeedFilterSurface(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        DesktopWidgetType type, bool list, bool grouped)
    {
        if (grouped) SeedGroupedMembers(app, files, list, type);
        else SeedMemberModel(app, files, type, list);
        app.items_oo_.clear(); app.containers_.clear();
        for (auto& widget : app.widgets_)
        {
            widget.customTitle = widget.title;
            if (widget.id == (grouped ? L"ordered-group" : L"ordered"))
            {
                widget.gridSpan = {6, 5}; widget.showSearchBox = true;
                if (type == DesktopWidgetType::Collection) widget.scrollContainerMode = true;
            }
        }
        auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        app.LayoutItems();
    }
    static void FilterSurfaceQuery(DesktopApp& app, bool grouped, const std::wstring& query)
    {
        auto* view = dynamic_cast<ScrollingItemWidget*>(MemberContainer(app, grouped ? L"ordered-group" : L"ordered"));
        if (!view) throw std::runtime_error("filter fixture requires its real native scrolling surface");
        view->SetSearchText(query);
    }
    static Slot* FilterSurfaceBoundary(DesktopApp& app, bool grouped, const std::wstring& name)
    {
        auto* view = MemberContainer(app, grouped ? L"ordered-group" : L"ordered");
        for (const auto& slot : view->GetSlots())
            if (auto* item = slot->GetItem(); item && std::filesystem::path(item->GetPath()).filename() == name) return slot.get();
        throw std::runtime_error("filtered insertion anchor must be an actual displayed native member");
    }
    static DropPreviewList FilterSurfacePreview(DesktopApp& app, bool grouped, const std::wstring& name)
    {
        auto* view = MemberContainer(app, grouped ? L"ordered-group" : L"ordered");
        auto* slot = FilterSurfaceBoundary(app, grouped, name);
        const RECT frame = slot->GetBounds();
        const POINT point{frame.left + 1, frame.top + 1};
        app.dragSession_.UpdatePoint(point);
        return app.BuildDropPreviewList(app.dragSession_.SourceList(), view, slot, HitRegion::SortBefore, 0, point);
    }
    static void DropFilterSurface(DesktopApp& app, bool grouped, const std::wstring& name)
    {
        auto* target = MemberContainer(app, grouped ? L"ordered-group" : L"ordered");
        auto* slot = FilterSurfaceBoundary(app, grouped, name);
        auto* origin = app.dragSession_.Source(); const auto items = app.dragSession_.Items();
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, HitRegion::SortBefore, 0);
        app.SaveLayoutSlots(); app.ClearSelection(); app.EndDragSession();
        app.ApplyPageMapping(); app.LayoutItems();
    }
    static void FilterSurfaceDateHeaders(DesktopApp& app, bool grouped)
    {
        const auto index = app.FindWidgetIndexById(grouped ? L"ordered-group" : L"ordered");
        if (index >= app.widgets_.size()) throw std::runtime_error("filtered date fixture requires its actual target");
        app.widgets_[index].dateHeaders = true;
        auto* view = MemberContainer(app, grouped ? L"ordered-group" : L"ordered");
        if (auto* group = dynamic_cast<FileGroup*>(view)) group->InvalidateHostedView();
        else if (auto* mapping = dynamic_cast<FolderMapping*>(view)) mapping->InvalidateFilterCache();
        else if (auto* categories = dynamic_cast<FileCategories*>(view)) categories->InvalidateCategoryCache();
        else view->InvalidateSlots();
    }
    static DWORD TrackGroupSurfaceDwell(DesktopApp& app, const std::wstring& child)
    {
        const RECT frame = GroupSurfaceTabRect(app, L"surface-group", child);
        if (IsRectEmpty(&frame)) throw std::runtime_error("dwell input requires a real native visible group tab");
        const POINT point{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.lastMousePoint_ = point; app.dragSession_.UpdatePoint(point);
        app.UpdateCollectionGroupTabDwell(point);
        return app.collectionGroupTabDwellTick_;
    }
    static bool ActivateGroupSurfaceDwell(DesktopApp& app, DWORD now)
    { return app.TryActivateCollectionGroupTab(now); }
    static void LeaveGroupSurfaceDwell(DesktopApp& app)
    {
        const POINT point{1150, 750}; app.lastMousePoint_ = point;
        app.dragSession_.UpdatePoint(point); app.UpdateCollectionGroupTabDwell(point);
    }
    static bool GroupSurfaceDwellArmed(const DesktopApp& app) { return app.collectionGroupTabDwellTimerArmed_; }
    static void ReplaceGroupSurfaceIdentity(DesktopApp& app)
    {
        const auto index = app.FindWidgetIndexById(L"surface-group");
        if (index >= app.widgets_.size()) throw std::runtime_error("replace the actual pending group identity");
        app.widgets_[index].id = L"replacement-group";
        app.RebuildContainersAndItems();
    }
    static void SeedSearchTypeSurface(DesktopApp& app, const std::vector<std::filesystem::path>& desktop,
        const std::filesystem::path& mapped, bool list)
    {
        SeedGroupedMembers(app, desktop, list, DesktopWidgetType::FileCategories);
        app.items_oo_.clear(); app.containers_.clear();
        auto& inactive = app.widgets_[1];
        inactive.type = DesktopWidgetType::FolderMapping; inactive.sourceFolderPath = mapped.wstring();
        auto snapshot = snowdesktop::shell_refresh::ReadFolder(mapped.wstring(), false);
        if (!snapshot.complete) throw std::runtime_error("read actual mixed search Type fixture");
        inactive.folderEntries = std::move(snapshot.entries);
        for (auto& widget : app.widgets_) widget.customTitle = widget.title;
        auto& group = app.widgets_[2]; group.showSearchBox = true; group.gridSpan = {6, 6};
        auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        app.LayoutItems();
    }
