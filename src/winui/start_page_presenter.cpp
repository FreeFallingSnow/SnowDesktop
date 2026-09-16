#include "pch.h"
#include "start_page_presenter.h"
#include "../l10n.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <array>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxa = mux::Automation;
using onboarding::Task;
namespace
{
constexpr std::array<const char*, 4> kTitles{
    L10N_KEY("start.collection.title"), L10N_KEY("start.application.title"),
    L10N_KEY("start.layout.title"), L10N_KEY("start.files.title")};
constexpr std::array<const char*, 4> kInstructions{
    L10N_KEY("start.collection.instructions"), L10N_KEY("start.application.instructions"),
    L10N_KEY("start.layout.instructions"), L10N_KEY("start.files.instructions")};
void Wrap(const muxc::TextBlock& text) { text.TextWrapping(mux::TextWrapping::Wrap); }
}

struct StartPagePresenter::Impl
{
    struct Step
    {
        muxc::Button button;
        muxc::StackPanel content;
        muxc::Border dot;
        muxc::TextBlock number, title;
        winrt::event_token token{};
    };
    struct Discovery
    {
        const char* titleKey;
        const char* descriptionKey;
        muxc::HyperlinkButton link;
        muxc::TextBlock title, description;
        winrt::event_token token{};
    };
    LocalizeCallback localize;
    StartPageActions actions;
    muxc::Border root;
    muxc::StackPanel body, basics, explore, detail;
    muxc::Grid track;
    muxc::SelectorBar tabs;
    muxc::SelectorBarItem basicsTab, exploreTab;
    muxc::TextBlock progress, instructions, collectionsText;
    muxc::Button practice;
    muxc::HyperlinkButton dismiss, review;
    muxc::InfoBar experimentNotice;
    std::array<Step, 4> steps;
    std::array<muxc::Border, 3> lines;
    std::vector<Discovery> discoveries;
    mux::ResourceDictionary styles{nullptr};
    winrt::event_token tabsToken{}, sizeToken{}, practiceToken{}, dismissToken{}, reviewToken{};
    std::uint64_t generation = 0, revision = 0;
    std::uint32_t completed = 0;
    std::size_t selected = 0;
    bool hasSnapshot = false, hasStatus = false, visible = false;
    bool active = false, closed = false, vertical = false, reviewing = false;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    mux::Style Style(const wchar_t* key) const { return styles.Lookup(winrt::box_value(key)).as<mux::Style>(); }

    Impl(LocalizeCallback callback, const mux::Style& cardStyle) : localize(std::move(callback))
    {
        // Native theme references remain live across light/dark/high-contrast changes.
        styles = mux::Markup::XamlReader::Load(LR"(<ResourceDictionary
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
 xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
 <Style x:Key="Step" TargetType="Button">
  <Setter Property="Background" Value="Transparent"/><Setter Property="BorderThickness" Value="0"/>
  <Setter Property="Padding" Value="8"/><Setter Property="MinHeight" Value="40"/><Setter Property="MinWidth" Value="40"/>
  <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
 </Style>
 <Style x:Key="Pending" TargetType="Border">
  <Setter Property="Background" Value="{ThemeResource ControlFillColorDefaultBrush}"/>
  <Setter Property="BorderBrush" Value="{ThemeResource ControlStrongStrokeColorDefaultBrush}"/>
 </Style>
 <Style x:Key="Current" TargetType="Border">
  <Setter Property="Background" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
  <Setter Property="BorderBrush" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
 </Style>
 <Style x:Key="Done" TargetType="Border">
  <Setter Property="Background" Value="{ThemeResource ControlFillColorDefaultBrush}"/>
  <Setter Property="BorderBrush" Value="{ThemeResource AccentTextFillColorPrimaryBrush}"/>
 </Style>
 <Style x:Key="Number" TargetType="TextBlock"><Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/></Style>
 <Style x:Key="CurrentNumber" TargetType="TextBlock"><Setter Property="Foreground" Value="{ThemeResource TextOnAccentFillColorPrimaryBrush}"/></Style>
 <Style x:Key="DoneNumber" TargetType="TextBlock"><Setter Property="Foreground" Value="{ThemeResource AccentTextFillColorPrimaryBrush}"/></Style>
 <Style x:Key="Line" TargetType="Border"><Setter Property="Background" Value="{ThemeResource ControlStrongStrokeColorDefaultBrush}"/></Style>
 <Style x:Key="DoneLine" TargetType="Border"><Setter Property="Background" Value="{ThemeResource AccentTextFillColorPrimaryBrush}"/></Style>
 </ResourceDictionary>)").as<mux::ResourceDictionary>();
        root.Style(cardStyle);
        root.Visibility(mux::Visibility::Collapsed);
        body.Spacing(12);
        root.Child(body);
        tabs.Items().Append(basicsTab);
        tabs.Items().Append(exploreTab);
        tabs.SelectedItem(basicsTab);
        body.Children().Append(tabs);
        experimentNotice.IsClosable(false);
        body.Children().Append(experimentNotice);
        basics.Spacing(12); explore.Spacing(8); detail.Spacing(12);
        Wrap(progress); Wrap(instructions); Wrap(collectionsText);
        progress.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        basics.Children().Append(progress);
        basics.Children().Append(review);
        basics.Children().Append(track);
        detail.Children().Append(instructions);
        practice.HorizontalAlignment(mux::HorizontalAlignment::Left);
        detail.Children().Append(practice);
        basics.Children().Append(detail);
        body.Children().Append(basics);
        body.Children().Append(explore);
        dismiss.HorizontalAlignment(mux::HorizontalAlignment::Right);
        body.Children().Append(dismiss);
        review.HorizontalAlignment(mux::HorizontalAlignment::Left);
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            auto& step = steps[i];
            step.button.Style(Style(L"Step"));
            step.button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            step.content.Spacing(8);
            step.dot.Width(24); step.dot.Height(24);
            step.dot.CornerRadius(mux::CornerRadius{12});
            step.dot.BorderThickness(mux::Thickness{1});
            step.number.HorizontalAlignment(mux::HorizontalAlignment::Center);
            step.number.VerticalAlignment(mux::VerticalAlignment::Center);
            step.number.FontSize(12);
            step.number.IsTextScaleFactorEnabled(false);
            step.dot.Child(step.number);
            Wrap(step.title);
            step.content.Children().Append(step.dot);
            step.content.Children().Append(step.title);
            step.button.Content(step.content);
            track.Children().Append(step.button);
            step.token = step.button.Click([this, i](auto&&, auto&&) {
                selected = i; reviewing = true; Render();
            });
            muxa::AutomationProperties::SetAutomationId(step.button,
                winrt::to_hstring("start." + std::string(onboarding::TaskKey(static_cast<Task>(i)))));
            if (i < lines.size()) { lines[i].IsHitTestVisible(false); track.Children().Append(lines[i]); }
        }
        Layout(false);
        sizeToken = track.SizeChanged([this](auto&&, const mux::SizeChangedEventArgs& args) {
            if (closed || args.NewSize().Width <= 0) return;
            const auto width = args.NewSize().Width;
            const bool narrow = width < 560;
            if (narrow != vertical) Layout(narrow);
            for (auto& step : steps)
                step.title.MaxWidth(std::max(32.0, narrow ? width - 56.0 : (width - 60.0) / 4.0 - 16.0));
        });
        tabsToken = tabs.SelectionChanged([this](auto&&, auto&&) { Render(); });
        practiceToken = practice.Click([this](auto&&, auto&&) {
            if (active && visible && hasSnapshot && !closed && actions.begin)
                actions.begin(generation, static_cast<Task>(selected));
        });
        dismissToken = dismiss.Click([this](auto&&, auto&&) {
            if (active && visible && hasSnapshot && !closed && actions.dismiss) actions.dismiss(generation);
        });
        reviewToken = review.Click([this](auto&&, auto&&) { reviewing = true; Render(); });
        // Collections are an introduction, with no shortcut that performs the lesson.
        explore.Children().Append(collectionsText);
        const auto add = [&](const char* title, const char* description, SettingsPage page, const char* focus = "") {
            Discovery item{title, description};
            muxc::StackPanel row;
            row.Spacing(2); Wrap(item.title); Wrap(item.description);
            item.link.Padding(mux::Thickness{0});
            item.link.HorizontalAlignment(mux::HorizontalAlignment::Left);
            item.link.Content(item.title);
            row.Children().Append(item.link); row.Children().Append(item.description);
            const auto route = SettingsRoute::ForPage(page, focus);
            item.token = item.link.Click([this, route](auto&&, auto&&) {
                if (active && hasSnapshot && !closed && actions.navigate) actions.navigate(route);
            });
            explore.Children().Append(row);
            discoveries.push_back(std::move(item));
        };
        add(L10N_KEY("start.explore.organize"), L10N_KEY("start.explore.organize.description"), SettingsPage::DesktopCategories);
        add(L10N_KEY("start.explore.dock"), L10N_KEY("start.explore.dock.description"), SettingsPage::Dock);
        add(L10N_KEY("start.explore.navigation"), L10N_KEY("start.explore.navigation.description"), SettingsPage::General, "general.quickNavigation");
        add(L10N_KEY("start.explore.pages"), L10N_KEY("start.explore.pages.description"), SettingsPage::DesktopPages);
        add(L10N_KEY("start.explore.appearance"), L10N_KEY("start.explore.appearance.description"), SettingsPage::AppearanceTheme);
        add(L10N_KEY("start.explore.widgets"), L10N_KEY("start.explore.widgets.description"), SettingsPage::Widgets);
        add(L10N_KEY("start.explore.backup"), L10N_KEY("start.explore.backup.description"), SettingsPage::BackupAndData);
        RefreshLocalizedText();
    }

    void Layout(bool narrow)
    {
        vertical = narrow;
        track.ColumnDefinitions().Clear(); track.RowDefinitions().Clear();
        for (int i = 0; i < 7; ++i)
        {
            if (narrow)
            {
                muxc::RowDefinition row;
                row.Height({i % 2 ? 12.0 : 1.0, i % 2 ? mux::GridUnitType::Pixel : mux::GridUnitType::Auto});
                track.RowDefinitions().Append(row);
            }
            else
            {
                muxc::ColumnDefinition column;
                column.Width({i % 2 ? 20.0 : 1.0, i % 2 ? mux::GridUnitType::Pixel : mux::GridUnitType::Star});
                track.ColumnDefinitions().Append(column);
            }
        }
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            auto& step = steps[i];
            muxc::Grid::SetRow(step.button, narrow ? static_cast<int>(i * 2) : 0);
            muxc::Grid::SetColumn(step.button, narrow ? 0 : static_cast<int>(i * 2));
            step.content.Orientation(narrow ? muxc::Orientation::Horizontal : muxc::Orientation::Vertical);
            step.dot.HorizontalAlignment(narrow ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Center);
            step.title.TextAlignment(narrow ? mux::TextAlignment::Left : mux::TextAlignment::Center);
            step.title.VerticalAlignment(mux::VerticalAlignment::Center);
            if (i >= lines.size()) continue;
            auto& line = lines[i];
            muxc::Grid::SetRow(line, narrow ? static_cast<int>(i * 2 + 1) : 0);
            muxc::Grid::SetColumn(line, narrow ? 0 : static_cast<int>(i * 2 + 1));
            line.Width(narrow ? 1.0 : 20.0); line.Height(narrow ? 12.0 : 1.0);
            line.HorizontalAlignment(narrow ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Center);
            line.VerticalAlignment(narrow ? mux::VerticalAlignment::Stretch : mux::VerticalAlignment::Top);
            line.Margin(narrow ? mux::Thickness{20, 0, 0, 0} : mux::Thickness{0, 20, 0, 0});
        }
    }

    void RefreshLocalizedText()
    {
        basicsTab.Text(L("start.basics")); exploreTab.Text(L("start.explore"));
        practice.Content(winrt::box_value(L("start.practice")));
        dismiss.Content(winrt::box_value(L("start.dismiss")));
        review.Content(winrt::box_value(L("start.review")));
        experimentNotice.Title(L("start.experiment"));
        experimentNotice.Message(L("start.experiment.description"));
        collectionsText.Text(L("start.explore.collections.description"));
        for (std::size_t i = 0; i < steps.size(); ++i) steps[i].title.Text(L(kTitles[i]));
        for (auto& item : discoveries)
        {
            item.title.Text(L(item.titleKey)); item.description.Text(L(item.descriptionKey));
            muxa::AutomationProperties::SetName(item.link, item.title.Text());
        }
        Render();
    }

    void Render()
    {
        if (closed) return;
        root.Visibility(visible ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        const bool basic = tabs.SelectedItem() == basicsTab;
        basics.Visibility(basic ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        explore.Visibility(basic ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        const bool allDone = (completed & onboarding::kAllSteps) == onboarding::kAllSteps;
        track.Visibility(allDone && !reviewing ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        detail.Visibility(track.Visibility());
        review.Visibility(allDone && !reviewing ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        unsigned count = 0;
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            auto& step = steps[i];
            const bool done = onboarding::Completed(completed, static_cast<Task>(i));
            if (done) ++count;
            step.dot.Style(Style(i == selected ? L"Current" : done ? L"Done" : L"Pending"));
            step.number.Style(Style(i == selected ? L"CurrentNumber" : done ? L"DoneNumber" : L"Number"));
            step.number.Text(done ? L"✓" : std::to_wstring(i + 1));
            muxa::AutomationProperties::SetName(step.button, L(kTitles[i]) + L", " +
                L(done ? L10N_KEY("start.completed") : i == selected ? L10N_KEY("start.next") : L10N_KEY("start.notStarted")));
            if (i < lines.size()) lines[i].Style(Style(done ? L"DoneLine" : L"Line"));
        }
        auto summary = allDone ? L("start.basicsCompleted") : L("start.progress");
        if (const auto pos = summary.find(L"{0}"); pos != std::wstring::npos) summary.replace(pos, 3, std::to_wstring(count));
        progress.Text(summary);
        auto text = L(kInstructions[selected]);
        if (selected == static_cast<std::size_t>(Task::Layout) && !onboarding::Completed(completed, Task::Layout))
        {
            if (completed & onboarding::kMoved) text += L"\n" + L("start.layout.resizeRemaining");
            else if (completed & onboarding::kResized) text += L"\n" + L("start.layout.moveRemaining");
        }
        instructions.Text(text);
        practice.IsEnabled(hasSnapshot && visible);
    }

    void Close()
    {
        if (closed) return;
        closed = true; active = false;
        tabs.SelectionChanged(tabsToken); track.SizeChanged(sizeToken);
        practice.Click(practiceToken); dismiss.Click(dismissToken); review.Click(reviewToken);
        for (auto& step : steps) step.button.Click(step.token);
        for (auto& item : discoveries) item.link.Click(item.token);
        actions = {};
    }
};

StartPagePresenter::StartPagePresenter(LocalizeCallback localize, const mux::Style& style)
    : impl_(std::make_unique<Impl>(std::move(localize), style)) {}
StartPagePresenter::~StartPagePresenter() { Close(); }
void StartPagePresenter::SetActions(StartPageActions actions) { impl_->actions = std::move(actions); }
void StartPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    if (!impl_->hasSnapshot || impl_->generation != snapshot.generation)
    { impl_->hasStatus = false; impl_->visible = false; impl_->reviewing = false; }
    impl_->generation = snapshot.generation; impl_->hasSnapshot = true;
    impl_->Render();
}
void StartPagePresenter::ApplyStatusPatch(const HomeAboutStatusPatch& patch)
{
    if (impl_->closed || !impl_->hasSnapshot || patch.generation != impl_->generation ||
        (impl_->hasStatus && patch.revision <= impl_->revision)) return;
    if (patch.onboardingSteps && (!impl_->hasStatus || *patch.onboardingSteps != impl_->completed))
    {
        impl_->completed = *patch.onboardingSteps;
        impl_->reviewing = false;
        impl_->selected = 0;
        while (impl_->selected < 3 && onboarding::Completed(impl_->completed, static_cast<Task>(impl_->selected)))
            ++impl_->selected;
    }
    impl_->hasStatus = true; impl_->revision = patch.revision;
    if (patch.onboardingVisible) impl_->visible = *patch.onboardingVisible;
    if (patch.temporaryInitializationEnabled) impl_->experimentNotice.IsOpen(*patch.temporaryInitializationEnabled);
    impl_->Render();
}
void StartPagePresenter::RefreshLocalizedText() { impl_->RefreshLocalizedText(); }
void StartPagePresenter::Activate(bool active) { impl_->active = active; }
void StartPagePresenter::Close() { if (impl_) impl_->Close(); }
mux::UIElement StartPagePresenter::Content() const { return impl_->root; }
void StartPagePresenter::SelectRoute(std::string_view id)
{
    if (!id.starts_with("start.")) return;
    impl_->tabs.SelectedItem(id == "start.explore" || id == "start.collections" ? impl_->exploreTab : impl_->basicsTab);
    for (std::size_t i = 0; i < impl_->steps.size(); ++i)
        if (id == "start." + std::string(onboarding::TaskKey(static_cast<Task>(i))))
        { impl_->selected = i; impl_->reviewing = true; }
    impl_->Render();
}
mux::FrameworkElement StartPagePresenter::FocusTarget(std::string_view id)
{
    if (id == "start.explore") return impl_->exploreTab;
    for (std::size_t i = 0; i < impl_->steps.size(); ++i)
        if (id == "start." + std::string(onboarding::TaskKey(static_cast<Task>(i)))) return impl_->steps[i].button;
    return impl_->basicsTab;
}
}
