#include "pch.h"
#include "animation_performance_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../animation_settings.h"
#include "../l10n.h"

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
using presenter_controls::SettingRow;

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
    muxc::TextBlock dockNotice{nullptr};
    muxc::HyperlinkButton dockLink{nullptr};
    std::vector<std::function<void()>> revoke;
    presenter_controls::CoalescedPreviewTimer<double> scalePreview;
    mux::DispatcherTimer scaleIdle{nullptr};
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

    void UpdateEnabled()
    {
        const bool enabled = mode.combo.SelectedIndex() != animation::Disabled;
        popup.row.SetEnabled(enabled);
        speed.row.SetEnabled(enabled);
        for (auto* choice : {&hover, &launch, &window})
            choice->row.SetEnabled(enabled && dockEnabled);
        scaleRow.SetEnabled(enabled && dockEnabled && hover.combo.SelectedIndex() != 0);
        dockNotice.Visibility(dockEnabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
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
        try { CommitScale(); } catch (...) {}
        active = false;
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
