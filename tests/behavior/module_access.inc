// Include inside NativeDragIntegrationAccess. These adapters expose real
// production entry points and observations; they do not supply final results.
static std::vector<std::wstring> QuickNavigationNames(DesktopApp& app,
    const std::vector<std::filesystem::path>& files, const std::wstring& query)
{
    PopulateModel(app, files, 0);
    app.quickNavigationSearchType_ = QuickNavigationSearchType::All;
    app.quickNavigationActiveWidgetIndex_ = static_cast<std::size_t>(-1);
    app.quickNavigationSearchText_ = app.quickNavigationEffectiveSearchText_ = query;
    app.quickNavigationSearchCompositionText_.clear();
    app.dockEntries_.clear();
    std::vector<std::wstring> names;
    for (const auto& entry : app.BuildQuickNavigationContentModel().entries)
        names.push_back(entry.name);
    return names;
}
static snowdesktop::SettingsController& HostSettings(DesktopApp& app)
{
    app.InitializeSettingsController();
    if (!app.settingsController_ || !app.settingsController_->Snapshot()->initialized)
        throw std::runtime_error("initialize real host settings adapter");
    return *app.settingsController_;
}
static std::uint32_t AppliedLogModules(const DesktopApp& app)
{ return app.generalSettings_.diagnosticLogModules; }
// User decision (2026-10-31): retain each source section and sort within it.
// Only the external SDK answer is supplied (no additional recursive matches);
// local identities, sorting and source-section projection execute production.
static void SeedGroupTypeSources(DesktopApp& app,
    const std::vector<std::filesystem::path>& desktop, const std::filesystem::path& mapped)
{
    app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
    DesktopWidget first;
    first.id = L"type-desktop"; first.type = DesktopWidgetType::FileCategories;
    first.title = first.customTitle = L"Desktop source";
    first.gridCell = {L"__page:1",0,0}; first.gridSpan = {4,3}; first.autoCollect = false;
    for (const auto& path : desktop)
    {
        auto item = FileItem(path);
        first.itemKeys.push_back(item.layoutKey); app.items_.push_back(std::move(item));
    }
    DesktopWidget second;
    second.id = L"type-mapped"; second.type = DesktopWidgetType::FolderMapping;
    second.title = second.customTitle = L"Mapped source";
    second.gridCell = first.gridCell; second.gridSpan = first.gridSpan;
    second.sourceFolderPath = mapped.wstring(); second.autoCollect = false;
    auto read = snowdesktop::shell_refresh::ReadFolder(mapped.wstring(), false);
    if (!read.complete) throw std::runtime_error("read actual mapped Type source");
    second.folderEntries = std::move(read.entries);
    DesktopWidget group;
    group.id = L"type-group"; group.type = DesktopWidgetType::FileGroup;
    group.title = group.customTitle = L"Combined files";
    group.gridCell = first.gridCell; group.gridSpan = {6,6}; group.showSearchBox = true;
    group.childWidgetIds = {first.id,second.id}; group.activeCategoryId = first.id;
    app.widgets_ = {first,second,group};
    auto& page = app.gridPages_.front(); page.columns = 12; page.rows = 8;
    page.bounds = page.workArea = page.visualWorkArea = RECT{0,0,1200,800};
    app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
    app.desktopItemsReady_ = true; app.RefreshDesktopItemIndexCache(); app.LayoutItems();
}
static std::vector<std::filesystem::path> GroupTypeOrder(DesktopApp& app, bool applySort = false)
{
    auto* view = dynamic_cast<ScrollingItemWidget*>(MemberContainer(app,L"type-group"));
    if (!view) throw std::runtime_error("real grouped file search surface");
    if (!view->folderSearch_ || !view->folderSearch_->search)
    {
        if (view->folderSearch_ && view->folderSearch_->worker) view->folderSearch_->worker->Stop();
        view->folderSearch_ = std::make_shared<snowdesktop::WidgetFolderSearchState>();
        view->folderSearch_->search = [](const std::wstring&, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse result;
            return result;
        };
        view->InvalidateSlots();
    }
    view->SetSearchText(L"needle"); view->EnsureFolderSearch();
    const auto deadline = GetTickCount64() + 8000;
    while (view->folderSearch_->pending)
    {
        if (GetTickCount64() > deadline) throw std::runtime_error("group search completion deadline");
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        view->EnsureFolderSearch();
    }
    if (applySort)
    {
        const auto groupIndex = app.FindWidgetIndexById(L"type-group");
        app.SortWidgetContents(groupIndex,snowdesktop::folder_sort_rules::kType,true);
    }
    view = dynamic_cast<ScrollingItemWidget*>(MemberContainer(app,L"type-group"));
    if (!view) throw std::runtime_error("reacquire real group after sorting");
    std::vector<std::filesystem::path> ordered;
    for (const auto& slot : view->GetSlots())
        if (const auto* value = slot->GetItem()) ordered.emplace_back(value->GetPath());
    return ordered;
}
static void DiscardGroupTypeRuntimeState(DesktopApp& app)
{
    for (auto& widget : app.widgets_)
    {
        widget.itemKeys.clear();
        if (widget.id != L"type-group") continue;
        widget.contentSortColumn = snowdesktop::list_detail_rules::Column::None;
        widget.contentSortAscending = false;
        widget.childWidgetIds = {L"type-mapped",L"type-desktop"};
        widget.activeCategoryId = L"type-mapped";
    }
}
