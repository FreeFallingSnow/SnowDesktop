#pragma once

#include "dock_preview_thumbnail_cases.h"
#include "dock/dock_window_peek.h"
#include "theme/personalization.h"

#include <chrono>
#include <thread>

// Exercise the actual preview HWND message/timer/click/teardown paths. Only
// the private DWM call and delivery of synthetic pointer/key/foreground input
// are replaced. No SnowDesktop desktop host or real system Peek is operated.
namespace dock_window_peek_cases
{
struct Call
{
    bool active;
    HWND target;
    HWND caller;
};
inline std::vector<Call> calls;
inline bool failBegin = false;
inline bool failStop = false;
inline bool escape = false;
inline HWND foreground = nullptr;
inline HWND pointerWindow = nullptr;
inline POINT pointerPosition{-32000, -32000};
inline BOOL WINAPI Cursor(POINT* point) { *point = pointerPosition; return TRUE; }
inline HWND WINAPI WindowAtPointer(POINT) { return pointerWindow; }
inline LRESULT SendPointer(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    // Native reflow now samples the pointer again after posted dispatch. Keep
    // that OS boundary consistent with the mouse input this fixture supplies.
    if (message == WM_MOUSELEAVE)
    {
        if (pointerWindow == window) { pointerWindow = nullptr; pointerPosition = {-32000, -32000}; }
    }
    else if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
    {
        pointerWindow = window;
        pointerPosition = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(window, &pointerPosition);
    }
    return SendMessageW(window, message, wp, lp);
}

inline HRESULT WINAPI Activate(BOOL active, HWND target, HWND caller, UINT trigger, const RECT* rect)
{
    if (trigger != 3 || rect) return E_INVALIDARG;
    calls.push_back({active != FALSE, target, caller});
    return (active ? failBegin : failStop) ? E_FAIL : S_OK;
}
inline BOOL WINAPI TrackPointer(TRACKMOUSEEVENT*) { return TRUE; }
inline SHORT WINAPI KeyState(int key) { return key == VK_ESCAPE && escape ? static_cast<SHORT>(-32768) : 0; }
inline HWND WINAPI Foreground() { return foreground; }

template<class Predicate>
bool Wait(Predicate predicate, DWORD milliseconds = 1600)
{
    const auto end = GetTickCount64() + milliseconds;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (GetTickCount64() < end);
    return predicate();
}

inline bool Peeking(HWND window)
{
    return !calls.empty() && calls.back().active && calls.back().target == window;
}

inline LPARAM ContentPoint(HWND popup)
{
    RECT card{};
    if (!GetClientRect(popup, &card) || IsRectEmpty(&card))
        throw std::runtime_error("Peek input requires a live card");
    return MAKELPARAM(card.right / 2, card.bottom * 2 / 3);
}

template<class Check>
void Run(Check report)
{
    try
    {
        const auto check = [&](bool value, const char* message) {
            report(value, message);
            if (!value) throw std::runtime_error(message);
        };
        using dock_preview_thumbnail_cases::ImportPatch;
        using dock_preview_thumbnail_cases::Source;
        using snowdesktop::dock_peek::Session;
        calls.clear();
        failBegin = failStop = escape = false;
        Source first, second;
        check(first.window && second.window, "Peek fixtures own two real source HWNDs");
        ShowWindow(first.window, SW_SHOWNOACTIVATE);
        ShowWindow(second.window, SW_SHOWMINNOACTIVE);
        const HWND actualForeground = GetForegroundWindow();
        foreground = actualForeground;
        pointerWindow = nullptr;
        pointerPosition = {-32000, -32000};
        ImportPatch pointer("TrackMouseEvent", reinterpret_cast<ULONG_PTR>(&TrackPointer));
        ImportPatch cursor("GetCursorPos", reinterpret_cast<ULONG_PTR>(&Cursor));
        ImportPatch hit("WindowFromPoint", reinterpret_cast<ULONG_PTR>(&WindowAtPointer));
        ImportPatch keys("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&KeyState));
        ImportPatch focus("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&Foreground));
        const auto nativeForeground = reinterpret_cast<HWND(WINAPI*)()>(focus.original);

        {
            Session unavailable(snowdesktop::dock_peek::Api{nullptr});
            check(!unavailable.Begin(first.window, nullptr) && unavailable.Stop() && calls.empty(),
                "missing private API neither invokes Peek nor changes source state");
            Session session(snowdesktop::dock_peek::Api{&Activate});
            check(session.Begin(second.window, first.window) && IsIconic(second.window),
                "a minimized source can be submitted without restoring it");
            check(session.Begin(first.window, second.window) && Peeking(first.window),
                "switching Peek submits the new concrete HWND");
            failStop = true;
            check(!session.Stop() && session.Active(), "failed Peek cleanup retains ownership for retry");
            failStop = false;
            check(session.Stop() && !session.Active(), "cleanup can retry without losing the old session");
        }
        const auto sourceHasFocus = [&] {
            const HWND current = nativeForeground();
            return current == first.window || current == second.window;
        };
        check(!sourceHasFocus() && IsIconic(second.window),
            "session operations never activate an owned source or restore its minimization");

        calls.clear();
        HWND activated = nullptr, closed = nullptr;
        HWND activatedFromPeek = nullptr;
        bool endedBeforeAction = false;
        bool peekHeldForHandoff = false;
        {
            DockWindowPreview preview(snowdesktop::dock_peek::Api{&Activate});
            preview.SetPeekActivateCallback([&](HWND window) {
                activatedFromPeek = window;
                peekHeldForHandoff = Peeking(window);
                // The foreign process has accepted the request but has not yet
                // processed its asynchronous restore/foreground handoff.
            });
            check(preview.Initialize(GetModuleHandleW(nullptr), [&](HWND window) {
                activated = window;
                endedBeforeAction = !calls.empty() && !calls.back().active;
            }, [&](HWND window) {
                closed = window;
                endedBeforeAction = !calls.empty() && !calls.back().active;
            }), "Peek production preview initializes");
            const auto show = [&] {
                preview.Show({{first.window, L"first"}, {second.window, L"second"}},
                    {0, 0, 30, 30}, DockPosition::Bottom, false);
                for (HWND popup : preview.GetWindows())
                    SetWindowPos(popup, nullptr, -16000, -16000, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            };
            const auto hover = [&](size_t index) {
                RECT card{};
                const HWND popup = preview.GetWindows().at(index);
                GetClientRect(popup, &card);
                SendPointer(popup, WM_MOUSEMOVE, 0,
                    MAKELPARAM(card.right / 2, card.bottom * 2 / 3));
            };
            const auto actionPoint = [&](bool pin) {
                RECT card{};
                GetClientRect(preview.GetWindow(), &card);
                const UINT dpi = GetDpiForWindow(preview.GetWindow());
                const RECT action = pin ? CalculateDockWindowPreviewPinButtonRect(card, dpi)
                    : CalculateDockWindowPreviewCloseButtonRect(card, dpi);
                return POINT{(action.left + action.right) / 2, (action.top + action.bottom) / 2};
            };
            show();
            hover(0);
            Wait([] { return false; }, 50);
            check(calls.empty(), "crossing a thumbnail briefly does not invoke Peek");
            check(Wait([&] { return Peeking(first.window); }),
                "resting on the thumbnail invokes Peek through the real popup timer");
            check(calls.back().caller == nullptr,
                "desktop Peek does not depend on a thumbnail popup that will hide during handoff");
            const size_t firstCalls = calls.size();
            hover(0);
            check(calls.size() == firstCalls, "movement within one thumbnail does not restart Peek");
            hover(1);
            check(Peeking(second.window) && IsIconic(second.window),
                "moving to a second thumbnail switches immediately without restoring its source");
            SendPointer(preview.GetWindows()[0], WM_MOUSELEAVE, 0, 0);
            check(Peeking(second.window), "a late leave from the previous popup cannot cancel its successor");
            const auto& popups = preview.GetWindows();
            check(!activated && !closed && !sourceHasFocus() &&
                std::find(popups.begin(), popups.end(), nativeForeground()) == popups.end(),
                "hover and switching never invoke click callbacks or give an owned source or popup foreground");
            SendPointer(preview.GetWindows()[1], WM_MOUSELEAVE, 0, 0);
            check(Wait([&] { return !calls.back().active; }), "leaving the active thumbnail restores the desktop");
            show();
            hover(1);
            check(Wait([&] { return Peeking(second.window); }), "returning to the panel starts a fresh Peek");

            const POINT pin = actionPoint(true);
            SendPointer(preview.GetWindow(), WM_MOUSEMOVE, 0, MAKELPARAM(pin.x, pin.y));
            check(!calls.back().active, "hovering the pin ends the full-window Peek");
            const size_t pinCalls = calls.size();
            Wait([] { return false; }, 450);
            check(calls.size() == pinCalls, "a pin hover cannot schedule another Peek");
            const POINT close = actionPoint(false);
            SendPointer(preview.GetWindow(), WM_MOUSEMOVE, 0, MAKELPARAM(close.x, close.y));
            Wait([] { return false; }, 450);
            check(calls.size() == pinCalls, "a close-button hover cannot schedule Peek");

            hover(0);
            check(Wait([&] { return Peeking(first.window); }), "reentering the content begins a new Peek");
            escape = true;
            check(Wait([&] { return !calls.back().active; }), "Escape ends Peek without popup keyboard focus");
            escape = false;
            const size_t escapedCalls = calls.size();
            hover(0);
            Wait([] { return false; }, 450);
            check(calls.size() == escapedCalls, "Escape suppresses the same thumbnail until its pointer leaves");
            hover(1);
            check(Wait([&] { return Peeking(second.window); }), "another thumbnail can Peek after Escape");
            foreground = first.window;
            check(Wait([&] { return !calls.back().active; }), "an external foreground change dismisses Peek");
            foreground = actualForeground;

            preview.Hide();
            check(preview.IsCleared(), "hiding clears pending/active Peek and its timer");
            calls.clear();
            show();
            hover(0);
            preview.Hide();
            Wait([] { return false; }, 450);
            check(calls.empty(), "hiding during the delay prevents a stale timer from beginning Peek");

            show();
            hover(0);
            foreground = first.window;
            Wait([] { return false; }, 450);
            check(calls.empty(), "a foreground change during the delay cancels the pending Peek");
            foreground = actualForeground;
            preview.Hide();
            show();
            failBegin = true;
            hover(0);
            check(Wait([&] { return !calls.empty(); }) && preview.IsVisible(),
                "rejected private API leaves the ordinary thumbnail panel available");
            failBegin = false;
            const size_t rejectedCalls = calls.size();
            hover(0);
            Wait([] { return false; }, 450);
            check(calls.size() == rejectedCalls, "a rejected target is not retried on every mouse move");
            hover(1);
            check(Wait([&] { return Peeking(second.window); }), "a different target remains eligible after rejection");
            failStop = true;
            preview.Hide();
            check(!preview.IsVisible() && !preview.IsCleared(), "failed hide cleanup remains pending");
            failStop = false;
            check(Wait([&] { return preview.IsCleared(); }), "hidden popup timer retries failed Peek cleanup");

            calls.clear();
            show();
            hover(0);
            SendPointer(preview.GetWindow(), WM_LBUTTONDOWN, 0, ContentPoint(preview.GetWindow()));
            Wait([] { return false; }, 450);
            SendPointer(preview.GetWindow(), WM_LBUTTONUP, 0, ContentPoint(preview.GetWindow()));
            check(calls.empty() && activated == first.window && !activatedFromPeek,
                "clicking before the delay cancels pending Peek and preserves the original click path");
            activated = nullptr;
            ShowWindow(first.window, SW_SHOWMINNOACTIVE);
            show();
            hover(0);
            check(Wait([&] { return Peeking(first.window); }), "click fixture begins Peek");
            SendPointer(preview.GetWindow(), WM_LBUTTONDOWN, 0, ContentPoint(preview.GetWindow()));
            check(Peeking(first.window), "left-button down retains the preview for a seamless click handoff");
            SendPointer(preview.GetWindow(), WM_LBUTTONUP, 0, ContentPoint(preview.GetWindow()));
            check(activatedFromPeek == first.window && !activated && !closed && peekHeldForHandoff &&
                    Peeking(first.window) && !preview.IsVisible() && IsIconic(first.window),
                "an active Peek selects the nonanimated callback and remains visible through asynchronous restoration");
            ShowWindow(first.window, SW_SHOWNOACTIVATE);
            foreground = first.window;
            check(Wait([&] { return preview.IsCleared(); }) && !calls.back().active,
                "Peek ends only after the real source has restored and taken foreground");
            foreground = actualForeground;
            ShowWindow(first.window, SW_SHOWMINNOACTIVE);
            show();
            hover(0);
            check(Wait([&] { return Peeking(first.window); }), "unresponsive-restore fixture begins Peek");
            SendPointer(preview.GetWindow(), WM_LBUTTONUP, 0, ContentPoint(preview.GetWindow()));
            check(Wait([&] { return preview.IsCleared(); }, 2000) && IsIconic(first.window),
                "an uncompleted activation has a bounded Peek handoff without fabricating restoration");
            ShowWindow(first.window, SW_SHOWNOACTIVATE);
            show();
            hover(1);
            check(Wait([&] { return Peeking(second.window); }), "middle-click fixture begins Peek");
            endedBeforeAction = false;
            SendPointer(preview.GetWindows()[1], WM_MBUTTONUP, 0, ContentPoint(preview.GetWindows()[1]));
            check(closed == second.window && endedBeforeAction && !Peeking(second.window) && preview.IsVisible(),
                "middle click ends Peek before dispatching close while retaining the multi-window preview");

            show();
            hover(1);
            check(Wait([&] { return Peeking(second.window); }), "destroyed-source fixture begins Peek");
            DestroyWindow(second.window);
            second.window = nullptr;
            check(Wait([&] { return !calls.back().active; }), "source destruction ends Peek without a mouse leave");
            preview.Hide();
            show();
            hover(0);
            check(Wait([&] { return Peeking(first.window); }), "destroyed-popup fixture begins Peek");
            DestroyWindow(preview.GetWindow());
            check(!calls.back().active, "destroying the popup dismisses its Peek before native teardown");

            Source survivor;
            ShowWindow(survivor.window, SW_SHOWNOACTIVATE);
            preview.Show({{first.window, L"primary"}, {survivor.window, L"survivor"}},
                {0, 0, 30, 30}, DockPosition::Bottom, false);
            for (const HWND popup : preview.GetWindows())
                SetWindowPos(popup, nullptr, -16000, -16000, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            hover(1);
            check(Wait([&] { return Peeking(survivor.window); }), "surviving-card cleanup fixture begins Peek");
            failStop = true;
            DestroyWindow(preview.GetWindow());
            const size_t rejectedStopCalls = calls.size();
            failStop = false;
            check(Wait([&] { return calls.size() > rejectedStopCalls && !calls.back().active; }),
                "a surviving card retries rejected Peek cleanup after the primary popup is destroyed");
            preview.Hide();
            check(preview.IsCleared(), "surviving-card cleanup leaves no hidden Peek timer or session");
        }
        check(!sourceHasFocus(), "preview lifecycle never gives an owned source foreground");

        calls.clear();
        {
            DockWindowPreview unavailable(snowdesktop::dock_peek::Api{nullptr});
            HWND fallbackActivated = nullptr;
            check(unavailable.Initialize(GetModuleHandleW(nullptr),
                [&](HWND window) { fallbackActivated = window; }, [](HWND) {}),
                "preview without the private API initializes");
            unavailable.Show({{first.window, L"fallback"}}, {0, 0, 30, 30}, DockPosition::Bottom, false);
            SetWindowPos(unavailable.GetWindow(), nullptr, -16000, -16000, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendPointer(unavailable.GetWindow(), WM_MOUSEMOVE, 0, ContentPoint(unavailable.GetWindow()));
            Wait([] { return false; }, 450);
            SendPointer(unavailable.GetWindow(), WM_LBUTTONUP, 0, ContentPoint(unavailable.GetWindow()));
            check(calls.empty() && fallbackActivated == first.window && unavailable.IsCleared(),
                "missing Peek support retains ordinary thumbnail activation and cleanup");
        }

        Source other;
        ShowWindow(other.window, SW_SHOWNOACTIVATE);
        {
            DockWindowPin pins;
            PersonalizationSettings appearance;
            appearance.windowPinBorderEnabled = true;
            appearance.windowPinBorderOpacity = 1.0f;
            pins.SetAppearance(appearance);
            check(pins.Toggle(first.window) && pins.Toggle(other.window), "Peek border fixture owns two pins");
            const HWND firstRing = static_cast<HWND>(GetPropW(first.window, L"SnowDesktop.DockWindowPin"));
            const HWND otherRing = static_cast<HWND>(GetPropW(other.window, L"SnowDesktop.DockWindowPin"));
            pins.SetPeekTarget(first.window);
            check(IsWindowVisible(firstRing) && !IsWindowVisible(otherRing),
                "Peek leaves only the target's owned pin ring visible");
            pins.Refresh();
            check(!IsWindowVisible(otherRing) && DockWindowPin::IsPinned(other.window),
                "pin refresh cannot resurrect an unrelated ring or cancel its pin during Peek");
            pins.SetPeekTarget(nullptr);
            check(IsWindowVisible(firstRing) && IsWindowVisible(otherRing) &&
                    DockWindowPin::IsPinned(first.window) && DockWindowPin::IsPinned(other.window),
                "ending Peek restores owned rings while retaining both pin requests");
        }
        std::cout << "preview Peek message, timer, cleanup and native pin-border cases completed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Peek fixture: " << error.what() << '\n';
        report(false, "Peek production-path fixture must complete");
    }
}
}
