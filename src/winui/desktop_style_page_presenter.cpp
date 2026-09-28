#include "pch.h"
#include "desktop_style_page_presenter.h"
#include "merged_bar_height_editor.h"
#include "settings_presenter_controls.h"
#include "../desktop_style_presets.h"
#include "../dock_collection_icon_rules.h"
#include "../status_bar_view.h"
#include "../status_bar_layout.h"

#include <array>
#include <cmath>
#include <shellapi.h>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxmi = winrt::Microsoft::UI::Xaml::Media::Imaging;
using presenter_controls::SettingRow;

namespace
{
constexpr std::array<std::string_view, 5> kPresets{
    "native", "taskbar-dock", "island", "merged", "side"};
constexpr double kPreviewWidth = 960, kPreviewHeight = 540;
constexpr double kPreviewStatusHeight = 32, kPreviewDockHeight = 64;

muxc::Border PreviewSurface()
{
    return mux::Markup::XamlReader::Load(
        LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SolidBackgroundFillColorSecondaryBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="8" />)")
        .as<muxc::Border>();
}

bool IsHighContrastEnabled() noexcept
{
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(state), &state, 0) &&
        (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

DockPosition NativeTaskbarPosition() noexcept
{
    APPBARDATA taskbar{};
    taskbar.cbSize = sizeof(taskbar);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &taskbar)) return DockPosition::Bottom;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&taskbar.rc, MONITOR_DEFAULTTOPRIMARY), &monitor))
        return DockPosition::Bottom;
    if (taskbar.rc.right - taskbar.rc.left > taskbar.rc.bottom - taskbar.rc.top)
        return std::abs(taskbar.rc.top - monitor.rcMonitor.top) < std::abs(taskbar.rc.bottom - monitor.rcMonitor.bottom)
            ? DockPosition::Top : DockPosition::Bottom;
    return std::abs(taskbar.rc.left - monitor.rcMonitor.left) < std::abs(taskbar.rc.right - monitor.rcMonitor.right)
        ? DockPosition::Left : DockPosition::Right;
}

muxc::FontIcon PreviewGlyph(std::wstring_view glyph, double size)
{
    muxc::FontIcon icon;
    icon.Glyph(winrt::hstring(glyph));
    icon.FontSize(size);
    return icon;
}

// The real Dock draws its Start control as four panes, without a font glyph.
muxc::Grid PreviewStartIcon(double size, bool highContrast)
{
    muxc::Grid icon;
    icon.Width(size); icon.Height(size);
    icon.ColumnSpacing(size * .09); icon.RowSpacing(size * .09);
    for (int i = 0; i != 2; ++i)
    {
        icon.ColumnDefinitions().Append(muxc::ColumnDefinition{});
        icon.RowDefinitions().Append(muxc::RowDefinition{});
    }
    for (int i = 0; i != 4; ++i)
    {
        auto pane = mux::Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource TextFillColorPrimaryBrush}" />)").as<muxc::Border>();
        if (!highContrast) pane.Background(muxm::SolidColorBrush(winrt::Windows::UI::Color{255, 0, 120, 214}));
        muxc::Grid::SetRow(pane, i / 2); muxc::Grid::SetColumn(pane, i % 2);
        icon.Children().Append(pane);
    }
    return icon;
}

mux::UIElement PreviewAppIcon(std::wstring_view asset, std::wstring_view fallback, bool highContrast, double size = 40)
{
    if (highContrast) return PreviewGlyph(fallback, size * .8);
    muxmi::SvgImageSource source;
    source.UriSource(winrt::Windows::Foundation::Uri(winrt::hstring(asset)));
    muxc::Image image;
    image.Source(source);
    image.Width(size); image.Height(size);
    image.Stretch(muxm::Stretch::Uniform);
    return image;
}

muxc::FontIcon PreviewFluentGlyph(std::wstring_view glyph, double size = 18)
{
    auto icon = PreviewGlyph(glyph, size);
    icon.FontFamily(muxm::FontFamily(L"ms-appx:///Assets/Fonts/FluentSystemIcons-Regular.ttf#FluentSystemIcons-Regular"));
    return icon;
}

muxc::Viewbox FitPreviewContent(const mux::UIElement& child, mux::HorizontalAlignment alignment)
{
    muxc::Viewbox view;
    view.Child(child);
    view.Stretch(muxm::Stretch::Uniform);
    view.StretchDirection(muxc::StretchDirection::DownOnly);
    view.HorizontalAlignment(alignment);
    view.VerticalAlignment(mux::VerticalAlignment::Center);
    return view;
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
    muxc::GridView presets;
    std::unique_ptr<MergedBarHeightEditor> mergedHeight;
    muxc::StackPanel companionOptions;
    muxc::ComboBox companionPosition, companionForm;
    SettingRow companionPositionRow, companionFormRow;
    muxc::TextBlock presetChanges, adjustmentTitle;
    muxc::Button apply;
    muxc::Expander changes;
    muxc::StackPanel presetDetails;
    struct PresetCard
    {
        muxc::GridViewItem item;
        muxc::Border surface, wallpaper;
        muxc::Grid layout;
        muxc::TextBlock title, description;
    };
    std::array<PresetCard, kPresets.size()> presetCards;
    // This is a layout illustration, not a live desktop view. Query the
    // Windows-owned edge once when the page is created, never per snapshot.
    const DockPosition nativeTaskbarPosition = NativeTaskbarPosition();
    std::vector<std::unique_ptr<Section>> sections;
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::unique_ptr<Choice>> choices;
    std::vector<std::function<void()>> revoke;
    std::shared_ptr<ConfirmationGate> gate = std::make_shared<ConfirmationGate>();
    GeneralSettings general;
    DockSettings dock;
    SettingsRoute lastRoute;
    int monitorCount = 0;
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
        presets.SelectionMode(muxc::ListViewSelectionMode::Single);
        presets.IsTabStop(true);
        presets.Padding({0, 0, 0, 0});
        presets.ItemsPanel(mux::Markup::XamlReader::Load(LR"(<ItemsPanelTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><ItemsWrapGrid Orientation="Horizontal" MaximumRowsOrColumns="5" /></ItemsPanelTemplate>)").as<muxc::ItemsPanelTemplate>());
        muxc::ScrollViewer::SetHorizontalScrollBarVisibility(presets, muxc::ScrollBarVisibility::Disabled);
        muxc::ScrollViewer::SetVerticalScrollBarVisibility(presets, muxc::ScrollBarVisibility::Disabled);
        muxc::ScrollViewer::SetHorizontalScrollMode(presets, muxc::ScrollMode::Disabled);
        muxc::ScrollViewer::SetVerticalScrollMode(presets, muxc::ScrollMode::Disabled);
        BuildPresetCards();
        presetCard.Children().Append(presets);
        companionOptions.Spacing(8);
        companionPosition.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        companionForm.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        companionPositionRow.Initialize(companionPosition);
        companionFormRow.Initialize(companionForm);
        companionOptions.Children().Append(companionPositionRow.root);
        companionOptions.Children().Append(companionFormRow.root);
        presetCard.Children().Append(companionOptions);
        for (const auto& control : {companionPosition, companionForm})
        {
            const auto optionToken = control.SelectionChanged([this](const auto&, const auto&) {
                if (!closed && !syncing) { UpdatePreset(); RefreshPreviews(); }
            });
            revoke.push_back([control, optionToken] { control.SelectionChanged(optionToken); });
        }
        presetDetails.Spacing(12);
        presetCard.Children().Append(presetDetails);
        ConfigureText(presetChanges);
        presetChanges.FontSize(13);
        changes.Content(presetChanges);
        changes.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        changes.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        presetDetails.Children().Append(changes);
        apply.HorizontalAlignment(mux::HorizontalAlignment::Right);
        apply.UseSystemFocusVisuals(true);
        if (const auto resource = mux::Application::Current().Resources().TryLookup(winrt::box_value(L"AccentButtonStyle")))
            if (const auto accent = resource.try_as<mux::Style>()) apply.Style(accent);
        presetDetails.Children().Append(apply);
        const auto sizeToken = presets.SizeChanged([this](const auto&, const mux::SizeChangedEventArgs& event) {
            if (!closed) ArrangePresets(event.NewSize().Width);
        });
        revoke.push_back([control = presets, sizeToken] { control.SizeChanged(sizeToken); });
        const auto loadedToken = presets.Loaded([this](const auto&, const auto&) {
            if (!closed) ArrangePresets(presets.ActualWidth());
        });
        revoke.push_back([control = presets, loadedToken] { control.Loaded(loadedToken); });
        const auto themeToken = root.ActualThemeChanged([this](const auto&, const auto&) {
            if (!closed) RefreshPreviews();
        });
        revoke.push_back([control = root, themeToken] { control.ActualThemeChanged(themeToken); });
        ArrangePresets(0);
        auto token = presets.SelectionChanged([this](const auto&, const auto&) {
            if (!closed && !syncing) UpdatePreset();
        });
        revoke.push_back([control = presets, token] { control.SelectionChanged(token); });
        token = apply.Click([this](const auto&, const auto&) {
            const int index = presets.SelectedIndex();
            if (CanEdit() && applyPreset && index >= 0 && index < static_cast<int>(kPresets.size()))
                applyPreset(std::string(kPresets[static_cast<std::size_t>(index)]),
                    static_cast<DockPosition>(std::max(0, companionPosition.SelectedIndex())),
                    companionForm.SelectedIndex() == 1);
        });
        revoke.push_back([control = apply, token] { control.Click(token); });
        ConfigureText(adjustmentTitle, true);
        adjustmentTitle.FontSize(18);
        adjustmentTitle.Margin({0, 12, 0, 0});
        root.Children().Append(adjustmentTitle);

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
                });
            }, [](const auto& settings, const auto& value) { return settings.dockEnabled && !value.showOnlyWhenSummoned; });
        AddChoice(dockCard, "settings.dock.edgeRevealGesture", "settings.dock.edgeRevealGesture.description",
            "desktopStyle.dock.edgeRevealGesture", {"settings.dock.gestureSwipe", "settings.dock.gestureHover"},
            [](const auto&, const auto& value) { return value.edgeRevealGesture; },
            [this](int index) { EmitDock([index](auto& value) { value.edgeRevealGesture = index; }); },
            [](const auto& settings, const auto& value) { return settings.dockEnabled &&
                dock_settings_rules::IsFloatingEdgeSwipeEnabled(value.showOnlyWhenSummoned, value.floatingEdgeSwipeEnabled); });
        AddDockToggle(dockCard, "settings.dock.windowPreviews", "settings.dock.windowPreviews.description",
            "desktopStyle.dock.showWindowPreviews", &DockSettings::showWindowPreviews, DockEnabled);
        AddDockToggle(dockCard, "settings.dock.reserveScreenSpace", "settings.dock.reserveScreenSpace.description",
            "desktopStyle.dock.reserveScreenSpace", &DockSettings::reserveScreenSpace,
            [](const auto& settings, const auto& value) {
                const auto state = ResolveBarSettingsAvailability(settings.dockEnabled, value, settings.statusBar, GetSystemMetrics(SM_CMONITORS));
                return settings.dockEnabled && !value.showOnlyWhenSummoned && !state.allDockMerged;
            });
        AddToggle(dockCard, "settings.dock.allowDesktopContentOverlap", "settings.dock.allowDesktopContentOverlap.description",
            "desktopStyle.dock.allowDesktopContentOverlap",
            [](const auto&, const auto& value) {
                return dock_settings_rules::ShouldReserveDesktopWorkArea(value.showOnlyWhenSummoned, value.allowDesktopContentOverlap);
            }, [this](bool enabled) {
                EmitDock([enabled](auto& value) {
                    value.allowDesktopContentOverlap = !enabled;
                });
            }, [](const auto& settings, const auto& value) {
                return settings.dockEnabled && !value.showOnlyWhenSummoned && !ResolveBarSettingsAvailability(
                    settings.dockEnabled, value, settings.statusBar, GetSystemMetrics(SM_CMONITORS)).allDockMerged;
            });

        mergedHeight = std::make_unique<MergedBarHeightEditor>(localize);
        dockCard.Children().Append(mergedHeight->Content());
        AddDockToggle(dockCard, "settings.bars.homeSize", "settings.bars.homeSize.description",
            "desktopStyle.dock.lastMonitorUseHomeSize", &DockSettings::lastMonitorUseHomeSize,
            [](const auto& settings, const auto& value) {
                const auto state = ResolveBarSettingsAvailability(settings.dockEnabled, value, settings.statusBar, GetSystemMetrics(SM_CMONITORS));
                return state.dockOnLastMonitor && !state.allDockMerged;
            });
        const auto taskbarCard = AddSection(style, "settings.nav.taskbar");
        AddChoice(taskbarCard, "settings.bars.taskbarMode", "", "desktopStyle.taskbar.displayMode",
            {"settings.bars.taskbarVisible", "settings.taskbar.autoHide", "settings.dock.suppressTaskbar"},
            [](const auto&, const auto& value) { return TaskbarDisplayMode(value); },
            [this](int index) {
                if (index == 2 && !general.dockEnabled) { Sync(); return; }
                EmitDock([index](auto& value) { SetTaskbarDisplayMode(value, index); });
            });
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

    void BuildPresetCards()
    {
        for (auto& card : presetCards)
        {
            card.surface = PreviewSurface();
            card.surface.Width(kPreviewWidth); card.surface.Height(kPreviewHeight);
            card.surface.BorderThickness({0, 0, 0, 0});
            card.surface.CornerRadius({0, 0, 0, 0});
            card.wallpaper = mux::Markup::XamlReader::Load(LR"(
                <Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Opacity="0.26">
                    <Border.Background>
                        <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
                            <GradientStop Color="{ThemeResource SystemAccentColorLight2}" Offset="0" />
                            <GradientStop Color="{ThemeResource SystemAccentColor}" Offset="0.5" />
                            <GradientStop Color="{ThemeResource SystemAccentColorDark2}" Offset="1" />
                        </LinearGradientBrush>
                    </Border.Background>
                </Border>)").as<muxc::Border>();
            muxc::Grid scene;
            scene.Children().Append(card.wallpaper);
            scene.Children().Append(card.layout);
            card.surface.Child(scene);
            muxc::Viewbox view;
            view.Child(card.surface); view.Stretch(muxm::Stretch::Uniform);
            view.IsHitTestVisible(false);
            muxa::AutomationProperties::SetAccessibilityView(view, muxa::Peers::AccessibilityView::Raw);
            ConfigureText(card.title, true); card.title.FontSize(14); card.title.Height(36); card.title.MaxLines(2);
            card.title.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            ConfigureText(card.description); card.description.FontSize(12); card.description.Height(32); card.description.MaxLines(2);
            card.description.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            muxc::StackPanel content;
            content.Spacing(6);
            content.Children().Append(view);
            content.Children().Append(card.title);
            content.Children().Append(card.description);
            card.item.Width(220);
            card.item.Padding({8, 8, 8, 8});
            card.item.Margin({0, 0, 8, 8});
            card.item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            card.item.VerticalContentAlignment(mux::VerticalAlignment::Top);
            card.item.Content(content);
            presets.Items().Append(card.item);
        }
        presets.SelectedIndex(0);
    }
    void ArrangePresets(double width)
    {
        if (width <= 0) return;
        // Keep all five previews in the page flow. The outer settings page owns
        // scrolling; narrowing the window changes columns, not preview access.
        // Five compact cards on wide pages; use 3+2 below that breakpoint,
        // instead of leaving the fifth preset alone under four wide cards.
        const int columns = width >= 1000 ? 5 : width >= 660 ? 3 : width >= 440 ? 2 : 1;
        const double cellWidth = std::floor(width / columns);
        const double cellHeight = std::ceil(std::max(1.0, cellWidth - 24) * kPreviewHeight / kPreviewWidth) + 108;
        // ItemsWrapGrid caches its first measured cell. Updating only item.Width
        // leaves old wrap boundaries after a window resize; update the panel too.
        if (const auto panel = presets.ItemsPanelRoot().try_as<muxc::ItemsWrapGrid>())
        {
            panel.ItemWidth(cellWidth); panel.ItemHeight(cellHeight);
        }
        for (auto& card : presetCards)
        {
            card.item.Width(std::max(1.0, cellWidth - 8));
            card.item.Height(cellHeight - 8);
        }
    }
    mux::UIElement PreviewCollection(bool highContrast)
    {
        auto group = PreviewSurface();
        group.Width(40); group.Height(40);
        group.CornerRadius({9, 9, 9, 9});
        muxc::Canvas content;
        const auto layout = dock_collection_icon_rules::CalculateLayout({0, 0, 40, 40});
        const std::array<std::pair<std::wstring_view, std::wstring_view>, 4> icons{{
            {L"browser", L"\xE774"}, {L"mail", L"\xE715"},
            {L"media", L"\xE714"}, {L"terminal", L"\xE756"}}};
        for (std::size_t i = 0; i < icons.size(); ++i)
        {
            const auto cell = dock_collection_icon_rules::CellRect(layout, static_cast<int>(i % 2), static_cast<int>(i / 2));
            const auto asset = L"ms-appx:///Assets/Settings/Icons/preview-" + std::wstring(icons[i].first) + L".svg";
            auto icon = PreviewAppIcon(asset, icons[i].second, highContrast, cell.right - cell.left);
            muxc::Canvas::SetLeft(icon, cell.left);
            muxc::Canvas::SetTop(icon, cell.top);
            content.Children().Append(icon);
        }
        group.Child(content);
        return group;
    }
    muxc::StackPanel PreviewApps(const DockSettings& settings, bool highContrast, bool nativeTaskbar = false)
    {
        const auto position = nativeTaskbar ? nativeTaskbarPosition : settings.position;
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        muxc::StackPanel items;
        items.Orientation(vertical ? muxc::Orientation::Vertical : muxc::Orientation::Horizontal);
        items.Spacing(3);
        items.HorizontalAlignment(mux::HorizontalAlignment::Center);
        items.VerticalAlignment(mux::VerticalAlignment::Center);
        const double extent = nativeTaskbar ? 40 : 48;
        const auto append = [&](const mux::UIElement& icon, bool running = false) {
            muxc::Grid cell;
            cell.Width(extent); cell.Height(extent);
            cell.Children().Append(icon);
            if (running)
            {
                auto dot = mux::Markup::XamlReader::Load(
                    LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Width="4" Height="4" CornerRadius="2" Background="{ThemeResource TextFillColorPrimaryBrush}" />)").as<muxc::Border>();
                dot.HorizontalAlignment(position == DockPosition::Left ? mux::HorizontalAlignment::Left :
                    position == DockPosition::Right ? mux::HorizontalAlignment::Right : mux::HorizontalAlignment::Center);
                dot.VerticalAlignment(position == DockPosition::Top ? mux::VerticalAlignment::Top :
                    position == DockPosition::Bottom ? mux::VerticalAlignment::Bottom : mux::VerticalAlignment::Center);
                cell.Children().Append(dot);
            }
            items.Children().Append(cell);
        };
        const auto app = [&](std::wstring_view name, std::wstring_view fallback, bool running = false) {
            const auto asset = L"ms-appx:///Assets/Settings/Icons/preview-" + std::wstring(name) + L".svg";
            append(PreviewAppIcon(asset, fallback, highContrast, nativeTaskbar ? 30 : 40), running);
        };
        const auto divider = [&] {
            auto line = mux::Markup::XamlReader::Load(
                LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource DividerStrokeColorDefaultBrush}" />)").as<muxc::Border>();
            line.Width(vertical ? 28 : 1); line.Height(vertical ? 1 : 28);
            line.Margin({5, 5, 5, 5});
            line.HorizontalAlignment(mux::HorizontalAlignment::Center);
            line.VerticalAlignment(mux::VerticalAlignment::Center);
            items.Children().Append(line);
        };
        if (nativeTaskbar || ShowDockWindowsButton(settings))
        {
            append(PreviewStartIcon(nativeTaskbar ? 24 : 28, highContrast));
            if (!nativeTaskbar) divider();
        }
        app(L"browser", L"\xE774", true);
        app(nativeTaskbar ? L"mail" : L"terminal", nativeTaskbar ? L"\xE715" : L"\xE756", true);
        if (!nativeTaskbar) { append(PreviewCollection(highContrast)); divider(); }
        app(L"folder", L"\xE8B7");
        if (!nativeTaskbar)
        {
            append(PreviewGlyph(L"\xE721", 26));
            app(L"trash", L"\xE74D");
        }
        return items;
    }
    muxc::StackPanel PreviewSystemControls(bool vertical = false)
    {
        muxc::StackPanel status;
        status.Orientation(muxc::Orientation::Horizontal);
        status.Spacing(vertical ? 3 : 8);
        status.VerticalAlignment(mux::VerticalAlignment::Center);
        status.Children().Append(PreviewGlyph(L"\xE702", 13));
        status.Children().Append(PreviewGlyph(L"\xE767", 13));
        status.Children().Append(PreviewGlyph(L"\xE83F", 16));
        return status;
    }
    muxc::Grid PreviewStatusBar(const StatusBarSettings& settings, bool merged,
        const mux::UIElement& dockContent = nullptr)
    {
        // Demonstrate the controls using the real item builder. Performance
        // metrics clutter the miniature; suppress them only in this local copy.
        auto previewSettings = settings;
        previewSettings.cpu = previewSettings.memory = previewSettings.gpu = previewSettings.traffic = false;
        StatusBarSnapshot data;
        data.clock = L"2026/9/28   09:41";
        data.notifications.unreadCount = 0;
        data.network.emplace(); data.network->available = true;
        data.network->connectivity = "internet"; data.network->transport = "ethernet";
        data.audio.emplace(); data.audio->available = true; data.audio->volume = .45;
        data.power.emplace(); data.power->available = true; data.power->batteryPercent = 76;
        const auto items = BuildStatusBarItems(previewSettings, data);
        muxc::StackPanel left, center, right;
        for (const auto& panel : {left, center, right})
        {
            panel.Orientation(muxc::Orientation::Horizontal);
            panel.VerticalAlignment(mux::VerticalAlignment::Center);
        }
        mux::UIElement clock = nullptr, notification = nullptr;
        for (const auto& item : items)
        {
            if (merged && item.key == "quickSearch") continue;
            muxc::Grid cell;
            cell.Height(merged ? 44 : 32);
            if (item.key == "controlCenter")
            {
                muxc::StackPanel controls;
                controls.Orientation(muxc::Orientation::Horizontal);
                controls.Spacing(10);
                controls.VerticalAlignment(mux::VerticalAlignment::Center);
                controls.HorizontalAlignment(mux::HorizontalAlignment::Center);
                for (const auto& glyph : item.controlGlyphs) controls.Children().Append(PreviewFluentGlyph(glyph));
                cell.Width(92); cell.Children().Append(controls);
            }
            else if (!item.text.empty())
            {
                muxc::TextBlock text;
                text.Text(item.key == "clock" ? StatusBarClockDisplay(item.text, merged) : item.text);
                text.FontSize(12);
                text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                text.TextAlignment(mux::TextAlignment::Center);
                text.VerticalAlignment(mux::VerticalAlignment::Center);
                text.Margin({8, 0, 8, 0});
                cell.Children().Append(text);
            }
            else
            {
                auto icon = PreviewFluentGlyph(item.glyph);
                if (item.flipGlyph)
                {
                    muxm::ScaleTransform flip;
                    flip.ScaleY(-1);
                    icon.RenderTransformOrigin({.5f, .5f}); icon.RenderTransform(flip);
                }
                cell.Width(32); cell.Children().Append(icon);
            }
            if (item.key == "clock") clock = cell;
            else if (item.key == "notifications") notification = cell;
            else (item.left ? left : right).Children().Append(cell);
        }
        if (merged)
        {
            if (clock) right.Children().Append(clock);
            if (notification) right.Children().Append(notification);
        }
        else
        {
            if (clock && notification) { muxc::Grid balance; balance.Width(32); center.Children().Append(balance); }
            if (clock) center.Children().Append(clock);
            if (notification) center.Children().Append(notification);
        }
        muxc::Grid content;
        content.Padding({10, 0, 10, 0});
        for (int index = 0; index < 3; ++index)
        {
            muxc::ColumnDefinition column;
            column.Width(index == 1 && !merged ? mux::GridLengthHelper::Auto() :
                mux::GridLengthHelper::FromValueAndType(index == 1 ? 1.5 : 1.0, mux::GridUnitType::Star));
            content.ColumnDefinitions().Append(column);
        }
        auto leading = FitPreviewContent(left, mux::HorizontalAlignment::Left);
        auto middle = FitPreviewContent(merged ? dockContent : center.as<mux::UIElement>(), mux::HorizontalAlignment::Center);
        auto trailing = FitPreviewContent(right, mux::HorizontalAlignment::Right);
        muxc::Grid::SetColumn(middle, 1); muxc::Grid::SetColumn(trailing, 2);
        content.Children().Append(leading); content.Children().Append(middle); content.Children().Append(trailing);
        return content;
    }
    void AddPreviewBar(const muxc::Grid& layout, const GeneralSettings& values, const DockSettings& settings,
        bool dockBar, bool highContrast, bool merged = false)
    {
        const auto position = dockBar ? settings.position : values.statusBar.position;
        const bool island = dockBar && !settings.edgeAttached;
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        auto bar = PreviewSurface();
        // Edge bars are flush and square. Only the island has an inset/radius.
        bar.BorderThickness({0, 0, 0, 0});
        bar.CornerRadius(island ? mux::CornerRadius{14, 14, 14, 14} : mux::CornerRadius{0, 0, 0, 0});
        const double thickness = merged ? settings.mergedBarHeight : (dockBar ? kPreviewDockHeight : kPreviewStatusHeight);
        bar.HorizontalAlignment(vertical
            ? (position == DockPosition::Left ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Right)
            : mux::HorizontalAlignment::Stretch);
        bar.VerticalAlignment(vertical ? mux::VerticalAlignment::Stretch
            : (position == DockPosition::Top ? mux::VerticalAlignment::Top : mux::VerticalAlignment::Bottom));
        if (vertical) bar.Width(thickness); else bar.Height(thickness);
        mux::Thickness inset{};
        if (island) inset = {12, 12, 12, 12};
        if (dockBar && !merged && values.statusBar.enabled)
        {
            if (values.statusBar.position == DockPosition::Top) inset.Top += kPreviewStatusHeight;
            else inset.Bottom += kPreviewStatusHeight;
        }
        bar.Margin(inset);
        if (dockBar)
        {
            const auto apps = PreviewApps(settings, highContrast);
            if (island)
            {
                apps.Measure({2000, 2000});
                if (vertical) { bar.Height(apps.DesiredSize().Height + 20); bar.VerticalAlignment(mux::VerticalAlignment::Center); }
                else { bar.Width(apps.DesiredSize().Width + 20); bar.HorizontalAlignment(mux::HorizontalAlignment::Center); }
            }
            bar.Child(merged ? PreviewStatusBar(values.statusBar, true, apps).as<mux::UIElement>() :
                FitPreviewContent(apps, mux::HorizontalAlignment::Center).as<mux::UIElement>());
        }
        else bar.Child(PreviewStatusBar(values.statusBar, false));
        layout.Children().Append(bar);
    }
    muxc::Grid PreviewTaskbarContent(const DockSettings& settings, bool highContrast, bool vertical)
    {
        muxc::Grid content;
        const bool centered = !vertical && settings.systemTaskbarAlignment != 0;
        for (int index = 0; index < (centered ? 3 : 2); ++index)
        {
            const auto length = index == 1 ? mux::GridLengthHelper::Auto() :
                mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star);
            if (vertical) { muxc::RowDefinition row; row.Height(length); content.RowDefinitions().Append(row); }
            else { muxc::ColumnDefinition column; column.Width(length); content.ColumnDefinitions().Append(column); }
        }
        auto apps = FitPreviewContent(PreviewApps(settings, highContrast, true),
            !vertical && settings.systemTaskbarAlignment == 0 ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Center);
        apps.Margin({8, 4, 8, 4});
        if (centered) muxc::Grid::SetColumn(apps, 1);
        content.Children().Append(apps);
        muxc::StackPanel info;
        info.Orientation(vertical ? muxc::Orientation::Vertical : muxc::Orientation::Horizontal);
        info.Spacing(vertical ? 5 : 10);
        info.VerticalAlignment(vertical ? mux::VerticalAlignment::Bottom : mux::VerticalAlignment::Center);
        info.HorizontalAlignment(vertical ? mux::HorizontalAlignment::Center : mux::HorizontalAlignment::Right);
        info.Margin(vertical ? mux::Thickness{3, 8, 3, 8} : mux::Thickness{12, 0, 10, 0});
        info.Children().Append(PreviewGlyph(nativeTaskbarPosition == DockPosition::Top ? L"\xE70D" : L"\xE70E", 10));
        info.Children().Append(PreviewSystemControls(vertical));
        muxc::TextBlock clock;
        clock.Text(L"09:41\n2026/9/28"); clock.FontSize(10);
        clock.TextAlignment(vertical ? mux::TextAlignment::Center : mux::TextAlignment::Right);
        clock.VerticalAlignment(mux::VerticalAlignment::Center);
        info.Children().Append(clock);
        info.Children().Append(PreviewGlyph(L"\xE7F4", 13));
        if (vertical) muxc::Grid::SetRow(info, 1); else muxc::Grid::SetColumn(info, centered ? 2 : 1);
        content.Children().Append(info);
        return content;
    }
    void UpdatePreset()
    {
        const int index = presets.SelectedIndex();
        if (index < 0 || index >= static_cast<int>(kPresets.size())) return;
        const std::string key(kPresets[static_cast<std::size_t>(index)]);
        const std::string prefix = "settings.desktopStyle." + key;
        companionOptions.Visibility(key == "taskbar-dock" ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        presetChanges.Text(L(prefix + ".changes"));
        muxa::AutomationProperties::SetName(apply, L(prefix + ".title") + L": " + L("settings.desktopStyle.applyLayout"));
    }
    void RefreshPreviews()
    {
        const bool highContrast = IsHighContrastEnabled();
        for (std::size_t i = 0; i < presetCards.size(); ++i)
            RenderPreview(presetCards[i], kPresets[i], highContrast);
    }
    void RenderPreview(PresetCard& card, std::string_view key, bool highContrast)
    {
        card.layout.Children().Clear();
        card.wallpaper.Visibility(highContrast ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        auto previewGeneral = general;
        auto previewDock = dock;
        ApplyDesktopStylePreset(std::wstring(key.begin(), key.end()), previewGeneral, previewDock,
            static_cast<DockPosition>(std::max(0, companionPosition.SelectedIndex())), companionForm.SelectedIndex() == 1);
        const bool merged = previewGeneral.dockEnabled && previewGeneral.statusBar.enabled && previewDock.edgeAttached &&
            previewDock.position == previewGeneral.statusBar.position;
        if (previewGeneral.statusBar.enabled && !merged)
            AddPreviewBar(card.layout, previewGeneral, previewDock, false, highContrast);
        if (previewGeneral.dockEnabled)
            AddPreviewBar(card.layout, previewGeneral, previewDock, true, highContrast, merged);
        if (!previewDock.suppressSystemTaskbar)
        {
            // The native taskbar is illustrative: this preset preserves its
            // Windows-owned position; Dock follows the preset's draft choices.
            auto taskbar = PreviewSurface();
            const bool vertical = nativeTaskbarPosition == DockPosition::Left || nativeTaskbarPosition == DockPosition::Right;
            const double thickness = previewDock.systemTaskbarAutoHide ? 3.0 : (vertical ? 64.0 : 48.0);
            if (vertical) taskbar.Width(thickness); else taskbar.Height(thickness);
            taskbar.CornerRadius({0, 0, 0, 0});
            taskbar.BorderThickness({0, 0, 0, 0});
            taskbar.HorizontalAlignment(vertical
                ? (nativeTaskbarPosition == DockPosition::Left ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Right)
                : mux::HorizontalAlignment::Stretch);
            taskbar.VerticalAlignment(vertical ? mux::VerticalAlignment::Stretch
                : (nativeTaskbarPosition == DockPosition::Top ? mux::VerticalAlignment::Top : mux::VerticalAlignment::Bottom));
            if (!previewDock.systemTaskbarAutoHide) taskbar.Child(PreviewTaskbarContent(previewDock, highContrast, vertical));
            card.layout.Children().Append(taskbar);
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
            if (choice->focus == "desktopStyle.taskbar.displayMode" && choice->control.Items().Size() == 3)
                choice->control.Items().GetAt(2).as<muxc::ComboBoxItem>().IsEnabled(general.dockEnabled || dock.suppressSystemTaskbar);
            choice->row.SetEnabled(hasSnapshot && (!choice->enabled || choice->enabled(general, dock)));
        }
        apply.IsEnabled(hasSnapshot);
        syncing = previous;
        UpdatePreset();
        RefreshPreviews();
    }
    void Localize()
    {
        if (closed) return;
        const bool previous = syncing;
        syncing = true;
        for (std::size_t i = 0; i < presetCards.size(); ++i)
        {
            auto& card = presetCards[i];
            const auto prefix = "settings.desktopStyle." + std::string(kPresets[i]);
            card.title.Text(L(prefix + ".title"));
            card.description.Text(L(prefix + ".description"));
            muxa::AutomationProperties::SetName(card.item, card.title.Text());
            muxa::AutomationProperties::SetHelpText(card.item, card.description.Text());
            muxc::ToolTipService::SetToolTip(card.item, winrt::box_value(card.title.Text()));
        }
        muxa::AutomationProperties::SetName(presets, L("settings.desktopStyle.presets"));
        mergedHeight->RefreshLocalizedText();
        const auto localizeDraft = [this](const muxc::ComboBox& control, SettingRow& row,
            std::string_view label, std::initializer_list<std::string_view> options) {
            const int selectedIndex = control.SelectedIndex();
            row.SetText(L(label), {});
            muxa::AutomationProperties::SetName(control, row.label.Text());
            control.Items().Clear();
            for (const auto option : options) control.Items().Append(winrt::box_value(L(option)));
            control.SelectedIndex(selectedIndex >= 0 ? selectedIndex : 0);
        };
        localizeDraft(companionPosition, companionPositionRow, "app.settings.dock_position",
            {"app.dock.bottom", "app.dock.top", "app.dock.left", "app.dock.right"});
        localizeDraft(companionForm, companionFormRow, "app.dock.layout", {"app.dock.island", "app.dock.edge"});
        apply.Content(winrt::box_value(L("settings.desktopStyle.applyLayout")));
        changes.Header(winrt::box_value(L("settings.desktopStyle.changes")));
        adjustmentTitle.Text(L("settings.desktopStyle.adjustments"));
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
            for (const auto& option : choice->options) {
                muxc::ComboBoxItem item; item.Content(winrt::box_value(L(option))); choice->control.Items().Append(item);
            }
        }
        syncing = previous;
        Sync();
    }
    void Close() noexcept
    {
        if (closed) return;
        mergedHeight->Flush(); mergedHeight->Close();
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
void DesktopStylePagePresenter::SetActions(DockPageActions actions) { impl_->mergedHeight->SetActions(actions); impl_->actions = std::move(actions); }
bool DesktopStylePagePresenter::NeedsRecommendedAnimations(std::string_view preset) const
{
    return !impl_->closed && impl_->hasSnapshot &&
        NeedsDesktopStyleAnimations(std::wstring(preset.begin(), preset.end()), impl_->dock);
}
mux::UIElement DesktopStylePagePresenter::Content() const { return impl_->root; }
void DesktopStylePagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    const bool newSession = !impl_->hasSnapshot || impl_->generation != snapshot.generation;
    const int monitorCount = GetSystemMetrics(SM_CMONITORS);
    // Route transitions can publish without changing either settings domain.
    // Only a real transition selects the requested preview; normal snapshot
    // echoes must preserve any preset the user subsequently chose by hand.
    const bool routeChanged = newSession || !impl_->hasRoute ||
        impl_->lastRoute.page != snapshot.route.page || impl_->lastRoute.focusId != snapshot.route.focusId;
    impl_->lastRoute = snapshot.route;
    impl_->hasRoute = true;
    const bool presetNavigation = routeChanged && snapshot.route.page == SettingsPage::DesktopStyle;
    if (!newSession && impl_->generalRevision == snapshot.domainRevisions.general &&
        impl_->dockRevision == snapshot.domainRevisions.dock && impl_->systemTaskbarRevision == snapshot.domainRevisions.systemTaskbar &&
        impl_->monitorCount == monitorCount)
    {
        if (presetNavigation) impl_->SelectPresetForFocus(snapshot.route.focusId);
        return;
    }
    impl_->mergedHeight->Update(snapshot);
    impl_->monitorCount = monitorCount;
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
    impl_->mergedHeight->Flush();
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
    target("desktopStyle.dock.mergedBarHeight", impl_->mergedHeight->FocusTarget());
    for (const auto preset : kPresets) target("desktopStyle." + std::string(preset), impl_->presets);
    for (const auto& toggle : impl_->toggles) target(toggle->focus, toggle->control);
    for (const auto& choice : impl_->choices) target(choice->focus, choice->control);
}

} // namespace snowdesktop::winui
