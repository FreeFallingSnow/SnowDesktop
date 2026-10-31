// Included inside NativeDragIntegrationAccess. These members set up inputs or
// observe state; routing, transactions and persistence remain production code.
static void ResetContainerScene(DesktopApp& app, const std::filesystem::path& freeFile = {})
{
    app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
    auto& page = app.gridPages_.front();
    page.columns = 12; page.rows = 8;
    page.bounds = page.workArea = page.visualWorkArea = RECT{0,0,1200,800};
    app.savedPageColumns_[page.id] = 12; app.savedPageRows_[page.id] = 8;
    if (!freeFile.empty())
    {
        auto item = FileItem(freeFile);
        item.gridCell = {page.id,8,0};
        app.items_.push_back(std::move(item));
    }
}
static bool ContainerWorkIdle(const DesktopApp& app)
{
    return AsyncIdle(app) && app.shellFileOperationInFlight_ == 0 && app.folderReadsPending_.empty();
}
static void ReadMappedContainer(DesktopApp& app, const std::wstring& path)
{ app.RequestFolderRefresh({path}); }
static std::vector<std::filesystem::path> ContainerMembers(const DesktopApp& app, const std::wstring& id)
{
    const auto it = std::find_if(app.widgets_.begin(),app.widgets_.end(),[&](const auto& w) { return w.id == id; });
    if (it == app.widgets_.end()) throw std::runtime_error("expected container identity is missing");
    std::vector<std::filesystem::path> result;
    if (it->type == DesktopWidgetType::FolderMapping)
        for (const auto& entry : it->folderEntries) result.emplace_back(entry.fullPath);
    else for (const auto& key : it->itemKeys) result.emplace_back(key);
    return result;
}
static void ReplaceContainerIdentity(DesktopApp& app)
{
    if (app.widgets_.size() != 1) throw std::runtime_error("identity replacement requires one ungrouped container");
    app.widgets_.front().id = L"replacement";
    app.RebuildContainersAndItems(); app.LayoutItems();
}
static void ChangeContainerMapping(DesktopApp& app, const std::filesystem::path& path)
{
    auto& target = app.widgets_.front();
    target.sourceFolderPath = path.wstring();
    auto snapshot = snowdesktop::shell_refresh::ReadFolder(target.sourceFolderPath,false);
    if (!snapshot.complete) throw std::runtime_error("read owned replacement mapping");
    target.folderEntries = std::move(snapshot.entries); target.itemKeys.clear();
    app.RebuildContainersAndItems(); app.LayoutItems();
}
static void SeedMemberOrder(DesktopApp& app, const std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& freeFile)
{
    ResetContainerScene(app,freeFile);
    DesktopWidget widget;
    widget.id = L"members"; widget.type = DesktopWidgetType::Collection;
    widget.title = widget.customTitle = L"Ordered collection";
    widget.gridCell = {L"__page:1",0,0}; widget.gridSpan = {4,3}; widget.autoCollect = false;
    for (const auto& path : paths)
    {
        auto item = FileItem(path); widget.itemKeys.push_back(item.layoutKey);
        app.items_.push_back(std::move(item));
    }
    app.widgets_.push_back(std::move(widget));
    app.desktopItemsReady_ = true; app.RefreshDesktopItemIndexCache(); app.LayoutItems();
    app.SortWidgetContents(0,snowdesktop::folder_sort_rules::kName,true);
}
static void BeginContainerMembers(DesktopApp& app, const std::wstring& owner,
    const std::vector<std::filesystem::path>& selected)
{
    app.ClearSelection();
    for (auto& item : app.items_)
        item.selected = std::any_of(selected.begin(),selected.end(),[&](const auto& path) {
            std::error_code error; return std::filesystem::equivalent(item.parsingName,path,error) && !error;
        });
    auto* source = MemberContainer(app,owner);
    auto items = source->GetSelectedItems();
    if (items.size() != selected.size()) throw std::runtime_error("native selection must capture every requested member");
    auto captured = app.BuildDragSourceList(items,source);
    app.dragGroupOriginX_ = 10; app.dragGroupOriginY_ = 10;
    app.mouseDownPoint_ = {50,50}; app.mouseDown_ = true;
    app.dragSession_.Begin(source,std::move(items),std::move(captured),{50,50},{50,50});
}
static void ReleaseContainerMembers(DesktopApp& app, POINT point)
{
    app.dragSession_.UpdatePoint(point);
    app.ResolveCurrentDragTargetAt(point);
    app.OnLeftButtonUpAt(0,point);
}
static bool ManualMemberOrder(const DesktopApp& app)
{
    const auto& widget = app.widgets_.front();
    return widget.contentSortColumn == snowdesktop::list_detail_rules::Column::None && widget.folderSortMode == snowdesktop::folder_sort_rules::kManual;
}

static std::size_t FreeDesktopCopies(const DesktopApp& app, const std::filesystem::path& path)
{
    return static_cast<std::size_t>(std::count_if(app.items_oo_.begin(),app.items_oo_.end(),[&](const auto& object) {
        const auto* icon = dynamic_cast<const DesktopIcon*>(object.get());
        if (!icon || icon->GetContainer() != app.containers_.front().get()) return false;
        std::error_code error;
        return std::filesystem::equivalent(icon->GetPath(),path,error) && !error;
    }));
}
