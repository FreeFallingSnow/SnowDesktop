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
// Appearance forms extend the original selector; management owns library actions.
class ThemeLibraryControls final
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    ThemeLibraryControls(Localize localize, std::string target, bool transfer = false,
        c::ComboBox existing = nullptr, int modeCount = 0, int customIndex = -1)
        : localize_(std::move(localize)), target_(std::move(target)), transfer_(transfer),
          modeCount_(modeCount), customIndex_(customIndex)
    {
        if (existing) choice_ = existing;
        root_.Spacing(8);
        if (transfer_) { choiceRow_.Initialize(choice_); root_.Children().Append(choiceRow_.root); }
        for (auto combo : {choice_, quick_, popup_}) combo.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        commands_.DefaultLabelPosition(c::CommandBarDefaultLabelPosition::Right);
        if (transfer_) root_.Children().Append(commands_);
        if (transfer_)
        {
            typeRow_.Initialize(type_); root_.Children().InsertAt(0, typeRow_.root);
            type_.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            AddCommand(ThemeLibraryCommand::Import, "themeLibrary.import");
            AddCommand(ThemeLibraryCommand::Export, "themeLibrary.export");
            AddCommand(ThemeLibraryCommand::Preview, "themeLibrary.preview");
            for (auto [command, key] : {std::pair{ThemeLibraryCommand::Share, "themeLibrary.share"},
                {ThemeLibraryCommand::ChooseCover, "themeLibrary.chooseCover"}, {ThemeLibraryCommand::Regenerate, "themeLibrary.regenerate"},
                {ThemeLibraryCommand::SyncSubscriptions, "themeLibrary.sync"}, {ThemeLibraryCommand::Remove, "themeLibrary.remove"}})
                AddCommand(command, key, true);
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
    bool Synchronizing() const { return syncing_ || busy_; }
    int NativeSelection() const { return Selected() ? customIndex_ : choice_.SelectedIndex(); }
    // The controller values and last successful library reference are authoritative.
    void SyncSelection(int nativeIndex, const SettingsValues& values)
    {
        const bool initial = !current_;
        nativeIndex_ = nativeIndex;
        current_ = themes::CaptureTarget(target_, values);
        Refresh();
        if (initial && name_.Text().empty()) LoadDraft();
    }
    bool ApplySelection()
    {
        if (Synchronizing()) return true;
        const auto selected = Selected();
        if (!selected)
        {
            const auto reference = library_.references.find(target_);
            if (reference != library_.references.end() && !reference->second.id.empty() && action_)
            {
                ThemeLibraryRequest request; request.command = ThemeLibraryCommand::Detach; request.target = target_;
                const auto result = action_(generation_, request);
                if (!result.succeeded) { RefreshChoices(); Feedback(result); return true; }
                library_ = result.library;
            }
            nativeIndex_ = choice_.SelectedIndex(); return false;
        }
        Run(ThemeLibraryCommand::Apply, selected->id);
        return true;
    }
    void SetActions(ThemeLibraryAction action, ThemeLibraryAsyncAction async, std::function<bool()> flush)
    { action_ = std::move(action); async_ = std::move(async); flush_ = std::move(flush); }
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
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
        if (transfer_) choiceRow_.SetText(L("themeLibrary.choose"), L("themeLibrary.transferHint"));
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
        if (transfer_)
        {
            syncing_ = true; const int selected = type_.SelectedIndex(); type_.Items().Clear();
            for (unsigned scope : {themes::Bars, unsigned(themes::Dock), unsigned(themes::StatusBar), unsigned(themes::Taskbar),
                unsigned(themes::Dock | themes::StatusBar), unsigned(themes::Dock | themes::Taskbar), unsigned(themes::StatusBar | themes::Taskbar)})
                type_.Items().Append(winrt::box_value(ScopeLabel(scope)));
            for (auto key : {"themeLibrary.quickPanel", "themeLibrary.popup"}) type_.Items().Append(winrt::box_value(L(key)));
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
        revoke_.clear(); action_ = {}; async_ = {}; flush_ = {}; changed_ = {};
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
    std::vector<themes::Theme> choices_, quickChoices_, popupChoices_;
    c::StackPanel root_, saveRoot_, saveBody_{nullptr};
    c::TextBlock saveTitle_{nullptr};
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
        const auto index = choice_.SelectedIndex() - (transfer_ ? 0 : modeCount_);
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
        if (transfer_ && preferred.empty() && Selected()) preferred = Selected()->id;
        if (!transfer_)
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
        const auto kind = transfer_ ? (type_.SelectedIndex() == 7 ? themes::Kind::QuickPanel : type_.SelectedIndex() == 8 ? themes::Kind::Popup : themes::Kind::Global) : themes::TargetKind(target_);
        choices_ = themes::Choices(library_, kind, transfer_ || target_ == "global" ? 0 : themes::TargetScope(target_));
        std::erase_if(choices_, [this](auto const& theme) {
            if (themes::Builtin(theme.id)) return true;
            if (theme.kind != themes::Kind::Global) return false;
            if (!transfer_) return target_ == "global" && !themes::FullScope(theme.scopes);
            constexpr std::array<unsigned, 7> scopes{themes::Bars, themes::Dock, themes::StatusBar, themes::Taskbar,
                themes::Dock | themes::StatusBar, themes::Dock | themes::Taskbar, themes::StatusBar | themes::Taskbar};
            const int index = type_.SelectedIndex();
            return index == 0 ? !themes::FullScope(theme.scopes) : index < 0 || index >= 7 ||
                themes::FullScope(theme.scopes) || (theme.scopes & themes::Bars) != scopes[static_cast<std::size_t>(index)];
        });
        syncing_ = true;
        if (transfer_) choice_.Items().Clear();
        else while (choice_.Items().Size() > static_cast<unsigned>(modeCount_)) choice_.Items().RemoveAtEnd();
        int selected = transfer_ ? -1 : nativeIndex_;
        for (std::size_t i = 0; i < choices_.size(); ++i)
        {
            choice_.Items().Append(winrt::box_value(Label(choices_[i])));
            if (choices_[i].id == preferred) selected = static_cast<int>(i) + (transfer_ ? 0 : modeCount_);
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
        syncing_ = false; PatchButtons(); PatchBindings();
    }
    void LoadDraft()
    {
        const auto selected = Selected();
        if (!selected) return;
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
        if (transfer_) { choice_.IsEnabled(!busy_); type_.IsEnabled(!busy_); }
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
    void Run(ThemeLibraryCommand command, std::string id = {}, std::string replacement = {})
    {
        if (closed_ || busy_ || !action_ || !generation_ || !flush_ || !flush_())
        { if (!closed_ && command == ThemeLibraryCommand::Apply) RefreshChoices(); return; }
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
                if (result.succeeded) { library_ = result.library; sharing_ = result.sharingAvailable; RefreshChoices(); if (changed_) changed_(); }
                Feedback(result);
            });
            return;
        }
        const auto result = action_(generation_, request);
        if (result.succeeded)
        {
            library_ = result.library; sharing_ = result.sharingAvailable;
            if (command == ThemeLibraryCommand::Apply)
            { if (auto applied = themes::Resolve(library_.themes, request.id)) current_ = *applied; nativeIndex_ = customIndex_; }
            RefreshChoices();
            if (command == ThemeLibraryCommand::Apply) LoadDraft();
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
