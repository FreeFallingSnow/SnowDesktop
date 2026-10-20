#pragma once

#include "dock/dock_window_rules.h"

#include <shobjidl.h>
#include <wrl/client.h>

namespace snowdesktop::tests::dock_task_window_filter
{

template <typename Check>
void Run(Check check, bool negativeControl = false)
{
    // Exercise the real Shell API used by Electron skipTaskbar windows. Only
    // our off-screen fixtures are modified; QQ and user windows are read-only.
    struct Apartment
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    check(SUCCEEDED(apartment.result) || apartment.result == RPC_E_CHANGED_MODE,
        "taskbar exclusion fixture initializes COM");
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return;

    Microsoft::WRL::ComPtr<ITaskbarList> taskbar;
    HRESULT result = CoCreateInstance(__uuidof(TaskbarList), nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
    if (SUCCEEDED(result)) result = taskbar->HrInit();
    check(SUCCEEDED(result), "taskbar exclusion fixture initializes ITaskbarList");
    if (FAILED(result)) return;

    struct Window
    {
        HWND handle = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"SnowDesktop taskbar exclusion fixture",
            WS_POPUP | WS_VISIBLE, -32000, -32000, 160, 90,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } flyout, sibling;
    check(flyout.handle && sibling.handle, "taskbar exclusion fixtures create live windows");
    if (!flyout.handle || !sibling.handle) return;

    const auto eligible = [negativeControl](HWND window) {
        // The negative control restores the old discovery input: ignore the
        // taskbar marker while keeping the same live HWND and Shell calls.
        return dock_window_rules::IsTaskWindowStyleEligible(
                GetWindowLongPtrW(window, GWL_EXSTYLE),
                GetWindow(window, GW_OWNER) != nullptr) &&
            dock_window_rules::IsTaskWindowPresentationEligible(
                IsWindowVisible(window) != FALSE, IsIconic(window) != FALSE,
                false, !negativeControl &&
                    dock_window_rules::IsTaskWindowExplicitlyExcluded(window));
    };
    const LONG_PTR originalStyle = GetWindowLongPtrW(flyout.handle, GWL_STYLE);
    const LONG_PTR originalExtendedStyle = GetWindowLongPtrW(flyout.handle, GWL_EXSTYLE);
    check(eligible(flyout.handle) && eligible(sibling.handle),
        "ordinary application windows start eligible for Dock discovery");

    result = taskbar->DeleteTab(flyout.handle);
    check(SUCCEEDED(result), "the real Shell removes the fixture taskbar button");
    if (FAILED(result)) return;
    check(dock_window_rules::IsTaskWindowExplicitlyExcluded(flyout.handle),
        "DeleteTab sets the explicit taskbar marker on a live window");
    check(IsWindowVisible(flyout.handle) &&
            GetWindowLongPtrW(flyout.handle, GWL_STYLE) == originalStyle &&
            GetWindowLongPtrW(flyout.handle, GWL_EXSTYLE) == originalExtendedStyle &&
            !GetWindow(flyout.handle, GW_OWNER),
        "taskbar exclusion leaves visibility, styles, and owner unchanged");
    check(!eligible(flyout.handle),
        "a visible explicitly excluded flyout must not become a Dock task even with APPWINDOW");
    check(eligible(sibling.handle),
        "excluding a flyout must preserve ordinary sibling windows of the same process");
    check(!dock_window_rules::IsTaskWindowPresentationEligible(true, true, false, true),
        "a minimized excluded flyout must not re-enter Dock discovery");
    check(!dock_window_rules::IsTaskbarDocumentProxyCandidateEligible(
            true, false, false, true, WS_POPUP | WS_CAPTION,
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE, true),
        "hidden taskbar document proxy discovery must also respect explicit exclusion");

    result = taskbar->AddTab(flyout.handle);
    check(SUCCEEDED(result), "the real Shell restores the same fixture taskbar button");
    if (FAILED(result)) return;
    check(!dock_window_rules::IsTaskWindowExplicitlyExcluded(flyout.handle) &&
            eligible(flyout.handle),
        "AddTab restores Dock eligibility on the same HWND without a stale exclusion cache");
    check(dock_window_rules::IsTaskWindowPresentationEligible(true, true),
        "ordinary minimized task windows remain available");
}

} // namespace snowdesktop::tests::dock_task_window_filter
