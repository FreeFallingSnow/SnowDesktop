#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::shell_refresh
{
// UI-owned requests rejected by a bounded worker. A later maintenance pass
// retries them even when no further filesystem notification arrives.
class FolderReadRetries
{
public:
    void Remember(const std::wstring& key, const std::wstring& path)
    {
        paths_.insert_or_assign(key, path);
    }

    void Forget(const std::wstring& key) { paths_.erase(key); }
    void Clear() { paths_.clear(); }
    bool Empty() const { return paths_.empty(); }

    template<class Submit>
    void Retry(Submit submit, std::size_t limit = 32)
    {
        if (limit == 0) return;
        std::vector<std::wstring> paths;
        paths.reserve(std::min(limit, paths_.size()));
        for (const auto& [key, path] : paths_)
        {
            paths.push_back(path);
            if (paths.size() == limit) break;
        }
        for (const auto& path : paths)
            if (!submit(path)) break;
    }

private:
    std::unordered_map<std::wstring, std::wstring> paths_;
};
}
