#include "pch.h"
#include "animation_performance_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../animation_settings.h"
#include "../dock_genie_rules.h"
#include "../dock_launch_animation.h"
#include "../dock_magnification.h"
#include "../popup_animation_rules.h"
#include "../l10n.h"

#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
using presenter_controls::SettingRow;
using Clock = std::chrono::steady_clock;

namespace
{
struct Card
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    void Initialize(const mux::Style& style, const muxc::StackPanel& parent)
    {
        root = muxc::Border{};
        root.Style(style);
        content = muxc::StackPanel{};
        content.Spacing(16);
        title = muxc::TextBlock{};
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(mux::TextWrapping::Wrap);
        content.Children().Append(title);
        root.Child(content);
        parent.Children().Append(root);
    }
};

struct Choice
{
    SettingRow row;
    muxc::ComboBox combo{nullptr};
    muxc::Button reset{nullptr};
    std::string key;
    std::string focusId;
    std::vector<std::string> options;
    int defaultIndex = 0;
};

struct Switch
{
    SettingRow row;
    muxc::ToggleSwitch toggle{nullptr};
    muxc::Button reset{nullptr};
    std::string key;
    bool GeneralSettings::* member = nullptr;
    bool defaultValue = false;
};

muxc::Grid EditorWithReset(const mux::UIElement& editor, muxc::Button& reset)
{
    muxc::Grid grid{};
    grid.ColumnSpacing(8);
    muxc::ColumnDefinition main{};
    main.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
    muxc::ColumnDefinition trailing{};
    trailing.Width(mux::GridLengthHelper::Auto());
    grid.ColumnDefinitions().Append(main);
    grid.ColumnDefinitions().Append(trailing);
    grid.Children().Append(editor);
    reset = muxc::Button{};
    muxc::Grid::SetColumn(reset, 1);
    grid.Children().Append(reset);
    return grid;
}

muxc::Border PreviewSlice()
{
    // ThemeResource remains live across light, dark and contrast switches.
    return mux::Markup::XamlReader::Load(LR"(<Border
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        Width="150" Height="6.1"
        Background="{ThemeResource AccentFillColorDefaultBrush}"
        IsHitTestVisible="False" />)").as<muxc::Border>();
}
}

struct AnimationPerformancePagePresenter::Impl
{
    Impl(LocalizeCallback callback, const mux::Style& style, NavigateCallback navigate)
        : localize(std::move(callback)), navigate(std::move(navigate))
    {
        root = muxc::StackPanel{};
        root.Spacing(8);
        generalCard.Initialize(style, root);
        dockCard.Initialize(style, root);
        performanceCard.Initialize(style, root);
        AddChoice(mode, generalCard, "mode", "animation.mode", 0,
            {L10N_KEY("settings.animation.option.followSystem"),
             L10N_KEY("settings.animation.option.alwaysOn"),
             L10N_KEY("settings.animation.option.off")});
        AddChoice(popup, generalCard, "popup", "animation.popup", 2,
            {L10N_KEY("settings.animation.option.off"),
             L10N_KEY("settings.animation.option.fade"),
             L10N_KEY("settings.animation.option.scale")});
        AddChoice(speed, generalCard, "speed", "animation.speed", 1,
            {L10N_KEY("settings.animation.option.fast"),
             L10N_KEY("settings.animation.option.standard"),
             L10N_KEY("settings.animation.option.slow")});
        HookGeneral(mode, &GeneralSettings::animationMode);
        HookGeneral(popup, &GeneralSettings::popupAnimationEffect);
        HookGeneral(speed, &GeneralSettings::animationSpeed);

        dockNotice = muxc::TextBlock{};
        dockNotice.TextWrapping(mux::TextWrapping::Wrap);
        dockCard.content.Children().Append(dockNotice);
        dockLink = muxc::HyperlinkButton{};
        dockLink.HorizontalAlignment(mux::HorizontalAlignment::Left);
        dockCard.content.Children().Append(dockLink);
        const auto linkToken = dockLink.Click([this](const auto&, const auto&) {
            if (active && !closed && this->navigate)
                this->navigate(SettingsRoute::ForPage(SettingsPage::Dock, "dock.enable"));
        });
        revoke.push_back([control = dockLink, linkToken]() { control.Click(linkToken); });
        AddChoice(hover, dockCard, "hover", "animation.hover", 2,
            {L10N_KEY("settings.animation.option.noMagnification"),
             L10N_KEY("settings.animation.option.singleIcon"),
             L10N_KEY("settings.animation.option.wave")});
        HookDock(hover, &DockSettings::hoverEffect);

        hoverScale = muxc::Slider{};
        hoverScale.Minimum(100);
        hoverScale.Maximum(200);
        hoverScale.StepFrequency(1);
        hoverScale.SmallChange(1);
        hoverScale.LargeChange(10);
        hoverScale.Value(128);
        scaleRow.Initialize(EditorWithReset(hoverScale, scaleReset));
        dockCard.content.Children().Append(scaleRow.root);
        scalePreview.Initialize([this](double value) { PublishScale(value, SettingsUpdateMode::Preview); });
        scaleIdle = mux::DispatcherTimer{};
        scaleIdle.Interval(std::chrono::milliseconds(350));
        auto token = scaleIdle.Tick([this](const auto&, const auto&) { CommitScale(); });
        revoke.push_back([timer = scaleIdle, token]() { timer.Tick(token); });
        token = hoverScale.ValueChanged([this](const auto&, const auto&) {
            if (!CanEdit()) return;
            scaleDirty = true;
            scalePreview.Queue(hoverScale.Value());
            scaleIdle.Stop();
            scaleIdle.Start();
            UpdateHover(hoveredIcon);
        });
        revoke.push_back([control = hoverScale, token]() { control.ValueChanged(token); });
        token = hoverScale.PointerCaptureLost([this](const auto&, const auto&) { CommitScale(); });
        revoke.push_back([control = hoverScale, token]() { control.PointerCaptureLost(token); });
        token = hoverScale.LostFocus([this](const auto&, const auto&) { CommitScale(); });
        revoke.push_back([control = hoverScale, token]() { control.LostFocus(token); });
        token = scaleReset.Click([this](const auto&, const auto&) {
            if (!CanEdit()) return;
            scalePreview.Cancel();
            scaleIdle.Stop();
            scaleDirty = false;
            PublishScale(128, SettingsUpdateMode::PreviewAndCommit);
        });
        revoke.push_back([control = scaleReset, token]() { control.Click(token); });
        AddChoice(launch, dockCard, "launch", "animation.launch", 1,
            {L10N_KEY("settings.animation.option.off"),
             L10N_KEY("settings.animation.option.bounce"),
             L10N_KEY("settings.animation.option.gentleScale")});
        AddChoice(window, dockCard, "window", "animation.window", 1,
            {L10N_KEY("settings.animation.option.systemDefault"),
             L10N_KEY("settings.animation.option.scale"),
             L10N_KEY("settings.animation.option.fade"),
             L10N_KEY("settings.animation.option.genie")});
        HookDock(launch, &DockSettings::launchEffect);
        HookDock(window, &DockSettings::windowEffect);
        AddChoice(frameLimit, performanceCard, "frameLimit", "animation.frameLimit", 0,
            {L10N_KEY("settings.animation.option.automatic"),
             L10N_KEY("settings.animation.option.fps30"),
             L10N_KEY("settings.animation.option.fps60"),
             L10N_KEY("settings.animation.option.fps120")});
        HookGeneral(frameLimit, &GeneralSettings::animationFrameLimit, true);
        AddSwitch(energySaver, "energySaver", &GeneralSettings::animationEnergySaver, true);
        AddSwitch(onBattery, "onBattery", &GeneralSettings::animationOnBattery, false);
        BuildPreview();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    NavigateCallback navigate;
    DockPageActions actions;
    muxc::StackPanel root{nullptr};
    Card generalCard, dockCard, performanceCard;
    Choice mode, popup, speed, hover, launch, window, frameLimit;
    Switch energySaver, onBattery;
    SettingRow scaleRow;
    muxc::Slider hoverScale{nullptr};
    muxc::Button scaleReset{nullptr};
    muxc::TextBlock dockNotice{nullptr}, previewTitle{nullptr}, previewHint{nullptr};
    muxc::HyperlinkButton dockLink{nullptr};
    muxc::Canvas previewCanvas{nullptr};
    std::array<muxc::Border, 12> slices;
    std::array<muxm::CompositeTransform, 12> sliceTransforms;
    std::array<muxm::MatrixTransform, 12> genieTransforms;
    std::array<muxc::Button, 5> previewIcons;
    std::array<muxm::CompositeTransform, 5> iconTransforms;
    std::array<muxc::Button, 3> playButtons;
    std::vector<std::function<void()>> revoke;
    presenter_controls::CoalescedPreviewTimer<double> scalePreview;
    mux::DispatcherTimer scaleIdle{nullptr}, previewTimer{nullptr};
    Clock::time_point previewStart{};
    int previewKind = -1, hoveredIcon = -1;
    bool dockEnabled = false, scaleDirty = false, updating = false;
    bool active = false, closed = false, hasSnapshot = false;
    std::uint64_t generation = 0, generalRevision = 0, dockRevision = 0;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    bool CanEdit() const { return active && hasSnapshot && !updating && !closed; }

    void AddChoice(Choice& choice, Card& card, std::string key, std::string focusId,
        int defaultIndex, std::initializer_list<std::string> options)
    {
        choice.key = "settings.animation." + key;
        choice.focusId = std::move(focusId);
        choice.defaultIndex = defaultIndex;
        choice.options.assign(options);
        choice.combo = muxc::ComboBox{};
        choice.combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        choice.row.Initialize(EditorWithReset(choice.combo, choice.reset));
        card.content.Children().Append(choice.row.root);
    }

    void HookGeneral(Choice& choice, int GeneralSettings::* member, bool frames = false)
    {
        const auto changed = [this, &choice, member, frames]() {
            if (!CanEdit() || choice.combo.SelectedIndex() < 0) return;
            const int index = choice.combo.SelectedIndex();
            const int value = frames ? std::array{0, 30, 60, 120}[index] : index;
            UpdateEnabled();
            if (actions.updateGeneral)
                actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit,
                    [member, value](GeneralSettings& settings) { settings.*member = value; });
        };
        auto token = choice.combo.SelectionChanged([changed](const auto&, const auto&) { changed(); });
        revoke.push_back([control = choice.combo, token]() { control.SelectionChanged(token); });
        token = choice.reset.Click([this, &choice, member, frames](const auto&, const auto&) {
            if (!CanEdit() || !actions.updateGeneral) return;
            const int value = frames ? 0 : choice.defaultIndex;
            actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit,
                [member, value](GeneralSettings& settings) { settings.*member = value; });
        });
        revoke.push_back([control = choice.reset, token]() { control.Click(token); });
    }

    void HookDock(Choice& choice, int DockSettings::* member)
    {
        auto token = choice.combo.SelectionChanged([this, &choice, member](const auto&, const auto&) {
            if (!CanEdit() || choice.combo.SelectedIndex() < 0 || !actions.updateDock) return;
            const int value = choice.combo.SelectedIndex();
            UpdateEnabled();
            actions.updateDock(generation, SettingsUpdateMode::PreviewAndCommit,
                [member, value](DockSettings& settings) { settings.*member = value; });
        });
        revoke.push_back([control = choice.combo, token]() { control.SelectionChanged(token); });
        token = choice.reset.Click([this, &choice, member](const auto&, const auto&) {
            if (!CanEdit() || !actions.updateDock) return;
            const int value = choice.defaultIndex;
            actions.updateDock(generation, SettingsUpdateMode::PreviewAndCommit,
                [member, value](DockSettings& settings) { settings.*member = value; });
        });
        revoke.push_back([control = choice.reset, token]() { control.Click(token); });
    }

    void AddSwitch(Switch& control, std::string key,
        bool GeneralSettings::* member, bool defaultValue)
    {
        control.key = "settings.animation." + key;
        control.member = member;
        control.defaultValue = defaultValue;
        control.toggle = muxc::ToggleSwitch{};
        control.toggle.MinWidth(0);
        control.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        control.row.Initialize(EditorWithReset(control.toggle, control.reset));
        performanceCard.content.Children().Append(control.row.root);
        auto token = control.toggle.Toggled([this, &control](const auto&, const auto&) {
            if (!CanEdit() || !actions.updateGeneral) return;
            const bool value = control.toggle.IsOn();
            const auto member = control.member;
            actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit,
                [value, member](GeneralSettings& settings) { settings.*member = value; });
        });
        revoke.push_back([toggle = control.toggle, token]() { toggle.Toggled(token); });
        token = control.reset.Click([this, &control](const auto&, const auto&) {
            if (!CanEdit() || !actions.updateGeneral) return;
            const bool value = control.defaultValue;
            const auto member = control.member;
            actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit,
                [value, member](GeneralSettings& settings) { settings.*member = value; });
        });
        revoke.push_back([button = control.reset, token]() { button.Click(token); });
    }

    void PublishScale(double value, SettingsUpdateMode updateMode)
    {
        if (!CanEdit() || !actions.updateDock) return;
        const float scale = static_cast<float>(std::clamp(value, 100.0, 200.0) / 100.0);
        actions.updateDock(generation, updateMode,
            [scale](DockSettings& settings) { settings.hoverScale = scale; });
    }

    void CommitScale()
    {
        scaleIdle.Stop();
        scalePreview.Cancel();
        if (!scaleDirty) return;
        scaleDirty = false;
        PublishScale(hoverScale.Value(), SettingsUpdateMode::PreviewAndCommit);
    }

    bool AnimationsEnabled() const
    {
        return animation::ResolveEnabled(mode.combo.SelectedIndex(), animation::SystemAnimationsEnabled());
    }

    void UpdateEnabled()
    {
        const bool enabled = mode.combo.SelectedIndex() != animation::Disabled;
        popup.row.SetEnabled(enabled);
        speed.row.SetEnabled(enabled);
        for (auto* choice : {&hover, &launch, &window})
            choice->row.SetEnabled(enabled && dockEnabled);
        scaleRow.SetEnabled(enabled && dockEnabled && hover.combo.SelectedIndex() != 0);
        dockNotice.Visibility(dockEnabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        if (!AnimationsEnabled()) StopPreview();
        for (std::size_t i = 0; i < playButtons.size(); ++i)
            if (playButtons[i]) playButtons[i].IsEnabled(AnimationsEnabled() && (i == 0 || dockEnabled));
        for (const auto& icon : previewIcons)
            if (icon) icon.IsEnabled(enabled && dockEnabled);
        UpdateHover(hoveredIcon);
    }

    void BuildPreview()
    {
        previewTitle = muxc::TextBlock{};
        previewTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        dockCard.content.Children().Append(previewTitle);
        previewHint = muxc::TextBlock{};
        previewHint.TextWrapping(mux::TextWrapping::Wrap);
        dockCard.content.Children().Append(previewHint);
        previewCanvas = muxc::Canvas{};
        previewCanvas.Width(280);
        previewCanvas.Height(112);
        previewCanvas.HorizontalAlignment(mux::HorizontalAlignment::Center);
        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            slices[i] = PreviewSlice();
            sliceTransforms[i] = muxm::CompositeTransform{};
            genieTransforms[i] = muxm::MatrixTransform{};
            sliceTransforms[i].CenterX(75);
            slices[i].RenderTransform(sliceTransforms[i]);
            muxc::Canvas::SetLeft(slices[i], 65);
            muxc::Canvas::SetTop(slices[i], 12 + i * 6.0);
            previewCanvas.Children().Append(slices[i]);
        }
        dockCard.content.Children().Append(previewCanvas);
        muxc::StackPanel icons{};
        icons.Orientation(muxc::Orientation::Horizontal);
        icons.Spacing(12);
        icons.Margin({0, 16, 0, 16});
        icons.HorizontalAlignment(mux::HorizontalAlignment::Center);
        constexpr std::array glyphs{L"\xE8B7", L"\xE774", L"\xE8A5", L"\xE721", L"\xE713"};
        for (int i = 0; i < 5; ++i)
        {
            auto& button = previewIcons[i];
            button = muxc::Button{};
            button.Width(38);
            button.Height(38);
            button.Padding({0, 0, 0, 0});
            muxc::FontIcon icon{};
            icon.Glyph(glyphs[i]);
            icon.FontSize(18);
            button.Content(icon);
            iconTransforms[i] = muxm::CompositeTransform{};
            iconTransforms[i].CenterX(19);
            iconTransforms[i].CenterY(38);
            button.RenderTransform(iconTransforms[i]);
            icons.Children().Append(button);
            auto token = button.PointerEntered([this, i](const auto&, const auto&) { UpdateHover(i); });
            revoke.push_back([button, token]() { button.PointerEntered(token); });
            token = button.PointerExited([this](const auto&, const auto&) { UpdateHover(-1); });
            revoke.push_back([button, token]() { button.PointerExited(token); });
            token = button.GotFocus([this, i](const auto&, const auto&) { UpdateHover(i); });
            revoke.push_back([button, token]() { button.GotFocus(token); });
            token = button.LostFocus([this](const auto&, const auto&) { UpdateHover(-1); });
            revoke.push_back([button, token]() { button.LostFocus(token); });
        }
        dockCard.content.Children().Append(icons);
        // Vertical commands remain readable with long translations at narrow widths.
        for (int i = 0; i < 3; ++i)
        {
            playButtons[i] = muxc::Button{};
            playButtons[i].HorizontalAlignment(mux::HorizontalAlignment::Left);
            dockCard.content.Children().Append(playButtons[i]);
            const auto token = playButtons[i].Click([this, i](const auto&, const auto&) { StartPreview(i); });
            revoke.push_back([button = playButtons[i], token]() { button.Click(token); });
        }
        previewTimer = mux::DispatcherTimer{};
        previewTimer.Interval(std::chrono::milliseconds(16));
        const auto token = previewTimer.Tick([this](const auto&, const auto&) { RenderPreview(); });
        revoke.push_back([timer = previewTimer, token]() { timer.Tick(token); });
    }

    void UpdateHover(int index)
    {
        hoveredIcon = index;
        const bool enabled = active && dockEnabled && AnimationsEnabled();
        const int effect = hover.combo.SelectedIndex();
        const float focusScale = static_cast<float>(hoverScale ? hoverScale.Value() / 100.0 : 1.28);
        for (int i = 0; i < 5; ++i)
        {
            if (!iconTransforms[i]) continue;
            const int distance = (i - index) * 50;
            const float scale = enabled && index >= 0 ? dock_magnification::ScaleForEffect(
                effect, i == index, static_cast<float>(distance), 50, focusScale) : 1.0f;
            iconTransforms[i].CenterY(38);
            iconTransforms[i].ScaleX(scale);
            iconTransforms[i].ScaleY(scale);
            iconTransforms[i].TranslateX(!enabled || index < 0 || effect == 0 ? 0 :
                effect == 1 ? dock_magnification::SingleFocusAxisShift(distance, 38, focusScale) :
                dock_magnification::AxisShiftForDistance(distance, 50, 38, focusScale));
            iconTransforms[i].TranslateY(0);
        }
    }

    void StartPreview(int kind)
    {
        if (!CanEdit() || !AnimationsEnabled() || (kind != 0 && !dockEnabled)) return;
        StopPreview();
        previewKind = kind;
        previewStart = Clock::now();
        SYSTEM_POWER_STATUS power{};
        const bool available = GetSystemPowerStatus(&power) != FALSE;
        const int requested = std::array{0, 30, 60, 120}[std::clamp(frameLimit.combo.SelectedIndex(), 0, 3)];
        const int limit = animation::ResolveFrameLimit(requested, energySaver.toggle.IsOn(),
            onBattery.toggle.IsOn(), available && power.SystemStatusFlag != 0,
            available && power.ACLineStatus == 0);
        previewTimer.Interval(std::chrono::milliseconds(limit == 0 ? 16 :
            static_cast<int>(std::ceil(1000.0 / limit))));
        RenderPreview();
        previewTimer.Start();
    }

    void RenderPreview()
    {
        if (!active || !AnimationsEnabled()) { StopPreview(); return; }
        const double elapsed = std::chrono::duration<double>(Clock::now() - previewStart).count();
        const double phase = elapsed / animation::DurationScale(speed.combo.SelectedIndex());
        if (previewKind == 1)
        {
            const double elapsedMs = phase * 1000.0;
            if (elapsedMs >= dock_launch_animation::kMinimumDurationMs) { StopPreview(); return; }
            const int effect = launch.combo.SelectedIndex();
            iconTransforms[2].CenterY(19);
            iconTransforms[2].TranslateY(effect == 1 ? -dock_launch_animation::OffsetPixels(elapsedMs, 38) : 0);
            iconTransforms[2].ScaleX(effect == 2 ? dock_launch_animation::PulseScale(elapsedMs) : 1);
            iconTransforms[2].ScaleY(iconTransforms[2].ScaleX());
            return;
        }
        const int selected = previewKind == 0 ? popup.combo.SelectedIndex() : window.combo.SelectedIndex();
        const double closeDuration = previewKind == 0 ?
            popup_animation_rules::kCloseDurationMs / 1000.0 :
            selected == 3 ? 0.36 : selected == 2 ? 0.18 : 0.24;
        const double openDuration = previewKind == 0 ?
            popup_animation_rules::kOpenDurationMs / 1000.0 : closeDuration;
        constexpr double pause = 0.20;
        const double reopen = closeDuration + pause;
        const double finish = reopen + openDuration;
        if (phase >= finish + 0.15) { StopPreview(); return; }
        // Actual transition durations, separated by a brief illustrative pause.
        const double progress = phase < closeDuration ? phase / closeDuration :
            phase < reopen ? 1.0 : phase < finish ? 1.0 - (phase - reopen) / openDuration : 0.0;
        const double t = popup_animation_rules::EaseInOutSmooth(static_cast<float>(progress));
        const bool noEffect = selected == 0;
        const bool fade = previewKind == 0 ? selected == 1 : selected == 2;
        const bool genie = previewKind == 2 && selected == 3;
        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            if (previewKind == 0)
            {
                const float visibleProgress = static_cast<float>(1.0 - progress);
                const double scale = noEffect || fade ? 1.0 :
                    popup_animation_rules::ScaleForProgress(visibleProgress);
                slices[i].RenderTransform(sliceTransforms[i]);
                sliceTransforms[i].ScaleX(scale);
                sliceTransforms[i].ScaleY(scale);
                // Each strip shares the popup's center; the popup never travels to the Dock.
                sliceTransforms[i].TranslateY((36.0 - i * 6.0) * (1.0 - scale));
                slices[i].Opacity(noEffect ? (progress > 0.5 ? 0.0 : 1.0) :
                    fade ? popup_animation_rules::EaseInOutSmooth(visibleProgress) :
                    visibleProgress > 0.0f ? 1.0 : 0.0);
                continue;
            }
            if (genie)
            {
                const double begin = static_cast<double>(i) / slices.size();
                const double end = static_cast<double>(i + 1) / slices.size();
                const auto matrix = dock_genie::StripMatrix({65, 12, 215, 84},
                    {131, 103, 149, 111}, dock_genie::Edge::Bottom, t, 150, 72,
                    begin, end, 0, 0);
                genieTransforms[i].Matrix(muxm::Matrix{matrix.m11, matrix.m12,
                    matrix.m21, matrix.m22,
                    matrix.dx + i * 6.0 * matrix.m21 - 65,
                    matrix.dy + i * 6.0 * matrix.m22 - 12 - i * 6.0});
                slices[i].RenderTransform(genieTransforms[i]);
                slices[i].Opacity(dock_genie::Opacity(t));
                continue;
            }
            slices[i].RenderTransform(sliceTransforms[i]);
            auto& transform = sliceTransforms[i];
            transform.ScaleX(noEffect || fade ? 1.0 : 1.0 - 0.88 * t);
            transform.ScaleY(noEffect || fade ? 1.0 : 1.0 - (1.0 - 8.0 / 72.0) * t);
            transform.TranslateY(noEffect || fade ? 0.0 : t * (91.0 - i * 6.0));
            slices[i].Opacity(noEffect ? (progress > 0.5 ? 0.0 : 1.0) : 1.0 - t);
        }
    }

    void StopPreview()
    {
        if (previewTimer) previewTimer.Stop();
        previewKind = -1;
        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            if (!slices[i]) continue;
            slices[i].Opacity(1);
            slices[i].RenderTransform(sliceTransforms[i]);
            sliceTransforms[i].ScaleX(1);
            sliceTransforms[i].ScaleY(1);
            sliceTransforms[i].TranslateY(0);
        }
        for (const auto& transform : iconTransforms)
        {
            if (!transform) continue;
            transform.ScaleX(1);
            transform.ScaleY(1);
            transform.TranslateX(0);
            transform.TranslateY(0);
        }
    }

    void RefreshLocalizedText()
    {
        const bool wasUpdating = updating;
        updating = true;
        generalCard.title.Text(L("settings.animation.general"));
        dockCard.title.Text(L("settings.animation.dock"));
        performanceCard.title.Text(L("settings.animation.performance"));
        for (auto* choice : {&mode, &popup, &speed, &hover, &launch, &window, &frameLimit})
        {
            choice->row.SetText(L(choice->key), L(choice->key + ".description"));
            const int selected = choice->combo.SelectedIndex();
            choice->combo.Items().Clear();
            for (const auto& key : choice->options)
                choice->combo.Items().Append(winrt::box_value(L(key)));
            choice->combo.SelectedIndex(selected >= 0 ? selected : choice->defaultIndex);
            muxa::AutomationProperties::SetName(choice->combo, L(choice->key));
            muxa::AutomationProperties::SetHelpText(choice->combo, L(choice->key + ".description"));
            presenter_controls::ConfigureRestoreDefaultButton(choice->reset,
                L(choice->key) + L": " + L("settings.animation.restore"));
        }
        scaleRow.SetText(L("settings.animation.hoverScale"), L("settings.animation.hoverScale.description"));
        muxa::AutomationProperties::SetName(hoverScale, L("settings.animation.hoverScale"));
        presenter_controls::ConfigureRestoreDefaultButton(scaleReset,
            L("settings.animation.hoverScale") + L": " + L("settings.animation.restore"));
        for (auto* control : {&energySaver, &onBattery})
        {
            control->row.SetText(L(control->key), L(control->key + ".description"));
            muxa::AutomationProperties::SetName(control->toggle, L(control->key));
            presenter_controls::ConfigureRestoreDefaultButton(control->reset,
                L(control->key) + L": " + L("settings.animation.restore"));
        }
        dockNotice.Text(L("settings.animation.dockDisabled"));
        dockLink.Content(winrt::box_value(L("settings.animation.openDock")));
        previewTitle.Text(L("settings.animation.preview"));
        previewHint.Text(L("settings.animation.preview.description"));
        constexpr std::array playKeys{"popup", "launch", "window"};
        for (std::size_t i = 0; i < playButtons.size(); ++i)
            playButtons[i].Content(winrt::box_value(L(std::string("settings.animation.preview.") + playKeys[i])));
        for (int i = 0; i < 5; ++i)
        {
            const std::wstring name = L("settings.animation.preview.icon") + L" " + std::to_wstring(i + 1);
            muxa::AutomationProperties::SetName(previewIcons[i], name);
            muxc::ToolTipService::SetToolTip(previewIcons[i], winrt::box_value(name));
        }
        updating = wasUpdating;
        UpdateEnabled();
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed) return;
        const bool newGeneration = !hasSnapshot || generation != snapshot.generation;
        if (newGeneration)
        {
            scalePreview.Cancel();
            scaleIdle.Stop();
            scaleDirty = false;
            StopPreview();
        }
        generation = snapshot.generation;
        updating = true;
        if (newGeneration || generalRevision != snapshot.domainRevisions.general)
        {
            const auto& general = snapshot.values.general;
            dockEnabled = general.dockEnabled;
            mode.combo.SelectedIndex(animation::NormalizeMode(general.animationMode));
            popup.combo.SelectedIndex(animation::NormalizePopupEffect(general.popupAnimationEffect));
            speed.combo.SelectedIndex(animation::NormalizeSpeed(general.animationSpeed));
            const int frames = animation::NormalizeFrameLimit(general.animationFrameLimit);
            frameLimit.combo.SelectedIndex(frames == 30 ? 1 : frames == 60 ? 2 : frames == 120 ? 3 : 0);
            energySaver.toggle.IsOn(general.animationEnergySaver);
            onBattery.toggle.IsOn(general.animationOnBattery);
            generalRevision = snapshot.domainRevisions.general;
        }
        if (newGeneration || dockRevision != snapshot.domainRevisions.dock)
        {
            const auto& dock = snapshot.values.dock;
            hover.combo.SelectedIndex(animation::NormalizeHoverEffect(dock.hoverEffect));
            if (!scaleDirty)
                hoverScale.Value(std::round(animation::NormalizeHoverScale(dock.hoverScale) * 100.0));
            launch.combo.SelectedIndex(animation::NormalizeLaunchEffect(dock.launchEffect));
            window.combo.SelectedIndex(animation::NormalizeWindowEffect(dock.windowEffect));
            dockRevision = snapshot.domainRevisions.dock;
        }
        updating = false;
        hasSnapshot = true;
        UpdateEnabled();
    }

    void Deactivate() noexcept
    {
        try { CommitScale(); StopPreview(); } catch (...) {}
        active = false;
        hoveredIcon = -1;
    }
    void Close() noexcept
    {
        if (closed) return;
        Deactivate();
        closed = true;
        scalePreview.Close();
        for (auto& unhook : revoke) { try { unhook(); } catch (...) {} }
        revoke.clear();
        actions = {};
        navigate = {};
    }
};

AnimationPerformancePagePresenter::AnimationPerformancePagePresenter(
    LocalizeCallback localize, const mux::Style& cardStyle, NavigateCallback navigate)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle, std::move(navigate))) {}
AnimationPerformancePagePresenter::~AnimationPerformancePagePresenter() { Close(); }
void AnimationPerformancePagePresenter::SetActions(DockPageActions actions) { impl_->actions = std::move(actions); }
mux::UIElement AnimationPerformancePagePresenter::Content() const noexcept { return impl_->root; }
void AnimationPerformancePagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot) { impl_->ApplySnapshot(snapshot); }
void AnimationPerformancePagePresenter::RefreshLocalizedText() { impl_->RefreshLocalizedText(); }
void AnimationPerformancePagePresenter::Activate() noexcept
{
    if (impl_->closed) return;
    impl_->active = true;
    try { impl_->UpdateEnabled(); } catch (...) {}
}
void AnimationPerformancePagePresenter::Deactivate() noexcept { impl_->Deactivate(); }
void AnimationPerformancePagePresenter::Close() noexcept { if (impl_) impl_->Close(); }
void AnimationPerformancePagePresenter::RegisterFocusTargets(const FocusRegistrar& registrar) const
{
    for (const auto* choice : {&impl_->mode, &impl_->popup, &impl_->speed,
        &impl_->hover, &impl_->launch, &impl_->window, &impl_->frameLimit})
        registrar(choice->focusId, choice->combo);
    registrar("animation.hoverScale", impl_->hoverScale);
    registrar("animation.energySaver", impl_->energySaver.toggle);
    registrar("animation.onBattery", impl_->onBattery.toggle);
}
}
