#pragma once

#include "../diagnostic_log.h"
#include <windows.h>
#include <array>
#include <cwchar>
#include <sstream>

// Host-private, UI-thread-only observations. Never changes activation, window
// geometry, taskbar preferences or display affinity; no titles/content collected.
namespace snowdesktop::dock_taskbar_diagnostics
{
namespace detail
{
constexpr size_t kRecordLimit = 96, kTaskbarLimit = 8;
constexpr size_t kMotionRecordLimit = 80; // Reserve space for activation and explicit phases.
constexpr ULONGLONG kSessionMs = 3000, kPollMs = 100;

struct WindowState
{
    HWND window = nullptr, owner = nullptr;
    DWORD process = 0;
    wchar_t className[64]{};
    RECT rect{};
    bool valid = false, rectValid = false, visible = false;
    bool iconic = false, maximized = false, topmost = false;
};
struct TaskbarState
{
    WindowState window;
    HWND above = nullptr;
    DWORD aboveProcess = 0;
    wchar_t aboveClass[64]{};
    HMONITOR monitor = nullptr;
    RECT screen{}, intersection{};
};
using Taskbars = std::array<TaskbarState, kTaskbarLimit>;
struct Entry
{
    ULONGLONG received = 0, eventReceived = 0;
    DWORD event = 0, eventTime = 0;
    wchar_t phase[512]{};
    WindowState foreground, subject;
    POINT cursor{};
    bool cursorValid = false;
    Taskbars taskbars;
};
struct State
{
    bool active = false, busy = false, taskbarOverflow = false;
    ULONGLONG sequence = 0, started = 0, lastPoll = 0;
    ULONGLONG dropped = 0, reentrantDropped = 0, expiredDropped = 0;
    HWND target = nullptr, lastForeground = nullptr;
    wchar_t targetExecutable[128]{};
    size_t count = 0, taskbarCount = 0;
    std::array<HWND, kTaskbarLimit> taskbarWindows{};
    Taskbars lastTaskbars;
    std::array<Entry, kRecordLimit> entries;
};
inline State& Storage() { static State state; return state; }
struct Scope
{
    State& state;
    explicit Scope(State& value) : state(value) { state.busy = true; }
    ~Scope() { state.busy = false; }
};
inline bool Reentrant(State& state)
{
    if (!state.busy) return false;
    if (state.active) ++state.reentrantDropped;
    return true;
}
inline bool DropHotSample(State& state, size_t limit = kRecordLimit)
{
    if (GetTickCount64() - state.started >= kSessionMs)
    { ++state.expiredDropped; return true; }
    if (state.count < limit) return false;
    ++state.dropped;
    return true;
}
inline WindowState ReadWindow(HWND window)
{
    WindowState result;
    result.window = window;
    if (!window || !IsWindow(window)) return result;
    result.valid = true;
    GetWindowThreadProcessId(window, &result.process);
    GetClassNameW(window, result.className, 64);
    result.owner = GetWindow(window, GW_OWNER);
    result.rectValid = GetWindowRect(window, &result.rect) != FALSE;
    result.visible = IsWindowVisible(window) != FALSE;
    result.iconic = IsIconic(window) != FALSE;
    result.maximized = IsZoomed(window) != FALSE;
    result.topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    return result;
}
inline Entry Capture(State& state, const wchar_t* phase, HWND subject,
    DWORD event = 0, DWORD eventTime = 0, ULONGLONG eventReceived = 0)
{
    Entry entry;
    entry.received = GetTickCount64();
    entry.event = event;
    entry.eventTime = eventTime;
    entry.eventReceived = event ? (eventReceived ? eventReceived : entry.received) : 0;
    wcsncpy_s(entry.phase, phase ? phase : L"record", _TRUNCATE);
    entry.foreground = ReadWindow(GetForegroundWindow());
    entry.subject = ReadWindow(subject);
    entry.cursorValid = GetCursorPos(&entry.cursor) != FALSE;
    for (size_t i = 0; i < state.taskbarCount; ++i)
    {
        auto& bar = entry.taskbars[i];
        bar.window = ReadWindow(state.taskbarWindows[i]);
        if (!bar.window.valid) continue;
        bar.above = GetWindow(bar.window.window, GW_HWNDPREV);
        if (bar.above)
        {
            GetClassNameW(bar.above, bar.aboveClass, 64);
            GetWindowThreadProcessId(bar.above, &bar.aboveProcess);
        }
        bar.monitor = MonitorFromWindow(bar.window.window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (bar.monitor && GetMonitorInfoW(bar.monitor, &info))
        {
            bar.screen = info.rcMonitor;
            if (bar.window.rectValid)
                IntersectRect(&bar.intersection, &bar.window.rect, &bar.screen);
        }
    }
    return entry;
}
inline void Append(State& state, const Entry& entry)
{
    if (state.count < kRecordLimit) state.entries[state.count++] = entry;
    else { state.entries.back() = entry; ++state.dropped; } // Preserve the latest/end state.
    state.lastTaskbars = entry.taskbars;
    state.lastForeground = entry.foreground.window;
}
inline void BeginSession(State& state, HWND target, const wchar_t* origin)
{
    state.active = true;
    ++state.sequence;
    state.started = GetTickCount64();
    state.lastPoll = state.started;
    state.target = target;
    state.count = state.taskbarCount = 0;
    state.dropped = state.reentrantDropped = state.expiredDropped = 0;
    state.taskbarOverflow = false;
    state.targetExecutable[0] = L'\0';
    const auto addTaskbar = [&state](HWND window) {
        if (!window) return true;
        for (size_t i = 0; i < state.taskbarCount; ++i)
            if (state.taskbarWindows[i] == window) return true;
        if (state.taskbarCount == kTaskbarLimit)
        { state.taskbarOverflow = true; return false; }
        state.taskbarWindows[state.taskbarCount++] = window;
        return true;
    };
    // Seed the primary directly: Shell UI can temporarily reparent it.
    addTaskbar(FindWindowW(L"Shell_TrayWnd", nullptr));
    // Only discover the two shell taskbar classes, once per bounded session.
    for (const auto* name : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
    {
        HWND previous = nullptr;
        while (HWND window = FindWindowExW(nullptr, previous, name, nullptr))
        {
            if (!addTaskbar(window)) break;
            previous = window;
        }
    }
    const auto entry = Capture(state, origin ? origin : L"begin", target);
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.subject.process))
    {
        wchar_t path[1024]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &length))
        {
            const wchar_t* name = wcsrchr(path, L'\\');
            wcsncpy_s(state.targetExecutable, name ? name + 1 : path, _TRUNCATE);
        }
        CloseHandle(process);
    }
    Append(state, entry);
}
inline bool SameTaskbar(const TaskbarState& a, const TaskbarState& b)
{
    return a.window.valid == b.window.valid && a.window.process == b.window.process &&
        a.window.rectValid == b.window.rectValid &&
        a.window.visible == b.window.visible && a.window.topmost == b.window.topmost &&
        a.window.owner == b.window.owner && a.above == b.above &&
        a.aboveProcess == b.aboveProcess && a.monitor == b.monitor &&
        EqualRect(&a.window.rect, &b.window.rect) && EqualRect(&a.screen, &b.screen) &&
        EqualRect(&a.intersection, &b.intersection) &&
        wcscmp(a.window.className, b.window.className) == 0 &&
        wcscmp(a.aboveClass, b.aboveClass) == 0;
}
inline void RectText(std::wostream& out, const RECT& rect)
{ out << L'[' << rect.left << L',' << rect.top << L',' << rect.right << L',' << rect.bottom << L']'; }
inline void WindowText(std::wostream& out, const WindowState& window)
{
    out << window.window << L'/' << window.className << L"{pid=" << window.process << L" valid=" << window.valid
        << L" rectValid=" << window.rectValid << L" rect=";
    RectText(out, window.rect);
    out << L" visible=" << window.visible << L" iconic=" << window.iconic
        << L" max=" << window.maximized << L" topmost=" << window.topmost
        << L" owner=" << window.owner << L'}';
}
inline void Flush(State& state, const wchar_t* reason)
{
    std::wostringstream out;
    out << L"DockTaskbarTrace hostPid=" << GetCurrentProcessId() << L" id=" << state.sequence
        << L" started=" << state.started << L" deadline=" << state.started + kSessionMs
        << L" flushed=" << GetTickCount64()
        << L" target=" << state.target << L" targetExe=" << state.targetExecutable
        << L" reason=" << reason << L" records=" << state.count
        << L" dropped=" << state.dropped << L" reentrantDropped=" << state.reentrantDropped
        << L" expiredDropped=" << state.expiredDropped
        << L" taskbars=" << state.taskbarCount << L" taskbarOverflow=" << state.taskbarOverflow;
    for (size_t i = 0; i < state.count; ++i)
    {
        const auto& entry = state.entries[i];
        out << L"\n  +" << entry.received - state.started << L"ms sampleTick=" << entry.received
            << L" phase=" << entry.phase << L" event=" << entry.event
            << L" eventTime=" << entry.eventTime << L" eventReceived=" << entry.eventReceived << L" fg=";
        WindowText(out, entry.foreground);
        out << L" subject="; WindowText(out, entry.subject);
        out << L" cursor=" << entry.cursor.x << L',' << entry.cursor.y
            << L" cursorValid=" << entry.cursorValid;
        for (size_t j = 0; j < state.taskbarCount; ++j)
        {
            const auto& bar = entry.taskbars[j];
            out << L" bar" << j << L'='; WindowText(out, bar.window);
            out << L" monitor=" << bar.monitor << L" screen="; RectText(out, bar.screen);
            out << L" intersection="; RectText(out, bar.intersection);
            out << L" above=" << bar.above << L'/' << bar.aboveClass << L" abovePid=" << bar.aboveProcess;
        }
    }
    state.active = false;
    WriteDiagnosticLogEntry(out.str().c_str(), DiagnosticLogLevel::Debug);
}
} // namespace detail

inline void Begin(HWND target, const wchar_t* origin)
{
    auto& state = detail::Storage();
    if (detail::Reentrant(state)) return;
    detail::Scope scope(state);
    // Related commands retain the first target and deadline; they cannot keep
    // recording indefinitely or erase a preceding foreground/taskbar change.
    if (state.active)
    {
        if (detail::DropHotSample(state)) return;
        detail::Append(state, detail::Capture(state, origin, target));
    }
    else detail::BeginSession(state, target, origin);
}
inline void Record(const wchar_t* phase, HWND subject = nullptr,
    DWORD event = 0, DWORD eventTime = 0)
{
    auto& state = detail::Storage();
    if (detail::Reentrant(state) || !state.active) return;
    detail::Scope scope(state);
    if (detail::DropHotSample(state)) return;
    detail::Append(state, detail::Capture(state, phase, subject, event, eventTime));
}
inline void ObserveWinEvent(DWORD event, HWND window, LONG objectId,
    LONG childId, DWORD eventTime)
{
    auto& state = detail::Storage();
    if (detail::Reentrant(state)) return;
    const ULONGLONG eventReceived = GetTickCount64();
    detail::Scope scope(state);
    const bool minimize = event == EVENT_SYSTEM_MINIMIZESTART;
    const bool restore = event == EVENT_SYSTEM_MINIMIZEEND; // About to restore, not animation completion.
    const bool foreground = event == EVENT_SYSTEM_FOREGROUND;
    if (!state.active && minimize)
    {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process && process != GetCurrentProcessId())
            detail::BeginSession(state, window, L"external-minimize");
    }
    if (!state.active) return;
    bool taskbarEvent = false;
    if (objectId == OBJID_WINDOW && childId == CHILDID_SELF &&
        (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_LOCATIONCHANGE))
        for (size_t i = 0; i < state.taskbarCount; ++i)
            taskbarEvent = taskbarEvent || window == state.taskbarWindows[i];
    if (minimize || restore || foreground || taskbarEvent)
    {
        if (detail::DropHotSample(state, event == EVENT_OBJECT_LOCATIONCHANGE
                ? detail::kMotionRecordLimit : detail::kRecordLimit)) return;
        detail::Append(state, detail::Capture(state, minimize ? L"win-minimize" :
            restore ? L"win-restore" : foreground ? L"win-foreground" : L"taskbar-event",
            window, event, eventTime, eventReceived));
    }
}
inline void Poll()
{
    auto& state = detail::Storage();
    if (detail::Reentrant(state) || !state.active) return;
    detail::Scope scope(state);
    const ULONGLONG now = GetTickCount64();
    if (now - state.started >= detail::kSessionMs)
    {
        detail::Flush(state, L"deadline");
        return;
    }
    if (now - state.lastPoll < detail::kPollMs) return;
    state.lastPoll = now;
    if (detail::DropHotSample(state, detail::kMotionRecordLimit)) return;
    const auto entry = detail::Capture(state, L"poll-change", state.target);
    bool changed = entry.foreground.window != state.lastForeground;
    for (size_t i = 0; i < state.taskbarCount; ++i)
        changed = changed || !detail::SameTaskbar(entry.taskbars[i], state.lastTaskbars[i]);
    if (changed) detail::Append(state, entry);
}
inline void Stop()
{
    auto& state = detail::Storage();
    if (detail::Reentrant(state) || !state.active) return;
    detail::Scope scope(state);
    if (GetTickCount64() - state.started < detail::kSessionMs)
        detail::Append(state, detail::Capture(state, L"session-stop", state.target));
    detail::Flush(state, L"stop");
}
} // namespace snowdesktop::dock_taskbar_diagnostics
