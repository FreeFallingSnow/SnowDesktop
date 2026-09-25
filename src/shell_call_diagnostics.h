#pragma once

#include "diagnostic_log.h"
#include <windows.h>
#include <atomic>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

namespace snowdesktop::shell_call_diagnostics
{
// Synchronous probes only: no new worker, Shell call, or disk write on the fast
// path. A slow call is logged when it returns, not while it remains blocked.
class Context final
{
public:
    using Clock = decltype(&GetTickCount64);
    Context(const wchar_t* kind, std::wstring_view path,
        Clock clock = &GetTickCount64) noexcept
        : previous_(current_), kind_(kind), path_(path), clock_(clock),
          id_(nextId_.fetch_add(1, std::memory_order_relaxed) + 1),
          started_(clock_())
    {
        current_ = this;
    }
    ~Context()
    {
        const auto elapsed = clock_() - started_;
        current_ = previous_;
        if (elapsed >= kSlowMs)
            Emit(L"total", 0, 0, started_, elapsed, {});
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ULONGLONG Id() const noexcept { return id_; }
    void SetItem(size_t item) noexcept { item_ = item; }

    class Step final
    {
    public:
        Step(const wchar_t* name, std::wstring_view detail) noexcept
            : context_(current_), name_(name), detail_(detail)
        {
            if (!context_) return;
            parent_ = context_->activeCall_;
            id_ = ++context_->calls_;
            context_->activeCall_ = id_;
            started_ = context_->clock_();
        }
        ~Step()
        {
            if (!context_) return;
            const auto elapsed = context_->clock_() - started_;
            context_->activeCall_ = parent_;
            if (elapsed >= kSlowMs)
                context_->Emit(name_, id_, parent_, started_, elapsed, detail_);
        }
        Step(const Step&) = delete;
        Step& operator=(const Step&) = delete;

    private:
        Context* context_;
        const wchar_t* name_;
        std::wstring_view detail_;
        ULONGLONG id_ = 0, parent_ = 0, started_ = 0;
    };

private:
    void Emit(const wchar_t* step, ULONGLONG call, ULONGLONG parentCall,
        ULONGLONG started, ULONGLONG elapsed, std::wstring_view detail) const noexcept
    {
        const DWORD savedError = GetLastError();
        try
        {
            wchar_t fields[512]{};
            swprintf_s(fields,
                L"Shell call slow: process=%lu thread=%lu trace=%llu parentTrace=%llu "
                L"kind=%ls call=%llu parentCall=%llu step=%ls item=%zu "
                L"start_tick=%llu elapsed_ms=%llu calls=%llu path=",
                GetCurrentProcessId(), GetCurrentThreadId(), id_,
                previous_ ? previous_->id_ : 0, kind_, call, parentCall,
                step, item_, started, elapsed, calls_);
            std::wstring message(fields);
            message.append(path_);
            if (!detail.empty())
            {
                message += L" detail=";
                message.append(detail);
            }
            // File names must not inject extra log records.
            for (auto& ch : message)
                if (ch == L'\r' || ch == L'\n') ch = L' ';
            WriteDiagnosticLogEntry(message.c_str());
        }
        catch (...) {} // Preserve the provider's result/error even if logging fails.
        SetLastError(savedError);
    }

    static constexpr ULONGLONG kSlowMs = 250;
    inline static std::atomic<ULONGLONG> nextId_{0};
    inline static thread_local Context* current_ = nullptr;
    Context* previous_;
    const wchar_t* kind_;
    std::wstring_view path_;
    Clock clock_;
    ULONGLONG id_, started_, calls_ = 0, activeCall_ = 0;
    size_t item_ = 0;
};

template<class Callback>
decltype(auto) Call(const wchar_t* step, Callback&& callback,
    std::wstring_view detail = {})
{
    Context::Step scope(step, detail);
    return std::forward<Callback>(callback)();
}
}
