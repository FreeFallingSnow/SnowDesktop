#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace snowdesktop::pending_drop_rules
{

template <typename T, typename Predicate>
inline std::vector<T> ExtractMatching(
    std::vector<T>& values,
    Predicate matches)
{
    std::vector<T> extracted;
    for (auto it = values.begin(); it != values.end();)
    {
        if (!matches(*it))
        {
            ++it;
            continue;
        }
        extracted.push_back(std::move(*it));
        it = values.erase(it);
    }
    return extracted;
}

template <typename T>
inline void InsertAt(
    std::vector<T>& values,
    size_t insertIndex,
    std::vector<T> inserted)
{
    const size_t insertAt = std::min(
        insertIndex, values.size());
    values.insert(
        values.begin() +
            static_cast<std::ptrdiff_t>(insertAt),
        std::make_move_iterator(inserted.begin()),
        std::make_move_iterator(inserted.end()));
}

} // namespace snowdesktop::pending_drop_rules
