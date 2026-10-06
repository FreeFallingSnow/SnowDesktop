#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <cstddef>
#include <vector>

namespace snowdesktop::dock_capture
{
struct IsolationError
{
    const wchar_t* operation = nullptr;
    HWND window = nullptr;
    DWORD error = ERROR_SUCCESS;
    HRESULT result = S_OK;
};

// Keep an externally supplied report alive until the guard is destroyed. This
// lets the caller log restoration failures after the capture scope has ended.
struct IsolationReport
{
    std::size_t excludedWindows = 0;
    std::size_t hiddenWindows = 0;
    IsolationError exclusionFallback;
    IsolationError preparationFailure;
    IsolationError restorationFailure;
};

// Host-private, synchronous capture guard. It changes no logical Dock state and
// never captures pixels itself. Only call the screen capture while Ready().
class ScopedWindowCaptureIsolation
{
public:
    ScopedWindowCaptureIsolation(HWND target, const RECT& source,
        IsolationReport* report = nullptr) noexcept
        : report_(report ? report : &localReport_)
    {
        *report_ = {};
        target = target ? GetAncestor(target, GA_ROOT) : nullptr;
        if (!target || !IsWindow(target) ||
            source.right <= source.left || source.bottom <= source.top)
        {
            Record(report_->preparationFailure, L"target", target,
                ERROR_INVALID_PARAMETER);
            return;
        }

        // Finish enumeration and all allocations before changing any HWND.
        // An allocation failure must not strand an already hidden surface.
        try
        {
            std::size_t remaining = 4096;
            for (HWND window = GetWindow(target, GW_HWNDPREV); window;)
            {
                // Other processes may reorder/destroy HWNDs while we walk.
                // A pathological Z-order race must fail closed, not loop.
                if (remaining-- == 0)
                {
                    Record(report_->preparationFailure, L"enumerateLimit",
                        window, ERROR_BUSY);
                    return;
                }
                const HWND previous = GetWindow(window, GW_HWNDPREV);
                if (window != target && IsOwned(window) && IsWindowVisible(window))
                {
                    RECT bounds{}, intersection{};
                    if (!GetWindowRect(window, &bounds))
                    {
                        const DWORD error = LastError();
                        if (IsOwned(window))
                        {
                            Record(report_->preparationFailure, L"GetWindowRect",
                                window, error);
                            return;
                        }
                    }
                    else if (IntersectRect(&intersection, &source, &bounds))
                        windows_.push_back({window});
                }
                window = previous;
            }
        }
        catch (...)
        {
            Record(report_->preparationFailure, L"enumerate", nullptr,
                ERROR_NOT_ENOUGH_MEMORY);
            return;
        }

        for (auto& entry : windows_)
        {
            if (!IsOwned(entry.window) || !IsWindowVisible(entry.window))
                continue;
            const bool affinityKnown = GetWindowDisplayAffinity(
                entry.window, &entry.affinity) != FALSE;
            const DWORD affinityError = affinityKnown ? ERROR_SUCCESS : LastError();
            if (affinityKnown && (entry.affinity == WDA_EXCLUDEFROMCAPTURE ||
                SetWindowDisplayAffinity(entry.window, WDA_EXCLUDEFROMCAPTURE)))
            {
                entry.action = entry.affinity == WDA_EXCLUDEFROMCAPTURE
                    ? Action::None : Action::Affinity;
                ++report_->excludedWindows;
                continue;
            }
            Record(report_->exclusionFallback, affinityKnown
                ? L"SetWindowDisplayAffinity" : L"GetWindowDisplayAffinity",
                entry.window, affinityKnown ? LastError() : affinityError);

            // Preserve the original visibility even when SetWindowPos reports
            // failure after window callbacks have already changed the surface.
            entry.action = Action::Hidden;
            const BOOL hidden = SetWindowPos(entry.window, nullptr, 0, 0, 0, 0,
                PositionFlags | SWP_HIDEWINDOW);
            const DWORD hideError = hidden ? ERROR_BUSY : LastError();
            if (!hidden || (IsOwned(entry.window) && IsWindowVisible(entry.window)))
            {
                Record(report_->preparationFailure, L"hide", entry.window, hideError);
                Restore();
                return;
            }
            ++report_->hiddenWindows;
        }
        const HRESULT flush = DwmFlush();
        if (FAILED(flush))
        {
            Record(report_->preparationFailure, L"DwmFlush", nullptr,
                ERROR_SUCCESS, flush);
            Restore();
            return;
        }
        ready_ = true;
    }

    ~ScopedWindowCaptureIsolation() noexcept { Restore(); }
    ScopedWindowCaptureIsolation(const ScopedWindowCaptureIsolation&) = delete;
    ScopedWindowCaptureIsolation& operator=(const ScopedWindowCaptureIsolation&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return ready_; }
    [[nodiscard]] const IsolationReport& Report() const noexcept { return *report_; }

    void Restore() noexcept
    {
        ready_ = false;
        bool changed = false;
        for (auto it = windows_.rbegin(); it != windows_.rend(); ++it)
        {
            auto& entry = *it;
            if (entry.action == Action::None)
                continue;
            if (!IsOwned(entry.window))
            {
                entry.action = Action::None;
                continue;
            }
            changed = true;
            if (entry.action == Action::Affinity)
            {
                if (!SetWindowDisplayAffinity(entry.window, entry.affinity))
                {
                    Record(report_->restorationFailure, L"restoreAffinity",
                        entry.window, LastError());
                    continue;
                }
            }
            else
            {
                const BOOL shown = SetWindowPos(entry.window, nullptr, 0, 0, 0, 0,
                    PositionFlags | SWP_SHOWWINDOW);
                const DWORD showError = shown ? ERROR_BUSY : LastError();
                if (!shown || !IsWindowVisible(entry.window))
                {
                    // A second, synchronous API is a last resort for a surface
                    // that refused SetWindowPos. Never activate the window.
                    if (IsOwned(entry.window))
                        ShowWindow(entry.window, SW_SHOWNOACTIVATE);
                    if (IsOwned(entry.window) && !IsWindowVisible(entry.window))
                    {
                        Record(report_->restorationFailure, L"restoreVisibility",
                            entry.window, showError);
                        continue;
                    }
                }
            }
            entry.action = Action::None;
        }
        if (changed)
        {
            const HRESULT flush = DwmFlush();
            if (FAILED(flush))
                Record(report_->restorationFailure, L"restoreDwmFlush", nullptr,
                    ERROR_SUCCESS, flush);
        }
    }

private:
    enum class Action { None, Affinity, Hidden };
    struct WindowState
    {
        HWND window = nullptr;
        DWORD affinity = WDA_NONE;
        Action action = Action::None;
    };
    static constexpr UINT PositionFlags = SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

    static bool IsOwned(HWND window) noexcept
    {
        DWORD processId = 0;
        return window && IsWindow(window) &&
            GetWindowThreadProcessId(window, &processId) != 0 &&
            processId == GetCurrentProcessId();
    }
    static DWORD LastError() noexcept
    {
        const DWORD error = GetLastError();
        return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
    }
    static void Record(IsolationError& first, const wchar_t* operation,
        HWND window, DWORD error, HRESULT result = S_OK) noexcept
    {
        if (!first.operation)
            first = {operation, window, error, result};
    }

    IsolationReport localReport_;
    IsolationReport* report_;
    std::vector<WindowState> windows_;
    bool ready_ = false;
};
}
