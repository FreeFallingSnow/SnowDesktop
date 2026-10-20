#pragma once

#include "widget_media_contract.h"
#include "system/controls/system_control_windows.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <objbase.h>
#include <stdexcept>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

namespace snowdesktop::widget_runtime
{
inline std::string NewMediaSessionToken()
{
    static const std::string epoch = [] {
        GUID value{}; wchar_t text[40]{};
        if (FAILED(CoCreateGuid(&value)) || !StringFromGUID2(value, text, 40))
            throw std::runtime_error("cannot allocate a media session identity");
        std::string result;
        for (const wchar_t character : text) if (character) result.push_back(static_cast<char>(character));
        return "media-session-" + result + ":";
    }();
    static std::atomic_uint64_t serial{0};
    return epoch + std::to_string(++serial);
}
// Tokens belong to retained session objects. Enumeration order, current-player
// changes and an identical application name cannot transfer a token.
template<class Session> class MediaSessionRegistry
{
public:
    struct Entry { std::string id; Session session; };
    explicit MediaSessionRegistry(std::function<bool(const Session&, const Session&)> same = std::equal_to<Session>{})
        : same_(std::move(same)) {}
    std::vector<Entry> Reconcile(const std::vector<Session>& sessions)
    {
        std::lock_guard lock(mutex_);
        std::vector<Entry> next;
        for (const auto& session : sessions)
        {
            if (!session || next.size() >= MaximumExposedMediaSessions) continue;
            if (std::any_of(next.begin(), next.end(), [&](const auto& item) { return same_(item.session, session); })) continue;
            const auto previous = std::find_if(entries_.begin(), entries_.end(),
                [&](const auto& item) { return same_(item.session, session); });
            next.push_back(previous == entries_.end()
                ? Entry{NewMediaSessionToken(), session} : *previous);
        }
        entries_ = next;
        return next;
    }
private:
    std::mutex mutex_;
    std::function<bool(const Session&, const Session&)> same_;
    std::vector<Entry> entries_;
};

using NativeMediaSession = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using NativeMediaSessionManager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
inline NativeMediaSessionManager AcquireMediaSessionManager(const system_control::Cancellation& cancellation)
{
    static std::mutex mutex;
    static NativeMediaSessionManager manager{nullptr};
    std::unique_lock lock(mutex, std::defer_lock);
    // RequestAsync can be outstanding on another consumer. Waiting for its
    // initialization must still honor this caller's own cancellation/deadline.
    while (!lock.try_lock())
    {
        if (cancellation.Stop()) throw winrt::hresult_canceled();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (cancellation.Stop()) throw winrt::hresult_canceled();
    if (!manager)
        manager = system_control::windows::Await(NativeMediaSessionManager::RequestAsync(), cancellation);
    return manager;
}
inline bool SameMediaSession(const NativeMediaSession& first, const NativeMediaSession& second)
{
    return first && second && first.as<::IUnknown>().get() == second.as<::IUnknown>().get();
}
template<class Entry> auto ResolveMediaSessionToken(const std::vector<Entry>& inventory, std::string_view id)
{
    const auto found = std::find_if(inventory.begin(), inventory.end(),
        [&](const auto& entry) { return entry.id == id; });
    using Session = decltype(Entry::session);
    return found == inventory.end() ? Session{nullptr} : found->session;
}
inline auto SnapshotMediaSessionInventory(const NativeMediaSessionManager& manager)
{
    // The Windows session class is Agile. Retaining it also prevents COM
    // identity-address reuse until the session has been removed from inventory.
    static MediaSessionRegistry<NativeMediaSession> registry{SameMediaSession};
    std::vector<NativeMediaSession> sessions;
    if (const auto current = manager.GetCurrentSession()) sessions.push_back(current);
    for (const auto& session : manager.GetSessions()) sessions.push_back(session);
    return registry.Reconcile(sessions);
}
inline NativeMediaSession ResolveMediaSessionIdentity(const NativeMediaSessionManager& manager, std::string_view id)
{
    if (id.empty()) return manager.GetCurrentSession();
    const auto inventory = SnapshotMediaSessionInventory(manager);
    return ResolveMediaSessionToken(inventory, id);
}
}
