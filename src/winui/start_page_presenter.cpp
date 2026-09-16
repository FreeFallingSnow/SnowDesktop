#include "pch.h"
#include "start_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxa = mux::Automation;
using namespace usage_guide;

struct StartPagePresenter::Impl
{
    struct Group
    {
        Section section;
        const char* title;
        const wchar_t* asset;
        const wchar_t* glyph;
        muxc::SelectorBarItem tab;
    };
    LocalizeCallback localize;
    StartPageActions actions;
    muxc::Expander root;
    muxc::StackPanel header, body;
    muxc::TextBlock title, summary, description, stepsLabel, instructions, tipsLabel, tips;
    muxc::ScrollViewer tabsScroll;
    muxc::SelectorBar tabs;
    muxc::ComboBox lessons;
    muxc::InfoBar prerequisite;
    muxc::Button prepare, practice, settings;
    muxc::TextBlock prepareLabel, practiceLabel, settingsLabel;
    muxc::Grid buttons;
    std::array<Group, 6> groups{{
        {Section::Basics, L10N_KEY("start.basics"), L"categories.svg", L"\xE8B7"},
        {Section::Files, L10N_KEY("start.section.files"), L"desktop.svg", L"\xE7F4"},
        {Section::Settings, L10N_KEY("start.section.settings"), L"appearance-desktop-icons.svg", L"\xE8A9"},
        {Section::Dock, L10N_KEY("start.section.dock"), L"dock.svg", L"\xEBC8"},
        {Section::Navigation, L10N_KEY("start.section.navigation"), L"search.svg", L"\xE721"},
        {Section::More, L10N_KEY("start.section.more"), L"widgets.svg", L"\xE74C"},
    }};
    std::vector<Topic> listedTopics;
    Topic selected = Topic::Startup;
    Section section = Section::Basics;
    mux::Style secondaryStyle{nullptr};
    winrt::event_token tabsToken{}, lessonsToken{}, prepareToken{}, practiceToken{}, settingsToken{}, themeToken{}, sizeToken{};
    std::int64_t expandedToken{};
    std::uint64_t generation = 0, revision = 0;
    std::uint32_t context = 0;
    bool hasSnapshot = false, hasStatus = false, active = false, closed = false;
    bool updating = false, applyingPreference = false, routeExpanded = false, expandedPreference = true;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static void Wrap(const muxc::TextBlock& text) { text.TextWrapping(mux::TextWrapping::Wrap); }
    static void Heading(const muxc::TextBlock& text)
    { Wrap(text); text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); }

    explicit Impl(LocalizeCallback callback) : localize(std::move(callback))
    {
        secondaryStyle = mux::Markup::XamlReader::Load(LR"(<Style
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="TextBlock">
 <Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}"/>
 </Style>)").as<mux::Style>();
        root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        root.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        root.IsExpanded(true); root.IsEnabled(false);
        Heading(title); Wrap(summary); summary.Style(secondaryStyle);
        header.Spacing(4); header.Children().Append(title); header.Children().Append(summary); root.Header(header);
        body.Spacing(12); root.Content(body);
        sizeToken = root.SizeChanged([this](auto&&, const mux::SizeChangedEventArgs& args) {
            if (closed) return;
            // Match the existing widget settings expanders: their template
            // contributes 16 DIP per side. Constrain long, wrapped lessons.
            const auto border = root.BorderThickness();
            body.Width(std::max(0.0, static_cast<double>(args.NewSize().Width) - 32.0 - border.Left - border.Right));
        });
        tabsScroll.HorizontalScrollMode(muxc::ScrollMode::Enabled);
        tabsScroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
        tabsScroll.VerticalScrollMode(muxc::ScrollMode::Disabled);
        tabsScroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
        tabsScroll.Content(tabs); body.Children().Append(tabsScroll);
        for (auto& group : groups) tabs.Items().Append(group.tab);
        tabs.SelectedItem(groups[0].tab);
        lessons.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        lessons.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        body.Children().Append(lessons);
        Wrap(description); description.Style(secondaryStyle); body.Children().Append(description);
        prerequisite.IsClosable(false); prerequisite.Severity(muxc::InfoBarSeverity::Informational);
        Wrap(prepareLabel); prepare.Content(prepareLabel); prepare.MinHeight(40);
        prerequisite.ActionButton(prepare); body.Children().Append(prerequisite);
        Heading(stepsLabel); Wrap(instructions); Heading(tipsLabel); Wrap(tips); tips.Style(secondaryStyle);
        body.Children().Append(stepsLabel); body.Children().Append(instructions);
        body.Children().Append(tipsLabel); body.Children().Append(tips);
        Wrap(practiceLabel); Wrap(settingsLabel); practice.Content(practiceLabel); settings.Content(settingsLabel);
        practice.MinHeight(40); settings.MinHeight(40);
        practice.HorizontalAlignment(mux::HorizontalAlignment::Left); settings.HorizontalAlignment(mux::HorizontalAlignment::Left);
        for (int i = 0; i < 2; ++i) { muxc::ColumnDefinition col; col.Width({1, mux::GridUnitType::Star}); buttons.ColumnDefinitions().Append(col); }
        buttons.ColumnSpacing(12); buttons.Children().Append(practice); muxc::Grid::SetColumn(settings, 1);
        buttons.Children().Append(settings); body.Children().Append(buttons);
        tabsToken = tabs.SelectionChanged([this](auto&&, auto&&) {
            if (closed || updating) return;
            for (auto& group : groups) if (tabs.SelectedItem() == group.tab) section = group.section;
            PopulateLessons();
        });
        lessonsToken = lessons.SelectionChanged([this](auto&&, auto&&) {
            if (closed || updating) return;
            const auto index = lessons.SelectedIndex();
            if (index >= 0 && static_cast<std::size_t>(index) < listedTopics.size()) selected = listedTopics[index];
            Render();
        });
        expandedToken = root.RegisterPropertyChangedCallback(muxc::Expander::IsExpandedProperty(), [this](auto&&, auto&&) {
            if (closed || applyingPreference) return;
            routeExpanded = false;
            if (active && hasSnapshot && hasStatus && actions.expandedChanged) actions.expandedChanged(generation, root.IsExpanded());
        });
        prepareToken = prepare.Click([this](auto&&, auto&&) {
            if (!active || closed || !actions.navigate) return;
            if (const auto missing = MissingPrerequisite(*Find(selected), context)) actions.navigate(PrerequisiteRoute(*missing));
        });
        practiceToken = practice.Click([this](auto&&, auto&&) {
            if (active && hasSnapshot && hasStatus && !closed && actions.begin && !MissingPrerequisite(*Find(selected), context))
                actions.begin(generation, selected);
        });
        settingsToken = settings.Click([this](auto&&, auto&&) {
            if (active && hasSnapshot && !closed && actions.navigate)
            {
                const auto& lesson = *Find(selected);
                actions.navigate(SettingsRoute::ForPage(lesson.settingsPage, lesson.settingsFocus));
            }
        });
        themeToken = root.ActualThemeChanged([this](auto&&, auto&&) { if (!closed) RefreshIcons(); });
        RefreshIcons(); RefreshLocalizedText();
    }
    void RefreshIcons()
    {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        for (auto& group : groups)
        {
            if (highContrast) { muxc::FontIcon icon; icon.Glyph(group.glyph); group.tab.Icon(icon); }
            else
            {
                mux::Media::Imaging::SvgImageSource source;
                source.UriSource(winrt::Windows::Foundation::Uri{std::wstring(L"ms-appx:///Assets/Settings/Icons/") + group.asset});
                muxc::ImageIcon icon; icon.Source(source); icon.Width(20); icon.Height(20); group.tab.Icon(icon);
            }
        }
    }
    void PopulateLessons()
    {
        updating = true; lessons.Items().Clear(); listedTopics.clear();
        int selection = 0;
        for (const auto& lesson : kLessons)
        {
            if (lesson.section != section) continue;
            if (lesson.topic == selected) selection = static_cast<int>(listedTopics.size());
            muxc::TextBlock label; Wrap(label); label.Text(L(lesson.title));
            muxc::ComboBoxItem item; item.Content(label); lessons.Items().Append(item); listedTopics.push_back(lesson.topic);
        }
        lessons.SelectedIndex(selection); selected = listedTopics[selection]; updating = false; Render();
    }
    void RefreshLocalizedText()
    {
        title.Text(L("start.title")); summary.Text(L("start.description"));
        for (auto& group : groups) group.tab.Text(L(group.title));
        lessons.Header(winrt::box_value(L("start.choose")));
        stepsLabel.Text(L("start.steps")); tipsLabel.Text(L("start.tips"));
        prepareLabel.Text(L("start.prepare")); practiceLabel.Text(L("start.practice")); settingsLabel.Text(L("start.openSettings"));
        muxa::AutomationProperties::SetName(root, title.Text()); muxa::AutomationProperties::SetHelpText(root, summary.Text());
        muxa::AutomationProperties::SetName(lessons, L("start.choose")); PopulateLessons();
    }
    void Render()
    {
        if (closed) return;
        root.IsEnabled(hasSnapshot && hasStatus);
        const auto& lesson = *Find(selected);
        description.Text(L(lesson.description)); instructions.Text(L(lesson.instructions)); tips.Text(L(lesson.tips));
        const auto missing = MissingPrerequisite(lesson, context);
        prerequisite.IsOpen(hasStatus && missing.has_value());
        prerequisite.Visibility(hasStatus && missing ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (missing) prerequisite.Message(L(PrerequisiteText(*missing)));
        practice.Visibility(lesson.practice ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        practice.IsEnabled(hasStatus && !missing);
        const bool hasSettings = !lesson.practice || lesson.settingsPage != SettingsPage::General || *lesson.settingsFocus;
        settings.Visibility(hasSettings ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        muxc::Grid::SetColumn(settings, lesson.practice ? 1 : 0);
        muxa::AutomationProperties::SetName(practice, L("start.practice") + L" · " + L(lesson.title));
    }
    void Close()
    {
        if (closed) return;
        closed = true; active = false;
        root.UnregisterPropertyChangedCallback(muxc::Expander::IsExpandedProperty(), expandedToken);
        root.ActualThemeChanged(themeToken); root.SizeChanged(sizeToken);
        tabs.SelectionChanged(tabsToken); lessons.SelectionChanged(lessonsToken);
        prepare.Click(prepareToken); practice.Click(practiceToken); settings.Click(settingsToken); actions = {};
    }
};
StartPagePresenter::StartPagePresenter(LocalizeCallback localize, const mux::Style&, const mux::Style&)
    : impl_(std::make_unique<Impl>(std::move(localize))) {}
StartPagePresenter::~StartPagePresenter() { Close(); }
void StartPagePresenter::SetActions(StartPageActions actions) { impl_->actions = std::move(actions); }
void StartPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    if (!impl_->hasSnapshot || impl_->generation != snapshot.generation) impl_->hasStatus = false;
    impl_->generation = snapshot.generation; impl_->hasSnapshot = true; impl_->Render();
}
void StartPagePresenter::ApplyStatusPatch(const HomeAboutStatusPatch& patch)
{
    if (impl_->closed || !impl_->hasSnapshot || patch.generation != impl_->generation || (impl_->hasStatus && patch.revision <= impl_->revision)) return;
    if (patch.usageGuideExpanded)
    {
        impl_->expandedPreference = *patch.usageGuideExpanded;
        impl_->applyingPreference = true; impl_->root.IsExpanded(impl_->routeExpanded || *patch.usageGuideExpanded); impl_->applyingPreference = false;
    }
    if (patch.usageGuideContext) impl_->context = *patch.usageGuideContext;
    impl_->hasStatus = true; impl_->revision = patch.revision; impl_->Render();
}
void StartPagePresenter::RefreshLocalizedText() { impl_->RefreshLocalizedText(); }
void StartPagePresenter::Activate(bool active) { impl_->active = active; }
void StartPagePresenter::Close() { if (impl_) impl_->Close(); }
mux::UIElement StartPagePresenter::Content() const { return impl_->root; }
void StartPagePresenter::SelectRoute(std::string_view id)
{
    if (!id.starts_with("start.")) return;
    if (id == "start.basics") impl_->section = Section::Basics;
    else if (id == "start.explore") impl_->section = Section::Settings;
    else if (id == "start.welcome")
    {
        impl_->selected = Topic::Startup; impl_->section = Section::Basics;
        impl_->routeExpanded = false; impl_->applyingPreference = true;
        impl_->root.IsExpanded(impl_->expandedPreference); impl_->applyingPreference = false;
    }
    else if (const auto topic = ParseTopic(id.substr(6))) { impl_->selected = *topic; impl_->section = Find(*topic)->section; }
    if (id != "start.welcome")
    {
        impl_->routeExpanded = true; impl_->applyingPreference = true;
        impl_->root.IsExpanded(true); impl_->applyingPreference = false;
    }
    impl_->updating = true;
    for (auto& group : impl_->groups) if (group.section == impl_->section) impl_->tabs.SelectedItem(group.tab);
    impl_->updating = false; impl_->PopulateLessons();
}
}
