// Only fixture windows receive native messages; never run the desktop host.
static void ResetPopupDragModel(DesktopApp& app, const std::filesystem::path& file, int source)
{
    app.items_oo_.clear();
    app.containers_.clear();
    app.widgets_.clear();
    PopulateModel(app, {file}, source);
}
static LRESULT CALLBACK PopupDragWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app && (message == WM_CANCELMODE || message == WM_CAPTURECHANGED))
        return app->HandleFloatingPopupMessage(window, message, wp, lp);
    return DefWindowProcW(window, message, wp, lp);
}
static void BindPopupDragWindow(DesktopApp& app, HWND window)
{
    app.floatingPopupHwnd_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PopupDragWindowProc));
}
static void CloseDragPopup(DesktopApp& app)
{
    app.popupWidgetIndex_ = 0;
    app.popupAnimation_.ShowImmediately();
    app.CloseCollectionPopup();
}
static bool DragSelected(const DesktopApp& app)
{
    return !app.items_.empty() && app.items_.front().selected;
}
static bool DragActive(const DesktopApp& app) { return app.dragSession_.IsActive(); }
static void UnbindPopupDragWindow(DesktopApp& app)
{
    if (app.floatingPopupHwnd_)
        SetWindowLongPtrW(app.floatingPopupHwnd_, GWLP_USERDATA, 0);
    app.floatingPopupHwnd_ = nullptr;
}
static bool PreparePreviewResources(DesktopApp& app)
{
    if (!app.dcompDevice_ && FAILED(app.InitGraphicsDevices())) return false;
    app.instance_ = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.hInstance = app.instance_;
    cls.lpfnWndProc = DesktopApp::DragPreviewWndProc;
    cls.lpszClassName = kDragPreviewWindowClassName;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return app.CreateDragPreviewWindow() &&
        SUCCEEDED(app.CreateOrResizeDragPreviewCompositionSurface(96, 96));
}
static bool HasPreviewSurface(const DesktopApp& app) { return !!app.dragPreviewDcompSurface_; }
static bool FinishPreviewDrag(DesktopApp& app)
{
    app.EndDragSession();
    return app.FlushPendingCompositionCommit() &&
        SUCCEEDED(app.dcompDevice_->WaitForCommitCompletion());
}
static void CleanPreviewResources(DesktopApp& app)
{
    app.DestroyDragPreviewWindow();
    if (app.dcompDevice_) app.dcompDevice_->Commit();
    app.dcompDevice_.Reset(); app.d2dContext_.Reset(); app.d2dDevice_.Reset();
    app.d2dFactory_.Reset(); app.d3dImmediateContext_.Reset(); app.d3dDevice_.Reset();
    app.instance_ = nullptr;
}
static HWND PreviewWindow(const DesktopApp& app) { return app.dragPreviewHwnd_; }
static WidgetContainer* PreparePaddingWidget(DesktopApp& app,
    const std::vector<std::filesystem::path>& files, bool scrolling, bool list)
{
    app.items_oo_.clear(); app.containers_.clear(); app.widgets_.clear();
    PopulateModel(app, files, 1);
    app.ClearSelection();
    auto* widget = MemberContainer(app, L"native-origin");
    auto* data = widget->GetWidgetData();
    data->gridSpan = {3, 3};
    data->scrollContainerMode = scrolling;
    data->listMode = list;
    data->listColumns = 1;
    const RECT frame{100, 100, 700, 640};
    widget->SetHostedFrame(&frame);
    widget->InvalidateSlots();
    return widget;
}
static RECT PreparePaddingPopup(DesktopApp& app,
    const std::vector<std::filesystem::path>& files, bool folder, bool list)
{
    PreparePaddingWidget(app, files, true, list);
    Select(app, {files.front()});
    BeginMembers(app, L"native-origin");
    app.gridPages_.front().workArea = {0, 0, 1200, 900};
    app.popupWidgetIndex_ = 0;
    app.popupAnchoredToDock_ = false;
    app.popupScrollOffset_ = 0;
    app.dockFolderPopupOpen_ = folder;
    if (folder)
    {
        app.dockFolderPopupWidget_ = app.widgets_.front();
        auto& data = app.dockFolderPopupWidget_;
        data.type = DesktopWidgetType::FolderMapping;
        data.itemKeys.clear();
        data.folderEntries.clear();
        for (const auto& file : files)
        {
            FolderEntry entry;
            entry.fullPath = file.wstring();
            entry.name = file.filename().wstring();
            data.folderEntries.push_back(std::move(entry));
        }
        app.dockFolderPopupContainer_ = std::make_unique<FolderMapping>(&data, &app);
    }
    app.popupAnimation_.ShowImmediately();
    app.popupRect_ = app.GetCollectionPopupRect(*app.GetOpenPopupWidget());
    return app.popupRect_;
}
static RECT PaddingPopupContent(DesktopApp& app, RECT popup)
{ return app.GetCollectionPopupContentRect(popup); }
static RECT PaddingPopupItem(DesktopApp& app, RECT popup, size_t index)
{ return app.GetCollectionPopupItemRect(popup, index); }
static int PaddingPopupColumns(DesktopApp& app, RECT popup)
{ return app.GetCollectionPopupColumnCount(popup); }
static DragTargetResolution HitPaddingPopup(DesktopApp& app, POINT point)
{
    DragTargetResolution result;
    app.HitTestPopupForDrag(point, result.container, result.slot, result.region);
    return result;
}
static void ResetPaddingPopup(DesktopApp& app)
{
    app.EndDragSession();
    app.ClearPopupDragTarget();
    app.dockFolderPopupContainer_.reset();
    app.dockFolderPopupOpen_ = false;
    app.popupWidgetIndex_ = static_cast<size_t>(-1);
    app.popupAnimation_.ResetHidden();
}
static void ApplyPreviewLayers(DesktopApp& app, HWND hint)
{
    app.hintHwnd_ = hint;
    app.ApplyDragPreviewLayerPolicy();
    app.hintHwnd_ = nullptr;
}
