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
#include "native_running_refresh_access.inc"
