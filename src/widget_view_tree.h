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
    Image,
    Button,
    Toggle,
    Checkbox,
    Icon,
    IconButton,
    Shape,
    Badge,
    Divider,
    ProgressBar,
    ProgressRing,
    Meter,
    Sparkline,
    LineChart,
    BarChart,
    Waveform,
    Spectrum,
    Spacer,
};

enum class ViewShapeKind
{
    Rectangle,
    RoundedRectangle,
    Circle,
    Ellipse,
};

enum class ViewIconFont
{
    FontAwesome,
    Fluent,
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

enum class ViewImageFit
{
    Fill,
    Contain,
    Cover,
    None,
};

enum class ViewImageAlignment
{
    Start,
    Center,
    End,
};

enum class ViewImageInterpolation
{
    Nearest,
    Linear,
};

enum class ViewOrientation
{
    Horizontal,
    Vertical,
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
    std::string imageResourceName;
    std::string fontResourceName;
    std::string alt;
    ViewLength width{ ViewLengthKind::Fill, 0.0f };
    ViewLength height{};
    float padding = 0.0f;
    float gap = 0.0f;
    float flexGrow = 0.0f;
    ViewAlignment alignItems = ViewAlignment::Stretch;
    ViewAlignment alignSelf = ViewAlignment::Auto;
    ViewJustification justifyContent = ViewJustification::Start;
    ViewTextAlignment textAlign = ViewTextAlignment::Start;
    ViewImageFit imageFit = ViewImageFit::Contain;
    ViewImageAlignment imageAlignment = ViewImageAlignment::Center;
    ViewImageInterpolation imageInterpolation =
        ViewImageInterpolation::Linear;
    ViewOrientation orientation = ViewOrientation::Horizontal;
    ViewShapeKind shapeKind = ViewShapeKind::Rectangle;
    ViewIconFont iconFont = ViewIconFont::FontAwesome;
    float fontSize = 15.0f;
    float value = 0.0f;
    float thickness = 4.0f;
    float trackOpacity = 1.0f;
    float fillOpacity = 1.0f;
    std::vector<float> values;
    std::optional<float> seriesMinimum;
    std::optional<float> seriesMaximum;
    bool bold = false;
    bool checked = false;
    bool visible = true;
    bool enabled = true;
    std::string cursor;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    ViewStyle style;
    ViewStyle hoverStyle;
    ViewStyle pressedStyle;
    ViewStyle checkedStyle;
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
    static constexpr std::size_t MaximumResources = 64;
    static constexpr std::size_t MaximumSeriesPoints = 512;
    static constexpr std::size_t MaximumTotalSeriesPoints = 4096;
};

bool ValidateAndLayoutViewTree(ViewNode& root, float width, float height,
    std::string& error);
bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error);
const char* ViewNodeTypeName(ViewNodeType type) noexcept;
}
