    static void SeedNamespaceMapping(DesktopApp& app, const std::filesystem::path& ordinary,
        const std::filesystem::path& mappedMember, const wchar_t* clsid, bool grouped)
    {
        PopulateModel(app, {ordinary}, 0);
        auto& page = app.gridPages_.front();
        page.columns = 12; page.rows = 8;
        page.bounds = page.workArea = page.visualWorkArea = RECT{0, 0, 1200, 800};
        app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
        auto special = FileItem(ordinary);
        special.name = L"System namespace source";
        special.desktopIconClsid = clsid;
        special.parsingName = std::wstring(L"::") + clsid;
        special.layoutKey = ToUpperInvariant(clsid);
        special.gridCell = {L"__page:1", 1, 0};
        app.items_.insert(app.items_.begin(), std::move(special));
        SeedExternalStorage(app, grouped ? DesktopWidgetType::FileGroup : DesktopWidgetType::FolderMapping,
            DesktopWidgetType::FolderMapping, false, mappedMember);
        app.widgets_.back().gridCell = {L"__page:1", 3, 0};
        app.LayoutItems();
    }
    static POINT NamespaceMappingPoint(DesktopApp& app, bool grouped)
    {
        const auto body = MemberContainer(app, L"ingress-root")->GetBodyRect();
        (void)grouped;
        return {(body.left + body.right) / 2, (body.top + body.bottom) / 2};
    }
    static std::wstring CurrentNativeDragHint(DesktopApp& app, int modifiers)
    {
        auto* target = Target(app);
        return target ? target->GetDragHint(TargetSlot(app), Region(app),
            app.dragSession_.Items(), app.dragSession_.Source(), modifiers) : std::wstring{};
    }
    static void EnableNamespaceRead(DesktopApp& app, const wchar_t* clsid)
    { app.settingsIconVisibility_[ToUpperInvariant(clsid)] = true; }
    static void BeginNamespaceMixed(DesktopApp& app, const wchar_t* clsid)
    {
        const auto special = ToUpperInvariant(clsid);
        for (auto& item : app.items_) item.selected = item.layoutKey == special || item.name == L"Ordinary.txt";
        auto* source = app.containers_.front().get();
        auto items = source->GetSelectedItems();
        if (items.size() != 2) throw std::runtime_error("D15 mixed capture requires one actual namespace and one ordinary file");
        auto captured = app.BuildDragSourceList(items, source);
        app.dragGroupOriginX_ = app.mouseDownPoint_.x = 10;
        app.dragGroupOriginY_ = app.mouseDownPoint_.y = 10;
        app.dragSession_.Begin(source, std::move(items), std::move(captured), {10, 10}, {10, 10});
    }
    static void SubmitNamespaceMapping(DesktopApp& app, int modifiers)
    {
        auto* target = Target(app);
        if (!target) throw std::runtime_error("D15 requires the actually hit foreground target");
        const auto items = app.dragSession_.Items();
        auto* source = app.dragSession_.Source(); auto* slot = TargetSlot(app); const auto region = Region(app);
        app.dragSession_.DeactivateForDrop();
        target->OnItemsDropped(items, source, slot, region, modifiers);
        app.EndDragSession();
    }
