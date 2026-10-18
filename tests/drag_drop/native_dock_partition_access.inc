    static void SeedDockPartitions(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        bool pinned, bool mapping)
    {
        PopulateModel(app, files, 0);
        // Six real entries need a visible Dock viewport and valid, distinct
        // desktop cells. The generic two-file fixture uses a 4 x 3 page.
        auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        for (auto& item : app.items_) item.selected = false;
        SeedDock(app);
        app.dockAreas_ = {RECT{0, 200, 1200, 300}};
        if (pinned)
            for (auto& item : app.items_)
            {
                app.dockEntries_.push_back({DockEntryType::DesktopItem, item.layoutKey, mapping});
                if (!mapping) item.gridCell = {kDockPageId, 0, 0};
            }
        app.RefreshCollectedKeysCache(); app.LayoutItems();
    }
    static void CommitDockPartition(DesktopApp& app, std::size_t index, int modifiers)
    {
        auto* dock = Dock(app);
        auto* source = app.dragSession_.Source(); const auto items = app.dragSession_.Items();
        Slot boundary(dock, RECT{}, index);
        app.dragSession_.DeactivateForDrop();
        dock->OnItemsDropped(items, source, &boundary, HitRegion::SortBefore, modifiers);
        app.ClearSelection(); app.EndDragSession();
        if (!app.SaveLayoutSlots(false)) throw std::runtime_error("save actual mixed Dock partition commit");
        app.ApplyPageMapping(); app.LayoutItems();
    }
    static std::size_t DockPartitionFolderCount(DesktopApp& app)
    { return app.DockFolderEntryCount(); }
    static void ReorderCapturedDockPartitions(DesktopApp& app)
    {
        std::vector<std::size_t> selected;
        for (const auto& captured : app.dragSession_.SourceList().entries)
        {
            const auto entry = std::find_if(app.dockEntries_.begin(), app.dockEntries_.end(), [&](const auto& current) {
                return current.type == captured.dockEntryType && PathsEqualInsensitive(current.reference, captured.dockReference);
            });
            if (entry == app.dockEntries_.end()) throw std::runtime_error("captured native Dock identity must exist before intervening reorder");
            selected.push_back(static_cast<std::size_t>(std::distance(app.dockEntries_.begin(), entry)));
        }
        if (selected.size() != 4) throw std::runtime_error("intervening reorder fixture requires two main and two folder captures");
        std::swap(app.dockEntries_[selected[0]], app.dockEntries_[selected[1]]);
        std::swap(app.dockEntries_[selected[2]], app.dockEntries_[selected[3]]);
        app.RebuildContainersAndItems();
    }
    static void CommitDockFolderAtFixedEntry(DesktopApp& app, const std::wstring& fixedKey, bool after)
    {
        auto* dock = Dock(app);
        Slot* target = nullptr;
        for (const auto& slot : dock->GetSlots())
            if (const auto* entry = dynamic_cast<const DockEntryItem*>(slot->GetItem());
                entry && PathsEqualInsensitive(entry->GetReference(), fixedKey)) target = slot.get();
        if (!target) throw std::runtime_error("fixed-area folder drop requires the actual existing main entry slot");
        auto* source = app.dragSession_.Source(); const auto items = app.dragSession_.Items();
        app.dragSession_.DeactivateForDrop();
        dock->OnItemsDropped(items, source, target, after ? HitRegion::SortAfter : HitRegion::SortBefore, 0);
        app.ClearSelection(); app.EndDragSession();
        if (!app.SaveLayoutSlots(false)) throw std::runtime_error("save actual fixed-area folder insertion");
        app.ApplyPageMapping(); app.LayoutItems();
    }
