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
        // Legacy filter/type scenarios explicitly exercise the unavailable-SDK
        // branch; scoped Everything scenarios inject their own boundary.
        SetFolderSearchBoundary(*view, [](const std::wstring&, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse response;
            response.error = ERROR_NOT_READY;
            return response;
        });
        view->SetSearchText(query);
        view->EnsureFolderSearch();
        const auto deadline = GetTickCount64() + 8000;
        while (FolderSearchPending(*view))
        {
            if (GetTickCount64() > deadline) throw std::runtime_error("unavailable SDK fixture must settle before legacy direct-only assertions");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    static std::wstring SortFreeType(DesktopApp& app, bool ascending)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.widgets_.clear();
        for (auto& item : app.items_) item.shortcutTarget = {};
        const std::pair<const wchar_t*, const wchar_t*> systemItems[]{
            {L"T", L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}"},
            {L"U", L"{645FF040-5081-101B-9F08-00AA002F954E}"}};
        // Only the external namespace metadata boundary is supplied here;
        // production SortIcons assigns and persists the actual grid cells.
        for (const auto& [name, clsid] : systemItems)
        {
            auto item = FileItem(name);
            item.desktopIconClsid = item.layoutKey = clsid;
            item.parsingName = L"::" + std::wstring(clsid);
            item.typeName = std::wstring(name) == L"T" ? L"Z" : L"A";
            app.items_.push_back(std::move(item));
        }
        app.RefreshDesktopItemIndexCache();
        app.RefreshCollectedKeysCache();
        app.SortIcons(snowdesktop::folder_sort_rules::kType, ascending);
        std::vector<const DesktopItem*> ordered;
        for (const auto& item : app.items_) ordered.push_back(&item);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->gridCell.column != b->gridCell.column
                ? a->gridCell.column < b->gridCell.column : a->gridCell.row < b->gridCell.row;
        });
        std::wstring result;
        for (const auto* item : ordered) result += item->name.front();
        return result;
    }
    static void ReadyGroupTypeMetadata(DesktopApp& app)
    {
        // The windowless host intentionally does not start the icon loader.
        // Supply only its independent Shell metadata boundary, never an order:
        // actual type strings and a real folder snapshot are consumed by the
        // production refresh projector and the actual sort command.
        const auto typeFor = [](const std::wstring& path) {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_TYPENAME) == 0 || !info.szTypeName[0])
                throw std::runtime_error("native Type fixture requires ready actual Shell type metadata");
            return std::wstring(info.szTypeName);
        };
        for (auto& item : app.items_) item.typeName = typeFor(item.parsingName);
        for (auto& widget : app.widgets_)
            if (widget.type == DesktopWidgetType::FolderMapping)
            {
                auto snapshot = snowdesktop::shell_refresh::ReadFolder(widget.sourceFolderPath, false);
                if (!snapshot.complete) throw std::runtime_error("read actual Type folder metadata snapshot");
                for (auto& entry : snapshot.entries) entry.typeName = typeFor(entry.fullPath);
                app.EnumerateFolderMappingEntries(widget, false, &snapshot);
            }
        app.RebuildContainersAndItems();
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
        app.ResolveWidgetContentViewData(&app.widgets_[index])->dateHeaders = true;
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
    static void ConfigureGroupCategoryContract(DesktopApp& app)
    {
        app.categorySettings_.rules = {{L"contract-text", L"Contract text", L".txt", true}};
        for (auto& widget : app.widgets_)
        {
            if (widget.id == L"surface-group")
            {
                widget.activeCategoryId = L"A";
                widget.showSearchBox = false;
            }
            else if (widget.id == L"A" || widget.id == L"B")
            {
                widget.activeCategoryId = L"all";
                widget.showFileCategories = true;
            }
        }
        app.RebuildContainersAndItems();
    }
    static POINT GroupCategoryContractPoint(DesktopApp& app)
    {
        auto* group = dynamic_cast<FileGroup*>(MemberContainer(app, L"surface-group"));
        if (!group) throw std::runtime_error("category dwell requires an actual native file group");
        // Supply a point found by the real public hit function. No category
        // activation or timer state is manufactured by this geometry search.
        const RECT frame = group->GetBounds();
        for (LONG y = frame.top; y < frame.bottom; y += 4)
            for (LONG x = frame.left; x < frame.right; x += 4)
            {
                const POINT point{x, y};
                if (group->CategoryIdAtPoint(point) == L"contract-text" && group->SourceIdAtPoint(point).empty())
                    return point;
            }
        throw std::runtime_error("configured .txt rule must expose a real inactive inner category tab");
    }
    static DWORD TrackGroupCategoryContract(DesktopApp& app, POINT point)
    {
        app.lastMousePoint_ = point; app.dragSession_.UpdatePoint(point);
        app.UpdateCollectionGroupTabDwell(point);
        return app.collectionGroupTabDwellTick_;
    }
    static void ReplaceGroupCategorySource(DesktopApp& app, POINT point)
    {
        auto* group = dynamic_cast<FileGroup*>(MemberContainer(app, L"surface-group"));
        if (!group || !group->GetWidgetData()) throw std::runtime_error("change only the actual category target source");
        group->GetWidgetData()->activeCategoryId = L"B";
        group->InvalidateHostedView();
        if (group->CategoryIdAtPoint(point) != L"contract-text" || !group->SourceIdAtPoint(point).empty())
            throw std::runtime_error("both real sources must expose the same category hit at the original point");
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

    static void SetFolderSearchBoundary(ScrollingItemWidget& view,
        snowdesktop::QuickNavigationEverythingSearchAsync::SearchFunction provider)
    {
        if (view.folderSearch_ && view.folderSearch_->worker) view.folderSearch_->worker->Stop();
        view.folderSearch_ = std::make_shared<snowdesktop::WidgetFolderSearchState>();
        view.folderSearch_->search = std::move(provider);
        view.InvalidateSlots();
    }
    static const auto& FolderSearchSections(const ScrollingItemWidget& view) { return view.folderSearch_->sections; }
    static bool FolderSearchPending(ScrollingItemWidget& view) { view.EnsureFolderSearch(); return view.folderSearch_ && view.folderSearch_->pending; }
    static void DumpFolderSearch(DesktopApp& app, ScrollingItemWidget& view)
    {
        std::cerr << "search box=" << view.GetWidgetData()->showSearchBox << " query=" << WideToUtf8(view.GetSearchText()) << '\n';
        if (const auto& state = view.folderSearch_)
        {
            std::cerr << "boundary=" << static_cast<bool>(state->search) << " generation=" << state->generation
                << " available=" << state->available << " pending=" << state->pending
                << " results=" << state->results.size() << " rows=" << state->rows.size() << '\n';
            for (const auto& row : state->rows)
                std::cerr << "row source=" << WideToUtf8(row.sourceId) << " recursive=" << static_cast<bool>(row.recursive)
                    << " key=" << WideToUtf8(row.desktopKey) << " index=" << row.folderIndex << '\n';
        }
        for (const auto& widget : app.widgets_)
        {
            std::cerr << "widget " << WideToUtf8(widget.id) << " type=" << static_cast<int>(widget.type)
                << " search=" << widget.showSearchBox << " root=" << WideToUtf8(widget.sourceFolderPath) << '\n';
            for (const auto& entry : widget.folderEntries) std::cerr << " entry " << WideToUtf8(entry.fullPath) << '\n';
            for (const auto& key : widget.itemKeys) std::cerr << " key " << WideToUtf8(key) << '\n';
        }
    }
    static std::vector<std::wstring> FolderSearchClipboard(const DesktopApp& app) { return app.GetSelectedFolderEntryPaths(); }
    static auto FolderSearchTargets(DesktopApp& app, const std::wstring& id) { return app.GetWidgetSelectionTargets(app.FindWidgetIndexById(id)); }
    static void ClearFolderSearchSelection(DesktopApp& app) { app.ClearSelection(); }
    static auto CaptureFolderSearch(DesktopApp& app, ScrollingItemWidget& view) { return app.BuildDragSourceList(view.GetSelectedItems(), &view); }
    static auto SearchContextPaths(DesktopApp& app, ScrollingItemWidget& view, POINT point)
    {
        const auto parent = app.FindWidgetIndexById(view.GetWidgetData()->id);
        return snowdesktop::widget_search_context_menu::Prepare(view, point,
            [&] { app.ClearSelection(); },
            [&] {
                app.ClearSelectionOutsideWidget(parent);
                if (parent < app.widgets_.size()) app.widgets_[parent].selected = true;
            },
            [&](Item* item) {
                const auto* desktop = dynamic_cast<DesktopIcon*>(item);
                return !desktop || (desktop->GetDesktopItem() && !app.IsProtectedDesktopIcon(*desktop->GetDesktopItem()));
            });
    }
    static void RebuildFolderSearch(DesktopApp& app) { app.RebuildContainersAndItems(); }
    static void WheelFolderSearch(DesktopApp& app, ScrollingItemWidget& view, int delta)
    {
        const RECT viewport = view.GetContentViewportRect();
        POINT point{(viewport.left + viewport.right) / 2, (viewport.top + viewport.bottom) / 2};
        ClientToScreen(app.hwnd_, &point);
        app.OnMouseWheel(MAKEWPARAM(0, delta), MAKELPARAM(point.x, point.y));
    }
    static void RevealFolderSearch(DesktopApp& app, ScrollingItemWidget& view, size_t index)
    {
        app.ScrollWidgetToMember(app.FindWidgetIndexById(view.GetWidgetData()->id), static_cast<int>(index));
    }
    static void AddSearchDropTarget(DesktopApp& app, const std::filesystem::path& mapped, int kind)
    {
        // Release wrappers before growing their model vector.
        app.items_oo_.clear(); app.containers_.clear();
        DesktopWidget target;
        target.id = L"search-target"; target.title = target.customTitle = L"Search destination";
        target.type = kind == 1 ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection;
        target.sourceFolderPath = kind == 1 ? mapped.wstring() : L"";
        target.gridCell = {L"__page:1", 8, 0}; target.gridSpan = {3, 4};
        target.autoCollect = false; target.activeCategoryId = L"all";
        app.widgets_.push_back(std::move(target)); app.LayoutItems();
    }
    static DragSourceList BeginSearchDrop(DesktopApp& app, ScrollingItemWidget& view, POINT destination, int mods)
    {
        auto items = view.GetSelectedItems();
        if (items.empty()) throw std::runtime_error("search drop requires a selected real result");
        const RECT bounds = items.front()->GetBounds();
        const POINT start{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        auto captured = app.BuildDragSourceList(items, &view);
        app.dragGroupOriginX_ = bounds.left; app.dragGroupOriginY_ = bounds.top; app.mouseDownPoint_ = start;
        app.dragSession_.Begin(&view, std::move(items), captured, start, destination);
        app.dragSession_.UpdateActionFromMods(mods);
        return captured;
    }
    static void SubmitSearchDrop(DesktopApp& app, int mods)
    {
        auto* target = app.dragSession_.TargetContainer(); auto* origin = app.dragSession_.Source();
        auto* slot = app.dragSession_.TargetSlot(); const auto region = app.dragSession_.TargetRegion();
        auto items = app.dragSession_.Items();
        if (!target) throw std::runtime_error("search drop must resolve an actual native target");
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, origin, slot, region, mods);
        app.EndDragSession();
    }
    static DropPreviewList FolderSearchDesktopPreview(DesktopApp& app, ScrollingItemWidget& view)
    {
        auto items = view.GetSelectedItems();
        if (items.empty()) throw std::runtime_error("search drag requires actual selected runtime items");
        const RECT bounds = items.front()->GetBounds();
        const POINT start{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        const POINT destination{850, 650};
        auto captured = app.BuildDragSourceList(items, &view);
        app.dragGroupOriginX_ = bounds.left; app.dragGroupOriginY_ = bounds.top; app.mouseDownPoint_ = start;
        app.dragSession_.Begin(&view, std::move(items), captured, start, destination);
        app.ResolveCurrentDragTargetAt(destination);
        auto preview = app.BuildDropPreviewList(app.dragSession_.SourceList(), app.dragSession_.TargetContainer(),
            app.dragSession_.TargetSlot(), app.dragSession_.TargetRegion(), 0, destination);
        app.dragSession_.End();
        return preview;
    }
