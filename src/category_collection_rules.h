#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::category_collection_rules
{
inline bool IsProgramItem(std::wstring_view extension,
    bool applicationShortcut,
    const std::vector<std::wstring>& configuredExtensions)
{
    std::wstring upper(extension);
    for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
    // Removing a matching rule must not bypass the collection opt-in.
    return applicationShortcut || upper == L".EXE" || upper == L".MSI" ||
        upper == L".BAT" || upper == L".CMD" || upper == L".LNK" || upper == L".URL" ||
        std::find(configuredExtensions.begin(), configuredExtensions.end(), upper) !=
            configuredExtensions.end();
}

inline std::vector<std::wstring> ResolveTabOrder(
    const std::vector<std::wstring>& defaults,
    const std::vector<std::wstring>& saved)
{
    std::vector<std::wstring> result;
    auto append = [&](const std::wstring& id) {
        if (std::find(defaults.begin(), defaults.end(), id) != defaults.end() &&
            std::find(result.begin(), result.end(), id) == result.end())
            result.push_back(id);
    };
    for (const auto& id : saved) append(id);
    for (const auto& id : defaults) append(id);
    return result;
}

inline bool MoveTab(std::vector<std::wstring>& order,
    const std::wstring& source, const std::wstring& target)
{
    const auto from = std::find(order.begin(), order.end(), source);
    const auto to = std::find(order.begin(), order.end(), target);
    if (from == order.end() || to == order.end() || from == to) return false;
    const auto destination = to - order.begin();
    const auto value = *from;
    order.erase(from);
    order.insert(order.begin() + destination, value);
    return true;
}
}
