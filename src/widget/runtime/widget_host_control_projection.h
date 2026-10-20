#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
// Keep controls and the accepted scene in one frame transaction. Resolve the
// owner again on rollback because a callback may move or remove its widget.
template<class Control>
class HostControlFrame
{
public:
    using Lookup = std::function<std::vector<Control>*()>;
    HostControlFrame(Lookup lookup, std::string_view surface)
        : lookup_(std::move(lookup))
    {
        if (auto* controls = lookup_())
        {
            previous_ = *controls;
            std::erase_if(*controls, [surface](const auto& control) {
                return control.surface == surface;
            });
        }
    }
    ~HostControlFrame() { Restore(); }
    HostControlFrame(const HostControlFrame&) = delete;
    HostControlFrame& operator=(const HostControlFrame&) = delete;
    void Commit() noexcept { completed_ = true; }
    void Restore() noexcept
    {
        if (completed_) return;
        if (auto* controls = lookup_()) controls->swap(previous_);
        completed_ = true;
    }
private:
    Lookup lookup_;
    std::vector<Control> previous_;
    bool completed_ = false;
};

template<class Focus>
bool HostInputFocusMatches(const Focus& focus, std::wstring_view widgetId,
    std::string_view key, std::string_view surface) noexcept
{
    return focus.active && focus.widgetId == widgetId && focus.id == key &&
        focus.surface == surface;
}

template<class Control>
struct HostInputAccessibilityProjection
{
    const Control* control = nullptr;
    std::string valueText;
    bool focused = false;
    std::size_t cursor = 0, anchor = 0;
};

template<class Control, class Focus, class ReadStored, class EncodeText>
std::optional<HostInputAccessibilityProjection<Control>>
ProjectHostInputAccessibility(const std::vector<Control>& controls,
    const Focus& focus, std::wstring_view widgetId, std::string_view key,
    std::string_view surface, ReadStored readStored, EncodeText encodeText)
{
    const auto found = std::find_if(controls.rbegin(), controls.rend(),
        [key, surface](const auto& control) {
            return control.type == Control::Type::Input &&
                control.surface == surface && control.id == key;
        });
    if (found == controls.rend()) return std::nullopt;
    HostInputAccessibilityProjection<Control> result;
    result.control = &*found;
    result.focused = HostInputFocusMatches(focus, widgetId, key, surface);
    result.valueText = result.focused ? encodeText(focus.text)
        : found->controlled ? found->controlledText : readStored(found->storageKey);
    if (result.focused)
    {
        result.cursor = focus.cursor;
        result.anchor = focus.selectionAnchor;
    }
    return result;
}
}
