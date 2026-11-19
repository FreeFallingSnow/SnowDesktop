#include "widget_render_host_context.h"

#include "common/utils.h"
#include "widget/runtime/widget_engine.h"

#include <shlobj.h>

AsyncShellIconLoader::~AsyncShellIconLoader()
{
    work_.Stop();
    for (auto& result : completed_) if (result.bitmap) DeleteObject(result.bitmap);
}

void AsyncShellIconLoader::Request(const std::wstring& path, const std::wstring& widgetId)
{
    if (path.empty()) return;
    work_.Submit(path, [path] {
        auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
        {
            result->bitmap = GetHighResolutionShellIconBitmap(pidl, -1, result->size);
            CoTaskMemFree(pidl);
        }
        return result;
    }, [this, path, widgetId](auto result) {
        completed_.push_back({path, result ? std::exchange(result->bitmap, nullptr) : nullptr});
        if (ready_) ready_(widgetId); // UI only; never inspect widgets on the worker.
    }, nullptr, 0);
}

std::vector<AsyncShellIconLoader::Result> AsyncShellIconLoader::Drain()
{
    Pump();
    return std::exchange(completed_, {});
}

static void SetWidgetExecutionContext(D2DState* state, const std::wstring& widgetId)
{
    if (!state) return;
    state->currentWidgetId = widgetId;
    state->storagePrefix = WideToUtf8(widgetId);
    if (state->engine)
        state->engine->ActivateWidgetState(widgetId);
}

WidgetExecutionContextGuard::WidgetExecutionContextGuard(
    D2DState* state, const std::wstring& widgetId)
    : performanceScope_("widget", "context", widgetId), state_(state)
{
    if (!state_)
        return;
    context_ = state_->ctx;
    widgetRect_ = state_->widgetRect;
    storagePrefix_ = state_->storagePrefix;
    widgetId_ = state_->currentWidgetId;
    surfaceKind_ = state_->surfaceKind;
    gridColumns_ = state_->gridColumns;
    gridRows_ = state_->gridRows;
    layoutContentWidth_ = state_->layoutContentWidth;
    layoutContentHeight_ = state_->layoutContentHeight;
    layoutMetrics_ = snowdesktop::widget_runtime::
        CaptureLayoutMetrics(*state_);
    widgetClipDepth_ = state_->widgetClipDepth;
    backgroundLayerActive_ = state_->backgroundLayerActive;
    SetWidgetExecutionContext(state_, widgetId);
}

WidgetExecutionContextGuard::~WidgetExecutionContextGuard()
{
    if (!state_)
        return;
    state_->ctx = context_;
    state_->widgetRect = widgetRect_;
    state_->storagePrefix = std::move(storagePrefix_);
    state_->currentWidgetId = std::move(widgetId_);
    state_->surfaceKind = surfaceKind_;
    state_->gridColumns = gridColumns_;
    state_->gridRows = gridRows_;
    state_->layoutContentWidth = layoutContentWidth_;
    state_->layoutContentHeight = layoutContentHeight_;
    state_->widgetClipDepth = widgetClipDepth_;
    state_->backgroundLayerActive = backgroundLayerActive_;
    snowdesktop::widget_runtime::RestoreLayoutMetrics(
        *state_, layoutMetrics_, [this]() {
            if (state_->engine)
            {
                state_->engine->ActivateWidgetState(
                    state_->currentWidgetId);
            }
        });
}

WidgetSurfaceScope::WidgetSurfaceScope(D2DState* state, const char* surfaceKind,
    bool allowOpeningFocus, std::string_view desktopPopupOpenerKey)
    : state_(state)
{
    if (!state_) return;
    previous_ = state_->surfaceKind;
    previousPopupOpenerKey_ = state_->desktopPopupOpenerKey;
    previousOpeningFocus_ = state_->openingFocusAllowed;
    state_->surfaceKind = surfaceKind ? surfaceKind : "desktop";
    state_->desktopPopupOpenerKey = desktopPopupOpenerKey;
    state_->openingFocusAllowed = allowOpeningFocus;
}

WidgetSurfaceScope::~WidgetSurfaceScope()
{
    if (!state_) return;
    state_->surfaceKind = previous_;
    state_->desktopPopupOpenerKey = previousPopupOpenerKey_;
    state_->openingFocusAllowed = previousOpeningFocus_;
}

void ClearShellIconBitmaps(D2DState* state)
{
    for (const auto& [path, bitmap] : state->shellIconCache)
        state->imageSamplingCache.Remove(bitmap.Get());
    state->shellIconCache.clear();
}

std::string ImageUseOwner(D2DState* state, bool background)
{
    return WideToUtf8(state->currentWidgetId) + "|" +
        state->surfaceKind +
        ((background || state->backgroundLayerActive) ? "#background" : "");
}

void BeginImageFrame(D2DState* state, bool background)
{
    if (state) state->packageImageCache.BeginFrame(ImageUseOwner(state, background));
}

void SetWidgetRectContext(D2DState* state, RECT bounds)
{
    if (!state || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    state->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
    state->layoutContentWidth = static_cast<float>(bounds.right - bounds.left);
    state->layoutContentHeight = static_cast<float>(bounds.bottom - bounds.top);
}

D2DState* GetD2D(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__d2d_ptr");
    auto* s = static_cast<D2DState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}
