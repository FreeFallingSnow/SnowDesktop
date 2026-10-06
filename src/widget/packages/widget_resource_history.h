#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetResourcePoint
{
    std::int64_t timestampMs = 0;
    std::optional<double> primary, secondary;
    bool operator==(const WidgetResourcePoint&) const = default;
};
// Host-internal history, fed by the existing shared sampling worker. At most
// one point per second and 61 points per stream; missing values stay missing.
class WidgetResourceHistory
{
public:
    static constexpr std::int64_t WindowMs = 60000;
    void Append(std::string_view topic, std::string_view identity, WidgetResourcePoint point)
    {
        if (point.timestampMs <= 0) return;
        for (auto* value : {&point.primary, &point.secondary})
            if (*value && (!std::isfinite(**value) || **value < 0)) value->reset();
        auto& stream = streams_[Key(topic, identity)];
        if (!stream.empty() && point.timestampMs < stream.back().timestampMs) stream.clear();
        if (!stream.empty() && point.timestampMs / 1000 == stream.back().timestampMs / 1000) stream.back() = point;
        else stream.push_back(point);
        while (!stream.empty() && (stream.front().timestampMs < point.timestampMs - WindowMs || stream.size() > 61))
            stream.pop_front();
    }
    std::vector<WidgetResourcePoint> Read(std::string_view topic, std::string_view identity = {}) const
    {
        const auto found = streams_.find(Key(topic, identity));
        return found == streams_.end() ? std::vector<WidgetResourcePoint>{} :
            std::vector<WidgetResourcePoint>(found->second.begin(), found->second.end());
    }
    void Clear(std::string_view topic)
    {
        const auto prefix = Key(topic, {});
        std::erase_if(streams_, [&](const auto& item) { return item.first.starts_with(prefix); });
    }
    void Retain(std::string_view topic, const std::vector<std::string>& identities)
    {
        const auto prefix = Key(topic, {});
        std::erase_if(streams_, [&](const auto& item) {
            return item.first.starts_with(prefix) && std::find(identities.begin(), identities.end(),
                item.first.substr(prefix.size())) == identities.end();
        });
    }
    void Clear() { streams_.clear(); }
private:
    static std::string Key(std::string_view topic, std::string_view identity) { return std::string(topic) + '\n' + std::string(identity); }
    std::map<std::string, std::deque<WidgetResourcePoint>> streams_;
};
}
