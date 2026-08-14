#pragma once

#include "widget_interaction_region.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class ViewNodeType
{
    Box,
    Row,
    Column,
    Stack,
    Text,
    Button,
    Spacer,
};

enum class ViewLengthKind
{
    Auto,
    Fill,
    Fixed,
};

struct ViewLength
{
    ViewLengthKind kind = ViewLengthKind::Auto;
    float value = 0.0f;
};

enum class ViewAlignment
{
    Auto,
    Start,
    Center,
    End,
    Stretch,
};

enum class ViewJustification
{
    Start,
    Center,
    End,
    SpaceBetween,
};

enum class ViewTextAlignment
{
    Start,
    Center,
    End,
};

struct ViewRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ViewStyle
{
    std::optional<std::uint32_t> background;
    std::optional<std::uint32_t> foreground;
    std::optional<std::uint32_t> borderColor;
    std::optional<float> borderWidth;
    std::optional<float> cornerRadius;
    std::optional<float> opacity;
};

struct ViewNode
{
    ViewNodeType type = ViewNodeType::Box;
    std::string key;
    std::string text;
    ViewLength width{ ViewLengthKind::Fill, 0.0f };
    ViewLength height{};
    float padding = 0.0f;
    float gap = 0.0f;
    float flexGrow = 0.0f;
    ViewAlignment alignItems = ViewAlignment::Stretch;
    ViewAlignment alignSelf = ViewAlignment::Auto;
    ViewJustification justifyContent = ViewJustification::Start;
    ViewTextAlignment textAlign = ViewTextAlignment::Start;
    float fontSize = 15.0f;
    bool bold = false;
    bool visible = true;
    bool enabled = true;
    std::string cursor;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    ViewStyle style;
    ViewStyle hoverStyle;
    ViewStyle pressedStyle;
    std::map<std::string, InteractionAction, std::less<>> events;
    std::vector<ViewNode> children;
    ViewRect frame;
};

struct ViewTreeLimits
{
    static constexpr std::size_t MaximumNodes = 512;
    static constexpr std::size_t MaximumDepth = 32;
    static constexpr std::size_t MaximumTextBytes = 4096;
    static constexpr std::size_t MaximumTotalTextBytes = 64 * 1024;
};

bool ValidateAndLayoutViewTree(ViewNode& root, float width, float height,
    std::string& error);
bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error);
const char* ViewNodeTypeName(ViewNodeType type) noexcept;
}
