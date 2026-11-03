namespace behavior
{
void DragPadding(HostProfile& profile)
{
    std::vector<std::filesystem::path> files;
    for (int i = 0; i < 6; ++i)
    {
        files.push_back(profile.desktop / (L"padding-" + std::to_wstring(i) + L".txt"));
        Write(files.back(), "padding-source-intact");
    }
    std::string failures;
    const auto check = [&](bool ok, const std::string& message) {
        if (!ok) { std::cerr << "padding: " << message << '\n'; failures += message + "; "; }
    };
    // Invoke real container/popup hit routing. Expected positions follow the
    // visible row order, independently of the distance implementation.
    for (int mode = 0; mode < 3; ++mode)
    {
        auto* widget = Access::PreparePaddingWidget(*profile.app, files, mode != 0, mode == 2);
        const auto& slots = widget->GetSlots();
        Require(slots.size() >= 3, "padding fixture exposes multiple real items");
        const RECT frame = widget->GetFrameRect();
        const bool list = mode == 2;
        const size_t first = list ? 1 : 0;
        size_t last = first;
        const RECT row = slots[first]->GetBounds();
        if (!list) while (last + 1 < slots.size() && slots[last + 1]->GetBounds().top == row.top) ++last;
        for (bool right : {false, true})
        {
            const POINT point{right ? frame.right - 1 : frame.left + 1,
                list ? row.top + (row.bottom - row.top) / 4 : (row.top + row.bottom) / 2};
            Slot* slot = nullptr;
            const auto region = widget->HitTestDrag(point, slot);
            const size_t expected = list ? first : right ? last + 1 : first;
            check(slot && (region == HitRegion::SortBefore || region == HitRegion::SortAfter) &&
                widget->GetDropInsertIndex(slot, region) == expected,
                "widget mode " + std::to_string(mode) + (right ? " right" : " left") + " selects nearest row insertion");
        }
        const RECT viewport = widget->GetContentViewportRect();
        Slot* terminal = nullptr;
        const auto terminalRegion = widget->HitTestDrag(
            {frame.right - 1, viewport.bottom - 1}, terminal);
        check(terminal && widget->GetDropInsertIndex(terminal, terminalRegion) == files.size(),
            "widget mode " + std::to_string(mode) + " distant blank retains a drawable final insertion");
    }
    for (bool folder : {false, true}) for (bool list : {false, true})
    {
        const RECT popup = Access::PreparePaddingPopup(*profile.app, files, folder, list);
        const RECT content = Access::PaddingPopupContent(*profile.app, popup);
        const size_t columns = static_cast<size_t>(Access::PaddingPopupColumns(*profile.app, popup));
        const size_t first = list ? 1 : 0;
        const RECT row = Access::PaddingPopupItem(*profile.app, popup, first);
        for (bool right : {false, true})
        {
            const POINT point{right ? popup.right - 1 : popup.left + 1,
                list ? row.top + (row.bottom - row.top) / 4 : (row.top + row.bottom) / 2};
            Require(point.y >= content.top && point.y < content.bottom, "popup padding fixture lies within visible row");
            const auto hit = Access::HitPaddingPopup(*profile.app, point);
            const size_t expected = list ? first : right ? std::min(columns, files.size()) : first;
            check(hit.slot && (hit.region == HitRegion::SortBefore || hit.region == HitRegion::SortAfter) &&
                hit.slot->GetIndex() + (hit.region == HitRegion::SortAfter ? 1 : 0) == expected,
                std::string(folder ? "folder popup" : "collection popup") + (list ? " list" : " grid") +
                    (right ? " right" : " left") + " selects nearest row insertion");
        }
        const auto header = Access::HitPaddingPopup(*profile.app, {popup.left + 1, popup.top + 1});
        check(!header.slot, "popup header remains outside item insertion targets");
        Access::ResetPaddingPopup(*profile.app);
    }
    Require(failures.empty(), failures);
}
void DragPopup()
{
    HostProfile profile;
    DragPadding(profile);
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
