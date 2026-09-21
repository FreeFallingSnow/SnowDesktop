#pragma once

#include "../diagnostic_log.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <utility>

namespace snowdesktop::startup_diagnostics
{
// Only explicit startup roots enable nested probes. Normal paints and reloads
// do not read clocks or write logs. Names are static operation names, not paths.
class Scope final
{
public:
    explicit Scope(const wchar_t* name, bool startupRoot = false,
        size_t items = 0) noexcept : name_(name), items_(items)
    {
        if (!startupRoot && !current_) return;
        previous_ = current_;
        id_ = nextId_.fetch_add(1, std::memory_order_relaxed) + 1;
        parent_ = previous_ ? previous_->id_ : 0;
        pass_ = !startupRoot && previous_ ? previous_->pass_ : id_;
        if (!startupRoot && previous_) items_ = previous_->items_;
        current_ = this;
        Emit(L"begin", 0);
        started_ = GetTickCount64();
    }

    ~Scope() { Finish(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void Finish() noexcept
    {
        if (!id_) return;
        const auto elapsed = GetTickCount64() - started_;
        current_ = previous_;
        Emit(L"end", elapsed);
        id_ = 0;
    }

private:
    void Emit(const wchar_t* event, ULONGLONG elapsed) const noexcept
    {
        const DWORD savedError = GetLastError();
        try
        {
            static const ULONGLONG run = GetTickCount64();
            wchar_t message[384]{};
            swprintf_s(message,
                L"Startup call %ls: run=%lu-%llu thread=%lu pass=%llu "
                L"call=%llu parent=%llu step=%ls items=%zu elapsed_ms=%llu",
                event, GetCurrentProcessId(), run, GetCurrentThreadId(), pass_,
                id_, parent_, name_, items_, elapsed);
            WriteDiagnosticLogEntry(message);
        }
        catch (...) {} // Diagnostics must not change startup error handling.
        SetLastError(savedError);
    }

    inline static std::atomic<ULONGLONG> nextId_{0};
    inline static thread_local Scope* current_ = nullptr;
    Scope* previous_ = nullptr;
    const wchar_t* name_;
    size_t items_;
    ULONGLONG id_ = 0, parent_ = 0, pass_ = 0, started_ = 0;
};

template<class Callback>
decltype(auto) Call(const wchar_t* name, Callback&& callback)
{
    Scope scope(name);
    return std::forward<Callback>(callback)();
}
}
