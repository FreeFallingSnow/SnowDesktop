#pragma once

#include "../background_work.h"

namespace snowdesktop::dock_icon_work
{
// Dock first pixels must not wait behind Shell identity/image-list providers.
// UI-owned requests coalesce until both lanes finish, including local misses.
// Both lanes retain BackgroundWork's cancellation guarantees.
class Work final
{
public:
    explicit Work(unsigned workers = 2) : local_(workers), shell_(workers) {}

    template<class Local, class Shell, class Apply>
    void Submit(std::wstring key, Local local, Shell shell, Apply apply, HWND window, UINT message)
    {
        if (!pending_.emplace(key, 2u).second) return;
        const auto requestKey = key;
        if (!local_.Submit(key, std::move(local),
            [this, requestKey, apply](auto result) mutable {
                Finished(requestKey);
                apply(std::move(result), false);
            }, window, message)) Finished(requestKey);
        if (!shell_.Submit(std::move(key), std::move(shell),
            [this, requestKey, apply](auto result) mutable {
                Finished(requestKey);
                apply(std::move(result), true);
            }, window, message)) Finished(requestKey);
    }

    void Drain() { local_.Drain(); shell_.Drain(); }
    void Cancel(const std::wstring& prefix)
    {
        local_.Cancel(prefix);
        shell_.Cancel(prefix);
        std::erase_if(pending_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
    }
    void Stop() { local_.Stop(); shell_.Stop(); pending_.clear(); }

private:
    void Finished(const std::wstring& key)
    {
        const auto found = pending_.find(key);
        if (found != pending_.end() && --found->second == 0) pending_.erase(found);
    }
    BackgroundWork local_, shell_;
    std::unordered_map<std::wstring, unsigned> pending_;
};
}
