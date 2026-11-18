#pragma once

#include <d2d1_1.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct IDWriteFactory;
struct LuaWidgetTheme;

namespace snowdesktop
{
struct NativeTooltipTextLayout;
}

namespace snowdesktop::widget_runtime
{
enum class DrawImageFit;
struct PackageImageSize;
struct ViewNode;
struct ViewThemePalette;
class ViewTransitionRuntime;
class WidgetDrawContext;
class WidgetInteractionRegions;

class WidgetViewHost
{
public:
    virtual ViewThemePalette ThemePalette() const = 0;
    virtual std::optional<std::wstring> ResolveFont(std::string_view name) const = 0;
    virtual bool ReadFocusedInput(const std::string& key, std::wstring& text,
        std::size_t& cursor, std::size_t& selectionAnchor, std::wstring& composition,
        std::size_t& compositionCursor) const = 0;
    virtual bool IsSelected() const = 0;
    virtual int InputScrollOffset(const std::string& key) const = 0;
    virtual ID2D1Bitmap1* LoadImage(std::string_view name, float width, float height,
        DrawImageFit fit, bool nearest, PackageImageSize& original) const = 0;
    virtual std::optional<std::wstring> ResolveReference(const std::string& reference) const = 0;
    virtual bool IsPreview() const = 0;
    virtual ID2D1Bitmap1* LoadShellIcon(const std::wstring& path) const = 0;

protected:
    ~WidgetViewHost() = default;
};

class WidgetPanelContentClip
{
public:
    WidgetPanelContentClip(ID2D1DeviceContext* context, const RECT& bounds);
    ~WidgetPanelContentClip();
    WidgetPanelContentClip(const WidgetPanelContentClip&) = delete;
    WidgetPanelContentClip& operator=(const WidgetPanelContentClip&) = delete;
    void Suspend();
    void Resume();

private:
    ID2D1DeviceContext* context_;
    D2D1_RECT_F bounds_;
    bool active_ = false;
};

ViewThemePalette BuildWidgetViewThemePalette(const LuaWidgetTheme& theme,
    bool highContrast, int accentColor);
bool DrawWidgetViewTree(WidgetDrawContext* state, const WidgetViewHost& host,
    const ViewNode& tree, const WidgetInteractionRegions& regions,
    std::string_view focusedKey, ViewTransitionRuntime& transitions,
    bool reducedMotion, WidgetPanelContentClip* contentClip = nullptr);
void DrawWidgetSelectOverlays(WidgetDrawContext* state, const WidgetViewHost& host,
    const ViewNode& node, const WidgetInteractionRegions& regions,
    std::string_view focusedKey, float viewportHeight);
bool MeasureWidgetViewTooltip(IDWriteFactory* dwrite, const D2D1_RECT_F& widgetRect,
    const WidgetInteractionRegions& regions, float scale,
    NativeTooltipTextLayout& measured, D2D1_RECT_F& bounds, std::wstring& name);
void DrawWidgetViewTooltip(WidgetDrawContext* state,
    const WidgetInteractionRegions& regions, float scale);
}
