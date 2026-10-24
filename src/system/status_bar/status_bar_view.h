#pragma once
#include "status_bar.h"
#include "status_bar_layout.h"
#include "system/tray/tray_service.h"
#include "widget/data/widget_system_data_provider.h"
#include "status_bar_battery.h"
#include "status_bar_notification.h"
#include "status_bar_input_method.h"
#include <d2d1_1.h>
#include <optional>
#include <array>
#include <memory>
#include <dwrite.h>
#include <wrl/client.h>

struct IDCompositionSurface;

namespace snowdesktop
{
struct StatusBarItem
{
    std::wstring text;
    StatusBarAction action;
    RECT bounds{};
    std::optional<tray::Icon> icon;
    std::string key;
    std::wstring glyph, tip;
    bool left = false;
    std::array<std::wstring, 3> controlTips;
    std::array<std::wstring, 3> controlGlyphs;
    StatusBarBatteryTone batteryTone = StatusBarBatteryTone::Normal;
    int batteryLevel = -1;
    bool flipGlyph = false;
    bool batteryPluggedIn = false;
    std::optional<status_bar_input_method::ProfileIcon> inputIcon;
    RECT contentBounds{}; // Full scrolled tray cell; bounds is its visible hit target.
    std::wstring batteryPercentageText;
};

// Data boundary only: no HWND, sampling, Explorer or device operations.
struct StatusBarSnapshot
{
    std::wstring clock;
    std::optional<widget_runtime::WidgetCpuDataSnapshot> cpu;
    std::optional<widget_runtime::WidgetMemoryDataSnapshot> memory;
    std::optional<widget_runtime::WidgetGpuDataSnapshot> gpu;
    std::optional<widget_runtime::WidgetNetworkTrafficDataSnapshot> traffic;
    std::optional<widget_runtime::WidgetNetworkStatusDataSnapshot> network;
    std::optional<system_control::Snapshot> wifi;
    status_bar_notification::Snapshot notifications;
    std::optional<widget_runtime::WidgetAudioOutputVolumeDataSnapshot> audio;
    std::optional<widget_runtime::WidgetAudioOutputDefaultDataSnapshot> audioDevice;
    std::optional<widget_runtime::WidgetPowerDataSnapshot> power;
    std::vector<tray::Icon> tray;
    bool trayExpanded = false;
    status_bar_input_method::Snapshot inputMethod;
};
struct StatusBarPalette
{
    bool highContrast = false;
    D2D1_COLOR_F background{}, foreground{}, highlight{}, highlightText{};
};
std::vector<StatusBarItem> BuildStatusBarItems(const StatusBarSettings&, const StatusBarSnapshot&);
bool SameStatusBarContent(const std::vector<StatusBarItem>&, const std::vector<StatusBarItem>&);
inline std::size_t StatusBarControlPart(float localDip, bool hasBattery = true)
{ return localDip < 28 ? 0 : (localDip < 56 || !hasBattery) ? 1 : 2; }
inline std::optional<std::size_t> HitTestStatusBarItems(const std::vector<StatusBarItem>& items, POINT point)
{
    for (std::size_t index = 0; index < items.size(); ++index)
        if (PtInRect(&items[index].bounds, point)) return index;
    return {};
}
// Owned by one status-bar window, on its drawing thread. A null resources
// argument below retains the complete uncached reference/fallback path.
struct StatusBarDrawResources
{
    struct Bitmap
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> image;
        RECT ink{};
    };
    struct Entry
    {
        std::string key;
        std::optional<tray::Identity> identity;
        DWORD width = 0, height = 0;
        std::vector<std::uint32_t> pixels;
        Bitmap bitmap;
    };
    static constexpr std::size_t MaximumBitmaps = 128;
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format, iconFormat, batteryFormat;
    Microsoft::WRL::ComPtr<IDWriteFontFace> iconFace;
    std::shared_ptr<const void> fontSelection;
    std::uint64_t fontRevision = 0;
    float scale = 0, dpiX = 0, dpiY = 0;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    std::vector<Entry> bitmaps;
    // Cumulative resource operations, also observable by offscreen regression.
    std::uint64_t fontBuilds = 0, faceLookups = 0, bitmapBuilds = 0, inkScans = 0;
    ~StatusBarDrawResources() { Reset(); }
    bool TextMatches(IDWriteFactory*, float) const;
    void RetainIcons(const std::vector<StatusBarItem>&);
    // Prepare one frame's font snapshot without opening a surface draw.
    HRESULT PrepareText(IDWriteFactory*, float);
    // Prepare the complete item set before reading images, including on a
    // partial draw; resource ownership follows items rather than dirty bounds.
    HRESULT Prepare(ID2D1DeviceContext*, IDWriteFactory*, float, const std::vector<StatusBarItem>&, bool keepPreparedText = false);
    Bitmap BitmapFor(ID2D1DeviceContext*, const StatusBarItem&);
    void Reset();
};

// Arrange without starting a surface draw, so geometry and drop previews are
// final before any window begins drawing. format is the prepared body format.
// frameReusable is false after recoverable measurement/drawing fallbacks;
// their existing pixels and success result remain unchanged.
HRESULT ArrangeStatusBarContent(IDWriteFactory*, IDWriteTextFormat*, std::vector<StatusBarItem>&,
    UINT width, UINT height, float scale, bool mergedDock, bool timeNotificationRightAligned,
    StatusBarTrayScroll&, bool dockQuickSearch, bool* frameReusable = nullptr);
// A successful retained-layout check updates only tray cells. Rejection leaves
// every item untouched and requires complete arrangement.
bool UpdateStatusBarTrayScrollLayout(std::vector<StatusBarItem>&, LONG previousOffset, const StatusBarTrayScroll&, bool frameReusable);
RECT StatusBarTrayScrollDamage(const std::vector<StatusBarItem>&, std::optional<std::size_t> previousHover,
    RECT viewport, const std::optional<RECT>& before, const std::optional<RECT>& after, UINT width, UINT height);
void DrawStatusBarDropIndicator(ID2D1DeviceContext*, const std::optional<RECT>&,
    const PersonalizationSettings&, const StatusBarPalette&, UINT dpi);
HRESULT DrawStatusBarContentUpdate(IDCompositionSurface*, IDWriteFactory*, std::vector<StatusBarItem>&,
    RECT damage, UINT width, UINT height, float scale, const PersonalizationSettings&, const StatusBarPalette&,
    std::optional<std::size_t> hovered, bool keyboardFocusVisible, std::size_t focused, bool mergedDock,
    StatusBarTrayScroll&, const std::optional<RECT>& indicator, UINT dpi, StatusBarDrawResources* resources = nullptr,
    bool* frameReusable = nullptr);

// updateBounds limits drawing to intersecting items and retains their existing
// geometry. Only use it with a tray viewport from a successful full draw.
HRESULT DrawStatusBarContent(ID2D1DeviceContext*, IDWriteFactory*, std::vector<StatusBarItem>&,
    UINT width, UINT height, float scale, const PersonalizationSettings&, const StatusBarPalette&,
    std::optional<std::size_t> hovered = {}, bool keyboardFocusVisible = false, std::size_t focused = 0,
    bool mergedDock = false, bool timeNotificationRightAligned = false, StatusBarTrayScroll* trayScroll = nullptr,
    bool dockQuickSearch = true, const RECT* updateBounds = nullptr, StatusBarDrawResources* resources = nullptr,
    bool keepPreparedText = false, bool* frameReusable = nullptr);
// A hover-only update reuses the last successful full layout and tray viewport.
// Empty/noninteractive targets (including Show Desktop) have no painted hover.
std::array<RECT, 2> StatusBarHoverDamage(const std::vector<StatusBarItem>&,
    std::optional<std::size_t> before, std::optional<std::size_t> after, UINT width, UINT height);
// The surface must already have a complete frame. Updates are closed with
// EndDraw but committed by the caller, together with its other visual changes.
HRESULT DrawStatusBarHoverUpdate(IDCompositionSurface*, IDWriteFactory*, std::vector<StatusBarItem>&,
    const std::array<RECT, 2>& damage, UINT width, UINT height, float scale,
    const PersonalizationSettings&, const StatusBarPalette&, std::optional<std::size_t> hovered,
    bool keyboardFocusVisible, std::size_t focused, bool mergedDock, StatusBarTrayScroll&,
    StatusBarDrawResources* resources = nullptr, bool* frameReusable = nullptr);
PersonalizationSettings StatusBarFillAppearance(const PersonalizationSettings&);
// The desktop-facing halo occupies space within the existing AppBar allocation.
void DrawStatusBarEdge(ID2D1DeviceContext*, RECT, const PersonalizationSettings&, float scale, DockPosition, RECT adjacentDock = {});
}
