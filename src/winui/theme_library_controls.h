#pragma once
#include "theme_library_actions.h"
#include "appearance_sections.h"
#include "../theme_library_settings.h"
#include <atomic>
#include <array>
#include <cwctype>
#include <memory>

namespace snowdesktop::winui::theme_controls
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = winrt::Microsoft::UI::Xaml::Controls;
// One form per appearance source. Refreshes never replace an unsaved draft.
// Transfer mode deliberately has only import/export and a file selection.
class ThemeLibraryControls final
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    ThemeLibraryControls(Localize localize, std::string target, bool transfer = false)
        : localize_(std::move(localize)), target_(std::move(target)), transfer_(transfer)
    {
        root_.Spacing(8);
        choiceRow_.Initialize(choice_); root_.Children().Append(choiceRow_.root);
        for (auto combo : {choice_, quick_, popup_}) combo.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        commands_.DefaultLabelPosition(c::CommandBarDefaultLabelPosition::Right);
        root_.Children().Append(commands_);
        if (transfer_)
        {
            typeRow_.Initialize(type_); root_.Children().InsertAt(0, typeRow_.root);
            type_.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            AddCommand(ThemeLibraryCommand::Import, "themeLibrary.import");
            AddCommand(ThemeLibraryCommand::Export, "themeLibrary.export");
        }
        else
        {
            AddCommand(ThemeLibraryCommand::Apply, "themeLibrary.apply");
            AddCommand(ThemeLibraryCommand::Preview, "themeLibrary.preview");
            for (auto [command, key] : {std::pair{ThemeLibraryCommand::Share, "themeLibrary.share"},
                {ThemeLibraryCommand::ChooseCover, "themeLibrary.chooseCover"}, {ThemeLibraryCommand::Regenerate, "themeLibrary.regenerate"},
                {ThemeLibraryCommand::SyncSubscriptions, "themeLibrary.sync"}, {ThemeLibraryCommand::Remove, "themeLibrary.remove"}})
                AddCommand(command, key, true);
            saveBody_ = AppearanceSections::Section(saveRoot_, saveTitle_, &disclosures_);
            name_.MaxLength(128); nameRow_.Initialize(name_); saveBody_.Children().Append(nameRow_.root);
            if (Global())
            {
                c::StackPanel scopes; scopes.Spacing(8);
                for (auto check : scopeChecks_) { check.IsChecked(true); scopes.Children().Append(check); }
                scopeRow_.Initialize(scopes); saveBody_.Children().Append(scopeRow_.root);
                quickRow_.Initialize(quick_); popupRow_.Initialize(popup_);
                saveBody_.Children().Append(quickRow_.root); saveBody_.Children().Append(popupRow_.root);
                legacyHint_.TextWrapping(x::TextWrapping::Wrap); legacyHint_.Visibility(x::Visibility::Collapsed);
                saveBody_.Children().Append(legacyHint_);
            }
            for (auto [command, key] : {std::pair{ThemeLibraryCommand::SaveAs, "themeLibrary.saveAs"}, {ThemeLibraryCommand::Update, "themeLibrary.update"}})
            {
                c::Button button; button.HorizontalAlignment(x::HorizontalAlignment::Right);
                button.Content(winrt::box_value(L(key))); saveBody_.Children().Append(button);
                const auto token = button.Click([this, command](const auto&, const auto&) { Run(command); });
                revoke_.push_back([button, token] { button.Click(token); });
                saveButtons_.push_back({command, key, button});
            }
        }
        feedback_.IsOpen(false); root_.Children().Append(feedback_);
        saveFeedback_.IsOpen(false); if (!transfer_) saveBody_.Children().Append(saveFeedback_);
        const auto choiceToken = choice_.SelectionChanged([this](const auto&, const auto&) {
            if (syncing_) return;
            PatchButtons();
            if (!transfer_) LoadDraft();
        });
        revoke_.push_back([control = choice_, choiceToken] { control.SelectionChanged(choiceToken); });
        const auto typeToken = type_.SelectionChanged([this](const auto&, const auto&) { if (!syncing_) RefreshChoices(); });
        revoke_.push_back([control = type_, typeToken] { control.SelectionChanged(typeToken); });
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
    x::FrameworkElement Choice() const { return choice_; }
    void SetActions(ThemeLibraryAction action, ThemeLibraryAsyncAction async, std::function<bool()> flush)
    { action_ = std::move(action); async_ = std::move(async); flush_ = std::move(flush); }
    void SetGeneration(std::uint64_t generation)
    {
        if (generation_ != generation) for (auto disclosure : disclosures_) disclosure.IsExpanded(false);
        generation_ = generation;
    }
    void Refresh()
    {
        if (!action_ || !generation_ || closed_) return;
        const auto result = action_(generation_, {});
        if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; RefreshChoices(); }
        else Feedback(result);
    }
    void Reveal() const { if (transfer_) return; AppearanceSections::RevealWithin(saveRoot_, name_); }
    void LocalizeText()
    {
        choiceRow_.SetText(L("themeLibrary.choose"), L(transfer_ ? "themeLibrary.transferHint" : "themeLibrary.selectionHint"));
        if (!transfer_) nameRow_.SetText(L("themeLibrary.name"), L("themeLibrary.nameHint"));
        if (saveTitle_) saveTitle_.Text(L("themeLibrary.save"));
        if (Global() && !transfer_)
        {
            scopeRow_.SetText(L("themeLibrary.appliesTo"));
            constexpr const char* keys[] = {"themeLibrary.dock", "themeLibrary.statusBar", "themeLibrary.taskbar"};
            for (std::size_t i = 0; i < scopeChecks_.size(); ++i) scopeChecks_[i].Content(winrt::box_value(L(keys[i])));
            quickRow_.SetText(L("themeLibrary.quickPanel")); popupRow_.SetText(L("themeLibrary.popup"));
            legacyHint_.Text(L("themeLibrary.legacyComponents"));
        }
        for (auto& button : buttons_) button.control.Label(L(button.key));
        for (auto& button : saveButtons_) button.control.Content(winrt::box_value(L(button.key)));
        if (transfer_)
        {
            syncing_ = true; const int selected = type_.SelectedIndex(); type_.Items().Clear();
            for (auto key : {"themeLibrary.global", "themeLibrary.quickPanel", "themeLibrary.popup"}) type_.Items().Append(winrt::box_value(L(key)));
            type_.SelectedIndex(std::max(selected, 0)); syncing_ = false;
            typeRow_.SetText(L("themeLibrary.type"));
        }
        RefreshChoices();
    }
    void Close()
    {
        if (closed_) return;
        closed_ = true; alive_->store(false);
        for (auto& revoke : revoke_) revoke();
        revoke_.clear(); action_ = {}; async_ = {}; flush_ = {};
    }
private:
    Localize localize_;
    std::string target_;
    bool transfer_ = false, syncing_ = false, busy_ = false, sharing_ = false, closed_ = false;
    std::uint64_t generation_ = 0;
    ThemeLibraryAction action_;
    ThemeLibraryAsyncAction async_;
    std::function<bool()> flush_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    themes::Library library_;
    std::vector<themes::Theme> choices_, quickChoices_, popupChoices_;
    c::StackPanel root_, saveRoot_, saveBody_{nullptr};
    c::TextBlock saveTitle_{nullptr}, legacyHint_;
    c::ComboBox choice_, type_, quick_, popup_;
    c::TextBox name_;
    presenter_controls::SettingRow choiceRow_, typeRow_, nameRow_, scopeRow_, quickRow_, popupRow_;
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
        const auto index = choice_.SelectedIndex();
        return index >= 0 && static_cast<std::size_t>(index) < choices_.size() ? &choices_[index] : nullptr;
    }
    std::wstring Label(const themes::Theme& theme) const
    {
        if (!themes::Builtin(theme.id)) return winrt::to_hstring(theme.name).c_str();
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
        if (preferred.empty() && Selected()) preferred = Selected()->id;
        const auto quick = Binding(quick_, quickChoices_), popup = Binding(popup_, popupChoices_);
        const bool quickUnset = quick_.SelectedIndex() < 0, popupUnset = popup_.SelectedIndex() < 0;
        const auto kind = transfer_ ? (type_.SelectedIndex() == 1 ? themes::Kind::QuickPanel : type_.SelectedIndex() == 2 ? themes::Kind::Popup : themes::Kind::Global) : themes::TargetKind(target_);
        choices_ = themes::Choices(library_, kind, transfer_ || target_ == "global" ? 0 : themes::TargetScope(target_));
        if (transfer_) std::erase_if(choices_, [](auto const& theme) { return themes::Builtin(theme.id); });
        syncing_ = true; choice_.Items().Clear(); int selected = -1;
        for (std::size_t i = 0; i < choices_.size(); ++i)
        {
            choice_.Items().Append(winrt::box_value(Label(choices_[i])));
            if (choices_[i].id == preferred) selected = static_cast<int>(i);
        }
        choice_.SelectedIndex(selected);
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
        syncing_ = false; PatchButtons();
    }
    void LoadDraft()
    {
        const auto selected = Selected();
        if (!selected) return;
        name_.Text(Label(*selected));
        if (!Global()) return;
        for (std::size_t i = 0; i < scopeChecks_.size(); ++i) scopeChecks_[i].IsChecked((selected->scopes & (themes::Dock << i)) != 0);
        SelectBinding(quick_, quickChoices_, selected->quickPanel); SelectBinding(popup_, popupChoices_, selected->popup);
    }
    void PatchButtons()
    {
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
        for (auto& button : saveButtons_) button.control.IsEnabled(!busy_ && (button.command == ThemeLibraryCommand::SaveAs || custom));
        if (Global() && !transfer_) legacyHint_.Visibility(custom && (selected->scopes & themes::Components) ? x::Visibility::Visible : x::Visibility::Collapsed);
    }
    void AddCommand(ThemeLibraryCommand command, const char* key, bool secondary = false)
    {
        c::AppBarButton button; button.Label(L(key));
        (secondary ? commands_.SecondaryCommands() : commands_.PrimaryCommands()).Append(button);
        const auto token = button.Click([this, command](const auto&, const auto&) {
            if (command == ThemeLibraryCommand::Remove) ConfirmRemove(); else Run(command);
        });
        revoke_.push_back([button, token] { button.Click(token); }); buttons_.push_back({command, key, button});
    }
    void Feedback(const ThemeLibraryResult& result, bool save = false)
    {
        auto feedback = save ? saveFeedback_ : feedback_;
        feedback.Severity(result.succeeded ? c::InfoBarSeverity::Success : c::InfoBarSeverity::Error);
        feedback.Message(result.message); feedback.IsOpen(!result.message.empty());
    }
    winrt::fire_and_forget ConfirmRemove()
    {
        const auto selected = Selected();
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
        for (auto const& theme : choices_) if (theme.id != id) { choices.push_back(theme); replacement.Items().Append(winrt::box_value(Label(theme))); }
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
    void Run(ThemeLibraryCommand command, std::string id = {}, std::string replacement = {})
    {
        if (closed_ || busy_ || !action_ || !generation_ || !flush_ || !flush_()) return;
        const bool save = command == ThemeLibraryCommand::SaveAs || command == ThemeLibraryCommand::Update;
        ThemeLibraryRequest request; request.command = command; request.target = target_; request.replacement = std::move(replacement);
        const auto selected = Selected(); request.id = id.empty() && selected ? selected->id : std::move(id);
        if (save)
        {
            constexpr auto whitespace = L" \t\r\n\v\f\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
            std::wstring name(name_.Text().c_str()); const auto first = name.find_first_not_of(whitespace);
            const auto last = name.find_last_not_of(whitespace);
            if (first == std::wstring::npos) { Feedback({false, {}, {}, L("themeLibrary.nameRequired")}, true); return; }
            request.name = winrt::to_string(name.substr(first, last - first + 1));
            if (Global())
            {
                request.scopes = 0;
                for (std::size_t i = 0; i < scopeChecks_.size(); ++i)
                    if (const auto check = scopeChecks_[i].IsChecked(); check && check.Value()) request.scopes |= themes::Dock << i;
                // Existing component references remain supported. New saves never
                // inherit the old component scope, even after selecting an old theme.
                if (command == ThemeLibraryCommand::Update && selected) request.scopes |= selected->scopes & themes::Components;
                if (!request.scopes) { Feedback({false, {}, {}, L("themeLibrary.scopesRequired")}, true); return; }
                if (quick_.SelectedIndex() < 0 || popup_.SelectedIndex() < 0)
                { Feedback({false, {}, {}, L("themeLibrary.bindingRequired")}, true); return; }
                request.quickPanel = Binding(quick_, quickChoices_); request.popup = Binding(popup_, popupChoices_);
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
                if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; RefreshChoices(); }
                Feedback(result);
            });
            return;
        }
        const auto result = action_(generation_, request);
        if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; RefreshChoices(result.savedId); }
        Feedback(result, save);
    }
};
}
namespace snowdesktop::winui { using ThemeLibraryControls = theme_controls::ThemeLibraryControls; }
