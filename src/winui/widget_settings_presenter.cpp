#include "pch.h"

#include "widget_settings_presenter.h"

#include "theme/personalization.h"
#include "settings_presenter_controls.h"
#include "panel_gradient_editor.h"
#include "appearance_sections.h"
#include "theme/theme_library_settings.h"
#include "widget/runtime/widget_theme_selection.h"
#include "edge_light_editor.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace wf = winrt::Windows::Foundation;
namespace ws = winrt::Windows::System;
namespace wui = winrt::Windows::UI;
namespace wr = widget_runtime;

namespace
{

winrt::hstring ToText(std::string_view value)
{
    return winrt::to_hstring(std::string(value));
}

std::string ToUtf8(const winrt::hstring& value)
{
    return winrt::to_string(value);
}

std::wstring ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8,
        MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), required) != required)
    {
        return {};
    }
    return result;
}

bool IsEnter(const muxi::KeyRoutedEventArgs& args) noexcept
{
    return args.Key() == ws::VirtualKey::Enter;
}

bool HasToken(winrt::event_token token) noexcept
{
    return token.value != 0;
}

double NumericValue(const wr::InteractionValue& value, double fallback)
    noexcept
{
    if (value.type == wr::InteractionValue::Type::Integer)
        return static_cast<double>(value.integer);
    if (value.type == wr::InteractionValue::Type::Number &&
        std::isfinite(value.number))
        return value.number;
    return fallback;
}

std::string StringValue(const wr::InteractionValue& value)
{
    return value.type == wr::InteractionValue::Type::String
        ? value.string
        : std::string{};
}

bool BooleanValue(const wr::InteractionValue& value) noexcept
{
    return value.type == wr::InteractionValue::Type::Boolean &&
        value.boolean;
}

std::vector<std::string> StringArrayValue(
    const wr::InteractionValue& value)
{
    std::vector<std::string> result;
    if (value.type != wr::InteractionValue::Type::Array)
        return result;
    result.reserve(value.array.size());
    for (const auto& item : value.array)
        if (item.type == wr::InteractionValue::Type::String)
            result.push_back(item.string);
    return result;
}

wui::Color ToColor(long long encoded) noexcept
{
    const auto value = static_cast<std::uint32_t>(
        std::clamp<long long>(encoded, 0, 0xFFFFFF));
    return {
        255,
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF),
    };
}

long long FromColor(const wui::Color& color) noexcept
{
    return (static_cast<long long>(color.R) << 16) |
        (static_cast<long long>(color.G) << 8) |
        static_cast<long long>(color.B);
}

std::optional<wf::DateTime> ParseDate(std::string_view encoded) noexcept
{
    if (!wr::IsValidDateSettingValue(encoded) || encoded.empty())
        return std::nullopt;
    const int year =
        (encoded[0] - '0') * 1000 + (encoded[1] - '0') * 100 +
        (encoded[2] - '0') * 10 + (encoded[3] - '0');
    try
    {
        winrt::Windows::Globalization::Calendar calendar;
        calendar.ChangeCalendarSystem(L"GregorianCalendar");
        calendar.ChangeClock(L"24HourClock");
        calendar.Day(1);
        calendar.Year(year);
        calendar.Month((encoded[5] - '0') * 10 + (encoded[6] - '0'));
        calendar.Day((encoded[8] - '0') * 10 + (encoded[9] - '0'));
        calendar.Hour(12); calendar.Minute(0); calendar.Second(0); calendar.Nanosecond(0);
        return calendar.GetDateTime();
    }
    catch (...) { return std::nullopt; }
}

std::string FormatDate(const wf::DateTime& date) noexcept
{
    try
    {
        winrt::Windows::Globalization::Calendar calendar;
        calendar.ChangeCalendarSystem(L"GregorianCalendar");
        calendar.SetDateTime(date);
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
            calendar.Year(), calendar.Month(), calendar.Day());
        return buffer;
    }
    catch (...) { return {}; }
}

std::optional<wf::TimeSpan> ParseTime(std::string_view encoded) noexcept
{
    if (!wr::IsValidTimeSettingValue(encoded) || encoded.empty())
        return std::nullopt;
    const int hours = (encoded[0] - '0') * 10 + (encoded[1] - '0');
    const int minutes = (encoded[3] - '0') * 10 + (encoded[4] - '0');
    return std::chrono::duration_cast<wf::TimeSpan>(
        std::chrono::minutes(hours * 60 + minutes));
}

std::string FormatTime(const wf::TimeSpan& time) noexcept
{
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
        time).count();
    constexpr long long minutesPerDay = 24 * 60;
    minutes %= minutesPerDay;
    if (minutes < 0) minutes += minutesPerDay;
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d",
        static_cast<int>(minutes / 60),
        static_cast<int>(minutes % 60));
    return buffer;
}

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
};

muxc::TextBox FindDurationInput(const mux::DependencyObject& root)
{
    if (const auto input = root.try_as<muxc::TextBox>(); input && input.Name() == L"InputBox") return input;
    const int count = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int index = 0; index < count; ++index)
        if (const auto input = FindDurationInput(winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, index))) return input;
    return nullptr;
}

struct MultiSelectOptionControl
{
    std::string value;
    muxc::CheckBox checkBox{nullptr};
    winrt::event_token checked{};
    winrt::event_token unchecked{};
};

struct WidgetFieldControl
{
    wr::WidgetSettingFieldSchema schema;
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    presenter_controls::SettingRow row;
    muxc::Grid editorRow{nullptr};
    muxc::StackPanel editorHost{nullptr};
    muxc::Button restoreDefault{nullptr};
    muxc::TextBlock validation{nullptr};
    muxc::TextBlock diagnostic{nullptr};

    muxc::TextBox text{nullptr};
    muxc::PasswordBox password{nullptr};
    muxc::Primitives::ToggleButton passwordReveal{nullptr};
    muxc::TextCommandBarFlyout passwordFlyout{nullptr};
    muxc::ToggleSwitch toggle{nullptr};
    muxc::Grid numericEditors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    std::unique_ptr<presenter_controls::ColorFlyoutEditor> colorEditor;
    muxc::ComboBox select{nullptr};
    muxc::ListView multiSelect{nullptr};
    std::vector<MultiSelectOptionControl> multiOptions;
    muxc::CalendarDatePicker date{nullptr};
    muxc::TimePicker time{nullptr};
    std::array<muxc::NumberBox, 3> durationParts{nullptr, nullptr, nullptr};
    std::array<muxc::TextBox, 3> durationInputs{nullptr, nullptr, nullptr};
    std::array<winrt::event_token, 3> durationTextTokens{};
    std::array<winrt::event_token, 3> durationLoadedTokens{};
    std::array<winrt::event_token, 3> durationKeyTokens{};
    std::array<winrt::hstring, 3> durationObservedText;
    bool durationDirty = false;
    muxc::AutoSuggestBox search{nullptr};
    muxc::ProgressRing searchProgress{nullptr};
    muxc::TextBlock searchError{nullptr};
    muxc::ListView searchResults{nullptr};
    std::vector<wr::WidgetSettingSearchResult> resultValues;
    muxc::TextBox referenceText{nullptr};
    muxc::TextBlock referenceStatus{nullptr};
    muxc::StackPanel opaqueActions{nullptr};
    muxc::Button choose{nullptr};
    muxc::Button clear{nullptr};
    std::optional<wr::WidgetSettingOpaqueState> referenceState;
    std::string temporalValue;

    winrt::event_token textChanged{};
    winrt::event_token passwordChanged{};
    winrt::event_token passwordRevealClicked{};
    winrt::event_token passwordRevealLostFocus{};
    winrt::event_token passwordFlyoutClosed{};
    winrt::event_token lostFocus{};
    winrt::event_token keyDown{};
    winrt::event_token toggled{};
    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderPointerReleased{};
    winrt::event_token numberPointerReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
    winrt::event_token selectionChanged{};
    winrt::event_token dateChanged{};
    winrt::event_token timeChanged{};
    winrt::event_token searchTextChanged{};
    winrt::event_token searchSubmitted{};
    winrt::event_token searchKeyDown{};
    winrt::event_token resultKeyDown{};
    winrt::event_token searchResultClicked{};
    winrt::event_token chooseClicked{};
    winrt::event_token clearClicked{};
    winrt::event_token restoreDefaultClicked{};
    winrt::event_token idleCommitTick{};

    bool visible = true;
    bool enabled = true;
    bool textDirty = false;
    winrt::hstring textObservedValue;
    bool passwordDirty = false;
    bool passwordCanClear = false;
    bool passwordConfigured = false;
    bool passwordLoaded = false;
    winrt::hstring passwordObservedValue;
    std::optional<std::uint32_t> passwordLength;
    winrt::event_token passwordGettingFocus{};
    winrt::event_token passwordRevealUnchecked{};
    winrt::event_token passwordPointerPressed{};
    winrt::event_token passwordFlyoutOpening{};
    bool continuousDirty = false;
    bool synchronizing = false;
    std::uint64_t searchRequestId = 0;
    mux::DispatcherTimer idleCommitTimer{nullptr};
    std::string draftError;
};

struct WidgetGroupControl
{
    wr::WidgetSettingGroupSchema schema;
    muxc::Border root{nullptr};
    muxc::Expander expander{nullptr};
    muxc::StackPanel content{nullptr};
};

struct AppearanceScalarControl
{
    presenter_controls::SettingRow row;
    muxc::Grid editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::TextBlock unit{nullptr};
    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderPointerReleased{};
    winrt::event_token numberPointerReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
    winrt::event_token idleCommitTick{};
    bool synchronizing = false;
    bool continuousDirty = false;
    mux::DispatcherTimer idleCommitTimer{nullptr};
    std::string previewOwner;
    double minimum = 0.0;
    double maximum = 1.0;
    double scale = 1.0;
    std::function<void(wr::WidgetHostAppearancePatch&, float)> assign;
};

struct PendingTransientPreview
{
    std::uint64_t requestId = 0;
    std::optional<wr::InteractionValue> ordinary;
    wr::WidgetHostAppearancePatch appearance;
};

struct TransientPreviewHint
{
    std::string owner;
    std::uint64_t requestId = 0;
};

enum class AppearanceThemeKind
{
    Saved,
    BuiltIn,
    Custom,
    Component,
};

struct AppearanceThemeChoice
{
    AppearanceThemeKind kind = AppearanceThemeKind::Custom;
    std::string id;
    int builtInPresetId = kAppearancePresetCustom;
    std::size_t componentPresetIndex = 0;
};

struct WidgetSettingsDispatchBridge
{
    std::recursive_mutex mutex;
    mud::DispatcherQueue dispatcher{nullptr};
    bool closed = false;
    std::uint64_t epoch = 1;
    std::function<void(wr::WidgetSettingsSnapshotChanged)> snapshot;
    std::function<void(wr::WidgetSettingSearchCompleted)> search;
    std::function<void(TransientPreviewHint)> transientPreview;
};

template <typename Hint, typename HandlerSelector>
void QueueServiceHint(
    const std::shared_ptr<WidgetSettingsDispatchBridge>& bridge,
    Hint hint,
    HandlerSelector selectHandler) noexcept
{
    try
    {
        mud::DispatcherQueue dispatcher{nullptr};
        std::uint64_t epoch = 0;
        {
            std::lock_guard lock(bridge->mutex);
            if (bridge->closed || !bridge->dispatcher)
                return;
            dispatcher = bridge->dispatcher;
            epoch = bridge->epoch;
        }
        (void)dispatcher.TryEnqueue(
            [bridge, epoch, hint = std::move(hint),
             selectHandler = std::move(selectHandler)]() mutable {
                std::lock_guard lock(bridge->mutex);
                if (bridge->closed || bridge->epoch != epoch)
                    return;
                auto& handler = selectHandler(*bridge);
                if (handler) handler(std::move(hint));
            });
    }
    catch (...)
    {
        // Service completion threads must never observe a UI dispatch failure.
    }
}

} // namespace

struct WidgetSettingsPresenter::Impl
{
    Impl(wr::IWidgetSettingsService& settingsService,
        LocalizeCallback callback,
        const mux::Style& style)
        : service(settingsService), localize(std::move(callback)),
          cardStyle(style),
          dispatch(std::make_shared<WidgetSettingsDispatchBridge>())
    {
        dispatch->dispatcher = mud::DispatcherQueue::GetForCurrentThread();
        dispatch->snapshot = [this](wr::WidgetSettingsSnapshotChanged hint) {
            ApplySnapshotHint(std::move(hint));
        };
        dispatch->search = [this](wr::WidgetSettingSearchCompleted hint) {
            ApplySearchHint(std::move(hint));
        };
        dispatch->transientPreview = [this](TransientPreviewHint hint) {
            PublishQueuedPreview(std::move(hint));
        };
        BuildStaticControls();
        HookStaticEvents();
        RefreshLocalizedText();
    }

    wr::IWidgetSettingsService& service;
    LocalizeCallback localize;
    WidgetSettingsPresenterCallbacks callbacks;
    mux::Style cardStyle{nullptr};
    std::shared_ptr<WidgetSettingsDispatchBridge> dispatch;

    muxc::StackPanel root{nullptr};
    SettingsCard appearanceCard;
    muxc::TextBlock appearanceTitle{nullptr};
    presenter_controls::SettingRow followGlobalRow;
    muxc::ToggleSwitch followGlobal{nullptr};
    presenter_controls::SettingRow appearanceThemeRow;
    muxc::ComboBox appearanceTheme{nullptr};
    muxc::StackPanel customAppearanceHost{nullptr};
    std::unique_ptr<presenter_controls::ColorFlyoutEditor>
        backgroundColorEditor;
    AppearanceScalarControl backgroundOpacity;
    AppearanceSections appearanceSections;
    std::shared_ptr<PanelGradientEditor> panelGradientEditor;
    std::shared_ptr<EdgeLightEditor> edgeLightEditor;
    std::vector<std::pair<presenter_controls::SettingRow*, muxc::Button>> appearanceResets;
    presenter_controls::SettingRow restoreBackgroundRow;
    std::unique_ptr<presenter_controls::ColorFlyoutEditor>
        borderColorEditor;
    AppearanceScalarControl borderOpacity;
    AppearanceScalarControl borderWidth;
    presenter_controls::SettingRow edgeHighlightRow;
    muxc::ToggleSwitch edgeHighlightEnabled{nullptr};
    AppearanceScalarControl edgeHighlightWidth;
    AppearanceScalarControl edgeHighlightStrength;
    AppearanceScalarControl gradientEndOpacity;
    presenter_controls::SettingRow glassRow;
    muxc::ToggleSwitch glassEnabled{nullptr};
    presenter_controls::SettingRow acrylicRow;
    muxc::ToggleSwitch acrylicEnabled{nullptr};
    presenter_controls::SettingRow contentThemeRow;
    muxc::ComboBox contentTheme{nullptr};

    SettingsCard stylePreviewCard;
    muxc::TextBlock stylePreviewTitle{nullptr};
    presenter_controls::SettingRow presetRow;
    muxc::ComboBox presetCombo{nullptr};
    presenter_controls::SettingRow restoreScriptDefaultRow;
    muxc::Button restoreScriptDefault{nullptr};
    muxc::TextBlock scriptSettingsTitle{nullptr};
    muxc::StackPanel fieldsHost{nullptr};
    SettingsCard ungroupedCard;
    SettingsCard relatedCard;
    muxc::TextBlock relatedTitle;
    presenter_controls::SettingRow permissionsRow;
    muxc::Button permissionsButton;
    winrt::event_token permissionsClicked{};
    SettingsCard resetCard;
    presenter_controls::SettingRow resetRow;
    muxc::Button reset{nullptr};

    std::vector<std::unique_ptr<WidgetFieldControl>> fields;
    std::vector<WidgetGroupControl> groups;
    std::unordered_map<std::string, WidgetFieldControl*> fieldsByKey;
    // Only the condition currently preventing access contributes a parent.
    // This is navigation metadata from the accepted snapshot, not a value edit.
    struct FocusParent { std::string key; std::string messageKey; };
    std::unordered_map<std::string, FocusParent> focusParents;
    std::unordered_map<std::string, muxc::StackPanel> groupPanels;
    std::unordered_map<std::string, bool> rememberedGroupExpansion;
    std::unordered_map<std::string, PendingTransientPreview>
        pendingTransientPreviews;

    std::vector<wr::WidgetSettingFieldSchema> cachedSchemas;
    std::vector<wr::WidgetSettingGroupSchema> cachedGroups;
    std::vector<wr::WidgetSettingPresetSchema> cachedPresets;
    std::vector<AppearanceThemeChoice> appearanceThemeChoices;
    std::string defaultPresetId;
    wr::WidgetHostAppearanceState currentHostAppearance;
    std::wstring widgetId;
    std::string packageId;
    std::string widgetName;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t nextTransientPreviewRequestId = 0;
    std::string transientPreviewOwner;
    bool transientPreviewActive = false;
    bool cachedCustomStyle = false;
    bool hasSnapshot = false;
    bool active = false;
    bool updatingControls = false;
    bool closed = false;

    winrt::event_token followGlobalToggled{};
    winrt::event_token appearanceThemeChanged{};
    winrt::event_token edgeHighlightToggled{};
    winrt::event_token glassToggled{};
    winrt::event_token acrylicToggled{};
    winrt::event_token contentThemeChanged{};
    winrt::event_token presetChanged{};
    winrt::event_token restoreScriptDefaultClicked{};
    winrt::event_token resetClicked{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback) const
    {
        if (localize)
        {
            std::wstring translated = localize(key);
            if (!translated.empty()) return translated;
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] wr::WidgetSettingMutationGuard Guard() const
    {
        return {widgetId, generation, revision};
    }

    [[nodiscard]] bool CanMutate() const noexcept
    {
        return !closed && active && hasSnapshot && !updatingControls &&
            !widgetId.empty() && generation != 0 && revision != 0;
    }

    void InitializeCard(SettingsCard& card)
    {
        card.root = muxc::Border{};
        if (cardStyle) card.root.Style(cardStyle);
        card.content = muxc::StackPanel{};
        card.content.Spacing(10.0);
        card.root.Child(card.content);
    }

    muxc::TextBlock MakeSectionTitle()
    {
        muxc::TextBlock title{};
        title.FontSize(18.0);
        title.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(mux::TextWrapping::Wrap);
        muxa::AutomationProperties::SetHeadingLevel(title,
            muxa::Peers::AutomationHeadingLevel::Level2);
        return title;
    }

    void InitializeAppearanceScalar(AppearanceScalarControl& control,
        double minimum = 0.0, double maximum = 1.0,
        double step = 0.01, double scale = 1.0,
        std::wstring unit = {})
    {
        control.editors = muxc::Grid{};
        control.editors.ColumnSpacing(8.0);
        control.editors.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        muxc::ColumnDefinition sliderColumn{};
        sliderColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition numberColumn{};
        numberColumn.Width(mux::GridLengthHelper::Auto());
        muxc::ColumnDefinition unitColumn{};
        unitColumn.Width(mux::GridLengthHelper::Auto());
        control.editors.ColumnDefinitions().Append(sliderColumn);
        control.editors.ColumnDefinitions().Append(numberColumn);
        control.editors.ColumnDefinitions().Append(unitColumn);
        control.slider = muxc::Slider{};
        control.slider.Minimum(minimum);
        control.slider.Maximum(maximum);
        control.slider.StepFrequency(step);
        control.slider.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        control.number = muxc::NumberBox{};
        control.number.Minimum(minimum);
        control.number.Maximum(maximum);
        control.number.SmallChange(step);
        control.number.LargeChange(step * 5.0);
        winrt::Windows::Globalization::NumberFormatting::IncrementNumberRounder rounder;
        rounder.Increment(step);
        winrt::Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
        formatter.FractionDigits(step < 1.0 ? 2 : 0);
        formatter.NumberRounder(rounder);
        control.number.NumberFormatter(formatter);
        control.number.Width(presenter_controls::kSettingNumberWidth);
        control.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        control.number.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        control.editors.Children().Append(control.slider);
        muxc::Grid::SetColumn(control.number, 1);
        control.editors.Children().Append(control.number);
        control.unit = muxc::TextBlock{};
        control.unit.Text(std::move(unit));
        control.unit.VerticalAlignment(mux::VerticalAlignment::Center);
        control.unit.Opacity(0.72);
        control.unit.Visibility(control.unit.Text().empty()
            ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        muxc::Grid::SetColumn(control.unit, 2);
        control.editors.Children().Append(control.unit);
        control.minimum = minimum;
        control.maximum = maximum;
        control.scale = scale;
        control.row.Initialize(control.editors);
    }

    void RunAppearancePatch(wr::WidgetHostAppearancePatch patch)
    {
        if (!patch.presetId && !patch.followPersonalization) patch.presetId = "__custom";
        RunMutation("__appearance",
            [this, patch = std::move(patch)](const auto& guard) {
                return service.UpdateHostAppearance(guard, patch);
            });
    }

    void HookAppearanceScalar(
        AppearanceScalarControl& control,
        std::string previewOwner,
        std::function<void(wr::WidgetHostAppearancePatch&, float)> assign)
    {
        control.previewOwner = std::move(previewOwner);
        control.assign = std::move(assign);
        control.idleCommitTimer = mux::DispatcherTimer{};
        control.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        control.idleCommitTick = control.idleCommitTimer.Tick(
            [this, &control](const auto&, const auto&) {
                control.idleCommitTimer.Stop();
                (void)CommitAppearanceScalar(control);
            });
        control.sliderChanged = control.slider.ValueChanged(
            [this, &control](const auto&, const auto&) {
                if (!CanMutate() || control.synchronizing) return;
                const float value = static_cast<float>(std::clamp(
                    control.slider.Value(), control.minimum,
                    control.maximum) * control.scale);
                control.synchronizing = true;
                presenter_controls::SetNumberBoxValue(control.number, value / control.scale);
                control.synchronizing = false;
                QueueAppearancePreview(control, value);
            });
        control.numberChanged = control.number.ValueChanged(
            [this, &control](const auto&, const auto&) {
                if (!CanMutate() || control.synchronizing ||
                    std::isnan(control.number.Value()))
                    return;
                const float value = static_cast<float>(std::clamp(
                    control.number.Value(), control.minimum,
                    control.maximum) * control.scale);
                control.synchronizing = true;
                control.slider.Value(value / control.scale);
                control.synchronizing = false;
                QueueAppearancePreview(control, value);
            });
        const auto released = [this, &control](const auto&, const auto&) {
            (void)CommitAppearanceScalar(control);
        };
        const auto lost = [this, &control](const auto&, const auto&) {
            (void)CommitAppearanceScalar(control);
        };
        const auto key = [this, &control](
            const auto&, const muxi::KeyRoutedEventArgs& args) {
            if (IsEnter(args)) (void)CommitAppearanceScalar(control);
        };
        control.sliderPointerReleased = control.slider.PointerReleased(released);
        control.numberPointerReleased = control.number.PointerReleased(released);
        control.sliderLostFocus = control.slider.LostFocus(lost);
        control.numberLostFocus = control.number.LostFocus(lost);
        control.sliderKeyDown = control.slider.KeyDown(key);
        control.numberKeyDown = control.number.KeyDown(key);
    }

    void BuildStaticControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        // Instance content comes first; authored field/group order is retained
        // by RebuildFields. Appearance remains a separate conditional section.
        scriptSettingsTitle = MakeSectionTitle();
        scriptSettingsTitle.Margin({4.0, 8.0, 4.0, 0.0});
        root.Children().Append(scriptSettingsTitle);
        fieldsHost = muxc::StackPanel{};
        fieldsHost.Spacing(8.0);
        root.Children().Append(fieldsHost);

        InitializeCard(appearanceCard);
        appearanceCard.root.Style(nullptr);
        appearanceCard.root.Padding({0, 0, 0, 0});
        appearanceCard.root.BorderThickness({0, 0, 0, 0});
        appearanceCard.root.Background(nullptr);
        appearanceTitle = MakeSectionTitle();
        appearanceTitle.Margin({4.0, 8.0, 4.0, 0.0});
        root.Children().Append(appearanceTitle);

        followGlobal = muxc::ToggleSwitch{};
        followGlobal.HorizontalAlignment(mux::HorizontalAlignment::Right);
        followGlobalRow.Initialize(followGlobal);
        presenter_controls::StyleSettingsCard(followGlobalRow.root);
        followGlobalRow.controlHost.MinHeight(40.0);
        muxa::AutomationProperties::SetAutomationId(followGlobalRow.root, L"WidgetFollowGlobalRow");
        followGlobalRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        appearanceCard.content.Children().Append(followGlobalRow.root);

        appearanceTheme = muxc::ComboBox{};
        appearanceTheme.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        appearanceTheme.MaxWidth(520.0);
        appearanceThemeRow.Initialize(appearanceTheme);
        presenter_controls::StyleSettingsCard(appearanceThemeRow.root);
        appearanceThemeRow.controlHost.MinHeight(40.0);
        muxa::AutomationProperties::SetAutomationId(appearanceThemeRow.root, L"WidgetAppearanceThemeRow");
        appearanceCard.content.Children().Append(appearanceThemeRow.root);

        muxc::Button restoreBackground; restoreBackground.HorizontalAlignment(mux::HorizontalAlignment::Right);
        restoreBackground.Content(winrt::box_value(L("app.settings.restore_default", L"Restore default")));
        restoreBackground.Click([this](auto const&, auto const&) {
            if (!CanMutate()) return;
            wr::WidgetHostAppearanceState defaults; wr::WidgetHostAppearancePatch patch;
            patch.backgroundColor = defaults.backgroundColor; patch.borderColor = defaults.borderColor;
            patch.backgroundOpacity = defaults.backgroundOpacity; patch.borderOpacity = defaults.borderOpacity;
            patch.borderWidth = defaults.borderWidth; patch.edgeHighlightEnabled = defaults.edgeHighlightEnabled;
            patch.edgeHighlightWidth = defaults.edgeHighlightWidth; patch.edgeHighlightStrength = defaults.edgeHighlightStrength;
            patch.edgeLight = defaults.edgeLight; patch.gradientEndOpacity = defaults.gradientEndOpacity;
            patch.glassEnabled = defaults.glassEnabled; patch.acrylicEnabled = defaults.acrylicEnabled;
            patch.contentTheme = defaults.contentTheme; patch.panelGradient = defaults.panelGradient;
            RunAppearancePatch(std::move(patch));
        });
        restoreBackgroundRow.Initialize(restoreBackground);
        presenter_controls::StyleSettingsCard(restoreBackgroundRow.root);
        restoreBackgroundRow.controlHost.MinHeight(40.0);
        muxa::AutomationProperties::SetAutomationId(restoreBackgroundRow.root, L"WidgetRestoreBackgroundRow");
        appearanceCard.content.Children().Append(restoreBackgroundRow.root);
        customAppearanceHost = muxc::StackPanel{};
        customAppearanceHost.Spacing(10.0);
        appearanceCard.content.Children().Append(customAppearanceHost);
        appearanceSections.Initialize(customAppearanceHost, true, true);

        backgroundColorEditor =
            std::make_unique<presenter_controls::ColorFlyoutEditor>();
        backgroundColorEditor->Initialize(
            [this](const wui::Color& color, SettingsUpdateMode mode) {
                if (!CanMutate()) return;
                constexpr std::string_view owner =
                    "__appearance.backgroundColor";
                wr::WidgetHostAppearancePatch patch;
                patch.backgroundColor = static_cast<int>(FromColor(color));
                QueueTransientAppearance(owner, std::move(patch));
                if (mode == SettingsUpdateMode::PreviewAndCommit)
                    (void)CommitTransientOwner(owner);
            });
        appearanceSections.colors.Children().Append(backgroundColorEditor->row.root);

        InitializeAppearanceScalar(backgroundOpacity);
        appearanceSections.colors.Children().Append(backgroundOpacity.row.root);
        panelGradientEditor = PanelGradientEditor::Create(
            [this](std::string_view key) { return L(key, {}); },
            [this](const PanelGradient& gradient, bool commit) {
                if (!CanMutate()) return;
                constexpr std::string_view owner = "__appearance.panelGradient";
                wr::WidgetHostAppearancePatch patch;
                patch.panelGradient = gradient;
                QueueTransientAppearance(owner, std::move(patch));
                if (commit) (void)CommitTransientOwner(owner);
                UpdateBackgroundControls(gradient.enabled);
            }, false, {}, true);
        appearanceSections.colors.Children().InsertAt(0, panelGradientEditor->Content());

        borderColorEditor =
            std::make_unique<presenter_controls::ColorFlyoutEditor>();
        borderColorEditor->Initialize(
            [this](const wui::Color& color, SettingsUpdateMode mode) {
                if (!CanMutate()) return;
                constexpr std::string_view owner =
                    "__appearance.borderColor";
                wr::WidgetHostAppearancePatch patch;
                patch.borderColor = static_cast<int>(FromColor(color));
                QueueTransientAppearance(owner, std::move(patch));
                if (mode == SettingsUpdateMode::PreviewAndCommit)
                    (void)CommitTransientOwner(owner);
            });
        appearanceSections.border.Children().Append(borderColorEditor->row.root);

        InitializeAppearanceScalar(borderOpacity);
        appearanceSections.border.Children().Append(borderOpacity.row.root);
        InitializeAppearanceScalar(borderWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.05, 1.0, L"px");
        appearanceSections.border.Children().Append(borderWidth.row.root);

        edgeHighlightEnabled = muxc::ToggleSwitch{};
        edgeHighlightEnabled.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        edgeHighlightRow.Initialize(edgeHighlightEnabled);
        edgeHighlightRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        appearanceSections.border.Children().Append(edgeHighlightRow.root);

        InitializeAppearanceScalar(edgeHighlightWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.05, 1.0, L"px");
        appearanceSections.border.Children().Append(edgeHighlightWidth.row.root);
        InitializeAppearanceScalar(edgeHighlightStrength,
            0.0, 100.0, 1.0, 0.01, L"%");
        appearanceSections.border.Children().Append(edgeHighlightStrength.row.root);
        edgeLightEditor = EdgeLightEditor::Create([this](auto key) { return L(key, L""); }, [this](auto const& light, bool commit) {
            if (!CanMutate()) return;
            wr::WidgetHostAppearancePatch patch; patch.edgeLight = light;
            constexpr std::string_view owner = "__appearance.edgeLight";
            QueueTransientAppearance(owner, std::move(patch));
            if (commit) (void)CommitTransientOwner(owner);
        });
        appearanceSections.border.Children().Append(edgeLightEditor->Content());
        InitializeAppearanceScalar(gradientEndOpacity);
        appearanceSections.bottomBar.Children().Append(gradientEndOpacity.row.root);

        glassEnabled = muxc::ToggleSwitch{};
        glassEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        glassRow.Initialize(glassEnabled);
        glassRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        appearanceSections.material.Children().Append(glassRow.root);

        acrylicEnabled = muxc::ToggleSwitch{};
        acrylicEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        acrylicRow.Initialize(acrylicEnabled);
        acrylicRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        appearanceSections.material.Children().Append(acrylicRow.root);

        contentTheme = muxc::ComboBox{};
        contentTheme.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        contentTheme.MaxWidth(520.0);
        contentThemeRow.Initialize(contentTheme);
        appearanceSections.text.Children().Append(contentThemeRow.root);
        contentThemeRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        contentTheme.HorizontalAlignment(mux::HorizontalAlignment::Right);
        const auto addAppearanceRestore = [this](presenter_controls::SettingRow& row, auto action) {
            appearanceResets.emplace_back(&row, presenter_controls::AddRestoreDefaultAction(row,
                L("app.settings.restore_default", L"Restore default"), [this, action] {
                    if (!CanMutate()) return;
                    wr::WidgetHostAppearancePatch patch; action(patch, wr::WidgetHostAppearanceState{});
                    RunAppearancePatch(std::move(patch));
                }));
        };
        addAppearanceRestore(backgroundColorEditor->row, [](auto& patch, auto const& value) { patch.backgroundColor = value.backgroundColor; });
        addAppearanceRestore(borderColorEditor->row, [](auto& patch, auto const& value) { patch.borderColor = value.borderColor; });
        addAppearanceRestore(backgroundOpacity.row, [](auto& patch, auto const& value) { patch.backgroundOpacity = value.backgroundOpacity; });
        addAppearanceRestore(borderOpacity.row, [](auto& patch, auto const& value) { patch.borderOpacity = value.borderOpacity; });
        addAppearanceRestore(borderWidth.row, [](auto& patch, auto const& value) { patch.borderWidth = value.borderWidth; });
        addAppearanceRestore(edgeHighlightRow, [](auto& patch, auto const& value) { patch.edgeHighlightEnabled = value.edgeHighlightEnabled; });
        addAppearanceRestore(edgeHighlightWidth.row, [](auto& patch, auto const& value) { patch.edgeHighlightWidth = value.edgeHighlightWidth; });
        addAppearanceRestore(edgeHighlightStrength.row, [](auto& patch, auto const& value) { patch.edgeHighlightStrength = value.edgeHighlightStrength; });
        addAppearanceRestore(gradientEndOpacity.row, [](auto& patch, auto const& value) { patch.gradientEndOpacity = value.gradientEndOpacity; });
        addAppearanceRestore(glassRow, [](auto& patch, auto const& value) { patch.glassEnabled = value.glassEnabled; });
        addAppearanceRestore(acrylicRow, [](auto& patch, auto const& value) { patch.acrylicEnabled = value.acrylicEnabled; });
        addAppearanceRestore(contentThemeRow, [](auto& patch, auto const& value) { patch.contentTheme = value.contentTheme; });
        root.Children().Append(appearanceCard.root);

        InitializeCard(stylePreviewCard);
        stylePreviewCard.root.Style(nullptr);
        stylePreviewCard.root.Child(nullptr);
        stylePreviewTitle = MakeSectionTitle();
        (void)presenter_controls::InitializeSettingsGroup(
            stylePreviewCard.root, stylePreviewTitle, stylePreviewCard.content, cardStyle);
        stylePreviewCard.content.Spacing(0.0);
        presetCombo = muxc::ComboBox{};
        presetCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presetCombo.MaxWidth(520.0);
        presetRow.Initialize(presetCombo);
        stylePreviewCard.content.Children().Append(presetRow.root);
        restoreScriptDefault = muxc::Button{};
        restoreScriptDefault.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        restoreScriptDefault.VerticalAlignment(
            mux::VerticalAlignment::Center);
        restoreScriptDefault.VerticalContentAlignment(
            mux::VerticalAlignment::Center);
        restoreScriptDefault.HorizontalContentAlignment(
            mux::HorizontalAlignment::Center);
        restoreScriptDefault.MinHeight(32.0);
        restoreScriptDefaultRow.Initialize(restoreScriptDefault);
        restoreScriptDefaultRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        stylePreviewCard.content.Children().Append(
            restoreScriptDefaultRow.root);
        presenter_controls::UpdateSettingsSubrows(stylePreviewCard.content, 0.0);
        root.Children().Append(stylePreviewCard.root);

        InitializeCard(relatedCard);
        relatedCard.root.Style(nullptr);
        relatedCard.root.Child(nullptr);
        relatedTitle.TextWrapping(mux::TextWrapping::Wrap);
        relatedTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        (void)presenter_controls::InitializeSettingsGroup(
            relatedCard.root, relatedTitle, relatedCard.content, cardStyle);
        relatedCard.content.Spacing(0.0);
        permissionsClicked = permissionsButton.Click([this](const auto&, const auto&) {
            if (!CanMutate() || packageId.empty() || !callbacks.openPackagePermissions)
                return;
            const auto result = FlushPendingEdits();
            if (!result.Succeeded()) return;
            callbacks.openPackagePermissions(ToWide(packageId));
        });
        permissionsRow.Initialize(permissionsButton);
        presenter_controls::StyleSettingsSubrow(permissionsRow.root, true, 0.0);
        permissionsRow.root.MinHeight(68.0);
        muxa::AutomationProperties::SetAutomationId(permissionsRow.root, L"WidgetPermissionsRow");
        permissionsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        relatedCard.content.Children().Append(permissionsRow.root);
        relatedCard.root.Visibility(mux::Visibility::Collapsed);
        root.Children().Append(relatedCard.root);

        InitializeCard(resetCard);
        reset = muxc::Button{};
        reset.HorizontalAlignment(mux::HorizontalAlignment::Right);
        reset.VerticalAlignment(mux::VerticalAlignment::Center);
        reset.VerticalContentAlignment(mux::VerticalAlignment::Center);
        reset.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
        reset.MinHeight(32.0);
        resetRow.Initialize(reset);
        resetCard.root.Padding({0.0});
        presenter_controls::StyleSettingsSubrow(resetRow.root, true, 0.0);
        resetRow.root.MinHeight(68.0);
        muxa::AutomationProperties::SetAutomationId(resetRow.root, L"WidgetResetRow");
        resetRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        resetCard.content.Children().Append(resetRow.root);
        root.Children().Append(resetCard.root);
        const auto equalizeActionRows = [permissions = winrt::make_weak(permissionsRow.root),
            restore = winrt::make_weak(resetRow.root), permissionText = winrt::make_weak(permissionsRow.text),
            restoreText = winrt::make_weak(resetRow.text), permissionControl = winrt::make_weak(permissionsRow.controlHost),
            restoreControl = winrt::make_weak(resetRow.controlHost)](const auto&, const auto&) {
            const auto first = permissions.get(); const auto second = restore.get();
            const auto firstText = permissionText.get(); const auto secondText = restoreText.get();
            const auto firstControl = permissionControl.get(); const auto secondControl = restoreControl.get();
            if (!first || !second || !firstText || !secondText || !firstControl || !secondControl) return;
            const auto firstPadding = first.Padding(); const auto secondPadding = second.Padding();
            const double height = std::ceil(std::max({68.0,
                std::max(firstText.ActualHeight(), firstControl.ActualHeight()) + firstPadding.Top + firstPadding.Bottom,
                std::max(secondText.ActualHeight(), secondControl.ActualHeight()) + secondPadding.Top + secondPadding.Bottom}));
            if (first.MinHeight() != height) first.MinHeight(height);
            if (second.MinHeight() != height) second.MinHeight(height);
        };
        permissionsRow.root.SizeChanged(equalizeActionRows);
        resetRow.root.SizeChanged(equalizeActionRows);

        appearanceCard.root.Visibility(mux::Visibility::Collapsed);
        appearanceTitle.Visibility(mux::Visibility::Collapsed);
        stylePreviewCard.root.Visibility(mux::Visibility::Collapsed);
        scriptSettingsTitle.Visibility(mux::Visibility::Collapsed);
        resetCard.root.Visibility(mux::Visibility::Collapsed);
    }

    void HookStaticEvents()
    {
        followGlobalToggled = followGlobal.Toggled(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                wr::WidgetHostAppearancePatch patch;
                patch.followPersonalization = followGlobal.IsOn();
                RunAppearancePatch(std::move(patch));
            });
        appearanceThemeChanged = appearanceTheme.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                const int index = appearanceTheme.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >=
                        appearanceThemeChoices.size())
                    return;
                const auto choice = appearanceThemeChoices[
                    static_cast<std::size_t>(index)];
                CommitOpenColorEditors();
                if (!FlushPendingEdits().Succeeded()) return;
                if (choice.kind == AppearanceThemeKind::Saved)
                {
                    const std::string target = "widget/" + winrt::to_string(widgetId);
                    RunMutation("__appearance", [this, target, id = choice.id](const auto& guard) {
                        const auto previous = currentHostAppearance;
                        themes::Library library; std::string error;
                        auto mutation = UnchangedResult();
                        auto restored = UnchangedResult();
                        const auto selected = themes::detail::SelectWidgetTheme(themes::LibraryPath(), target, id,
                            [&](const auto& patch) {
                                mutation = service.UpdateHostAppearance(guard, patch);
                                return themes::detail::WidgetThemeMutation{mutation.Succeeded(), mutation.Changed()};
                            }, [&] {
                                restored = service.UpdateHostAppearance(
                                    wr::WidgetSettingMutationGuard{widgetId, mutation.generation, mutation.revision},
                                    themes::detail::WidgetAppearanceSnapshotPatch(previous));
                                return restored.Succeeded();
                            }, library, error);
                        if (selected.committed) return mutation;
                        // Refresh only after the transaction/rollback completes.
                        // RunMutation's normal success refresh likewise sees the
                        // new committed reference, never the old one mid-write.
                        if (const auto current = service.Snapshot(widgetId)) (void)ApplySnapshot(*current);
                        if (!selected.mutationSucceeded && error == "widgetApplyFailed") return mutation;
                        const bool rollbackFailed = selected.rollbackAttempted && !selected.rollbackSucceeded;
                        const auto final = selected.rollbackAttempted ? restored : mutation;
                        return wr::WidgetSettingMutationResult{wr::WidgetSettingMutationStatus::PersistenceFailed,
                            final.generation, final.revision,
                            rollbackFailed ? "themeSelectionRollbackFailed" : error,
                            winrt::to_string(L(rollbackFailed ? "themeLibrary.error.widgetRollbackFailed" : themes::ErrorLocalizationKey(error), L""))};
                    });
                    return;
                }
                if (choice.kind == AppearanceThemeKind::Component)
                {
                    if (choice.componentPresetIndex >= cachedPresets.size())
                        return;
                    const std::string id = cachedPresets[
                        choice.componentPresetIndex].id;
                    RunMutation("__preset",
                        [this, id](const auto& guard) {
                            return service.ApplyPreset(guard, id);
                        });
                    return;
                }
                wr::WidgetHostAppearancePatch patch;
                patch.presetId = choice.id;
                if (choice.kind == AppearanceThemeKind::BuiltIn)
                {
                    const auto preset = MakeAppearancePreset(
                        choice.builtInPresetId);
                    const auto rgb = [](float red, float green, float blue) {
                        const auto channel = [](float value) {
                            return std::clamp(static_cast<int>(
                                std::lround(value * 255.0f)), 0, 255);
                        };
                        return (channel(red) << 16) |
                            (channel(green) << 8) | channel(blue);
                    };
                    patch.backgroundColor = rgb(preset.widgetBgR,
                        preset.widgetBgG, preset.widgetBgB);
                    patch.borderColor = rgb(preset.widgetBorderR,
                        preset.widgetBorderG, preset.widgetBorderB);
                    patch.backgroundOpacity = preset.widgetAlpha;
                    patch.borderOpacity = preset.widgetBorderAlpha;
                    patch.borderWidth = preset.widgetBorderWidth;
                    patch.edgeHighlightEnabled =
                        preset.widgetEdgeHighlightEnabled;
                    patch.edgeHighlightWidth =
                        preset.widgetEdgeHighlightWidth;
                    patch.edgeHighlightStrength =
                        preset.widgetEdgeHighlightStrength;
                    patch.edgeLight = preset.edgeLight;
                    patch.gradientEndOpacity = preset.gradientEndA;
                    patch.panelGradient = preset.panelGradient;
                    patch.glassEnabled = preset.glassEnabled;
                    patch.acrylicEnabled = preset.acrylicEnabled;
                    patch.contentTheme = preset.contentTheme;
                }
                else if (choice.kind == AppearanceThemeKind::Custom)
                {
                    // Enter custom editing from the effective current recipe,
                    // including a preset that changed since the saved snapshot.
                    patch.backgroundColor = currentHostAppearance.backgroundColor;
                    patch.borderColor = currentHostAppearance.borderColor;
                    patch.backgroundOpacity = currentHostAppearance.backgroundOpacity;
                    patch.borderOpacity = currentHostAppearance.borderOpacity;
                    patch.borderWidth = currentHostAppearance.borderWidth;
                    patch.edgeHighlightEnabled = currentHostAppearance.edgeHighlightEnabled;
                    patch.edgeHighlightWidth = currentHostAppearance.edgeHighlightWidth;
                    patch.edgeHighlightStrength = currentHostAppearance.edgeHighlightStrength;
                    patch.edgeLight = currentHostAppearance.edgeLight;
                    patch.gradientEndOpacity = currentHostAppearance.gradientEndOpacity;
                    patch.panelGradient = currentHostAppearance.panelGradient;
                    patch.glassEnabled = currentHostAppearance.glassEnabled;
                    patch.acrylicEnabled = currentHostAppearance.acrylicEnabled;
                    patch.contentTheme = currentHostAppearance.contentTheme;
                }
                RunAppearancePatch(std::move(patch));
            });
        HookAppearanceScalar(backgroundOpacity,
            "__appearance.backgroundOpacity",
            [](auto& patch, float value) {
                patch.backgroundOpacity = value;
            });
        HookAppearanceScalar(borderOpacity,
            "__appearance.borderOpacity",
            [](auto& patch, float value) {
                patch.borderOpacity = value;
            });
        HookAppearanceScalar(borderWidth,
            "__appearance.borderWidth",
            [](auto& patch, float value) {
                patch.borderWidth = value;
            });
        edgeHighlightToggled = edgeHighlightEnabled.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = edgeHighlightEnabled.IsOn();
                edgeHighlightWidth.row.root.Visibility(enabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
                edgeHighlightStrength.row.root.Visibility(enabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
                edgeHighlightWidth.row.SetEnabled(enabled);
                edgeHighlightStrength.row.SetEnabled(
                    enabled);
                if (!CanMutate()) return;
                wr::WidgetHostAppearancePatch patch;
                patch.edgeHighlightEnabled = enabled;
                RunAppearancePatch(std::move(patch));
            });
        HookAppearanceScalar(edgeHighlightWidth,
            "__appearance.edgeHighlightWidth",
            [](auto& patch, float value) {
                patch.edgeHighlightWidth = value;
            });
        HookAppearanceScalar(edgeHighlightStrength,
            "__appearance.edgeHighlightStrength",
            [](auto& patch, float value) {
                patch.edgeHighlightStrength = value;
            });
        HookAppearanceScalar(gradientEndOpacity,
            "__appearance.gradientEndOpacity",
            [](auto& patch, float value) {
                patch.gradientEndOpacity = value;
            });
        glassToggled = glassEnabled.Toggled(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                wr::WidgetHostAppearancePatch patch;
                patch.glassEnabled = glassEnabled.IsOn();
                if (!glassEnabled.IsOn() && acrylicEnabled.IsOn())
                    patch.acrylicEnabled = false;
                RunAppearancePatch(std::move(patch));
            });
        acrylicToggled = acrylicEnabled.Toggled(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                wr::WidgetHostAppearancePatch patch;
                patch.acrylicEnabled = acrylicEnabled.IsOn();
                if (acrylicEnabled.IsOn() && !glassEnabled.IsOn())
                    patch.glassEnabled = true;
                RunAppearancePatch(std::move(patch));
            });
        contentThemeChanged = contentTheme.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                const int selection = contentTheme.SelectedIndex();
                if (selection < 0 || selection > 1) return;
                wr::WidgetHostAppearancePatch patch;
                patch.contentTheme = selection;
                RunAppearancePatch(std::move(patch));
            });
        presetChanged = presetCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                const int index = presetCombo.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= cachedPresets.size())
                    return;
                const std::string id = cachedPresets[
                    static_cast<std::size_t>(index)].id;
                RunMutation("__preset", [this, id](const auto& guard) {
                    return service.ApplyPreset(guard, id);
                });
            });
        restoreScriptDefaultClicked = restoreScriptDefault.Click(
            [this](const auto&, const auto&) {
                if (!CanMutate() || defaultPresetId.empty()) return;
                const std::string id = defaultPresetId;
                RunMutation("__preset", [this, id](const auto& guard) {
                    return service.ApplyPreset(guard, id);
                });
            });
        resetClicked = reset.Click(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                RunMutation({}, [this](const auto& guard) {
                    return service.Reset(guard);
                });
            });
    }

    void CaptureGroupExpansion()
    {
        for (const auto& group : groups)
            if (group.expander)
                rememberedGroupExpansion.insert_or_assign(
                    group.schema.id, group.expander.IsExpanded());
    }

    void RebuildGroups(
        const wr::WidgetSettingsSnapshot& snapshot,
        bool preserveExpansion)
    {
        if (preserveExpansion)
            CaptureGroupExpansion();
        else
            rememberedGroupExpansion.clear();
        groups.clear();
        groupPanels.clear();

        for (const auto& schema : snapshot.groups)
        {
            WidgetGroupControl group;
            group.schema = schema;
            group.root = muxc::Border{};
            group.content = muxc::StackPanel{};
            group.content.Spacing(0.0);

            if (schema.collapsible)
            {
                group.expander = muxc::Expander{};
                group.expander.Padding({0, 0, 0, 0});
                group.expander.HorizontalAlignment(
                    mux::HorizontalAlignment::Stretch);
                group.expander.HorizontalContentAlignment(
                    mux::HorizontalAlignment::Stretch);
                muxc::StackPanel header;
                header.Spacing(2.0);
                muxc::TextBlock title;
                title.Text(ToText(schema.label));
                title.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
                title.TextWrapping(mux::TextWrapping::Wrap);
                muxa::AutomationProperties::SetHeadingLevel(title,
                    muxa::Peers::AutomationHeadingLevel::Level2);
                header.Children().Append(title);
                if (!schema.description.empty())
                {
                    muxc::TextBlock description;
                    description.Text(ToText(schema.description));
                    description.Opacity(0.72);
                    description.TextWrapping(mux::TextWrapping::Wrap);
                    header.Children().Append(description);
                }
                group.expander.Header(header);
                group.expander.Content(group.content);
                const auto remembered =
                    rememberedGroupExpansion.find(schema.id);
                group.expander.IsExpanded(
                    remembered == rememberedGroupExpansion.end()
                        ? schema.defaultExpanded
                        : remembered->second);
                group.root.Child(group.expander);
            }
            else
            {
                muxc::StackPanel container;
                container.Spacing(4.0);
                muxc::TextBlock title;
                title.Text(ToText(schema.label));
                title.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
                title.TextWrapping(mux::TextWrapping::Wrap);
                muxa::AutomationProperties::SetHeadingLevel(title,
                    muxa::Peers::AutomationHeadingLevel::Level2);
                container.Children().Append(title);
                if (!schema.description.empty())
                {
                    muxc::TextBlock description;
                    description.Text(ToText(schema.description));
                    description.Opacity(0.72);
                    description.TextWrapping(mux::TextWrapping::Wrap);
                    description.FontSize(12);
                    description.Margin({16, 12, 16, 8});
                    group.content.Children().Append(description);
                }
                presenter_controls::InitializeSettingsGroup(group.root, container, group.content, cardStyle);
            }
            muxa::AutomationProperties::SetName(
                group.root, ToText(schema.label));
            groupPanels.emplace(schema.id, group.content);
            groups.push_back(std::move(group));
        }
    }

    void InitializeFieldCard(WidgetFieldControl& field)
    {
        field.root = muxc::Border{};
        presenter_controls::StyleSettingsSubrow(field.root, false, 0);
        field.content = muxc::StackPanel{};
        field.content.Spacing(7.0);
        field.editorRow = muxc::Grid{};
        field.editorRow.ColumnSpacing(8.0);
        field.editorRow.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        const bool compactEditor =
            field.schema.Kind() == wr::WidgetSettingKind::Boolean ||
            field.schema.Kind() == wr::WidgetSettingKind::Color;
        muxc::ColumnDefinition editorColumn{};
        editorColumn.Width(compactEditor
                ? mux::GridLengthHelper::Auto()
                : mux::GridLengthHelper::FromValueAndType(
                      1.0, mux::GridUnitType::Star));
        field.editorRow.ColumnDefinitions().Append(editorColumn);
        field.editorHost = muxc::StackPanel{};
        field.editorHost.Spacing(7.0);
        field.editorHost.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        field.editorRow.Children().Append(field.editorHost);
        if (field.schema.Channel() ==
            wr::WidgetSettingValueChannel::Ordinary)
        {
            muxc::ColumnDefinition restoreColumn{};
            restoreColumn.Width(mux::GridLengthHelper::Auto());
            field.editorRow.ColumnDefinitions().Append(restoreColumn);
            field.restoreDefault = muxc::Button{};
            presenter_controls::ConfigureRestoreDefaultButton(
                field.restoreDefault,
                L("app.settings.restore_default", L"Restore Default"));
            muxc::Grid::SetColumn(field.restoreDefault, 1);
            field.editorRow.Children().Append(field.restoreDefault);
            field.restoreDefaultClicked = field.restoreDefault.Click(
                [this, &field](const auto&, const auto&) {
                    RestoreFieldDefault(field);
                });
        }
        // TimePicker's template needs 242 DIPs before the clear/reset actions.
        const double editorWidth = field.schema.Kind() == wr::WidgetSettingKind::Time
            ? 242.0 + 2 * (presenter_controls::kSettingResetWidth + presenter_controls::kSettingControlGap)
            : presenter_controls::kSettingControlWidth;
        field.row.Initialize(field.editorRow, editorWidth);
        field.row.SetCompactEditor(compactEditor);
        if (compactEditor)
            field.row.SetControlAlignment(mux::HorizontalAlignment::Right);
        field.row.SetText(
            ToWide(field.schema.label),
            ToWide(field.schema.description));
        field.content.Children().Append(field.row.root);
        field.root.Child(field.content);
        muxa::AutomationProperties::SetName(
            field.root, ToText(field.schema.label));
        muxa::AutomationProperties::SetAutomationId(
            field.root, ToText("WidgetSetting." + field.schema.key));
        if (!field.schema.description.empty())
            muxa::AutomationProperties::SetHelpText(
                field.root, ToText(field.schema.description));
    }

    void AppendValidation(WidgetFieldControl& field)
    {
        field.validation = muxc::TextBlock{};
        field.validation.TextWrapping(mux::TextWrapping::Wrap);
        field.validation.Opacity(0.86);
        field.validation.Visibility(mux::Visibility::Collapsed);
        field.content.Children().Append(field.validation);

        field.diagnostic = muxc::TextBlock{};
        field.diagnostic.TextWrapping(mux::TextWrapping::Wrap);
        field.diagnostic.Opacity(0.72);
        field.diagnostic.Visibility(mux::Visibility::Collapsed);
        field.content.Children().Append(field.diagnostic);
    }

    void BuildTextEditor(WidgetFieldControl& field)
    {
        field.text = muxc::TextBox{};
        field.text.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.text.MinWidth(0.0);
        field.idleCommitTimer = mux::DispatcherTimer{};
        field.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        field.idleCommitTick = field.idleCommitTimer.Tick(
            [this, &field](const auto&, const auto&) {
                field.idleCommitTimer.Stop();
                (void)CommitText(field);
            });
        field.textChanged = field.text.TextChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing) return;
                const auto value = field.text.Text();
                if (value == field.textObservedValue) return;
                field.textObservedValue = value;
                field.textDirty = true;
                if (field.restoreDefault) field.restoreDefault.IsEnabled(field.enabled);
                field.draftError.clear();
                RefreshFieldValidation(field);
                field.idleCommitTimer.Stop();
                field.idleCommitTimer.Start();
            });
        field.lostFocus = field.text.LostFocus(
            [this, &field](const auto&, const auto&) {
                (void)CommitText(field);
            });
        field.keyDown = field.text.KeyDown(
            [this, &field](const auto&,
                           const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args))
                {
                    (void)CommitText(field);
                    args.Handled(true);
                }
            });
        field.editorHost.Children().Append(field.text);
    }

    static bool PasswordEditorHasFocus(const WidgetFieldControl& field)
    {
        return field.password.FocusState() != mux::FocusState::Unfocused ||
            (field.passwordReveal &&
                field.passwordReveal.FocusState() != mux::FocusState::Unfocused);
    }

    void RefreshPasswordReveal(WidgetFieldControl& field)
    {
        const bool hasDraft = !field.password.Password().empty();
        field.passwordReveal.IsEnabled(field.password.IsEnabled() &&
            (hasDraft || field.passwordConfigured));
        if (field.clear)
            field.clear.IsEnabled(field.enabled &&
                (field.passwordCanClear || hasDraft));
        if (!hasDraft)
        {
            field.password.PasswordRevealMode(muxc::PasswordRevealMode::Hidden);
            field.passwordReveal.IsChecked(false);
        }
        const auto label = field.password.PasswordRevealMode() ==
                muxc::PasswordRevealMode::Visible
            ? L("app.settings.widget.hide_password", L"Hide password")
            : L("app.settings.widget.show_password", L"Show password");
        muxa::AutomationProperties::SetName(field.passwordReveal, label);
        muxc::ToolTipService::SetToolTip(
            field.passwordReveal, winrt::box_value(label));
    }

    void HidePasswordDraft(WidgetFieldControl& field)
    {
        field.password.PasswordRevealMode(muxc::PasswordRevealMode::Hidden);
        field.passwordReveal.IsChecked(false);
        RefreshPasswordReveal(field);
    }

    void ClearPasswordDraft(WidgetFieldControl& field)
    {
        field.passwordLoaded = false;
        field.passwordObservedValue = L"";
        field.synchronizing = true;
        field.password.Password(L"");
        field.synchronizing = false;
        HidePasswordDraft(field);
    }

    void RefreshPasswordPlaceholder(WidgetFieldControl& field)
    {
        const auto placeholder = field.passwordConfigured
            ? std::wstring(field.passwordLength.value_or(0), L'\x25cf')
            : L("app.settings.widget.not_configured", L"Not configured");
        field.password.PlaceholderText(placeholder);
        muxa::AutomationProperties::SetItemStatus(field.password,
            L(field.passwordConfigured ? "app.settings.widget.configured"
                : "app.settings.widget.not_configured",
                field.passwordConfigured ? L"Configured" : L"Not configured"));
    }

    bool LoadPasswordForEditing(WidgetFieldControl& field)
    {
        if (field.passwordLoaded || field.passwordDirty) return true;
        if (!CanMutateField(field)) return false;
        if (field.idleCommitTimer) field.idleCommitTimer.Stop();
        if (!field.passwordConfigured) { field.passwordLoaded = true; return true; }
        auto read = service.ReadSecret(Guard(), field.schema.key);
        if (!read.result.Succeeded())
        {
            SetDraftError(field, read.result);
            if (callbacks.mutationCompleted)
                callbacks.mutationCompleted(field.schema.key, read.result);
            return false;
        }
        std::wstring plaintext = ToWide(read.plaintext);
        read.ClearPlaintext();
        field.passwordLoaded = true;
        field.passwordLength = static_cast<std::uint32_t>(plaintext.size());
        field.passwordObservedValue = plaintext;
        field.synchronizing = true;
        field.password.Password(plaintext);
        field.synchronizing = false;
        if (!plaintext.empty()) SecureZeroMemory(plaintext.data(), plaintext.size() * sizeof(wchar_t));
        RefreshPasswordPlaceholder(field);
        RefreshPasswordReveal(field);
        return true;
    }

    void BuildPasswordEditor(WidgetFieldControl& field)
    {
        field.password = muxc::PasswordBox{};
        field.password.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        field.password.MinWidth(0.0);
        field.password.PasswordRevealMode(
            muxc::PasswordRevealMode::Hidden);
        // Reserve a separate native button column: long API keys scroll inside
        // the password control without hiding its reveal action.
        muxc::Grid passwordEditors{};
        passwordEditors.ColumnSpacing(8.0);
        muxc::ColumnDefinition passwordColumn{};
        passwordColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition revealColumn{};
        revealColumn.Width(mux::GridLengthHelper::Auto());
        passwordEditors.ColumnDefinitions().Append(passwordColumn);
        passwordEditors.ColumnDefinitions().Append(revealColumn);
        passwordEditors.Children().Append(field.password);
        field.passwordReveal = muxc::Primitives::ToggleButton{};
        field.passwordReveal.Width(32.0);
        field.passwordReveal.MinWidth(32.0);
        field.passwordReveal.Height(32.0);
        field.passwordReveal.Padding({0.0, 0.0, 0.0, 0.0});
        field.passwordReveal.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::SymbolIcon revealIcon{muxc::Symbol::View};
        field.passwordReveal.Content(revealIcon);
        field.passwordRevealClicked = field.passwordReveal.Checked(
            [this, &field](const auto&, const auto&) {
                if (!LoadPasswordForEditing(field))
                {
                    field.passwordReveal.IsChecked(false);
                    return;
                }
                (void)field.password.Focus(mux::FocusState::Programmatic);
                field.password.PasswordRevealMode(muxc::PasswordRevealMode::Visible);
                RefreshPasswordReveal(field);
            });
        field.passwordRevealUnchecked = field.passwordReveal.Unchecked(
            [this, &field](const auto&, const auto&) {
                field.password.PasswordRevealMode(muxc::PasswordRevealMode::Hidden);
                RefreshPasswordReveal(field);
            });
        muxc::Grid::SetColumn(field.passwordReveal, 1);
        passwordEditors.Children().Append(field.passwordReveal);
        // Give island-hosted password controls the native text-command menu.
        // WinUI owns clipboard handling and omits unsafe password commands.
        field.passwordFlyout = muxc::TextCommandBarFlyout{};
        field.password.ContextFlyout(field.passwordFlyout);
        field.passwordFlyoutOpening = field.passwordFlyout.Opening(
            [this, &field](const auto&, const auto&) {
                if (!LoadPasswordForEditing(field)) field.passwordFlyout.Hide();
            });
        field.idleCommitTimer = mux::DispatcherTimer{};
        field.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        field.idleCommitTick = field.idleCommitTimer.Tick(
            [this, &field](const auto&, const auto&) {
                field.idleCommitTimer.Stop();
                // Save while typing without finalizing the focused editor.
                // Clipboard commands finish before their draft is submitted.
                if (!field.passwordFlyout.IsOpen())
                    (void)CommitPassword(field);
            });
        field.passwordFlyoutClosed = field.passwordFlyout.Closed(
            [&field](const auto&, const auto&) {
                if (field.passwordDirty || !PasswordEditorHasFocus(field))
                    field.idleCommitTimer.Start();
            });
        field.passwordChanged = field.password.PasswordChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing) return;
                const auto value = field.password.Password();
                if (value == field.passwordObservedValue) return;
                field.passwordObservedValue = value;
                field.passwordLoaded = true;
                RefreshPasswordReveal(field);
                field.passwordDirty = true;
                field.draftError.clear();
                RefreshFieldValidation(field);
                field.idleCommitTimer.Stop();
                field.idleCommitTimer.Interval(std::chrono::milliseconds(650));
                field.idleCommitTimer.Start();
            });
        field.passwordGettingFocus = field.password.GettingFocus(
            [this, &field](const auto&, const auto& args) {
                // Page activation may assign focus automatically. Only an
                // input gesture or explicit reveal requests plaintext.
                if (args.FocusState() != mux::FocusState::Programmatic &&
                    !LoadPasswordForEditing(field)) args.Cancel(true);
            });
        field.passwordPointerPressed = field.password.PointerPressed(
            [this, &field](const auto&, const auto& args) {
                if (!LoadPasswordForEditing(field)) args.Handled(true);
            });
        field.lostFocus = field.password.LostFocus(
            [&field](const auto&, const auto&) {
                field.idleCommitTimer.Stop();
                field.idleCommitTimer.Interval(std::chrono::milliseconds(100));
                field.idleCommitTimer.Start();
            });
        field.passwordRevealLostFocus = field.passwordReveal.LostFocus(
            [&field](const auto&, const auto&) {
                field.idleCommitTimer.Stop();
                field.idleCommitTimer.Interval(std::chrono::milliseconds(100));
                field.idleCommitTimer.Start();
            });
        field.keyDown = field.password.PreviewKeyDown(
            [this, &field](const auto&,
                           const muxi::KeyRoutedEventArgs& args) {
                if (args.Key() == ws::VirtualKey::Tab || args.Key() == ws::VirtualKey::Escape) return;
                if (!LoadPasswordForEditing(field)) { args.Handled(true); return; }
                if (IsEnter(args))
                {
                    (void)CommitPassword(field);
                    args.Handled(true);
                }
            });
        RefreshPasswordReveal(field);
        BuildOpaqueActions(field, false, true, passwordEditors);
        field.editorHost.Children().Append(passwordEditors);
    }

    void BuildBooleanEditor(WidgetFieldControl& field)
    {
        field.toggle = muxc::ToggleSwitch{};
        field.toggle.MinWidth(0.0);
        field.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        field.toggled = field.toggle.Toggled(
            [this, &field](const auto&, const auto&) {
                if (!CanMutateField(field)) return;
                const bool value = field.toggle.IsOn();
                RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingBoolean(value));
                    });
            });
        field.editorHost.Children().Append(field.toggle);
    }

    void BuildNumericEditor(WidgetFieldControl& field)
    {
        const bool integer =
            field.schema.Kind() == wr::WidgetSettingKind::Integer;
        double minimum = std::isfinite(field.schema.minimum)
            ? field.schema.minimum
            : 0.0;
        double maximum = std::isfinite(field.schema.maximum)
            ? field.schema.maximum
            : 100.0;
        if (maximum < minimum) std::swap(maximum, minimum);
        double step = std::isfinite(field.schema.step) &&
                field.schema.step > 0.0
            ? field.schema.step
            : (integer ? 1.0 : 0.1);
        if (integer) step = std::max(1.0, std::round(step));

        field.numericEditors = muxc::Grid{};
        field.numericEditors.ColumnSpacing(8.0);
        field.numericEditors.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        muxc::ColumnDefinition sliderColumn{};
        sliderColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition numberColumn{};
        numberColumn.Width(mux::GridLengthHelper::Auto());
        field.numericEditors.ColumnDefinitions().Append(sliderColumn);
        field.numericEditors.ColumnDefinitions().Append(numberColumn);
        field.slider = muxc::Slider{};
        field.slider.Minimum(minimum);
        field.slider.Maximum(maximum);
        field.slider.StepFrequency(step);
        field.slider.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        field.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        field.number = muxc::NumberBox{};
        field.number.Minimum(minimum);
        field.number.Maximum(maximum);
        field.number.SmallChange(step);
        field.number.LargeChange(step * 5.0);
        field.number.Width(presenter_controls::kSettingNumberWidth);
        field.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        field.number.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        field.numericEditors.Children().Append(field.slider);
        muxc::Grid::SetColumn(field.number, 1);
        field.numericEditors.Children().Append(field.number);
        field.editorHost.Children().Append(field.numericEditors);

        field.idleCommitTimer = mux::DispatcherTimer{};
        field.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        field.idleCommitTick = field.idleCommitTimer.Tick(
            [this, &field](const auto&, const auto&) {
                field.idleCommitTimer.Stop();
                (void)CommitNumber(field);
            });

        field.sliderChanged = field.slider.ValueChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing || updatingControls) return;
                const double value = field.slider.Value();
                field.synchronizing = true;
                presenter_controls::SetNumberBoxValue(field.number, value);
                field.synchronizing = false;
                QueueNumberPreview(field, value);
            });
        field.numberChanged = field.number.ValueChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing || updatingControls) return;
                const double value = field.number.Value();
                if (!std::isfinite(value)) return;
                field.synchronizing = true;
                field.slider.Value(value);
                field.synchronizing = false;
                QueueNumberPreview(field, value);
            });
        const auto released = [this, &field](const auto&, const auto&) {
            (void)CommitNumber(field);
        };
        const auto lost = [this, &field](const auto&, const auto&) {
            (void)CommitNumber(field);
        };
        const auto key = [this, &field](
            const auto&, const muxi::KeyRoutedEventArgs& args) {
            if (IsEnter(args)) (void)CommitNumber(field);
        };
        field.sliderPointerReleased = field.slider.PointerReleased(released);
        field.numberPointerReleased = field.number.PointerReleased(released);
        field.sliderLostFocus = field.slider.LostFocus(lost);
        field.numberLostFocus = field.number.LostFocus(lost);
        field.sliderKeyDown = field.slider.KeyDown(key);
        field.numberKeyDown = field.number.KeyDown(key);
    }

    void BuildColorEditor(WidgetFieldControl& field)
    {
        field.colorEditor =
            std::make_unique<presenter_controls::ColorFlyoutEditor>();
        field.colorEditor->Initialize(
            [this, &field](
                const wui::Color& color,
                SettingsUpdateMode mode) {
                if (!CanMutateField(field)) return;
                const long long value = FromColor(color);
                QueueTransientOrdinary(field.schema.key,
                    wr::MakeWidgetSettingInteger(value));
                if (mode == SettingsUpdateMode::PreviewAndCommit)
                    (void)CommitTransientOwner(field.schema.key);
            });
        // ColorFlyoutEditor normally owns a complete SettingRow. This
        // presenter already supplies the common field row, so move only its
        // compact swatch button into that row's editor host.
        field.colorEditor->row.controlHost.Content(nullptr);
        field.editorHost.Children().Append(field.colorEditor->button);
    }

    void BuildSelectEditor(WidgetFieldControl& field)
    {
        field.select = muxc::ComboBox{};
        field.select.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.select.MaxWidth(520.0);
        field.select.MinWidth(0.0);
        field.select.MaxDropDownHeight(320.0);
        for (const auto& option : field.schema.options)
        {
            muxc::TextBlock label;
            const auto text = ToText(option.label.empty() ? option.value : option.label);
            label.Text(text);
            label.TextWrapping(mux::TextWrapping::Wrap);
            label.MaxLines(2);
            label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            muxc::ToolTipService::SetToolTip(label, winrt::box_value(text));
            field.select.Items().Append(label);
        }
        field.selectionChanged = field.select.SelectionChanged(
            [this, &field](const auto&, const auto&) {
                if (!CanMutateField(field)) return;
                const int index = field.select.SelectedIndex();
                if (index < 0 || static_cast<std::size_t>(index) >=
                        field.schema.options.size())
                    return;
                const std::string value = field.schema.options[
                    static_cast<std::size_t>(index)].value;
                RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingString(value));
                    });
            });
        field.editorHost.Children().Append(field.select);
    }

    void BuildMultiSelectEditor(WidgetFieldControl& field)
    {
        field.multiSelect = muxc::ListView{};
        field.multiSelect.SelectionMode(
            muxc::ListViewSelectionMode::None);
        field.multiSelect.MaxHeight(320.0);
        field.multiSelect.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.multiSelect.IsTabStop(false);
        muxc::ScrollViewer::SetHorizontalScrollMode(field.multiSelect, muxc::ScrollMode::Disabled);
        muxc::ScrollViewer::SetHorizontalScrollBarVisibility(field.multiSelect, muxc::ScrollBarVisibility::Disabled);
        mux::Style optionStyle{winrt::xaml_typename<muxc::ListViewItem>()};
        optionStyle.Setters().Append(mux::Setter{muxc::Control::HorizontalContentAlignmentProperty(),
            winrt::box_value(mux::HorizontalAlignment::Stretch)});
        optionStyle.Setters().Append(mux::Setter{muxc::Control::PaddingProperty(), winrt::box_value(mux::Thickness{0})});
        field.multiSelect.ItemContainerStyle(optionStyle);
        for (const auto& option : field.schema.options)
        {
            MultiSelectOptionControl item;
            item.value = option.value;
            item.checkBox = muxc::CheckBox{};
            muxc::TextBlock label;
            const auto text = ToText(option.label.empty() ? option.value : option.label);
            label.Text(text);
            label.TextWrapping(mux::TextWrapping::Wrap);
            item.checkBox.Content(label);
            item.checkBox.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            muxa::AutomationProperties::SetName(item.checkBox, text);
            item.checkBox.HorizontalAlignment(
                mux::HorizontalAlignment::Stretch);
            item.checked = item.checkBox.Checked(
                [this, &field](const auto&, const auto&) {
                    CommitMultiSelect(field);
                });
            item.unchecked = item.checkBox.Unchecked(
                [this, &field](const auto&, const auto&) {
                    CommitMultiSelect(field);
                });
            field.multiSelect.Items().Append(item.checkBox);
            field.multiOptions.push_back(std::move(item));
        }
        // The list owns a bounded vertical viewport below the complete row;
        // option text uses the page width instead of the editor column.
        field.row.SetCompactEditor(true);
        field.content.Children().Append(field.multiSelect);
    }

    void BuildDurationEditor(WidgetFieldControl& field)
    {
        muxc::Grid parts;
        parts.ColumnSpacing(8.0);
        const int scale = wr::DurationSettingUnitSeconds(field.schema.durationUnit);
        const int count = scale == 3600 ? 1 : scale == 60 ? 2 : 3;
        field.idleCommitTimer = mux::DispatcherTimer{};
        field.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        field.idleCommitTick = field.idleCommitTimer.Tick([this, &field](const auto&, const auto&) {
            (void)CommitDuration(field);
        });
        for (int index = 0; index < count; ++index)
        {
            muxc::ColumnDefinition column;
            column.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
            parts.ColumnDefinitions().Append(column);
            auto& number = field.durationParts[index];
            number = muxc::NumberBox{};
            number.MinWidth(0.0);
            number.Minimum(0);
            number.Maximum(index == 0 ? 99 : 59);
            number.SmallChange(1);
            number.LargeChange(index == 0 ? 1 : 5);
            number.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
            number.ValidationMode(muxc::NumberBoxValidationMode::Disabled);
            winrt::Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
            formatter.FractionDigits(0);
            formatter.IsGrouped(false);
            number.NumberFormatter(formatter);
            muxc::Grid::SetColumn(number, index);
            parts.Children().Append(number);
            field.durationLoadedTokens[index] = number.Loaded([this, &field, index](const auto&, const auto&) {
                const auto input = FindDurationInput(field.durationParts[index]);
                if (!input || input == field.durationInputs[index]) return;
                if (field.durationInputs[index] && HasToken(field.durationTextTokens[index]))
                    field.durationInputs[index].TextChanged(field.durationTextTokens[index]);
                field.durationInputs[index] = input;
                field.durationObservedText[index] = input.Text();
                field.durationTextTokens[index] = input.TextChanged([this, &field, index](const auto&, const auto&) {
                    if (field.synchronizing) return;
                    const auto text = field.durationInputs[index].Text();
                    if (text != field.durationObservedText[index])
                    {
                        field.durationObservedText[index] = text;
                        field.durationDirty = true;
                        if (field.restoreDefault) field.restoreDefault.IsEnabled(field.enabled);
                        field.draftError.clear();
                        RefreshFieldValidation(field);
                        field.idleCommitTimer.Stop();
                        field.idleCommitTimer.Start();
                    }
                });
            });
            field.durationKeyTokens[index] = number.KeyDown([this, &field](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args)) (void)CommitDuration(field);
            });
        }
        field.editorHost.Children().Append(parts);
    }

    void BuildDateEditor(WidgetFieldControl& field)
    {
        field.date = muxc::CalendarDatePicker{};
        field.date.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.date.MinWidth(0.0);
        field.date.CalendarIdentifier(L"GregorianCalendar");
        if (const auto first = ParseDate("0001-01-01")) field.date.MinDate(*first);
        if (const auto last = ParseDate("9999-12-31")) field.date.MaxDate(*last);
        field.date.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        field.dateChanged = field.date.DateChanged(
            [this, &field](const muxc::CalendarDatePicker&,
                           const muxc::CalendarDatePickerDateChangedEventArgs& args) {
                if (!CanMutateField(field)) return;
                const auto selected = args.NewDate();
                if (!selected && field.schema.required)
                {
                    RestoreTemporalValue(field);
                    return;
                }
                const std::string value = selected
                    ? FormatDate(selected.Value())
                    : std::string{};
                if (value == field.temporalValue) return;
                const auto result = RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingString(value));
                    });
                if (!result.Succeeded()) RestoreTemporalValue(field);
            });
        BuildNullableEditor(field, field.date);
    }

    void BuildTimeEditor(WidgetFieldControl& field)
    {
        field.time = muxc::TimePicker{};
        field.time.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.time.MinWidth(242.0);
        field.time.ClockIdentifier(L"24HourClock");
        field.time.MinuteIncrement(1);
        field.timeChanged = field.time.SelectedTimeChanged(
            [this, &field](const muxc::TimePicker&,
                           const muxc::TimePickerSelectedValueChangedEventArgs& args) {
                if (!CanMutateField(field)) return;
                const auto selected = args.NewTime();
                if (!selected && field.schema.required)
                {
                    RestoreTemporalValue(field);
                    return;
                }
                const std::string value = selected
                    ? FormatTime(selected.Value())
                    : std::string{};
                if (value == field.temporalValue) return;
                const auto result = RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingString(value));
                    });
                if (!result.Succeeded()) RestoreTemporalValue(field);
            });
        BuildNullableEditor(field, field.time);
    }

    void BuildNullableEditor(WidgetFieldControl& field, const mux::UIElement& editor)
    {
        muxc::Grid pair;
        pair.ColumnSpacing(8.0);
        muxc::ColumnDefinition value;
        value.Width(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
        pair.ColumnDefinitions().Append(value);
        pair.Children().Append(editor);
        BuildOpaqueActions(field, false, !field.schema.required, pair);
        field.editorHost.Children().Append(pair);
    }

    void RestoreTemporalValue(WidgetFieldControl& field)
    {
        if (const auto snapshot = service.Snapshot(widgetId))
            for (const auto& state : snapshot->fields)
                if (state.schema.key == field.schema.key)
                {
                    PatchField(field, state);
                    break;
                }
    }

    void BuildSearchEditor(WidgetFieldControl& field)
    {
        muxc::Grid searchRow;
        searchRow.ColumnSpacing(8.0);
        muxc::ColumnDefinition searchColumn;
        searchColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        searchRow.ColumnDefinitions().Append(searchColumn);
        field.search = muxc::AutoSuggestBox{};
        field.search.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.search.MinWidth(0.0);
        field.search.QueryIcon(muxc::SymbolIcon{muxc::Symbol::Find});
        field.search.VerticalAlignment(mux::VerticalAlignment::Center);
        searchRow.Children().Append(field.search);
        field.editorHost.Children().Append(searchRow);
        if (field.schema.Kind() == wr::WidgetSettingKind::AppReference)
        {
            BuildOpaqueActions(field, true, true, searchRow);
            BuildReferenceStatus(field);
        }

        field.searchProgress = muxc::ProgressRing{};
        field.searchProgress.Width(22.0);
        field.searchProgress.Height(22.0);
        field.searchProgress.Visibility(mux::Visibility::Collapsed);
        field.searchError = muxc::TextBlock{};
        field.searchError.TextWrapping(mux::TextWrapping::Wrap);
        field.searchError.Visibility(mux::Visibility::Collapsed);
        field.searchResults = muxc::ListView{};
        field.searchResults.SelectionMode(
            muxc::ListViewSelectionMode::Single);
        field.searchResults.IsItemClickEnabled(true);
        field.searchResults.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.searchResults.MaxHeight(300.0);
        field.searchResults.Visibility(mux::Visibility::Collapsed);

        field.searchTextChanged = field.search.TextChanged(
            [this, &field](const muxc::AutoSuggestBox&,
                           const muxc::AutoSuggestBoxTextChangedEventArgs& args) {
                if (updatingControls || field.synchronizing || closed ||
                    args.Reason() !=
                        muxc::AutoSuggestionBoxTextChangeReason::UserInput)
                    return;
                BeginSearch(field, ToUtf8(field.search.Text()));
            });
        field.searchSubmitted = field.search.QuerySubmitted(
            [this, &field](const muxc::AutoSuggestBox&,
                           const muxc::AutoSuggestBoxQuerySubmittedEventArgs&) {
                if (!field.resultValues.empty())
                    CommitSearchResult(field, static_cast<std::size_t>(std::max(0, field.searchResults.SelectedIndex())));
                else
                    BeginSearch(field, ToUtf8(field.search.Text()));
            });
        field.searchKeyDown = field.search.PreviewKeyDown(
            [this, &field](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (!CanMutateField(field)) return;
                if (args.Key() == ws::VirtualKey::Escape &&
                    field.searchResults.Visibility() == mux::Visibility::Visible)
                {
                    (void)service.CancelSearch(Guard(), field.schema.key);
                    ClearSearch(field);
                    args.Handled(true);
                }
                else if ((args.Key() == ws::VirtualKey::Down || args.Key() == ws::VirtualKey::Up) &&
                    !field.resultValues.empty())
                {
                    const int last = static_cast<int>(field.resultValues.size()) - 1;
                    const int selected = field.searchResults.SelectedIndex();
                    const int next = selected < 0 ? (args.Key() == ws::VirtualKey::Down ? 0 : last)
                        : std::clamp(selected + (args.Key() == ws::VirtualKey::Down ? 1 : -1), 0, last);
                    field.searchResults.SelectedIndex(next);
                    field.searchResults.ScrollIntoView(field.searchResults.Items().GetAt(static_cast<std::uint32_t>(next)));
                    args.Handled(true);
                }
            });
        field.resultKeyDown = field.searchResults.KeyDown(
            [this, &field](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args) && field.searchResults.SelectedIndex() >= 0)
                {
                    CommitSearchResult(field, static_cast<std::size_t>(field.searchResults.SelectedIndex()));
                    args.Handled(true);
                }
            });
        field.searchResultClicked =
            field.searchResults.ItemClick(
                [this, &field](const auto&, const muxc::ItemClickEventArgs& args) {
                    if (updatingControls || field.synchronizing) return;
                    std::uint32_t index = 0;
                    if (field.searchResults.Items().IndexOf(args.ClickedItem(), index))
                        CommitSearchResult(field, index);
                });
        // Results belong below the complete setting row, outside its fixed
        // editor column. They must not stretch the label's vertical alignment.
        field.content.Children().Append(field.searchProgress);
        field.content.Children().Append(field.searchError);
        field.content.Children().Append(field.searchResults);
    }

    void BuildOpaqueActions(
        WidgetFieldControl& field,
        bool allowChoose,
        bool allowClear,
        const muxc::Grid& inlineRow)
    {
        field.opaqueActions = muxc::StackPanel{};
        field.opaqueActions.Orientation(muxc::Orientation::Horizontal);
        field.opaqueActions.Spacing(8.0);
        field.opaqueActions.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        field.opaqueActions.VerticalAlignment(mux::VerticalAlignment::Center);
        const auto configureInlineAction = [](const muxc::Button& button,
                                               muxc::Symbol symbol) {
            muxc::SymbolIcon icon{symbol};
            button.Content(icon);
            button.Width(32.0);
            button.Height(32.0);
            button.MinWidth(32.0);
            button.MinHeight(32.0);
            button.Padding({0.0, 0.0, 0.0, 0.0});
        };
        if (allowChoose)
        {
            field.choose = muxc::Button{};
            configureInlineAction(field.choose, muxc::Symbol::OpenFile);
            field.chooseClicked = field.choose.Click(
                [this, &field](const auto&, const auto&) {
                    ChooseOpaque(field);
                });
            field.opaqueActions.Children().Append(field.choose);
        }
        if (allowClear)
        {
            field.clear = muxc::Button{};
            configureInlineAction(field.clear, muxc::Symbol::ClearSelection);
            field.clearClicked = field.clear.Click(
                [this, &field](const auto&, const auto&) {
                    ClearOpaque(field);
                });
            field.opaqueActions.Children().Append(field.clear);
        }
        if (allowChoose || allowClear)
        {
            muxc::ColumnDefinition actionsColumn;
            actionsColumn.Width(mux::GridLengthHelper::Auto());
            const auto column = inlineRow.ColumnDefinitions().Size();
            inlineRow.ColumnDefinitions().Append(actionsColumn);
            muxc::Grid::SetColumn(field.opaqueActions, static_cast<int>(column));
            inlineRow.Children().Append(field.opaqueActions);
        }
    }

    void BuildReferenceEditor(WidgetFieldControl& field)
    {
        muxc::Grid pair;
        pair.ColumnSpacing(8.0);
        muxc::ColumnDefinition name;
        name.Width(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
        pair.ColumnDefinitions().Append(name);
        field.referenceText = muxc::TextBox{};
        field.referenceText.IsReadOnly(true);
        field.referenceText.MinWidth(0.0);
        field.referenceText.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxa::AutomationProperties::SetName(field.referenceText, ToText(field.schema.label));
        pair.Children().Append(field.referenceText);
        BuildOpaqueActions(field, true, true, pair);
        field.editorHost.Children().Append(pair);
        BuildReferenceStatus(field);
    }

    void BuildReferenceStatus(WidgetFieldControl& field)
    {
        field.referenceStatus = muxc::TextBlock{};
        field.referenceStatus.TextWrapping(mux::TextWrapping::Wrap);
        field.referenceStatus.FontSize(12.0);
        field.referenceStatus.Visibility(mux::Visibility::Collapsed);
        field.content.Children().Append(field.referenceStatus);
    }

    std::unique_ptr<WidgetFieldControl> BuildField(
        const wr::WidgetSettingFieldState& state)
    {
        auto field = std::make_unique<WidgetFieldControl>();
        field->schema = state.schema;
        InitializeFieldCard(*field);
        switch (field->schema.Kind())
        {
        case wr::WidgetSettingKind::Text:
        case wr::WidgetSettingKind::Url:
        case wr::WidgetSettingKind::Unknown:
            BuildTextEditor(*field);
            break;
        case wr::WidgetSettingKind::Password:
            BuildPasswordEditor(*field);
            break;
        case wr::WidgetSettingKind::Boolean:
            BuildBooleanEditor(*field);
            break;
        case wr::WidgetSettingKind::Integer:
        case wr::WidgetSettingKind::FloatingPoint:
        case wr::WidgetSettingKind::Range:
            BuildNumericEditor(*field);
            break;
        case wr::WidgetSettingKind::Color:
            BuildColorEditor(*field);
            break;
        case wr::WidgetSettingKind::Select:
            BuildSelectEditor(*field);
            break;
        case wr::WidgetSettingKind::MultiSelect:
            BuildMultiSelectEditor(*field);
            break;
        case wr::WidgetSettingKind::Date:
            BuildDateEditor(*field);
            break;
        case wr::WidgetSettingKind::Duration:
            BuildDurationEditor(*field);
            break;
        case wr::WidgetSettingKind::Time:
            BuildTimeEditor(*field);
            break;
        case wr::WidgetSettingKind::AppSearch:
        case wr::WidgetSettingKind::AppReference:
            BuildSearchEditor(*field);
            break;
        case wr::WidgetSettingKind::DesktopItemReference:
        case wr::WidgetSettingKind::FileReference:
        case wr::WidgetSettingKind::FolderReference:
        case wr::WidgetSettingKind::FileHandle:
        case wr::WidgetSettingKind::FolderHandle:
            BuildReferenceEditor(*field);
            break;
        }
        AppendValidation(*field);
        SetFieldLocalizedText(*field);
        return field;
    }

    void RebuildFields(
        const wr::WidgetSettingsSnapshot& snapshot,
        bool preserveExpansion)
    {
        UnhookFields();
        fieldsHost.Children().Clear();
        fields.clear();
        fieldsByKey.clear();
        RebuildGroups(snapshot, preserveExpansion);

        InitializeCard(ungroupedCard);
        ungroupedCard.root.Padding({0, 0, 0, 0});
        ungroupedCard.content.Spacing(0.0);
        bool hasUngrouped = false;
        for (const auto& state : snapshot.fields)
        {
            auto field = BuildField(state);
            WidgetFieldControl* pointer = field.get();
            auto parent = groupPanels.find(state.schema.group);
            if (!state.schema.group.empty() && parent != groupPanels.end())
                parent->second.Children().Append(field->root);
            else
            {
                ungroupedCard.content.Children().Append(field->root);
                hasUngrouped = true;
            }
            fieldsByKey.emplace(state.schema.key, pointer);
            fields.push_back(std::move(field));
        }
        if (hasUngrouped)
            fieldsHost.Children().Append(ungroupedCard.root);
        // Match the legacy editor: fields without a group always
        // precede authored groups, and groups retain their declaration order.
        for (auto& group : groups)
            fieldsHost.Children().Append(group.root);

        cachedSchemas.clear();
        cachedSchemas.reserve(snapshot.fields.size());
        for (const auto& field : snapshot.fields)
            cachedSchemas.push_back(field.schema);
        cachedGroups = snapshot.groups;
        cachedPresets = snapshot.presets;
        defaultPresetId = snapshot.defaultPresetId;
        cachedCustomStyle = snapshot.customStyle;
        currentHostAppearance = snapshot.hostAppearance;
        RebuildPresetItems();
    }

    [[nodiscard]] bool SchemaChanged(
        const wr::WidgetSettingsSnapshot& snapshot) const
    {
        if (snapshot.fields.size() != cachedSchemas.size() ||
            snapshot.groups != cachedGroups ||
            snapshot.presets != cachedPresets ||
            snapshot.defaultPresetId != defaultPresetId ||
            snapshot.customStyle != cachedCustomStyle)
            return true;
        for (std::size_t index = 0; index < snapshot.fields.size(); ++index)
            if (snapshot.fields[index].schema != cachedSchemas[index])
                return true;
        return false;
    }

    void RebuildPresetItems()
    {
        const bool oldUpdating = updatingControls;
        updatingControls = true;
        appearanceThemeChoices.clear();
        appearanceTheme.Items().Clear();
        constexpr std::array<const char*, 7> ids = {
            "__global_dark", "__global_light",
            "__global_glass_dark", "__global_glass_light", "__global_glass_transparent",
            "__global_acrylic_dark", "__global_acrylic_light",
        };
        constexpr std::array<int, 7> presetIds = {
            kAppearancePresetDark, kAppearancePresetLight,
            kAppearancePresetGlassDark, kAppearancePresetGlassLight, kAppearancePresetGlassTransparent,
            kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight,
        };
        constexpr std::array<const char*, 7> labelKeys = {
            "app.settings.dark", "app.settings.light",
            "app.settings.dark_glass", "app.settings.light_glass", "app.settings.transparent_glass",
            "app.settings.dark_acrylic", "app.settings.light_acrylic",
        };
        constexpr std::array<const wchar_t*, 7> labelFallbacks = {
            L"Dark", L"Light", L"Dark glass", L"Light glass", L"Transparent glass",
            L"Dark acrylic", L"Light acrylic",
        };
        for (std::size_t index = 0; index < ids.size(); ++index)
        {
            std::wstring formatted = L("engine.editor.global_theme_format",
                L"Global - {0}");
            const std::wstring name = L(labelKeys[index],
                labelFallbacks[index]);
            const auto marker = formatted.find(L"{0}");
            if (marker == std::wstring::npos)
                formatted += L" " + name;
            else
                formatted.replace(marker, 3, name);
            appearanceTheme.Items().Append(winrt::box_value(formatted));
            appearanceThemeChoices.push_back({
                AppearanceThemeKind::BuiltIn, ids[index],
                presetIds[index], 0});
        }
        appearanceTheme.Items().Append(winrt::box_value(
            L("app.settings.custom", L"Custom")));
        appearanceThemeChoices.push_back({
            AppearanceThemeKind::Custom, "__custom",
            kAppearancePresetCustom, 0});
        themes::Library library;
        std::string libraryError;
        if (themes::Load(themes::LibraryPath(), library, libraryError))
            for (const auto& theme : themes::Choices(library, themes::Kind::Global, themes::Components))
            {
                if (themes::Builtin(theme.id)) continue;
                appearanceTheme.Items().Append(winrt::box_value(ToText(theme.name)));
                appearanceThemeChoices.push_back({AppearanceThemeKind::Saved, theme.id, kAppearancePresetCustom, 0});
            }
        for (std::size_t index = 0; index < cachedPresets.size(); ++index)
        {
            const auto& preset = cachedPresets[index];
            appearanceTheme.Items().Append(winrt::box_value(ToText(
                preset.label.empty() ? preset.id : preset.label)));
            appearanceThemeChoices.push_back({
                AppearanceThemeKind::Component, preset.id,
                kAppearancePresetCustom, index});
        }

        presetCombo.Items().Clear();
        for (std::size_t index = 0; index < cachedPresets.size(); ++index)
        {
            const auto& preset = cachedPresets[index];
            presetCombo.Items().Append(winrt::box_value(ToText(
                preset.label.empty() ? preset.id : preset.label)));
        }
        appearanceTheme.SelectedIndex(-1);
        presetCombo.SelectedIndex(-1);
        appearanceTheme.IsEnabled(cachedCustomStyle);
        presetCombo.IsEnabled(!cachedPresets.empty());
        restoreScriptDefault.IsEnabled(!defaultPresetId.empty());
        updatingControls = oldUpdating;
    }

    [[nodiscard]] bool IsAuthoritative(
        const wr::WidgetSettingsSnapshot& snapshot) const
    {
        const auto current = service.Snapshot(snapshot.widgetId);
        return current &&
            current->generation == snapshot.generation &&
            current->revision == snapshot.revision;
    }

    bool ApplySnapshot(const wr::WidgetSettingsSnapshot& snapshot)
    {
        if (closed || snapshot.widgetId.empty() ||
            snapshot.generation == 0 || snapshot.revision == 0 ||
            !IsAuthoritative(snapshot))
            return false;
        if (hasSnapshot && snapshot.widgetId == widgetId &&
            snapshot.generation == generation &&
            snapshot.revision <= revision)
            return false;

        const bool newIdentity = !hasSnapshot ||
            snapshot.widgetId != widgetId ||
            snapshot.generation != generation;
        const bool rebuild = newIdentity || SchemaChanged(snapshot);
        if (rebuild)
        {
            panelGradientEditor->SetValue(snapshot.hostAppearance.panelGradient, true);
            edgeLightEditor->SetValue(snapshot.hostAppearance.edgeLight, true);
            pendingTransientPreviews.clear();
            transientPreviewActive = false;
            transientPreviewOwner.clear();
            for (AppearanceScalarControl* control : {
                    &backgroundOpacity, &borderOpacity,
                    &borderWidth, &edgeHighlightWidth,
                    &edgeHighlightStrength,
                    &gradientEndOpacity })
            {
                if (control->idleCommitTimer)
                    control->idleCommitTimer.Stop();
                control->continuousDirty = false;
            }
        }
        const bool oldUpdating = updatingControls;
        updatingControls = true;
        const bool nameChanged = newIdentity || widgetName != snapshot.widgetName;
        widgetId = snapshot.widgetId;
        widgetName = snapshot.widgetName;
        packageId = snapshot.packageId;
        generation = snapshot.generation;
        revision = snapshot.revision;
        hasSnapshot = true;
        if (rebuild) RebuildFields(snapshot, !newIdentity);
        PatchAppearance(snapshot);
        PatchFields(snapshot);
        updatingControls = oldUpdating;
        if (rebuild) ReportDiagnostics();
        if (nameChanged && callbacks.nameChanged) callbacks.nameChanged();
        return true;
    }

    void PatchAppearanceScalar(
        AppearanceScalarControl& control, float value)
    {
        control.synchronizing = true;
        const double normalized = std::clamp(
            static_cast<double>(value) / control.scale,
            control.minimum, control.maximum);
        control.slider.Value(normalized);
        presenter_controls::SyncNumberBoxValue(control.number, normalized);
        control.synchronizing = false;
    }

    void UpdateBackgroundControls(bool gradientEnabled)
    {
        appearanceSections.PlaceOpacity(backgroundOpacity.row.root, gradientEnabled);
        backgroundColorEditor->row.root.Visibility(gradientEnabled
            ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        backgroundOpacity.row.root.Visibility(gradientEnabled && currentHostAppearance.gradientEndOpacity <= .001f
            ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        backgroundOpacity.row.SetText(L(gradientEnabled
            ? "panelGradient.barStartOpacity" : "app.settings.bg_opacity", L"Background opacity"));
        appearanceSections.UpdateRows();
    }

    void PatchAppearance(const wr::WidgetSettingsSnapshot& snapshot)
    {
        currentHostAppearance = snapshot.hostAppearance;
        panelGradientEditor->SetValue(snapshot.hostAppearance.panelGradient);
        UpdateBackgroundControls(snapshot.hostAppearance.panelGradient.enabled);
        appearanceCard.root.Visibility(snapshot.customStyle
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        appearanceTitle.Visibility(appearanceCard.root.Visibility());
        stylePreviewCard.root.Visibility(
            !snapshot.customStyle && !cachedPresets.empty()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        fieldsHost.Visibility(snapshot.fields.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        scriptSettingsTitle.Visibility(snapshot.fields.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        resetCard.root.Visibility(snapshot.fields.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        relatedCard.root.Visibility(!packageId.empty() && callbacks.openPackagePermissions
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);

        followGlobal.IsOn(snapshot.hostAppearance.followPersonalization);
        int selectedTheme = -1;
        for (std::size_t index = 0;
            index < appearanceThemeChoices.size(); ++index)
        {
            if (appearanceThemeChoices[index].id ==
                snapshot.hostAppearance.presetId)
            {
                selectedTheme = static_cast<int>(index);
                break;
            }
        }
        const auto customChoice = std::find_if(appearanceThemeChoices.begin(), appearanceThemeChoices.end(), [](auto const& choice) { return choice.kind == AppearanceThemeKind::Custom; });
        themes::Library library;
        std::string libraryError;
        const std::string target = "widget/" + winrt::to_string(widgetId);
        if (themes::Load(themes::LibraryPath(), library, libraryError))
            if (const auto reference = library.references.find(target); reference != library.references.end() && !reference->second.id.empty())
            {
                const auto theme = themes::Resolve(reference->second.snapshot, reference->second.id);
                if (theme && themes::WidgetMatches(snapshot.hostAppearance, *theme))
                {
                    for (std::size_t i = 0; i < appearanceThemeChoices.size(); ++i)
                        if (appearanceThemeChoices[i].kind == AppearanceThemeKind::Saved && appearanceThemeChoices[i].id == reference->second.id)
                            selectedTheme = static_cast<int>(i);
                }
                else
                    (void)themes::Transact(themes::LibraryPath(), [&](auto& current, auto&) { themes::Detach(current, target); return true; }, library, libraryError);
            }
        const int customThemeIndex = static_cast<int>(std::distance(appearanceThemeChoices.begin(), customChoice));
        if (selectedTheme < 0 && snapshot.customStyle)
            selectedTheme = customThemeIndex;
        appearanceTheme.SelectedIndex(selectedTheme);
        const bool showCustomAppearance = snapshot.customStyle &&
            !snapshot.hostAppearance.followPersonalization &&
            customChoice != appearanceThemeChoices.end() &&
            selectedTheme == customThemeIndex;
        if (showCustomAppearance && customAppearanceHost.Visibility() != mux::Visibility::Visible)
            appearanceSections.CollapseAll();
        restoreBackgroundRow.root.Visibility(showCustomAppearance ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        appearanceTheme.IsEnabled(snapshot.customStyle &&
            !snapshot.hostAppearance.followPersonalization);
        customAppearanceHost.Visibility(
            showCustomAppearance
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);

        if (backgroundColorEditor)
            backgroundColorEditor->SetColor(ToColor(
                snapshot.hostAppearance.backgroundColor));
        PatchAppearanceScalar(backgroundOpacity,
            snapshot.hostAppearance.backgroundOpacity);
        if (borderColorEditor)
            borderColorEditor->SetColor(ToColor(
                snapshot.hostAppearance.borderColor));
        PatchAppearanceScalar(borderOpacity,
            snapshot.hostAppearance.borderOpacity);
        PatchAppearanceScalar(borderWidth,
            snapshot.hostAppearance.borderWidth);
        edgeLightEditor->SetValue(snapshot.hostAppearance.edgeLight);
        edgeLightEditor->Content().Visibility(snapshot.hostAppearance.edgeHighlightEnabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeHighlightEnabled.IsOn(
            snapshot.hostAppearance.edgeHighlightEnabled);
        PatchAppearanceScalar(edgeHighlightWidth,
            snapshot.hostAppearance.edgeHighlightWidth);
        PatchAppearanceScalar(edgeHighlightStrength,
            snapshot.hostAppearance.edgeHighlightStrength);
        edgeHighlightWidth.row.root.Visibility(snapshot.hostAppearance.edgeHighlightEnabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeHighlightStrength.row.root.Visibility(snapshot.hostAppearance.edgeHighlightEnabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeHighlightWidth.row.SetEnabled(
            snapshot.hostAppearance.edgeHighlightEnabled);
        edgeHighlightStrength.row.SetEnabled(
            snapshot.hostAppearance.edgeHighlightEnabled);
        PatchAppearanceScalar(gradientEndOpacity,
            snapshot.hostAppearance.gradientEndOpacity);
        glassEnabled.IsOn(snapshot.hostAppearance.glassEnabled);
        acrylicEnabled.IsOn(snapshot.hostAppearance.acrylicEnabled);
        acrylicRow.root.Visibility(snapshot.hostAppearance.glassEnabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        contentTheme.SelectedIndex(std::clamp(
            snapshot.hostAppearance.contentTheme, 0, 1));

        int selectedPreset = -1;
        for (std::size_t index = 0; index < cachedPresets.size(); ++index)
            if (cachedPresets[index].id ==
                snapshot.hostAppearance.presetId)
            {
                selectedPreset = static_cast<int>(index);
                break;
        }
        if (selectedPreset < 0 && !cachedPresets.empty())
            selectedPreset = 0;
        presetCombo.SelectedIndex(selectedPreset);
        restoreScriptDefaultRow.root.Visibility(defaultPresetId.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        appearanceSections.UpdateRows();
    }

    void PatchFields(const wr::WidgetSettingsSnapshot& snapshot)
    {
        std::unordered_map<std::string_view, const wr::WidgetSettingFieldState*> states;
        states.reserve(snapshot.fields.size());
        for (const auto& state : snapshot.fields)
            states.emplace(state.schema.key, &state);
        const auto conditionMatches = [&states](const wr::WidgetSettingCondition& condition) {
            const auto source = states.find(condition.key);
            if (source == states.end()) return false;
            auto expected = condition.values;
            // Match ResolveWidgetSettingFieldStates, including range snapping.
            // Opaque fields contribute configured state, never their contents.
            if (source->second->schema.Kind() == wr::WidgetSettingKind::Range)
                for (auto& value : expected)
                {
                    double parsed = source->second->schema.minimum;
                    if (wr::ParseFiniteSettingNumber(value, parsed))
                        value = wr::FormatWidgetSettingNumber(wr::SnapRangeSettingValue(
                            parsed, source->second->schema.minimum,
                            source->second->schema.maximum, source->second->schema.step));
                }
            return wr::EvaluateSettingCondition(condition.operation,
                wr::WidgetSettingConditionValues(*source->second), expected);
        };
        focusParents.clear();
        for (const auto& state : snapshot.fields)
        {
            // Visibility blocks access first. If enabledWhen is satisfied,
            // dependsOn can still be the condition disabling the same field.
            const wr::WidgetSettingCondition* condition = nullptr;
            wr::WidgetSettingCondition dependency;
            if (!state.visible && state.schema.showWhen)
                condition = &*state.schema.showWhen;
            else if (!state.enabled)
            {
                if (state.schema.enabledWhen && !conditionMatches(*state.schema.enabledWhen))
                    condition = &*state.schema.enabledWhen;
                else if (!state.schema.dependsOn.empty())
                {
                    dependency.key = state.schema.dependsOn;
                    dependency.operation = "truthy";
                    condition = &dependency;
                }
            }
            if (condition && !condition->key.empty())
            {
                std::string messageKey = "settings.search.prerequisite.dependency";
                const auto source = states.find(condition->key);
                if (source != states.end())
                {
                    const auto kind = source->second->schema.Kind();
                    if (kind == wr::WidgetSettingKind::Boolean)
                    {
                        // A condition may require false (or even accept neither
                        // state). Only name an on/off action when it is proven.
                        const bool acceptsOff = wr::EvaluateSettingCondition(
                            condition->operation, {"0"}, condition->values);
                        const bool acceptsOn = wr::EvaluateSettingCondition(
                            condition->operation, {"1"}, condition->values);
                        if (acceptsOff != acceptsOn)
                            messageKey = acceptsOn
                                ? "settings.search.prerequisite.enable"
                                : "settings.search.prerequisite.disable";
                    }
                    else if (kind == wr::WidgetSettingKind::Select ||
                        kind == wr::WidgetSettingKind::MultiSelect)
                        messageKey = "settings.search.prerequisite.choice";
                }
                focusParents.emplace(state.schema.key,
                    FocusParent{condition->key, std::move(messageKey)});
            }
            const auto found = fieldsByKey.find(state.schema.key);
            if (found != fieldsByKey.end())
                PatchField(*found->second, state);
        }
        for (auto& group : groups)
        {
            const bool anyVisible = std::any_of(
                fields.begin(), fields.end(), [&](const auto& field) {
                    return field->schema.group == group.schema.id &&
                        field->visible;
                });
            group.root.Visibility(anyVisible
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
            presenter_controls::UpdateSettingsSubrows(
                group.content, 0.0);
        }
        presenter_controls::UpdateSettingsSubrows(ungroupedCard.content, 0.0);
        const bool visibleUngrouped = std::any_of(fields.begin(), fields.end(),
            [this](const auto& field) {
                return field->visible && (field->schema.group.empty() ||
                    !groupPanels.contains(field->schema.group));
            });
        ungroupedCard.root.Visibility(visibleUngrouped
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        const bool anyVisible = std::any_of(fields.begin(), fields.end(),
            [](const auto& field) { return field->visible; });
        fieldsHost.Visibility(anyVisible
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        scriptSettingsTitle.Visibility(fieldsHost.Visibility());
    }

    void PatchField(
        WidgetFieldControl& field,
        const wr::WidgetSettingFieldState& state,
        bool updateFocusedSearch = false)
    {
        field.visible = state.visible;
        field.enabled = state.enabled;
        if (field.password)
        {
            field.passwordCanClear = state.opaque.canClear;
            field.passwordConfigured = state.opaque.configured;
            // A legacy length learned by an explicit read remains useful until
            // the next snapshot includes the migrated metadata.
            if (state.opaque.secretLength || !state.opaque.configured)
                field.passwordLength = state.opaque.secretLength;
        }
        field.root.Visibility(state.visible
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        field.root.Opacity(state.enabled ? 1.0 : 0.58);
        SetFieldEnabled(field, state.enabled);

        if (field.text && !field.textDirty)
        {
            field.synchronizing = true;
            const auto value = ToText(StringValue(state.currentValue));
            field.textObservedValue = value;
            if (field.text.Text() != value) field.text.Text(value);
            field.synchronizing = false;
        }
        if (field.password)
        {
            RefreshPasswordPlaceholder(field);
            if (!field.passwordDirty &&
                !PasswordEditorHasFocus(field) &&
                !field.passwordFlyout.IsOpen())
            {
                ClearPasswordDraft(field);
            }
        }
        if (field.toggle)
            field.toggle.IsOn(BooleanValue(state.currentValue));
        if (field.slider && field.number)
        {
            const double value = NumericValue(
                state.currentValue, field.schema.minimum);
            field.synchronizing = true;
            field.slider.Value(value);
            presenter_controls::SyncNumberBoxValue(field.number, value);
            field.synchronizing = false;
        }
        if (field.colorEditor)
        {
            const long long value = state.currentValue.type ==
                    wr::InteractionValue::Type::Integer
                ? state.currentValue.integer
                : 0;
            field.colorEditor->SetColor(ToColor(value));
        }
        if (field.select)
        {
            const std::string value = StringValue(state.currentValue);
            int selected = -1;
            for (std::size_t index = 0;
                 index < field.schema.options.size(); ++index)
                if (field.schema.options[index].value == value)
                {
                    selected = static_cast<int>(index);
                    break;
                }
            field.select.SelectedIndex(selected);
            muxc::ToolTipService::SetToolTip(field.select, winrt::box_value(selected < 0 ? winrt::hstring{}
                : ToText(field.schema.options[static_cast<std::size_t>(selected)].label.empty()
                    ? field.schema.options[static_cast<std::size_t>(selected)].value
                    : field.schema.options[static_cast<std::size_t>(selected)].label)));
        }
        if (field.multiSelect)
        {
            const auto values = StringArrayValue(state.currentValue);
            field.synchronizing = true;
            for (auto& option : field.multiOptions)
                option.checkBox.IsChecked(std::find(
                    values.begin(), values.end(), option.value) !=
                    values.end());
            field.synchronizing = false;
        }
        if (field.date)
        {
            field.synchronizing = true;
            field.temporalValue = StringValue(state.currentValue);
            const auto date = ParseDate(field.temporalValue);
            const auto current = field.date.Date();
            if (static_cast<bool>(current) != date.has_value() || (current && date && current.Value() != *date))
                field.date.Date(date ? winrt::box_value(*date).as<wf::IReference<wf::DateTime>>() : nullptr);
            field.synchronizing = false;
        }
        if (field.durationParts[0] && !field.durationDirty)
        {
            const auto seconds = state.currentValue.integer * wr::DurationSettingUnitSeconds(field.schema.durationUnit);
            const std::array<long long, 3> values{seconds / 3600, seconds / 60 % 60, seconds % 60};
            field.synchronizing = true;
            for (std::size_t index = 0; index < field.durationParts.size(); ++index)
                if (const auto number = field.durationParts[index])
                {
                    if (number.Value() != static_cast<double>(values[index])) number.Value(static_cast<double>(values[index]));
                    const auto text = field.durationInputs[index] ? field.durationInputs[index].Text() : number.Text();
                    long long displayed = 0;
                    if (!wr::ReadWidgetSettingInteger(ToUtf8(text), displayed) || displayed != values[index])
                    {
                        const auto restored = ToText(std::to_string(values[index]));
                        number.Text(restored);
                        // NumberBox.Text may already equal the saved value while
                        // its uncommitted native InputBox still contains a draft.
                        if (field.durationInputs[index] && field.durationInputs[index].Text() != restored)
                            field.durationInputs[index].Text(restored);
                    }
                    field.durationObservedText[index] = field.durationInputs[index] ? field.durationInputs[index].Text() : number.Text();
                }
            field.synchronizing = false;
        }
        if (field.time)
        {
            field.synchronizing = true;
            field.temporalValue = StringValue(state.currentValue);
            const auto time = ParseTime(field.temporalValue);
            const auto current = field.time.SelectedTime();
            if (static_cast<bool>(current) != time.has_value() || (current && time && current.Value() != *time))
                field.time.SelectedTime(time ? winrt::box_value(*time).as<wf::IReference<wf::TimeSpan>>() : nullptr);
            field.synchronizing = false;
        }
        const bool referenceSelectionChanged = field.search &&
            state.schema.Kind() == wr::WidgetSettingKind::AppReference &&
            (!field.referenceState ||
                field.referenceState->configured != state.opaque.configured ||
                field.referenceState->displayLabel != state.opaque.displayLabel);
        const bool referenceSearchFinished = field.search &&
            state.schema.Kind() == wr::WidgetSettingKind::AppReference &&
            field.searchRequestId != 0 &&
            !service.SearchSnapshot(widgetId, field.schema.key);
        if (field.search && state.schema.Kind() == wr::WidgetSettingKind::AppReference)
        {
            if ((referenceSelectionChanged && field.referenceState) || referenceSearchFinished)
                ClearSearch(field);
            field.referenceState = state.opaque;
        }
        if (field.search && (updateFocusedSearch || referenceSelectionChanged || referenceSearchFinished ||
            field.search.FocusState() == mux::FocusState::Unfocused))
        {
            field.synchronizing = true;
            if (state.schema.Kind() == wr::WidgetSettingKind::AppReference)
                field.search.Text(ToText(state.opaque.displayLabel));
            else
                field.search.Text(ToText(StringValue(state.currentValue).empty()
                    ? state.searchQuery : StringValue(state.currentValue)));
            field.synchronizing = false;
        }
        if (field.referenceText)
        {
            field.referenceText.Text(ToText(state.opaque.displayLabel));
            field.referenceText.PlaceholderText(state.schema.emptyLabel.empty()
                ? L("app.settings.widget.choose", L"Choose or replace") : ToWide(state.schema.emptyLabel));
            muxc::ToolTipService::SetToolTip(field.referenceText,
                winrt::box_value(ToText(state.opaque.displayLabel)));
        }
        if (field.referenceStatus)
        {
            const bool unavailable = state.opaque.configured && !state.opaque.available;
            field.referenceStatus.Text(unavailable
                ? L("iconAppearance.unavailable", L"Target unavailable") : std::wstring{});
            field.referenceStatus.Visibility(unavailable ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        }
        if (field.choose)
            field.choose.IsEnabled(state.enabled && state.opaque.canChoose);
        if (field.clear)
            field.clear.IsEnabled(state.enabled && ((field.date || field.time)
                ? !state.schema.required && !StringValue(state.currentValue).empty()
                : state.opaque.canClear || (field.password && !field.password.Password().empty())));
        if (field.restoreDefault)
        {
            const bool differsFromDefault =
                state.currentValue != state.defaultValue ||
                (state.schema.Kind() == wr::WidgetSettingKind::AppSearch &&
                    !state.searchQuery.empty());
            field.restoreDefault.IsEnabled(
                state.enabled && (differsFromDefault || field.textDirty || field.durationDirty));
        }

        field.diagnostic.Text(state.diagnosticCode.empty()
            ? winrt::hstring{}
            : winrt::hstring(L("app.settings.widget.unknown_type",
                L"Unknown setting type; using a text field.")));
        field.diagnostic.Visibility(state.diagnosticCode.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        RefreshFieldValidation(field, &state);
    }

    void RefreshFieldValidation(WidgetFieldControl& field,
        const wr::WidgetSettingFieldState* supplied = nullptr)
    {
        const wr::WidgetSettingFieldState* state = supplied;
        std::optional<wr::WidgetSettingsSnapshot> snapshot;
        if (!state && hasSnapshot)
        {
            snapshot = service.Snapshot(widgetId);
            if (snapshot)
            {
                const auto found = std::find_if(snapshot->fields.begin(),
                    snapshot->fields.end(), [&](const auto& candidate) {
                        return candidate.schema.key == field.schema.key;
                    });
                if (found != snapshot->fields.end()) state = &*found;
            }
        }
        std::string validation = field.draftError;
        if (validation.empty() && state && !state->valid)
        {
            validation = state->schema.validationMessage.empty()
                ? state->validationError
                : state->schema.validationMessage;
        }
        field.validation.Text(ToText(validation));
        field.validation.Visibility(validation.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        const std::wstring status = !validation.empty()
            ? std::wstring(field.validation.Text())
            : field.referenceStatus && field.referenceStatus.Visibility() == mux::Visibility::Visible
                ? std::wstring(field.referenceStatus.Text()) : std::wstring(field.diagnostic.Text());
        muxa::AutomationProperties::SetItemStatus(field.root, status);
    }

    void SetDraftError(WidgetFieldControl& field,
        const wr::WidgetSettingMutationResult& result)
    {
        if (result.status == wr::WidgetSettingMutationStatus::InvalidValue)
        {
            field.draftError = !field.schema.validationMessage.empty()
                ? field.schema.validationMessage
                : (!result.message.empty() ? result.message
                                           : result.errorCode);
        }
        else if (result.Succeeded())
        {
            field.draftError.clear();
        }
        RefreshFieldValidation(field);
    }

    void SetFieldEnabled(WidgetFieldControl& field, bool enabled)
    {
        if (field.text) field.text.IsEnabled(enabled);
        if (field.referenceText) field.referenceText.IsEnabled(enabled);
        if (field.password) field.password.IsEnabled(enabled);
        if (field.passwordReveal) RefreshPasswordReveal(field);
        if (field.toggle) field.toggle.IsEnabled(enabled);
        if (field.slider) field.slider.IsEnabled(enabled);
        if (field.number) field.number.IsEnabled(enabled);
        if (field.colorEditor)
            field.colorEditor->button.IsEnabled(enabled);
        if (field.select) field.select.IsEnabled(enabled);
        if (field.multiSelect) field.multiSelect.IsEnabled(enabled);
        for (auto& option : field.multiOptions)
            option.checkBox.IsEnabled(enabled);
        if (field.date) field.date.IsEnabled(enabled);
        if (field.time) field.time.IsEnabled(enabled);
        for (const auto& number : field.durationParts) if (number) number.IsEnabled(enabled);
        if (field.search) field.search.IsEnabled(enabled);
        if (field.searchResults) field.searchResults.IsEnabled(enabled);
        if (field.restoreDefault) field.restoreDefault.IsEnabled(enabled);
    }

    [[nodiscard]] bool CanMutateField(
        const WidgetFieldControl& field) const noexcept
    {
        return CanMutate() && field.visible && field.enabled &&
            !field.synchronizing;
    }

    [[nodiscard]] bool CanFinalizeField(
        const WidgetFieldControl& field) const noexcept
    {
        // A dependency mutation may hide a text editor before its LostFocus
        // event is delivered. Finalizing an existing draft is still safe
        // while the authoritative field remains enabled; visibility only
        // governs new user input.
        return CanMutate() && field.enabled && !field.synchronizing;
    }

    [[nodiscard]] std::string MutationCallbackKey(
        std::string_view owner) const
    {
        return owner.starts_with("__appearance.")
            ? std::string("__appearance")
            : std::string(owner);
    }

    void NotifyMutation(std::string_view owner,
        const wr::WidgetSettingMutationResult& result)
    {
        if (callbacks.mutationCompleted)
            callbacks.mutationCompleted(MutationCallbackKey(owner), result);
    }

    void ApplyCurrentSnapshot()
    {
        if (const auto current = service.Snapshot(widgetId))
            (void)ApplySnapshot(*current);
    }

    void ClearContinuousDirty(std::string_view owner)
    {
        const auto clearAppearance = [&](AppearanceScalarControl& control) {
            if (control.previewOwner != owner) return false;
            if (control.idleCommitTimer) control.idleCommitTimer.Stop();
            control.continuousDirty = false;
            return true;
        };
        if (clearAppearance(backgroundOpacity) ||
            clearAppearance(borderOpacity) ||
            clearAppearance(borderWidth) ||
            clearAppearance(edgeHighlightWidth) ||
            clearAppearance(edgeHighlightStrength) ||
            clearAppearance(gradientEndOpacity))
            return;
        const auto field = fieldsByKey.find(std::string(owner));
        if (field != fieldsByKey.end())
        {
            if (field->second->idleCommitTimer)
                field->second->idleCommitTimer.Stop();
            field->second->continuousDirty = false;
        }
    }

    void ScheduleTransientPreview(
        std::string owner, PendingTransientPreview preview)
    {
        preview.requestId = ++nextTransientPreviewRequestId;
        const std::uint64_t requestId = preview.requestId;
        pendingTransientPreviews.insert_or_assign(owner, std::move(preview));
        QueueServiceHint(dispatch,
            TransientPreviewHint{std::move(owner), requestId},
            [](WidgetSettingsDispatchBridge& value) -> auto& {
                return value.transientPreview;
            });
    }

    void QueueTransientOrdinary(
        std::string_view owner, wr::InteractionValue value)
    {
        PendingTransientPreview preview;
        preview.ordinary = std::move(value);
        ScheduleTransientPreview(std::string(owner), std::move(preview));
    }

    void QueueTransientAppearance(
        std::string_view owner, wr::WidgetHostAppearancePatch patch)
    {
        if (!patch.presetId && !patch.followPersonalization) patch.presetId = "__custom";
        PendingTransientPreview preview;
        preview.appearance = std::move(patch);
        ScheduleTransientPreview(std::string(owner), std::move(preview));
    }

    void QueueNumberPreview(WidgetFieldControl& field, double value)
    {
        if (!CanMutateField(field) || !std::isfinite(value)) return;
        field.continuousDirty = true;
        field.draftError.clear();
        RefreshFieldValidation(field);
        field.idleCommitTimer.Stop();
        field.idleCommitTimer.Start();
        wr::InteractionValue encoded =
            field.schema.Kind() == wr::WidgetSettingKind::Integer
            ? wr::MakeWidgetSettingInteger(
                static_cast<long long>(std::llround(value)))
            : wr::MakeWidgetSettingNumber(value);
        QueueTransientOrdinary(field.schema.key, std::move(encoded));
    }

    void QueueAppearancePreview(
        AppearanceScalarControl& control, float value)
    {
        control.continuousDirty = true;
        control.idleCommitTimer.Stop();
        control.idleCommitTimer.Start();
        wr::WidgetHostAppearancePatch patch;
        control.assign(patch, value);
        QueueTransientAppearance(control.previewOwner, std::move(patch));
    }

    wr::WidgetSettingMutationResult CommitActiveTransient()
    {
        if (!transientPreviewActive) return UnchangedResult();
        const std::string owner = transientPreviewOwner;
        const auto result = service.CommitPreview(Guard());
        if (result.Succeeded())
        {
            transientPreviewActive = false;
            transientPreviewOwner.clear();
            ClearContinuousDirty(owner);
            ApplyCurrentSnapshot();
        }
        NotifyMutation(owner, result);
        return result;
    }

    wr::WidgetSettingMutationResult PublishTransientPreview(
        const std::string& owner, const PendingTransientPreview& preview)
    {
        if (!CanMutate())
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "presenterInactive", {}};
        if (transientPreviewActive && transientPreviewOwner != owner)
        {
            const auto committed = CommitActiveTransient();
            if (!committed.Succeeded()) return committed;
        }

        wr::WidgetSettingMutationResult result;
        if (preview.ordinary)
            result = service.PreviewOrdinary(
                Guard(), owner, *preview.ordinary);
        else
            result = service.PreviewHostAppearance(
                Guard(), preview.appearance);
        if (result.Succeeded() ||
            result.status == wr::WidgetSettingMutationStatus::StaleSnapshot)
            ApplyCurrentSnapshot();
        if (result.Changed())
        {
            transientPreviewActive = true;
            transientPreviewOwner = owner;
        }
        if (const auto field = fieldsByKey.find(owner);
            field != fieldsByKey.end())
            SetDraftError(*field->second, result);
        NotifyMutation(owner, result);
        return result;
    }

    wr::WidgetSettingMutationResult FlushScheduledPreview(
        std::string_view owner)
    {
        const auto found = pendingTransientPreviews.find(std::string(owner));
        if (found == pendingTransientPreviews.end())
            return UnchangedResult();
        PendingTransientPreview preview = std::move(found->second);
        const std::string stableOwner = found->first;
        pendingTransientPreviews.erase(found);
        return PublishTransientPreview(stableOwner, preview);
    }

    void PublishQueuedPreview(TransientPreviewHint hint)
    {
        const auto found = pendingTransientPreviews.find(hint.owner);
        if (found == pendingTransientPreviews.end() ||
            found->second.requestId != hint.requestId)
            return;
        (void)FlushScheduledPreview(hint.owner);
    }

    wr::WidgetSettingMutationResult CommitTransientOwner(
        std::string_view owner)
    {
        const auto previewed = FlushScheduledPreview(owner);
        if (!previewed.Succeeded()) return previewed;
        if (transientPreviewActive && transientPreviewOwner == owner)
            return CommitActiveTransient();
        ClearContinuousDirty(owner);
        return previewed;
    }

    wr::WidgetSettingMutationResult CommitAllTransient()
    {
        while (!pendingTransientPreviews.empty())
        {
            const std::string owner =
                pendingTransientPreviews.begin()->first;
            const auto result = CommitTransientOwner(owner);
            if (!result.Succeeded()) return result;
        }
        return CommitActiveTransient();
    }

    template <typename Mutation>
    wr::WidgetSettingMutationResult RunMutation(
        std::string key,
        Mutation mutation,
        bool flushPendingEditors = true)
    {
        if (!CanMutate())
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "presenterInactive", {}};
        if (flushPendingEditors)
        {
            // Commit text and password drafts before a toggle, selection, or
            // reset can change showWhen/enabledWhen and hide their editors.
            const auto editorCommit = FlushPendingEdits();
            if (!editorCommit.Succeeded()) return editorCommit;
        }
        const auto previewCommit = CommitAllTransient();
        if (!previewCommit.Succeeded()) return previewCommit;
        const wr::WidgetSettingMutationGuard guard = Guard();
        wr::WidgetSettingMutationResult result = mutation(guard);
        if (result.status == wr::WidgetSettingMutationStatus::Applied ||
            result.status == wr::WidgetSettingMutationStatus::Unchanged ||
            result.status == wr::WidgetSettingMutationStatus::StaleSnapshot)
        {
            if (const auto current = service.Snapshot(widgetId))
                (void)ApplySnapshot(*current);
        }
        if (const auto field = fieldsByKey.find(key);
            field != fieldsByKey.end())
            SetDraftError(*field->second, result);
        if (callbacks.mutationCompleted)
            callbacks.mutationCompleted(std::move(key), result);
        return result;
    }

    [[nodiscard]] wr::WidgetSettingMutationResult UnchangedResult() const
    {
        return {wr::WidgetSettingMutationStatus::Unchanged,
            generation, revision, {}, {}};
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitText(
        WidgetFieldControl& field)
    {
        if (field.idleCommitTimer) field.idleCommitTimer.Stop();
        if (!field.textDirty)
            return UnchangedResult();
        if (!CanFinalizeField(field) || !field.text)
        {
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "editorUnavailable", {}};
        }
        const std::string key = field.schema.key;
        const std::string value = ToUtf8(field.text.Text());
        field.textDirty = false;
        const auto result = RunMutation(key,
            [this, key, value](const auto& guard) {
                return service.SetOrdinary(
                    guard, key, wr::MakeWidgetSettingString(value));
            }, false);
        if (!result.Succeeded())
        {
            if (const auto current = fieldsByKey.find(key);
                current != fieldsByKey.end() && current->second->text)
            {
                current->second->synchronizing = true;
                current->second->textObservedValue = ToText(value);
                if (current->second->text.Text() != current->second->textObservedValue)
                    current->second->text.Text(current->second->textObservedValue);
                current->second->synchronizing = false;
                current->second->textDirty = true;
            }
        }
        return result;
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitPassword(
        WidgetFieldControl& field)
    {
        if (field.idleCommitTimer) field.idleCommitTimer.Stop();
        if (field.password && !PasswordEditorHasFocus(field) &&
            !field.passwordFlyout.IsOpen())
            HidePasswordDraft(field);
        if (!field.passwordDirty)
        {
            if (field.password &&
                !PasswordEditorHasFocus(field) &&
                !field.passwordFlyout.IsOpen())
            {
                ClearPasswordDraft(field);
            }
            return UnchangedResult();
        }
        if (!CanFinalizeField(field) || !field.password)
        {
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "editorUnavailable", {}};
        }
        const std::string key = field.schema.key;
        std::string plaintext = ToUtf8(field.password.Password());
        field.passwordDirty = false;
        const auto result = RunMutation(key,
            [this, key, &plaintext](const auto& guard) {
                return service.SetSecret(guard, key, plaintext);
            }, false);
        if (const auto current = fieldsByKey.find(key);
            current != fieldsByKey.end() && current->second->password)
        {
            if (result.Succeeded())
            {
                // Keep the draft and reveal choice while the editor owns focus;
                // an idle save or Enter must not interrupt subsequent editing.
                if (!PasswordEditorHasFocus(*current->second) &&
                    !current->second->passwordFlyout.IsOpen())
                {
                    ClearPasswordDraft(*current->second);
                }
            }
            else
            {
                std::wstring restored = ToWide(plaintext);
                current->second->synchronizing = true;
                if (current->second->password.Password() != restored)
                    current->second->password.Password(restored);
                current->second->synchronizing = false;
                current->second->passwordDirty = true;
                RefreshPasswordReveal(*current->second);
                if (!restored.empty())
                {
                    SecureZeroMemory(restored.data(),
                        restored.size() * sizeof(wchar_t));
                }
            }
        }
        if (!plaintext.empty())
            SecureZeroMemory(plaintext.data(), plaintext.size());
        return result;
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitDuration(WidgetFieldControl& field)
    {
        if (field.idleCommitTimer) field.idleCommitTimer.Stop();
        if (!field.durationDirty) return UnchangedResult();
        if (!CanFinalizeField(field)) return {wr::WidgetSettingMutationStatus::Disabled, generation, revision, "editorUnavailable", {}};
        std::array<winrt::hstring, 3> draft;
        std::array<long long, 3> values{};
        bool valid = true;
        for (std::size_t index = 0; index < field.durationParts.size(); ++index)
            if (const auto number = field.durationParts[index])
            {
                draft[index] = field.durationInputs[index] ? field.durationInputs[index].Text() : number.Text();
                valid = valid && wr::ReadWidgetSettingInteger(ToUtf8(draft[index]), values[index]) &&
                    values[index] >= 0 && values[index] <= (index == 0 ? 99 : 59);
            }
        const int scale = wr::DurationSettingUnitSeconds(field.schema.durationUnit);
        const auto seconds = valid ? values[0] * 3600 + values[1] * 60 + values[2] : 0;
        const long long value = scale ? seconds / scale : 0;
        valid = valid && scale && seconds % scale == 0 && wr::IsValidDurationSettingValue(
            static_cast<double>(value), field.schema.durationUnit, field.schema.minimum, field.schema.maximum, field.schema.step);
        if (!valid)
        {
            const auto message = L("app.widget.duration_picker.invalid", L"Enter a valid duration within the allowed range");
            field.draftError = ToUtf8(winrt::hstring(message));
            RefreshFieldValidation(field);
            return {wr::WidgetSettingMutationStatus::InvalidValue, generation, revision, "invalidDuration", field.draftError};
        }
        const auto key = field.schema.key;
        field.durationDirty = false;
        const auto result = RunMutation(key, [this, key, value](const auto& guard) {
            return service.SetOrdinary(guard, key, wr::MakeWidgetSettingInteger(value));
        }, false);
        if (!result.Succeeded())
            if (const auto current = fieldsByKey.find(key); current != fieldsByKey.end())
            {
                auto& target = *current->second;
                target.synchronizing = true;
                for (std::size_t index = 0; index < target.durationParts.size(); ++index)
                    if (target.durationParts[index])
                    {
                        target.durationParts[index].Text(draft[index]);
                        target.durationObservedText[index] = draft[index];
                    }
                target.synchronizing = false;
                target.durationDirty = true;
            }
        return result;
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitNumber(
        WidgetFieldControl& field)
    {
        if (field.idleCommitTimer) field.idleCommitTimer.Stop();
        if (!field.continuousDirty) return UnchangedResult();
        return CommitTransientOwner(field.schema.key);
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitAppearanceScalar(
        AppearanceScalarControl& control)
    {
        if (control.idleCommitTimer) control.idleCommitTimer.Stop();
        if (!control.continuousDirty) return UnchangedResult();
        return CommitTransientOwner(control.previewOwner);
    }

    void CommitMultiSelect(WidgetFieldControl& field)
    {
        if (!CanMutateField(field)) return;
        std::vector<std::string> selected;
        for (const auto& option : field.multiOptions)
        {
            const auto checked = option.checkBox.IsChecked();
            if (checked && checked.Value())
                selected.push_back(option.value);
        }
        RunMutation(field.schema.key,
            [this, key = field.schema.key,
             selected = std::move(selected)](const auto& guard) {
                return service.SetOrdinary(guard, key,
                    wr::MakeWidgetSettingStringArray(selected));
            });
    }

    void ChooseOpaque(WidgetFieldControl& field)
    {
        if (!CanMutateField(field)) return;
        const auto kind = field.schema.Kind();
        if (kind == wr::WidgetSettingKind::FileHandle ||
            kind == wr::WidgetSettingKind::FolderHandle)
        {
            RunMutation(field.schema.key,
                [this, key = field.schema.key](const auto& guard) {
                    return service.ChooseFilesystemHandle(guard, key);
                });
            return;
        }
        RunMutation(field.schema.key,
            [this, key = field.schema.key](const auto& guard) {
                return service.OpenEntityReferencePicker(guard, key);
            });
    }

    void ClearOpaque(WidgetFieldControl& field)
    {
        if (!CanMutateField(field)) return;
        if (field.date || field.time)
        {
            if (!field.schema.required)
                RunMutation(field.schema.key, [this, key = field.schema.key](const auto& guard) {
                    return service.SetOrdinary(guard, key, wr::MakeWidgetSettingString({}));
                });
            return;
        }
        const std::string key = field.schema.key;
        const bool password = static_cast<bool>(field.password);
        if (password && field.idleCommitTimer) field.idleCommitTimer.Stop();
        // Clearing a secret discards its draft instead of saving it first.
        const auto result = RunMutation(key,
            [this, key](const auto& guard) {
                return service.ClearOpaque(guard, key);
            }, !password);
        if (password && result.Succeeded())
            if (const auto current = fieldsByKey.find(key);
                current != fieldsByKey.end() && current->second->password)
            {
                current->second->passwordDirty = false;
                ClearPasswordDraft(*current->second);
            }
    }

    void RestoreFieldDefault(WidgetFieldControl& field)
    {
        if (!field.restoreDefault || !CanMutateField(field)) return;
        if (field.text || field.durationParts[0])
        {
            field.idleCommitTimer.Stop();
            field.textDirty = false;
            field.durationDirty = false;
            field.draftError.clear();
        }
        const std::string key = field.schema.key;
        RunMutation(key, [this, key](const auto& guard) {
            return service.ResetField(guard, key);
        });
    }

    void BeginSearch(WidgetFieldControl& field, std::string query)
    {
        if (!CanMutateField(field)) return;
        const std::string key = field.schema.key;
        const auto kind = field.schema.Kind();
        const bool queryEmpty = query.empty();
        wr::WidgetSettingMutationResult queryResult = UnchangedResult();
        if (kind == wr::WidgetSettingKind::AppSearch)
        {
            queryResult = service.SetSearchQuery(
                Guard(), key, query);
            if (queryResult.status ==
                    wr::WidgetSettingMutationStatus::Applied ||
                queryResult.status ==
                    wr::WidgetSettingMutationStatus::Unchanged ||
                queryResult.status ==
                    wr::WidgetSettingMutationStatus::StaleSnapshot)
            {
                if (const auto current = service.Snapshot(widgetId))
                    (void)ApplySnapshot(*current);
            }
            if (!queryResult.Succeeded())
            {
                if (callbacks.mutationCompleted)
                    callbacks.mutationCompleted(key, queryResult);
                return;
            }
        }

        const auto current = fieldsByKey.find(key);
        if (current == fieldsByKey.end() || !current->second ||
            !current->second->search || !CanMutateField(*current->second))
        {
            if (callbacks.mutationCompleted && queryResult.Changed())
                callbacks.mutationCompleted(key, queryResult);
            return;
        }
        WidgetFieldControl& currentField = *current->second;
        wr::WidgetSettingMutationResult result;
        if (queryEmpty)
            result = service.CancelSearch(Guard(), key);
        else
            result = service.StartSearch(
                Guard(), key, std::move(query), 16);
        if (queryResult.Changed() &&
            result.status == wr::WidgetSettingMutationStatus::Unchanged)
            result = queryResult;
        if (queryEmpty)
            ClearSearch(currentField);
        else if (const auto search = service.SearchSnapshot(
                     widgetId, key))
            PatchSearch(currentField, *search);
        else
            ClearSearch(currentField);
        if (callbacks.mutationCompleted)
            callbacks.mutationCompleted(key, result);
    }

    void CommitSearchResult(
        WidgetFieldControl& field,
        std::size_t index)
    {
        if (!CanMutateField(field) || index >= field.resultValues.size() ||
            field.searchRequestId == 0)
            return;
        const std::string key = field.schema.key;
        const std::string resultId = field.resultValues[index].id;
        const std::uint64_t requestId = field.searchRequestId;
        field.synchronizing = true;
        field.searchResults.SelectedIndex(-1);
        field.synchronizing = false;
        const auto result = RunMutation(key,
            [this, key, requestId,
             resultId](const auto& guard) {
                return service.CommitSearchResult(
                    guard, key, requestId, resultId);
            });
        if (result.Succeeded())
            if (const auto current = fieldsByKey.find(key);
                current != fieldsByKey.end() && current->second->search)
            {
                ClearSearch(*current->second);
                // Enter commits while the input still has focus. Replace the
                // query with the selected name now that it is the sole value display.
                if (current->second->schema.Kind() == wr::WidgetSettingKind::AppReference ||
                    current->second->schema.Kind() == wr::WidgetSettingKind::AppSearch)
                {
                    if (const auto snapshot = service.Snapshot(widgetId))
                        for (const auto& state : snapshot->fields)
                            if (state.schema.key == key)
                            {
                                PatchField(*current->second, state, true);
                                break;
                            }
                }
            }
    }

    void ClearSearch(WidgetFieldControl& field)
    {
        field.synchronizing = true;
        field.searchRequestId = 0;
        field.resultValues.clear();
        field.searchResults.Items().Clear();
        field.searchResults.SelectedIndex(-1);
        field.searchResults.Visibility(mux::Visibility::Collapsed);
        field.searchProgress.IsActive(false);
        field.searchProgress.Visibility(mux::Visibility::Collapsed);
        field.searchError.Text(L"");
        field.searchError.Visibility(mux::Visibility::Collapsed);
        field.synchronizing = false;
    }

    void PatchSearch(
        WidgetFieldControl& field,
        const wr::WidgetSettingSearchSnapshot& snapshot)
    {
        if (snapshot.widgetId != widgetId ||
            snapshot.generation != generation ||
            snapshot.settingKey != field.schema.key ||
            snapshot.requestId < field.searchRequestId)
            return;
        field.synchronizing = true;
        field.searchRequestId = snapshot.requestId;
        field.searchProgress.IsActive(snapshot.pending);
        field.searchProgress.Visibility(snapshot.pending
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        std::wstring searchStatus;
        if (!snapshot.errorCode.empty())
            searchStatus = ToText(snapshot.errorCode);
        else if (snapshot.completed && snapshot.results.empty())
            searchStatus = field.schema.noResultsLabel.empty()
                ? L("app.nav.no_results", L"No results found")
                : std::wstring(ToText(field.schema.noResultsLabel));
        field.searchError.Text(searchStatus);
        field.searchError.Visibility(searchStatus.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        field.resultValues = snapshot.results;
        field.searchResults.Items().Clear();
        for (const auto& result : field.resultValues)
        {
            muxc::StackPanel item;
            item.Spacing(2.0);
            muxc::TextBlock title;
            title.Text(ToText(result.title));
            title.TextWrapping(mux::TextWrapping::Wrap);
            title.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            item.Children().Append(title);
            if (!result.source.empty())
            {
                muxc::TextBlock source;
                source.Text(ToText(result.source));
                source.TextWrapping(mux::TextWrapping::Wrap);
                source.Opacity(0.72);
                item.Children().Append(source);
            }
            field.searchResults.Items().Append(item);
        }
        field.searchResults.Visibility(field.resultValues.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        field.synchronizing = false;
    }

    void ApplySnapshotHint(wr::WidgetSettingsSnapshotChanged hint)
    {
        if (closed || !hasSnapshot || hint.widgetId != widgetId)
            return;
        const auto snapshot = service.Snapshot(hint.widgetId);
        if (!snapshot || snapshot->generation != hint.generation ||
            snapshot->revision != hint.revision)
            return;
        (void)ApplySnapshot(*snapshot);
    }

    void ApplySearchHint(wr::WidgetSettingSearchCompleted hint)
    {
        if (closed || !hasSnapshot || hint.widgetId != widgetId ||
            hint.generation != generation)
            return;
        const auto found = fieldsByKey.find(hint.settingKey);
        if (found == fieldsByKey.end()) return;
        const auto snapshot = service.SearchSnapshot(
            hint.widgetId, hint.settingKey);
        if (!snapshot || snapshot->generation != generation ||
            snapshot->requestId != hint.requestId)
            return;
        PatchSearch(*found->second, *snapshot);
    }

    void SetFieldLocalizedText(WidgetFieldControl& field)
    {
        field.row.SetText(
            ToWide(field.schema.label),
            ToWide(field.schema.description));
        if (field.search)
        {
            field.search.PlaceholderText(field.schema.emptyLabel.empty()
                ? L("app.settings.widget.search", L"Search")
                : std::wstring(ToText(field.schema.emptyLabel)));
            muxa::AutomationProperties::SetName(
                field.search, ToText(field.schema.label));
        }
        if (field.choose)
        {
            const auto chooseText =
                L("app.settings.widget.choose", L"Choose or replace");
            muxc::ToolTipService::SetToolTip(
                field.choose, winrt::box_value(chooseText));
            muxa::AutomationProperties::SetName(
                field.choose, chooseText);
        }
        if (field.clear)
        {
            const auto clearText =
                L("app.settings.widget.clear", L"Clear");
            muxc::ToolTipService::SetToolTip(
                field.clear, winrt::box_value(clearText));
            muxa::AutomationProperties::SetName(field.clear,
                clearText);
        }
        const auto name = ToText(field.schema.label);
        if (field.restoreDefault)
        {
            std::wstring restoreText =
                L("app.settings.restore_default", L"Restore Default");
            if (!name.empty())
            {
                restoreText += L": ";
                restoreText.append(name.c_str(), name.size());
            }
            presenter_controls::ConfigureRestoreDefaultButton(
                field.restoreDefault, restoreText);
        }
        if (field.text) muxa::AutomationProperties::SetName(field.text, name);
        if (field.password)
            muxa::AutomationProperties::SetName(field.password, name);
        if (field.toggle)
            muxa::AutomationProperties::SetName(field.toggle, name);
        if (field.slider)
            muxa::AutomationProperties::SetName(field.slider, name);
        if (field.number)
            muxa::AutomationProperties::SetName(field.number, name);
        if (field.colorEditor)
        {
            const auto cancelText = L("app.settings.cancel", L"Cancel");
            field.colorEditor->cancel.Content(
                winrt::box_value(cancelText));
            muxa::AutomationProperties::SetName(
                field.colorEditor->button, name);
            muxa::AutomationProperties::SetHelpText(
                field.colorEditor->button,
                ToText(field.schema.description));
            muxa::AutomationProperties::SetName(
                field.colorEditor->cancel, cancelText);
        }
        if (field.select)
            muxa::AutomationProperties::SetName(field.select, name);
        if (field.multiSelect)
            muxa::AutomationProperties::SetName(field.multiSelect, name);
        if (field.date)
            muxa::AutomationProperties::SetName(field.date, name);
        if (field.time)
            muxa::AutomationProperties::SetName(field.time, name);
        const std::array<std::wstring, 3> units{
            L("app.widget.time_picker.hour", L"Hour"), L("app.widget.time_picker.minute", L"Minute"),
            L("app.widget.duration_picker.second", L"Second")};
        for (std::size_t index = 0; index < field.durationParts.size(); ++index)
            if (const auto number = field.durationParts[index])
            {
                number.Header(winrt::box_value(units[index]));
                muxa::AutomationProperties::SetName(number, winrt::hstring{std::wstring{name} + L" " + units[index]});
            }
    }

    void RefreshLocalizedText()
    {
        if (closed) return;
        if (active && panelGradientEditor) panelGradientEditor->Flush();
        const bool oldUpdating = updatingControls;
        updatingControls = true;
        appearanceTitle.Text(L("app.settings.appearance", L"Appearance"));
        restoreBackgroundRow.SetText(L("appearance.restoreBackground", L"Restore panel defaults"), L("appearance.restoreBackground.description", L""));
        followGlobalRow.SetText(
            L("engine.editor.follow_global", L"Follow global settings"));
        appearanceThemeRow.SetText(
            L("app.settings.theme", L"Theme"));
        const auto cancelText = L("app.settings.cancel", L"Cancel");
        backgroundColorEditor->SetText(
            L("app.settings.bg_color", L"Background color"), {},
            cancelText);
        backgroundOpacity.row.SetText(
            L("app.settings.bg_opacity", L"Background opacity"));
        appearanceSections.RefreshLocalizedText([this](auto key) { return L(key, {}); });
        edgeLightEditor->RefreshLocalizedText();
        panelGradientEditor->RefreshLocalizedText();
        UpdateBackgroundControls(currentHostAppearance.panelGradient.enabled);
        borderColorEditor->SetText(
            L("app.settings.border_color", L"Border color"), {},
            cancelText);
        borderOpacity.row.SetText(
            L("app.settings.border_opacity", L"Border opacity"));
        borderWidth.row.SetText(
            L("app.settings.border_width", L"Border width"));
        edgeHighlightRow.SetText(
            L("app.settings.edge_highlight", L"Edge highlight"));
        edgeHighlightWidth.row.SetText(L(
            "app.settings.edge_highlight_width", L"Edge highlight width"));
        edgeHighlightStrength.row.SetText(L(
            "app.settings.edge_highlight_strength",
            L"Edge highlight strength"));
        gradientEndOpacity.row.SetText(L(
            "app.settings.gradient_end_alpha", L"Gradient end opacity"));
        glassRow.SetText(
            L("app.settings.glass_bg", L"Glass background"));
        acrylicRow.SetText(
            L("app.settings.acrylic_enable", L"Enable acrylic"));
        contentThemeRow.SetText(L(
            "app.settings.widget_content_theme", L"Widget content theme"));
        contentTheme.Items().Clear();
        contentTheme.Items().Append(winrt::box_value(
            L("appearance.lightText", L"Light text")));
        contentTheme.Items().Append(winrt::box_value(
            L("appearance.darkText", L"Dark text")));

        stylePreviewTitle.Text(
            L("widgetSettings.presets", L"Component presets"));
        presetRow.SetText(
            L("widgetSettings.preset", L"Preset"));
        restoreScriptDefaultRow.SetText(
            L("widgetSettings.restorePreset", L"Restore the component's default preset"));
        const auto restoreScriptDefaultText = L(
            "app.settings.restore_default", L"Restore default");
        restoreScriptDefault.Content(
            winrt::box_value(restoreScriptDefaultText));
        scriptSettingsTitle.Text(
            L("widgetSettings.content", L"Content and behavior"));
        resetRow.SetText(
            L("widgetSettings.restoreParameters", L"Restore default component parameters"),
            L("widgetSettings.restoreParametersHint", L"Restore this instance's ordinary parameters, including hidden ones. Keep secrets, authorized files and folders, object references, and appearance."));
        permissionsRow.SetText(
            L("widgetSettings.packagePermissions", L"Component permissions"),
            L("widgetSettings.packagePermissionsHint", L"Permission changes affect all instances of this component package."));
        relatedTitle.Text(L("settings.common.relatedSettings", L"Related settings"));
        permissionsButton.Content(winrt::box_value(
            L("app.settings.widgets_manage_permissions", L"Manage permissions")));
        muxa::AutomationProperties::SetName(permissionsButton, permissionsRow.label.Text());
        const auto resetText = L(
            "app.settings.restore_default_settings",
            L"Restore default settings");
        reset.Content(winrt::box_value(resetText));
        muxa::AutomationProperties::SetName(
            appearanceTitle, appearanceTitle.Text());
        muxa::AutomationProperties::SetName(
            followGlobal, followGlobalRow.label.Text());
        muxa::AutomationProperties::SetName(
            appearanceTheme, appearanceThemeRow.label.Text());
        muxa::AutomationProperties::SetName(
            backgroundOpacity.slider, backgroundOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            backgroundOpacity.number, backgroundOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            borderOpacity.slider, borderOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            borderOpacity.number, borderOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            borderWidth.slider, borderWidth.row.label.Text());
        muxa::AutomationProperties::SetName(
            borderWidth.number, borderWidth.row.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightEnabled, edgeHighlightRow.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightWidth.slider,
            edgeHighlightWidth.row.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightWidth.number,
            edgeHighlightWidth.row.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightStrength.slider,
            edgeHighlightStrength.row.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightStrength.number,
            edgeHighlightStrength.row.label.Text());
        muxa::AutomationProperties::SetName(
            gradientEndOpacity.slider,
            gradientEndOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            gradientEndOpacity.number,
            gradientEndOpacity.row.label.Text());
        muxa::AutomationProperties::SetName(
            glassEnabled, glassRow.label.Text());
        muxa::AutomationProperties::SetName(
            acrylicEnabled, acrylicRow.label.Text());
        muxa::AutomationProperties::SetName(
            contentTheme, contentThemeRow.label.Text());
        muxa::AutomationProperties::SetName(
            stylePreviewTitle, stylePreviewTitle.Text());
        muxa::AutomationProperties::SetName(
            presetCombo, presetRow.label.Text());
        muxa::AutomationProperties::SetName(
            restoreScriptDefault, restoreScriptDefaultText);
        muxa::AutomationProperties::SetName(
            scriptSettingsTitle, scriptSettingsTitle.Text());
        muxa::AutomationProperties::SetName(
            reset, resetText);

        const auto explain = [](auto& row, const std::wstring& text) { row.SetText(std::wstring(row.label.Text().c_str()), text); };
        explain(glassRow, L("appearance.glassHelp", L""));
        for (auto const& [row, button] : appearanceResets) presenter_controls::ConfigureRestoreDefaultButton(button, L("app.settings.restore_default", L"Restore default") + L" · " + std::wstring(row->label.Text()));
        RebuildPresetItems();
        for (auto& field : fields)
            SetFieldLocalizedText(*field);
        if (hasSnapshot)
        {
            if (const auto current = service.Snapshot(widgetId))
                PatchAppearance(*current);
        }
        updatingControls = oldUpdating;
    }

    void ReportDiagnostics()
    {
        if (!callbacks.diagnostic) return;
        for (const auto& field : fields)
        {
            const std::string code(field->schema.DiagnosticCode());
            if (!code.empty())
                callbacks.diagnostic(
                    widgetId, field->schema.key, code);
        }
    }

    [[nodiscard]] wr::WidgetSettingMutationResult FlushPendingEdits()
    {
        std::vector<std::string> keys;
        keys.reserve(fields.size());
        for (const auto& field : fields)
            if (field->textDirty || field->passwordDirty || field->durationDirty)
                keys.push_back(field->schema.key);
        if (closed || !active)
        {
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "presenterInactive", {}};
        }
        edgeLightEditor->Flush();
        panelGradientEditor->Flush();
        wr::WidgetSettingMutationResult aggregate = CommitAllTransient();
        if (!aggregate.Succeeded()) return aggregate;
        for (const auto& key : keys)
        {
            const auto found = fieldsByKey.find(key);
            if (found == fieldsByKey.end()) continue;
            auto result = CommitText(*found->second);
            if (!result.Succeeded())
                return result;
            if (result.Changed())
                aggregate = result;

            // Applying the first mutation may refresh the snapshot and its
            // control map. Re-resolve by stable schema key before touching the
            // secret editor.
            const auto current = fieldsByKey.find(key);
            if (current == fieldsByKey.end())
                continue;
            result = CommitPassword(*current->second);
            if (!result.Succeeded())
                return result;
            if (result.Changed())
                aggregate = result;
            const auto duration = fieldsByKey.find(key);
            if (duration != fieldsByKey.end())
            {
                result = CommitDuration(*duration->second);
                if (!result.Succeeded()) return result;
                if (result.Changed()) aggregate = result;
            }
        }
        return aggregate;
    }

    void CancelSearches() noexcept
    {
        if (!hasSnapshot || !active) return;
        try
        {
            for (auto& field : fields)
            {
                if (!field->search || field->searchRequestId == 0)
                    continue;
                (void)service.CancelSearch(Guard(), field->schema.key);
                ClearSearch(*field);
            }
        }
        catch (...)
        {
        }
    }

    void CommitOpenColorEditors() noexcept
    {
        try
        {
            const auto commitAppearance = [](auto& editor) {
                if (editor && editor->open)
                    editor->Dismiss();
            };
            commitAppearance(backgroundColorEditor);
            commitAppearance(borderColorEditor);
            for (auto& field : fields)
            {
                if (field->colorEditor && field->colorEditor->open)
                    field->colorEditor->Dismiss();
            }
        }
        catch (...)
        {
        }
    }

    void UnhookField(WidgetFieldControl& field) noexcept
    {
        if (field.colorEditor)
        {
            try
            {
                // Rebuild teardown must not mutate a possibly newer service
                // generation. Deactivate/Close explicitly commit while the
                // old identity and guard are still authoritative.
                field.colorEditor->changed = {};
                field.colorEditor->Dismiss();
                field.colorEditor->Close();
            }
            catch (...)
            {
            }
        }
        try
        {
            if (field.idleCommitTimer)
            {
                field.idleCommitTimer.Stop();
                if (HasToken(field.idleCommitTick))
                    field.idleCommitTimer.Tick(field.idleCommitTick);
            }
            if (field.text && HasToken(field.textChanged))
                field.text.TextChanged(field.textChanged);
            if (field.password && HasToken(field.passwordChanged))
                field.password.PasswordChanged(field.passwordChanged);
            if (field.password && HasToken(field.passwordGettingFocus))
                field.password.GettingFocus(field.passwordGettingFocus);
            if (field.password && HasToken(field.passwordPointerPressed))
                field.password.PointerPressed(field.passwordPointerPressed);
            if (field.passwordReveal)
            {
                if (HasToken(field.passwordRevealClicked))
                    field.passwordReveal.Checked(field.passwordRevealClicked);
                if (HasToken(field.passwordRevealUnchecked))
                    field.passwordReveal.Unchecked(field.passwordRevealUnchecked);
                if (HasToken(field.passwordRevealLostFocus))
                    field.passwordReveal.LostFocus(field.passwordRevealLostFocus);
                HidePasswordDraft(field);
                ClearPasswordDraft(field);
            }
            if (field.passwordFlyout)
            {
                if (HasToken(field.passwordFlyoutOpening))
                    field.passwordFlyout.Opening(field.passwordFlyoutOpening);
                if (HasToken(field.passwordFlyoutClosed))
                    field.passwordFlyout.Closed(field.passwordFlyoutClosed);
            }
            if (field.text && HasToken(field.lostFocus))
                field.text.LostFocus(field.lostFocus);
            if (field.password && HasToken(field.lostFocus))
                field.password.LostFocus(field.lostFocus);
            if (field.text && HasToken(field.keyDown))
                field.text.KeyDown(field.keyDown);
            if (field.password && HasToken(field.keyDown))
                field.password.PreviewKeyDown(field.keyDown);
            if (field.passwordFlyout) field.passwordFlyout.Hide();
            if (field.toggle && HasToken(field.toggled))
                field.toggle.Toggled(field.toggled);
            if (field.slider && HasToken(field.sliderChanged))
                field.slider.ValueChanged(field.sliderChanged);
            if (field.number && HasToken(field.numberChanged))
                field.number.ValueChanged(field.numberChanged);
            if (field.slider && HasToken(field.sliderPointerReleased))
                field.slider.PointerReleased(field.sliderPointerReleased);
            if (field.number && HasToken(field.numberPointerReleased))
                field.number.PointerReleased(field.numberPointerReleased);
            if (field.slider && HasToken(field.sliderLostFocus))
                field.slider.LostFocus(field.sliderLostFocus);
            if (field.number && HasToken(field.numberLostFocus))
                field.number.LostFocus(field.numberLostFocus);
            if (field.slider && HasToken(field.sliderKeyDown))
                field.slider.KeyDown(field.sliderKeyDown);
            if (field.number && HasToken(field.numberKeyDown))
                field.number.KeyDown(field.numberKeyDown);
            if (field.select && HasToken(field.selectionChanged))
                field.select.SelectionChanged(field.selectionChanged);
            for (auto& option : field.multiOptions)
            {
                if (option.checkBox && HasToken(option.checked))
                    option.checkBox.Checked(option.checked);
                if (option.checkBox && HasToken(option.unchecked))
                    option.checkBox.Unchecked(option.unchecked);
            }
            if (field.date && HasToken(field.dateChanged))
                field.date.DateChanged(field.dateChanged);
            if (field.time && HasToken(field.timeChanged))
                field.time.SelectedTimeChanged(field.timeChanged);
            for (std::size_t index = 0; index < field.durationParts.size(); ++index)
                if (const auto number = field.durationParts[index])
                {
                    if (field.durationInputs[index] && HasToken(field.durationTextTokens[index]))
                        field.durationInputs[index].TextChanged(field.durationTextTokens[index]);
                    if (HasToken(field.durationLoadedTokens[index])) number.Loaded(field.durationLoadedTokens[index]);
                    if (HasToken(field.durationKeyTokens[index])) number.KeyDown(field.durationKeyTokens[index]);
                }
            if (field.search && HasToken(field.searchTextChanged))
                field.search.TextChanged(field.searchTextChanged);
            if (field.search && HasToken(field.searchSubmitted))
                field.search.QuerySubmitted(field.searchSubmitted);
            if (field.search && HasToken(field.searchKeyDown))
                field.search.PreviewKeyDown(field.searchKeyDown);
            if (field.searchResults && HasToken(field.resultKeyDown))
                field.searchResults.KeyDown(field.resultKeyDown);
            if (field.searchResults &&
                HasToken(field.searchResultClicked))
                field.searchResults.ItemClick(field.searchResultClicked);
            if (field.choose && HasToken(field.chooseClicked))
                field.choose.Click(field.chooseClicked);
            if (field.clear && HasToken(field.clearClicked))
                field.clear.Click(field.clearClicked);
            if (field.restoreDefault &&
                HasToken(field.restoreDefaultClicked))
                field.restoreDefault.Click(field.restoreDefaultClicked);
        }
        catch (...)
        {
        }
    }

    void UnhookFields() noexcept
    {
        for (auto& field : fields)
            UnhookField(*field);
    }

    mux::FrameworkElement FocusTarget(
        std::string_view key, bool reveal,
        SettingsFocusPrerequisite* prerequisite) const noexcept
    {
        if (prerequisite) *prerequisite = {};
        try
        {
            const auto found = fieldsByKey.find(std::string(key));
            if (found == fieldsByKey.end()) return nullptr;
            const WidgetFieldControl* candidate = found->second;
            const bool requiresPrerequisite = !candidate->visible || !candidate->enabled;
            const FocusParent* resolvedParent = nullptr;
            std::unordered_set<std::string_view> visited;
            while ((!candidate->visible || !candidate->enabled) &&
                visited.emplace(candidate->schema.key).second)
            {
                const auto parent = focusParents.find(candidate->schema.key);
                if (parent == focusParents.end()) break;
                const auto control = fieldsByKey.find(parent->second.key);
                if (control == fieldsByKey.end() ||
                    visited.contains(control->second->schema.key)) break;
                resolvedParent = &parent->second;
                candidate = control->second;
            }
            const WidgetFieldControl& field = *candidate;
            if (prerequisite && requiresPrerequisite)
            {
                if (field.visible && field.enabled && resolvedParent &&
                    !field.schema.label.empty())
                    *prerequisite = {resolvedParent->messageKey, ToWide(field.schema.label)};
                else
                    prerequisite->messageKey = "settings.search.prerequisite.unavailable";
            }
            const auto group = std::find_if(groups.begin(), groups.end(),
                [&field](const auto& candidate) {
                    return candidate.schema.id == field.schema.group;
                });
            if (group != groups.end() &&
                group->root.Visibility() == mux::Visibility::Visible)
            {
                // Reveal the authored container before the shell waits for
                // layout. Revealing a group never changes its field values.
                if (reveal && group->expander) group->expander.IsExpanded(true);
                if (!field.visible)
                    return group->expander
                        ? group->expander.as<mux::FrameworkElement>()
                        : group->root.as<mux::FrameworkElement>();
            }
            if (!field.visible)
                return fieldsHost.Visibility() == mux::Visibility::Visible
                    ? scriptSettingsTitle.as<mux::FrameworkElement>()
                    : root.as<mux::FrameworkElement>();
            // No reachable editable controller (for example, an invalid or
            // cyclic authored dependency): still reveal the visible field row.
            if (!field.enabled) return field.root;
            if (field.text) return field.text;
            if (field.password) return field.password;
            if (field.toggle) return field.toggle;
            if (field.slider) return field.slider;
            if (field.colorEditor) return field.colorEditor->button;
            if (field.select) return field.select;
            if (field.multiSelect) return field.multiSelect;
            if (field.date) return field.date;
            if (field.time) return field.time;
            if (field.durationParts[0]) return field.durationParts[0];
            if (field.search) return field.search;
            if (field.choose) return field.choose;
            if (field.clear) return field.clear;
            return field.root;
        }
        catch (...) { return nullptr; }
    }

    void Close() noexcept
    {
        if (closed) return;
        try
        {
            CommitOpenColorEditors();
            (void)FlushPendingEdits();
            CancelSearches();
        }
        catch (...)
        {
        }
        active = false;
        closed = true;
        {
            std::lock_guard lock(dispatch->mutex);
            dispatch->closed = true;
            ++dispatch->epoch;
            dispatch->snapshot = {};
            dispatch->search = {};
            dispatch->transientPreview = {};
            dispatch->dispatcher = nullptr;
        }
        try
        {
            if (followGlobal && HasToken(followGlobalToggled))
                followGlobal.Toggled(followGlobalToggled);
            if (appearanceTheme && HasToken(appearanceThemeChanged))
                appearanceTheme.SelectionChanged(appearanceThemeChanged);
            if (edgeHighlightEnabled && HasToken(edgeHighlightToggled))
                edgeHighlightEnabled.Toggled(edgeHighlightToggled);
            if (glassEnabled && HasToken(glassToggled))
                glassEnabled.Toggled(glassToggled);
            if (acrylicEnabled && HasToken(acrylicToggled))
                acrylicEnabled.Toggled(acrylicToggled);
            if (contentTheme && HasToken(contentThemeChanged))
                contentTheme.SelectionChanged(contentThemeChanged);
            if (presetCombo && HasToken(presetChanged))
                presetCombo.SelectionChanged(presetChanged);
            if (restoreScriptDefault &&
                HasToken(restoreScriptDefaultClicked))
                restoreScriptDefault.Click(restoreScriptDefaultClicked);
            if (reset && HasToken(resetClicked))
                reset.Click(resetClicked);
            if (permissionsButton && HasToken(permissionsClicked))
                permissionsButton.Click(permissionsClicked);
            const auto unhookScalar = [](AppearanceScalarControl& scalar) {
                if (scalar.idleCommitTimer)
                {
                    scalar.idleCommitTimer.Stop();
                    if (HasToken(scalar.idleCommitTick))
                        scalar.idleCommitTimer.Tick(scalar.idleCommitTick);
                }
                if (scalar.slider && HasToken(scalar.sliderChanged))
                    scalar.slider.ValueChanged(scalar.sliderChanged);
                if (scalar.number && HasToken(scalar.numberChanged))
                    scalar.number.ValueChanged(scalar.numberChanged);
                if (scalar.slider &&
                    HasToken(scalar.sliderPointerReleased))
                    scalar.slider.PointerReleased(
                        scalar.sliderPointerReleased);
                if (scalar.number &&
                    HasToken(scalar.numberPointerReleased))
                    scalar.number.PointerReleased(
                        scalar.numberPointerReleased);
                if (scalar.slider && HasToken(scalar.sliderLostFocus))
                    scalar.slider.LostFocus(scalar.sliderLostFocus);
                if (scalar.number && HasToken(scalar.numberLostFocus))
                    scalar.number.LostFocus(scalar.numberLostFocus);
                if (scalar.slider && HasToken(scalar.sliderKeyDown))
                    scalar.slider.KeyDown(scalar.sliderKeyDown);
                if (scalar.number && HasToken(scalar.numberKeyDown))
                    scalar.number.KeyDown(scalar.numberKeyDown);
            };
            unhookScalar(backgroundOpacity);
            unhookScalar(borderOpacity);
            unhookScalar(borderWidth);
            unhookScalar(edgeHighlightWidth);
            unhookScalar(edgeHighlightStrength);
            unhookScalar(gradientEndOpacity);
            if (panelGradientEditor) panelGradientEditor->Close();
            if (edgeLightEditor) edgeLightEditor->Close();
            if (backgroundColorEditor)
            {
                backgroundColorEditor->changed = {};
                backgroundColorEditor->Dismiss();
                backgroundColorEditor->Close();
            }
            if (borderColorEditor)
            {
                borderColorEditor->changed = {};
                borderColorEditor->Dismiss();
                borderColorEditor->Close();
            }
        }
        catch (...)
        {
        }
        UnhookFields();
        callbacks = {};
        localize = {};
    }
};

WidgetSettingsPresenter::WidgetSettingsPresenter(
    wr::IWidgetSettingsService& service,
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(
          service, std::move(localize), cardStyle))
{
}

WidgetSettingsPresenter::~WidgetSettingsPresenter()
{
    Close();
}

void WidgetSettingsPresenter::SetCallbacks(
    WidgetSettingsPresenterCallbacks callbacks)
{
    if (!impl_ || impl_->closed) return;
    impl_->callbacks = std::move(callbacks);
    impl_->relatedCard.root.Visibility(
        impl_->hasSnapshot && !impl_->packageId.empty() && impl_->callbacks.openPackagePermissions
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    impl_->ReportDiagnostics();
}

mux::UIElement WidgetSettingsPresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

bool WidgetSettingsPresenter::ApplySnapshot(
    const wr::WidgetSettingsSnapshot& snapshot)
{
    return impl_ && impl_->ApplySnapshot(snapshot);
}

WidgetSettingsEventDispatchers
WidgetSettingsPresenter::EventDispatchers() const
{
    WidgetSettingsEventDispatchers result;
    if (!impl_ || impl_->closed) return result;
    const auto bridge = impl_->dispatch;
    result.snapshotChanged =
        [bridge](wr::WidgetSettingsSnapshotChanged hint) noexcept {
            QueueServiceHint(bridge, std::move(hint),
                [](WidgetSettingsDispatchBridge& value) -> auto& {
                    return value.snapshot;
                });
        };
    result.searchCompleted =
        [bridge](wr::WidgetSettingSearchCompleted hint) noexcept {
            QueueServiceHint(bridge, std::move(hint),
                [](WidgetSettingsDispatchBridge& value) -> auto& {
                    return value.search;
                });
        };
    return result;
}

void WidgetSettingsPresenter::RefreshLocalizedText()
{
    if (impl_) impl_->RefreshLocalizedText();
}

void WidgetSettingsPresenter::Activate() noexcept
{
    if (impl_ && !impl_->closed) impl_->active = true;
}

void WidgetSettingsPresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed) return;
    try
    {
        impl_->CommitOpenColorEditors();
        const auto result = impl_->FlushPendingEdits();
        if (!result.Succeeded())
        {
            // Leaving component settings must never strand an invisible,
            // still-active editor. Closing the failed session discards its
            // unsaved draft and reverts any transient preview; re-entry loads
            // a fresh authoritative snapshot.
            const std::wstring widgetId = impl_->widgetId;
            impl_->active = false;
            impl_->hasSnapshot = false;
            impl_->service.Close(widgetId);
            return;
        }
        impl_->CancelSearches();
    }
    catch (...)
    {
    }
    impl_->active = false;
}

wr::WidgetSettingMutationResult
WidgetSettingsPresenter::FlushPendingEdits()
{
    if (impl_) return impl_->FlushPendingEdits();
    return {wr::WidgetSettingMutationStatus::Unavailable,
        0, 0, "presenterUnavailable", {}};
}

mux::FrameworkElement WidgetSettingsPresenter::FocusTarget(
    std::string_view settingKey, bool reveal,
    SettingsFocusPrerequisite* prerequisite) const noexcept
{
    if (prerequisite) *prerequisite = {};
    return impl_ ? impl_->FocusTarget(settingKey, reveal, prerequisite) : nullptr;
}

std::wstring_view WidgetSettingsPresenter::WidgetId() const noexcept
{
    return impl_ ? std::wstring_view(impl_->widgetId)
                 : std::wstring_view{};
}

std::string_view WidgetSettingsPresenter::WidgetName() const noexcept
{
    return impl_ ? std::string_view(impl_->widgetName) : std::string_view{};
}

std::uint64_t WidgetSettingsPresenter::Generation() const noexcept
{
    return impl_ ? impl_->generation : 0;
}

std::uint64_t WidgetSettingsPresenter::Revision() const noexcept
{
    return impl_ ? impl_->revision : 0;
}

void WidgetSettingsPresenter::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui
