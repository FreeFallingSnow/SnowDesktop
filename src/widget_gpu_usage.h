#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
inline std::string WidgetGpuAdapterId(std::uint64_t luid)
{
    // A Windows-session identity, independent of DXGI enumeration order.
    return "adapter-" + std::to_string(luid);
}

// PDH reports one sample per process and engine. Only samples belonging to
// the same physical engine are additive; independent engines run in parallel.
class WidgetGpuUsageAccumulator
{
public:
    struct Engine
    {
        std::uint64_t luid = 0;
        std::uint32_t physical = 0, index = 0, samples = 0;
        std::wstring type;
        double rawTotal = 0, usagePercent = 0;
    };
    void AddSample(const wchar_t* instance, double usagePercent)
    {
        if (!instance || !std::isfinite(usagePercent) || usagePercent < 0.0)
            return;
        std::wstring_view name(instance);
        const auto marker = name.find(L"luid_0x");
        if (marker == std::wstring_view::npos) return;
        name.remove_prefix(marker);
        std::uint32_t high = 0, low = 0, physical = 0, engine = 0;
        if (!ReadNumber(name, L"luid_0x", 16, high) ||
            !ReadNumber(name, L"_0x", 16, low) ||
            !ReadNumber(name, L"_phys_", 10, physical) ||
            !ReadNumber(name, L"_eng_", 10, engine) ||
            !name.starts_with(L"_engtype_") || name.size() <= 9)
            return;

        const auto luid = (static_cast<std::uint64_t>(high) << 32) | low;
        auto& engineUsage = usageByEngine_[{ luid, physical, engine }];
        engineUsage.luid = luid; engineUsage.physical = physical; engineUsage.index = engine;
        if (!engineUsage.samples) engineUsage.type = name.substr(9);
        ++engineUsage.samples;
        engineUsage.rawTotal = usagePercent > std::numeric_limits<double>::max() - engineUsage.rawTotal ?
            std::numeric_limits<double>::max() : engineUsage.rawTotal + usagePercent;
        engineUsage.usagePercent = std::min(100.0, engineUsage.rawTotal);
        auto& adapterUsage = usageByLuid_[luid];
        adapterUsage = std::max(adapterUsage, engineUsage.usagePercent);
    }

    std::optional<double> UsagePercent(std::uint64_t luid) const
    {
        const auto found = usageByLuid_.find(luid);
        if (found == usageByLuid_.end()) return std::nullopt;
        return found->second;
    }
    std::vector<Engine> Engines(std::uint64_t luid) const
    {
        std::vector<Engine> result;
        for (const auto& [key, engine] : usageByEngine_)
        { (void)key; if (engine.luid == luid) result.push_back(engine); }
        return result;
    }
    static std::optional<std::uint64_t> MemoryLuid(const wchar_t* instance)
    {
        if (!instance) return {};
        std::wstring_view name(instance);
        std::uint32_t high = 0, low = 0, physical = 0;
        if (!ReadNumber(name, L"luid_0x", 16, high) || !ReadNumber(name, L"_0x", 16, low) ||
            !ReadNumber(name, L"_phys_", 10, physical) || !name.empty()) return {};
        return (static_cast<std::uint64_t>(high) << 32) | low;
    }

private:
    static bool ReadNumber(std::wstring_view& text,
        std::wstring_view prefix, std::uint32_t base, std::uint32_t& value)
    {
        if (!text.starts_with(prefix)) return false;
        text.remove_prefix(prefix.size());
        std::size_t length = 0;
        value = 0;
        while (length < text.size())
        {
            const wchar_t character = text[length];
            std::uint32_t digit = base;
            if (character >= L'0' && character <= L'9')
                digit = static_cast<std::uint32_t>(character - L'0');
            else if (character >= L'a' && character <= L'f')
                digit = static_cast<std::uint32_t>(character - L'a') + 10;
            else if (character >= L'A' && character <= L'F')
                digit = static_cast<std::uint32_t>(character - L'A') + 10;
            if (digit >= base) break;
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / base)
                return false;
            value = value * base + digit;
            ++length;
        }
        text.remove_prefix(length);
        return length != 0;
    }

    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, Engine>
        usageByEngine_;
    std::unordered_map<std::uint64_t, double> usageByLuid_;
};

class WidgetGpuMemoryAccumulator
{
public:
    void AddSample(const wchar_t* instance, std::int64_t bytes)
    {
        const auto luid = WidgetGpuUsageAccumulator::MemoryLuid(instance);
        if (!luid || bytes < 0) return;
        auto [found, inserted] = values_.try_emplace(*luid, std::uint64_t{0});
        (void)inserted;
        auto& value = found->second;
        if (!value) return; // An overflow cannot be repaired by later rows.
        const auto next = static_cast<std::uint64_t>(bytes);
        if (*value > std::numeric_limits<std::uint64_t>::max() - next) value.reset();
        else *value += next;
    }
    std::optional<std::uint64_t> UsageBytes(std::uint64_t luid) const
    {
        const auto found = values_.find(luid);
        return found == values_.end() ? std::nullopt : found->second;
    }
private:
    std::unordered_map<std::uint64_t, std::optional<std::uint64_t>> values_;
};
}
