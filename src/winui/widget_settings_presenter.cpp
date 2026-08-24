#include "pch.h"

#include "widget_settings_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
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
    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(
        (encoded[0] - '0') * 1000 + (encoded[1] - '0') * 100 +
        (encoded[2] - '0') * 10 + (encoded[3] - '0'));
    systemTime.wMonth = static_cast<WORD>(
        (encoded[5] - '0') * 10 + (encoded[6] - '0'));
    systemTime.wDay = static_cast<WORD>(
        (encoded[8] - '0') * 10 + (encoded[9] - '0'));
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&systemTime, &fileTime))
        return std::nullopt;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return wf::DateTime{wf::TimeSpan{
        static_cast<std::int64_t>(ticks.QuadPart)}};
}

std::string FormatDate(const wf::DateTime& date) noexcept
{
    const auto rawTicks = static_cast<std::uint64_t>(
        date.time_since_epoch().count());
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = rawTicks;
    FILETIME fileTime{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME systemTime{};
    if (!FileTimeToSystemTime(&fileTime, &systemTime))
        return {};
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u",
        static_cast<unsigned>(systemTime.wYear),
        static_cast<unsigned>(systemTime.wMonth),
        static_cast<unsigned>(systemTime.wDay));
    return buffer;
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
    muxc::TextBlock label{nullptr};
    muxc::TextBlock description{nullptr};
    muxc::TextBlock validation{nullptr};
    muxc::TextBlock diagnostic{nullptr};

    muxc::TextBox text{nullptr};
    muxc::PasswordBox password{nullptr};
    muxc::ToggleSwitch toggle{nullptr};
    muxc::StackPanel numericEditors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::ColorPicker color{nullptr};
    muxc::ComboBox select{nullptr};
    muxc::ListView multiSelect{nullptr};
    std::vector<MultiSelectOptionControl> multiOptions;
    muxc::CalendarDatePicker date{nullptr};
    muxc::TimePicker time{nullptr};
    muxc::AutoSuggestBox search{nullptr};
    muxc::ProgressRing searchProgress{nullptr};
    muxc::TextBlock searchError{nullptr};
    muxc::ListView searchResults{nullptr};
    std::vector<wr::WidgetSettingSearchResult> resultValues;
    muxc::TextBlock opaqueValue{nullptr};
    muxc::StackPanel opaqueActions{nullptr};
    muxc::Button choose{nullptr};
    muxc::Button clear{nullptr};

    winrt::event_token textChanged{};
    winrt::event_token passwordChanged{};
    winrt::event_token lostFocus{};
    winrt::event_token keyDown{};
    winrt::event_token toggled{};
    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token colorChanged{};
    winrt::event_token selectionChanged{};
    winrt::event_token dateChanged{};
    winrt::event_token timeChanged{};
    winrt::event_token searchTextChanged{};
    winrt::event_token searchSubmitted{};
    winrt::event_token searchSelectionChanged{};
    winrt::event_token chooseClicked{};
    winrt::event_token clearClicked{};

    bool visible = true;
    bool enabled = true;
    bool textDirty = false;
    bool passwordDirty = false;
    bool synchronizing = false;
    std::uint64_t searchRequestId = 0;
};

struct WidgetGroupControl
{
    wr::WidgetSettingGroupSchema schema;
    muxc::Border root{nullptr};
    muxc::Expander expander{nullptr};
    muxc::StackPanel content{nullptr};
};

struct WidgetSettingsDispatchBridge
{
    std::recursive_mutex mutex;
    mud::DispatcherQueue dispatcher{nullptr};
    bool closed = false;
    std::uint64_t epoch = 1;
    std::function<void(wr::WidgetSettingsSnapshotChanged)> snapshot;
    std::function<void(wr::WidgetSettingSearchCompleted)> search;
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
    Impl(wr::WidgetSettingsService& settingsService,
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
        BuildStaticControls();
        HookStaticEvents();
        RefreshLocalizedText();
    }

    wr::WidgetSettingsService& service;
    LocalizeCallback localize;
    WidgetSettingsPresenterCallbacks callbacks;
    mux::Style cardStyle{nullptr};
    std::shared_ptr<WidgetSettingsDispatchBridge> dispatch;

    muxc::StackPanel root{nullptr};
    muxc::TextBlock widgetHeading{nullptr};
    SettingsCard commandsCard;
    muxc::ComboBox presetCombo{nullptr};
    muxc::Button applyPreset{nullptr};
    muxc::Button reset{nullptr};
    muxc::StackPanel fieldsHost{nullptr};

    std::vector<std::unique_ptr<WidgetFieldControl>> fields;
    std::vector<WidgetGroupControl> groups;
    std::unordered_map<std::string, WidgetFieldControl*> fieldsByKey;
    std::unordered_map<std::string, muxc::StackPanel> groupPanels;
    std::unordered_map<std::string, bool> rememberedGroupExpansion;

    std::vector<wr::WidgetSettingFieldSchema> cachedSchemas;
    std::vector<wr::WidgetSettingGroupSchema> cachedGroups;
    std::vector<wr::WidgetSettingPresetSchema> cachedPresets;
    std::string defaultPresetId;
    std::wstring widgetId;
    std::string widgetName;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool hasSnapshot = false;
    bool active = false;
    bool updatingControls = false;
    bool closed = false;

    winrt::event_token applyPresetClicked{};
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

    void BuildStaticControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        widgetHeading = muxc::TextBlock{};
        widgetHeading.FontSize(24.0);
        widgetHeading.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        widgetHeading.TextWrapping(mux::TextWrapping::Wrap);
        widgetHeading.Margin({4.0, 4.0, 4.0, 8.0});
        root.Children().Append(widgetHeading);

        InitializeCard(commandsCard);
        presetCombo = muxc::ComboBox{};
        presetCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presetCombo.MaxWidth(520.0);
        commandsCard.content.Children().Append(presetCombo);

        muxc::StackPanel buttons;
        buttons.Orientation(muxc::Orientation::Horizontal);
        buttons.Spacing(8.0);
        applyPreset = muxc::Button{};
        reset = muxc::Button{};
        buttons.Children().Append(applyPreset);
        buttons.Children().Append(reset);
        commandsCard.content.Children().Append(buttons);
        root.Children().Append(commandsCard.root);

        fieldsHost = muxc::StackPanel{};
        fieldsHost.Spacing(8.0);
        root.Children().Append(fieldsHost);
    }

    void HookStaticEvents()
    {
        applyPresetClicked = applyPreset.Click(
            [this](const auto&, const auto&) {
                if (!CanMutate()) return;
                const int index = presetCombo.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= cachedPresets.size())
                    return;
                const std::string presetId =
                    cachedPresets[static_cast<std::size_t>(index)].id;
                RunMutation({}, [this, presetId](const auto& guard) {
                    return service.ApplyPreset(guard, presetId);
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
            if (cardStyle) group.root.Style(cardStyle);
            group.content = muxc::StackPanel{};
            group.content.Spacing(8.0);

            if (schema.collapsible)
            {
                group.expander = muxc::Expander{};
                muxc::StackPanel header;
                header.Spacing(2.0);
                muxc::TextBlock title;
                title.Text(ToText(schema.label));
                title.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
                title.TextWrapping(mux::TextWrapping::Wrap);
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
                container.Spacing(8.0);
                muxc::TextBlock title;
                title.Text(ToText(schema.label));
                title.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
                title.TextWrapping(mux::TextWrapping::Wrap);
                container.Children().Append(title);
                if (!schema.description.empty())
                {
                    muxc::TextBlock description;
                    description.Text(ToText(schema.description));
                    description.Opacity(0.72);
                    description.TextWrapping(mux::TextWrapping::Wrap);
                    container.Children().Append(description);
                }
                container.Children().Append(group.content);
                group.root.Child(container);
            }
            muxa::AutomationProperties::SetName(
                group.root, ToText(schema.label));
            fieldsHost.Children().Append(group.root);
            groupPanels.emplace(schema.id, group.content);
            groups.push_back(std::move(group));
        }
    }

    void InitializeFieldCard(WidgetFieldControl& field)
    {
        field.root = muxc::Border{};
        if (cardStyle) field.root.Style(cardStyle);
        field.content = muxc::StackPanel{};
        field.content.Spacing(7.0);
        field.label = muxc::TextBlock{};
        field.label.Text(ToText(field.schema.label));
        field.label.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        field.label.TextWrapping(mux::TextWrapping::Wrap);
        field.content.Children().Append(field.label);

        field.description = muxc::TextBlock{};
        field.description.Text(ToText(field.schema.description));
        field.description.Opacity(0.72);
        field.description.TextWrapping(mux::TextWrapping::Wrap);
        field.description.Visibility(field.schema.description.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        field.content.Children().Append(field.description);
        field.root.Child(field.content);
        muxa::AutomationProperties::SetName(
            field.root, ToText(field.schema.label));
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
        field.text.Header(winrt::box_value(ToText(field.schema.label)));
        field.textChanged = field.text.TextChanged(
            [&field](const auto&, const auto&) {
                if (!field.synchronizing) field.textDirty = true;
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
        field.content.Children().Append(field.text);
    }

    void BuildPasswordEditor(WidgetFieldControl& field)
    {
        field.password = muxc::PasswordBox{};
        field.password.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        field.password.Header(winrt::box_value(ToText(field.schema.label)));
        field.password.PasswordRevealMode(
            muxc::PasswordRevealMode::Peek);
        field.passwordChanged = field.password.PasswordChanged(
            [&field](const auto&, const auto&) {
                if (!field.synchronizing) field.passwordDirty = true;
            });
        field.lostFocus = field.password.LostFocus(
            [this, &field](const auto&, const auto&) {
                (void)CommitPassword(field);
            });
        field.keyDown = field.password.KeyDown(
            [this, &field](const auto&,
                           const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args))
                {
                    (void)CommitPassword(field);
                    args.Handled(true);
                }
            });
        field.content.Children().Append(field.password);
        BuildOpaqueActions(field, false, true);
    }

    void BuildBooleanEditor(WidgetFieldControl& field)
    {
        field.toggle = muxc::ToggleSwitch{};
        field.toggle.Header(winrt::box_value(ToText(field.schema.label)));
        field.toggle.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
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
        field.content.Children().Append(field.toggle);
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

        field.numericEditors = muxc::StackPanel{};
        field.numericEditors.Orientation(muxc::Orientation::Horizontal);
        field.numericEditors.Spacing(12.0);
        field.slider = muxc::Slider{};
        field.slider.Minimum(minimum);
        field.slider.Maximum(maximum);
        field.slider.StepFrequency(step);
        field.slider.Width(340.0);
        field.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        field.number = muxc::NumberBox{};
        field.number.Minimum(minimum);
        field.number.Maximum(maximum);
        field.number.SmallChange(step);
        field.number.LargeChange(step * 5.0);
        field.number.Width(128.0);
        field.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        field.number.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        field.numericEditors.Children().Append(field.slider);
        field.numericEditors.Children().Append(field.number);
        field.content.Children().Append(field.numericEditors);

        field.sliderChanged = field.slider.ValueChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing || updatingControls) return;
                const double value = field.slider.Value();
                field.synchronizing = true;
                field.number.Value(value);
                field.synchronizing = false;
                CommitNumber(field, value);
            });
        field.numberChanged = field.number.ValueChanged(
            [this, &field](const auto&, const auto&) {
                if (field.synchronizing || updatingControls) return;
                const double value = field.number.Value();
                if (!std::isfinite(value)) return;
                field.synchronizing = true;
                field.slider.Value(value);
                field.synchronizing = false;
                CommitNumber(field, value);
            });
    }

    void BuildColorEditor(WidgetFieldControl& field)
    {
        field.color = muxc::ColorPicker{};
        field.color.IsAlphaEnabled(false);
        field.color.IsMoreButtonVisible(false);
        field.color.HorizontalAlignment(mux::HorizontalAlignment::Left);
        field.colorChanged = field.color.ColorChanged(
            [this, &field](const auto&, const auto&) {
                if (!CanMutateField(field)) return;
                const long long value = FromColor(field.color.Color());
                RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingInteger(value));
                    });
            });
        field.content.Children().Append(field.color);
    }

    void BuildSelectEditor(WidgetFieldControl& field)
    {
        field.select = muxc::ComboBox{};
        field.select.Header(winrt::box_value(ToText(field.schema.label)));
        field.select.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.select.MaxWidth(520.0);
        for (const auto& option : field.schema.options)
            field.select.Items().Append(winrt::box_value(ToText(
                option.label.empty() ? option.value : option.label)));
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
        field.content.Children().Append(field.select);
    }

    void BuildMultiSelectEditor(WidgetFieldControl& field)
    {
        field.multiSelect = muxc::ListView{};
        field.multiSelect.SelectionMode(
            muxc::ListViewSelectionMode::None);
        field.multiSelect.MaxHeight(320.0);
        for (const auto& option : field.schema.options)
        {
            MultiSelectOptionControl item;
            item.value = option.value;
            item.checkBox = muxc::CheckBox{};
            item.checkBox.Content(winrt::box_value(ToText(
                option.label.empty() ? option.value : option.label)));
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
        field.content.Children().Append(field.multiSelect);
    }

    void BuildDateEditor(WidgetFieldControl& field)
    {
        field.date = muxc::CalendarDatePicker{};
        field.date.Header(winrt::box_value(ToText(field.schema.label)));
        field.date.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.date.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        field.dateChanged = field.date.DateChanged(
            [this, &field](const muxc::CalendarDatePicker&,
                           const muxc::CalendarDatePickerDateChangedEventArgs& args) {
                if (!CanMutateField(field)) return;
                const auto selected = args.NewDate();
                const std::string value = selected
                    ? FormatDate(selected.Value())
                    : std::string{};
                RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingString(value));
                    });
            });
        field.content.Children().Append(field.date);
    }

    void BuildTimeEditor(WidgetFieldControl& field)
    {
        field.time = muxc::TimePicker{};
        field.time.Header(winrt::box_value(ToText(field.schema.label)));
        field.time.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.time.ClockIdentifier(L"24HourClock");
        field.time.MinuteIncrement(1);
        field.timeChanged = field.time.SelectedTimeChanged(
            [this, &field](const muxc::TimePicker&,
                           const muxc::TimePickerSelectedValueChangedEventArgs& args) {
                if (!CanMutateField(field)) return;
                const auto selected = args.NewTime();
                const std::string value = selected
                    ? FormatTime(selected.Value())
                    : std::string{};
                RunMutation(field.schema.key,
                    [this, key = field.schema.key, value](const auto& guard) {
                        return service.SetOrdinary(guard, key,
                            wr::MakeWidgetSettingString(value));
                    });
            });
        field.content.Children().Append(field.time);
    }

    void BuildSearchEditor(WidgetFieldControl& field)
    {
        field.search = muxc::AutoSuggestBox{};
        field.search.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        field.search.MaxWidth(620.0);
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
                    CommitSearchResult(field, 0);
                else
                    BeginSearch(field, ToUtf8(field.search.Text()));
            });
        field.searchSelectionChanged =
            field.searchResults.SelectionChanged(
                [this, &field](const auto&, const auto&) {
                    if (updatingControls || field.synchronizing) return;
                    const int index = field.searchResults.SelectedIndex();
                    if (index >= 0)
                        CommitSearchResult(
                            field, static_cast<std::size_t>(index));
                });
        field.content.Children().Append(field.search);
        field.content.Children().Append(field.searchProgress);
        field.content.Children().Append(field.searchError);
        field.content.Children().Append(field.searchResults);

        if (field.schema.Kind() == wr::WidgetSettingKind::AppReference)
            BuildOpaqueActions(field, true, true);
    }

    void BuildOpaqueActions(
        WidgetFieldControl& field,
        bool allowChoose,
        bool allowClear)
    {
        field.opaqueValue = muxc::TextBlock{};
        field.opaqueValue.TextWrapping(mux::TextWrapping::Wrap);
        field.content.Children().Append(field.opaqueValue);
        field.opaqueActions = muxc::StackPanel{};
        field.opaqueActions.Orientation(muxc::Orientation::Horizontal);
        field.opaqueActions.Spacing(8.0);
        if (allowChoose)
        {
            field.choose = muxc::Button{};
            field.chooseClicked = field.choose.Click(
                [this, &field](const auto&, const auto&) {
                    ChooseOpaque(field);
                });
            field.opaqueActions.Children().Append(field.choose);
        }
        if (allowClear)
        {
            field.clear = muxc::Button{};
            field.clearClicked = field.clear.Click(
                [this, &field](const auto&, const auto&) {
                    ClearOpaque(field);
                });
            field.opaqueActions.Children().Append(field.clear);
        }
        field.content.Children().Append(field.opaqueActions);
    }

    void BuildReferenceEditor(WidgetFieldControl& field)
    {
        BuildOpaqueActions(field, true, true);
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

        auto ungrouped = muxc::StackPanel{};
        ungrouped.Spacing(8.0);
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
                ungrouped.Children().Append(field->root);
                hasUngrouped = true;
            }
            fieldsByKey.emplace(state.schema.key, pointer);
            fields.push_back(std::move(field));
        }
        if (hasUngrouped)
            fieldsHost.Children().Append(ungrouped);

        cachedSchemas.clear();
        cachedSchemas.reserve(snapshot.fields.size());
        for (const auto& field : snapshot.fields)
            cachedSchemas.push_back(field.schema);
        cachedGroups = snapshot.groups;
        cachedPresets = snapshot.presets;
        defaultPresetId = snapshot.defaultPresetId;
        RebuildPresetItems();
    }

    [[nodiscard]] bool SchemaChanged(
        const wr::WidgetSettingsSnapshot& snapshot) const
    {
        if (snapshot.fields.size() != cachedSchemas.size() ||
            snapshot.groups != cachedGroups ||
            snapshot.presets != cachedPresets ||
            snapshot.defaultPresetId != defaultPresetId)
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
        presetCombo.Items().Clear();
        int selected = -1;
        for (std::size_t index = 0; index < cachedPresets.size(); ++index)
        {
            const auto& preset = cachedPresets[index];
            presetCombo.Items().Append(winrt::box_value(ToText(
                preset.label.empty() ? preset.id : preset.label)));
            if ((!defaultPresetId.empty() &&
                    preset.id == defaultPresetId) ||
                (defaultPresetId.empty() && preset.isDefault))
                selected = static_cast<int>(index);
        }
        if (selected < 0 && !cachedPresets.empty()) selected = 0;
        presetCombo.SelectedIndex(selected);
        presetCombo.IsEnabled(!cachedPresets.empty());
        applyPreset.IsEnabled(!cachedPresets.empty());
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
        const bool oldUpdating = updatingControls;
        updatingControls = true;
        widgetId = snapshot.widgetId;
        widgetName = snapshot.widgetName;
        generation = snapshot.generation;
        revision = snapshot.revision;
        hasSnapshot = true;
        if (rebuild) RebuildFields(snapshot, !newIdentity);
        PatchFields(snapshot);
        commandsCard.root.Visibility(mux::Visibility::Visible);
        widgetHeading.Text(ToText(widgetName));
        muxa::AutomationProperties::SetName(
            widgetHeading, ToText(widgetName));
        updatingControls = oldUpdating;
        if (rebuild) ReportDiagnostics();
        return true;
    }

    void PatchFields(const wr::WidgetSettingsSnapshot& snapshot)
    {
        for (const auto& state : snapshot.fields)
        {
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
        }
    }

    void PatchField(
        WidgetFieldControl& field,
        const wr::WidgetSettingFieldState& state)
    {
        field.visible = state.visible;
        field.enabled = state.enabled;
        field.root.Visibility(state.visible
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        field.root.Opacity(state.enabled ? 1.0 : 0.58);
        SetFieldEnabled(field, state.enabled);

        if (field.text && !field.textDirty)
        {
            field.synchronizing = true;
            field.text.Text(ToText(StringValue(state.currentValue)));
            field.synchronizing = false;
        }
        if (field.password)
        {
            if (!field.passwordDirty)
            {
                field.synchronizing = true;
                field.password.Password(L"");
                field.synchronizing = false;
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
            field.number.Value(value);
            field.synchronizing = false;
        }
        if (field.color)
        {
            const long long value = state.currentValue.type ==
                    wr::InteractionValue::Type::Integer
                ? state.currentValue.integer
                : 0;
            field.color.Color(ToColor(value));
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
            const auto date = ParseDate(StringValue(state.currentValue));
            field.date.Date(date
                ? winrt::box_value(*date).as<wf::IReference<wf::DateTime>>()
                : nullptr);
            field.synchronizing = false;
        }
        if (field.time)
        {
            field.synchronizing = true;
            const auto time = ParseTime(StringValue(state.currentValue));
            field.time.SelectedTime(time
                ? winrt::box_value(*time).as<wf::IReference<wf::TimeSpan>>()
                : nullptr);
            field.synchronizing = false;
        }
        if (field.search &&
            field.search.FocusState() == mux::FocusState::Unfocused)
        {
            field.synchronizing = true;
            if (state.schema.Kind() == wr::WidgetSettingKind::AppReference)
                field.search.Text(ToText(state.opaque.displayLabel));
            else
                field.search.Text(ToText(StringValue(state.currentValue)));
            field.synchronizing = false;
        }
        if (field.opaqueValue)
        {
            std::wstring value;
            if (state.opaque.configured)
                value = state.opaque.displayLabel.empty()
                    ? L("app.settings.widget.configured", L"Configured")
                    : std::wstring(ToText(state.opaque.displayLabel));
            else
                value = state.schema.emptyLabel.empty()
                    ? L("app.settings.widget.not_configured",
                        L"Not configured")
                    : std::wstring(ToText(state.schema.emptyLabel));
            field.opaqueValue.Text(value);
        }
        if (field.choose)
            field.choose.IsEnabled(state.enabled && state.opaque.canChoose);
        if (field.clear)
            field.clear.IsEnabled(state.enabled && state.opaque.canClear);

        const std::string validation = state.schema.validationMessage.empty()
            ? state.validationError
            : state.schema.validationMessage;
        field.validation.Text(ToText(validation));
        field.validation.Visibility(!state.valid && !validation.empty()
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        field.diagnostic.Text(state.diagnosticCode.empty()
            ? winrt::hstring{}
            : winrt::hstring(L("app.settings.widget.unknown_type",
                L"Unknown setting type; using a text field.")));
        field.diagnostic.Visibility(state.diagnosticCode.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);

        const std::wstring status = !state.valid
            ? std::wstring(field.validation.Text())
            : std::wstring(field.diagnostic.Text());
        muxa::AutomationProperties::SetItemStatus(field.root, status);
    }

    void SetFieldEnabled(WidgetFieldControl& field, bool enabled)
    {
        if (field.text) field.text.IsEnabled(enabled);
        if (field.password) field.password.IsEnabled(enabled);
        if (field.toggle) field.toggle.IsEnabled(enabled);
        if (field.slider) field.slider.IsEnabled(enabled);
        if (field.number) field.number.IsEnabled(enabled);
        if (field.color) field.color.IsEnabled(enabled);
        if (field.select) field.select.IsEnabled(enabled);
        if (field.multiSelect) field.multiSelect.IsEnabled(enabled);
        for (auto& option : field.multiOptions)
            option.checkBox.IsEnabled(enabled);
        if (field.date) field.date.IsEnabled(enabled);
        if (field.time) field.time.IsEnabled(enabled);
        if (field.search) field.search.IsEnabled(enabled);
        if (field.searchResults) field.searchResults.IsEnabled(enabled);
    }

    [[nodiscard]] bool CanMutateField(
        const WidgetFieldControl& field) const noexcept
    {
        return CanMutate() && field.visible && field.enabled &&
            !field.synchronizing;
    }

    template <typename Mutation>
    wr::WidgetSettingMutationResult RunMutation(
        std::string key,
        Mutation mutation)
    {
        if (!CanMutate())
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "presenterInactive", {}};
        const wr::WidgetSettingMutationGuard guard = Guard();
        wr::WidgetSettingMutationResult result = mutation(guard);
        if (result.status == wr::WidgetSettingMutationStatus::Applied ||
            result.status == wr::WidgetSettingMutationStatus::Unchanged ||
            result.status == wr::WidgetSettingMutationStatus::StaleSnapshot)
        {
            if (const auto current = service.Snapshot(widgetId))
                (void)ApplySnapshot(*current);
        }
        if (callbacks.mutationCompleted)
            callbacks.mutationCompleted(key, result);
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
        if (!field.textDirty)
            return UnchangedResult();
        if (!CanMutateField(field) || !field.text)
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
            });
        if (!result.Succeeded())
        {
            if (const auto current = fieldsByKey.find(key);
                current != fieldsByKey.end() && current->second->text)
            {
                current->second->synchronizing = true;
                current->second->text.Text(ToWide(value));
                current->second->synchronizing = false;
                current->second->textDirty = true;
            }
        }
        return result;
    }

    [[nodiscard]] wr::WidgetSettingMutationResult CommitPassword(
        WidgetFieldControl& field)
    {
        if (!field.passwordDirty)
            return UnchangedResult();
        if (!CanMutateField(field) || !field.password)
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
            });
        if (const auto current = fieldsByKey.find(key);
            current != fieldsByKey.end() && current->second->password)
        {
            if (result.Succeeded())
            {
                current->second->synchronizing = true;
                current->second->password.Password(L"");
                current->second->synchronizing = false;
            }
            else
            {
                std::wstring restored = ToWide(plaintext);
                current->second->synchronizing = true;
                current->second->password.Password(restored);
                current->second->synchronizing = false;
                current->second->passwordDirty = true;
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

    void CommitNumber(WidgetFieldControl& field, double value)
    {
        if (!CanMutateField(field) || !std::isfinite(value)) return;
        wr::InteractionValue encoded;
        if (field.schema.Kind() == wr::WidgetSettingKind::Integer)
            encoded = wr::MakeWidgetSettingInteger(
                static_cast<long long>(std::llround(value)));
        else
            encoded = wr::MakeWidgetSettingNumber(value);
        RunMutation(field.schema.key,
            [this, key = field.schema.key,
             value = std::move(encoded)](const auto& guard) {
                return service.SetOrdinary(guard, key, value);
            });
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
        RunMutation(field.schema.key,
            [this, key = field.schema.key](const auto& guard) {
                return service.ClearOpaque(guard, key);
            });
    }

    void BeginSearch(WidgetFieldControl& field, std::string query)
    {
        if (!CanMutateField(field)) return;
        const auto guard = Guard();
        wr::WidgetSettingMutationResult result;
        if (query.empty())
            result = service.CancelSearch(guard, field.schema.key);
        else
            result = service.StartSearch(
                guard, field.schema.key, std::move(query), 16);
        if (const auto search = service.SearchSnapshot(
                widgetId, field.schema.key))
            PatchSearch(field, *search);
        else
            ClearSearch(field);
        if (callbacks.mutationCompleted)
            callbacks.mutationCompleted(field.schema.key, result);
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
                ClearSearch(*current->second);
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
        field.searchError.Text(ToText(snapshot.errorCode));
        field.searchError.Visibility(snapshot.errorCode.empty()
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
            title.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            item.Children().Append(title);
            if (!result.source.empty())
            {
                muxc::TextBlock source;
                source.Text(ToText(result.source));
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
            field.choose.Content(winrt::box_value(
                L("app.settings.widget.choose", L"Choose or replace")));
            muxa::AutomationProperties::SetName(
                field.choose,
                L("app.settings.widget.choose", L"Choose or replace"));
        }
        if (field.clear)
        {
            field.clear.Content(winrt::box_value(
                L("app.settings.widget.clear", L"Clear")));
            muxa::AutomationProperties::SetName(field.clear,
                L("app.settings.widget.clear", L"Clear"));
        }
        const auto name = ToText(field.schema.label);
        if (field.text) muxa::AutomationProperties::SetName(field.text, name);
        if (field.password)
            muxa::AutomationProperties::SetName(field.password, name);
        if (field.toggle)
            muxa::AutomationProperties::SetName(field.toggle, name);
        if (field.slider)
            muxa::AutomationProperties::SetName(field.slider, name);
        if (field.number)
            muxa::AutomationProperties::SetName(field.number, name);
        if (field.color)
            muxa::AutomationProperties::SetName(field.color, name);
        if (field.select)
            muxa::AutomationProperties::SetName(field.select, name);
        if (field.multiSelect)
            muxa::AutomationProperties::SetName(field.multiSelect, name);
        if (field.date)
            muxa::AutomationProperties::SetName(field.date, name);
        if (field.time)
            muxa::AutomationProperties::SetName(field.time, name);
    }

    void RefreshLocalizedText()
    {
        if (closed) return;
        commandsCard.root.Visibility(cachedPresets.empty() &&
                !hasSnapshot
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        presetCombo.Header(winrt::box_value(
            L("app.settings.widget.preset", L"Preset")));
        applyPreset.Content(winrt::box_value(
            L("app.settings.widget.apply_preset", L"Apply preset")));
        reset.Content(winrt::box_value(
            L("app.settings.widget.reset", L"Reset ordinary settings")));
        muxa::AutomationProperties::SetName(presetCombo,
            L("app.settings.widget.preset", L"Preset"));
        muxa::AutomationProperties::SetName(applyPreset,
            L("app.settings.widget.apply_preset", L"Apply preset"));
        muxa::AutomationProperties::SetName(reset,
            L("app.settings.widget.reset", L"Reset ordinary settings"));
        for (auto& field : fields)
            SetFieldLocalizedText(*field);
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
            if (field->textDirty || field->passwordDirty)
                keys.push_back(field->schema.key);
        if (keys.empty())
            return UnchangedResult();
        if (closed || !active)
        {
            return {wr::WidgetSettingMutationStatus::Disabled,
                generation, revision, "presenterInactive", {}};
        }

        wr::WidgetSettingMutationResult aggregate = UnchangedResult();
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

    void UnhookField(WidgetFieldControl& field) noexcept
    {
        try
        {
            if (field.text && HasToken(field.textChanged))
                field.text.TextChanged(field.textChanged);
            if (field.password && HasToken(field.passwordChanged))
                field.password.PasswordChanged(field.passwordChanged);
            if (field.text && HasToken(field.lostFocus))
                field.text.LostFocus(field.lostFocus);
            if (field.password && HasToken(field.lostFocus))
                field.password.LostFocus(field.lostFocus);
            if (field.text && HasToken(field.keyDown))
                field.text.KeyDown(field.keyDown);
            if (field.password && HasToken(field.keyDown))
                field.password.KeyDown(field.keyDown);
            if (field.toggle && HasToken(field.toggled))
                field.toggle.Toggled(field.toggled);
            if (field.slider && HasToken(field.sliderChanged))
                field.slider.ValueChanged(field.sliderChanged);
            if (field.number && HasToken(field.numberChanged))
                field.number.ValueChanged(field.numberChanged);
            if (field.color && HasToken(field.colorChanged))
                field.color.ColorChanged(field.colorChanged);
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
            if (field.search && HasToken(field.searchTextChanged))
                field.search.TextChanged(field.searchTextChanged);
            if (field.search && HasToken(field.searchSubmitted))
                field.search.QuerySubmitted(field.searchSubmitted);
            if (field.searchResults &&
                HasToken(field.searchSelectionChanged))
                field.searchResults.SelectionChanged(
                    field.searchSelectionChanged);
            if (field.choose && HasToken(field.chooseClicked))
                field.choose.Click(field.chooseClicked);
            if (field.clear && HasToken(field.clearClicked))
                field.clear.Click(field.clearClicked);
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
        std::string_view key) const noexcept
    {
        const auto found = fieldsByKey.find(std::string(key));
        if (found == fieldsByKey.end()) return nullptr;
        const WidgetFieldControl& field = *found->second;
        if (field.text) return field.text;
        if (field.password) return field.password;
        if (field.toggle) return field.toggle;
        if (field.slider) return field.slider;
        if (field.color) return field.color;
        if (field.select) return field.select;
        if (field.multiSelect) return field.multiSelect;
        if (field.date) return field.date;
        if (field.time) return field.time;
        if (field.search) return field.search;
        if (field.choose) return field.choose;
        if (field.clear) return field.clear;
        return field.root;
    }

    void Close() noexcept
    {
        if (closed) return;
        try
        {
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
            dispatch->dispatcher = nullptr;
        }
        try
        {
            if (applyPreset && HasToken(applyPresetClicked))
                applyPreset.Click(applyPresetClicked);
            if (reset && HasToken(resetClicked))
                reset.Click(resetClicked);
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
    wr::WidgetSettingsService& service,
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
        const auto result = impl_->FlushPendingEdits();
        if (!result.Succeeded())
            return;
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
    std::string_view settingKey) const noexcept
{
    return impl_ ? impl_->FocusTarget(settingKey) : nullptr;
}

std::wstring_view WidgetSettingsPresenter::WidgetId() const noexcept
{
    return impl_ ? std::wstring_view(impl_->widgetId)
                 : std::wstring_view{};
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
