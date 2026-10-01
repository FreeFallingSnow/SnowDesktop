#pragma once
#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

namespace snowdesktop
{
// Single-owner cache. Values (including GPU resources) are released on the
// caller's thread. Copy/move is disabled because nodes borrow recency iterators.
template<class Key, class Value>
class BoundedLruCache
{
public:
    explicit BoundedLruCache(std::size_t capacity) : capacity_(capacity) {}
    BoundedLruCache(const BoundedLruCache&) = delete;
    BoundedLruCache& operator=(const BoundedLruCache&) = delete;
    Value* Find(const Key& key)
    {
        const auto found = values_.find(key);
        if (found == values_.end()) return nullptr;
        recency_.splice(recency_.begin(), recency_, found->second.position);
        return &found->second.value;
    }
    Value* Insert(const Key& key, Value value)
    {
        if (capacity_ == 0) return nullptr;
        if (auto* existing = Find(key)) { *existing = std::move(value); return existing; }
        recency_.push_front(key);
        try { values_.emplace(key, Node{std::move(value), recency_.begin()}); }
        catch (...) { recency_.pop_front(); throw; }
        if (values_.size() > capacity_)
        {
            values_.erase(recency_.back());
            recency_.pop_back();
        }
        return &values_.find(key)->second.value;
    }
    void Clear() { values_.clear(); recency_.clear(); }
    std::size_t Size() const noexcept { return values_.size(); }
private:
    struct Node { Value value; typename std::list<Key>::iterator position; };
    const std::size_t capacity_;
    std::list<Key> recency_;
    std::unordered_map<Key, Node> values_;
};
}
