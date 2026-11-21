#pragma once

#include "widget/runtime/widget_instance.h"

#include <string>
#include <string_view>

bool HasDeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept;

const snowdesktop::widget_runtime::ViewNode*
DeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept;

const snowdesktop::widget_runtime::ViewNode* FindViewNodeByKey(
    const snowdesktop::widget_runtime::ViewNode& node,
    std::string_view key) noexcept;

void QueueDeclarativeVisualFrame(
    LuaWidget& widget, std::string_view surface) noexcept;

RECT BoundsForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept;

std::string ViewVirtualKeyName(WPARAM key);

bool HostControlContainsPoint(
    const LuaWidget::HostControl& control, POINT point) noexcept;
