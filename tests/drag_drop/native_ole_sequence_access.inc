    // Initial transport handoff is an input. Every returned OLE callback below
    // still executes the host; no DoDragDrop or cursor/window discovery runs.
    static ComPtr<IDataObject> BeginSequenceSelfOle(DesktopApp& app, bool right)
    {
        app.rightButtonItemDrag_ = right;
        app.middleButtonWidgetMove_ = false;
        app.dragSession_.SetVisualVisible(false);
        app.dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        auto source = app.CreateDataObjectForItems(app.dragSession_.Items());
        if (!source) throw std::runtime_error("S04: serialize actual captured native items for OLE");
        app.dragDropController_.BeginSelfDrag();
        return source;
    }
    static bool SequenceSelfRetained(const DesktopApp& app, std::size_t count)
    {
        return app.dragDropController_.IsSelfDragActive() && app.dragDropController_.SelfDragReturned() &&
            app.dragSession_.IsActive() && !app.dragSession_.IsVisualVisible() &&
            app.dragSession_.Items().size() == count && app.dragSession_.SourceList().entries.size() == count &&
            !app.dragDropController_.IsExternalDragActive();
    }
    static void EndSequenceSelfOle(DesktopApp& app)
    {
        app.dragDropController_.EndSelfDrag();
        app.rightButtonItemDrag_ = false;
    }
    static bool SequenceSessionEnded(const DesktopApp& app)
    { return !app.dragSession_.IsActive() && !app.dragSession_.HasContext(); }
    static void SeedSequenceOrigin(DesktopApp& app, const std::vector<std::filesystem::path>& files,
        const std::filesystem::path& sentinel, int source)
    {
        if (source <= 2) PopulateModel(app, files, source);
        else if (source == 3) SeedMemberModel(app, files, DesktopWidgetType::FolderMapping, false);
        else if (source < 7) SeedGroupedMembers(app, files, false, source == 4 ? DesktopWidgetType::Collection
            : source == 5 ? DesktopWidgetType::FileCategories : DesktopWidgetType::FolderMapping);
        else SeedDockFiles(app, files, sentinel);
        // Actual desktop export needs its real Shell folder and child PIDLs;
        // the lightweight layout fixture intentionally omits those providers.
        if (!app.items_.empty())
        {
            ComPtr<IShellFolder> shellDesktop;
            if (FAILED(SHGetDesktopFolder(&shellDesktop))) throw std::runtime_error("S04: bind real desktop Shell provider");
            auto parent = app.items_.front().parsingName;
            parent = std::filesystem::path(parent).parent_path().wstring();
            Pidl parentPidl(ILCreateFromPathW(parent.c_str()));
            if (!parentPidl.get() || FAILED(shellDesktop->BindToObject(parentPidl.get(),nullptr,IID_PPV_ARGS(&app.desktopFolder_))))
                throw std::runtime_error("S04: bind the isolated physical source folder");
            for (auto& item : app.items_)
            {
                item.absolutePidl = Pidl(ILCreateFromPathW(item.parsingName.c_str()));
                if (!item.absolutePidl.get()) throw std::runtime_error("S04: resolve real selected file PIDL");
                item.childPidl = Pidl(ILClone(ILFindLastID(item.absolutePidl.get())));
            }
        }
        for (auto& widget : app.widgets_) widget.customTitle = widget.title;
        if (source)
        {
            auto& page = app.gridPages_.front();
            page.columns = 12; page.rows = 8; page.bounds = page.workArea = page.visualWorkArea = {0,0,1200,800};
            app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        }
        app.LayoutItems();
    }
    static void BeginSequenceOrigin(DesktopApp& app, int source, std::size_t count, POINT destination)
    {
        for (auto& item : app.items_) item.selected = item.name == L"A.txt" || (count == 2 && item.name == L"B.txt");
        for (auto& widget : app.widgets_)
        {
            widget.selected = false;
            for (auto& item : widget.folderEntries) item.selected = item.name == L"A.txt" || (count == 2 && item.name == L"B.txt");
        }
        Container* origin = nullptr;
        if (source == 0) origin = Desktop(app);
        else if (source <= 2) origin = MemberContainer(app,L"native-origin");
        else if (source == 3) origin = MemberContainer(app,L"ordered");
        else if (source < 7) origin = MemberContainer(app,L"ordered-group");
        else origin = Dock(app);
        if (source == 7)
            for (const auto& slot : origin->GetSlots())
                if (auto* item = dynamic_cast<DockEntryItem*>(slot->GetItem()))
                    item->SetSelected(std::filesystem::path(item->GetPath()).filename() == L"A.txt" ||
                        (count == 2 && std::filesystem::path(item->GetPath()).filename() == L"B.txt"));
        auto items = origin->GetSelectedItems();
        if (items.size() != count) throw std::runtime_error("S04: real source must capture all selected original members");
        RECT frame = items.front()->GetBounds();
        const POINT start{(frame.left + frame.right) / 2,(frame.top + frame.bottom) / 2};
        for (auto* item : items) { const auto bounds = item->GetBounds(); UnionRect(&frame,&frame,&bounds); }
        app.dragGroupOriginX_ = frame.left; app.dragGroupOriginY_ = frame.top; app.mouseDownPoint_ = start;
        auto captured = app.BuildDragSourceList(items,origin);
        app.dragSession_.Begin(origin,std::move(items),std::move(captured),start,destination);
    }
    static void SeedSequenceGroup(DesktopApp& app, const std::filesystem::path& member)
    {
        SeedExternalStorage(app, DesktopWidgetType::CollectionGroup, DesktopWidgetType::Collection, false, member);
        auto& page = app.gridPages_.front();
        page.columns = 12; page.rows = 8; page.bounds = page.workArea = page.visualWorkArea = {0,0,1200,800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        auto& group = app.widgets_.back(); group.gridSpan = {4,3};
        app.LayoutItems();
    }
    static void SwitchSequenceActive(DesktopApp& app)
    {
        auto index = app.FindWidgetIndexById(L"ingress-root");
        if (index >= app.widgets_.size()) throw std::runtime_error("S06: actual group identity must exist");
        app.widgets_[index].activeCategoryId = L"ingress-inactive";
        app.LayoutItems();
    }
