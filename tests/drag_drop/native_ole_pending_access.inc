    static bool QueuePendingShellGate(DesktopApp& app, std::function<bool()> gate)
    {
        // A prior real read job is the only controlled external boundary.
        // Subsequent file requests retain their production queue and executor.
        return app.shellFileOperationWorker_.Enqueue(snowdesktop::ShellReadRequest{std::move(gate)}, [](bool) {});
    }
    static void EnlargePendingPage(DesktopApp& app)
    {
        auto& page = app.gridPages_.front();
        page.columns = 12; page.rows = 8; page.bounds = page.workArea = page.visualWorkArea = RECT{0,0,1200,800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        app.LayoutItems();
    }
    static void SeedPendingTarget(DesktopApp& app, const std::vector<std::filesystem::path>& files, bool mapped)
    {
        SeedMemberModel(app,files,mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection,false);
        auto& widget = app.widgets_.front(); widget.customTitle = widget.title; widget.userRenamed = true;
        widget.contentSortColumn = snowdesktop::list_detail_rules::Column::None;
        widget.folderSortMode = -1;
        if (mapped)
        {
            widget.itemKeys.clear();
            for (const auto& file : files) widget.itemKeys.push_back(ToUpperInvariant(file.wstring()));
        }
        EnlargePendingPage(app);
    }
    static POINT PendingAnchorPoint(DesktopApp& app)
    {
        auto* target = MemberContainer(app,L"ordered");
        for (const auto& slot : target->GetSlots())
            if (auto* item = slot->GetItem(); item && std::filesystem::path(item->GetPath()).filename() == L"B.txt")
            {
                const auto frame = slot->GetBounds();
                return {frame.left + 1,(frame.top + frame.bottom) / 2};
            }
        throw std::runtime_error("D14: real destination must expose the captured middle anchor B");
    }
    static void MutatePendingTarget(DesktopApp& app, int mutation, const std::filesystem::path& replacement)
    {
        auto& target = app.widgets_.front();
        if (mutation == 0) target.id = L"replacement-target";
        else if (mutation == 1) target.type = DesktopWidgetType::FileCategories;
        else if (mutation == 2)
        {
            target.sourceFolderPath = replacement.wstring();
            auto snapshot = snowdesktop::shell_refresh::ReadFolder(target.sourceFolderPath,false);
            if (!snapshot.complete) throw std::runtime_error("D14: read replacement mapped directory");
            target.folderEntries = std::move(snapshot.entries);
            target.itemKeys.clear();
            for (const auto& entry : target.folderEntries) target.itemKeys.push_back(ToUpperInvariant(entry.fullPath));
        }
        else
        {
            std::erase_if(target.itemKeys, [](const auto& key) {
                return _wcsicmp(std::filesystem::path(key).filename().c_str(),L"B.txt") == 0;
            });
            if (mutation == 4)
                std::erase_if(target.folderEntries, [](const auto& entry) { return entry.name == L"B.txt"; });
        }
        app.RebuildContainersAndItems(); app.LayoutItems();
    }
    static std::vector<std::wstring> PendingTargetOrder(const DesktopApp& app, bool mapped)
    {
        const auto& target = app.widgets_.front();
        if (!mapped) return target.itemKeys;
        std::vector<std::wstring> result;
        for (const auto& entry : target.folderEntries) result.push_back(ToUpperInvariant(entry.fullPath));
        return result;
    }
