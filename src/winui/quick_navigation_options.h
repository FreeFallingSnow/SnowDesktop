#pragma once
#include "settings_presenter_controls.h"
#include "../app/quick_navigation_theme.h"
#include "../quick_navigation_query.h"
#include <memory>
#include <map>
#include <cstdio>

namespace snowdesktop::winui
{
// Host-internal navigation editors. Edits apply to the latest controller value.
class QuickNavigationOptions final
{
public:
    using Edit = std::function<void(NavigationSettings&)>;
    using Localize = std::function<std::wstring(std::string_view)>;
    QuickNavigationOptions(Localize localize, std::function<void(Edit)> commit)
        : localize_(std::move(localize)), commit_(std::move(commit))
    {
        root_.Spacing(8);
        notice_.Severity(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        notice_.IsClosable(false); notice_.IsOpen(false); root_.Children().Append(notice_);
        collapsed_.Toggled([this](auto&&, auto&&) { if (!sync_) commit_([value = collapsed_.IsOn()](auto& settings) {settings.defaultCollapsed = value;}); });
        AddRow(root_, "quickNav.defaultCollapsed", collapsed_, [](auto& value) {value.defaultCollapsed = false;});
        view_.SelectionChanged([this](auto&&, auto&&) {if (!sync_ && view_.SelectedIndex() >= 0) commit_([mode = static_cast<QuickNavigationDesktopViewMode>(view_.SelectedIndex())](auto& value) {value.desktopViewMode = mode;});});
        AddRow(root_, "quickNav.view", view_, [](auto& value) {value.desktopViewMode = QuickNavigationDesktopViewMode::Tile;});
        auto layout = Group("quickNav.layout", [this] {commit_([](auto& value) {value.layout = QuickNavigationLayout{};});});
        AddNumber(layout, "expandedWidth", &QuickNavigationLayout::expandedWidth, 400, 1800);
        AddNumber(layout, "collapsedWidth", &QuickNavigationLayout::collapsedWidth, 360, 1400);
        AddNumber(layout, "maximumHeight", &QuickNavigationLayout::maximumHeight, 220, 1400);
        AddNumber(layout, "visibleRows", &QuickNavigationLayout::visibleRows, 1, 20);
        AddNumber(layout, "padding", &QuickNavigationLayout::padding, 8, 40);
        AddNumber(layout, "searchHeight", &QuickNavigationLayout::searchHeight, 40, 80);
        AddNumber(layout, "iconSize", &QuickNavigationLayout::iconSize, 24, 96);
        AddNumber(layout, "gridGap", &QuickNavigationLayout::gridGap, 4, 48);
        AddNumber(layout, "rowGap", &QuickNavigationLayout::rowGap, 0, 48);
        AddNumber(layout, "fontSize", &QuickNavigationLayout::fontSize, 10, 24);
        AddNumber(layout, "secondaryFontSize", &QuickNavigationLayout::secondaryFontSize, 10, 20);
        AddNumber(layout, "searchFontSize", &QuickNavigationLayout::searchFontSize, 12, 24);
        AddNumber(layout, "resultRowHeight", &QuickNavigationLayout::resultRowHeight, 40, 96);
        AddNumber(layout, "labelLines", &QuickNavigationLayout::labelLines, 1, 3);
        AddNumber(layout, "cornerRadius", &QuickNavigationLayout::cornerRadius, 0, 32);
        AddNumber(layout, "searchRadius", &QuickNavigationLayout::searchRadius, 0, 24);
        AddNumber(layout, "tabRadius", &QuickNavigationLayout::tabRadius, 0, 20);
        AddNumber(layout, "itemRadius", &QuickNavigationLayout::itemRadius, 0, 24);
        auto prefixes = Section("quickNav.prefixes", "quickNav.prefixes.hint", [this] {commit_([](auto& value) {value.prefixes = NavigationSettings{}.prefixes;});});
        constexpr const char* typeKeys[] = {"quickNav.type.app", "quickNav.type.file", "quickNav.type.web", "quickNav.type.settings", "quickNav.type.run", "quickNav.type.calculator"};
        for (size_t i = 0; i < prefixes_.size(); ++i)
        {
            prefixes_[i].MaxLength(32);
            prefixes_[i].LostFocus([this, i](auto&&, auto&&) {
                if (sync_) return;
                const auto prefix = quick_navigation_query::Prefix(prefixes_[i].Text().c_str());
                auto candidate = values_; candidate.prefixes[i] = prefix;
                if (Accept(candidate)) commit_([i, prefix](auto& value) {value.prefixes[i] = prefix;});
            });
            AddRow(prefixes, typeKeys[i], prefixes_[i], [i](auto& value) {value.prefixes[i] = NavigationSettings{}.prefixes[i];});
        }
        auto engines = Section("quickNav.engines", "quickNav.engines.hint", [this] {commit_([](auto& value) {value.engines = NavigationSettings{}.engines; value.defaultEngine = "bing";});});
        defaultEngine_.SelectionChanged([this](auto&&, auto&&) {
            const int index = defaultEngine_.SelectedIndex();
            if (!sync_ && index >= 0 && static_cast<size_t>(index) < values_.engines.size())
                commit_([id = values_.engines[static_cast<size_t>(index)].id](auto& value) {value.defaultEngine = id;});
        });
        AddRow(engines, "quickNav.defaultEngine", defaultEngine_, [](auto& value) {if (!value.engines.empty()) value.defaultEngine = value.engines.front().id;});
        engines.Children().Append(engineRows_);
        addEngine_.Click([this](auto&&, auto&&) {
            auto candidate = values_;
            unsigned id = 1;
            while (std::any_of(candidate.engines.begin(), candidate.engines.end(), [id](auto& engine) {return engine.id == "custom" + std::to_string(id) || engine.prefix == "web" + std::to_string(id);})) ++id;
            candidate.engines.push_back({"custom" + std::to_string(id), "Bing " + std::to_string(id), "web" + std::to_string(id), "https://www.bing.com/search?q={query}"});
            if (Accept(candidate)) commit_([engines = candidate.engines](auto& value) {value.engines = engines;});
        });
        engines.Children().Append(addEngine_);
        std::map<std::string, winrt::Microsoft::UI::Xaml::Controls::StackPanel> colorGroups;
        for (const auto& [key, field] : kQuickNavColorFields)
        {
            (void)field;
            const std::string name(key);
            const std::string area = name.starts_with("search") || name.starts_with("type") ? "search" :
                name.starts_with("tab") || name.starts_with("header") ? "navigation" : "results";
            if (!colorGroups.contains(area)) colorGroups.emplace(area, Group("quickNav.colors." + area, [this, area] {
                commit_([area](auto& value) {std::erase_if(value.colors, [&](auto& color) {
                    const auto& key = color.first;
                    const std::string owner = key.starts_with("search") || key.starts_with("type") ? "search" : key.starts_with("tab") || key.starts_with("header") ? "navigation" : "results";
                    return owner == area;
                });});
            }));
            auto color = std::make_unique<Color>(); color->key = name;
            color->editor = std::make_unique<presenter_controls::ColorFlyoutEditor>();
            auto* current = color.get();
            current->editor->Initialize([this, current](auto rgba, SettingsUpdateMode mode) {
                if (sync_ || mode == SettingsUpdateMode::Preview) return;
                char hex[8]{}; std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgba.R, rgba.G, rgba.B);
                commit_([key = current->key, value = std::string(hex)](auto& settings) {settings.colors[key] = value;});
            });
            current->editor->SetText(L("quickNav.color." + name), {}, L("app.settings.cancel"));
            presenter_controls::AddRestoreDefaultAction(current->editor->row, L("app.settings.restore_default"), [this, name] {commit_([name](auto& value) {value.colors.erase(name);});});
            colorGroups.at(area).Children().Append(current->editor->row.root);
            focus_.emplace("quickNav.color." + name,current->editor->row.root);
            colors_.push_back(std::move(color));
        }
        RefreshText();
    }
    auto Content() const {return root_;}
    void Apply(const NavigationSettings& values, bool light)
    {
        if (hasValues_ && values == values_ && light == light_) return;
        const bool enginesChanged = !hasValues_ || values.engines != values_.engines;
        sync_ = true; values_ = values; light_ = light; hasValues_ = true;
        collapsed_.IsOn(values.defaultCollapsed); view_.SelectedIndex(static_cast<int>(values.desktopViewMode));
        for (const auto& number : numbers_) presenter_controls::SyncNumberBoxValue(number->box, values.layout.*number->field);
        for (size_t i = 0; i < prefixes_.size(); ++i)
            if (prefixes_[i].Text() != winrt::to_hstring(values.prefixes[i])) prefixes_[i].Text(winrt::to_hstring(values.prefixes[i]));
        if (enginesChanged) BuildEngines();
        for (size_t i = 0; i < values.engines.size(); ++i) if (values.engines[i].id == values.defaultEngine) defaultEngine_.SelectedIndex(static_cast<int>(i));
        const auto theme = ResolveQuickNavTheme(light, values);
        for (const auto& color : colors_)
            for (const auto& [name, field] : kQuickNavColorFields) if (color->key == name)
            {
                const COLORREF rgb = theme.*field;
                color->editor->SetColor({255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)});
            }
        sync_ = false;
    }
    void RefreshText()
    {
        sync_ = true;
        for (const auto& [key, label] : labels_) label.Text(L(key));
        for (const auto& [key, row] : rows_) row->SetText(L(key));
        for (const auto& [key, group] : groups_) group.Header(winrt::box_value(L(key)));
        view_.Items().Clear();
        for (const char* key : {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial"}) view_.Items().Append(winrt::box_value(L(key)));
        view_.SelectedIndex(static_cast<int>(values_.desktopViewMode));
        addEngine_.Content(winrt::box_value(L("quickNav.engine.add")));
        for (const auto& color : colors_) color->editor->SetText(L("quickNav.color." + color->key), {}, L("app.settings.cancel"));
        if (hasValues_) BuildEngines();
        sync_ = false;
    }
    void Register(const std::function<void(std::string, const winrt::Microsoft::UI::Xaml::FrameworkElement&)>& registrar) const
    {for (const auto& [id, target] : focus_) registrar(id, target);}
    void Close() { for (const auto& color : colors_) color->editor->Close(); }
private:
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using NumberBox = winrt::Microsoft::UI::Xaml::Controls::NumberBox;
    struct Number {std::string key; int QuickNavigationLayout::*field; NumberBox box;};
    struct Color {std::string key; std::unique_ptr<presenter_controls::ColorFlyoutEditor> editor;};
    std::wstring L(std::string_view key) const {return localize_(key);}
    bool Accept(const NavigationSettings& candidate)
    {
        const bool valid = ValidateNavigationSearchConfiguration(candidate);
        notice_.Message(L("quickNav.search.invalidConfiguration")); notice_.IsOpen(!valid); return valid;
    }
    template<class Control> void AddRow(Panel panel, std::string key, Control control, Edit reset)
    {
        auto row = std::make_unique<presenter_controls::SettingRow>();
        row->Initialize(control); row->SetText(L(key));
        presenter_controls::AddRestoreDefaultAction(*row, L("app.settings.restore_default"), [this, reset] {commit_(reset);});
        panel.Children().Append(row->root); focus_.emplace(key, control);
        rows_.emplace_back(key, row.get()); rowStorage_.push_back(std::move(row));
    }
    Panel Section(std::string key, std::string hint, std::function<void()> reset)
    {
        Panel panel; panel.Spacing(12); panel.Padding({16,16,16,16});
        winrt::Microsoft::UI::Xaml::Controls::TextBlock title; title.FontSize(17); title.Text(L(key));
        labels_.emplace_back(key, title); panel.Children().Append(title);
        if (!hint.empty()) {winrt::Microsoft::UI::Xaml::Controls::TextBlock text; text.Text(L(hint)); text.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); text.Opacity(.7); panel.Children().Append(text); labels_.emplace_back(hint,text);}
        winrt::Microsoft::UI::Xaml::Controls::Button button; presenter_controls::ConfigureRestoreDefaultButton(button,L("app.settings.restore_default"));
        button.Click([reset](auto&&, auto&&) {reset();}); panel.Children().Append(button);
        root_.Children().Append(panel); focus_.emplace(key,panel); return panel;
    }
    Panel Group(std::string key, std::function<void()> reset)
    {
        Panel panel; panel.Spacing(12); panel.Padding({0,8,0,8});
        winrt::Microsoft::UI::Xaml::Controls::Button button; presenter_controls::ConfigureRestoreDefaultButton(button,L("app.settings.restore_default"));
        button.Click([reset](auto&&, auto&&) {reset();}); panel.Children().Append(button);
        winrt::Microsoft::UI::Xaml::Controls::Expander expander; expander.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch); expander.Header(winrt::box_value(L(key))); expander.Content(panel);
        root_.Children().Append(expander); groups_.emplace_back(key,expander); focus_.emplace(key,expander); return panel;
    }
    void AddNumber(Panel panel, const char* key, int QuickNavigationLayout::* field, int min, int max)
    {
        auto number = std::make_unique<Number>(); number->key = key; number->field = field;
        number->box.Minimum(min); number->box.Maximum(max); number->box.SmallChange(1);
        number->box.SpinButtonPlacementMode(winrt::Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Compact);
        number->box.ValueChanged([this, field](auto&&, auto&& args) {if (!sync_ && std::isfinite(args.NewValue())) commit_([field, value = static_cast<int>(std::lround(args.NewValue()))](auto& settings) {settings.layout.*field = value;});});
        AddRow(panel, "quickNav.layout." + std::string(key), number->box, [field](auto& value) {value.layout.*field = QuickNavigationLayout{}.*field;});
        numbers_.push_back(std::move(number));
    }
    void BuildEngines()
    {
        engineRows_.Children().Clear(); defaultEngine_.Items().Clear();
        for (const auto& engine : values_.engines)
        {
            defaultEngine_.Items().Append(winrt::box_value(winrt::to_hstring(engine.name)));
            Panel group; group.Spacing(8); group.Padding({0,8,0,8});
            const auto id = engine.id;
            for (int field = 0; field < 3; ++field)
            {
                winrt::Microsoft::UI::Xaml::Controls::TextBox input;
                input.Header(winrt::box_value(L(field == 0 ? "quickNav.engine.name" : field == 1 ? "quickNav.engine.prefix" : "quickNav.engine.url")));
                input.Text(winrt::to_hstring(field == 0 ? engine.name : field == 1 ? engine.prefix : engine.url));
                input.MaxLength(field == 2 ? 4096 : field == 0 ? 128 : 32);
                input.LostFocus([this,id,field,input](auto&&, auto&&) {
                    if (sync_) return;
                    const auto text = field == 1 ? quick_navigation_query::Prefix(input.Text().c_str()) : winrt::to_string(input.Text());
                    auto candidate = values_;
                    for (auto& current : candidate.engines) if (current.id == id)
                        (field == 0 ? current.name : field == 1 ? current.prefix : current.url) = text;
                    if (Accept(candidate)) commit_([id,field,text](auto& value) {for (auto& current : value.engines) if (current.id == id) (field == 0 ? current.name : field == 1 ? current.prefix : current.url) = text;});
                });
                group.Children().Append(input);
                winrt::Microsoft::UI::Xaml::Controls::Button reset;
                presenter_controls::ConfigureRestoreDefaultButton(reset,L("app.settings.restore_default"));
                reset.Click([this,id,field,original = engine](auto&&,auto&&) {
                    const NavigationSettings defaults; auto baseline = original;
                    for (const auto& builtin : defaults.engines) if (builtin.id == id) baseline = builtin;
                    auto candidate = values_;
                    const auto text = field == 0 ? baseline.name : field == 1 ? baseline.prefix : baseline.url;
                    for (auto& entry : candidate.engines) if (entry.id == id) (field == 0 ? entry.name : field == 1 ? entry.prefix : entry.url) = text;
                    if (Accept(candidate)) commit_([id,field,text](auto& value) {for (auto& entry : value.engines) if (entry.id == id) (field == 0 ? entry.name : field == 1 ? entry.prefix : entry.url) = text;});
                });
                group.Children().Append(reset);
            }
            winrt::Microsoft::UI::Xaml::Controls::Button remove; remove.Content(winrt::box_value(L("quickNav.engine.remove"))); remove.IsEnabled(values_.engines.size() > 1);
            remove.Click([this,id](auto&&, auto&&) {commit_([id](auto& value) {std::erase_if(value.engines,[&](auto& engine) {return engine.id == id;}); if (value.defaultEngine == id && !value.engines.empty()) value.defaultEngine = value.engines.front().id;});});
            group.Children().Append(remove); engineRows_.Children().Append(group);
        }
    }
    Localize localize_; std::function<void(Edit)> commit_; NavigationSettings values_;
    bool sync_ = false, hasValues_ = false, light_ = false;
    Panel root_, engineRows_;
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch collapsed_;
    winrt::Microsoft::UI::Xaml::Controls::ComboBox view_, defaultEngine_;
    winrt::Microsoft::UI::Xaml::Controls::InfoBar notice_;
    winrt::Microsoft::UI::Xaml::Controls::Button addEngine_;
    std::array<winrt::Microsoft::UI::Xaml::Controls::TextBox, 6> prefixes_;
    std::vector<std::unique_ptr<Number>> numbers_;
    std::vector<std::unique_ptr<Color>> colors_;
    std::vector<std::unique_ptr<presenter_controls::SettingRow>> rowStorage_;
    std::vector<std::pair<std::string,presenter_controls::SettingRow*>> rows_;
    std::vector<std::pair<std::string,winrt::Microsoft::UI::Xaml::Controls::TextBlock>> labels_;
    std::vector<std::pair<std::string,winrt::Microsoft::UI::Xaml::Controls::Expander>> groups_;
    std::map<std::string,winrt::Microsoft::UI::Xaml::FrameworkElement> focus_;
};
}
