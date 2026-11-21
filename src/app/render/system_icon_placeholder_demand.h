#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop
{
// Only actual cache misses enter this UI-thread index. A complete owner draw
// replaces its own interests; other tiles/windows keep their last pixels.
class SystemIconPlaceholderDemand
{
public:
    enum class Host { DesktopItem, Widget, Dock, Popup, Navigation, Drag };
    struct Owner
    {
        Host host;
        std::uintptr_t token;
        std::wstring id;
        std::uint64_t generation;
    };
    class Paint
    {
    public:
        Paint(SystemIconPlaceholderDemand& demand, Host host, const void* token = nullptr,
            std::wstring_view id = {}, std::uint64_t generation = 0)
            : demand_(demand), host_(host), token_(reinterpret_cast<std::uintptr_t>(token)),
              id_(id), generation_(generation), previous_(std::exchange(demand.current_, this))
        {
            if (!demand.waiting_.empty()) demand.waiting_.erase({host_, token_});
        }
        ~Paint() { demand_.current_ = previous_; }
        Paint(const Paint&) = delete;
        Paint& operator=(const Paint&) = delete;
    private:
        friend class SystemIconPlaceholderDemand;
        SystemIconPlaceholderDemand& demand_;
        Host host_;
        std::uintptr_t token_;
        std::wstring_view id_;
        std::uint64_t generation_;
        Paint* previous_;
    };

    void Miss(std::uint64_t key)
    {
        ++missRevision_;
        if (!current_) return;
        const auto owner = std::pair{current_->host_, current_->token_};
        auto [entry, inserted] = waiting_.try_emplace(owner);
        if (inserted)
        {
            entry->second.id = current_->id_;
            entry->second.generation = current_->generation_;
        }
        auto& keys = entry->second.keys;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }

    std::uint64_t MissRevision() const { return missRevision_; }
    std::vector<Owner> Take(std::uint64_t key)
    {
        std::vector<Owner> result;
        for (auto it = waiting_.begin(); it != waiting_.end();)
        {
            auto& wait = it->second;
            if (std::erase(wait.keys, key))
                result.push_back({it->first.first, it->first.second, wait.id, wait.generation});
            if (wait.keys.empty()) it = waiting_.erase(it);
            else ++it;
        }
        return result;
    }
private:
    struct Wait { std::wstring id; std::uint64_t generation = 0; std::vector<std::uint64_t> keys; };
    std::map<std::pair<Host, std::uintptr_t>, Wait> waiting_;
    Paint* current_ = nullptr;
    std::uint64_t missRevision_ = 0;
};
}
