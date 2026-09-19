#pragma once
#include "shell_extension_menu.h"
#include <algorithm>

namespace snowdesktop::shell_extensions
{
// Settings-only display data. Native commands and sessions never enter this
// cache. A short lifetime bounds changes inside third-party applications that
// do not emit Shell/registry notifications; Refresh explicitly bypasses it.
class CatalogueCache
{
  public:
    static constexpr ULONGLONG LifetimeMs = 30000;
    const Reply *Find(const Request &request, std::uint64_t generation, ULONGLONG now)
    {
        std::erase_if(rows_, [&](const auto &row) { return row.generation != generation || now >= row.expires; });
        const auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto &row) { return row.request == request; });
        return found == rows_.end() ? nullptr : &found->reply;
    }
    void Store(const Request &request, const Reply &reply, std::uint64_t generation, ULONGLONG now)
    {
        // Custom inspected objects remain live queries, not samples shared by
        // file extension or folder type.
        if (!request.catalogueOnly || !request.paths.empty() || !reply.ok) return;
        std::erase_if(rows_, [&](const auto &row) { return row.request == request; });
        if (rows_.size() >= 4) rows_.erase(rows_.begin());
        auto snapshot = reply;
        StripCommands(snapshot.entries);
        rows_.push_back({request, std::move(snapshot), generation, now + LifetimeMs});
    }
    void Clear() { rows_.clear(); }
  private:
    static void StripCommands(std::vector<Entry> &entries)
    {
        for (auto &entry : entries)
        {
            entry.token = 0;
            StripCommands(entry.children);
        }
    }
    struct Row { Request request; Reply reply; std::uint64_t generation; ULONGLONG expires; };
    std::vector<Row> rows_;
};
} // namespace snowdesktop::shell_extensions
