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
enum class Role { Text, Button, Toggle, Slider, ListItem, Icon, Card, Separator, Chart, Image };
struct Image
{
    unsigned width = 0, height = 0, stride = 0;
    std::vector<std::uint32_t> pixels;
};
struct Node
{
    std::string id;
    Role role = Role::Text;
    D2D1_RECT_F bounds{}, clip{};
    std::wstring text, detail, glyph, tooltip;
    float fontSize = 14, value = 0;
    bool enabled = true, selected = false, accent = false, centered = false, bold = false;
    bool outlined = false, secondary = false, charging = false;
    std::shared_ptr<const Image> image;
    std::vector<std::vector<D2D1_POINT_2F>> paths;
    bool Interactive() const;
};
struct Palette
{
    D2D1_COLOR_F text{}, secondary{}, accent{}, accentText{}, hover{}, control{}, stroke{};
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
    bool Focus(std::string_view);
    std::vector<widget_runtime::InteractionRegion> AccessibilityRegions() const;
    std::string Identity(std::string_view regionKey) const;
    std::string Pressed() const;
    const std::string& Focused() const { return focused_; }
    bool Dragging() const { return dragging_; }
private:
    widget_runtime::WidgetInteractionRegions regions_;
    std::map<std::string, std::string> identities_;
    std::string focused_;
    D2D1_POINT_2F origin_{};
    bool right_ = false, dragging_ = false;
    InputResult Resolve(const std::optional<widget_runtime::InteractionResolvedAction>&) const;
};
HRESULT Draw(ID2D1DeviceContext*, IDWriteFactory*, const Scene&, const Palette&,
    std::string_view hovered = {}, std::string_view focused = {}, std::string_view pressed = {});
}
