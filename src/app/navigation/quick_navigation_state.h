#pragma once

#include "quick_navigation_models.h"
#include "app/render/desktop_backdrop_compositor.h"
#include "app/popup/popup_dwell_controller.h"
#include "common/bounded_lru_cache.h"
#include "icons/icon_row_index.h"
#include "navigation/navigation_settings_types.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "navigation/quick_navigation_search_async.h"
#include "settings/settings_search_index.h"
#include "theme/personalization.h"
#include "ui/render/scroll_content_clip.h"
#include "ui/render/ui_animation_scheduler.h"
#include "widget/runtime/widget_host_requests.h"
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::quick_navigation
{
// One navigation session owns its presentation, input, indexing and result
// caches. It shares no DesktopApp layout and contains no application pointer.
struct State
{
    snowdesktop::UiScheduleToken animationFrameToken = 0;
    bool compositionCommitPending = false;
    bool collapsed = false;
    bool fixedTop = false;
    int anchorTop = 0;
    QuickNavigationSearchType searchType = QuickNavigationSearchType::All;
    std::string searchEngine;
    Menu menu = Menu::None;
    int menuSelection = 0;
    int listSelection = -1;
    std::vector<snowdesktop::SettingsSearchResult> settingsResults;
    std::wstring actionNotice;
    bool preview = false;
    ULONGLONG settingsRefreshTick = 0;
    bool lightTheme = false;
    bool glassTheme = false;
    float blurRadius = 24.0f;
    PersonalizationSettings appearance =
        MakeQuickNavigationAppearancePreset(kAppearancePresetLight);
    HWND hwnd = nullptr;
    /** @brief 快捷导航顶层窗口下方的原生毛玻璃层。 */
    DesktopBackdropCompositor backdropCompositor;
    HWND searchEdit = nullptr;
    HFONT searchFont = nullptr;
    // 快捷导航与桌面共享 D2D 渲染设备，但使用独立 DComp device。
    // 否则面板的逐帧变换与桌面/Dock hover 会进入同一 Commit 批次，
    // 在 DWM 合并事务时造成指针反馈的队头阻塞。
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> dcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> dcompEffect;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> dcompScaleTransform;
    Microsoft::WRL::ComPtr<IDCompositionSurface> dcompSurface;
    std::vector<Microsoft::WRL::ComPtr<IDCompositionVisual2>> genieStrips;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> attractionBitmap;
    Microsoft::WRL::ComPtr<IDCompositionSurface> attractionSurface;
    SIZE attractionSurfaceSize{};
    int genieStripEdge = -1;
    UINT compWidth = 0;
    UINT compHeight = 0;
    bool compositionRenderRecoveryPending = false;
    bool compositionPaintInProgress = false;
    // 快捷导航 DirectWrite 文本格式（替代 GDI HFONT）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tabTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> itemTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> searchTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> pathTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fluentTextFormat;
    snowdesktop::ScrollContentFadeCache scrollFadeCache;
    /** @brief 快捷导航应用/Everything 行图标的 D2D 位图缓存（按索引和源尺寸）。 */
    std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID2D1Bitmap>> sysIconCache;
    /** @brief 应用图标快照的 D2D 缓存；键为 AppsFolder 稳定解析身份。 */
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> appIconCache;
    std::array<Microsoft::WRL::ComPtr<ID2D1Bitmap>, 7> actionIconCache;
    ID2D1DeviceContext* actionIconContext = nullptr;
    std::vector<int> tabWidths;
    int viewModeSlotWidth = 0;
    bool open = false;
    LogicalSlotPickerRequest slotPickerRequest{};
    bool topmost = true;
    snowdesktop::quick_navigation_animation_rules::State
        animation;
    snowdesktop::UiScheduleToken
        animationCompletionToken = 0;
    bool animationCompositorDriven = false;
    QuickNavigationInvocationSource invocationSource =
        QuickNavigationInvocationSource::Pointer;
    std::function<void()> postCloseAction;
    RECT renameItemRect{};
    size_t activeWidgetIndex = static_cast<size_t>(-1);
    int scrollOffset = 0;
    int tabScrollOffset = 0;
    bool initialJumpOpen = false;
    size_t initialJumpSelection = 0;
    POINT openPoint{};
    /** @brief 快速导航缩放动画锚点（app 坐标；Dock 搜索入口固定到搜索图标中心）。 */
    POINT animationAnchorPoint{};
    RECT animationDockRect{};
    int animationDockEdge = 0;
    bool deformationFailed = false;
    BYTE lastEditAnimationOpacity = 0;
    bool hasLastEditAnimationFrame = false;
    RECT rect{};
    /** @brief 快速导航合成宿主范围，覆盖目标面板与当前会话动画锚点。 */
    RECT hostRect{};
    bool windowRegionExpanded = false;
    std::wstring searchText;
    std::wstring effectiveSearchText;
    std::wstring searchCompositionText;
    std::vector<EverythingEntry> everythingResults;
    DWORD everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
    bool everythingHasMore = false;
    bool everythingSearchPending = false;
    std::uint64_t everythingSearchGeneration = 0;
    std::wstring everythingResultsQuery;
    std::vector<size_t> appResultIndices;
    std::vector<AppEntry> appEntries;
    bool appsIndexed = false;
    std::atomic<bool> appIndexing{false};
    uint64_t appIndexSerial = 0;
    bool appsExpanded = false;
    KeyboardTargetKind keyboardTargetKind =
        KeyboardTargetKind::None;
    size_t keyboardTargetIndex = 0;
    float dpiScale = 1.0f;
    std::vector<std::wstring> tabOrder;
    size_t tabDragIndex = static_cast<size_t>(-1);
    int tabDragDeltaX = 0;
    POINT tabDragStartPoint{};
    bool tabDragging = false;
    bool scrollbarDragging = false;
    int scrollbarDragStartY = 0;
    int scrollbarDragThumbTop = 0;
    int scrollbarDragStartOffset = 0;
    bool scrollbarHovered = false;
    // Quick Navigation is a separate pointer surface. Sharing lastMousePoint_
    // with the desktop/Dock makes its animated HWND overwrite passive hover
    // state and replay that transition after the window moves away.
    POINT lastMousePoint{ LONG_MIN, LONG_MIN };
    std::vector<HoverRegion>
        hoverRegions;
    PointerTarget
        pointerTarget{};
    PopupHoverController tabHoverController;
    bool tabHoverTimerArmed = false;
    HIMAGELIST systemImageListSmall = nullptr;
    snowdesktop::BoundedLruCache<std::wstring, int> everythingIconCache{512};
    snowdesktop::IconRowIndex<std::wstring> everythingIconRows;
    snowdesktop::QuickNavigationEverythingSearchAsync
        everythingSearch;
    bool everythingSearchAvailable = true;
};
}
