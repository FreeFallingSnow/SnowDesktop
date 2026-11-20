#include "widget_surface_host.h"

#include "common/constants.h"
#include "common/l10n.h"
#include "common/utils.h"
#include "layout/logical_slot_keyboard_rules.h"
#include "settings/animation_settings.h"
#include "ui/render/app_font.h"
#include "ui/render/native_tooltip_content.h"
#include "widget/api/widget_api_registry.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_host_control_projection.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_draw_geometry.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_renderer.h"
#include "widget/view/widget_view_lua.h"
#include "widget/view/widget_view_text.h"
#include "widgets/widget_chrome_rules.h"

#include <d2d1effects.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

using snowdesktop::lua_runtime::DeferLuaError;
using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::CurrentLayoutContentHeight;
using snowdesktop::widget_runtime::CurrentLayoutContentWidth;
using snowdesktop::widget_runtime::PreviewExecutionScope;

WidgetSystemEnvironment QueryWidgetSystemEnvironment()
{
    if (snowdesktop::widget_runtime::IsPreviewExecution())
    {
        WidgetSystemEnvironment preview;
        preview.locale = Locale::Instance().GetEffectiveLanguage();
        preview.inputLanguage = preview.locale;
        preview.timeZone = "UTC";
        preview.utcOffsetMinutes = 0;
        const std::wstring localeName =
            Utf8ToWide(preview.locale);
        wchar_t region[LOCALE_NAME_MAX_LENGTH]{};
        if (!localeName.empty() && GetLocaleInfoEx(localeName.c_str(),
                LOCALE_SISO3166CTRYNAME, region,
                LOCALE_NAME_MAX_LENGTH) > 0)
            preview.region = WideToUtf8(region);
        return preview;
    }
    static std::mutex cacheMutex;
    static WidgetSystemEnvironment cached;
    static ULONGLONG lastRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    std::scoped_lock lock(cacheMutex);
    if (lastRefresh != 0 && now - lastRefresh < 1000)
        return cached;

    WidgetSystemEnvironment result;
    HIGHCONTRASTW highContrast{ sizeof(highContrast) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,
            sizeof(highContrast), &highContrast, 0))
        result.highContrast =
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    BOOL animationsEnabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
            &animationsEnabled, 0))
        result.reducedMotion = animationsEnabled == FALSE;

    DWORD textScalePercent = 100;
    DWORD textScaleBytes = sizeof(textScalePercent);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Accessibility", L"TextScaleFactor",
            RRF_RT_REG_DWORD, nullptr, &textScalePercent,
            &textScaleBytes) == ERROR_SUCCESS)
    {
        result.textScale = std::clamp(
            static_cast<double>(textScalePercent) / 100.0, 0.5, 5.0);
    }

    DWORD colorization = 0;
    BOOL colorizationOpaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(
            &colorization, &colorizationOpaque)))
        result.accentColor = static_cast<int>(colorization & 0x00ffffffu);
    else
    {
        const COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
        result.accentColor = (GetRValue(accent) << 16) |
            (GetGValue(accent) << 8) | GetBValue(accent);
    }

    result.locale = Locale::Instance().GetEffectiveLanguage();
    std::wstring localeName = Utf8ToWide(result.locale);
    wchar_t localeBuffer[LOCALE_NAME_MAX_LENGTH]{};
    if (localeName.empty() || GetLocaleInfoEx(localeName.c_str(),
            LOCALE_SISO3166CTRYNAME, localeBuffer,
            LOCALE_NAME_MAX_LENGTH) <= 0)
    {
        if (GetUserDefaultLocaleName(localeBuffer,
                LOCALE_NAME_MAX_LENGTH) > 0)
            localeName = localeBuffer;
        localeBuffer[0] = L'\0';
        GetLocaleInfoEx(localeName.c_str(), LOCALE_SISO3166CTRYNAME,
            localeBuffer, LOCALE_NAME_MAX_LENGTH);
    }
    result.region = WideToUtf8(localeBuffer);

    DYNAMIC_TIME_ZONE_INFORMATION timeZone{};
    const DWORD timeZoneId = GetDynamicTimeZoneInformation(&timeZone);
    const wchar_t* timeZoneName = timeZone.TimeZoneKeyName[0]
        ? timeZone.TimeZoneKeyName : timeZone.StandardName;
    result.timeZone = WideToUtf8(timeZoneName);
    LONG bias = timeZone.Bias;
    if (timeZoneId == TIME_ZONE_ID_STANDARD)
        bias += timeZone.StandardBias;
    else if (timeZoneId == TIME_ZONE_ID_DAYLIGHT)
        bias += timeZone.DaylightBias;
    result.utcOffsetMinutes = -static_cast<int>(bias);

    const LANGID inputLanguage = LOWORD(
        reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
    wchar_t inputLocale[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(inputLanguage, SORT_DEFAULT),
            inputLocale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
        result.inputLanguage = WideToUtf8(inputLocale);

    cached = result;
    lastRefresh = now;
    return result;
}

void AttachVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    const LuaWidget& widget, std::string_view surface)
{
    node.virtualMeasurements.clear();
    if (node.estimatedItemSize)
    {
        const auto& states = VariableVirtualStatesForSurface(widget, surface);
        const auto found = states.find(node.key);
        const bool horizontal = node.orientation ==
            snowdesktop::widget_runtime::ViewOrientation::Horizontal;
        const float mainGap = horizontal
            ? node.columnGap.value_or(node.gap)
            : node.rowGap.value_or(node.gap);
        if (found != states.end() && VariableVirtualStateMatches(
                found->second, node.itemCount, *node.estimatedItemSize,
                mainGap, horizontal,
                node.virtualLayoutRevision))
            node.virtualMeasurements =
                VariableVirtualMeasurements(found->second);
    }
    for (auto& child : node.children)
        AttachVariableVirtualMeasurements(child, widget, surface);
}

bool VariableVirtualStateMatches(
    const LuaWidget::VariableVirtualState& state,
    std::size_t itemCount, float estimatedItemSize, float mainGap,
    bool horizontal, std::uint64_t layoutRevision) noexcept
{
    return state.itemCount == itemCount &&
        state.estimatedItemSize == estimatedItemSize &&
        state.mainGap == mainGap &&
        state.horizontal == horizontal &&
        state.layoutRevision == layoutRevision;
}

bool IsPanelSurface(std::string_view surface) noexcept
{
    return surface == "panel" || surface == "dialog" ||
        surface == "popover";
}

std::string NormalizeWidgetSurface(std::string_view surface)
{
    if (surface == "dialog") return "dialog";
    if (surface == "popover") return "popover";
    if (surface == "panel") return "panel";
    return "desktop";
}

snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelInteractionRegions : widget.interactionRegions;
}

const snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelInteractionRegions : widget.interactionRegions;
}

bool ValidateAndLayoutWidgetView(
    snowdesktop::widget_runtime::ViewNode& root,
    LuaWidget& widget, std::string_view surface,
    float width, float height, std::string& error)
{
    snowdesktop::performance::Scope scope("widget.view", "layout", widget.widgetId);
    AttachVariableVirtualMeasurements(root, widget, surface);
    auto& cache = IsPanelSurface(surface)
        ? widget.panelViewLayoutCache : widget.viewLayoutCache;
    bool reused = false;
    const bool accepted = cache.ValidateAndLayout(
        root, width, height, error, &reused);
    snowdesktop::performance::Value("widget.view.layout_cache",
        reused ? "hit" : "miss", widget.widgetId, 1);
    return accepted;
}

std::string FormatViewValidationDiagnostic(
    std::string_view code, std::string_view message)
{
    if (!snowdesktop::widget_runtime::
            IsKnownViewValidationDiagnosticCode(code))
        code = "view.layout";
    return "[" + std::string(code) + "] " + std::string(message);
}

bool CommitVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    LuaWidget& widget, std::string_view surface)
{
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualItemStart;
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualRange;
    using snowdesktop::widget_runtime::ViewNodeType;
    using snowdesktop::widget_runtime::ViewVirtualRange;
    constexpr std::size_t maximumCachedMeasurements = 4096;
    bool changed = false;
    auto& states = VariableVirtualStatesForSurface(widget, surface);
    if (node.type == ViewNodeType::VirtualList && !node.estimatedItemSize)
        states.erase(node.key);
    if (node.estimatedItemSize &&
        node.collectionContent == snowdesktop::widget_runtime::
            ViewCollectionContent::Items &&
        node.virtualMeasuredExtents.size() == node.children.size() &&
        node.virtualMeasuredIndices.size() == node.children.size())
    {
        const bool horizontal = node.orientation ==
            snowdesktop::widget_runtime::ViewOrientation::Horizontal;
        const float mainGap = horizontal
            ? node.columnGap.value_or(node.gap)
            : node.rowGap.value_or(node.gap);
        const auto oldMeasurements = node.virtualMeasurements;
        ViewVirtualRange oldRange;
        std::string rangeError;
        std::size_t anchorIndex = node.firstIndex;
        float oldAnchorStart = 0.0f;
        if (node.itemCount > 0 &&
            ComputeViewVariableVirtualRange(node.itemCount,
                *node.estimatedItemSize, mainGap,
                node.scrollViewportExtent, node.scrollOffset, 0,
                oldMeasurements, oldRange, rangeError))
        {
            anchorIndex = oldRange.firstIndex;
            (void)ComputeViewVariableVirtualItemStart(node.itemCount,
                *node.estimatedItemSize, mainGap, anchorIndex,
                oldMeasurements, oldAnchorStart, rangeError);
        }

        LuaWidget::VariableVirtualState nextState;
        const auto existing = states.find(node.key);
        if (existing != states.end() && VariableVirtualStateMatches(
                existing->second, node.itemCount,
                *node.estimatedItemSize, mainGap, horizontal,
                node.virtualLayoutRevision))
            nextState = existing->second;
        else
        {
            nextState.itemCount = node.itemCount;
            nextState.estimatedItemSize = *node.estimatedItemSize;
            nextState.mainGap = mainGap;
            nextState.horizontal = horizontal;
            nextState.layoutRevision = node.virtualLayoutRevision;
            changed = true;
        }
        for (std::size_t child = 0;
            child < node.virtualMeasuredExtents.size(); ++child)
        {
            const std::size_t index = node.virtualMeasuredIndices[child];
            const float extent = node.virtualMeasuredExtents[child];
            const std::string& itemKey = node.children[child].key;
            auto& measurement = nextState.measurements[index];
            if (std::fabs(measurement.extent - extent) > 0.01f ||
                measurement.itemKey != itemKey)
                changed = true;
            measurement.extent = extent;
            measurement.itemKey = itemKey;
            measurement.lastUsed = ++nextState.sequence;
        }
        while (nextState.measurements.size() > maximumCachedMeasurements)
        {
            auto oldest = nextState.measurements.end();
            for (auto current = nextState.measurements.begin();
                current != nextState.measurements.end(); ++current)
            {
                if (std::ranges::find(node.virtualMeasuredIndices,
                        current->first) != node.virtualMeasuredIndices.end())
                    continue;
                if (oldest == nextState.measurements.end() ||
                    current->second.lastUsed < oldest->second.lastUsed)
                    oldest = current;
            }
            if (oldest == nextState.measurements.end()) break;
            nextState.measurements.erase(oldest);
            changed = true;
        }

        if (changed && node.itemCount > 0)
        {
            const auto newMeasurements =
                VariableVirtualMeasurements(nextState);
            float newAnchorStart = oldAnchorStart;
            if (ComputeViewVariableVirtualItemStart(node.itemCount,
                    *node.estimatedItemSize, mainGap, anchorIndex,
                    newMeasurements, newAnchorStart, rangeError))
            {
                ViewVirtualRange newRange;
                if (ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize, mainGap,
                        node.scrollViewportExtent, node.scrollOffset, 0,
                        newMeasurements, newRange, rangeError))
                {
                    auto& offsets = ScrollOffsetsForSurface(widget, surface);
                    const auto offset = offsets.find(node.key);
                    if (offset != offsets.end())
                    {
                        const std::int64_t anchored =
                            static_cast<std::int64_t>(offset->second) +
                            static_cast<std::int64_t>(std::lround(
                                newAnchorStart - oldAnchorStart));
                        offset->second = static_cast<int>(
                            std::clamp<std::int64_t>(anchored, 0,
                                static_cast<std::int64_t>(std::lround(
                                    newRange.maximum))));
                    }
                    states[node.key] = std::move(nextState);
                }
            }
        }
        else if (node.itemCount == 0 || !changed)
            states[node.key] = std::move(nextState);
    }
    for (auto& child : node.children)
        changed = CommitVariableVirtualMeasurements(
            child, widget, surface) || changed;
    return changed;
}

std::string_view VisualViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return ViewFocusCueForSurface(widget, surface)
        ? std::string_view(ViewFocusForSurface(widget, surface))
        : std::string_view{};
}

bool HostControlBelongsToSurface(
    const LuaWidget::HostControl& control, std::string_view surface) noexcept
{
    return control.surface == surface;
}

void EnsureBitmapCachesForCurrentDevice(D2DState* state)
{
    if (!state || !state->ctx) return;
    ComPtr<ID2D1Device> device;
    state->ctx->GetDevice(&device);
    if (state->bitmapDevice.Get() == device.Get()) return;
    state->bitmapDevice = std::move(device);
    state->backgroundCache.Clear();
    state->textLayoutCache.Clear();
    state->textGlowCache.Clear();
    state->imageSamplingCache.Clear();
    state->immediateCommandContext.Reset();
    state->imageCache.clear();
    state->imageCacheRevisions.clear();
    state->runtimeImageCache.ClearBitmaps();
    ClearShellIconBitmaps(state);
    state->shellIconFailures.clear();
}

ID2D1Bitmap1* LoadImageBitmap(
    D2DState* s, const std::string& contentKey,
    float width, float height,
    snowdesktop::widget_runtime::DrawImageFit fit,
    bool originalPixels,
    snowdesktop::widget_runtime::PackageImageSize* originalSize)
{
    if (!s || !s->ctx || contentKey.empty()) return nullptr;
    EnsureBitmapCachesForCurrentDevice(s);
    auto original = s->packageImageCache.OriginalSize(contentKey);
    const bool synchronous = s->engine && s->engine->IsPreviewOnly();
    if (!original.width && width > 0 && height > 0)
    {
        s->packageImageCache.RequestMetadata(contentKey, ImageUseOwner(s), synchronous);
        original = s->packageImageCache.OriginalSize(contentKey);
    }
    if (originalSize) *originalSize = original;
    if (!original.width || !original.height) return nullptr;
    const auto* source = s->packageImageCache.Find(contentKey);
    if (width > 0 && height > 0)
    {
        D2D1_MATRIX_3X2_F transform{};
        s->ctx->GetTransform(&transform);
        float dpiX = 96.0f, dpiY = 96.0f;
        s->ctx->GetDpi(&dpiX, &dpiY);
        const double scale = snowdesktop::widget_runtime::RequiredImageRasterScale(
            static_cast<float>(original.width), static_cast<float>(original.height),
            width, height, fit, {transform._11, transform._12, transform._21,
                transform._22, dpiX, dpiY,
                s->ctx->GetUnitMode() == D2D1_UNIT_MODE_PIXELS}, originalPixels);
        source = s->packageImageCache.Request(contentKey, ImageUseOwner(s),
            scale, snowdesktop::widget_runtime::WidgetPackageImageCache::Clock::now(),
            synchronous);
    }
    auto it = s->imageCache.find(contentKey);
    const auto revision = s->packageImageCache.Revision(contentKey);
    if (it != s->imageCache.end() &&
        (s->imageCacheRevisions[contentKey] == revision || !source))
        return it->second.Get();
    if ((!source || source->pixels.empty()) && width <= 0 &&
        s->packageImageCache.Ready(contentKey))
    {
        const std::string owner = "load:" + ImageUseOwner(s);
        source = s->packageImageCache.Request(contentKey, owner, 1.0,
            snowdesktop::widget_runtime::WidgetPackageImageCache::Clock::now(), true);
        s->packageImageCache.ForgetOwner(owner);
    }
    if (!source || source->pixels.empty()) return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
        D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(s->ctx->CreateBitmap(
            D2D1::SizeU(source->width, source->height),
            source->pixels.data(), source->stride,
            &properties, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    if (s->imageCache.size() >= 128)
    {
        // Retain only a bounded small BGRA8 working set past the original
        // 128-entry boundary. This estimates source pixels, not GPU residency.
        constexpr std::size_t smallImageEntryLimit = 256;
        constexpr std::uint64_t smallImagePixelByteLimit =
            16ULL * 1024ULL * 1024ULL;
        std::uint64_t estimatedPixelBytes = 0;
        const auto addBgra8PixelBytes =
            [&](ID2D1Bitmap1* cachedBitmap) -> bool {
                if (!cachedBitmap || cachedBitmap->GetPixelFormat().format !=
                        DXGI_FORMAT_B8G8R8A8_UNORM)
                    return false;
                const D2D1_SIZE_U pixels = cachedBitmap->GetPixelSize();
                const std::uint64_t width = pixels.width;
                const std::uint64_t height = pixels.height;
                if (width == 0 || height == 0)
                    return false;
                // Divide before multiplying: a successful check proves
                // width*height*4 <= 16MiB, so the uint64_t product is safe.
                if (height > (smallImagePixelByteLimit / 4ULL) / width)
                    return false;
                const std::uint64_t pixelBytes = width * height * 4ULL;
                // Running total never exceeds the limit; subtraction and
                // addition remain safe even for adversarial bitmap sizes.
                if (pixelBytes > smallImagePixelByteLimit -
                        estimatedPixelBytes)
                    return false;
                estimatedPixelBytes += pixelBytes;
                return true;
            };
        bool retainSmallWorkingSet =
            s->imageCache.size() < smallImageEntryLimit &&
            addBgra8PixelBytes(bitmap.Get());
        if (retainSmallWorkingSet)
        {
            for (const auto& cached : s->imageCache)
            {
                if (!addBgra8PixelBytes(cached.second.Get()))
                {
                    retainSmallWorkingSet = false;
                    break;
                }
            }
        }
        if (!retainSmallWorkingSet)
        {
            s->imageSamplingCache.Clear();
            s->imageCache.clear();
            s->imageCacheRevisions.clear();
        }
    }
    if (const auto old = s->imageCache.find(contentKey); old != s->imageCache.end())
        s->imageSamplingCache.Remove(old->second.Get());
    s->imageCache[contentKey] = bitmap;
    s->imageCacheRevisions[contentKey] = s->packageImageCache.Revision(contentKey);
    return result;
}

ID2D1Bitmap1* LoadRuntimeImageBitmap(D2DState* state,
    const std::wstring& widgetId, std::string_view token)
{
    if (!state || !state->ctx || widgetId.empty() ||
        !snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(token))
        return nullptr;
    EnsureBitmapCachesForCurrentDevice(state);
    return state->runtimeImageCache.LoadBitmap(state->ctx, widgetId, token);
}

void DrainShellIconResults(D2DState* state)
{
    if (!state || !state->ctx || !state->shellIconLoader)
        return;
    EnsureBitmapCachesForCurrentDevice(state);
    for (auto& result : state->shellIconLoader->Drain())
    {
        if (!result.bitmap)
        {
            state->shellIconFailures.insert(result.path);
            continue;
        }
        ComPtr<ID2D1Bitmap1> bitmap =
            BitmapFromHBitmap(state->ctx, result.bitmap);
        DeleteObject(result.bitmap);
        result.bitmap = nullptr;
        if (!bitmap)
        {
            state->shellIconFailures.insert(result.path);
            continue;
        }
        if (state->shellIconCache.size() >= 512)
            ClearShellIconBitmaps(state);
        if (const auto old = state->shellIconCache.find(result.path);
            old != state->shellIconCache.end())
            state->imageSamplingCache.Remove(old->second.Get());
        state->shellIconCache[result.path] = std::move(bitmap);
    }
}

ComPtr<ID2D1Bitmap1> BitmapFromHBitmap(ID2D1DeviceContext* ctx, HBITMAP hbm)
{
    ComPtr<ID2D1Bitmap1> result;
    if (!ctx || !hbm) return result;
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || !bm.bmBits) return result;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(ctx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(bm.bmWidth), static_cast<UINT32>(bm.bmHeight)),
        bm.bmBits, static_cast<UINT32>(bm.bmWidthBytes), &props, &result)))
        result.Reset();
    return result;
}

std::unordered_map<std::string, LuaWidget::VariableVirtualState>&
VariableVirtualStatesForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelVariableVirtualStates : widget.variableVirtualStates;
}

const std::unordered_map<std::string,
    LuaWidget::VariableVirtualState>& VariableVirtualStatesForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelVariableVirtualStates : widget.variableVirtualStates;
}

std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
VariableVirtualMeasurements(
    const LuaWidget::VariableVirtualState& state)
{
    std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
        result;
    result.reserve(state.measurements.size());
    for (const auto& [index, measurement] : state.measurements)
        result.push_back({ index, measurement.extent });
    std::ranges::sort(result, {},
        &snowdesktop::widget_runtime::ViewVirtualItemMeasurement::index);
    return result;
}

std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelScrollOffsets : widget.scrollOffsets;
}

const std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelScrollOffsets : widget.scrollOffsets;
}

bool& ViewFocusCueForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewFocusCueVisible : widget.viewFocusCueVisible;
}

bool ViewFocusCueForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewFocusCueVisible : widget.viewFocusCueVisible;
}

std::string& ViewFocusForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewKeyboardFocusKey : widget.viewKeyboardFocusKey;
}

const std::string& ViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewKeyboardFocusKey : widget.viewKeyboardFocusKey;
}

static void PushContextRect(lua_State* L, const RECT& rect,
    double scaleX, double scaleY)
{
    const double divisorX = scaleX > 0.0 ? scaleX : 1.0;
    const double divisorY = scaleY > 0.0 ? scaleY : 1.0;
    lua_createtable(L, 0, 6);
    lua_pushnumber(L, rect.left / divisorX); lua_setfield(L, -2, "left");
    lua_pushnumber(L, rect.top / divisorY); lua_setfield(L, -2, "top");
    lua_pushnumber(L, rect.right / divisorX); lua_setfield(L, -2, "right");
    lua_pushnumber(L, rect.bottom / divisorY); lua_setfield(L, -2, "bottom");
    lua_pushnumber(L, (rect.right - rect.left) / divisorX);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, (rect.bottom - rect.top) / divisorY);
    lua_setfield(L, -2, "height");
}


static int LuaWidgetContextWithDeferredError(lua_State* caller)
{
    auto* contextD2D = GetD2D(caller);
    lua_getfield(caller, LUA_REGISTRYINDEX, "__widget_preview");
    const bool preview = lua_toboolean(caller, -1) != 0;
    lua_pop(caller, 1);
    const std::wstring widgetId = BoundWidgetId(caller);
    LuaWidgetContextState contextState = contextD2D && contextD2D->engine
        ? contextD2D->engine->RuntimeGetWidgetContextState(widgetId)
        : LuaWidgetContextState{};
    if (preview) contextState.preview = true;
    const WidgetSystemEnvironment contextSystem = QueryWidgetSystemEnvironment();
    const double contextScaleX = static_cast<double>(contextState.surface.dpiX) /
        USER_DEFAULT_SCREEN_DPI;
    const double contextScaleY = static_cast<double>(contextState.surface.dpiY) /
        USER_DEFAULT_SCREEN_DPI;
    const double contextPixelWidth = contextD2D
        ? contextD2D->widgetRect.right - contextD2D->widgetRect.left : 0.0;
    const double contextPixelHeight = contextD2D
        ? contextD2D->widgetRect.bottom - contextD2D->widgetRect.top : 0.0;
    const int contextColumns = contextD2D ? std::max(1, contextD2D->gridColumns) : 1;
    const int contextRows = contextD2D ? std::max(1, contextD2D->gridRows) : 1;
    const int contextArea = contextColumns * contextRows;
    const LuaWidgetTheme contextTheme = contextD2D && contextD2D->engine
        ? contextD2D->engine->RuntimeGetWidgetTheme(widgetId)
        : LuaWidgetTheme{};

    struct ContextArguments
    {
        D2DState* d2d;
        const LuaWidgetContextState* state;
        const WidgetSystemEnvironment* system;
        const LuaWidgetTheme* theme;
        double scaleX, scaleY, pixelWidth, pixelHeight;
        int columns, rows, area;
    } arguments{contextD2D, &contextState, &contextSystem, &contextTheme,
        contextScaleX, contextScaleY, contextPixelWidth, contextPixelHeight,
        contextColumns, contextRows, contextArea};
    std::string error;
    // Own all native strings above the protected call. Lua allocation failure
    // can jump only across this push-only callback, never their destructors.
    const bool pushed = snowdesktop::lua_runtime::ProtectedHostCall(caller,
        +[](lua_State* inner) {
            const auto& input = *static_cast<ContextArguments*>(lua_touserdata(inner, 1));
            lua_State* L = inner;
            auto* d2d = input.d2d;
            const auto& state = *input.state;
            const auto& system = *input.system;
            const auto& theme = *input.theme;
            const double scaleX = input.scaleX, scaleY = input.scaleY;
            const double pixelWidth = input.pixelWidth, pixelHeight = input.pixelHeight;
            const int columns = input.columns, rows = input.rows, area = input.area;

            lua_createtable(L, 0, 19);
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, pixelWidth / scaleX); lua_setfield(L, -2, "width");
            lua_pushnumber(L, pixelHeight / scaleY); lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "logicalSize");
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, pixelWidth); lua_setfield(L, -2, "width");
            lua_pushnumber(L, pixelHeight); lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "pixelSize");
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, CurrentLayoutContentWidth(d2d));
            lua_setfield(L, -2, "width");
            lua_pushnumber(L, CurrentLayoutContentHeight(d2d));
            lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "layoutSize");
            lua_createtable(L, 0, 4);
            lua_pushinteger(L, state.surface.dpiX); lua_setfield(L, -2, "x");
            lua_pushinteger(L, state.surface.dpiY); lua_setfield(L, -2, "y");
            lua_pushnumber(L, scaleX); lua_setfield(L, -2, "scaleX");
            lua_pushnumber(L, scaleY); lua_setfield(L, -2, "scaleY");
            lua_setfield(L, -2, "dpi");
            lua_pushstring(L, area <= 2 ? "small" :
                (area <= 6 ? "medium" : "large"));
            lua_setfield(L, -2, "sizeClass");
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, columns); lua_setfield(L, -2, "columns");
            lua_pushinteger(L, rows); lua_setfield(L, -2, "rows");
            lua_setfield(L, -2, "grid");

            lua_createtable(L, 0, 6);
            lua_pushboolean(L, state.surface.monitorAvailable);
            lua_setfield(L, -2, "available");
            lua_pushboolean(L, state.surface.primaryMonitor);
            lua_setfield(L, -2, "primary");
            PushContextRect(L, state.surface.monitorBounds, 1.0, 1.0);
            lua_setfield(L, -2, "pixelBounds");
            PushContextRect(L, state.surface.workArea, 1.0, 1.0);
            lua_setfield(L, -2, "pixelWorkArea");
            PushContextRect(L, state.surface.monitorBounds, scaleX, scaleY);
            lua_setfield(L, -2, "logicalBounds");
            PushContextRect(L, state.surface.workArea, scaleX, scaleY);
            lua_setfield(L, -2, "logicalWorkArea");
            lua_setfield(L, -2, "monitor");

            lua_createtable(L, 0, 6);
            lua_pushstring(L, theme.contentTheme == 1 ? "light" : "dark");
            lua_setfield(L, -2, "mode");
            lua_pushinteger(L, theme.bg); lua_setfield(L, -2, "background");
            lua_pushinteger(L, theme.border); lua_setfield(L, -2, "border");
            lua_pushliteral(L, "systemAccent"); lua_setfield(L, -2, "accentToken");
            lua_pushinteger(L, system.accentColor); lua_setfield(L, -2, "accentColor");
            lua_pushboolean(L, system.highContrast); lua_setfield(L, -2, "highContrast");
            lua_setfield(L, -2, "theme");

            lua_createtable(L, 0, 3);
            lua_pushboolean(L, system.highContrast); lua_setfield(L, -2, "highContrast");
            lua_pushboolean(L, system.reducedMotion); lua_setfield(L, -2, "reducedMotion");
            lua_pushnumber(L, system.textScale); lua_setfield(L, -2, "textScale");
            lua_setfield(L, -2, "accessibility");
            lua_pushlstring(L, system.locale.data(), system.locale.size());
            lua_setfield(L, -2, "locale");
            lua_pushlstring(L, system.region.data(), system.region.size());
            lua_setfield(L, -2, "region");
            lua_pushlstring(L, system.timeZone.data(), system.timeZone.size());
            lua_setfield(L, -2, "timeZone");
            lua_pushinteger(L, system.utcOffsetMinutes);
            lua_setfield(L, -2, "utcOffsetMinutes");
            lua_pushlstring(L, system.inputLanguage.data(),
                system.inputLanguage.size());
            lua_setfield(L, -2, "inputLanguage");
            lua_pushboolean(L, state.visible); lua_setfield(L, -2, "visible");
            lua_pushboolean(L, state.preview); lua_setfield(L, -2, "preview");
            lua_pushboolean(L, state.focused); lua_setfield(L, -2, "focused");
            lua_pushboolean(L, state.selected); lua_setfield(L, -2, "selected");
            lua_pushstring(L, state.preview ? "preview" :
                (d2d && d2d->surfaceKind ? d2d->surfaceKind : "desktop"));
            lua_setfield(L, -2, "surface");
            return 1;
        }, &arguments, 1, error);
    return pushed ? 1 : DeferLuaError(caller, error);
}

int lua_WidgetContext(lua_State* L)
{
    const int result = LuaWidgetContextWithDeferredError(L);
    return result < 0 ? lua_error(L) : result;
}

void WidgetEngine::RenderAll(ID2D1DeviceContext* context)
{
    d2dState_->ctx = context;
}

// Host-controlled visuals have their own effective preference. The public Lua
// system accessibility snapshot and animation.request semantics stay intact.
bool HostManagedAnimationsSuppressed() noexcept
{
    return !snowdesktop::animation::RuntimeAnimationsEnabled();
}

using snowdesktop::widget_runtime::DrawImageFit;
using snowdesktop::widget_runtime::PackageImageSize;
using snowdesktop::widget_runtime::ViewNode;
using snowdesktop::widget_runtime::ViewThemePalette;
using snowdesktop::widget_runtime::ViewTransitionRuntime;
using snowdesktop::widget_runtime::WidgetInteractionRegions;
using snowdesktop::widget_runtime::WidgetPanelContentClip;
using snowdesktop::widget_runtime::WidgetViewHost;

ViewThemePalette BuildWidgetViewThemePalette(D2DState* state,
    const std::wstring& widgetId)
{
    const WidgetSystemEnvironment system = QueryWidgetSystemEnvironment();
    const LuaWidgetTheme theme = state && state->engine
        ? state->engine->RuntimeGetWidgetTheme(widgetId)
        : LuaWidgetTheme{};
    return snowdesktop::widget_runtime::BuildWidgetViewThemePalette(
        theme, system.highContrast, system.accentColor);
}

namespace
{
class EngineWidgetViewHost final : public WidgetViewHost
{
public:
    explicit EngineWidgetViewHost(D2DState* state) : state_(state) {}

    ViewThemePalette ThemePalette() const override
    {
        return BuildWidgetViewThemePalette(state_, state_->currentWidgetId);
    }

    std::optional<std::wstring> ResolveFont(std::string_view name) const override
    {
        return state_->engine ? state_->engine->RuntimeResolvePackageResource(
            state_->currentWidgetId, name, "font") : std::nullopt;
    }

    bool ReadFocusedInput(const std::string& key, std::wstring& text,
        std::size_t& cursor, std::size_t& selectionAnchor,
        std::wstring& composition, std::size_t& compositionCursor) const override
    {
        return state_->engine && state_->engine->RuntimeGetFocusedHostInput(
            state_->currentWidgetId, key, text, cursor, selectionAnchor,
            composition, compositionCursor);
    }

    bool IsSelected() const override
    {
        return state_->engine &&
            state_->engine->RuntimeIsWidgetSelected(state_->currentWidgetId);
    }

    int InputScrollOffset(const std::string& key) const override
    {
        return state_->engine
            ? state_->engine->RuntimeGetScrollOffset(state_->currentWidgetId, key)
            : 0;
    }

    ID2D1Bitmap1* LoadImage(std::string_view name, float width, float height,
        DrawImageFit fit, bool nearest, PackageImageSize& original) const override
    {
        if (!state_->engine) return nullptr;
        if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(name))
            return LoadRuntimeImageBitmap(state_, state_->currentWidgetId, name);
        const auto contentKey = state_->engine->RuntimeResolvePackageImageContentKey(
            state_->currentWidgetId, name);
        return contentKey ? LoadImageBitmap(state_, *contentKey,
            width, height, fit, nearest, &original) : nullptr;
    }

    std::optional<std::wstring> ResolveReference(const std::string& reference) const override
    {
        return state_->engine ? state_->engine->RuntimeResolveViewReference(
            state_->currentWidgetId, reference) : std::nullopt;
    }

    bool IsPreview() const override
    {
        return state_->engine && state_->engine->IsPreviewOnly();
    }

    ID2D1Bitmap1* LoadShellIcon(const std::wstring& path) const override
    {
        EnsureBitmapCachesForCurrentDevice(state_);
        if (state_->shellIconFailures.contains(path)) return nullptr;
        if (const auto cached = state_->shellIconCache.find(path);
            cached != state_->shellIconCache.end())
            return cached->second.Get();
        if (state_->shellIconLoader)
            state_->shellIconLoader->Request(path, state_->currentWidgetId);
        return nullptr;
    }

private:
    D2DState* state_;
};

bool DrawWidgetViewTree(D2DState* state, const ViewNode& tree,
    const WidgetInteractionRegions& regions, std::string_view focusedKey,
    ViewTransitionRuntime& transitions, bool reducedMotion,
    WidgetPanelContentClip* contentClip = nullptr)
{
    return snowdesktop::widget_runtime::DrawWidgetViewTree(state,
        EngineWidgetViewHost(state), tree, regions, focusedKey, transitions,
        reducedMotion, contentClip);
}

void DrawWidgetSelectOverlays(D2DState* state, const ViewNode& node,
    const WidgetInteractionRegions& regions, std::string_view focusedKey,
    float viewportHeight)
{
    snowdesktop::widget_runtime::DrawWidgetSelectOverlays(
        state, EngineWidgetViewHost(state), node, regions, focusedKey, viewportHeight);
}

void DrawWidgetViewTooltip(D2DState* state, const WidgetInteractionRegions& regions)
{
    if (!state || !state->ctx || !state->dwrite) return;
    const auto context = state->engine
        ? state->engine->RuntimeGetWidgetContextState(state->currentWidgetId)
        : LuaWidgetContextState{};
    if (state->engine && state->engine->UsesHostedInteractionTooltips() && !context.preview)
        return;
    const float scale = static_cast<float>(context.surface.dpiX) / 96.f;
    snowdesktop::widget_runtime::DrawWidgetViewTooltip(state, regions, scale);
}
}

bool WidgetEngine::MeasureHoveredInteractionTooltip(const std::wstring& widgetId,
    std::string_view surface, RECT frame, snowdesktop::NativeTooltipTextLayout& measured,
    D2D1_RECT_F& bounds, std::wstring& name) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !d2dState_ || !d2dState_->dwrite) return false;
    const auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || (IsPanelSurface(surface)
        ? (!widget.panelActive || widget.panelSurface != surface)
        : !widget.desktopVisible)) return false;
    const auto& regions = InteractionRegionsForSurface(widget, surface);
    const auto context = RuntimeGetWidgetContextState(widgetId);
    const float scale = static_cast<float>(context.surface.dpiX) / 96.f;
    return snowdesktop::widget_runtime::MeasureWidgetViewTooltip(d2dState_->dwrite,
        D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
            static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
        regions, scale, measured, bounds, name);
}

static LuaWidget::HostControl BuildViewScrollHostControl(
    const snowdesktop::widget_runtime::ViewScrollViewport& viewport, bool panel)
{
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Scroll;
    control.id = viewport.key;
    control.rect = {
        static_cast<LONG>(std::lround(viewport.frame.x)),
        static_cast<LONG>(std::lround(viewport.frame.y)),
        static_cast<LONG>(std::lround(
            viewport.frame.x + viewport.frame.width)),
        static_cast<LONG>(std::lround(
            viewport.frame.y + viewport.frame.height)),
    };
    if (panel)
    {
        control.selectPopup = viewport.selectPopup;
        control.initialScrollOffset =
            static_cast<int>(std::lround(viewport.offset));
    }
    if (panel && viewport.scrollbarFrame)
    {
        const auto& track = *viewport.scrollbarFrame;
        control.rect.right = static_cast<LONG>(std::lround(
            track.x + track.width));
        control.rect.bottom = static_cast<LONG>(std::lround(
            track.y + track.height));
    }
    control.horizontal = viewport.orientation ==
        snowdesktop::widget_runtime::ViewOrientation::Horizontal;
    const int viewportExtent = std::max(1,
        static_cast<int>(std::lround(viewport.viewportExtent)));
    const int contentExtent = std::max(viewportExtent,
        static_cast<int>(std::lround(viewport.contentExtent)));
    control.viewportWidth = std::max(1,
        static_cast<int>(std::lround(viewport.frame.width)));
    control.contentWidth = control.viewportWidth;
    control.viewportHeight = std::max(1,
        static_cast<int>(std::lround(viewport.frame.height)));
    control.contentHeight = control.viewportHeight;
    if (control.horizontal)
    {
        control.viewportWidth = viewportExtent;
        control.contentWidth = contentExtent;
    }
    else
    {
        control.viewportHeight = viewportExtent;
        control.contentHeight = contentExtent;
    }
    return control;
}

static LuaWidget::HostControl BuildViewInputHostControl(
    WidgetEngine& engine, D2DState* state, const std::wstring& widgetId,
    const snowdesktop::widget_runtime::ViewInputControl& input)
{
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Input;
    control.id = input.key;
    control.rect = {
        static_cast<LONG>(std::lround(input.frame.x)),
        static_cast<LONG>(std::lround(input.frame.y)),
        static_cast<LONG>(std::lround(
            input.frame.x + input.frame.width)),
        static_cast<LONG>(std::lround(
            input.frame.y + input.frame.height)),
    };
    if (input.clip)
    {
        control.clipRect = RECT{
            static_cast<LONG>(std::lround(input.clip->x)),
            static_cast<LONG>(std::lround(input.clip->y)),
            static_cast<LONG>(std::lround(
                input.clip->x + input.clip->width)),
            static_cast<LONG>(std::lround(
                input.clip->y + input.clip->height)),
        };
    }
    control.enabled = input.enabled;
    control.focusable = input.focusable;
    control.readOnly = input.readOnly;
    control.controlled = true;
    control.numeric = input.type ==
        snowdesktop::widget_runtime::ViewNodeType::NumberInput;
    control.controlledText = input.value;
    control.placeholder = input.placeholder;
    control.changeAction = input.changeAction;
    control.selection = input.selection;
    control.selectionChangeAction = input.selectionChangeAction;
    control.focusAction = input.focusAction;
    control.blurAction = input.blurAction;
    control.submitAction = input.submitAction;
    control.selectAll = input.selectAll;
    control.liveUpdate = input.liveUpdate;
    control.multiline = input.type ==
        snowdesktop::widget_runtime::ViewNodeType::TextArea;
    control.fontSize = input.fontSize;
    control.padding = input.padding;
    control.maximumUtf8Bytes = input.maximumUtf8Bytes == 0
        ? snowdesktop::widget_runtime::ViewTreeLimits::MaximumTextBytes
        : input.maximumUtf8Bytes;
    control.minimum = input.minimum;
    control.maximum = input.maximum;
    control.step = input.step;
    control.viewportHeight = std::max(1,
        static_cast<int>(std::lround(input.frame.height)));
    control.contentHeight = control.viewportHeight;
    if (control.multiline)
    {
        const float innerWidth = std::max(1.0f,
            input.frame.width - input.padding.left -
                input.padding.right - 8.0f);
        std::wstring metricText = Utf8ToWide(input.value);
        size_t metricCursor = 0;
        size_t metricAnchor = 0;
        std::wstring metricComposition;
        size_t metricCompositionCursor = 0;
        (void)engine.RuntimeGetFocusedHostInput(widgetId,
            input.key, metricText, metricCursor, metricAnchor,
            metricComposition, metricCompositionCursor);
        ComPtr<IDWriteTextLayout> layout =
            CreateHostMultilineTextLayout(state,
                metricText, input.fontSize, innerWidth);
        DWRITE_TEXT_METRICS metrics{};
        if (layout) layout->GetMetrics(&metrics);
        control.contentHeight = std::max(control.viewportHeight,
            static_cast<int>(std::ceil(metrics.height +
                input.padding.top + input.padding.bottom)));
    }
    return control;
}

static std::vector<LuaWidget::HostControl> BuildPanelViewHostControls(
    WidgetEngine& engine, D2DState* state, const std::wstring& widgetId,
    const snowdesktop::widget_runtime::WidgetViewFrame& frame)
{
    std::vector<LuaWidget::HostControl> result;
    result.reserve(frame.scrollViewports.size() + frame.inputControls.size());
    for (const auto& viewport : frame.scrollViewports)
        result.push_back(BuildViewScrollHostControl(viewport, true));
    for (const auto& input : frame.inputControls)
        result.push_back(BuildViewInputHostControl(engine, state, widgetId, input));
    return result;
}

bool WidgetDeclaresFeature(
    const LuaWidget& widget, std::string_view feature)
{
    const auto contains = [feature](const std::vector<std::string>& values) {
        return std::any_of(values.begin(), values.end(),
            [feature](const std::string& value) {
                return value == feature;
            });
    };
    return contains(widget.manifest.requiredFeatures) ||
        contains(widget.manifest.optionalFeatures);
}

bool WidgetDeclaresNativeMarquee(const LuaWidget& widget)
{
    return WidgetDeclaresFeature(widget, "draw.marqueeText");
}

bool HasActiveNativeMarquee(
    const LuaWidget::NativeMarqueeSurface& surface) noexcept
{
    return std::any_of(surface.marquees.begin(), surface.marquees.end(),
        [](const LuaWidget::NativeMarqueeText& marquee) {
            return marquee.scrolling;
        });
}

std::optional<RECT> NativeMarqueeDirtyRect(
    const LuaWidget::NativeMarqueeSurface& surface)
{
    std::optional<RECT> result;
    for (const auto& marquee : surface.marquees)
    {
        if (!marquee.scrolling) continue;
        RECT current{
            static_cast<LONG>(std::floor(marquee.viewport.left)),
            static_cast<LONG>(std::floor(marquee.viewport.top)),
            static_cast<LONG>(std::ceil(marquee.viewport.right)),
            static_cast<LONG>(std::ceil(marquee.viewport.bottom)),
        };
        if (IsRectEmpty(&current)) continue;
        if (!result)
            result = current;
        else
        {
            RECT combined{};
            UnionRect(&combined, &*result, &current);
            result = combined;
        }
    }
    return result;
}

static bool BeginNativeMarqueeCapture(D2DState* state,
    LuaWidget::NativeMarqueeSurface& surface,
    snowdesktop::widget_runtime::WidgetCommandCapture& capture)
{
    surface.pendingMarquees.clear();
    surface.collecting = false;
    surface.requiresLuaRender = true;
    if (!state || !state->ctx) return false;
    EnsureBitmapCachesForCurrentDevice(state);
    using Result = snowdesktop::widget_runtime::WidgetCommandCapture::BeginResult;
    if (capture.Begin(*state, state->immediateClipRects,
            state->bitmapDevice.Get(), state->immediateCommandContext) != Result::Started)
        return false;
    surface.collecting = true;
    surface.requiresLuaRender = false;
    return true;
}

static void StepNativeMarqueeOffsets(LuaWidget::NativeMarqueeSurface& surface,
    std::chrono::steady_clock::time_point now)
{
    const float elapsed = surface.lastAdvance.time_since_epoch().count() == 0
        ? 0.0f : std::chrono::duration<float, std::milli>(now - surface.lastAdvance).count();
    for (auto& marquee : surface.marquees)
    {
        if (!marquee.scrolling) continue;
        marquee.offset = snowdesktop::widget_runtime::AdvanceDrawMarqueeOffset(
            marquee.offset, elapsed, marquee.speed /
                static_cast<float>(snowdesktop::animation::RuntimeDurationScale()),
            marquee.textWidth + marquee.gap);
    }
    surface.lastAdvance = now;
}

static void CommitNativeMarqueeCapture(D2DState* state,
    LuaWidget::NativeMarqueeSurface& surface,
    ComPtr<ID2D1CommandList> commands)
{
    const auto now = std::chrono::steady_clock::now();
    if (surface.compositionManaged) StepNativeMarqueeOffsets(surface, now);
    for (auto& pending : surface.pendingMarquees)
    {
        const auto previous = std::find_if(surface.marquees.begin(),
            surface.marquees.end(), [&pending](const auto& candidate) {
                return candidate.key == pending.key;
            });
        const float cycle = pending.textWidth + pending.gap;
        if (pending.scrolling && previous != surface.marquees.end() &&
            std::isfinite(previous->offset) && cycle > 0.0f)
            pending.offset = std::fmod(previous->offset, cycle);
    }
    surface.marquees = std::move(surface.pendingMarquees);
    surface.pendingMarquees.clear();
    surface.staticCommands = std::move(commands);
    surface.commandDevice.Reset();
    if (state) surface.commandDevice = state->bitmapDevice;
    surface.lastAdvance = now;
    surface.framePending = false;
}

static void DrawNativeMarqueeSurface(D2DState* state,
    const LuaWidget::NativeMarqueeSurface& surface,
    bool reducedMotion, bool drawMarquees = true)
{
    if (!state || !state->ctx) return;
    if (surface.staticCommands)
        state->ctx->DrawImage(surface.staticCommands.Get());
    if (!drawMarquees) return;
    for (const auto& marquee : surface.marquees)
    {
        if (!marquee.layout ||
            marquee.viewport.right <= marquee.viewport.left ||
            marquee.viewport.bottom <= marquee.viewport.top)
            continue;
        auto* brush = GetCachedBrush(
            state, marquee.color, marquee.alpha);
        if (!brush) continue;
        state->ctx->PushAxisAlignedClip(marquee.viewport,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float offset = marquee.scrolling && !reducedMotion
            ? marquee.offset : 0.0f;
        const float firstX = marquee.originX - offset;
        state->ctx->DrawTextLayout(D2D1::Point2F(
                firstX, marquee.originY),
            marquee.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (marquee.scrolling && !reducedMotion)
        {
            state->ctx->DrawTextLayout(D2D1::Point2F(
                    firstX + marquee.textWidth + marquee.gap,
                    marquee.originY),
                marquee.layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        state->ctx->PopAxisAlignedClip();
    }
}

bool AdvanceNativeMarqueeSurface(
    LuaWidget::NativeMarqueeSurface& surface,
    std::chrono::steady_clock::time_point now)
{
    if (surface.compositionManaged || !HasActiveNativeMarquee(surface))
        return false;
    StepNativeMarqueeOffsets(surface, now);
    surface.framePending = true;
    return true;
}

static void ConfigureDesktopContentLayout(D2DState* state,
    lua_State* lua, int descriptorIndex)
{
    if (!state || !lua) return;
    lua_getfield(lua, descriptorIndex, "showTitle");
    const bool showTitle = !lua_isnil(lua, -1) &&
        lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
    lua_getfield(lua, descriptorIndex, "bottomBarHover");
    const bool bottomBarHover = lua_isnil(lua, -1) ||
        lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
    const int scaledBarHeight = static_cast<int>(std::round(
        static_cast<float>(state->barHeight) *
        CalculateWidgetCellScale(
            std::max(4, state->gridCellW),
            std::max(4, state->gridCellH))));
    const int reservedBarHeight =
        snowdesktop::widget_chrome_rules::ReservedBottomBarHeight(
            showTitle, bottomBarHover, scaledBarHeight);
    state->layoutContentWidth = std::max(1.0f,
        state->widgetRect.right - state->widgetRect.left);
    state->layoutContentHeight = std::max(1.0f,
        state->widgetRect.bottom - state->widgetRect.top -
            static_cast<float>(reservedBarHeight));
}

bool WidgetEngine::RenderWidgetBackgroundLayer(
    const std::wstring& widgetId, ID2D1DeviceContext* context,
    RECT bounds, int columns, int rows, float inheritedBlurRadius,
    float cornerRadius)
{
    snowdesktop::performance::Scope performanceScope("widget.render", "backgroundLayer", widgetId);
    const int index = FindWidget(widgetId);
    if (index < 0 || !context || IsRectEmpty(&bounds)) return false;
    LuaWidget& widget = widgets_[index];
    if (!widget.valid || !widget.state) return false;

    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");
    BeginImageFrame(d2dState_, true);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    lua_State* state = widget.state;
    d2dState_->ctx = context;
    d2dState_->gridColumns = std::max(1, columns);
    d2dState_->gridRows = std::max(1, rows);
    SetWidgetRectContext(d2dState_, bounds);

    auto recordError = [&](std::string message) {
        d2dState_->backgroundCache.Erase(widgetId);
        if (message.size() > 4096) message.resize(4096);
        if (widget.backgroundLayerError == message) return;
        widget.backgroundLayerError = message;
        RuntimeAddLog(widgetId, "error", message);
    };

    lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(state, -1)) return false;
    const int descriptor = lua_absindex(state, -1);
    ConfigureDesktopContentLayout(d2dState_, state, descriptor);
    lua_getfield(state, descriptor, "backgroundLayer");
    if (lua_isnil(state, -1))
    {
        widget.backgroundLayerError.clear();
        d2dState_->backgroundCache.Erase(widgetId);
        return false;
    }
    if (!lua_istable(state, -1))
    {
        recordError("backgroundLayer must remain a table while rendering");
        return false;
    }
    const int background = lua_absindex(state, -1);

    float opacity = 1.0f;
    lua_getfield(state, background, "opacity");
    if (!lua_isnil(state, -1))
    {
        const double value = lua_tonumber(state, -1);
        if (lua_type(state, -1) != LUA_TNUMBER || !std::isfinite(value) ||
            value < 0.0 || value > 1.0)
        {
            recordError(
                "backgroundLayer.opacity must be finite and between 0 and 1");
            return false;
        }
        opacity = static_cast<float>(value);
    }
    lua_pop(state, 1);

    float blurRadius = std::clamp(
        inheritedBlurRadius, 0.0f, 48.0f);
    lua_getfield(state, background, "blurRadius");
    if (!lua_isnil(state, -1))
    {
        const double value = lua_tonumber(state, -1);
        if (lua_type(state, -1) != LUA_TNUMBER || !std::isfinite(value) ||
            value < 0.0 || value > 48.0)
        {
            recordError(
                "backgroundLayer.blurRadius must be finite and between 0 and 48");
            return false;
        }
        blurRadius = static_cast<float>(value);
    }
    lua_pop(state, 1);

    lua_getfield(state, background, "render");
    if (!lua_isfunction(state, -1))
    {
        recordError("backgroundLayer.render must remain a function");
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.PushRenderArguments(state, pushContext))
    {
        recordError("backgroundLayer lifecycle is not initialized");
        return false;
    }

    EnsureBitmapCachesForCurrentDevice(d2dState_);
    snowdesktop::widget_runtime::WidgetCommandCapture capture;
    using CaptureResult = snowdesktop::widget_runtime::WidgetCommandCapture::BeginResult;
    switch (capture.Begin(*d2dState_, d2dState_->immediateClipRects,
            d2dState_->bitmapDevice.Get(), d2dState_->immediateCommandContext))
    {
    case CaptureResult::NoDevice:
        recordError("backgroundLayer cannot access the Direct2D device");
        return false;
    case CaptureResult::NoRecordingContext:
        recordError("backgroundLayer cannot create a recording context");
        return false;
    case CaptureResult::NoCommandList:
        recordError("backgroundLayer cannot create a command list");
        return false;
    case CaptureResult::Started:
        break;
    }
    d2dState_->backgroundLayerActive = true;

    const int callResult = snowdesktop::lua_runtime::ProtectedCall(
        state, 2, 0);
    std::string callbackError;
    if (callResult != LUA_OK)
    {
        const char* value = lua_tostring(state, -1);
        callbackError = value ? value : "backgroundLayer render failed";
        lua_pop(state, 1);
    }
    const HRESULT drawResult = capture.FinishDraw();
    d2dState_->backgroundLayerActive = false;
    const HRESULT closeResult = capture.Close();
    if (!callbackError.empty())
    {
        recordError("backgroundLayer.render: " + callbackError);
        return false;
    }
    if (FAILED(drawResult) || FAILED(closeResult))
    {
        recordError("backgroundLayer command recording failed");
        return false;
    }

    const auto cachedBackground = d2dState_->backgroundCache.Resolve(widgetId,
        context, capture.Commands(), D2D1::RectF(static_cast<float>(bounds.left),
            static_cast<float>(bounds.top), static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom)), blurRadius, cornerRadius,
        [this](ID2D1Bitmap* bitmap) {
            // These caches create immutable decoded bitmaps. Arbitrary target
            // bitmaps or effect inputs must never enter the signature cache.
            const auto contains = [bitmap](const auto& cache) {
                return std::any_of(cache.begin(), cache.end(), [&](const auto& item) {
                    return static_cast<ID2D1Bitmap*>(item.second.Get()) == bitmap;
                });
            };
            return contains(d2dState_->imageCache) || d2dState_->runtimeImageCache.ContainsBitmap(bitmap) ||
                d2dState_->imageSamplingCache.Contains(bitmap);
        });
    using CacheOutcome = snowdesktop::widget_runtime::WidgetBackgroundCache::Outcome;
    snowdesktop::performance::Value("widget.background.cache",
        cachedBackground.outcome == CacheOutcome::Hit ? "hit" :
            cachedBackground.outcome == CacheOutcome::Miss ? "miss" : "bypass", widgetId, 1);
    snowdesktop::performance::Value("widget.background.cache", "retained_bytes_estimate", {},
        static_cast<double>(d2dState_->backgroundCache.RetainedBytes()));
    if (cachedBackground.bitmap)
    {
        if (auto* paint = d2dState_->desktopSurfacePaint)
        {
            paint->backgroundKnown = true;
            paint->background = cachedBackground.bitmap;
            paint->backgroundOpacity = opacity;
        }
        // Cached pixels already contain the rounded mask. Applying opacity to
        // the premultiplied bitmap preserves the group's opacity semantics.
        context->DrawBitmap(cachedBackground.bitmap.Get(),
            D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
            opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        widget.backgroundLayerError.clear();
        return true;
    }
    if (auto* paint = d2dState_->desktopSurfacePaint)
    {
        D2D1_RECT_F ink{};
        paint->backgroundKnown =
            SUCCEEDED(context->GetImageLocalBounds(capture.Commands(), &ink)) &&
            (ink.left >= ink.right || ink.top >= ink.bottom);
    }
    ComPtr<ID2D1Image> output;
    if (FAILED(capture.Commands()->QueryInterface(IID_PPV_ARGS(&output))) || !output)
    {
        recordError("backgroundLayer cannot expose its command image");
        return false;
    }
    ComPtr<ID2D1Effect> blur;
    if (!cachedBackground.bitmap && blurRadius > 0.0f)
    {
        if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) ||
            !blur)
        {
            recordError("backgroundLayer cannot create the blur effect");
            return false;
        }
        blur->SetInput(0, capture.Commands());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
            blurRadius);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
            D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
            D2D1_BORDER_MODE_HARD);
        blur->GetOutput(&output);
        if (!output)
        {
            recordError("backgroundLayer blur did not expose an image");
            return false;
        }
    }

    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    const D2D1_RECT_F frame = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
    const auto clip = snowdesktop::smooth_corners::Geometry(factory.Get(), frame, cornerRadius);
    if (!clip)
    {
        recordError("backgroundLayer cannot create the rounded clip");
        return false;
    }
    context->PushLayer(D2D1::LayerParameters(frame, clip.Get(),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::Matrix3x2F::Identity(), opacity), nullptr);
    context->DrawImage(output.Get());
    context->PopLayer();
    widget.backgroundLayerError.clear();
    return true;
}

bool WidgetEngine::CanReplayDesktopMarqueeFrame(const std::wstring& widgetId,
    const RECT& bounds, int columns, int rows, ID2D1Device* device) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& widget = widgets_[index];
    const auto& marquee = widget.desktopMarquee;
    if (!widget.valid || !widget.state || widget.preview || !widget.desktopVisible || !widget.hostVisible ||
        widget.desktopOccluded || widget.desktopOcclusionResumePending ||
        widget.viewTransitionFramePending || widget.hasBackgroundLayer ||
        widget.lastColumns != columns || widget.lastRows != rows ||
        !EqualRect(&widget.lastBounds, &bounds) || !marquee.framePending || marquee.compositionManaged ||
        marquee.requiresLuaRender || !marquee.staticCommands ||
        !device || marquee.commandDevice.Get() != device)
        return false;

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    return GetFileAttributesExW(widget.filePath.c_str(), GetFileExInfoStandard, &attributes) &&
        CompareFileTime(&attributes.ftLastWriteTime, &widget.lastModified) == 0;
}

void WidgetEngine::RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath,
    ID2D1DeviceContext* context, RECT bounds, int columns, int rows)
{
    snowdesktop::performance::Scope performanceScope("widget.render", "desktop", widgetId);
    // A stack is a surface owner; invalidation belongs to the active Lua VM.
    snowdesktop::performance::DrawLink("desktop", widgetId, true);
    (void)scriptPath;

    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    LuaWidget* found = &widgets_[idx];
    PreviewExecutionScope previewScope(
        found && found->preview ? &found->previewStorage : nullptr);

    // Hot-reload: check if file changed or deleted
    if (!found->preview)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        bool exists = GetFileAttributesExW(found->filePath.c_str(), GetFileExInfoStandard, &attr) != 0;
        if (!exists) { found->valid = false; return; }
        if (CompareFileTime(&attr.ftLastWriteTime, &found->lastModified) != 0)
        {
            if (!ReloadWidget(widgetId)) return;
            idx = FindWidget(widgetId);
            if (idx < 0) return;
            found = &widgets_[idx];
        }
    }
    if (!found->valid) return;

    lua_State* state = found->state;
    if (!state) return;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    d2dState_->ctx = context;
    DrainShellIconResults(d2dState_);
    const RECT previousBounds = found->lastBounds;
    const auto previousPaintRevision = std::exchange(found->desktopPaintRevision, 0);
    auto* surfacePaint = d2dState_->desktopSurfacePaint;
    const bool resumeAfterOcclusion = std::exchange(found->desktopOcclusionResumePending, false);
    const int previousColumns = found->lastColumns;
    const int previousRows = found->lastRows;
    found->lastBounds = bounds;
    d2dState_->gridColumns = std::max(1, columns);
    d2dState_->gridRows = std::max(1, rows);
    SetWidgetRectContext(d2dState_, bounds);

    found->desktopVisible = true;
    found->keepRuntimeActiveForHiddenPage = false;
    ApplyWidgetHostVisibility(*found, true);
    if (found->lastColumns != columns || found->lastRows != rows)
    {
        found->lastColumns = std::max(1, columns);
        found->lastRows = std::max(1, rows);
        const int currentColumns = found->lastColumns;
        const int currentRows = found->lastRows;
        (void)InvokeLifecycleEvent(*found, "resize",
            [currentColumns, currentRows](lua_State* eventState) {
                lua_pushinteger(eventState, currentColumns);
                lua_setfield(eventState, -2, "columns");
                lua_pushinteger(eventState, currentRows);
                lua_setfield(eventState, -2, "rows");
            });
    }
    const bool transitionFrameRequested =
        std::exchange(found->viewTransitionFramePending, false);
    const bool transitionGeometryUnchanged =
        EqualRect(&previousBounds, &bounds) != FALSE &&
        previousColumns == columns && previousRows == rows;
    if (!resumeAfterOcclusion && transitionFrameRequested && transitionGeometryUnchanged &&
        found->viewTree)
    {
        BeginImageFrame(d2dState_);
        d2dState_->widgetClipDepth = 0;
        const bool transitionActive = DrawWidgetViewTree(
            d2dState_, *found->viewTree, found->interactionRegions,
            VisualViewFocusForSurface(*found, "desktop"),
            found->viewTransitions,
            found->preview || !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed());
        DrawWidgetSelectOverlays(d2dState_, *found->viewTree,
            found->interactionRegions,
            VisualViewFocusForSurface(*found, "desktop"),
            found->viewTree->frame.height);
        DrawHostViewInteractionOverlays(*found,
            found->interactionRegions,
            found->viewKeyboardFocusKey, true);
        DrawWidgetViewTooltip(d2dState_, found->interactionRegions);
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        if (transitionActive || found->viewIndeterminateProgressActive)
            (void)ScheduleAnimationFrame(*found);
        return;
    }
    const bool nativeMarqueeFrame =
        std::exchange(found->desktopMarquee.framePending, false);
    if (!resumeAfterOcclusion && nativeMarqueeFrame && transitionGeometryUnchanged &&
        !found->desktopMarquee.requiresLuaRender &&
        found->desktopMarquee.staticCommands)
    {
        EnsureBitmapCachesForCurrentDevice(d2dState_);
        if (found->desktopMarquee.commandDevice.Get() ==
                d2dState_->bitmapDevice.Get())
        {
            DrawNativeMarqueeSurface(d2dState_, found->desktopMarquee,
                found->preview || !widgetTimerRequestCallback_ ||
                    HostManagedAnimationsSuppressed());
            DrawHostViewInteractionOverlays(*found,
                found->interactionRegions,
                found->viewKeyboardFocusKey, true);
            DrawWidgetViewTooltip(d2dState_, found->interactionRegions);
            (void)ScheduleAnimationFrame(*found);
            return;
        }
        found->desktopMarquee.requiresLuaRender = true;
    }
    // A replayed command list still consumes its images. Replace demands only
    // when drawing the tree or recording a fresh Lua frame.
    if (resumeAfterOcclusion) (void)ScheduleAnimationFrame(*found);
    BeginImageFrame(d2dState_);
    snowdesktop::widget_runtime::HostControlFrame<LuaWidget::HostControl>
        hostControlFrame([this, ownerId = widgetId]() -> std::vector<LuaWidget::HostControl>* {
            const int currentIndex = FindWidget(ownerId);
            return currentIndex < 0 ? nullptr : &widgets_[currentIndex].hostControls;
        }, "desktop");
    found->interactionRegions.BeginFrame();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(state, -1))
    {
        found->interactionRegions.AbortFrame();
        lua_pop(state, 1);
        return;
    }

    // Inject widgetId global
    {
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(state, widUtf8.c_str());
        lua_setfield(state, -2, "widgetId");
    }

    const int descriptorIndex = lua_absindex(state, -1);
    ConfigureDesktopContentLayout(d2dState_, state, descriptorIndex);

    lua_getfield(state, descriptorIndex, "view");
    if (lua_isfunction(state, -1))
    {
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        bool accepted = false;
        std::string viewError;
        std::string_view viewErrorCode;
        if (!found->lifecycle.PushRenderArguments(state, pushContext))
        {
            lua_pop(state, 1);
            viewError = "Widget lifecycle is not initialized";
            viewErrorCode = "view.lifecycle";
        }
        else if (snowdesktop::lua_runtime::ProtectedCall(
                state, 2, 1) != LUA_OK)
        {
            const char* error = lua_tostring(state, -1);
            viewError = error ? error : "Widget view evaluation failed";
            viewErrorCode = "view.evaluate";
            lua_pop(state, 1);
        }
        else
        {
            snowdesktop::widget_runtime::WidgetViewFrame frame;
            auto& candidate = frame.tree;
            const auto& scrollViewports = frame.scrollViewports;
            const auto& inputControls = frame.inputControls;
            viewErrorCode = snowdesktop::widget_runtime::PrepareWidgetViewFrame(
                state, -1, d2dState_, EngineWidgetViewHost(d2dState_),
                found->logicalSlots, found->scrollOffsets,
                [this, found](auto& tree, std::string& error) {
                    return ValidateAndLayoutWidgetView(tree, *found, "desktop",
                        CurrentLayoutContentWidth(d2dState_), CurrentLayoutContentHeight(d2dState_), error);
                },
                frame, viewError);
            if (!viewErrorCode.empty())
                viewError = "view(): " + viewError;
            else
            {
                accepted = frame.SubmitRegions(found->interactionRegions, viewError);
                if (!accepted)
                {
                    viewError = "view(): " + viewError;
                    viewErrorCode = "view.interaction";
                }
                const auto desktopControlCount = std::count_if(
                    found->hostControls.begin(), found->hostControls.end(),
                    [](const auto& control) {
                        return HostControlBelongsToSurface(
                            control, "desktop");
                    });
                if (accepted && desktopControlCount +
                        scrollViewports.size() + inputControls.size() > 128)
                {
                    accepted = false;
                    viewError = "view(): host control limit exceeded (128)";
                    viewErrorCode = "view.hostControls";
                }
                if (accepted)
                {
                    std::unordered_set<std::string> hostKeys;
                    for (const auto& control : found->hostControls)
                    {
                        if (HostControlBelongsToSurface(control, "desktop"))
                            hostKeys.insert(control.id);
                    }
                    for (const auto& viewport : scrollViewports)
                    {
                        if (!hostKeys.insert(viewport.key).second)
                        {
                            accepted = false;
                            viewError = "view(): duplicate host control key: " +
                                viewport.key;
                            viewErrorCode = "view.hostControls";
                            break;
                        }
                    }
                    if (accepted)
                    {
                        for (const auto& input : inputControls)
                        {
                            if (!hostKeys.insert(input.key).second)
                            {
                                accepted = false;
                                viewError = "view(): duplicate host control key: " +
                                    input.key;
                                viewErrorCode = "view.hostControls";
                                break;
                            }
                        }
                    }
                }
                if (accepted)
                {
                    for (const auto& viewport : scrollViewports)
                        RuntimeRegisterHostControl(widgetId,
                            BuildViewScrollHostControl(viewport, false));
                    for (const auto& input : inputControls)
                        RuntimeRegisterHostControl(widgetId,
                            BuildViewInputHostControl(*this, d2dState_, widgetId, input));
                }
            }
            lua_pop(state, 1);
            if (accepted)
            {
                const int currentIndex = FindWidget(widgetId);
                if (currentIndex >= 0)
                {
                    found = &widgets_[currentIndex];
                    for (const auto& viewport : scrollViewports)
                    {
                        if (viewport.initialized)
                            found->scrollOffsets.emplace(viewport.key,
                                static_cast<int>(std::lround(
                                    viewport.offset)));
                    }
                    const bool variableLayoutChanged =
                        CommitVariableVirtualMeasurements(
                            candidate, *found, "desktop");
                    if (found->viewTree)
                    {
                        found->viewTransitions.SetDurationScale(
                            snowdesktop::animation::RuntimeDurationScale());
                        found->viewTransitions.CommitScene(
                            *found->viewTree, candidate,
                            snowdesktop::widget_runtime::
                                ViewTransitionRuntime::Clock::now(),
                            found->preview ||
                                !widgetTimerRequestCallback_ ||
                                HostManagedAnimationsSuppressed());
                    }
                    if (surfacePaint && HasLocalRefresh(candidate))
                    {
                        surfacePaint->revision = ++d2dState_->nextDesktopPaintRevision;
                        if (previousPaintRevision != 0 && found->viewTree &&
                            transitionGeometryUnchanged)
                        {
                            CollectLocalRefresh(*found->viewTree, candidate, bounds,
                                *surfacePaint);
                            if (candidate == *found->viewTree)
                                surfacePaint->baseRevision = previousPaintRevision;
                        }
                    }
                    found->viewTree = std::move(candidate);
                    found->viewIndeterminateProgressActive =
                        snowdesktop::widget_runtime::
                            HasVisibleIndeterminateProgress(
                                *found->viewTree);
                    if (variableLayoutChanged)
                        RuntimeInvalidateHost(widgetId);
                }
                else
                    accepted = false;
            }
        }

        if (!accepted)
        {
            hostControlFrame.Restore();
            found->interactionRegions.AbortFrame();
            if (!viewError.empty())
            {
                RuntimeRecordError(widgetId,
                    FormatViewValidationDiagnostic(
                        viewErrorCode, viewError));
                RuntimeInvalidateHost(widgetId);
            }
        }
        else
        {
            hostControlFrame.Commit();
            RuntimeRecordSuccess(widgetId);
            const auto transition =
                found->interactionRegions.CommitFrame();
            if (transition.Changed())
            {
                float pointerX = 0.0f;
                float pointerY = 0.0f;
                found->interactionRegions.LastPointer(pointerX, pointerY);
                DispatchInteractionTransition(*found, transition,
                    static_cast<int>(pointerX),
                    static_cast<int>(pointerY));
                RuntimeInvalidateHost(widgetId);
            }
            ResolveDeferredHostInputFocus(widgetId,
                d2dState_->surfaceKind ? d2dState_->surfaceKind : "desktop");
            ReconcileHostViewKeyboardFocus(*found, "desktop");
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        if (found->viewTree)
        {
            const bool transitionActive = DrawWidgetViewTree(
                d2dState_, *found->viewTree,
                found->interactionRegions,
                VisualViewFocusForSurface(*found, "desktop"),
                found->viewTransitions,
                found->preview || !widgetTimerRequestCallback_ ||
                    HostManagedAnimationsSuppressed());
            DrawWidgetSelectOverlays(d2dState_, *found->viewTree,
                found->interactionRegions,
                VisualViewFocusForSurface(*found, "desktop"),
                found->viewTree->frame.height);
            if (!RuntimeIsWidgetSelected(found->widgetId) ||
                (!found->viewKeyboardFocusKey.empty() &&
                    !found->interactionRegions.IsKeyboardFocusable(
                        found->viewKeyboardFocusKey)))
            {
                found->viewKeyboardFocusKey.clear();
                found->viewFocusCueVisible = false;
                found->logicalSlotFocus.reset();
            }
            DrawHostViewInteractionOverlays(*found,
                found->interactionRegions,
                found->viewKeyboardFocusKey, true);
            DrawWidgetViewTooltip(
                d2dState_, found->interactionRegions);
            if (transitionActive ||
                found->viewIndeterminateProgressActive)
                (void)ScheduleAnimationFrame(*found);
            if (surfacePaint)
            {
                if (transitionActive || found->viewIndeterminateProgressActive ||
                    !found->interactionRegions.HoveredKey().empty() ||
                    !found->interactionRegions.PressedKey().empty() ||
                    !found->viewKeyboardFocusKey.empty())
                    surfacePaint->revision = surfacePaint->baseRevision = 0;
                found->desktopPaintRevision = surfacePaint->revision;
            }
        }
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        lua_pop(state, 1);
        return;
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "render");
    if (lua_isfunction(state, -1))
    {
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        if (!found->lifecycle.PushRenderArguments(
                state, pushContext))
        {
            lua_pop(state, 2);
            RuntimeRecordError(widgetId,
                "Widget lifecycle is not initialized");
            RuntimeInvalidateHost(widgetId);
            found->interactionRegions.AbortFrame();
            return;
        }
        snowdesktop::widget_runtime::WidgetCommandCapture nativeMarqueeCapture;
        const bool capturingNativeMarquee =
            WidgetDeclaresNativeMarquee(*found) &&
            BeginNativeMarqueeCapture(d2dState_,
                found->desktopMarquee, nativeMarqueeCapture);
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 2, 0) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            if (capturingNativeMarquee)
            {
                nativeMarqueeCapture.Abort();
                found->desktopMarquee.pendingMarquees.clear();
                found->desktopMarquee.collecting = false;
                found->desktopMarquee.requiresLuaRender = true;
            }
            RuntimeRecordError(widgetId, err ? err : "(render error)");
            lua_pop(state, 1);
            snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);

            // draw conspicuous placeholder
            if (d2dState_ && d2dState_->ctx)
            {
                ComPtr<ID2D1SolidColorBrush> brush;
                d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.15f, 1.0f), &brush);
                d2dState_->ctx->FillRectangle(d2dState_->widgetRect, brush.Get());

                // draw white text
                if (d2dState_->dwrite)
                {
                    ComPtr<IDWriteTextFormat> format;
                    const float errFontSize = std::max(9.0f, 15.0f * CalculateWidgetCellScale(
                        d2dState_->gridCellW, d2dState_->gridCellH));
                    snowdesktop::app_fonts::CreateTextFormat(d2dState_->dwrite, L"Segoe UI",
                        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        errFontSize, L"", &format);
                    if (format)
                    {
                        std::wstring wmsg = L"WIDGET ERROR: ";
                        std::string serr = err ? err : "(unknown)";
                        // append part of error
                        size_t maxlen = 128;
                        if (serr.size() > maxlen) serr = serr.substr(0, maxlen) + "...";
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, nullptr, 0);
                        std::wstring werr(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, &werr[0], wlen);
                        wmsg += werr;

                        D2D1_RECT_F r = d2dState_->widgetRect;
                        r.left += 8; r.top += 8; r.right -= 8; r.bottom -= 8;
                        ComPtr<ID2D1SolidColorBrush> textBrush;
                        d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &textBrush);
                        d2dState_->ctx->DrawTextW(wmsg.c_str(), (UINT32)wmsg.size() - 1, format.Get(), &r, textBrush.Get());
                    }
                }
            }
            if (const int currentIndex = FindWidget(widgetId);
                currentIndex >= 0)
            {
                widgets_[currentIndex].interactionRegions.AbortFrame();
            }
            lua_pop(state, 1);
            RuntimeInvalidateHost(widgetId);
            return;
        }
        if (capturingNativeMarquee)
        {
            const HRESULT drawResult = nativeMarqueeCapture.FinishDraw();
            found->desktopMarquee.collecting = false;
            if (FAILED(drawResult) || FAILED(nativeMarqueeCapture.Close()))
            {
                found->desktopMarquee.pendingMarquees.clear();
                found->desktopMarquee.requiresLuaRender = true;
                RuntimeRecordError(widgetId,
                    "draw.marqueeText: static draw cache could not be recorded");
                found->interactionRegions.AbortFrame();
                lua_pop(state, 1);
                RuntimeInvalidateHost(widgetId);
                return;
            }
            CommitNativeMarqueeCapture(d2dState_,
                found->desktopMarquee,
                nativeMarqueeCapture.TakeCommands());
            const bool reducedMotion = found->preview ||
                !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed();
            const bool placementStable = hostInputDropWidgetId_ != widgetId;
            if (!placementStable) found->desktopMarquee.compositionManaged = false;
            const bool compositionManaged = placementStable &&
                SyncNativeMarqueeComposition(*found, reducedMotion);
            DrawNativeMarqueeSurface(d2dState_,
                found->desktopMarquee,
                reducedMotion, !compositionManaged);
            if (!compositionManaged &&
                HasActiveNativeMarquee(found->desktopMarquee))
                (void)ScheduleAnimationFrame(*found);
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        hostControlFrame.Commit();
        RuntimeRecordSuccess(widgetId);
        if (const int currentIndex = FindWidget(widgetId);
            currentIndex >= 0)
        {
            auto& current = widgets_[currentIndex];
            const auto transition =
                current.interactionRegions.CommitFrame();
            if (transition.Changed())
            {
                float pointerX = 0.0f;
                float pointerY = 0.0f;
                current.interactionRegions.LastPointer(
                    pointerX, pointerY);
                DispatchInteractionTransition(current, transition,
                    static_cast<int>(pointerX),
                    static_cast<int>(pointerY));
                RuntimeInvalidateHost(widgetId);
            }
            ResolveDeferredHostInputFocus(widgetId,
                d2dState_->surfaceKind ? d2dState_->surfaceKind : "desktop");
            ReconcileHostViewKeyboardFocus(current, "desktop");
        }
    }
    else
    {
        hostControlFrame.Restore();
        found->interactionRegions.AbortFrame();
        lua_pop(state, 1);
    }
    snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
    if (const int currentIndex = FindWidget(widgetId); currentIndex >= 0)
    {
        auto& current = widgets_[currentIndex];
        if (!RuntimeIsWidgetSelected(current.widgetId) ||
            (!current.viewKeyboardFocusKey.empty() &&
                !current.interactionRegions.IsKeyboardFocusable(
                    current.viewKeyboardFocusKey)))
        {
            current.viewKeyboardFocusKey.clear();
            current.viewFocusCueVisible = false;
            current.logicalSlotFocus.reset();
        }
        DrawHostViewInteractionOverlays(current,
            current.interactionRegions,
            current.viewKeyboardFocusKey, true);
        // Immediate-mode regions carry the same tooltip contract as view nodes.
        // Paint after the component's clips and cached content have been restored.
        DrawWidgetViewTooltip(d2dState_, current.interactionRegions);
    }
    lua_pop(state, 1);
}

bool WidgetEngine::RenderWidgetPanel(
    const std::wstring& widgetId,
    ID2D1DeviceContext* context, RECT bounds,
    std::string_view surface, const RECT* scrollbarBounds)
{
    snowdesktop::performance::Scope performanceScope("widget.render", surface, widgetId);
    snowdesktop::performance::DrawLink(surface, widgetId, true);
    const int index = FindWidget(widgetId);
    if (index < 0 || !context)
        return false;
    auto& widget = widgets_[index];
    if (!widget.valid)
        return false;
    lua_State* state = widget.state;
    if (!state)
        return false;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    if (!IsPanelSurface(normalizedSurface))
        return false;
    const std::string callbackName =
        normalizedSurface == "dialog" ? "dialog" :
        normalizedSurface == "popover" ? "popover" :
        "panel";

    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    WidgetSurfaceScope surfaceScope(
        d2dState_, normalizedSurface.c_str());
    BeginImageFrame(d2dState_);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    d2dState_->ctx = context;
    WidgetPanelContentClip contentClip(context, bounds);
    DrainShellIconResults(d2dState_);
    const RECT previousPanelBounds = widget.panelBounds;
    const std::string previousPanelSurface = widget.panelSurface;
    const bool panelWasActive = widget.panelActive;
    widget.panelBounds = bounds;
    widget.panelSurface = normalizedSurface;
    widget.panelActive = true;
    SetWidgetRectContext(d2dState_, bounds);
    ApplyWidgetHostVisibility(widget, true);
    const auto drawPanelView = [this, &normalizedSurface, &contentClip](LuaWidget& current) {
        const bool active = DrawWidgetViewTree(
            d2dState_, *current.panelViewTree, current.panelInteractionRegions,
            VisualViewFocusForSurface(current, normalizedSurface),
            current.panelViewTransitions, current.preview || !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed(), &contentClip);
        DrawWidgetSelectOverlays(d2dState_, *current.panelViewTree,
            current.panelInteractionRegions,
            VisualViewFocusForSurface(current, normalizedSurface),
            current.panelViewTree->frame.height);
        DrawWidgetViewTooltip(d2dState_, current.panelInteractionRegions);
        return active;
    };
    const bool transitionFrameRequested =
        std::exchange(widget.panelViewTransitionFramePending, false);
    if (transitionFrameRequested && panelWasActive &&
        EqualRect(&previousPanelBounds, &bounds) != FALSE &&
        previousPanelSurface == normalizedSurface &&
        widget.panelViewTree)
    {
        d2dState_->widgetClipDepth = 0;
        const bool transitionActive = drawPanelView(widget);
        DrawHostViewInteractionOverlays(widget,
            widget.panelInteractionRegions,
            widget.panelViewKeyboardFocusKey, false);
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        if (transitionActive || widget.panelIndeterminateProgressActive)
            (void)ScheduleAnimationFrame(widget);
        return true;
    }
    std::vector<LuaWidget::HostControl> previousPanelControls;
    for (const auto& control : widget.hostControls)
    {
        if (HostControlBelongsToSurface(control, normalizedSurface))
            previousPanelControls.push_back(control);
    }
    std::erase_if(widget.hostControls, [&normalizedSurface](
            const auto& control) {
        return HostControlBelongsToSurface(
            control, normalizedSurface);
    });
    widget.panelInteractionRegions.BeginFrame();
    d2dState_->widgetClipDepth = 0;

    const auto restorePanelControls = [this, &widgetId,
        &previousPanelControls, &normalizedSurface]() {
        const int currentIndex = FindWidget(widgetId);
        if (currentIndex < 0) return;
        auto& controls = widgets_[currentIndex].hostControls;
        std::erase_if(controls, [&normalizedSurface](const auto& control) {
            return HostControlBelongsToSurface(
                control, normalizedSurface);
        });
        controls.insert(controls.end(), previousPanelControls.begin(),
            previousPanelControls.end());
    };

    lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(state, -1))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 1);
        return false;
    }
    lua_getfield(state, -1, callbackName.c_str());
    if (!lua_isfunction(state, -1))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 2);
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.PushRenderArguments(state, pushContext))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 2);
        RuntimeRecordError(widgetId, FormatViewValidationDiagnostic(
            "view.lifecycle",
            "Widget lifecycle is not initialized"));
        return false;
    }
    widget.panelFrameOpen = true;
    const bool succeeded = snowdesktop::lua_runtime::ProtectedCall(
        state, 2, 1) == LUA_OK;
    widget.panelFrameOpen = false;
    bool panelAccepted = succeeded;
    if (!succeeded)
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        const char* error = lua_tostring(state, -1);
        RuntimeRecordError(widgetId, FormatViewValidationDiagnostic(
            "view.evaluate", error ? std::string(error)
                : callbackName + " render error"));
        lua_pop(state, 1);
    }
    else
    {
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        const bool hasView = !lua_isnil(state, -1);
        std::string viewError;
        std::string_view viewErrorCode;
        snowdesktop::widget_runtime::WidgetViewFrame frame;
        auto& candidate = frame.tree;
        auto& scrollViewports = frame.scrollViewports;
        if (hasView)
        {
            viewErrorCode = snowdesktop::widget_runtime::PrepareWidgetViewFrame(
                state, -1, d2dState_, EngineWidgetViewHost(d2dState_),
                widget.logicalSlots, widget.panelScrollOffsets,
                [this, &widget, &normalizedSurface](auto& tree, std::string& error) {
                    return ValidateAndLayoutWidgetView(tree, widget, normalizedSurface,
                        std::max(1.0f, d2dState_->widgetRect.right - d2dState_->widgetRect.left),
                        std::max(1.0f, d2dState_->widgetRect.bottom - d2dState_->widgetRect.top),
                        error);
                },
                frame, viewError);
            if (!viewErrorCode.empty())
            {
                panelAccepted = false;
                viewError = callbackName + "(): " + viewError;
            }
        }

        std::vector<LuaWidget::HostControl> viewControls;
        if (panelAccepted && hasView)
        {
            if (scrollbarBounds)
                snowdesktop::widget_runtime::ApplyViewSurfaceScrollbarGutters(
                    candidate, scrollViewports,
                    {0, 0, static_cast<float>(bounds.right - bounds.left),
                        static_cast<float>(bounds.bottom - bounds.top)},
                    {0, 0, static_cast<float>(scrollbarBounds->right - bounds.left),
                        static_cast<float>(scrollbarBounds->bottom - bounds.top)});
            viewControls = BuildPanelViewHostControls(*this, d2dState_, widgetId, frame);
            const std::size_t existingCount =
                static_cast<std::size_t>(std::count_if(
                    widget.hostControls.begin(), widget.hostControls.end(),
                    [&normalizedSurface](const auto& control) {
                        return HostControlBelongsToSurface(
                            control, normalizedSurface);
                    }));
            if (existingCount + viewControls.size() > 128)
            {
                panelAccepted = false;
                viewError =
                    callbackName +
                    "(): host control limit exceeded (128)";
                viewErrorCode = "view.hostControls";
            }
            std::unordered_set<std::string> keys;
            if (panelAccepted)
            {
                for (const auto& control : widget.hostControls)
                {
                    if (HostControlBelongsToSurface(
                            control, normalizedSurface))
                        keys.insert(control.id);
                }
                for (const auto& control : viewControls)
                {
                    if (!keys.insert(control.id).second)
                    {
                        panelAccepted = false;
                        viewError =
                            callbackName +
                            "(): duplicate host control key: " +
                            control.id;
                        viewErrorCode = "view.hostControls";
                        break;
                    }
                }
            }
            if (panelAccepted &&
                !frame.SubmitRegions(widget.panelInteractionRegions, viewError))
            {
                panelAccepted = false;
                viewError = callbackName + "(): " + viewError;
                viewErrorCode = "view.interaction";
            }
            if (panelAccepted)
            {
                for (auto& control : viewControls)
                    RuntimeRegisterHostControl(widgetId, std::move(control));
            }
        }
        lua_pop(state, 1);

        if (!panelAccepted)
        {
            widget.panelInteractionRegions.AbortFrame();
            restorePanelControls();
            if (!viewError.empty())
            {
                RuntimeRecordError(widgetId,
                    FormatViewValidationDiagnostic(
                        viewErrorCode, viewError));
            }
        }
        else
        {
            const int currentIndex = FindWidget(widgetId);
            if (currentIndex >= 0)
            {
                auto& current = widgets_[currentIndex];
                if (hasView)
                {
                    for (const auto& viewport : scrollViewports)
                    {
                        if (viewport.initialized)
                            current.panelScrollOffsets.emplace(viewport.key,
                                static_cast<int>(std::lround(
                                    viewport.offset)));
                    }
                    const bool variableLayoutChanged =
                        CommitVariableVirtualMeasurements(candidate,
                            current, normalizedSurface);
                    if (current.panelViewTree)
                    {
                        current.panelViewTransitions.SetDurationScale(
                            snowdesktop::animation::RuntimeDurationScale());
                        current.panelViewTransitions.CommitScene(
                            *current.panelViewTree, candidate,
                            snowdesktop::widget_runtime::
                                ViewTransitionRuntime::Clock::now(),
                            current.preview ||
                                !widgetTimerRequestCallback_ ||
                                HostManagedAnimationsSuppressed());
                    }
                    current.panelViewTree = std::move(candidate);
                    current.panelIndeterminateProgressActive =
                        snowdesktop::widget_runtime::
                            HasVisibleIndeterminateProgress(
                                *current.panelViewTree);
                    if (variableLayoutChanged)
                        RuntimeInvalidateHost(widgetId);
                }
                else
                {
                    current.panelViewTree.reset();
                    current.panelViewTransitions.Clear();
                    current.panelViewTransitionFramePending = false;
                    current.panelIndeterminateProgressActive = false;
                }
                const auto transition =
                    current.panelInteractionRegions.CommitFrame();
                if (transition.Changed())
                {
                    float pointerX = 0.0f;
                    float pointerY = 0.0f;
                    current.panelInteractionRegions.LastPointer(
                        pointerX, pointerY);
                    DispatchInteractionTransition(current, transition,
                        static_cast<int>(pointerX),
                        static_cast<int>(pointerY), normalizedSurface);
                    RuntimeInvalidateHost(widgetId);
                }
                ResolveDeferredHostInputFocus(
                    widgetId, normalizedSurface);
                ReconcileHostViewKeyboardFocus(current, normalizedSurface);
                const auto initialFocusKeys =
                    current.panelInteractionRegions.KeyboardFocusableKeys();
                const auto initialFocus = snowdesktop::widget_runtime::
                    BeginAuxiliarySurfaceKeyboardFocus(
                        initialFocusKeys.size(),
                        current.panelInitialKeyboardFocusPending,
                        !current.panelViewKeyboardFocusKey.empty());
                if (!current.panelViewKeyboardFocusKey.empty())
                {
                    current.panelInitialKeyboardFocusPending = false;
                }
                else if (initialFocus && RuntimeFocusViewTarget(
                        widgetId, initialFocusKeys[*initialFocus], "keyboard"))
                {
                    const int focusedIndex = FindWidget(widgetId);
                    if (focusedIndex >= 0)
                    {
                        widgets_[focusedIndex].
                            panelInitialKeyboardFocusPending = false;
                    }
                }
                if (current.panelViewTree)
                {
                    const bool transitionActive = drawPanelView(current);
                    if (transitionActive ||
                        current.panelIndeterminateProgressActive)
                        (void)ScheduleAnimationFrame(current);
                }
                if (!current.panelViewKeyboardFocusKey.empty() &&
                    !current.panelInteractionRegions.IsKeyboardFocusable(
                        current.panelViewKeyboardFocusKey))
                {
                    current.panelViewKeyboardFocusKey.clear();
                    current.panelViewFocusCueVisible = false;
                }
                DrawHostViewInteractionOverlays(current,
                    current.panelInteractionRegions,
                    current.panelViewKeyboardFocusKey, false);
            }
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
    }
    if (panelAccepted)
        RuntimeRecordSuccess(widgetId);
    if (!panelAccepted)
    {
        RuntimeInvalidateHost(widgetId);
        const int currentIndex = FindWidget(widgetId);
        if (currentIndex >= 0)
        {
            auto& current = widgets_[currentIndex];
            if (current.panelViewTree)
            {
                const bool transitionActive = drawPanelView(current);
                if (transitionActive ||
                    current.panelIndeterminateProgressActive)
                    (void)ScheduleAnimationFrame(current);
            }
            DrawHostViewInteractionOverlays(current,
                current.panelInteractionRegions,
                current.panelViewKeyboardFocusKey, false);
        }
    }
    snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
    lua_pop(state, 1);
    return panelAccepted;
}
