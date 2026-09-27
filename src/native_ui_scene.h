#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include "widget_interaction_region.h"

namespace snowdesktop::native_ui
{
// Host-internal controls. No XAML, device service, HWND ownership or Lua API.
// A renderer and input controller consume the same immutable hit rectangles.
enum class Role { Text, Button, Toggle, Slider, ListItem, Icon, Card, Separator, Chart, Image, Scrollbar };
struct Image
{
    unsigned width = 0, height = 0, stride = 0;
    std::vector<std::uint32_t> pixels;
};
struct Node
{
    std::string id;
    // Related controls share their card's pointer highlight without changing
    // their independent hit, keyboard or accessibility identities.
    std::string hoverGroup;
    Role role = Role::Text;
    D2D1_RECT_F bounds{}, clip{};
    std::wstring text, detail, glyph, tooltip, accessibilityLabel;
    float fontSize = 14, value = 0;
    bool enabled = true, selected = false, accent = false, centered = false, trailing = false, bold = false;
    bool outlined = false, secondary = false, charging = false, positiveGlyph = false;
    bool wrap = false, joinLeft = false, joinRight = false, switchStyle = false, busy = false, batteryStyle = false;
    std::shared_ptr<const Image> image;
    std::vector<std::vector<D2D1_POINT_2F>> paths;
    // Style belongs to each continuous sample segment, including after gaps.
    std::vector<bool> dashedPaths;
    bool fillPaths = false, chartGrid = true;
    bool Interactive() const;
};
struct Palette
{
    D2D1_COLOR_F text{}, secondary{}, accent{}, accentText{}, hover{}, control{}, stroke{};
    float cornerRadius = 10;
    bool highContrast = false;
};
struct Scene
{
    float width = 440, height = 300;
    std::vector<D2D1_RECT_F> cards;
    std::vector<Node> nodes;
    const Node* Find(std::string_view id) const;
    const Node* Hit(D2D1_POINT_2F point, bool interactiveOnly = true) const;
    bool SameContent(const Scene&) const;
};
struct InputResult
{
    enum class Kind { None, Invoke, Context, Value, Drag } kind = Kind::None;
    std::string id;
    float value = 0;
};
class Input
{
public:
    bool Press(const Scene&, D2D1_POINT_2F, bool right = false);
    InputResult Move(const Scene&, D2D1_POINT_2F);
    InputResult Release(const Scene&, D2D1_POINT_2F, bool right = false);
    InputResult Key(const Scene&, unsigned key, bool shift);
    void Cancel();
    void Sync(const Scene&);
    // Logical focus includes clipped nodes; the host reveals Focused() before
    // drawing focus or dispatching an action. Pointer input remains clipped.
    bool Focus(std::string_view, bool visible = true);
    // Mouse input keeps logical/UIA focus without painting keyboard cues.
    bool PointerInput();
    // Includes offscreen nodes. Host UIA collection may need bounded batches.
    std::vector<widget_runtime::InteractionRegion> AccessibilityRegions() const;
    std::string Identity(std::string_view regionKey) const;
    std::string Pressed() const;
    const std::string& Focused() const { return focused_; }
    std::string_view VisibleFocus() const { return focusVisible_ ? std::string_view(focused_) : std::string_view{}; }
    bool Dragging() const { return dragging_; }
private:
    widget_runtime::WidgetInteractionRegions regions_;
    std::vector<widget_runtime::InteractionRegion> semanticRegions_;
    std::vector<std::string> focusable_;
    std::map<std::string, std::string> identities_;
    std::string focused_;
    D2D1_POINT_2F origin_{};
    bool right_ = false, dragging_ = false;
    bool focusVisible_ = false;
    InputResult Resolve(const std::optional<widget_runtime::InteractionResolvedAction>&) const;
};
HRESULT Draw(ID2D1DeviceContext*, IDWriteFactory*, const Scene&, const Palette&,
    std::string_view hovered = {}, std::string_view focused = {}, std::string_view pressed = {});
}
