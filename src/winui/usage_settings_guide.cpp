#include "pch.h"
#include "usage_settings_guide.h"
#include "../usage_guide_settings.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxa = mux::Automation;
using namespace usage_guide;

struct UsageSettingsGuide::Impl
{
    struct Row
    {
        StaticSettingSearchDescriptor entry;
        muxc::ListViewItem item;
        muxc::Grid grid, heading;
        muxc::StackPanel labels;
        muxc::Button disclosure, open;
        muxc::TextBlock title, description, detail, openLabel;
        muxc::FontIcon chevron;
        winrt::event_token disclosureToken{}, openToken{};
        ~Row() { disclosure.Click(disclosureToken); open.Click(openToken); }
    };
    Localize localize;
    std::function<std::vector<StaticSettingSearchDescriptor>()> source;
    std::function<void(const SettingsRoute&)> navigate;
    muxc::StackPanel root;
    muxc::ScrollViewer tabsScroll;
    muxc::SelectorBar tabs;
    std::array<muxc::SelectorBarItem, kIndexModules.size()> tabItems;
    muxc::TextBlock introduction;
    muxc::ListView list;
    mux::Style secondary{nullptr}, quiet{nullptr};
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<StaticSettingSearchDescriptor> entries;
    std::string expandedId;
    Module module = Module::General;
    double width = 0;
    bool closed = false, updating = false;
    winrt::event_token tabToken{}, themeToken{};

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static void Wrap(const muxc::TextBlock& text) { text.TextWrapping(mux::TextWrapping::Wrap); }
    static void Column(const muxc::Grid& grid, double size, mux::GridUnitType unit)
    { muxc::ColumnDefinition column; column.Width({size, unit}); grid.ColumnDefinitions().Append(column); }
    static void AutoRow(const muxc::Grid& grid)
    { muxc::RowDefinition row; row.Height({1, mux::GridUnitType::Auto}); grid.RowDefinitions().Append(row); }

    explicit Impl(Localize callback) : localize(std::move(callback))
    {
        secondary = mux::Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="TextBlock"><Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}"/></Style>)").as<mux::Style>();
        quiet = mux::Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Button"><Setter Property="Background" Value="Transparent"/><Setter Property="BorderThickness" Value="0"/><Setter Property="Padding" Value="8,10"/><Setter Property="HorizontalContentAlignment" Value="Stretch"/></Style>)").as<mux::Style>();
        root.Spacing(8);
        tabsScroll.HorizontalScrollMode(muxc::ScrollMode::Enabled);
        tabsScroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
        tabsScroll.VerticalScrollMode(muxc::ScrollMode::Disabled);
        tabsScroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
        tabsScroll.Content(tabs); root.Children().Append(tabsScroll);
        for (const auto& tab : tabItems) tabs.Items().Append(tab);
        tabs.SelectedItem(tabItems.front());
        Wrap(introduction); introduction.Style(secondary); introduction.Margin({8, 0, 8, 4});
        root.Children().Append(introduction);
        list.SelectionMode(muxc::ListViewSelectionMode::None);
        list.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch); list.MaxHeight(360);
        list.ItemContainerStyle(mux::Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem"><Setter Property="HorizontalContentAlignment" Value="Stretch"/><Setter Property="Padding" Value="0"/><Setter Property="Margin" Value="0,0,0,4"/></Style>)").as<mux::Style>());
        root.Children().Append(list);
        tabToken = tabs.SelectionChanged([this](auto&&, auto&&) {
            if (closed || updating) return;
            for (std::size_t i = 0; i < tabItems.size(); ++i)
                if (tabs.SelectedItem() == tabItems[i]) module = kIndexModules[i].id;
            Populate();
        });
        themeToken = root.ActualThemeChanged([this](auto&&, auto&&) { if (!closed) RefreshIcons(); });
    }
    void RefreshIcons()
    {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        for (std::size_t i = 0; i < tabItems.size(); ++i)
        {
            if (highContrast)
            { muxc::FontIcon icon; icon.Glyph(kIndexModules[i].glyph); icon.FontSize(18); tabItems[i].Icon(icon); }
            else
            {
                mux::Media::Imaging::SvgImageSource image;
                image.UriSource(winrt::Windows::Foundation::Uri{std::wstring(L"ms-appx:///Assets/Settings/Icons/") + kIndexModules[i].asset});
                muxc::ImageIcon icon; icon.Source(image); icon.Width(20); icon.Height(20); tabItems[i].Icon(icon);
            }
        }
    }
    void SizeRows()
    {
        if (!std::isfinite(width) || width <= 20) return;
        for (auto& row : rows)
        {
            row->grid.Width(width - 20);
            const bool narrow = width < 480;
            muxc::Grid::SetRow(row->open, narrow ? 1 : 0);
            muxc::Grid::SetColumn(row->open, narrow ? 0 : 1);
            muxc::Grid::SetColumnSpan(row->open, narrow ? 2 : 1);
            row->open.Margin(narrow ? mux::Thickness{8, 0, 8, 8} : mux::Thickness{8, 8, 8, 0});
        }
    }
    void RenderDetails()
    {
        for (auto& row : rows)
        {
            const bool expanded = expandedId == row->entry.focusId;
            row->description.MaxLines(expanded ? 0 : 2);
            row->detail.Visibility(expanded && !row->detail.Text().empty() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            row->chevron.Glyph(expanded ? L"\xE70E" : L"\xE70D");
            muxa::AutomationProperties::SetName(row->disclosure, row->entry.label + L" · " + L(expanded ? "start.hideDetails" : "start.details"));
        }
    }
    void Populate()
    {
        if (closed) return;
        rows.clear(); list.Items().Clear();
        for (const auto& group : kIndexModules) if (group.id == module) introduction.Text(L(group.advice));
        for (const auto& entry : entries)
        {
            if (ClassifySetting(entry) != module) continue;
            auto row = std::make_unique<Row>(); auto* r = row.get(); r->entry = entry;
            Column(r->grid, 1, mux::GridUnitType::Star); Column(r->grid, 1, mux::GridUnitType::Auto);
            AutoRow(r->grid); AutoRow(r->grid); AutoRow(r->grid);
            Column(r->heading, 1, mux::GridUnitType::Star); Column(r->heading, 16, mux::GridUnitType::Pixel);
            r->heading.ColumnSpacing(12);
            Wrap(r->title); r->title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            r->title.Text(entry.label); Wrap(r->description); r->description.Style(secondary);
            r->description.Text(entry.description); r->description.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            r->labels.Spacing(3); r->labels.Children().Append(r->title); r->labels.Children().Append(r->description);
            r->heading.Children().Append(r->labels); r->chevron.FontSize(12);
            muxc::Grid::SetColumn(r->chevron, 1); r->heading.Children().Append(r->chevron);
            r->disclosure.Style(quiet); r->disclosure.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            r->disclosure.MinHeight(60); r->disclosure.Content(r->heading); r->grid.Children().Append(r->disclosure);
            Wrap(r->openLabel); r->openLabel.Text(L("start.openSettings")); r->open.Content(r->openLabel);
            r->open.MinHeight(36); r->open.MaxWidth(180); r->open.VerticalAlignment(mux::VerticalAlignment::Top);
            r->open.HorizontalAlignment(mux::HorizontalAlignment::Left); r->grid.Children().Append(r->open);
            Wrap(r->detail); r->detail.Margin({8, 0, 12, 16});
            if (const auto advice = SettingAdvice(entry.focusId)) r->detail.Text(L(advice));
            muxc::Grid::SetRow(r->detail, 2); muxc::Grid::SetColumnSpan(r->detail, 2); r->grid.Children().Append(r->detail);
            r->item.Content(r->grid); list.Items().Append(r->item);
            muxa::AutomationProperties::SetName(r->open, L("start.openSettings") + L" · " + entry.label);
            r->disclosureToken = r->disclosure.Click([this, id = entry.focusId](auto&&, auto&&) {
                if (closed) return; expandedId = expandedId == id ? "" : id; RenderDetails();
            });
            r->openToken = r->open.Click([this, route = SettingsRoute::ForPage(entry.page, entry.focusId)](auto&&, auto&&) {
                if (!closed && navigate) navigate(route);
            });
            rows.push_back(std::move(row));
        }
        RenderDetails(); SizeRows();
    }
    void Refresh()
    {
        if (closed) return;
        auto next = source ? source() : std::vector<StaticSettingSearchDescriptor>{};
        const bool changed = entries.size() != next.size() || !std::equal(entries.begin(), entries.end(), next.begin(),
            [](const auto& a, const auto& b) {
                return a.page == b.page && a.focusId == b.focusId && a.label == b.label &&
                    a.description == b.description && a.visible == b.visible;
            });
        entries = std::move(next);
        for (std::size_t i = 0; i < tabItems.size(); ++i) tabItems[i].Text(L(kIndexModules[i].title));
        muxa::AutomationProperties::SetName(list, L("guide.personalization"));
        RefreshIcons();
        // Status patches must not destroy a focused row or reset its scroll.
        if (changed || rows.empty()) Populate();
    }
    void Close()
    {
        if (closed) return; closed = true;
        tabs.SelectionChanged(tabToken); root.ActualThemeChanged(themeToken);
        rows.clear(); source = {}; navigate = {};
    }
};
UsageSettingsGuide::UsageSettingsGuide(Localize localize) : impl_(std::make_unique<Impl>(std::move(localize))) {}
UsageSettingsGuide::~UsageSettingsGuide() { Close(); }
void UsageSettingsGuide::SetActions(std::function<std::vector<StaticSettingSearchDescriptor>()> source,
    std::function<void(const SettingsRoute&)> navigate)
{ impl_->source = std::move(source); impl_->navigate = std::move(navigate); }
void UsageSettingsGuide::Refresh() { impl_->Refresh(); }
void UsageSettingsGuide::Resize(double width, double height)
{ impl_->width = width; impl_->list.MaxHeight(std::clamp(height * .48, 200.0, 420.0)); impl_->SizeRows(); }
void UsageSettingsGuide::SelectRoute(const SettingsRoute& route)
{
    StaticSettingSearchDescriptor entry; entry.page = route.page; entry.focusId = route.focusId;
    const auto module = ClassifySetting(entry); if (!module) return;
    impl_->module = *module; impl_->expandedId = route.focusId; impl_->updating = true;
    for (std::size_t i = 0; i < kIndexModules.size(); ++i)
        if (kIndexModules[i].id == *module) impl_->tabs.SelectedItem(impl_->tabItems[i]);
    impl_->updating = false; impl_->Refresh(); impl_->Populate();
    for (auto& row : impl_->rows) if (row->entry.focusId == route.focusId) impl_->list.ScrollIntoView(row->item);
}
void UsageSettingsGuide::Close() { if (impl_) impl_->Close(); }
mux::UIElement UsageSettingsGuide::Content() const { return impl_->root; }
}
