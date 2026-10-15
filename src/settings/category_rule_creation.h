#pragma once

#include "category_settings.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace snowdesktop
{
// The returned identity is the one inserted into the current edit input and
// later acknowledged by the presenter. A failed creation leaves it untouched.
inline std::optional<std::wstring> AppendCustomCategoryRule(CategorySettings& settings,
    std::wstring label, std::wstring extensions)
{
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        GUID guid{};
        std::array<wchar_t, 40> text{};
        if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 39)
            return std::nullopt;
        std::wstring id = L"custom-" + std::wstring(text.data() + 1, 36);
        if (std::any_of(settings.rules.begin(), settings.rules.end(),
                [&](const CategoryRule& rule) { return rule.id == id; }))
            continue;
        settings.rules.push_back({id, std::move(label), std::move(extensions)});
        return id;
    }
    return std::nullopt;
}
}
