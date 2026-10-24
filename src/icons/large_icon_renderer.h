#pragma once
#include "large_icon_config.h"
#include "large_icon_render_rules.h"
#include "large_icon_transform.h"
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <wrl/client.h>

namespace snowdesktop::large_icon_renderer
{
struct TitleResources
{
    struct Key
    {
        double width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
        double scale = 0, contentScale = 0, size = 0;
        int weight = 0, direction = 0;
        bool automatic = false, original = false;
        bool operator==(const Key&) const = default;
    } key;
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    std::uint64_t fontRevision = 0;
    std::wstring name;
    large_icon_render_rules::ContentGeometry closed, open;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    bool valid = false;
};

struct View
{
    RECT frame{};
    float scale = 1, opacity = 1, hover = 0;
    float shine = -1;
    bool original = true, animations = true, selected = false;
    unsigned neutral = 0x414751, accent = 0, edgeColor = 0, componentForeground = 0xffffff;
    bool hasEdgeColor = false, backgroundResolved = false;
    large_icon_render_rules::BackgroundStyle background;
    std::function<void(ID2D1RenderTarget*, RECT, float, float)> drawBackground;
    std::wstring_view name;
    ID2D1Bitmap* bitmap = nullptr;
    TitleResources* titleResources = nullptr;
    std::function<void(ID2D1RenderTarget*, RECT, float)> placeholder;
};

struct CardResources
{
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> recorder;
    TitleResources title;
};

// These are the production desktop draw paths, also usable with a WIC render
// target for deterministic visual checks without creating a desktop window.
void DrawFrame(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view);
// Special shapes show both their occupied grid bounds and visible card contour.
void DrawPlacementPreview(ID2D1RenderTarget* target, const LargeIconConfig& config,
    RECT allocation, float scale, bool valid = true, const RECT* gridAllocation = nullptr);
void DrawResizeHandle(ID2D1RenderTarget* target, POINT center, int diameter, float radius,
    bool light, bool selected);
// Record the complete card independently of the display context's clip stack.
// A false result leaves the caller responsible for the ordinary frame fallback.
bool DrawCard3D(ID2D1DeviceContext* target, IDWriteFactory* fonts, const LargeIconConfig& config,
    const View& view, const large_icon_transform::Card& transform, CardResources& resources);
}
