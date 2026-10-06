#pragma once
#include "settings_presenter_controls.h"
#include "appearance_sections.h"
#include "app/navigation/quick_navigation_theme.h"
#include "navigation/quick_navigation_query.h"
#include <memory>
#include <map>
#include <cstdio>
#include <cstdint>

namespace snowdesktop::winui
{
// Host-internal navigation editors. Edits apply to the latest controller value.
class QuickNavigationOptions final
{
public:
    using Edit = std::function<void(NavigationSettings&)>;
    using Localize = std::function<std::wstring(std::string_view)>;
    QuickNavigationOptions(Localize localize, std::function<void(Edit)> commit, const winrt::Microsoft::UI::Xaml::Style& style,
        bool appearanceOnly = false)
        : localize_(std::move(localize)), commit_(std::move(commit)), cardStyle_(style), appearanceOnly_(appearanceOnly)
    {
        auto apply = std::move(commit_);
        commit_ = [this, apply = std::move(apply)](Edit edit) {
            auto candidate = values_;
            edit(candidate);
            if (Accept(candidate)) apply(std::move(edit));
        };
        root_.Spacing(8);
        notice_.Severity(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        notice_.IsClosable(false); notice_.IsOpen(false); root_.Children().Append(notice_);
        if (!appearanceOnly_)
        {
            auto opening = Section("quickNav.opening", {}, [this] {commit_([](auto& value) {value.desktopViewMode = QuickNavigationDesktopViewMode::Tile;});});
            view_.SelectionChanged([this](auto&&, auto&&) {if (!sync_ && view_.SelectedIndex() >= 0) commit_([mode = static_cast<QuickNavigationDesktopViewMode>(view_.SelectedIndex())](auto& value) {value.desktopViewMode = mode;});});
            AddRow(opening, "quickNav.view", view_, [](auto& value) {value.desktopViewMode = QuickNavigationDesktopViewMode::Tile;});
            focus_.emplace("quickNav.defaultCollapsed", opening);
        }
        if (appearanceOnly_)
        {
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
        }
        if (!appearanceOnly_)
        {
            auto prefixes = Section("quickNav.prefixes", "quickNav.prefixes.hint", [this] {commit_([](auto& value) {value.prefixes = NavigationSettings{}.prefixes;});});
            constexpr const char* typeKeys[] = {"quickNav.type.app", "quickNav.type.file", "quickNav.type.web", "quickNav.type.settings", "quickNav.type.run", "quickNav.type.calculator"};
            for (size_t i = 0; i < prefixes_.size(); ++i)
            {
                if (i == static_cast<size_t>(QuickNavigationSearchType::File) - 1) continue;
                prefixes_[i].MaxLength(32);
                prefixes_[i].LostFocus([this, i](auto&&, auto&&) {
                    if (sync_) return;
                    const auto prefix = quick_navigation_query::Prefix(prefixes_[i].Text().c_str());
                    auto candidate = values_; candidate.prefixes[i] = prefix;
                    if (Accept(candidate)) commit_([i, prefix](auto& value) {value.prefixes[i] = prefix;});
                });
                AddRow(prefixes, typeKeys[i], prefixes_[i], [i](auto& value) {value.prefixes[i] = NavigationSettings{}.prefixes[i];});
            }
            auto engines = Section("quickNav.engines", {}, [this] {commit_([](auto& value) {value.engines = NavigationSettings{}.engines; value.defaultEngine = "bing";});});
            defaultEngine_.SelectionChanged([this](auto&&, auto&&) {
                const int index = defaultEngine_.SelectedIndex();
                if (!sync_ && index >= 0 && static_cast<size_t>(index) < values_.engines.size())
                    commit_([id = values_.engines[static_cast<size_t>(index)].id](auto& value) {value.defaultEngine = id;});
            });
            AddRow(engines, "quickNav.defaultEngine", defaultEngine_, [](auto& value) {if (!value.engines.empty()) value.defaultEngine = std::any_of(value.engines.begin(), value.engines.end(), [](const auto& engine) {return engine.id == "bing";}) ? "bing" : value.engines.front().id;});
            Panel management; management.Spacing(8);
            winrt::Microsoft::UI::Xaml::Controls::TextBlock engineHint; engineHint.Text(L("quickNav.engines.hint"));
            engineHint.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); engineHint.Opacity(.68);
            labels_.emplace_back("quickNav.engines.hint",engineHint); management.Children().Append(engineHint);
            management.Children().Append(engineRows_);
            engineManagement_.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            engineManagement_.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            engineManagement_.Header(winrt::box_value(L("quickNav.engines.manage"))); engineManagement_.Content(management);
            groups_.emplace_back("quickNav.engines.manage",engineManagement_); engines.Children().Append(engineManagement_);
            addEngine_.Click([this](auto&&, auto&&) {
                auto candidate = values_;
                unsigned id = 1;
                while (std::any_of(candidate.engines.begin(), candidate.engines.end(), [id](auto& engine) {return engine.id == "custom" + std::to_string(id) || engine.prefix == "web" + std::to_string(id);})) ++id;
                expandedEngine_ = "custom" + std::to_string(id);
                candidate.engines.push_back({"custom" + std::to_string(id), "Bing " + std::to_string(id), "web" + std::to_string(id), "https://www.bing.com/search?q={query}"});
                if (Accept(candidate)) commit_([engines = candidate.engines](auto& value) {value.engines = engines;});
            });
            addEngine_.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
            management.Children().Append(addEngine_);
        }
        if (appearanceOnly_)
        {
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
                    commit_([key = current->key, value = EncodeRgbaColor(rgba.R, rgba.G, rgba.B, rgba.A)](auto& settings) {settings.colors[key] = value;});
                });
                current->editor->picker.IsAlphaEnabled(true);
                current->editor->picker.IsAlphaSliderVisible(true);
                current->editor->picker.IsAlphaTextInputVisible(true);
                current->editor->SetText(L(std::string("quickNav.color.") + name), L(ColorHint(name)), L("app.settings.cancel"));
                presenter_controls::AddRestoreDefaultAction(current->editor->row, L("app.settings.restore_default"), [this, name] {commit_([name](auto& value) {value.colors.erase(name);});});
                colorGroups.at(area).Children().Append(current->editor->row.root);
                focus_.emplace("quickNav.color." + name,current->editor->row.root);
                colors_.push_back(std::move(color));
            }
        }
        RefreshText();
    }
    auto Content() const {return root_;}
    void Apply(const NavigationSettings& values, bool light, bool glass = false)
    {
        if (hasValues_ && values == values_ && light == light_ && glass == glass_) return;
        const bool enginesChanged = !hasValues_ || values.engines != values_.engines;
        sync_ = true; values_ = values; light_ = light; glass_ = glass; hasValues_ = true;
        if (!appearanceOnly_)
        {
            view_.SelectedIndex(static_cast<int>(values.desktopViewMode));
            for (size_t i = 0; i < prefixes_.size(); ++i)
                if (prefixes_[i].Text() != winrt::to_hstring(values.prefixes[i])) prefixes_[i].Text(winrt::to_hstring(values.prefixes[i]));
            if (enginesChanged) BuildEngines();
            for (size_t i = 0; i < values.engines.size(); ++i) if (values.engines[i].id == values.defaultEngine) defaultEngine_.SelectedIndex(static_cast<int>(i));
        }
        for (const auto& number : numbers_) presenter_controls::SyncNumberBoxValue(number->box, values.layout.*number->field);
        const auto theme = ResolveQuickNavTheme(light, values, glass);
        for (const auto& color : colors_)
            for (const auto& [name, field] : kQuickNavColorFields) if (color->key == name)
            {
                const auto value = theme.*field;
                color->editor->SetColor({static_cast<std::uint8_t>(std::lround(value.alpha * 255.f)),
                    GetRValue(value.rgb), GetGValue(value.rgb), GetBValue(value.rgb)});
            }
        sync_ = false;
    }
    void RefreshText()
    {
        sync_ = true;
        for (const auto& [key, label] : labels_) label.Text(L(key));
        for (const auto& [key, row] : rows_) row->SetText(L(key), key.starts_with("quickNav.layout.") ? L(LayoutHint(key)) : std::wstring{});
        for (const auto& [key, group] : groups_) group.Header(winrt::box_value(L(key)));
        if (!appearanceOnly_)
        {
            view_.Items().Clear();
            for (const char* key : {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial"}) view_.Items().Append(winrt::box_value(L(key)));
            view_.SelectedIndex(static_cast<int>(values_.desktopViewMode));
            addEngine_.Content(winrt::box_value(L("quickNav.engine.add")));
            if (hasValues_) BuildEngines();
        }
        for (const auto& color : colors_) color->editor->SetText(L(std::string("quickNav.color.") + color->key), L(ColorHint(color->key)), L("app.settings.cancel"));
        sync_ = false;
    }
    void Register(const std::function<void(std::string, const winrt::Microsoft::UI::Xaml::FrameworkElement&)>& registrar) const
    {for (const auto& [id, target] : focus_) registrar(id, target);}
    winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        try
        {
            const auto found = focus_.find(std::string(id));
            if (found == focus_.end()) return nullptr;
            AppearanceSections::RevealWithin(root_, found->second);
            return found->second;
        }
        catch (...) {return nullptr;}
    }
    void Dismiss() { for (const auto& color : colors_) color->editor->Dismiss(); }
    void Close() { for (const auto& color : colors_) color->editor->Close(); }
private:
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using NumberBox = winrt::Microsoft::UI::Xaml::Controls::NumberBox;
    struct Number {std::string key; int QuickNavigationLayout::*field; NumberBox box;};
    struct Color {std::string key; std::unique_ptr<presenter_controls::ColorFlyoutEditor> editor;};
    std::wstring L(std::string_view key) const {return localize_(key);}
    static const char* ColorHint(std::string_view key)
    {
        if (key == "resultFill" || key == "resultBorder" || key == "iconPlateFill" || key == "iconPlateBorder" ||
            key == "tabDefaultFill" || key == "tabDefaultStroke" || key == "tabActiveStroke" || key == "tabHoverStroke")
            return "quickNav.colorHint.transparent";
        return "quickNav.colorHint.opacity";
    }
    static const char* LayoutHint(std::string_view key)
    {
        if (key.ends_with("visibleRows")) return "quickNav.layout.rowsHint";
        if (key.ends_with("labelLines")) return "quickNav.layout.linesHint";
        return "quickNav.layout.pixelHint";
    }
    bool Accept(const NavigationSettings& candidate)
    {
        const bool valid = ValidateNavigationSearchConfiguration(candidate);
        notice_.Message(L("quickNav.search.invalidConfiguration")); notice_.IsOpen(!valid); return valid;
    }
    template<class Control> void AddRow(Panel panel, std::string key, Control control, Edit reset)
    {
        auto row = std::make_unique<presenter_controls::SettingRow>();
        control.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        const bool compact = control.template try_as<winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch>() != nullptr;
        control.HorizontalAlignment(compact ? winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right : winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        row->Initialize(control); row->SetText(L(key));
        if (compact) row->SetControlAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
        presenter_controls::AddRestoreDefaultAction(*row, L("app.settings.restore_default"), [this, reset] {commit_(reset);});
        panel.Children().Append(row->root); focus_.emplace(key, control);
        rows_.emplace_back(key, row.get()); rowStorage_.push_back(std::move(row));
    }
    Panel Section(std::string key, std::string hint, std::function<void()> reset)
    {
        Panel panel; panel.Spacing(12);
        winrt::Microsoft::UI::Xaml::Controls::Border card; card.Style(cardStyle_); card.Child(panel);
        winrt::Microsoft::UI::Xaml::Controls::Grid heading;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock title; title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.Text(L(key)); title.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        title.Margin({0,0,40,0}); title.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
        labels_.emplace_back(key, title); heading.Children().Append(title);
        winrt::Microsoft::UI::Xaml::Controls::Button button; presenter_controls::ConfigureRestoreDefaultButton(button,L("app.settings.restore_default"));
        button.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
        button.Click([reset](auto&&, auto&&) {reset();}); heading.Children().Append(button); panel.Children().Append(heading);
        if (!hint.empty()) {winrt::Microsoft::UI::Xaml::Controls::TextBlock text; text.Text(L(hint)); text.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); text.Opacity(.68); panel.Children().Append(text); labels_.emplace_back(hint,text);}
        root_.Children().Append(card); focus_.emplace(key,panel); return panel;
    }
    Panel Group(std::string key, std::function<void()> reset)
    {
        Panel panel; panel.Spacing(12); panel.Padding({20,8,20,16});
        winrt::Microsoft::UI::Xaml::Controls::Button button; presenter_controls::ConfigureRestoreDefaultButton(button,L("app.settings.restore_default"));
        button.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
        button.Click([reset](auto&&, auto&&) {reset();}); panel.Children().Append(button);
        winrt::Microsoft::UI::Xaml::Controls::Expander expander; expander.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch); expander.Header(winrt::box_value(L(key))); expander.Content(panel);
        if (appearanceOnly_) root_.Children().Append(expander);
        else
        {
            winrt::Microsoft::UI::Xaml::Controls::Border card; card.Style(cardStyle_); card.Padding({0,0,0,0}); card.Child(expander);
            root_.Children().Append(card);
        }
        groups_.emplace_back(key,expander); focus_.emplace(key,expander); return panel;
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
        std::map<std::string,bool> expanded;
        for (const auto& [id, expander] : engineExpanders_) expanded[id] = expander.IsExpanded();
        engineExpanders_.clear(); engineRows_.Children().Clear(); defaultEngine_.Items().Clear(); engineRows_.Spacing(8);
        for (const auto& engine : values_.engines)
        {
            defaultEngine_.Items().Append(winrt::box_value(winrt::to_hstring(engine.name)));
            Panel group; group.Spacing(12);
            winrt::Microsoft::UI::Xaml::Controls::Border card; card.Style(cardStyle_); card.Child(group);
            const auto id = engine.id;
            winrt::Microsoft::UI::Xaml::Controls::Grid heading;
            winrt::Microsoft::UI::Xaml::Controls::TextBlock title; title.Text(winrt::to_hstring(engine.name));
            title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); title.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
            title.Margin({0,0,150,0}); title.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); heading.Children().Append(title);
            winrt::Microsoft::UI::Xaml::Controls::Button remove; remove.Content(winrt::box_value(L("quickNav.engine.remove")));
            remove.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right); remove.IsEnabled(values_.engines.size() > 1);
            remove.Click([this,id](auto&&, auto&&) {commit_([id](auto& value) {std::erase_if(value.engines,[&](auto& entry) {return entry.id == id;}); if (value.defaultEngine == id && !value.engines.empty()) value.defaultEngine = value.engines.front().id;});});
            heading.Children().Append(remove); group.Children().Append(heading);
            for (int field = 0; field < 3; ++field)
            {
                winrt::Microsoft::UI::Xaml::Controls::TextBox input;
                input.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                input.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
                input.Text(winrt::to_hstring(field == 0 ? engine.name : field == 1 ? engine.prefix : engine.url));
                input.MaxLength(field == 2 ? 4096 : field == 0 ? 128 : 32);
                input.LostFocus([this,id,field,input](auto&&, auto&&) {
                    if (sync_) return;
                    const auto text = field == 1 ? quick_navigation_query::Prefix(input.Text().c_str()) : winrt::to_string(input.Text());
                    commit_([id,field,text](auto& value) {for (auto& current : value.engines) if (current.id == id) (field == 0 ? current.name : field == 1 ? current.prefix : current.url) = text;});
                });
                presenter_controls::SettingRow row; row.Initialize(input);
                row.SetText(L(field == 0 ? "quickNav.engine.name" : field == 1 ? "quickNav.engine.prefix" : "quickNav.engine.url"));
                presenter_controls::AddRestoreDefaultAction(row,L("app.settings.restore_default"),[this,id,field,original = engine] {
                    const NavigationSettings defaults; auto baseline = original;
                    for (const auto& builtin : defaults.engines) if (builtin.id == id) baseline = builtin;
                    const auto text = field == 0 ? baseline.name : field == 1 ? baseline.prefix : baseline.url;
                    commit_([id,field,text](auto& value) {for (auto& current : value.engines) if (current.id == id) (field == 0 ? current.name : field == 1 ? current.prefix : current.url) = text;});
                });
                group.Children().Append(row.root);
            }
            winrt::Microsoft::UI::Xaml::Controls::Expander expander;
            expander.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            expander.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            expander.Header(winrt::box_value(winrt::to_hstring(engine.name + " · " + engine.prefix)));
            expander.Content(card); expander.IsExpanded(expanded[id] || expandedEngine_ == id);
            engineExpanders_.emplace_back(id,expander); engineRows_.Children().Append(expander);
        }
        expandedEngine_.clear();
    }
    Localize localize_; std::function<void(Edit)> commit_; winrt::Microsoft::UI::Xaml::Style cardStyle_{nullptr}; NavigationSettings values_;
    bool appearanceOnly_ = false, sync_ = false, hasValues_ = false, light_ = false, glass_ = false;
    Panel root_, engineRows_;
    winrt::Microsoft::UI::Xaml::Controls::ComboBox view_, defaultEngine_;
    winrt::Microsoft::UI::Xaml::Controls::InfoBar notice_;
    winrt::Microsoft::UI::Xaml::Controls::Button addEngine_;
    winrt::Microsoft::UI::Xaml::Controls::Expander engineManagement_;
    std::string expandedEngine_;
    std::vector<std::pair<std::string,winrt::Microsoft::UI::Xaml::Controls::Expander>> engineExpanders_;
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
