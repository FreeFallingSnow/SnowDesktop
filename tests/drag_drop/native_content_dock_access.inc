    // Included inside NativeDragIntegrationAccess. These are seed and commit
    // boundary inputs; the real Dock wrappers, capture, rebuild, mutation and
    // persistence services execute without constructing a desktop host window.
    static void SeedDockRelations(DesktopApp& app,
        const std::vector<std::filesystem::path>& files, bool frequent)
    {
        PopulateModel(app, files, 0);
        for (auto& item : app.items_) item.selected = false;
        SeedDock(app);
        app.dockUsageStats_.clear();
        if (frequent)
        {
            app.dockSettings_.showFrequentItems = true;
            app.dockSettings_.frequentItemCount = 8;
            for (std::size_t index = 0; index < 2; ++index)
                app.dockUsageStats_[app.items_[index].layoutKey] = {10 - static_cast<int>(index), 100};
        }
        else
            for (std::size_t index = 0; index < 2; ++index)
                app.dockEntries_.push_back({DockEntryType::DesktopItem, app.items_[index].layoutKey, true});
        app.items_[2].gridCell = {kDockPageId, 0, 0};
        app.dockEntries_.push_back({DockEntryType::DesktopItem, app.items_[2].layoutKey, false});
        app.RefreshCollectedKeysCache();
        app.LayoutItems();
        app.SaveDockUsageStats();
    }
    static DragSourceList BeginDockRelations(DesktopApp& app,
        const std::vector<std::wstring>& keys, bool frequent, POINT point)
    {
        auto* dock = Dock(app);
        for (const auto& slot : dock->GetSlots())
        {
            auto* item = slot->GetItem();
            const bool expectedRole = frequent ? dynamic_cast<DockFrequentItem*>(item) != nullptr
                : dynamic_cast<DockEntryItem*>(item) != nullptr;
            if (item && expectedRole)
                item->SetSelected(std::any_of(keys.begin(), keys.end(), [&](const auto& key) {
                    return PathsEqualInsensitive(item->GetPath(), key);
                }));
        }
        Item* pressed = nullptr;
        for (const auto& slot : dock->GetSlots())
            if (auto* item = slot->GetItem(); item && PathsEqualInsensitive(item->GetPath(), keys.front())) pressed = item;
        auto capture = snowdesktop::native_item_drag_source::Capture(pressed, false,
            [](Container* source) { return source ? source->GetSelectedItems() : std::vector<Item*>{}; });
        auto items = std::move(capture.items);
        if (capture.source != dock || items.size() != keys.size())
            throw std::runtime_error("B19: the actual native pointer source boundary must admit and capture every selected Dock mapping/frequent entry");
        RECT frame = items.front()->GetBounds();
        for (const auto* item : items) { const auto bounds = item->GetBounds(); UnionRect(&frame, &frame, &bounds); }
        const POINT start{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        app.dragGroupOriginX_ = frame.left; app.dragGroupOriginY_ = frame.top; app.mouseDownPoint_ = start;
        auto source = app.BuildDragSourceList(items, dock);
        app.dragSession_.Begin(dock, std::move(items), source, start, point);
        return source;
    }
    static void RebuildDockRelationSource(DesktopApp& app, bool reorder)
    {
        if (reorder)
        {
            std::swap(app.items_[0], app.items_[1]);
            app.RefreshDesktopItemIndexCache();
        }
        app.RebuildContainersAndItems();
    }
    static bool DockRelationBindingsCurrent(DesktopApp& app, const std::vector<std::wstring>& keys)
    {
        if (!app.dragSession_.IsActive() || app.dragSession_.Source() != Dock(app) ||
            app.dragSession_.Items().size() != keys.size() || app.dragSession_.SourceList().entries.size() != keys.size()) return false;
        const auto& slots = Dock(app)->GetSlots();
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const auto* bound = app.dragSession_.Items()[index];
            const auto live = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) { return slot->GetItem() == bound; });
            if (live == slots.end() || !bound || !PathsEqualInsensitive(bound->GetPath(), keys[index]) ||
                app.dragSession_.SourceList().entries[index].item != bound ||
                !PathsEqualInsensitive(app.dragSession_.SourceList().entries[index].desktopKey, keys[index])) return false;
        }
        return true;
    }
    static bool CommitDockRelationRemoval(DesktopApp& app, POINT point)
    {
        app.ResolveCurrentDragTargetAt(point);
        if (app.GetDockDragOutRemovalHint(point).empty()) return false;
        // Same commit and outer lifetime ordering as native release and SelfOLE
        // Drop. Global pointer/window discovery is outside this model boundary.
        const bool changed = app.RemoveDockDragOutItems(app.dragSession_.Items());
        app.ClearSelection();
        app.EndDragSession();
        if (changed)
        {
            if (!app.SaveLayoutSlots(false)) throw std::runtime_error("persist actual Dock relation removal");
            app.RebuildContainersAndItems();
            app.LayoutItems();
        }
        return changed;
    }
    static bool HasDockUsage(DesktopApp& app, const std::wstring& key)
    { return app.dockUsageStats_.contains(ToUpperInvariant(key)); }
    static void ReloadDockUsage(DesktopApp& app)
    { app.dockUsageStats_.clear(); app.LoadDockUsageStats(); app.LayoutItems(); }
    static std::wstring DockUsagePath(DesktopApp& app)
    { (void)app; return GetDataFilePath(L"SnowDesktop.dock-usage.json"); }
