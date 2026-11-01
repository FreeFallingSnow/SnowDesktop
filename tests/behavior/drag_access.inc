// Included in the existing friend adapter. No replacement planning/commit logic.
static void EnableFileDispatch(DesktopApp& app)
{
    if (asyncOwner != &app || fileDispatch) throw std::runtime_error("owned async host required");
    SetLastError(ERROR_SUCCESS);
    previousWindowData = SetWindowLongPtrW(completionWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    if (!previousWindowData && GetLastError() != ERROR_SUCCESS) throw std::runtime_error("bind file completion host");
    // Real deferred layout saves need the control-window timer and completion
    // services too. Both procedures use this fixture-owned userdata/window.
    app.controlHwnd_ = completionWindow;
    fileDispatch = true;
}
static void AddCollection(DesktopApp& app, const std::wstring& id, int column,
    const std::vector<std::filesystem::path>& members = {})
{
    DesktopWidget widget;
    widget.id = widget.title = id; widget.type = DesktopWidgetType::Collection;
    widget.gridCell = {L"__page:1",column,0}; widget.gridSpan = {2,2}; widget.autoCollect = false;
    for (const auto& path : members) {
        auto item = FileItem(path); widget.itemKeys.push_back(item.layoutKey);
        app.items_.push_back(std::move(item));
    }
    app.widgets_.push_back(std::move(widget));
    app.RefreshDesktopItemIndexCache(); app.LayoutItems();
}
static void Select(DesktopApp& app, const std::vector<std::filesystem::path>& paths)
{
    app.ClearSelection();
    for (auto& item : app.items_) item.selected = std::any_of(paths.begin(),paths.end(),[&](const auto& path) {
        return _wcsicmp(item.parsingName.c_str(),path.c_str()) == 0;
    });
}
static void BeginMembers(DesktopApp& app, const std::wstring& id)
{
    Container* origin = id.empty() ? static_cast<Container*>(app.GetDesktopGrid()) : MemberContainer(app,id);
    if (!origin) throw std::runtime_error("real drag origin required");
    auto members = origin->GetSelectedItems();
    auto sources = app.BuildDragSourceList(members,origin);
    app.mouseDownPoint_ = {50,50}; app.mouseDown_ = true;
    app.dragGroupOriginX_ = 10; app.dragGroupOriginY_ = 10;
    app.dragSession_.Begin(origin,std::move(members),std::move(sources),{50,50},{50,50});
}
static const DragSourceList& Sources(const DesktopApp& app) { return app.dragSession_.SourceList(); }
static std::size_t VisibleMembers(const DesktopApp& app)
{
    std::size_t count = 0;
    for (std::size_t i=0;i<app.items_.size();++i) {
        const auto rect = app.GetVisibleCollectionItemBounds(i);
        if (!IsRectEmpty(&rect)) ++count;
    }
    return count;
}
static void RebuildDrag(DesktopApp& app)
{
    std::reverse(app.items_.begin(),app.items_.end());
    app.RefreshDesktopItemIndexCache(); app.RebuildContainersAndItems(); app.LayoutItems();
}
static void CancelDrag(DesktopApp& app) { app.OnKeyDown(VK_ESCAPE,false); }
static bool HasDrag(const DesktopApp& app) { return app.dragSession_.HasContext() || app.mouseDown_; }
static void ReleaseDrag(DesktopApp& app, POINT point)
{
    app.dragSession_.UpdatePoint(point); app.ResolveCurrentDragTargetAt(point);
    app.OnLeftButtonUpAt(0,point); app.FlushQueuedLayoutSave();
}
static GridCell Cell(const DesktopApp& app, const std::filesystem::path& path)
{
    for (const auto& item : app.items_) if (_wcsicmp(item.parsingName.c_str(),path.c_str()) == 0) return item.gridCell;
    throw std::runtime_error("file missing from actual host model");
}
static void PerturbCell(DesktopApp& app) { for (auto& item : app.items_) item.gridCell = {L"__page:1",0,2}; }
static std::vector<std::filesystem::path> CollectionPaths(const DesktopApp& app, const std::wstring& id)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& widget : app.widgets_) if (widget.id == id)
        for (const auto& key : widget.itemKeys)
            for (const auto& item : app.items_) if (item.layoutKey == key) paths.emplace_back(item.parsingName);
    return paths;
}
static ComPtr<IDataObject> ShellSource(DesktopApp& app, const std::filesystem::path& path)
{ return app.CreateFileDropDataObject({path.wstring()}); }
static ComPtr<OleDragDropAdapter> Adapter(DesktopApp& app)
{ ComPtr<OleDragDropAdapter> value; value.Attach(new OleDragDropAdapter(&app)); return value; }
static POINTL OlePoint(POINT point)
{
    if (!ClientToScreen(completionWindow,&point)) throw std::runtime_error("convert owned OLE point");
    return {point.x,point.y};
}
static bool SubmitFiles(DesktopApp& app, const std::vector<std::filesystem::path>& paths,
    const std::wstring& target, POINT point, int mods, DesktopApp::FileOperationCompletion done,
    const std::shared_ptr<snowdesktop::ShellFileOperationResult>& outputs)
{
    DragSourceList sources; sources.hasExternalFiles = true;
    for (const auto& path : paths) {
        DragSourceEntry entry; entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sources.entries.size(); entry.filePath = path.wstring();
        entry.displayName = path.filename().wstring(); sources.entries.push_back(std::move(entry));
    }
    Container* destination = target.empty() ? static_cast<Container*>(app.GetDesktopGrid()) : MemberContainer(app,target);
    const auto preview = app.BuildDropPreviewList(sources,destination,nullptr,HitRegion::Empty,mods,point);
    if (preview.Empty()) throw std::runtime_error("real file preview rejected fixture input");
    return app.ExecuteFileBackedDropPlan(sources,preview,std::move(done),false,outputs);
}
static bool FileOperationsIdle(const DesktopApp& app) { return app.shellFileOperationInFlight_ == 0; }
static bool LayoutSaved(const DesktopApp& app) { return !app.layoutSavePending_; }
static bool QueuePartial(DesktopApp& app, snowdesktop::ShellFileOperationRequest request,
    DesktopApp::FileOperationCompletion done)
{ return app.QueueShellFileOperation(std::move(request),std::move(done)); }
