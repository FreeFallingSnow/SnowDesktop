#pragma once
#include "theme_library_actions.h"
#include "theme_edit_state.h"
#include "appearance_sections.h"
#include "../theme_library_settings.h"
#include "../theme_workshop_tags.h"
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <atomic>
#include <array>
#include <cwctype>
#include <memory>

namespace snowdesktop::winui::theme_controls
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = winrt::Microsoft::UI::Xaml::Controls;
// One form per appearance source. Refreshes never replace an unsaved draft.
// Appearance forms extend the original selector; management owns library actions.
class ThemeLibraryControls final
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    ThemeLibraryControls(Localize localize, std::string target, bool transfer = false,
        c::ComboBox existing = nullptr, int modeCount = 0, int customIndex = -1, x::Style cardStyle = nullptr)
        : localize_(std::move(localize)), target_(std::move(target)), transfer_(transfer),
          modeCount_(modeCount), customIndex_(customIndex), cardStyle_(cardStyle)
    {
        if (existing) choice_ = existing;
        root_.Spacing(8);
        if (transfer_)
        {
            filters_.ColumnSpacing(8); filters_.RowSpacing(8);
            root_.Children().Append(filters_);
            for (std::size_t i = 0; i < tabs_.size(); ++i)
            {
                auto button = tabs_[i]; button.UseSystemFocusVisuals(true);
                button.HorizontalAlignment(x::HorizontalAlignment::Stretch);
                filters_.Children().Append(button);
                const auto token = button.Click([this, i](const auto&, const auto&) {
                    if (syncing_) return;
                    filter_ = static_cast<FilterTab>(i); PatchTabs(); RefreshChoices();
                });
                revoke_.push_back([button, token] { button.Click(token); });
            }
            const auto token = filters_.SizeChanged([this](const auto&, const auto&) { LayoutTabs(); });
            revoke_.push_back([control = filters_, token] { control.SizeChanged(token); });
            entries_.Spacing(8); root_.Children().Append(entries_);
            empty_.TextWrapping(x::TextWrapping::Wrap); root_.Children().Append(empty_);
        }
        for (auto combo : {choice_, quick_, popup_}) combo.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        commands_.DefaultLabelPosition(c::CommandBarDefaultLabelPosition::Right);
        if (transfer_) root_.Children().InsertAt(1, commands_);
        if (transfer_)
        {
            AddCommand(ThemeLibraryCommand::Import, "themeLibrary.import");
            AddCommand(ThemeLibraryCommand::SyncSubscriptions, "themeLibrary.sync");
        }
        else
        {
            saveBody_ = AppearanceSections::Section(saveRoot_, saveTitle_, &disclosures_);
            name_.MaxLength(128); nameRow_.Initialize(name_); saveBody_.Children().Append(nameRow_.root);
            if (Global())
            {
                c::StackPanel scopes; scopes.Spacing(8);
                for (auto check : scopeChecks_)
                {
                    check.IsChecked(true); scopes.Children().Append(check);
                    const auto checked = check.Checked([this](const auto&, const auto&) { PatchBindings(); });
                    const auto unchecked = check.Unchecked([this](const auto&, const auto&) { PatchBindings(); });
                    revoke_.push_back([check, checked, unchecked] { check.Checked(checked); check.Unchecked(unchecked); });
                }
                scopeRow_.Initialize(scopes); saveBody_.Children().Append(scopeRow_.root);
                quickRow_.Initialize(quick_); popupRow_.Initialize(popup_);
                saveBody_.Children().Append(quickRow_.root); saveBody_.Children().Append(popupRow_.root);
            }
            for (auto [command, key] : {std::pair{ThemeLibraryCommand::Update, "themeLibrary.saveTo"}, {ThemeLibraryCommand::SaveAs, "themeLibrary.create"}})
            {
                c::Button button; button.HorizontalAlignment(x::HorizontalAlignment::Right);
                button.Content(winrt::box_value(L(key))); saveBody_.Children().Append(button);
                const auto token = button.Click([this, command](const auto&, const auto&) { Run(command); });
                revoke_.push_back([button, token] { button.Click(token); });
                saveButtons_.push_back({command, key, button});
            }
            cancel_.HorizontalAlignment(x::HorizontalAlignment::Right); saveBody_.Children().Append(cancel_);
            const auto cancelToken = cancel_.Click([this](const auto&, const auto&) {
                if (edit_.theme && themes::Find(library_.themes, edit_.theme->id)) Run(ThemeLibraryCommand::Apply, edit_.theme->id);
            });
            revoke_.push_back([control = cancel_, cancelToken] { control.Click(cancelToken); });
        }
        feedback_.IsOpen(false); root_.Children().Append(feedback_);
        saveFeedback_.IsOpen(false); if (!transfer_) saveBody_.Children().Append(saveFeedback_);
        const auto choiceToken = choice_.SelectionChanged([this](const auto&, const auto&) {
            if (syncing_) return;
            PatchButtons();
        });
        revoke_.push_back([control = choice_, choiceToken] { control.SelectionChanged(choiceToken); });
        for (auto control : {choice_, quick_, popup_})
        {
            const auto token = control.DropDownOpened([this](const auto&, const auto&) { Refresh(); });
            revoke_.push_back([control, token] { control.DropDownOpened(token); });
        }
        LocalizeText();
    }
    ~ThemeLibraryControls() { Close(); }
    x::UIElement Content() const { return root_; }
    x::UIElement SaveContent() const { return saveRoot_; }
    x::FrameworkElement Choice() const { return transfer_ ? x::FrameworkElement{entries_} : x::FrameworkElement{choice_}; }
    bool CustomSelected() const { return !transfer_ && !Selected() && choice_.SelectedIndex() == customIndex_; }
    bool HasSavedSelection() const { return Selected() != nullptr; }
    void SetCustomContent(std::initializer_list<x::UIElement> elements) { customContent_.assign(elements); PatchButtons(); }
    bool Synchronizing() const { return syncing_ || busy_; }
    int NativeSelection() const { return Selected() ? customIndex_ : choice_.SelectedIndex(); }
    // The controller values and last successful library reference are authoritative.
    void SyncSelection(int nativeIndex, const SettingsValues& values)
    {
        nativeIndex_ = nativeIndex;
        current_ = themes::CaptureTarget(target_, values);
        Refresh();
    }
    bool ApplySelection()
    {
        if (Synchronizing()) return true;
        const auto selected = Selected();
        if (!selected)
        {
            const bool enteringCustom = choice_.SelectedIndex() == customIndex_;
            auto source = edit_;
            if (enteringCustom && !editing_) source.Begin(library_, target_);
            else if (!enteringCustom) source.Reset();
            const auto reference = library_.references.find(target_);
            if (reference != library_.references.end() && !reference->second.id.empty() && action_)
            {
                ThemeLibraryRequest request; request.command = ThemeLibraryCommand::Detach; request.target = target_;
                const auto result = action_(generation_, request);
                if (!result.succeeded) { RefreshChoices(); Feedback(result); return true; }
                library_ = result.library;
            }
            if (enteringCustom && !editing_) { edit_ = std::move(source); LoadDraft(); }
            else if (!enteringCustom) { edit_.Reset(); name_.Text(L""); }
            editing_ = enteringCustom;
            nativeIndex_ = choice_.SelectedIndex(); PatchButtons(); return false;
        }
        Run(ThemeLibraryCommand::Apply, selected->id);
        return true;
    }
    void SetActions(ThemeLibraryAction action, ThemeLibraryAsyncAction async, std::function<bool()> flush)
    { action_ = std::move(action); async_ = std::move(async); flush_ = std::move(flush); }
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
    void SetGeneration(std::uint64_t generation)
    {
        if (generation_ != generation)
        {
            for (auto disclosure : disclosures_) disclosure.IsExpanded(false);
            edit_.Reset(); editing_ = false;
        }
        generation_ = generation;
    }
    void Refresh()
    {
        if (!action_ || !generation_ || closed_) return;
        const auto result = action_(generation_, {});
        if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; publishedUrls_ = result.publishedUrls; RefreshChoices(); }
        else Feedback(result);
    }
    void Reveal() const { if (transfer_) return; AppearanceSections::RevealWithin(saveRoot_, name_); }
    void LocalizeText()
    {
        if (transfer_)
        {
            empty_.Text(L("themeLibrary.empty"));
            constexpr const char* keys[]{"themeLibrary.all", "themeLibrary.global", "themeLibrary.dock", "themeLibrary.statusBar",
                "themeLibrary.taskbar", "themeLibrary.quickPanel", "themeLibrary.popup"};
            for (std::size_t i = 0; i < tabs_.size(); ++i)
            {
                c::TextBlock text; text.Text(L(keys[i])); text.TextWrapping(x::TextWrapping::Wrap);
                tabs_[i].Content(text);
            }
            PatchTabs(); LayoutTabs();
        }
        cancel_.Content(winrt::box_value(L("themeLibrary.cancelEdit")));
        if (!transfer_) nameRow_.SetText(L("themeLibrary.name"), L("themeLibrary.nameHint"));
        if (saveTitle_) saveTitle_.Text(L("themeLibrary.save"));
        if (Global() && !transfer_)
        {
            scopeRow_.SetText(L("themeLibrary.appliesTo"));
            constexpr const char* keys[] = {"themeLibrary.dock", "themeLibrary.statusBar", "themeLibrary.taskbar"};
            for (std::size_t i = 0; i < scopeChecks_.size(); ++i) scopeChecks_[i].Content(winrt::box_value(L(keys[i])));
            quickRow_.SetText(L("themeLibrary.quickPanel")); popupRow_.SetText(L("themeLibrary.popup"));
        }
        for (auto& button : buttons_) button.control.Label(L(button.key));
        for (auto& button : saveButtons_) button.control.Content(winrt::box_value(L(button.key)));
        RefreshChoices();
    }
    void Close()
    {
        if (closed_) return;
        closed_ = true; alive_->store(false);
        for (auto& revoke : revoke_) revoke();
        ClearEntries(); revoke_.clear(); action_ = {}; async_ = {}; flush_ = {}; changed_ = {};
    }
private:
    Localize localize_;
    std::string target_;
    bool transfer_ = false, syncing_ = false, busy_ = false, sharing_ = false, closed_ = false;
    int modeCount_ = 0, customIndex_ = -1, nativeIndex_ = -1;
    std::optional<themes::Theme> current_;
    std::uint64_t generation_ = 0;
    ThemeLibraryAction action_;
    ThemeLibraryAsyncAction async_;
    std::function<bool()> flush_;
    std::function<void()> changed_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    themes::Library library_;
    std::map<std::string, std::string> publishedUrls_;
    std::vector<themes::Theme> choices_, quickChoices_, popupChoices_;
    c::StackPanel root_, saveRoot_, saveBody_{nullptr};
    c::TextBlock saveTitle_{nullptr};
    c::ComboBox choice_, quick_, popup_;
    c::StackPanel entries_;
    x::Style cardStyle_{nullptr};
    struct CardCommand { ThemeLibraryCommand command; std::string id; c::AppBarButton control; };
    std::vector<CardCommand> cardCommands_;
    std::vector<std::function<void()>> cardRevoke_;
    c::Grid filters_;
    std::array<c::Primitives::ToggleButton, 7> tabs_;
    FilterTab filter_ = FilterTab::All;
    c::TextBlock empty_;
    c::Button cancel_;
    EditSource edit_;
    bool editing_ = false;
    std::vector<x::UIElement> customContent_;
    c::TextBox name_;
    presenter_controls::SettingRow nameRow_, scopeRow_, quickRow_, popupRow_;
    std::array<c::CheckBox, 3> scopeChecks_;
    std::vector<c::Expander> disclosures_;
    c::CommandBar commands_;
    c::InfoBar feedback_, saveFeedback_;
    struct Command { ThemeLibraryCommand command; const char* key; c::AppBarButton control; };
    struct SaveButton { ThemeLibraryCommand command; const char* key; c::Button control; };
    std::vector<Command> buttons_;
    std::vector<SaveButton> saveButtons_;
    std::vector<std::function<void()>> revoke_;

    bool Global() const { return themes::TargetKind(target_) == themes::Kind::Global; }
    std::wstring L(std::string_view key) const { return localize_ ? localize_(key) : std::wstring{}; }
    const themes::Theme* Selected() const
    {
        if (transfer_) return nullptr;
        const auto index = choice_.SelectedIndex() - modeCount_;
        return index >= 0 && static_cast<std::size_t>(index) < choices_.size() ? &choices_[index] : nullptr;
    }
    std::wstring ScopeLabel(unsigned scope) const
    {
        if (themes::FullScope(scope)) return L("themeLibrary.global");
        std::wstring label;
        for (auto [bit, key] : {std::pair{themes::Dock, "themeLibrary.dock"}, {themes::StatusBar, "themeLibrary.statusBar"}, {themes::Taskbar, "themeLibrary.taskbar"}})
            if (scope & bit) { if (!label.empty()) label += L" + "; label += L(key); }
        return label;
    }
    std::wstring Label(const themes::Theme& theme) const
    {
        if (!themes::Builtin(theme.id))
        {
            std::wstring label(winrt::to_hstring(theme.name).c_str());
            if (transfer_) label += L" · " + (theme.kind == themes::Kind::Global ? ScopeLabel(theme.scopes) :
                L(theme.kind == themes::Kind::QuickPanel ? "themeLibrary.quickPanel" : "themeLibrary.popup"));
            return label;
        }
        static const std::map<std::string, const char*> keys = {{"dark", "app.settings.dark"}, {"light", "app.settings.light"},
            {"glass-dark", "app.settings.dark_glass"}, {"glass-light", "app.settings.light_glass"},
            {"glass-transparent", "app.settings.transparent_glass"}, {"acrylic-dark", "app.settings.dark_acrylic"},
            {"acrylic-light", "app.settings.light_acrylic"}};
        const auto found = keys.find(theme.name);
        return found == keys.end() ? std::wstring(winrt::to_hstring(theme.name).c_str()) : L(found->second);
    }
    static std::string Binding(const c::ComboBox& combo, const std::vector<themes::Theme>& choices)
    {
        const auto index = combo.SelectedIndex();
        return index > 0 && static_cast<std::size_t>(index) <= choices.size() ? choices[index - 1].id : std::string{};
    }
    static void SelectBinding(const c::ComboBox& combo, const std::vector<themes::Theme>& choices, const std::string& id)
    {
        int index = id.empty() ? 0 : -1;
        for (std::size_t i = 0; i < choices.size(); ++i) if (choices[i].id == id) index = static_cast<int>(i + 1);
        combo.SelectedIndex(index);
    }
    void RefreshChoices(std::string preferred = {})
    {
        if (!transfer_ && !editing_)
        {
            const auto reference = library_.references.find(target_);
            if (nativeIndex_ == customIndex_ && current_ && reference != library_.references.end() && !reference->second.id.empty())
            {
                const auto saved = themes::Resolve(reference->second.snapshot, reference->second.id);
                if (saved && EncodePanelAppearance(saved->appearance) == EncodePanelAppearance(current_->appearance) &&
                    saved->appearance.gradientEndA == current_->appearance.gradientEndA &&
                    (saved->kind != themes::Kind::QuickPanel || (saved->layout == current_->layout && saved->colors == current_->colors)))
                    preferred = saved->id;
            }
        }
        const auto quick = Binding(quick_, quickChoices_), popup = Binding(popup_, popupChoices_);
        const bool quickUnset = quick_.SelectedIndex() < 0, popupUnset = popup_.SelectedIndex() < 0;
        choices_.clear();
        if (transfer_)
        {
            for (const auto& [id, theme] : library_.themes)
                if (!themes::Builtin(id) && MatchesFilter(theme, filter_)) choices_.push_back(theme);
            std::stable_sort(choices_.begin(), choices_.end(), [](auto const& a, auto const& b) { return a.name < b.name; });
        }
        else
        {
            choices_ = themes::Choices(library_, themes::TargetKind(target_), target_ == "global" ? 0 : themes::TargetScope(target_));
            std::erase_if(choices_, [this](auto const& theme) {
                return themes::Builtin(theme.id) || (target_ == "global" && theme.kind == themes::Kind::Global && !themes::FullScope(theme.scopes));
            });
        }
        syncing_ = true;
        if (transfer_) ClearEntries();
        else while (choice_.Items().Size() > static_cast<unsigned>(modeCount_)) choice_.Items().RemoveAtEnd();
        int selected = transfer_ ? (choices_.empty() ? -1 : 0) : nativeIndex_;
        for (std::size_t i = 0; i < choices_.size(); ++i)
        {
            if (transfer_) entries_.Children().Append(Entry(choices_[i]));
            else choice_.Items().Append(winrt::box_value(Label(choices_[i])));
            if (choices_[i].id == preferred) selected = static_cast<int>(i) + (transfer_ ? 0 : modeCount_);
        }
        if (!transfer_) choice_.SelectedIndex(selected);
        if (transfer_) empty_.Visibility(choices_.empty() ? x::Visibility::Visible : x::Visibility::Collapsed);
        if (Global() && !transfer_)
        {
            quickChoices_ = themes::Choices(library_, themes::Kind::QuickPanel);
            popupChoices_ = themes::Choices(library_, themes::Kind::Popup);
            const auto fill = [this](auto combo, auto const& choices, const char* key, const auto& binding) {
                combo.Items().Clear(); combo.Items().Append(winrt::box_value(L(key)));
                for (auto const& theme : choices) combo.Items().Append(winrt::box_value(Label(theme)));
                SelectBinding(combo, choices, binding);
            };
            fill(quick_, quickChoices_, "themeLibrary.currentQuick", quick);
            fill(popup_, popupChoices_, "themeLibrary.currentPopup", popup);
            if (quickUnset) quick_.SelectedIndex(-1);
            if (popupUnset) popup_.SelectedIndex(-1);
        }
        syncing_ = false; PatchButtons(); PatchBindings();
    }
    void LoadDraft()
    {
        const auto selected = edit_.theme ? &*edit_.theme : nullptr;
        if (!selected)
        {
            name_.Text(L"");
            for (auto check : scopeChecks_) check.IsChecked(true);
            quick_.SelectedIndex(-1); popup_.SelectedIndex(-1); PatchBindings(); return;
        }
        name_.Text(winrt::to_hstring(selected->name));
        if (!Global()) return;
        for (std::size_t i = 0; i < scopeChecks_.size(); ++i) scopeChecks_[i].IsChecked((selected->scopes & (themes::Dock << i)) != 0);
        SelectBinding(quick_, quickChoices_, selected->quickPanel); SelectBinding(popup_, popupChoices_, selected->popup);
        PatchBindings();
    }
    unsigned Scopes() const
    {
        unsigned scopes = 0;
        for (std::size_t i = 0; i < scopeChecks_.size(); ++i)
            if (const auto check = scopeChecks_[i].IsChecked(); check && check.Value()) scopes |= themes::Dock << i;
        return scopes;
    }
    void PatchBindings()
    {
        if (!Global() || transfer_) return;
        const bool required = themes::FullScope(Scopes());
        quickRow_.root.Visibility(required ? x::Visibility::Visible : x::Visibility::Collapsed);
        popupRow_.root.Visibility(quickRow_.root.Visibility());
    }
    void PatchButtons()
    {
        if (transfer_) { for (auto tab : tabs_) tab.IsEnabled(!busy_); }
        else
        {
            const auto visibility = CustomSelected() ? x::Visibility::Visible : x::Visibility::Collapsed;
            saveRoot_.Visibility(visibility);
            for (auto content : customContent_) content.Visibility(visibility);
            saveTitle_.Text(L(edit_.theme ? "themeLibrary.edit" : "themeLibrary.save"));
            cancel_.Visibility(edit_.theme && themes::Find(library_.themes, edit_.theme->id) ? x::Visibility::Visible : x::Visibility::Collapsed);
            cancel_.IsEnabled(!busy_);
        }
        const auto selected = Selected();
        const bool custom = selected && !themes::Builtin(selected->id);
        for (auto& button : buttons_)
        {
            const bool independent = button.command == ThemeLibraryCommand::Import || button.command == ThemeLibraryCommand::SyncSubscriptions ||
                (button.command == ThemeLibraryCommand::Preview && (target_ == "dock" || target_ == "taskbar"));
            button.control.IsEnabled(!busy_ && (independent || (button.command == ThemeLibraryCommand::Remove ? custom : selected != nullptr)));
            if (button.command == ThemeLibraryCommand::Share || button.command == ThemeLibraryCommand::SyncSubscriptions)
                button.control.Visibility(sharing_ ? x::Visibility::Visible : x::Visibility::Collapsed);
        }
        for (auto& button : cardCommands_)
        {
            button.control.IsEnabled(!busy_ && themes::Find(library_.themes, button.id));
            if (button.command == ThemeLibraryCommand::Share) button.control.Visibility(sharing_ ? x::Visibility::Visible : x::Visibility::Collapsed);
        }
        for (auto& button : saveButtons_)
        {
            const bool update = button.command == ThemeLibraryCommand::Update;
            button.control.Visibility(!update || edit_.CanUpdate(library_) ? x::Visibility::Visible : x::Visibility::Collapsed);
            auto label = L(button.key);
            if (update && edit_.theme)
            {
                const auto position = label.find(L"{name}");
                if (position != std::wstring::npos) label.replace(position, 6, std::wstring(winrt::to_hstring(edit_.theme->name).c_str()));
            }
            c::TextBlock text; text.Text(label); text.TextWrapping(x::TextWrapping::Wrap); button.control.Content(text);
            button.control.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            button.control.IsEnabled(!busy_ && (!update || edit_.CanUpdate(library_)));
        }
    }
    void PatchTabs()
    {
        for (std::size_t i = 0; i < tabs_.size(); ++i) tabs_[i].IsChecked(i == static_cast<std::size_t>(filter_));
    }
    void LayoutTabs()
    {
        const int count = std::clamp(static_cast<int>(filters_.ActualWidth() / 140), 1, 7);
        if (filters_.ColumnDefinitions().Size() == static_cast<unsigned>(count)) return;
        filters_.ColumnDefinitions().Clear(); filters_.RowDefinitions().Clear();
        for (int i = 0; i < count; ++i) filters_.ColumnDefinitions().Append(c::ColumnDefinition{});
        for (std::size_t i = 0; i < tabs_.size(); ++i)
        {
            if (i % count == 0) { c::RowDefinition row; row.Height(x::GridLengthHelper::Auto()); filters_.RowDefinitions().Append(row); }
            c::Grid::SetRow(tabs_[i], static_cast<int>(i) / count); c::Grid::SetColumn(tabs_[i], static_cast<int>(i) % count);
        }
    }
    x::UIElement Entry(const themes::Theme& theme)
    {
        c::StackPanel entry; entry.Spacing(4); entry.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        const auto line = [&](std::wstring text, bool title = false) {
            c::TextBlock label; label.Text(text); label.TextWrapping(x::TextWrapping::Wrap);
            if (title) label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); else { label.FontSize(12); label.Opacity(.8); }
            entry.Children().Append(label);
        };
        line(std::wstring(winrt::to_hstring(theme.name).c_str()), true);
        auto tags = theme.kind == themes::Kind::Global ? ScopeLabel(theme.scopes) :
            L(theme.kind == themes::Kind::QuickPanel ? "themeLibrary.quickPanel" : "themeLibrary.popup");
        if (theme.kind == themes::Kind::Global && themes::FullScope(theme.scopes))
            tags += L" · " + L("themeLibrary.dock") + L" · " + L("themeLibrary.statusBar") + L" · " + L("themeLibrary.taskbar");
        line(tags);
        bool installed = false;
        for (const auto& [item, origin] : library_.workshop) { (void)item; if (origin.ids.contains(theme.id)) installed = true; }
        line(L(installed ? "themeLibrary.installedSource" : "themeLibrary.localSource"));
        if (const auto found = publishedUrls_.find(theme.id); found != publishedUrls_.end())
        {
            c::HyperlinkButton link; link.Content(winrt::box_value(L("themeLibrary.workshopPage")));
            link.NavigateUri(winrt::Windows::Foundation::Uri(winrt::to_hstring(found->second)));
            link.HorizontalAlignment(x::HorizontalAlignment::Left); entry.Children().Append(link);
        }
        for (auto [id, key] : {std::pair{theme.quickPanel, "themeLibrary.quickPanel"}, {theme.popup, "themeLibrary.popup"}})
            if (!id.empty()) if (auto child = themes::Resolve(library_.themes, id)) line(L(key) + L"：" + Label(*child));
        c::CommandBar actions; actions.DefaultLabelPosition(c::CommandBarDefaultLabelPosition::Right);
        for (auto [command, key] : {std::pair{ThemeLibraryCommand::Preview, "themeLibrary.preview"},
            {ThemeLibraryCommand::Export, "themeLibrary.export"}, {ThemeLibraryCommand::Share, "themeLibrary.share"},
            {ThemeLibraryCommand::ChooseCover, "themeLibrary.chooseCover"}, {ThemeLibraryCommand::Regenerate, "themeLibrary.regenerate"},
            {ThemeLibraryCommand::Remove, "themeLibrary.remove"}})
        {
            c::AppBarButton button; button.Label(L(key));
            (command == ThemeLibraryCommand::ChooseCover || command == ThemeLibraryCommand::Regenerate || command == ThemeLibraryCommand::Remove ?
                actions.SecondaryCommands() : actions.PrimaryCommands()).Append(button);
            const auto id = theme.id;
            const auto token = button.Click([this, command, id](const auto&, const auto&) {
                if (command == ThemeLibraryCommand::Remove) ConfirmRemove(id);
                else if (command == ThemeLibraryCommand::Share) ConfirmShare(id);
                else Run(command, id);
            });
            cardRevoke_.push_back([button, token] { button.Click(token); });
            cardCommands_.push_back({command, id, button});
        }
        entry.Children().Append(actions);
        c::Border card; if (cardStyle_) card.Style(cardStyle_); card.Child(entry);
        return card;
    }
    void ClearEntries()
    {
        for (auto& revoke : cardRevoke_) revoke();
        cardRevoke_.clear(); cardCommands_.clear(); entries_.Children().Clear();
    }
    void AddCommand(ThemeLibraryCommand command, const char* key, bool secondary = false)
    {
        c::AppBarButton button; button.Label(L(key));
        (secondary ? commands_.SecondaryCommands() : commands_.PrimaryCommands()).Append(button);
        const auto token = button.Click([this, command](const auto&, const auto&) {
            Run(command);
        });
        revoke_.push_back([button, token] { button.Click(token); }); buttons_.push_back({command, key, button});
    }
    void Feedback(const ThemeLibraryResult& result, bool save = false)
    {
        auto feedback = save ? saveFeedback_ : feedback_;
        if (result.succeeded && result.message.empty()) { feedback.IsOpen(false); return; }
        feedback.Severity(result.succeeded ? c::InfoBarSeverity::Warning : c::InfoBarSeverity::Error);
        feedback.Message(result.message); feedback.IsOpen(!result.message.empty());
    }
    winrt::fire_and_forget ConfirmRemove(std::string themeId)
    {
        const auto selected = themes::Find(library_.themes, themeId);
        if (!selected || themes::Builtin(selected->id) || busy_ || !generation_ || !root_.XamlRoot()) co_return;
        const auto id = selected->id; const auto generation = generation_; auto alive = alive_;
        c::ContentDialog dialog; dialog.XamlRoot(root_.XamlRoot()); dialog.Title(winrt::box_value(L("themeLibrary.remove")));
        dialog.PrimaryButtonText(L("themeLibrary.remove")); dialog.CloseButtonText(L("themeLibrary.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Close);
        c::StackPanel body; body.Spacing(12); c::TextBlock hint;
        hint.Text(Label(*selected) + L"\n" + L("themeLibrary.removeHint")); hint.TextWrapping(x::TextWrapping::Wrap); body.Children().Append(hint);
        c::ComboBox replacement; replacement.Header(winrt::box_value(L("themeLibrary.replacement")));
        replacement.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        replacement.Items().Append(winrt::box_value(L("themeLibrary.preserve")));
        std::vector<themes::Theme> choices;
        for (auto const& theme : themes::Choices(library_, selected->kind))
            if (theme.id != id && (theme.kind != themes::Kind::Global || (theme.scopes & selected->scopes) == selected->scopes))
            { choices.push_back(theme); replacement.Items().Append(winrt::box_value(Label(theme))); }
        replacement.SelectedIndex(0); body.Children().Append(replacement); dialog.Content(body);
        c::ContentDialogResult result = c::ContentDialogResult::None;
        try { result = co_await dialog.ShowAsync(); }
        catch (const winrt::hresult_error&)
        {
            if (alive->load() && generation == generation_) Feedback({false, {}, {}, L("themeLibrary.operationFailed")});
            co_return;
        }
        if (!alive->load() || generation != generation_ || result != c::ContentDialogResult::Primary) co_return;
        Run(ThemeLibraryCommand::Remove, id, Binding(replacement, choices));
    }
    winrt::fire_and_forget ConfirmShare(std::string id)
    {
        const auto selected = themes::Resolve(library_.themes, id);
        if (!selected || busy_ || !sharing_ || !generation_ || !root_.XamlRoot()) co_return;
        const auto required = themes::tags::Applicable(*selected);
        const auto generation = generation_; auto alive = alive_;
        c::ContentDialog dialog; dialog.XamlRoot(root_.XamlRoot()); dialog.Title(winrt::box_value(L("themeLibrary.share")));
        dialog.PrimaryButtonText(L("themeLibrary.prepareShare")); dialog.CloseButtonText(L("themeLibrary.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Close);
        c::StackPanel body; body.Spacing(12);
        const auto text = [&](std::wstring value) { c::TextBlock label; label.Text(value); label.TextWrapping(x::TextWrapping::Wrap); body.Children().Append(label); };
        text(Label(*selected)); text(L("themeLibrary.tagsRequired"));
        std::vector<c::CheckBox> checks;
        for (const auto& tag : required)
        {
            const auto category = std::find_if(themes::tags::Categories.begin(), themes::tags::Categories.end(),
                [&](const auto& item) { return item.value == tag; });
            c::CheckBox check; check.Content(winrt::box_value(L(category->label))); check.IsChecked(true);
            body.Children().Append(check); checks.push_back(check);
        }
        const auto validate = [dialog, checks] { dialog.IsPrimaryButtonEnabled(!checks.empty() &&
            std::all_of(checks.begin(), checks.end(), [](auto check) { const auto value = check.IsChecked(); return value && value.Value(); })); };
        std::vector<std::function<void()>> revokers;
        for (auto check : checks)
        {
            const auto checked = check.Checked([validate](const auto&, const auto&) { validate(); });
            const auto unchecked = check.Unchecked([validate](const auto&, const auto&) { validate(); });
            revokers.push_back([check, checked, unchecked] { check.Checked(checked); check.Unchecked(unchecked); });
        }
        c::ComboBox cover; cover.Header(winrt::box_value(L("themeLibrary.coverSource")));
        cover.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        cover.Items().Append(winrt::box_value(L("themeLibrary.generateCover")));
        cover.Items().Append(winrt::box_value(L("themeLibrary.chooseCover"))); cover.SelectedIndex(0); body.Children().Append(cover);
        if (const auto found = publishedUrls_.find(id); found != publishedUrls_.end()) text(L("themeLibrary.publishUpdate") + L"\n" + std::wstring(winrt::to_hstring(found->second)));
        dialog.Content(body); validate(); busy_ = true; PatchButtons();
        c::ContentDialogResult result = c::ContentDialogResult::None;
        try { result = co_await dialog.ShowAsync(); }
        catch (const winrt::hresult_error&) { if (alive->load() && generation == generation_) Feedback({false, {}, {}, L("themeLibrary.operationFailed")}); }
        for (auto& revoke : revokers) revoke();
        if (!alive->load()) co_return;
        busy_ = false; PatchButtons();
        if (generation != generation_ || result != c::ContentDialogResult::Primary) co_return;
        Run(ThemeLibraryCommand::Share, id, {}, required, cover.SelectedIndex() == 1);
    }
    void Run(ThemeLibraryCommand command, std::string id = {}, std::string replacement = {},
        std::vector<std::string> tags = {}, bool chooseCover = false)
    {
        if (closed_ || busy_ || !action_ || !generation_ || !flush_ || !flush_())
        { if (!closed_ && command == ThemeLibraryCommand::Apply) RefreshChoices(); return; }
        const bool save = command == ThemeLibraryCommand::SaveAs || command == ThemeLibraryCommand::Update;
        ThemeLibraryRequest request; request.command = command; request.target = target_; request.replacement = std::move(replacement);
        request.tags = std::move(tags); request.chooseCover = chooseCover;
        const auto selected = Selected(); request.id = id.empty() && selected ? selected->id : std::move(id);
        if (save)
        {
            if (!CustomSelected()) return;
            if (command == ThemeLibraryCommand::Update)
            {
                if (!edit_.CanUpdate(library_)) return;
                request.id = edit_.theme->id;
            }
            else request.id.clear();
            constexpr auto whitespace = L" \t\r\n\v\f\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
            std::wstring name(name_.Text().c_str()); const auto first = name.find_first_not_of(whitespace);
            const auto last = name.find_last_not_of(whitespace);
            if (first == std::wstring::npos) { Feedback({false, {}, {}, L("themeLibrary.nameRequired")}, true); return; }
            request.name = winrt::to_string(name.substr(first, last - first + 1));
            if (Global())
            {
                request.scopes = Scopes();
                if (!request.scopes) { Feedback({false, {}, {}, L("themeLibrary.scopesRequired")}, true); return; }
                if (themes::FullScope(request.scopes) && (quick_.SelectedIndex() < 0 || popup_.SelectedIndex() < 0))
                { Feedback({false, {}, {}, L("themeLibrary.bindingRequired")}, true); return; }
                if (themes::FullScope(request.scopes))
                { request.quickPanel = Binding(quick_, quickChoices_); request.popup = Binding(popup_, popupChoices_); }
            }
        }
        const bool async = command == ThemeLibraryCommand::Preview || command == ThemeLibraryCommand::Share || command == ThemeLibraryCommand::ChooseCover ||
            command == ThemeLibraryCommand::Regenerate || command == ThemeLibraryCommand::SyncSubscriptions;
        if (async)
        {
            if (!async_) return;
            busy_ = true; PatchButtons(); const auto generation = generation_; auto alive = alive_;
            async_(generation, request, [this, alive, generation](ThemeLibraryResult result) {
                if (!alive->load()) return;
                busy_ = false; PatchButtons();
                if (generation != generation_) return;
                if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; publishedUrls_ = result.publishedUrls; RefreshChoices(); if (changed_) changed_(); }
                Feedback(result);
            });
            return;
        }
        const auto result = action_(generation_, request);
        if (result.succeeded)
        {
            library_ = result.library; sharing_ = result.sharingAvailable;
            if (command == ThemeLibraryCommand::Apply)
            {
                if (auto applied = themes::Resolve(library_.themes, request.id)) current_ = *applied;
                nativeIndex_ = customIndex_; edit_.Reset(); editing_ = false;
            }
            RefreshChoices();
            if (changed_) changed_();
            if (save && !result.savedId.empty())
            {
                const auto saved = themes::Resolve(library_.themes, result.savedId);
                if (saved && (saved->kind != themes::Kind::Global ||
                    (target_ == "global" ? themes::FullScope(saved->scopes) : (saved->scopes & themes::TargetScope(target_)) != 0)))
                { Run(ThemeLibraryCommand::Apply, result.savedId); return; }
            }
        }
        else if (command == ThemeLibraryCommand::Apply) RefreshChoices();
        Feedback(result, save);
    }
};
}
namespace snowdesktop::winui { using ThemeLibraryControls = theme_controls::ThemeLibraryControls; }
