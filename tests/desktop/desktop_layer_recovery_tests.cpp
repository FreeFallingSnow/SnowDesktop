#include "desktop/desktop_layer_recovery.h"

#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace layer = snowdesktop::desktop_layer_recovery;

namespace
{
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

struct RenderState
{
    unsigned placements = 0;
    bool blockReorder = false;
};

LRESULT CALLBACK RenderProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* state = reinterpret_cast<RenderState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state && message == WM_WINDOWPOSCHANGING && state->blockReorder)
        reinterpret_cast<WINDOWPOS*>(lp)->flags |= SWP_NOZORDER;
    if (state && message == WM_WINDOWPOSCHANGED) ++state->placements;
    return DefWindowProcW(window, message, wp, lp);
}

struct Fixture
{
    RenderState state;
    HWND host = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"private-desktop-host",
        WS_POPUP | WS_VISIBLE, 0, 0, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND view = CreateWindowExW(0, L"STATIC", L"native-view",
        WS_CHILD | WS_VISIBLE, 0, 0, 200, 200, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = CreateWindowExW(0, L"STATIC", L"native-icon-list",
        WS_CHILD | WS_VISIBLE, 0, 0, 200, 200, view, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND render = CreateWindowExW(WS_EX_NOACTIVATE, L"SnowDesktop.LayerRecoveryFixture", L"renderer",
        WS_CHILD | WS_VISIBLE, 0, 0, 200, 200, host, nullptr, GetModuleHandleW(nullptr), &state);
    HWND companion = CreateWindowExW(0, L"STATIC", L"independent-sibling",
        WS_CHILD | WS_VISIBLE, 150, 150, 20, 20, host, nullptr, GetModuleHandleW(nullptr), nullptr);

    ~Fixture() { if (host) DestroyWindow(host); }
    bool Valid() const { return host && view && list && render && companion; }
    void Drift() const
    {
        ShowWindow(list, SW_HIDE);
        SetWindowPos(view, HWND_TOP, 0, 0, 200, 200, SWP_NOACTIVATE);
        SetWindowPos(companion, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    HWND Hit() const
    {
        return ChildWindowFromPointEx(host, POINT{10, 10},
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    }
    layer::Result Recover(layer::Conditions conditions = {true, true, false}) const
    {
        return layer::TryRestore(render, host, view, conditions);
    }
};

void RunCases(bool withoutRecovery)
{
    Fixture fixture;
    Check(fixture.Valid(), "create isolated parent, view, hidden-list and render windows");
    if (!fixture.Valid()) return;
    fixture.Drift();
    Check(!IsWindowVisible(fixture.list) && IsWindowVisible(fixture.view) &&
            fixture.Hit() == fixture.view,
        "a raised native parent still owns hit testing after its icon list is hidden");
    if (withoutRecovery)
    {
        // The old host watch never repairs a valid same-parent hierarchy. This
        // isolated negative control omits that one recovery step, retaining the
        // same native windows, input point and required business result.
        Check(fixture.Hit() == fixture.render,
            "desktop input must reach the renderer after native-view drift");
        return;
    }

    RECT before{}, after{};
    GetWindowRect(fixture.render, &before);
    const HWND foreground = GetForegroundWindow();
    const auto recovered = fixture.Recover();
    GetWindowRect(fixture.render, &after);
    Check(recovered.status == layer::Status::Restored && fixture.Hit() == fixture.render,
        "production recovery restores native child hit testing to the renderer");
    Check(layer::IsAbove(fixture.companion, fixture.render) &&
            layer::IsAbove(fixture.render, fixture.view),
        "recovery crosses only the native view and preserves the preceding sibling");
    Check(EqualRect(&before, &after) && GetParent(fixture.render) == fixture.host &&
            IsWindowVisible(fixture.render) && IsWindowVisible(fixture.view) &&
            !IsWindowVisible(fixture.list) && GetForegroundWindow() == foreground,
        "repair preserves bounds, parents, visibility, native icon state and foreground");
    const auto placements = fixture.state.placements;
    Check(fixture.Recover().status == layer::Status::Unchanged &&
            fixture.state.placements == placements,
        "an already healthy hierarchy does not repeat native placement calls");

    for (const auto conditions : {layer::Conditions{false, true, false},
             layer::Conditions{true, false, false}, layer::Conditions{true, true, true}})
    {
        fixture.Drift();
        Check(fixture.Recover(conditions).status == layer::Status::Deferred &&
                fixture.Hit() == fixture.view,
            "native mode, pending presentation and active interactions defer recovery");
    }
    fixture.Drift();
    SetCapture(fixture.render);
    Check(GetCapture() == fixture.render, "establish real private-desktop pointer capture");
    Check(fixture.Recover().status == layer::Status::Deferred &&
            GetCapture() == fixture.render && layer::IsAbove(fixture.view, fixture.render),
        "pointer capture is retained and native reordering waits for its release");
    ReleaseCapture();
    Check(fixture.Recover().status == layer::Status::Restored,
        "the next poll repairs the same hierarchy after the gesture ends");

    fixture.Drift();
    ShowWindow(fixture.view, SW_HIDE);
    Check(fixture.Recover().status == layer::Status::Unchanged,
        "a hidden native view does not trigger recovery");
    ShowWindow(fixture.view, SW_SHOWNOACTIVATE);
    fixture.Drift();
    SetWindowPos(fixture.view, nullptr, 250, 250, 200, 200, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(fixture.Recover().status == layer::Status::Unchanged,
        "a nonoverlapping sibling does not trigger recovery");
    fixture.Drift();
    ShowWindow(fixture.render, SW_HIDE);
    Check(fixture.Recover().status == layer::Status::Unchanged,
        "recovery never revives a hidden renderer");
    ShowWindow(fixture.render, SW_SHOWNOACTIVATE);

    fixture.Drift();
    Check(layer::TryRestore(fixture.render, fixture.host, nullptr, {true, true, false}).status ==
            layer::Status::Unchanged && layer::IsAbove(fixture.view, fixture.render),
        "missing native windows are left to existing host recreation");
    const HWND otherParent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"other-parent",
        WS_POPUP | WS_VISIBLE, 0, 0, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(otherParent != nullptr, "create an independent host boundary");
    if (otherParent)
    {
        SetParent(fixture.view, otherParent);
        Check(fixture.Recover().status == layer::Status::Unchanged &&
                GetParent(fixture.view) == otherParent,
            "the repair cannot move a view from another parent");
        SetParent(fixture.view, fixture.host);
        DestroyWindow(otherParent);
    }
    fixture.Drift();
    fixture.state.blockReorder = true;
    Check(fixture.Recover().status == layer::Status::Failed && fixture.Hit() == fixture.view,
        "a successful API return without changed input order is not restoration success");
    fixture.state.blockReorder = false;
    Check(fixture.Recover().status == layer::Status::Restored && fixture.Hit() == fixture.render,
        "a declined reorder remains recoverable on the following poll");
}
}

int main(int argc, char** argv)
{
    const bool withoutRecovery = argc == 2 && std::string_view(argv[1]) == "--without-recovery";
    if (argc != 1 && !withoutRecovery) return 2;
    std::thread worker([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        const auto name = L"SnowDesktop.LayerRecovery." + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const bool attached = desktop && SetThreadDesktop(desktop);
        Check(attached, "use a private Windows desktop without touching Explorer or user input");
        if (attached)
        {
            WNDCLASSW definition{};
            definition.lpfnWndProc = RenderProcedure;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.lpszClassName = L"SnowDesktop.LayerRecoveryFixture";
            const ATOM registered = RegisterClassW(&definition);
            Check(registered != 0, "register the private renderer procedure");
            if (registered) RunCases(withoutRecovery);
            if (registered) UnregisterClassW(definition.lpszClassName, definition.hInstance);
            Check(SetThreadDesktop(original) != FALSE, "detach the private desktop after fixture cleanup");
        }
        if (desktop) Check(CloseDesktop(desktop) != FALSE, "close the isolated Windows desktop");
    });
    worker.join();
    std::cout << "desktop layer recovery failures=" << failures << '\n';
    return failures ? 1 : 0;
}
