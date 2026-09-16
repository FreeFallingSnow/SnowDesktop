#include "pch.h"
#include "start_page_presenter.h"
#include <algorithm>
#include <cmath>

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
        const wchar_t* asset;
        const wchar_t* glyph;
        muxc::SelectorBarItem tab;
    };
    struct Row
    {
        Topic topic;
        muxc::ListViewItem item;
        muxc::Grid layout, heading;
        muxc::StackPanel labels, details;
        muxc::TextBlock title, description, status, instructions, tips, prerequisite, launchLabel, settingsLabel;
        muxc::Button disclosure, launch, settings;
        muxc::FontIcon chevron;
        muxc::ContentControl icon;
        winrt::event_token disclosureToken{}, launchToken{}, settingsToken{};
        explicit Row(Topic value) : topic(value) {}
        ~Row()
        {
            disclosure.Click(disclosureToken); launch.Click(launchToken); settings.Click(settingsToken);
        }
    };
    LocalizeCallback localize;
    StartPageActions actions;
    muxc::Expander root;
    muxc::StackPanel header, body;
    muxc::TextBlock title, summary;
    muxc::ScrollViewer tabsScroll;
    muxc::SelectorBar tabs;
    muxc::ListView lessons;
    std::array<Group, 6> groups{{
        {Section::Basics, L"categories.svg", L"\xE8B7"},
        {Section::Files, L"desktop.svg", L"\xE7F4"},
        {Section::Settings, L"appearance-desktop-icons.svg", L"\xE8A9"},
        {Section::Dock, L"dock.svg", L"\xEBC8"},
        {Section::Navigation, L"search.svg", L"\xE721"},
        {Section::More, L"widgets.svg", L"\xE74C"},
    }};
    std::vector<std::unique_ptr<Row>> rows;
    Topic selected = Topic::Startup;
    Section section = Section::Basics;
    std::optional<Topic> expandedTopic, practiceTopic;
    mux::Style secondaryStyle{nullptr}, quietButtonStyle{nullptr};
    winrt::event_token tabsToken{}, themeToken{}, sizeToken{};
    std::int64_t expandedToken{};
    std::uint64_t generation = 0, revision = 0;
    std::uint32_t context = 0;
    bool hasSnapshot = false, hasStatus = false, active = false, closed = false;
    bool updating = false, applyingPreference = false, routeExpanded = false, expandedPreference = true;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static void Wrap(const muxc::TextBlock& text) { text.TextWrapping(mux::TextWrapping::Wrap); }
    static void Heading(const muxc::TextBlock& text)
    { Wrap(text); text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); }
    static void Column(const muxc::Grid& grid, double width, mux::GridUnitType unit)
    { muxc::ColumnDefinition column; column.Width({width, unit}); grid.ColumnDefinitions().Append(column); }
    static void AutoRow(const muxc::Grid& grid)
    { muxc::RowDefinition row; row.Height({1, mux::GridUnitType::Auto}); grid.RowDefinitions().Append(row); }

    explicit Impl(LocalizeCallback callback) : localize(std::move(callback))
    {
        secondaryStyle = mux::Markup::XamlReader::Load(LR"(<Style
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="TextBlock">
 <Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}"/>
 </Style>)").as<mux::Style>();
        quietButtonStyle = mux::Markup::XamlReader::Load(LR"(<Style
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Button">
 <Setter Property="Background" Value="Transparent"/><Setter Property="BorderThickness" Value="0"/>
 <Setter Property="Padding" Value="8,10"/><Setter Property="HorizontalContentAlignment" Value="Stretch"/>
 </Style>)").as<mux::Style>();
        root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        root.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        root.IsExpanded(true); root.IsEnabled(false);
        Heading(title); Wrap(summary); summary.Style(secondaryStyle);
        header.Spacing(4); header.Children().Append(title); header.Children().Append(summary); root.Header(header);
        body.Spacing(8); root.Content(body);
        sizeToken = root.SizeChanged([this](auto&&, const mux::SizeChangedEventArgs& args) {
            if (closed) return;
            // Expander's native template contributes 16 DIP on each side.
            const auto border = root.BorderThickness();
            body.Width(std::max(0.0, static_cast<double>(args.NewSize().Width) - 32.0 - border.Left - border.Right));
            if (const auto xamlRoot = root.XamlRoot())
                lessons.MaxHeight(std::clamp(static_cast<double>(xamlRoot.Size().Height) * .48, 200.0, 420.0));
            SizeRows();
        });
        tabsScroll.HorizontalScrollMode(muxc::ScrollMode::Enabled);
        tabsScroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
        tabsScroll.VerticalScrollMode(muxc::ScrollMode::Disabled);
        tabsScroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
        tabsScroll.Content(tabs); body.Children().Append(tabsScroll);
        for (auto& group : groups) tabs.Items().Append(group.tab);
        tabs.SelectedItem(groups[0].tab);
        // The native list owns scrolling, including expanded details.
        lessons.SelectionMode(muxc::ListViewSelectionMode::None);
        lessons.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        lessons.MaxHeight(360);
        lessons.ItemContainerStyle(mux::Markup::XamlReader::Load(LR"(<Style
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem">
 <Setter Property="HorizontalContentAlignment" Value="Stretch"/><Setter Property="Padding" Value="0"/>
 <Setter Property="Margin" Value="0,0,0,4"/>
 </Style>)").as<mux::Style>());
        body.Children().Append(lessons);
        tabsToken = tabs.SelectionChanged([this](auto&&, auto&&) {
            if (closed || updating) return;
            for (auto& group : groups) if (tabs.SelectedItem() == group.tab) section = group.section;
            PopulateLessons();
        });
        expandedToken = root.RegisterPropertyChangedCallback(muxc::Expander::IsExpandedProperty(), [this](auto&&, auto&&) {
            if (closed || applyingPreference) return;
            routeExpanded = false;
            if (active && hasSnapshot && hasStatus && actions.expandedChanged) actions.expandedChanged(generation, root.IsExpanded());
        });
        themeToken = root.ActualThemeChanged([this](auto&&, auto&&) { if (!closed) RefreshIcons(); });
        RefreshLocalizedText();
    }
    void SizeRows()
    {
        const double width = std::max(0.0, body.Width() - 20.0); // Native scrollbar gutter.
        if (!std::isfinite(width) || width <= 0) return;
        for (auto& row : rows)
        {
            row->layout.Width(width);
            // Long translations and large text remain clear of the action.
            const bool narrow = width < 440;
            muxc::Grid::SetRow(row->launch, narrow ? 1 : 0);
            muxc::Grid::SetColumn(row->launch, narrow ? 0 : 1);
            muxc::Grid::SetColumnSpan(row->launch, narrow ? 2 : 1);
            row->launch.HorizontalAlignment(narrow ? mux::HorizontalAlignment::Left : mux::HorizontalAlignment::Right);
            row->launch.Margin(narrow ? mux::Thickness{44, 0, 8, 8} : mux::Thickness{8, 0, 8, 0});
        }
    }
    const Group& GroupFor(Section value) const
    {
        for (const auto& group : groups) if (group.section == value) return group;
        return groups.front();
    }
    muxc::IconElement Icon(const wchar_t* asset, const wchar_t* glyph, bool highContrast)
    {
        if (highContrast) { muxc::FontIcon icon; icon.Glyph(glyph); icon.FontSize(20); return icon; }
        mux::Media::Imaging::SvgImageSource source;
        source.UriSource(winrt::Windows::Foundation::Uri{std::wstring(L"ms-appx:///Assets/Settings/Icons/") + asset});
        muxc::ImageIcon icon; icon.Source(source); icon.Width(24); icon.Height(24); return icon;
    }
    void RefreshIcons()
    {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        for (auto& group : groups) group.tab.Icon(Icon(group.asset, group.glyph, highContrast));
        for (auto& row : rows)
        {
            const auto& group = GroupFor(Find(row->topic)->section);
            const wchar_t* asset = group.asset;
            switch (row->topic)
            {
            case Topic::Startup: asset = L"general.svg"; break;
            case Topic::Grid: case Topic::Move: case Topic::Resize: asset = L"pages.svg"; break;
            case Topic::Beautify: asset = L"appearance-icon-beautification.svg"; break;
            case Topic::Theme: asset = L"appearance-theme.svg"; break;
            case Topic::Backup: asset = L"backup.svg"; break;
            default: break;
            }
            row->icon.Content(Icon(asset, group.glyph, highContrast));
        }
    }
    void PopulateLessons()
    {
        rows.clear(); lessons.Items().Clear();
        for (const auto& lesson : kLessons)
        {
            if (lesson.section != section) continue;
            auto row = std::make_unique<Row>(lesson.topic);
            auto* r = row.get();
            Column(r->layout, 1, mux::GridUnitType::Star); Column(r->layout, 1, mux::GridUnitType::Auto);
            AutoRow(r->layout); AutoRow(r->layout); AutoRow(r->layout);
            Column(r->heading, 24, mux::GridUnitType::Pixel); Column(r->heading, 1, mux::GridUnitType::Star);
            Column(r->heading, 16, mux::GridUnitType::Pixel); r->heading.ColumnSpacing(12);
            r->icon.VerticalAlignment(mux::VerticalAlignment::Top); r->icon.Margin({0, 3, 0, 0});
            r->heading.Children().Append(r->icon);
            Heading(r->title); r->title.Text(L(lesson.title));
            Wrap(r->description); r->description.Style(secondaryStyle); r->description.Text(L(lesson.description));
            r->description.MaxLines(2); r->description.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            Wrap(r->status); r->status.Style(secondaryStyle);
            r->labels.Spacing(3); r->labels.Children().Append(r->title); r->labels.Children().Append(r->description); r->labels.Children().Append(r->status);
            muxc::Grid::SetColumn(r->labels, 1); r->heading.Children().Append(r->labels);
            r->chevron.FontSize(12); muxc::Grid::SetColumn(r->chevron, 2); r->heading.Children().Append(r->chevron);
            r->disclosure.Style(quietButtonStyle); r->disclosure.Content(r->heading);
            r->disclosure.HorizontalAlignment(mux::HorizontalAlignment::Stretch); r->disclosure.MinHeight(64);
            r->layout.Children().Append(r->disclosure);
            r->launch.MinHeight(36); r->launch.VerticalAlignment(mux::VerticalAlignment::Center);
            Wrap(r->launchLabel); r->launchLabel.Text(L(lesson.practice ? "start.practiceShort" : "start.openSettings"));
            r->launch.Content(r->launchLabel); r->launch.MaxWidth(180);
            muxc::Grid::SetColumn(r->launch, 1); r->layout.Children().Append(r->launch);
            r->details.Spacing(10); r->details.Margin({44, 0, 12, 16});
            Wrap(r->prerequisite); r->details.Children().Append(r->prerequisite);
            Wrap(r->instructions); r->instructions.Text(L(lesson.instructions)); r->details.Children().Append(r->instructions);
            Wrap(r->tips); r->tips.Style(secondaryStyle); r->tips.Text(L(lesson.tips)); r->details.Children().Append(r->tips);
            Wrap(r->settingsLabel); r->settingsLabel.Text(L("start.openSettings")); r->settings.Content(r->settingsLabel);
            r->settings.HorizontalAlignment(mux::HorizontalAlignment::Left);
            r->settings.Visibility(lesson.practice && HasSettings(lesson) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            r->details.Children().Append(r->settings);
            muxc::Grid::SetRow(r->details, 2); muxc::Grid::SetColumnSpan(r->details, 2); r->layout.Children().Append(r->details);
            r->item.Content(r->layout); lessons.Items().Append(r->item);
            muxa::AutomationProperties::SetName(r->launch, L(lesson.practice ? "start.practice" : "start.openSettings") + L" · " + L(lesson.title));
            r->disclosureToken = r->disclosure.Click([this, topic = lesson.topic](auto&&, auto&&) {
                if (closed) return;
                selected = topic; expandedTopic = expandedTopic == topic ? std::nullopt : std::optional{topic}; Render();
            });
            r->launchToken = r->launch.Click([this, topic = lesson.topic](auto&&, auto&&) {
                if (closed || !active || !hasSnapshot || !hasStatus || !actions.begin) return;
                selected = topic; actions.begin(generation, topic);
            });
            r->settingsToken = r->settings.Click([this, topic = lesson.topic](auto&&, auto&&) {
                if (!closed && active && actions.navigate)
                { const auto& lesson = *Find(topic); actions.navigate(SettingsRoute::ForPage(lesson.settingsPage, lesson.settingsFocus)); }
            });
            rows.push_back(std::move(row));
        }
        SizeRows(); RefreshIcons(); Render();
    }
    void RefreshLocalizedText()
    {
        title.Text(L("start.title")); summary.Text(L("start.description"));
        for (auto& group : groups) group.tab.Text(L(SectionTitle(group.section)));
        muxa::AutomationProperties::SetName(root, title.Text()); muxa::AutomationProperties::SetHelpText(root, summary.Text());
        muxa::AutomationProperties::SetName(lessons, L("start.choose")); PopulateLessons();
    }
    void Render()
    {
        if (closed) return;
        root.IsEnabled(hasSnapshot && hasStatus);
        for (auto& row : rows)
        {
            const auto& lesson = *Find(row->topic);
            const bool expanded = expandedTopic == row->topic, current = practiceTopic == row->topic;
            row->details.Visibility(expanded ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            row->chevron.Glyph(expanded ? L"\xE70E" : L"\xE70D");
            row->status.Text(current ? L("start.currentPractice") : L"");
            row->status.Visibility(current ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            row->launchLabel.Text(L(current && lesson.practice ? "start.resume" : lesson.practice ? "start.practiceShort" : "start.openSettings"));
            row->description.MaxLines(expanded ? 0 : 2);
            muxa::AutomationProperties::SetName(row->disclosure, L(lesson.title) + L" · " + L(expanded ? "start.hideDetails" : "start.details"));
            const auto missing = MissingPrerequisite(lesson, context);
            row->prerequisite.Visibility(missing ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            if (missing) row->prerequisite.Text(L(PrerequisiteText(*missing)));
        }
    }
    void Close()
    {
        if (closed) return;
        closed = true; active = false;
        root.UnregisterPropertyChangedCallback(muxc::Expander::IsExpandedProperty(), expandedToken);
        root.ActualThemeChanged(themeToken); root.SizeChanged(sizeToken); tabs.SelectionChanged(tabsToken);
        rows.clear(); actions = {};
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
    if (patch.usageGuideTopic) impl_->practiceTopic = ParseTopic(*patch.usageGuideTopic);
    impl_->hasStatus = true; impl_->revision = patch.revision; impl_->Render();
}
void StartPagePresenter::RefreshLocalizedText() { impl_->RefreshLocalizedText(); }
void StartPagePresenter::Activate(bool active) { impl_->active = active; }
void StartPagePresenter::Close() { if (impl_) impl_->Close(); }
mux::UIElement StartPagePresenter::Content() const { return impl_->root; }
void StartPagePresenter::SelectRoute(std::string_view id)
{
    if (!id.starts_with("start.")) return;
    const auto oldSection = impl_->section;
    if (id == "start.basics") impl_->section = Section::Basics;
    else if (id == "start.explore") impl_->section = Section::Settings;
    else if (id == "start.welcome")
    {
        impl_->selected = Topic::Startup; impl_->section = Section::Basics;
        impl_->routeExpanded = false; impl_->applyingPreference = true;
        impl_->root.IsExpanded(impl_->expandedPreference); impl_->applyingPreference = false;
    }
    else if (const auto topic = ParseTopic(id.substr(6)))
    { impl_->selected = *topic; impl_->section = Find(*topic)->section; impl_->expandedTopic = *topic; }
    if (id != "start.welcome")
    {
        impl_->routeExpanded = true; impl_->applyingPreference = true;
        impl_->root.IsExpanded(true); impl_->applyingPreference = false;
    }
    impl_->updating = true;
    for (auto& group : impl_->groups) if (group.section == impl_->section) impl_->tabs.SelectedItem(group.tab);
    impl_->updating = false;
    if (oldSection != impl_->section) impl_->PopulateLessons(); else impl_->Render();
    if (id != "start.welcome")
        for (auto& row : impl_->rows) if (row->topic == impl_->selected) impl_->lessons.ScrollIntoView(row->item);
}
}
