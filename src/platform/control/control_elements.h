#pragma once

#include "control_protocol.h"
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::control
{
enum class ElementBackend { Pointer, Lua, NavigationSearch, WidgetSearch, MenuText };

struct Element
{
    std::string id, parent, surface, role, name, value;
    RECT bounds{}, clip{}; // Physical screen pixels, including negative origins.
    HWND window = nullptr; // Internal routing only; never serialized.
    bool enabled = true, visible = true, focused = false, selected = false;
    bool password = false;
    std::optional<bool> checked, expanded;
    std::vector<std::string> actions;
    ElementBackend backend = ElementBackend::Pointer;
    std::wstring widgetId;
    std::string nodeKey;
};

struct ElementSnapshot
{
    std::vector<Element> elements;
    std::vector<std::string> coveredSurfaces, pendingSurfaces;
    std::string revision;
    void Finish();
    Value ToJson() const;
    const Element& Resolve(std::string_view id, std::string_view action = {}) const;
};

std::string ElementKey(std::string_view text);
RECT VisibleBounds(const Element& element);
POINT ElementCenter(const Element& element);
bool Supports(const Element& element, std::string_view action);
Value ElementJson(const Element& element);
}
