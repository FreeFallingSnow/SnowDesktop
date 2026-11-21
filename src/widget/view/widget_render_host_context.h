#pragma once

#include "common/background_work.h"
#include "diagnostics/performance_trace.h"
#include "widget/packages/widget_package_image_cache.h"
#include "widget/runtime/widget_layout_context.h"
#include "widget/runtime/widget_script_read_ahead.h"
#include "widget/view/widget_background_cache.h"
#include "widget/view/widget_layout_lua.h"
#include "widget/view/widget_runtime_image_cache.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class WidgetEngine;
struct lua_State;

class AsyncShellIconLoader
{
public:
    struct Result { std::wstring path; HBITMAP bitmap = nullptr; };
    using ReadyCallback = std::function<void(const std::wstring&)>;
    explicit AsyncShellIconLoader(ReadyCallback readyCallback) : ready_(std::move(readyCallback)) {}
    ~AsyncShellIconLoader();
    void Request(const std::wstring& path, const std::wstring& widgetId);
    void Pump() { work_.Drain(); }
    std::vector<Result> Drain();
private:
    snowdesktop::BackgroundWork work_{2, 128};
    ReadyCallback ready_;
    std::vector<Result> completed_;
};

struct D2DState : snowdesktop::widget_runtime::WidgetLayoutContext
{
    WidgetEngine* engine = nullptr;
    std::string storagePrefix;
    std::string_view desktopPopupOpenerKey;
    bool openingFocusAllowed = false;
    float semanticCuScale = 1.0f;
    snowdesktop::widget_runtime::SemanticUiMetricTokens semanticUiMetrics;
    std::uint64_t nextDesktopPaintRevision = 0;
    Microsoft::WRL::ComPtr<ID2D1Device> bitmapDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> immediateCommandContext;
    snowdesktop::widget_runtime::WidgetBackgroundCache backgroundCache;
    snowdesktop::widget_runtime::WidgetScriptReadAhead scriptReadAhead;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> imageCache;
    std::unordered_map<std::string, std::uint64_t> imageCacheRevisions;
    snowdesktop::widget_runtime::WidgetPackageImageCache packageImageCache;
    snowdesktop::widget_runtime::WidgetRuntimeImageCache runtimeImageCache{imageSamplingCache};
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> shellIconCache;
    std::unordered_set<std::wstring> shellIconFailures;
    std::unique_ptr<AsyncShellIconLoader> shellIconLoader;
};

class WidgetExecutionContextGuard
{
public:
    WidgetExecutionContextGuard(
        D2DState* state, const std::wstring& widgetId);
    ~WidgetExecutionContextGuard();

    WidgetExecutionContextGuard(
        const WidgetExecutionContextGuard&) = delete;
    WidgetExecutionContextGuard& operator=(
        const WidgetExecutionContextGuard&) = delete;

private:
    snowdesktop::performance::Scope performanceScope_;
    D2DState* state_ = nullptr;
    ID2D1DeviceContext* context_ = nullptr;
    D2D1_RECT_F widgetRect_{};
    std::string storagePrefix_;
    std::wstring widgetId_;
    const char* surfaceKind_ = "desktop";
    int gridColumns_ = 1;
    int gridRows_ = 1;
    float layoutContentWidth_ = 0.0f;
    float layoutContentHeight_ = 0.0f;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics_;
    int widgetClipDepth_ = 0;
    bool backgroundLayerActive_ = false;
};

class WidgetSurfaceScope
{
public:
    WidgetSurfaceScope(D2DState* state, const char* surfaceKind,
        bool allowOpeningFocus = false, std::string_view desktopPopupOpenerKey = {});
    ~WidgetSurfaceScope();

    WidgetSurfaceScope(const WidgetSurfaceScope&) = delete;
    WidgetSurfaceScope& operator=(const WidgetSurfaceScope&) = delete;

private:
    D2DState* state_ = nullptr;
    const char* previous_ = "desktop";
    std::string_view previousPopupOpenerKey_;
    bool previousOpeningFocus_ = false;
};


void ClearShellIconBitmaps(D2DState* state);
std::string ImageUseOwner(D2DState* state, bool background = false);
void BeginImageFrame(D2DState* state, bool background = false);
void SetWidgetRectContext(D2DState* state, RECT bounds);
D2DState* GetD2D(lua_State* state);
