#pragma once
#include "window_support_protocol.h"

namespace snowdesktop::window_support
{
// A provider failure is scoped to one operation and one HWND lifetime. A short
// retry interval permits recovery without disabling unrelated observations.
class WindowOperationFailures
{
public:
    void Record(protocol::WindowIdentity target, Capability capability, Provider provider, DWORD error, ULONGLONG now) noexcept
    {
        if (!target.window || !error) return;
        for (auto& entry : entries_)
            if (Matches(entry, target, capability, provider)) { entry.error = error; entry.at = now; return; }
        entries_[next_++ % entries_.size()] = {target, capability, provider, error, now};
    }
    bool Blocked(protocol::WindowIdentity target, Capability capability, Provider provider, ULONGLONG now) const noexcept
    {
        for (const auto& entry : entries_)
            if (entry.error && Matches(entry, target, capability, provider) && now - entry.at < 5000) return true;
        return false;
    }
private:
    struct Entry
    {
        protocol::WindowIdentity target;
        Capability capability = Capability::None;
        Provider provider = Provider::Unavailable;
        DWORD error = 0;
        ULONGLONG at = 0;
    };
    static bool Matches(const Entry& entry, protocol::WindowIdentity target, Capability capability, Provider provider) noexcept
    {
        return entry.target.window == target.window && entry.target.process == target.process && entry.target.thread == target.thread &&
            entry.capability == capability && entry.provider == provider;
    }
    std::array<Entry, 64> entries_{};
    std::size_t next_ = 0;
};
}
