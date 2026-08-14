#pragma once

#include <string_view>

namespace snowdesktop::widget_runtime
{
inline bool IsTrustedWidgetGestureCallback(
    std::string_view callback) noexcept
{
    return callback == "onClick" ||
        callback == "onPanelClick" ||
        callback == "onDoubleClick" ||
        callback == "onMouseDown" ||
        callback == "onPanelMouseDown" ||
        callback == "onMouseUp" ||
        callback == "onPanelMouseUp" ||
        callback == "onWheel" ||
        callback == "onPanelWheel";
}

class WidgetTrustedGestureState
{
public:
    bool Active() const noexcept
    {
        return active_;
    }

private:
    friend class WidgetTrustedGestureScope;
    bool active_ = false;
};

class WidgetTrustedGestureScope
{
public:
    WidgetTrustedGestureScope(
        WidgetTrustedGestureState& state, bool trusted) noexcept
        : state_(state), previous_(state.active_)
    {
        state_.active_ = trusted;
    }

    ~WidgetTrustedGestureScope()
    {
        state_.active_ = previous_;
    }

    WidgetTrustedGestureScope(
        const WidgetTrustedGestureScope&) = delete;
    WidgetTrustedGestureScope& operator=(
        const WidgetTrustedGestureScope&) = delete;

private:
    WidgetTrustedGestureState& state_;
    bool previous_ = false;
};
}
