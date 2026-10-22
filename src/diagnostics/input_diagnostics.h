#pragma once

#include "diagnostic_log.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

namespace snowdesktop
{
// Opt-in, bounded diagnostics. The worker observes the UI without attaching
// input queues or installing hooks. A late asynchronous probe is an observation,
// not a hang verdict (modal loops and a busy system can also delay delivery).
class InputDiagnostics final
{
public:
    struct Timing
    {
        DWORD sampleMs = 2000;
        DWORD reportMs = 30000;
        DWORD lateMs = 5000;
    };
    using Sink = void (*)(const wchar_t*);

    explicit InputDiagnostics(HWND control) : InputDiagnostics(control, Timing{}, Log) {}
    InputDiagnostics(HWND control, Timing timing, Sink sink)
        : control_(control), uiThread_(GetCurrentThreadId()), timing_(timing), sink_(sink)
    {
        if (current_ || !control || !timing.sampleMs || !timing.reportMs || !sink ||
            GetWindowThreadProcessId(control, nullptr) != uiThread_)
            return;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_)
        {
            WriteDiagnosticLogEntry(L"InputDiagnostics unavailable: stop event creation failed", DiagnosticLogLevel::Warning);
            return;
        }
        current_ = this;
        ackTick_ = GetTickCount64();
        try { worker_ = std::thread([this] { Run(); }); }
        catch (...)
        {
            current_ = nullptr;
            CloseHandle(stop_);
            stop_ = nullptr;
            WriteDiagnosticLogEntry(L"InputDiagnostics unavailable: worker creation failed", DiagnosticLogLevel::Warning);
        }
    }
    ~InputDiagnostics()
    {
        // No posted message carries this object's address. A pending probe is
        // harmless after destruction, even if its HWND remains alive.
        if (current_ == this) current_ = nullptr;
        if (stop_) SetEvent(stop_);
        if (worker_.joinable()) worker_.join();
        if (stop_) CloseHandle(stop_);
    }
    InputDiagnostics(const InputDiagnostics&) = delete;
    InputDiagnostics& operator=(const InputDiagnostics&) = delete;

    static bool Acknowledge(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        auto* self = current_;
        if (!self || window != self->control_ || message != WM_NULL || wp != ProbeTag ||
            static_cast<ULONG_PTR>(lp) != self->sequence_.load() || !self->pending_.load())
            return false;
        self->queue_.store(HIWORD(GetQueueStatus(QS_INPUT)));
        self->ackTick_.store(GetTickCount64());
        self->pending_.store(false);
        return true;
    }

    // UI-thread calls only. Mouse moves update counters; never format or write
    // logs from the pointer path. Counts describe the main pump, not global input.
    static void ObserveDequeued(const MSG& message) noexcept
    {
        auto* self = current_;
        if (!self || (self->sink_ == Log && !diagnostics::IsLogEnabled(DiagnosticLogModule::Input))) return;
        if (message.message < WM_MOUSEFIRST || message.message > WM_MOUSELAST) return;
        if (message.message == WM_MOUSEMOVE) ++self->moves_;
        else ++self->buttons_;
        self->pointerMessage_.store(message.message);
        self->pointerWindow_.store(message.hwnd);
        self->pointerTick_.store(GetTickCount64());
    }

    static void DeviceChange(WPARAM event) noexcept
    {
        if (auto* self = current_)
        {
            self->deviceEvent_.store(static_cast<DWORD>(event));
            self->deviceTick_.store(GetTickCount64());
            ++self->deviceCount_;
        }
    }

private:
    static constexpr WPARAM ProbeTag = 0x5344494e; // SDIN; WM_NULL has no payload contract.
    inline static thread_local InputDiagnostics* current_ = nullptr;
    static void Log(const wchar_t* line) { WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Input, line); }
    void Emit(const wchar_t* line) noexcept { try { sink_(line); } catch (...) {} }
    static ULONGLONG Age(ULONGLONG now, ULONGLONG tick) noexcept
    { return tick && now >= tick ? now - tick : 0; }

    void Run() noexcept
    {
        wchar_t line[2048]{};
        swprintf_s(line, L"InputDiagnostics started schema=1 uiTid=%lu sampleMs=%lu reportMs=%lu lateMs=%lu",
            uiThread_, timing_.sampleMs, timing_.reportMs, timing_.lateMs);
        Emit(line);
        ULONGLONG lastReport = 0, captureSince = 0, probeSent = 0;
        HWND lastCapture = nullptr;
        DWORD lastDevices = 0, postError = 0;
        bool wasLate = false, wasStale = false, haveCursor = false;
        POINT lastCursor{};
        unsigned long long cursorChanges = 0;
        while (WaitForSingleObject(stop_, timing_.sampleMs) == WAIT_TIMEOUT)
        {
            // Custom sinks are explicit diagnostic sessions (including tests).
            // The normal worker sleeps without probing other threads when off.
            if (sink_ == Log && !snowdesktop::diagnostics::IsLogEnabled(DiagnosticLogModule::Input))
            {
                pending_.store(false);
                wasLate = wasStale = false;
                lastReport = 0;
                continue;
            }
            const ULONGLONG now = GetTickCount64();
            GUITHREADINFO gui{sizeof(gui)};
            const BOOL guiOk = GetGUIThreadInfo(uiThread_, &gui);
            DWORD physical = 0;
            const int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            for (unsigned i = 0; i < 5; ++i)
                if (GetAsyncKeyState(keys[i]) & 0x8000) physical |= 1U << i;
            if (!gui.hwndCapture || gui.hwndCapture != lastCapture || physical)
                captureSince = now;
            const bool stale = guiOk && gui.hwndCapture && !physical && now - captureSince >= timing_.sampleMs;
            const bool late = pending_.load() && now - probeSent >= timing_.lateMs;
            const DWORD devices = deviceCount_.load();
            POINT cursor{};
            const BOOL cursorOk = GetCursorPos(&cursor);
            if (cursorOk)
            {
                if (haveCursor && (cursor.x != lastCursor.x || cursor.y != lastCursor.y)) ++cursorChanges;
                lastCursor = cursor;
                haveCursor = true;
            }
            const bool periodic = !lastReport || now - lastReport >= timing_.reportMs;
            // Capture transitions during ordinary clicks are deliberately not
            // logged. Retain sustained all-up capture, late/recovered probes,
            // device bursts and a sparse baseline; never one line per event.
            if (periodic || late != wasLate || stale != wasStale || devices != lastDevices)
            {
                const HWND foreground = GetForegroundWindow();
                DWORD foregroundPid = 0, capturePid = 0;
                const DWORD foregroundTid = foreground ? GetWindowThreadProcessId(foreground, &foregroundPid) : 0;
                const DWORD captureTid = gui.hwndCapture ? GetWindowThreadProcessId(gui.hwndCapture, &capturePid) : 0;
                GUITHREADINFO foregroundGui{sizeof(foregroundGui)};
                const BOOL foregroundGuiOk = foregroundTid && GetGUIThreadInfo(foregroundTid, &foregroundGui);
                LASTINPUTINFO input{sizeof(input)};
                const BOOL inputOk = GetLastInputInfo(&input);
                wchar_t captureClass[96]{};
                if (gui.hwndCapture) GetClassNameW(gui.hwndCapture, captureClass, 96);
                const auto pointerTick = pointerTick_.load(), deviceTick = deviceTick_.load();
                swprintf_s(line, L"InputState reason=%ls uiProbeLate=%d uiAckAgeMs=%llu pending=%d postError=%lu guiOk=%d guiFlags=0x%lX active=%p focus=%p capture=%p captureTid=%lu capturePid=%lu captureVisible=%d captureClass=%ls allUpCapture=%d physicalButtons=0x%lX foreground=%p foregroundTid=%lu foregroundPid=%lu foregroundGuiOk=%d foregroundCapture=%p lastInputOk=%d lastInputAgeMs=%lu cursorOk=%d cursorChanges=%llu pumpMoves=%llu pumpButtons=%llu pointerMessage=0x%X pointerWindow=%p pointerAgeMs=%llu queueAtAck=0x%X deviceCount=%lu deviceEvent=0x%lX deviceAgeMs=%llu",
                    late != wasLate ? (late ? L"probe-late" : L"probe-recovered") :
                        stale != wasStale ? (stale ? L"capture-all-up" : L"capture-cleared") :
                        devices != lastDevices ? L"device-change" : L"periodic",
                    late, Age(now, ackTick_.load()), pending_.load(), postError, guiOk, gui.flags,
                    static_cast<void*>(gui.hwndActive), static_cast<void*>(gui.hwndFocus), static_cast<void*>(gui.hwndCapture),
                    captureTid, capturePid, gui.hwndCapture ? IsWindowVisible(gui.hwndCapture) : FALSE, captureClass,
                    stale, physical, static_cast<void*>(foreground), foregroundTid, foregroundPid, foregroundGuiOk,
                    static_cast<void*>(foregroundGui.hwndCapture), inputOk, inputOk ? GetTickCount() - input.dwTime : 0,
                    cursorOk, cursorChanges, moves_.load(), buttons_.load(), pointerMessage_.load(),
                    static_cast<void*>(pointerWindow_.load()), Age(now, pointerTick),
                    queue_.load(), devices, deviceEvent_.load(), Age(now, deviceTick));
                Emit(line);
                lastReport = now;
            }
            wasLate = late; wasStale = stale; lastCapture = gui.hwndCapture; lastDevices = devices;
            if (!pending_.load())
            {
                ++sequence_;
                probeSent = now;
                pending_.store(true);
                if (!PostMessageW(control_, WM_NULL, ProbeTag, static_cast<LPARAM>(sequence_.load())))
                {
                    postError = GetLastError();
                    pending_.store(false);
                }
                else postError = 0;
            }
        }
        Emit(L"InputDiagnostics stopped");
    }

    HWND control_ = nullptr;
    DWORD uiThread_ = 0;
    Timing timing_;
    Sink sink_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> pending_{false};
    std::atomic<ULONG_PTR> sequence_{0};
    std::atomic<ULONGLONG> ackTick_{0}, pointerTick_{0}, deviceTick_{0};
    std::atomic<unsigned long long> moves_{0}, buttons_{0};
    std::atomic<UINT> pointerMessage_{0}, queue_{0};
    std::atomic<HWND> pointerWindow_{nullptr};
    std::atomic<DWORD> deviceEvent_{0}, deviceCount_{0};
};
}
