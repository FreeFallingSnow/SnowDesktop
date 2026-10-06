#pragma once
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace snowdesktop
{
// The UI owner clears this index whenever current result rows are replaced.
// It stores row numbers, never references into a resizable result vector.
template<class Key>
class IconRowIndex
{
public:
    void Clear() { rows_.clear(); }
    void Add(const Key& key, std::size_t row) { rows_[key].push_back(row); }
    template<class Visitor>
    void Visit(const Key& key, std::size_t rowCount, Visitor visitor) const
    {
        const auto found = rows_.find(key);
        if (found == rows_.end()) return;
        for (const auto row : found->second)
            if (row < rowCount) visitor(row);
    }
private:
    std::unordered_map<Key, std::vector<std::size_t>> rows_;
};
}
