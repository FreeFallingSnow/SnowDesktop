#pragma once

#include "app/render/desktop_backdrop_compositor.h"
#include "layout/popup_round_geometry.h"
#include <MinHook.h>
#include <d2d1_1helper.h>
#include <atomic>
#include <initializer_list>

namespace snowdesktop::test::backdrop_region_cache
{
// Observe only this test thread; all ordinary GDI work remains real. Faults
// affect one allocation or this fixture's own hidden helper HWND, once.
inline std::atomic<DWORD> observedThread{0};
inline std::atomic<unsigned> allocations{0};
inline std::atomic<bool> failAllocation{false}, failPublication{false};
inline std::atomic<HWND> failureWindow{nullptr};
inline decltype(&::CreateRectRgn) originalCreateRectRgn = nullptr;
inline decltype(&::SetWindowRgn) originalSetWindowRgn = nullptr;

inline HRGN WINAPI ObserveCreateRectRgn(int left, int top, int right, int bottom)
{
    if (observedThread.load() == GetCurrentThreadId())
    {
        ++allocations;
        if (failAllocation.exchange(false))
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    }
    return originalCreateRectRgn(left, top, right, bottom);
}

inline int WINAPI ObserveSetWindowRgn(HWND window, HRGN region, BOOL redraw)
{
    if (observedThread.load() == GetCurrentThreadId() &&
        window == failureWindow.load() && region && failPublication.exchange(false))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return originalSetWindowRgn(window, region, redraw);
}

struct GdiObservation
{
    bool ownsInitialization = false, ownsAllocation = false, ownsPublication = false;
    bool active = false;
    GdiObservation()
    {
        const auto status = MH_Initialize();
        ownsInitialization = status == MH_OK;
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
        ownsAllocation = MH_CreateHook(reinterpret_cast<void*>(&::CreateRectRgn),
            reinterpret_cast<void*>(&ObserveCreateRectRgn),
            reinterpret_cast<void**>(&originalCreateRectRgn)) == MH_OK;
        if (!ownsAllocation) return;
        ownsPublication = MH_CreateHook(reinterpret_cast<void*>(&::SetWindowRgn),
            reinterpret_cast<void*>(&ObserveSetWindowRgn),
            reinterpret_cast<void**>(&originalSetWindowRgn)) == MH_OK;
        if (!ownsPublication) return;
        observedThread = GetCurrentThreadId();
        active = MH_EnableHook(reinterpret_cast<void*>(&::CreateRectRgn)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&::SetWindowRgn)) == MH_OK;
    }
    ~GdiObservation()
    {
        observedThread = 0;
        failAllocation = failPublication = false;
        failureWindow = nullptr;
        if (ownsPublication)
        {
            MH_DisableHook(reinterpret_cast<void*>(&::SetWindowRgn));
            MH_RemoveHook(reinterpret_cast<void*>(&::SetWindowRgn));
        }
        if (ownsAllocation)
        {
            MH_DisableHook(reinterpret_cast<void*>(&::CreateRectRgn));
            MH_RemoveHook(reinterpret_cast<void*>(&::CreateRectRgn));
        }
        if (ownsInitialization) MH_Uninitialize();
    }
};

struct Region
{
    HRGN handle = CreateRectRgn(0, 0, 0, 0);
    ~Region() { if (handle) DeleteObject(handle); }
};

struct Fence { RECT frame{}; float radius = 0; };

// Execute the pre-cache GDI union/subtraction as an independent output oracle.
inline bool Matches(HWND helper, std::initializer_list<Fence> fences, RECT occlusion = {})
{
    Region expected, actual;
    if (!expected.handle || !actual.handle) return false;
    for (const auto& fence : fences)
    {
        HRGN part = popup_round_geometry::CreateWindowFence(fence.frame, fence.radius);
        const bool ok = part && CombineRgn(expected.handle, expected.handle, part, RGN_OR) != ERROR;
        if (part) DeleteObject(part);
        if (!ok) return false;
    }
    if (!IsRectEmpty(&occlusion))
    {
        HRGN excluded = CreateRectRgnIndirect(&occlusion);
        const bool ok = excluded && CombineRgn(expected.handle, expected.handle, excluded, RGN_DIFF) != ERROR;
        if (excluded) DeleteObject(excluded);
        if (!ok) return false;
    }
    return GetWindowRgn(helper, actual.handle) != ERROR &&
        EqualRgn(expected.handle, actual.handle) != FALSE;
}

template<class Check, class FindBackdrop>
void CheckWindowRegionCache(Check&& check, FindBackdrop&& findBackdrop)
{
    GdiObservation observe;
    if (!check(observe.active, "observe real native region allocation and bounded failure recovery")) return;
    struct HiddenWindow
    {
        HWND handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"backdrop-region-cache-test", WS_POPUP,
            80, 80, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } content;
    DesktopBackdropCompositor glass;
    if (!check(content.handle && glass.InitializePopup(content.handle, false, false),
            "region-cache fixture initializes only its own hidden target")) return;
    HWND helper = findBackdrop(glass);
    if (!check(helper && glass.IsBackdropWindow(helper), "region-cache fixture identifies its own helper")) return;

    RECT first{16, 20, 144, 112}, second{176, 132, 300, 220};
    float radius = 18.75f;
    const auto paint = [&](bool complete = true) {
        glass.BeginFrame(complete);
        check(glass.AddPanel(first, radius, 24, 64001) &&
            glass.AddPanel(second, 12.5f, 24, 64002), "collect both region-cache fixture panels");
        glass.EndFrame(false);
    };
    paint();
    allocations = 0;
    check(Matches(helper, {{first, radius}, {second, 12.5f}}) && allocations.load() > 0,
        "uncached GDI negative control allocates regions and matches the native union exactly");
    for (bool complete : {true, false, true, false})
    {
        allocations = 0;
        paint(complete);
        check(allocations.load() == 0, "unchanged complete and partial paints allocate no window regions");
        check(Matches(helper, {{first, radius}, {second, 12.5f}}),
            "reused native region is pixel-identical to the uncached union");
    }
    check(glass.BeginStagedFrame() && glass.AddPanel(first, radius, 24, 64001) &&
        glass.AddPanel(second, 12.5f, 24, 64002), "stage an unchanged full native frame");
    allocations = 0;
    check(glass.ApplyStagedFrame() && allocations.load() == 0,
        "unchanged staged publication reuses the final region, without transient invalidations");
    check(glass.BeginStagedFrame() && glass.AddPanel({24, 24, 160, 120}, 0, 24, 64001),
        "collect a candidate that will be discarded");
    glass.SetOcclusionRect({40, 40, 120, 120});
    glass.DiscardStagedFrame();
    allocations = 0; glass.BeginFrame(false); glass.EndFrame(false);
    check(allocations.load() == 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
        "discarded geometry and occlusion leave the completed native region reusable");
    check(glass.SetPanelOpacity(first, 0) && glass.SetVisualOpacity(0),
        "temporarily hide retained visuals without retiring their fences");
    allocations = 0; glass.BeginFrame(false); glass.EndFrame(false);
    check(allocations.load() == 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
        "opacity-only changes preserve the original visibility-fence semantics");
    check(glass.SetVisualOpacity(1), "restore the fixture's ordinary visual opacity");
    paint();

    for (float scale : {1.25f, 1.5f, 2.f, 1.f})
    {
        first = {static_cast<LONG>(16 * scale), static_cast<LONG>(20 * scale),
            static_cast<LONG>(144 * scale), static_cast<LONG>(112 * scale)};
        radius = 18.75f * scale;
        allocations = 0; paint();
        check(allocations.load() > 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
            "physical-pixel size and fractional DPI-radius changes rebuild the exact old fence");
    }
    allocations = 0;
    glass.SetOcclusionRect({30, 30, 100, 160});
    check(allocations.load() > 0 && Matches(helper, {{first, radius}, {second, 12.5f}}, {30, 30, 100, 160}),
        "occlusion changes synchronously subtract the old GDI region");
    allocations = 0; paint(false);
    check(allocations.load() == 0 && Matches(helper, {{first, radius}, {second, 12.5f}}, {30, 30, 100, 160}),
        "unchanged guide occlusion survives partial paints without rebuilding");
    glass.SetOcclusionRect({});

    const auto translated = D2D1::Matrix4x4F::Translation(4, -4, 0);
    RECT projected{20, 16, 148, 108};
    check(glass.SetPanelTransform(64001, translated, projected), "update card projection outside paint");
    allocations = 0; glass.CommitVisualChanges();
    check(allocations.load() > 0 && Matches(helper, {{projected, radius}, {second, 12.5f}}),
        "card commits synchronize new projected bounds and preserve rounded translation");
    // AddPanel first restores the ordinary pose; the final transform is the
    // same as the last successful frame. Compare final inputs, not setters.
    glass.BeginFrame(true);
    check(glass.AddPanel(first, radius, 24, 64001) && glass.AddPanel(second, 12.5f, 24, 64002) &&
        glass.SetPanelTransform(64001, translated, projected), "collect the same final projected pose");
    allocations = 0; glass.EndFrame(false);
    check(allocations.load() == 0 && Matches(helper, {{projected, radius}, {second, 12.5f}}),
        "transient ordinary-to-projected setters do not rebuild an unchanged final fence");
    auto folded = translated;
    folded._11 = .8f; // Retain the same conservative projected fence.
    check(glass.SetPanelTransform(64001, folded, projected), "fold the card within identical projected bounds");
    allocations = 0; glass.CommitVisualChanges();
    check(allocations.load() > 0 && Matches(helper, {{projected, 0}, {second, 12.5f}}),
        "projection contour changes invalidate a geometrically equal rectangle");

    projected = {26, 24, 154, 116};
    failureWindow = helper;
    failPublication = true;
    check(glass.SetPanelTransform(64001, D2D1::Matrix4x4F::Translation(10, 4, 0), projected),
        "prepare a recoverable native publication failure");
    glass.CommitVisualChanges();
    check(!failPublication.load() && Matches(helper, {{{20, 16, 148, 108}, 0}, {second, 12.5f}}),
        "failed SetWindowRgn retains its old visibility fence and is not cached as success");
    allocations = 0; glass.CommitVisualChanges();
    check(allocations.load() > 0 && Matches(helper, {{projected, radius}, {second, 12.5f}}),
        "the next ordinary commit retries the same failed geometry synchronously");
    projected = {30, 28, 158, 120};
    check(glass.SetPanelTransform(64001, D2D1::Matrix4x4F::Translation(14, 8, 0), projected),
        "prepare a recoverable GDI allocation failure");
    failAllocation = true; glass.CommitVisualChanges();
    check(!failAllocation.load() && Matches(helper, {{{26, 24, 154, 116}, radius}, {second, 12.5f}}),
        "allocation failure also preserves the previous fence");
    allocations = 0; glass.CommitVisualChanges();
    check(allocations.load() > 0 && Matches(helper, {{projected, radius}, {second, 12.5f}}),
        "allocation failure remains retryable with identical inputs");
    paint();

    for (int mode : {0, 1, 2})
    {
        if (mode == 0) glass.SetVisualTranslation(8, 0);
        else if (mode == 1) glass.SetVisualTransform(.8f, 1.f, 160.f, 120.f);
        else check(glass.SetGenieTransform(first, {120, 220, 200, 236}, 3, .5f, 1.f),
            "enter the native genie animation using the existing panel");
        Region expanded;
        check(GetWindowRgn(helper, expanded.handle) == ERROR,
            "animation removes the native fence exactly as before");
        allocations = 0;
        if (mode == 0) glass.SetVisualTranslation(0, 0);
        else if (mode == 1) glass.SetVisualTransform(1.f, 1.f, 160.f, 120.f);
        else glass.ClearGenieTransform();
        check(allocations.load() > 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
            "animation exit restores the fence even when panel geometry stayed identical");
        allocations = 0; paint(false);
        check(allocations.load() == 0, "animation restoration establishes a reusable successful snapshot");
    }

    const UINT previousDpi = GetDpiForWindow(helper);
    check(SetWindowPos(content.handle, nullptr, -1200, -100, 320, 240,
        SWP_NOACTIVATE | SWP_NOZORDER), "move only the fixture window to negative screen coordinates");
    allocations = 0; paint(false);
    check((GetDpiForWindow(helper) != previousDpi ? allocations.load() > 0 : allocations.load() == 0) &&
        Matches(helper, {{first, radius}, {second, 12.5f}}),
        "window translation preserves local fence pixels, reusing only when DPI also matches");
    check(SetWindowPos(content.handle, nullptr, 0, 0, 420, 280,
        SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER), "resize only the fixture allocation");
    allocations = 0; paint(false);
    check(allocations.load() > 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
        "helper size changes conservatively recheck the native region");
    allocations = 0; paint(false);
    check(allocations.load() == 0, "unchanged resized allocations return to the fast path");

    glass.BeginFrame(true);
    check(glass.KeepPanel(first), "complete collection retains only its explicitly kept panel");
    glass.EndFrame(false);
    check(Matches(helper, {{first, radius}}), "unseen panel retirement updates the fence synchronously");
    check(glass.RemovePanel(first, 64001) && Matches(helper, {}),
        "explicit removal of the last panel publishes an empty fence outside paint");
    allocations = 0; glass.BeginFrame(false); glass.EndFrame(false);
    check(allocations.load() == 0, "the empty successful collection also reuses its fence");
    paint();
    check(glass.InitializePopup(content.handle, false, false), "recreate the target with identical content geometry");
    helper = findBackdrop(glass);
    allocations = 0; paint();
    check(allocations.load() > 0 && Matches(helper, {{first, radius}, {second, 12.5f}}),
        "resource recreation cannot inherit an old target's successful snapshot");

    IconBackdropClip clip{{32, 32, 96, 96}, {}};
    const auto iconPaint = [&] {
        glass.BeginFrame(true);
        check(glass.AddIconPanel(first, IconBeautifyShape::Circle, 24, 64003, &clip),
            "publish an icon with a real viewport clip");
        glass.EndFrame(false);
    };
    iconPaint();
    allocations = 0; iconPaint();
    check(allocations.load() == 0 && Matches(helper, {{clip.bounds, 0}}),
        "a retained clipped icon reuses its viewport-intersected native fence");
    clip.bounds = {48, 40, 100, 104};
    allocations = 0; iconPaint();
    check(allocations.load() > 0 && Matches(helper, {{clip.bounds, 0}}),
        "changing only the viewport invalidates the icon's final fence");
    clip.bounds = {200, 200, 240, 240};
    iconPaint();
    check(glass.PanelCount() == 0 && Matches(helper, {}),
        "an icon leaving the viewport retires its native region synchronously");
    check(!IsWindowVisible(content.handle) && !IsWindowVisible(helper),
        "region-cache regression never reveals or manipulates the desktop host");
}
} // namespace snowdesktop::test::backdrop_region_cache
