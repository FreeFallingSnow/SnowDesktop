#pragma once

#include "widget/runtime/widget_instance.h"
#include "widget_draw_geometry.h"
#include "widget_view_renderer.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct D2DState;

bool IsPanelSurface(std::string_view surface) noexcept;

std::string NormalizeWidgetSurface(std::string_view surface);

snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(LuaWidget& widget, std::string_view surface);

const snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(
    const LuaWidget& widget, std::string_view surface);

bool ValidateAndLayoutWidgetView(
    snowdesktop::widget_runtime::ViewNode& root,
    LuaWidget& widget, std::string_view surface,
    float width, float height, std::string& error);

std::string FormatViewValidationDiagnostic(
    std::string_view code, std::string_view message);

bool CommitVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    LuaWidget& widget, std::string_view surface);

std::string_view VisualViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface);

bool HostControlBelongsToSurface(
    const LuaWidget::HostControl& control, std::string_view surface) noexcept;

void EnsureBitmapCachesForCurrentDevice(D2DState* state);

ID2D1Bitmap1* LoadImageBitmap(
    D2DState* s, const std::string& contentKey,
    float width = 0.0f, float height = 0.0f,
    snowdesktop::widget_runtime::DrawImageFit fit =
        snowdesktop::widget_runtime::DrawImageFit::Fill,
    bool originalPixels = false,
    snowdesktop::widget_runtime::PackageImageSize* originalSize = nullptr);

ID2D1Bitmap1* LoadRuntimeImageBitmap(D2DState* state,
    const std::wstring& widgetId, std::string_view token);

void DrainShellIconResults(D2DState* state);

Microsoft::WRL::ComPtr<ID2D1Bitmap1> BitmapFromHBitmap(
    ID2D1DeviceContext* ctx, HBITMAP hbm);

std::unordered_map<std::string, LuaWidget::VariableVirtualState>&
VariableVirtualStatesForSurface(
    LuaWidget& widget, std::string_view surface);

const std::unordered_map<std::string,
    LuaWidget::VariableVirtualState>& VariableVirtualStatesForSurface(
    const LuaWidget& widget, std::string_view surface);

std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
VariableVirtualMeasurements(
    const LuaWidget::VariableVirtualState& state);

std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    LuaWidget& widget, std::string_view surface);

const std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    const LuaWidget& widget, std::string_view surface);

bool& ViewFocusCueForSurface(
    LuaWidget& widget, std::string_view surface);

bool ViewFocusCueForSurface(
    const LuaWidget& widget, std::string_view surface);

std::string& ViewFocusForSurface(
    LuaWidget& widget, std::string_view surface);

const std::string& ViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface);

bool VariableVirtualStateMatches(
    const LuaWidget::VariableVirtualState& state,
    std::size_t itemCount, float estimatedItemSize, float mainGap,
    bool horizontal, std::uint64_t layoutRevision) noexcept;

void AttachVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    const LuaWidget& widget, std::string_view surface);

struct WidgetSystemEnvironment
{
    bool highContrast = false;
    bool reducedMotion = false;
    double textScale = 1.0;
    int accentColor = 0x0078D4;
    std::string locale;
    std::string region;
    std::string timeZone;
    int utcOffsetMinutes = 0;
    std::string inputLanguage;
};

WidgetSystemEnvironment QueryWidgetSystemEnvironment();

int lua_WidgetContext(lua_State* state);

bool HostManagedAnimationsSuppressed() noexcept;

bool WidgetDeclaresFeature(
    const LuaWidget& widget, std::string_view feature);

bool WidgetDeclaresNativeMarquee(const LuaWidget& widget);

bool HasActiveNativeMarquee(
    const LuaWidget::NativeMarqueeSurface& surface) noexcept;

std::optional<RECT> NativeMarqueeDirtyRect(
    const LuaWidget::NativeMarqueeSurface& surface);

bool AdvanceNativeMarqueeSurface(
    LuaWidget::NativeMarqueeSurface& surface,
    std::chrono::steady_clock::time_point now);
void PauseNativeMarqueeClock(LuaWidget::NativeMarqueeSurface& surface);

snowdesktop::widget_runtime::ViewThemePalette BuildWidgetViewThemePalette(D2DState* state,
    const std::wstring& widgetId);
