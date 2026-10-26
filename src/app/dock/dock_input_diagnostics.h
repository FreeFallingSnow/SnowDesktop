#pragma once

#include "diagnostics/diagnostic_log.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <sstream>

// Host-private observations only. The hook publishes atomics; the UI thread
// logs values already used by dismissal. Never queries additional key state,
// activates windows, or changes the decision under investigation.
// A running hook does not prove visibility across integrity levels. Its event
// stamps can remain unchanged over an elevated window; compare both observations
// rather than interpreting hook=true as a complete button history.
namespace snowdesktop::dock_input_diagnostics
{
inline std::atomic<std::uint64_t> lastDown{0}, lastUp{0};

inline void ObserveMouse(WPARAM message, LPARAM data) noexcept
{
    if (!diagnostics::IsLogEnabled(DiagnosticLogModule::Dock) || !data) return;
    const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
    const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    if (!down && !up) return;
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    const auto value = (static_cast<std::uint64_t>(mouse->time) << 32) |
        static_cast<std::uint32_t>(message);
    (down ? lastDown : lastUp).store(value, std::memory_order_relaxed);
}

enum class Gate : unsigned
{
    Fullscreen, Passthrough, VisibleOrBar, Press, PassiveReveal, Minimize,
    PopupActivation, Visible, StartMenu, BarSession, Bar, RevealedDock,
    PromotedDock, Drag, Menu, DismissRule, Count
};
inline constexpr std::array<const wchar_t*, static_cast<unsigned>(Gate::Count)> gateNames{
    L"fullscreen", L"passthrough", L"visibleOrBar", L"press", L"passiveReveal", L"minimize",
    L"popupActivation", L"visible", L"startMenu", L"barSession", L"bar", L"revealedDock",
    L"promotedDock", L"drag", L"menu", L"dismissRule"
};

struct Sample
{
    SHORT left = 0, right = 0, middle = 0;
    UINT buttons = 0, previous = 0, recent = 0;
    bool sampled = false, hook = false, cursorKnown = false;
    LONG x = 0, y = 0;
    HWND foreground = nullptr;
    DWORD process = 0, thread = 0;
    std::uint64_t downEvent = 0, upEvent = 0;
    unsigned evaluated = 0, values = 0;
    const wchar_t* outcome = L"retained";
    bool operator==(const Sample&) const = default;
};

struct Session
{
    bool active = false, hasSample = false, exhausted = false;
    ULONGLONG sequence = 0, started = 0, lastWrite = 0;
    unsigned records = 0;
    Sample previous;
};
inline Session& Storage() { static Session session; return session; }

class Scope
{
public:
    Scope(bool visible, bool hook) noexcept
    {
        auto& session = Storage();
        enabled_ = diagnostics::IsLogEnabled(DiagnosticLogModule::Dock) && visible;
        if (!enabled_)
        {
            session.active = false;
            return;
        }
        if (!session.active)
        {
            const auto sequence = session.sequence + 1;
            session = {};
            session.active = true;
            session.sequence = sequence;
            session.started = GetTickCount64();
        }
        sample.hook = hook;
        sample.foreground = GetForegroundWindow();
        if (sample.foreground)
            sample.thread = GetWindowThreadProcessId(sample.foreground, &sample.process);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    bool Observe(Gate gate, bool value) noexcept
    {
        if (enabled_)
        {
            const unsigned bit = 1u << static_cast<unsigned>(gate);
            sample.evaluated |= bit;
            if (value) sample.values |= bit;
            else sample.values &= ~bit;
        }
        return value;
    }
    ~Scope() noexcept
    {
        if (!enabled_) return;
        try { Write(); } catch (...) {} // Diagnostics must not interrupt input.
    }

    Sample sample;

private:
    bool enabled_ = false;
    void Write()
    {
        auto& session = Storage();
        if (session.exhausted) return;
        const ULONGLONG now = GetTickCount64();
        if (session.records >= 512 || now - session.started >= 120000)
        {
            session.exhausted = true;
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
                L"DockInputTrace limit reached; hide and summon Dock to start another session",
                DiagnosticLogLevel::Debug);
            return;
        }
        sample.downEvent = lastDown.load(std::memory_order_relaxed);
        sample.upEvent = lastUp.load(std::memory_order_relaxed);
        // Cursor motion alone must not generate a record every 20 ms.
        Sample comparable = sample;
        comparable.x = comparable.y = 0;
        if (session.hasSample && comparable == session.previous &&
            now - session.lastWrite < 1000) return;
        session.previous = comparable;
        session.hasSample = true;
        session.lastWrite = now;
        ++session.records;
        std::wostringstream out;
        out << L"DockInputTrace pid=" << GetCurrentProcessId() << L" session=" << session.sequence
            << L" tick=" << now << L" elapsed=" << now - session.started
            << L" record=" << session.records << L" outcome=" << sample.outcome
            << L" sampled=" << sample.sampled << L" raw=" << sample.left << L',' << sample.right << L',' << sample.middle
            << L" buttons=" << sample.buttons << L" previous=" << sample.previous << L" recent=" << sample.recent
            << L" hook=" << sample.hook << L" hookDown=" << (sample.downEvent >> 32) << L'/' << (sample.downEvent & 0xffffffffu)
            << L" hookUp=" << (sample.upEvent >> 32) << L'/' << (sample.upEvent & 0xffffffffu)
            << L" foreground=" << sample.foreground << L" foregroundPid=" << sample.process << L" foregroundTid=" << sample.thread
            << L" cursorKnown=" << sample.cursorKnown << L" cursor=" << sample.x << L',' << sample.y;
        for (unsigned i = 0; i < gateNames.size(); ++i)
            out << L' ' << gateNames[i] << L'='
                << ((sample.evaluated & (1u << i)) ? ((sample.values & (1u << i)) ? 1 : 0) : -1);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, out.str().c_str(), DiagnosticLogLevel::Debug);
    }
};
} // namespace snowdesktop::dock_input_diagnostics
