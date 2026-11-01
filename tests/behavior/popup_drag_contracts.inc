namespace behavior
{
void DragPopup()
{
    HostProfile profile;
    WindowOwner popup, hint;
    const auto createWindow = [](const wchar_t* title) {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", title, WS_POPUP, -30000, -30000, 64, 64,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    popup.handle = createWindow(L"Isolated drag popup");
    hint.handle = createWindow(L"Isolated drag hint");
    Require(popup.handle && hint.handle, "create isolated native surfaces");
    Access::BindPopupDragWindow(*profile.app, popup.handle);
    struct Cleanup
    {
        DesktopApp& app;
        ~Cleanup()
        {
            Access::CancelDrag(app);
            ReleaseCapture();
            Access::UnbindPopupDragWindow(app);
            Access::CleanPreviewResources(app);
        }
    } cleanup{*profile.app};
    auto& mode = snowdesktop::animation::detail::mode;
    const int previousMode = mode.exchange(snowdesktop::animation::Disabled);
    struct RestoreMode
    {
        int previous;
        ~RestoreMode() { snowdesktop::animation::detail::mode.store(previous); }
    } restoreMode{previousMode};
    const auto input = profile.desktop / L"popup-source.txt";
    Write(input, "popup-source-intact");
    Access::ResetPopupDragModel(*profile.app, input, 1);

    // Equivalent negative boundary: cancellation while the old popup still
    // owns capture really terminates the production gesture. Close must hand
    // capture over before that same native message can arrive.
    Access::Select(*profile.app, {input});
    Access::BeginMembers(*profile.app, L"native-origin");
    SetCapture(popup.handle);
    Require(GetCapture() == popup.handle, "source popup owns capture");
    SendMessageW(popup.handle, WM_CANCELMODE, 0, 0);
    Require(!Access::DragActive(*profile.app), "negative boundary reaches real drag cancellation");

    for (const auto& origin : {std::wstring{}, std::wstring{L"native-origin"}})
    {
        Access::ResetPopupDragModel(*profile.app, input, origin.empty() ? 0 : 1);
        if (origin.empty()) Access::AddCollection(*profile.app, L"target", 0);
        Access::Select(*profile.app, {input});
        Access::BeginMembers(*profile.app, origin);
        const auto captured = Access::Sources(*profile.app).FilePaths();
        ShowWindow(popup.handle, SW_SHOWNOACTIVATE);
        SetCapture(popup.handle);
        Access::CloseDragPopup(*profile.app);
        Require(GetCapture() == profile.window.handle, "shared close hands capture to surviving desktop host");
        SendMessageW(popup.handle, WM_CANCELMODE, 0, 0);
        Require(Access::DragActive(*profile.app), "outgoing popup cancellation preserves live drag");
        Require(Access::DragSelected(*profile.app), "popup switch preserves source selection");
        Require(Access::Sources(*profile.app).FilePaths() == captured, "popup switch preserves source identities");
        Access::CancelDrag(*profile.app);
    }

    // Use actual DComp surfaces. Ending each drag must relinquish its payload
    // surface, while the single reusable native presentation window survives.
    HWND retainedWindow = nullptr;
    Access::ResetPopupDragModel(*profile.app, input, 1);
    for (int cycle = 0; cycle < 32; ++cycle)
    {
        Access::Select(*profile.app, {input});
        Access::BeginMembers(*profile.app, L"native-origin");
        Require(Access::PreparePreviewResources(*profile.app), "allocate production preview surface");
        Require(Access::HasPreviewSurface(*profile.app), "preview owns a surface during drag");
        const HWND window = Access::PreviewWindow(*profile.app);
        if (!retainedWindow) retainedWindow = window;
        Require(window == retainedWindow, "repeated drags reuse one native preview window");
        Require(Access::FinishPreviewDrag(*profile.app), "submit surface retirement at the input frame boundary");
        Require(!Access::HasPreviewSurface(*profile.app), "ended drag releases its composition surface");
    }
    ShowWindow(retainedWindow, SW_SHOWNOACTIVATE);
    ShowWindow(popup.handle, SW_SHOWNOACTIVATE);
    ShowWindow(hint.handle, SW_SHOWNOACTIVATE);
    SetWindowPos(popup.handle, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Access::ApplyPreviewLayers(*profile.app, hint.handle);
    namespace order = snowdesktop::popup_window_pair_z_order;
    Require(order::IsTopmost(retainedWindow) && order::IsTopmost(hint.handle) &&
        order::IsAbove(hint.handle, retainedWindow) && order::IsAbove(retainedWindow, popup.handle),
        "popup promotion preserves hint, preview, popup order across bands");
    const HWND previous = GetWindow(retainedWindow, GW_HWNDPREV);
    for (int sample = 0; sample < 64; ++sample) Access::ApplyPreviewLayers(*profile.app, hint.handle);
    Require(GetWindow(retainedWindow, GW_HWNDPREV) == previous,
        "unchanged presentation retains its native order");
    Require(Read(input) == "popup-source-intact", "popup transitions leave source bytes intact");
}
}
