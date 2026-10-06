#pragma once

#include "shell_refresh_snapshot.h"
#include <memory>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <string>

namespace snowdesktop::shell_refresh
{
// A drag may read its destination, but must keep the source containers alive.
// Retain only the latest result per folder for model publication after release.
class FolderReadDelivery
{
public:
    template<class Preview, class Apply>
    void Deliver(const std::wstring& key, std::uint64_t version,
        std::shared_ptr<Snapshot> snapshot, bool deferModel, Preview preview, Apply apply)
    {
        pending_.erase(key);
        if (!snapshot) return;
        if (!deferModel) { apply(*snapshot); return; }
        pending_.emplace(key, Pending{version, snapshot});
        preview(*snapshot);
    }

    template<class CurrentVersion, class Apply>
    void Drain(CurrentVersion currentVersion, Apply apply)
    {
        auto pending = std::exchange(pending_, {});
        for (auto& [key, result] : pending)
            if (currentVersion(key) == result.version) apply(*result.snapshot);
    }

    void Clear() { pending_.clear(); }

private:
    struct Pending { std::uint64_t version; std::shared_ptr<Snapshot> snapshot; };
    std::unordered_map<std::wstring, Pending> pending_;
};
}
