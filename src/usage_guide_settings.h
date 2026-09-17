#pragma once
#include "usage_guide.h"
#include "settings_search_index.h"
#include <algorithm>
namespace snowdesktop::usage_guide
{
inline bool IsLessonVisible(const Lesson& lesson,
    const std::vector<StaticSettingSearchDescriptor>& settings)
{
    if (lesson.practice) return true;
    // The developer article opens the existing opt-in entry, even before the
    // Developer Tools navigation item is enabled.
    const std::string_view focus = lesson.topic == Topic::Develop ? "widgets.developer" : lesson.settingsFocus;
    return std::any_of(settings.begin(), settings.end(), [&](const auto& entry) {
        return entry.visible && entry.focusId == focus;
    });
}
}
