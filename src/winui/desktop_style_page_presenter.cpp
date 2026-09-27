#include "pch.h"
#include "desktop_style_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../desktop_style_presets.h"

#include <array>
#include <vector>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
using presenter_controls::SettingRow;

namespace
{
constexpr std::array<std::string_view, 5> kPresets{
    "native", "taskbar-dock", "island", "merged", "side"};

muxc::Border PreviewSurface(bool accent = false)
{
    return mux::Markup::XamlReader::Load(accent
        ? LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource AccentFillColorDefaultBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="4" />)"
        : LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource CardBackgroundFillColorDefaultBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="4" />)")
        .as<muxc::Border>();
}

void ConfigureText(const muxc::TextBlock& text, bool heading = false)
{
    text.TextWrapping(mux::TextWrapping::Wrap);
    if (heading)
        text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    else
        text.Opacity(.72);
}
} // namespace

struct DesktopStylePagePresenter::Impl
{
    using ReadToggle = std::function<bool(const GeneralSettings&, const DockSettings&)>;
    using ReadChoice = std::function<int(const GeneralSettings&, const DockSettings&)>;
    struct Toggle
    {
        std::string key, help, focus;
        muxc::ToggleSwitch control;
        SettingRow row;
        ReadToggle read, enabled;
    };
    struct Choice
    {
        std::string key, help, focus;
        std::vector<std::string> options;
        muxc::ComboBox control;
        SettingRow row;
        ReadChoice read;
        ReadToggle enabled;
    };
    struct Section
    {
        std::string key;
        muxc::TextBlock title;
        muxc::StackPanel content;
    };
    // No asynchronous completion retains the presenter or an editor control.
    struct ConfirmationGate
    {
        bool alive = true, active = false, pending = false;
        std::uint64_t generation = 0, serial = 0;
    };

    LocalizeCallback localize;
    ApplyPresetCallback applyPreset;
    DockPageActions actions;
    muxc::StackPanel root;
    muxc::ComboBox presets;
    SettingRow presetRow;
    muxc::TextBlock presetDescription, presetChanges, adjustmentTitle, adjustmentHint;
    muxc::Button apply;
    muxc::Border preview;
    muxc::Grid previewLayout;
    std::vector<std::unique_ptr<Section>> sections;
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::unique_ptr<Choice>> choices;
    std::vector<std::function<void()>> revoke;
    std::shared_ptr<ConfirmationGate> gate = std::make_shared<ConfirmationGate>();
    GeneralSettings general;
    DockSettings dock;
    SettingsRoute lastRoute;
    std::uint64_t generation = 0, generalRevision = 0, dockRevision = 0, systemTaskbarRevision = 0;
    bool syncing = false, closed = false, active = false, hasSnapshot = false, hasRoute = false;

    std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }
    bool CanEdit() const noexcept
    {
        return !closed && !syncing && active && hasSnapshot && generation != 0;
    }
    static bool DockEnabled(const GeneralSettings& value, const DockSettings&)
    {
        return value.dockEnabled;
    }
    static bool BarEnabled(const GeneralSettings& value, const DockSettings&)
    {
        return value.statusBar.enabled;
    }
    static bool AnimationEnabled(const GeneralSettings& value, const DockSettings&)
    {
        return value.dockEnabled && value.animationMode != animation::Disabled;
    }
    muxc::StackPanel Card(const mux::Style& style)
    {
        muxc::Border border;
        if (style) border.Style(style);
        muxc::StackPanel content;
        content.Spacing(16);
        border.Child(content);
        root.Children().Append(border);
        return content;
    }
    muxc::StackPanel AddSection(const mux::Style& style, const char* key)
    {
        auto section = std::make_unique<Section>();
        section->key = key;
        section->content = Card(style);
        ConfigureText(section->title, true);
        section->content.Children().Append(section->title);
        const auto content = section->content;
        sections.push_back(std::move(section));
        return content;
    }
    void EmitGeneral(DockPageActions::GeneralEdit edit)
    {
        if (CanEdit() && actions.updateGeneral)
            actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, std::move(edit));
    }
    void EmitDock(DockPageActions::DockEdit edit)
    {
        if (CanEdit() && actions.updateDock)
            actions.updateDock(generation, SettingsUpdateMode::PreviewAndCommit, std::move(edit));
    }
    void AddToggle(const muxc::StackPanel& parent, const char* key,
        const char* help, const char* focus, ReadToggle read,
        std::function<void(bool)> write, ReadToggle enabled = {})
    {
        auto toggle = std::make_unique<Toggle>();
        toggle->key = key;
        toggle->help = help;
        toggle->focus = focus;
        toggle->read = std::move(read);
        toggle->enabled = std::move(enabled);
        toggle->row.Initialize(toggle->control);
        toggle->row.SetControlAlignment(mux::HorizontalAlignment::Right);
        parent.Children().Append(toggle->row.root);
        const auto control = toggle->control;
        const auto token = control.Toggled([this, control, write = std::move(write)](const auto&, const auto&) {
            if (CanEdit()) write(control.IsOn());
        });
        revoke.push_back([control, token] { control.Toggled(token); });
        toggles.push_back(std::move(toggle));
    }
    void AddDockToggle(const muxc::StackPanel& parent, const char* key,
        const char* help, const char* focus, bool DockSettings::* member,
        ReadToggle enabled = {})
    {
        AddToggle(parent, key, help, focus,
            [member](const auto&, const auto& value) { return value.*member; },
            [this, member](bool value) {
                EmitDock([member, value](auto& settings) { settings.*member = value; });
            }, std::move(enabled));
    }
    void AddChoice(const muxc::StackPanel& parent, const char* key,
        const char* help, const char* focus, std::vector<std::string> options,
        ReadChoice read, std::function<void(int)> write, ReadToggle enabled = {})
    {
        auto choice = std::make_unique<Choice>();
        choice->key = key;
        choice->help = help;
        choice->focus = focus;
        choice->options = std::move(options);
        choice->read = std::move(read);
        choice->enabled = std::move(enabled);
        choice->control.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        choice->row.Initialize(choice->control);
        parent.Children().Append(choice->row.root);
        const auto control = choice->control;
        const auto count = static_cast<int>(choice->options.size());
        const auto token = control.SelectionChanged([this, control, count, write = std::move(write)](const auto&, const auto&) {
            const int index = control.SelectedIndex();
            if (CanEdit() && index >= 0 && index < count) write(index);
        });
        revoke.push_back([control, token] { control.SelectionChanged(token); });
        choices.push_back(std::move(choice));
    }
    void ConfirmDisableDock()
    {
        // Restore the switch while the host-owned dialog decides the action.
        Sync();
        if (!CanEdit() || !actions.confirm || !actions.updateGeneral || gate->pending) return;
        gate->pending = true;
        const auto serial = ++gate->serial;
        const auto expectedGeneration = generation;
        const std::weak_ptr<ConfirmationGate> weak = gate;
        const auto update = actions.updateGeneral;
        actions.confirm(generation, L("settings.dock.disableConfirm.title"),
            L("settings.dock.disableConfirm.description"),
            [weak, update, serial, expectedGeneration](bool confirmed) {
                const auto state = weak.lock();
                if (!state || state->serial != serial) return;
                state->pending = false;
                if (!confirmed || !state->alive || !state->active || state->generation != expectedGeneration) return;
                update(expectedGeneration, SettingsUpdateMode::PreviewAndCommit,
                    [](GeneralSettings& settings) { settings.dockEnabled = false; });
            });
    }

    Impl(LocalizeCallback callback, const mux::Style& style, ApplyPresetCallback applyCallback)
        : localize(std::move(callback)), applyPreset(std::move(applyCallback))
    {
        root.Spacing(12);
        const auto presetCard = Card(style);
        presets.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presetRow.Initialize(presets);
        presetCard.Children().Append(presetRow.root);
        ConfigureText(presetDescription, true);
        presetCard.Children().Append(presetDescription);
        preview = PreviewSurface();
        preview.CornerRadius({8, 8, 8, 8});
        preview.Height(136);
        preview.Padding({12, 12, 12, 12});
        preview.IsHitTestVisible(false);
        preview.Child(previewLayout);
        presetCard.Children().Append(preview);
        ConfigureText(presetChanges);
        presetChanges.FontSize(13);
        presetCard.Children().Append(presetChanges);
        apply.HorizontalAlignment(mux::HorizontalAlignment::Right);
        apply.UseSystemFocusVisuals(true);
        if (const auto resource = mux::Application::Current().Resources().TryLookup(winrt::box_value(L"AccentButtonStyle")))
            if (const auto accent = resource.try_as<mux::Style>()) apply.Style(accent);
        presetCard.Children().Append(apply);
        auto token = presets.SelectionChanged([this](const auto&, const auto&) {
            if (!closed && !syncing) UpdatePreset();
        });
        revoke.push_back([control = presets, token] { control.SelectionChanged(token); });
        token = apply.Click([this](const auto&, const auto&) {
            const int index = presets.SelectedIndex();
            if (CanEdit() && applyPreset && index >= 0 && index < static_cast<int>(kPresets.size()))
                applyPreset(std::string(kPresets[static_cast<std::size_t>(index)]));
        });
        revoke.push_back([control = apply, token] { control.Click(token); });
        ConfigureText(adjustmentTitle, true);
        adjustmentTitle.FontSize(18);
        adjustmentTitle.Margin({0, 12, 0, 0});
        ConfigureText(adjustmentHint);
        root.Children().Append(adjustmentTitle);
        root.Children().Append(adjustmentHint);

        const auto dockCard = AddSection(style, "settings.dock.dock");
        AddToggle(dockCard, "app.dock.enable", "", "desktopStyle.dock.enable", DockEnabled,
            [this](bool enabled) {
                if (!enabled && general.dockEnabled) ConfirmDisableDock();
                else EmitGeneral([enabled](auto& settings) { settings.dockEnabled = enabled; });
            });
        AddChoice(dockCard, "app.settings.dock_position", "", "desktopStyle.dock.position",
            {"app.dock.bottom", "app.dock.top", "app.dock.left", "app.dock.right"},
            [](const auto&, const auto& value) { return static_cast<int>(value.position); },
            [this](int index) { EmitDock([index](auto& value) { value.position = static_cast<DockPosition>(index); }); }, DockEnabled);
        AddChoice(dockCard, "app.dock.layout", "", "desktopStyle.dock.layout",
            {"app.dock.island", "app.dock.edge"},
            [](const auto&, const auto& value) { return value.edgeAttached ? 1 : 0; },
            [this](int index) { EmitDock([index](auto& value) { value.edgeAttached = index == 1; }); }, DockEnabled);
        const std::vector<std::string> monitorOptions{
            "app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"};
        AddChoice(dockCard, "settings.dock.monitor", "", "desktopStyle.dock.monitor", monitorOptions,
            [](const auto&, const auto& value) { return static_cast<int>(value.monitorScope); },
            [this](int index) { EmitDock([index](auto& value) { value.monitorScope = static_cast<DockMonitorScope>(index); }); }, DockEnabled);
        AddDockToggle(dockCard, "settings.dock.showOnlyWhenSummoned", "settings.dock.showOnlyWhenSummoned.description",
            "desktopStyle.dock.showOnlyWhenSummoned", &DockSettings::showOnlyWhenSummoned, DockEnabled);
        AddToggle(dockCard, "app.dock.floating_edge_swipe", "app.dock.floating_edge_swipe_hint",
            "desktopStyle.dock.edgeSwipe",
            [](const auto&, const auto& value) {
                return dock_settings_rules::IsFloatingEdgeSwipeEnabled(value.showOnlyWhenSummoned, value.floatingEdgeSwipeEnabled);
            }, [this](bool enabled) {
                EmitDock([enabled](auto& value) {
                    value.floatingEdgeSwipeEnabled = enabled;
                    dock_settings_rules::DisableSummonOnlyWhenPrerequisiteDisabled(enabled, value.showOnlyWhenSummoned);
                });
            }, DockEnabled);
        AddDockToggle(dockCard, "settings.dock.reserveScreenSpace", "settings.dock.reserveScreenSpace.description",
            "desktopStyle.dock.reserveScreenSpace", &DockSettings::reserveScreenSpace,
            [](const auto& settings, const auto& value) {
                const auto& bar = settings.statusBar;
                const bool everyDockMerged = bar.enabled && value.edgeAttached && value.position == bar.position &&
                    (value.monitorScope == bar.monitorScope || bar.monitorScope == DockMonitorScope::All);
                return settings.dockEnabled && !value.showOnlyWhenSummoned && !everyDockMerged;
            });
        AddToggle(dockCard, "settings.dock.allowDesktopContentOverlap", "settings.dock.allowDesktopContentOverlap.description",
            "desktopStyle.dock.allowDesktopContentOverlap",
            [](const auto&, const auto& value) {
                return dock_settings_rules::IsDesktopContentOverlapEnabled(value.showOnlyWhenSummoned, value.allowDesktopContentOverlap);
            }, [this](bool enabled) {
                EmitDock([enabled](auto& value) {
                    value.allowDesktopContentOverlap = enabled;
                    dock_settings_rules::DisableSummonOnlyWhenPrerequisiteDisabled(enabled, value.showOnlyWhenSummoned);
                });
            }, DockEnabled);

        const auto taskbarCard = AddSection(style, "settings.nav.taskbar");
        AddDockToggle(taskbarCard, "settings.dock.suppressTaskbar", "settings.dock.suppressTaskbar.description",
            "desktopStyle.taskbar.suppress", &DockSettings::suppressSystemTaskbar, DockEnabled);
        AddDockToggle(taskbarCard, "settings.taskbar.autoHide", "settings.taskbar.autoHide.description",
            "desktopStyle.taskbar.autoHide", &DockSettings::systemTaskbarAutoHide);
        AddDockToggle(taskbarCard, "settings.desktopStyle.taskbarAppearance", "settings.desktopStyle.taskbarAppearance.description",
            "desktopStyle.taskbar.appearance", &DockSettings::systemTaskbarBackdropEnabled);

        const auto barCard = AddSection(style, "settings.nav.statusBar");
        AddToggle(barCard, "statusBar.enabled", "", "desktopStyle.statusBar.enable", BarEnabled,
            [this](bool enabled) { EmitGeneral([enabled](auto& value) { value.statusBar.enabled = enabled; }); });
        AddChoice(barCard, "statusBar.position", "", "desktopStyle.statusBar.position",
            {"app.dock.bottom", "app.dock.top"},
            [](const auto& value, const auto&) { return static_cast<int>(value.statusBar.position); },
            [this](int index) { EmitGeneral([index](auto& value) { value.statusBar.position = static_cast<DockPosition>(index); }); }, BarEnabled);
        AddChoice(barCard, "settings.dock.monitor", "", "desktopStyle.statusBar.monitor", monitorOptions,
            [](const auto& value, const auto&) { return static_cast<int>(value.statusBar.monitorScope); },
            [this](int index) { EmitGeneral([index](auto& value) { value.statusBar.monitorScope = static_cast<DockMonitorScope>(index); }); }, BarEnabled);

        const auto animationCard = AddSection(style, "settings.animation.dock");
        AddChoice(animationCard, "settings.animation.hover", "settings.animation.hover.description", "desktopStyle.animation.hover",
            {"settings.animation.option.noMagnification", "settings.animation.option.singleIcon", "settings.animation.option.wave"},
            [](const auto&, const auto& value) { return animation::NormalizeHoverEffect(value.hoverEffect); },
            [this](int index) { EmitDock([index](auto& value) { value.hoverEffect = index; }); }, AnimationEnabled);
        AddChoice(animationCard, "settings.animation.launch", "settings.animation.launch.description", "desktopStyle.animation.launch",
            {"settings.animation.option.off", "settings.animation.option.bounce", "settings.animation.option.gentleScale"},
            [](const auto&, const auto& value) { return animation::NormalizeLaunchEffect(value.launchEffect); },
            [this](int index) { EmitDock([index](auto& value) { value.launchEffect = index; }); }, AnimationEnabled);
        Localize();
    }

    void AddPreviewBar(DockPosition position, bool island, bool dockBar, bool merged = false)
    {
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        auto bar = PreviewSurface(dockBar);
        const double thickness = dockBar ? 20.0 : 12.0;
        const double length = island ? 104.0 : 156.0;
        bar.HorizontalAlignment(vertical
            ? (position == DockPosition::Left ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Right)
            : mux::HorizontalAlignment::Stretch);
        bar.VerticalAlignment(vertical ? mux::VerticalAlignment::Stretch
            : (position == DockPosition::Top ? mux::VerticalAlignment::Top : mux::VerticalAlignment::Bottom));
        if (vertical)
        {
            bar.Width(thickness);
            if (island) { bar.Height(76); bar.VerticalAlignment(mux::VerticalAlignment::Center); }
        }
        else
        {
            bar.Height(thickness);
            if (island) { bar.Width(length); bar.HorizontalAlignment(mux::HorizontalAlignment::Center); }
        }
        bar.Margin(island ? mux::Thickness{4, 4, 4, 4} : mux::Thickness{0, 0, 0, 0});
        if (dockBar)
        {
            muxc::StackPanel items;
            items.Orientation(vertical ? muxc::Orientation::Vertical : muxc::Orientation::Horizontal);
            items.Spacing(5);
            items.HorizontalAlignment(mux::HorizontalAlignment::Center);
            items.VerticalAlignment(mux::VerticalAlignment::Center);
            for (int index = 0; index != 5; ++index)
            {
                auto item = PreviewSurface();
                item.Width(8); item.Height(8);
                item.CornerRadius({2, 2, 2, 2});
                items.Children().Append(item);
            }
            muxc::Grid barContent;
            barContent.Children().Append(items);
            if (merged)
            {
                auto status = PreviewSurface();
                status.Width(22); status.Height(4);
                status.HorizontalAlignment(mux::HorizontalAlignment::Right);
                status.VerticalAlignment(mux::VerticalAlignment::Center);
                status.Margin({0, 0, 8, 0});
                barContent.Children().Append(status);
            }
            bar.Child(barContent);
        }
        previewLayout.Children().Append(bar);
    }
    void UpdatePreset()
    {
        const int index = presets.SelectedIndex();
        if (index < 0 || index >= static_cast<int>(kPresets.size())) return;
        const std::string key(kPresets[static_cast<std::size_t>(index)]);
        const std::string prefix = "settings.desktopStyle." + key;
        presetDescription.Text(L(prefix + ".description"));
        presetChanges.Text(L(prefix + ".changes"));
        muxa::AutomationProperties::SetName(apply, L(prefix + ".title") + L": " + L("settings.desktopStyle.applyLayout"));
        muxa::AutomationProperties::SetName(preview, L(prefix + ".title"));
        previewLayout.Children().Clear();
        auto previewGeneral = general;
        auto previewDock = dock;
        ApplyDesktopStylePreset(std::wstring(key.begin(), key.end()), previewGeneral, previewDock);
        const bool merged = previewGeneral.dockEnabled && previewGeneral.statusBar.enabled && previewDock.edgeAttached &&
            previewDock.position == previewGeneral.statusBar.position;
        if (previewGeneral.statusBar.enabled && !merged)
            AddPreviewBar(previewGeneral.statusBar.position, false, false);
        if (previewGeneral.dockEnabled)
            AddPreviewBar(previewDock.position, !previewDock.edgeAttached, true, merged);
        if (!previewDock.suppressSystemTaskbar)
        {
            // The native taskbar is illustrative: this preset preserves its
            // Windows-owned position and its saved Dock edge/layout.
            auto taskbar = PreviewSurface();
            taskbar.Height(previewDock.systemTaskbarAutoHide ? 3.0 : 12.0);
            taskbar.VerticalAlignment(mux::VerticalAlignment::Bottom);
            taskbar.Opacity(.6);
            previewLayout.Children().Append(taskbar);
        }
    }
    bool SelectPresetForFocus(std::string_view focusId)
    {
        if (closed) return false;
        constexpr std::string_view prefix = "desktopStyle.";
        if (!focusId.starts_with(prefix)) return false;
        const auto preset = focusId.substr(prefix.size());
        const auto found = std::find(kPresets.begin(), kPresets.end(), preset);
        if (found == kPresets.end()) return false;
        const bool previous = syncing;
        syncing = true;
        presets.SelectedIndex(static_cast<int>(found - kPresets.begin()));
        syncing = previous;
        UpdatePreset();
        return true;
    }
    void Sync()
    {
        if (closed) return;
        const bool previous = syncing;
        syncing = true;
        for (const auto& toggle : toggles)
        {
            toggle->control.IsOn(toggle->read(general, dock));
            toggle->row.SetEnabled(hasSnapshot && (!toggle->enabled || toggle->enabled(general, dock)));
        }
        for (const auto& choice : choices)
        {
            choice->control.SelectedIndex(choice->read(general, dock));
            choice->row.SetEnabled(hasSnapshot && (!choice->enabled || choice->enabled(general, dock)));
        }
        apply.IsEnabled(hasSnapshot);
        syncing = previous;
        UpdatePreset();
    }
    void Localize()
    {
        if (closed) return;
        const bool previous = syncing;
        syncing = true;
        const int selected = presets.SelectedIndex();
        presets.Items().Clear();
        for (const auto key : kPresets)
            presets.Items().Append(winrt::box_value(L("settings.desktopStyle." + std::string(key) + ".title")));
        presets.SelectedIndex(selected >= 0 ? selected : 0);
        presetRow.SetText(L("settings.desktopStyle.presets"), L("settings.desktopStyle.presets.description"));
        muxa::AutomationProperties::SetName(presets, presetRow.label.Text());
        muxa::AutomationProperties::SetHelpText(presets, presetRow.help.Text());
        apply.Content(winrt::box_value(L("settings.desktopStyle.applyLayout")));
        adjustmentTitle.Text(L("settings.desktopStyle.adjustments"));
        adjustmentHint.Text(L("settings.desktopStyle.adjustments.description"));
        for (const auto& section : sections) section->title.Text(L(section->key));
        for (const auto& toggle : toggles)
        {
            toggle->row.SetText(L(toggle->key), toggle->help.empty() ? std::wstring{} : L(toggle->help));
            muxa::AutomationProperties::SetName(toggle->control, toggle->row.label.Text());
            muxa::AutomationProperties::SetHelpText(toggle->control, toggle->row.help.Text());
        }
        for (const auto& choice : choices)
        {
            choice->row.SetText(L(choice->key), choice->help.empty() ? std::wstring{} : L(choice->help));
            muxa::AutomationProperties::SetName(choice->control, choice->row.label.Text());
            muxa::AutomationProperties::SetHelpText(choice->control, choice->row.help.Text());
            choice->control.Items().Clear();
            for (const auto& option : choice->options) choice->control.Items().Append(winrt::box_value(L(option)));
        }
        syncing = previous;
        Sync();
    }
    void Close() noexcept
    {
        if (closed) return;
        closed = true;
        active = false;
        gate->alive = false;
        gate->active = false;
        ++gate->serial;
        for (const auto& remove : revoke)
        {
            try { remove(); } catch (...) {}
        }
        revoke.clear();
        actions = {};
        applyPreset = {};
    }
};

DesktopStylePagePresenter::DesktopStylePagePresenter(LocalizeCallback localize,
    const mux::Style& cardStyle, ApplyPresetCallback applyPreset)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle, std::move(applyPreset))) {}
DesktopStylePagePresenter::~DesktopStylePagePresenter() { Close(); }
void DesktopStylePagePresenter::SetActions(DockPageActions actions) { impl_->actions = std::move(actions); }
mux::UIElement DesktopStylePagePresenter::Content() const { return impl_->root; }
void DesktopStylePagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    const bool newSession = !impl_->hasSnapshot || impl_->generation != snapshot.generation;
    // Route transitions can publish without changing either settings domain.
    // Only a real transition selects the requested preview; normal snapshot
    // echoes must preserve any preset the user subsequently chose by hand.
    const bool routeChanged = newSession || !impl_->hasRoute ||
        impl_->lastRoute.page != snapshot.route.page || impl_->lastRoute.focusId != snapshot.route.focusId;
    impl_->lastRoute = snapshot.route;
    impl_->hasRoute = true;
    const bool presetNavigation = routeChanged && snapshot.route.page == SettingsPage::DesktopStyle;
    if (!newSession && impl_->generalRevision == snapshot.domainRevisions.general &&
        impl_->dockRevision == snapshot.domainRevisions.dock && impl_->systemTaskbarRevision == snapshot.domainRevisions.systemTaskbar)
    {
        if (presetNavigation) impl_->SelectPresetForFocus(snapshot.route.focusId);
        return;
    }
    impl_->generation = snapshot.generation;
    impl_->gate->generation = snapshot.generation;
    if (newSession)
    {
        impl_->gate->pending = false;
        ++impl_->gate->serial;
    }
    impl_->general = snapshot.values.general;
    impl_->dock = snapshot.values.dock;
    if (!impl_->hasSnapshot)
    {
        const auto& general = impl_->general;
        const auto& dock = impl_->dock;
        int selected = 0;
        if (general.dockEnabled)
        {
            if (!general.statusBar.enabled) selected = 1;
            else if (dock.position == DockPosition::Left || dock.position == DockPosition::Right) selected = 4;
            else if (dock.edgeAttached && dock.position == general.statusBar.position) selected = 3;
            else selected = 2;
        }
        impl_->syncing = true;
        impl_->presets.SelectedIndex(selected);
        impl_->syncing = false;
    }
    impl_->hasSnapshot = true;
    impl_->generalRevision = snapshot.domainRevisions.general;
    impl_->dockRevision = snapshot.domainRevisions.dock;
    impl_->systemTaskbarRevision = snapshot.domainRevisions.systemTaskbar;
    impl_->Sync();
    if (presetNavigation) impl_->SelectPresetForFocus(snapshot.route.focusId);
}
void DesktopStylePagePresenter::RefreshLocalizedText() { impl_->Localize(); }
void DesktopStylePagePresenter::Activate(std::string_view focusId)
{
    if (impl_->closed) return;
    impl_->active = true;
    impl_->gate->active = true;
    impl_->SelectPresetForFocus(focusId);
}
void DesktopStylePagePresenter::Deactivate()
{
    impl_->active = false;
    impl_->gate->active = false;
    impl_->gate->pending = false;
    ++impl_->gate->serial;
}
void DesktopStylePagePresenter::Close() noexcept { if (impl_) impl_->Close(); }
void DesktopStylePagePresenter::RegisterFocusTargets(const std::function<void(std::string, const mux::FrameworkElement&)>& target) const
{
    if (!target || impl_->closed) return;
    target("desktopStyle", impl_->presets);
    for (const auto preset : kPresets) target("desktopStyle." + std::string(preset), impl_->presets);
    for (const auto& toggle : impl_->toggles) target(toggle->focus, toggle->control);
    for (const auto& choice : impl_->choices) target(choice->focus, choice->control);
}

} // namespace snowdesktop::winui
