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
        app.dockWindowPreview_ = std::make_unique<DockWindowPreview>();
        if (!app.dockWindowPreview_->Initialize(GetModuleHandleW(nullptr), [](HWND) {},
                [&app, legacyRoute](HWND window) {
                    // Restore each actual old dismissal route independently.
                    if (legacyRoute == 1) app.dockWindowPreview_->Hide();
                    if (legacyRoute == 2) app.DismissDockWindowPreviewUntilLeave();
                    app.CloseDockWindowFromPreview(window);
                })) throw std::runtime_error("initialize owned close-preview fixture");
        app.dockWindowPreviewKey_ = L"close-preview-fixture";
        app.dockWindowPreviewHover_.MarkPreviewShown(app.dockWindowPreviewKey_);
        app.dockWindowPreviewAnchorScreen_ = {0, 0, 20, 20};
        app.dockWindowPreview_->Show(items, app.dockWindowPreviewAnchorScreen_,
            DockPosition::Bottom, false);
        // Only test-owned nonactivating popups are moved; no desktop host exists.
        const RECT bounds = app.dockWindowPreview_->GetBounds();
        for (const HWND popup : app.dockWindowPreview_->GetWindows())
        {
            RECT rect{};
            GetWindowRect(popup, &rect);
            SetWindowPos(popup, nullptr, -12000 + rect.left - bounds.left,
                -12000 + rect.top - bounds.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return *app.dockWindowPreview_;
    }
    static bool ClosePreviewSuppressed(DesktopApp& app)
    {
        return !app.dockWindowPreviewHover_.SuppressedTarget().empty();
    }
#include "native_running_refresh_access.inc"
