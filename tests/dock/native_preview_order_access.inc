    static void PreparePreviewOrder(DesktopApp& app)
    {
        app.generalSettings_.dockEnabled = true;
        // Defer unrelated icon/process discovery through the real input guard.
        // WinEvent delivery and preview enumeration remain production code.
        app.mouseDown_ = true;
    }
    static void NotifyPreviewWindow(DesktopApp& app, HWND window, DWORD event)
    {
        const HWND previous = DesktopApp::dockForegroundNotificationWindow_.exchange(completionWindow);
        DesktopApp::DockForegroundWinEventProc(nullptr, event, window, OBJID_WINDOW, CHILDID_SELF, 0, GetTickCount());
        DesktopApp::dockForegroundNotificationWindow_.store(previous);
        MSG message{};
        while (PeekMessageW(&message, completionWindow, kDockWindowListChangedMessage,
                kDockWindowListChangedMessage, PM_REMOVE))
            app.HandleMessage(message.hwnd, message.message, message.wParam, message.lParam);
    }
    static std::vector<DockWindowPreviewItem> PreviewItems(DesktopApp& app, const std::wstring& executable)
    {
        return app.CollectDockWindowPreviewItems(app.ReadDockAppIdentity(executable));
    }
    static DockWindowPreview& PrepareClosePreview(DesktopApp& app,
        const std::vector<DockWindowPreviewItem>& items, int legacyRoute)
    {
        app.dockWindowPreview_ = std::make_unique<DockWindowPreview>(snowdesktop::dock_peek::Api{nullptr});
        if (!app.dockWindowPreview_->Initialize(GetModuleHandleW(nullptr), [](HWND) {},
                [&app, legacyRoute](HWND window) {
                    // Restore each actual old dismissal route independently.
                    if (legacyRoute == 1) app.dockWindowPreview_->Hide();
                    if (legacyRoute == 2) app.DismissDockWindowPreviewUntilLeave();
                    app.CloseDockWindowFromPreview(window);
                })) throw std::runtime_error("initialize owned close-preview fixture");
        app.dockWindowPreviewKey_ = L"close-preview-fixture";
        app.dockWindowPreviewHover_.MarkPreviewShown(app.dockWindowPreviewKey_);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
            throw std::runtime_error("resolve close-preview anchor monitor");
        const LONG x = (monitor.rcWork.left + monitor.rcWork.right) / 2;
        const LONG y = (monitor.rcWork.top + monitor.rcWork.bottom) / 2;
        app.dockWindowPreviewAnchorScreen_ = {x - 10, y - 10, x + 10, y + 10};
        app.dockWindowPreview_->Show(items, app.dockWindowPreviewAnchorScreen_,
            DockPosition::Bottom, false);
        return *app.dockWindowPreview_;
    }
    static bool ClosePreviewSuppressed(DesktopApp& app)
    {
        return !app.dockWindowPreviewHover_.SuppressedTarget().empty();
    }
    static void TrackLivePreview(DesktopApp& app, const std::wstring& executable)
    {
        app.dockWindowPreviewIdentity_ = app.ReadDockAppIdentity(executable);
    }
    static void EnableCloseRefresh(DesktopApp& app)
    {
        // Unlike the opening-order fixture, do not mask discovery with an
        // active-input guard when checking the actual close callback.
        app.mouseDown_ = false;
    }
    static void ReconcileCloseRefresh(DesktopApp& app)
    {
        app.RefreshDockRunningWindows();
    }
    static bool CloseRefreshReconciled(DesktopApp& app)
    {
        return app.dockRunningWindowsStateTick_ == DesktopApp::dockWindowListChangedTick_.load();
    }
    static void PruneCloseRefresh(DesktopApp& app)
    {
        app.PruneDockPendingCloseWindows();
    }
#include "native_running_refresh_access.inc"
