    static void SeedDockPartitions(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        bool pinned, bool mapping)
    {
        PopulateModel(app, files, 0);
        for (auto& item : app.items_) item.selected = false;
        SeedDock(app);
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
