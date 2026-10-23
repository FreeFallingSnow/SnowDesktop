#include "pch.h"
#include "runtime_modules_page_presenter.h"
#include "settings_presenter_controls.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = runtime_modules;
namespace
{
constexpr std::array<std::string_view, m::kCount> kImpactKeys{
    "modules.taskbar.impact", "modules.tray.impact", "modules.windowMinimize.impact",
    "modules.startMenu.impact", "modules.wallpaperPreview.impact"};
constexpr std::array<std::string_view, m::kCount> kConflictKeys{
    "modules.taskbar.conflict", "modules.tray.conflict", "modules.windowMinimize.conflict",
    "modules.startMenu.conflict", "modules.wallpaperPreview.conflict"};
}
struct RuntimeModulesPagePresenter::Impl
{
    struct Lifetime { Impl* owner = nullptr; };
    struct Card
    {
        c::Border root;
        c::StackPanel body;
        c::TextBlock name, purpose, impact, state, details;
        c::Button button;
        c::Expander expander;
    };
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::function<std::wstring(std::string_view)> localize;
    DockPageActions actions;
    RuntimeModulesQuery query;
    c::StackPanel root;
    c::TextBlock introduction;
    c::Button restart;
    std::array<Card, m::kCount> cards;
    x::DispatcherTimer timer;
    m::Policy policy;
    std::uint64_t generation = 0, serial = 0;
    bool active = false, pending = false;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    std::string Key(std::size_t index, std::string_view suffix) const
    { return "modules." + std::string(m::kKeys[index]) + "." + std::string(suffix); }
    Impl(std::function<std::wstring(std::string_view)> value, const x::Style& style,
        DockPageActions callbacks, RuntimeModulesQuery provider)
        : localize(std::move(value)), actions(std::move(callbacks)), query(std::move(provider))
    {
        lifetime->owner = this;
        root.Spacing(8);
        introduction.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(introduction);
        for (std::size_t i = 0; i < cards.size(); ++i)
        {
            auto& card = cards[i];
            card.root.Style(style);
            card.body.Spacing(8);
            c::Grid heading;
            c::ColumnDefinition textColumn; textColumn.Width({1, x::GridUnitType::Star});
            c::ColumnDefinition actionColumn; actionColumn.Width({1, x::GridUnitType::Auto});
            heading.ColumnDefinitions().Append(textColumn); heading.ColumnDefinitions().Append(actionColumn);
            card.name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            card.name.TextWrapping(x::TextWrapping::Wrap);
            card.name.VerticalAlignment(x::VerticalAlignment::Center);
            card.button.Margin({16, 0, 0, 0});
            c::Grid::SetColumn(card.button, 1);
            heading.Children().Append(card.name); heading.Children().Append(card.button);
            card.body.Children().Append(heading);
            for (auto text : {card.purpose, card.state, card.impact})
            { text.TextWrapping(x::TextWrapping::Wrap); card.body.Children().Append(text); }
            card.details.TextWrapping(x::TextWrapping::Wrap);
            card.expander.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            card.expander.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
            card.expander.Content(card.details);
            card.body.Children().Append(card.expander);
            card.root.Child(card.body);
            root.Children().Append(card.root);
            const auto weak = std::weak_ptr<Lifetime>(lifetime);
            card.button.Click([weak, i](const auto&, const auto&) {
                const auto gate = weak.lock(); if (!gate || !gate->owner) return;
                auto& self = *gate->owner;
                if (!self.active || !self.actions.updateGeneral) return;
                const bool allow = !self.policy.allowed[i];
                self.actions.updateGeneral(self.generation, SettingsUpdateMode::PreviewAndCommit,
                    [i, allow](GeneralSettings& settings) { settings.runtimeModules.allowed[i] = allow; });
            });
        }
        restart.HorizontalAlignment(x::HorizontalAlignment::Left);
        restart.Visibility(x::Visibility::Collapsed);
        root.Children().Append(restart);
        const auto weak = std::weak_ptr<Lifetime>(lifetime);
        restart.Click([weak](const auto&, const auto&) {
            const auto gate = weak.lock(); if (!gate || !gate->owner) return;
            auto& self = *gate->owner;
            if (!self.active || !self.actions.confirm || !self.actions.invokeHost) return;
            const auto expected = self.generation;
            self.actions.confirm(expected, self.L("modules.restartExplorer"), self.L("modules.restartExplorerDescription"),
                [weak, expected](bool confirmed) {
                    const auto current = weak.lock();
                    if (!confirmed || !current || !current->owner || !current->owner->active || current->owner->generation != expected) return;
                    SettingsHostActions::Request request; request.action = SettingsHostActions::Action::RestartExplorer;
                    current->owner->actions.invokeHost(expected, std::move(request));
                });
        });
        timer.Interval(std::chrono::seconds(1));
        timer.Tick([weak](const auto&, const auto&) { const auto gate = weak.lock(); if (gate && gate->owner) gate->owner->Refresh(); });
        Text();
    }
    ~Impl() { timer.Stop(); lifetime->owner = nullptr; }
    void Text()
    {
        introduction.Text(L("modules.description"));
        restart.Content(winrt::box_value(L("modules.restartExplorer")));
        for (std::size_t i = 0; i < cards.size(); ++i)
        {
            auto& card = cards[i];
            card.name.Text(L(Key(i, "name")));
            card.purpose.Text(L(Key(i, "purpose")));
            card.impact.Text(L(kImpactKeys[i]));
            card.expander.Header(winrt::box_value(L("modules.compatibilityDetails")));
            card.button.Content(winrt::box_value(L(policy.allowed[i] ? "modules.disable" : "modules.allow")));
            x::Automation::AutomationProperties::SetName(card.button, L(Key(i, "name")) + L" · " + L(policy.allowed[i] ? "modules.disable" : "modules.allow"));
        }
    }
    void Refresh()
    {
        if (!active || pending || !query) return;
        pending = true;
        const auto expected = serial;
        const auto weak = std::weak_ptr<Lifetime>(lifetime);
        query([weak, expected](m::Snapshot snapshot) {
            const auto gate = weak.lock(); if (!gate || !gate->owner) return;
            auto& self = *gate->owner;
            self.pending = false;
            if (!self.active || self.serial != expected) return;
            if (snapshot.modules.empty())
            { for (auto& card : self.cards) card.state.Text(self.L("modules.unknownResident")); return; }
            bool explorer = false;
            for (const auto& status : snapshot.modules)
            {
                const auto i = static_cast<std::size_t>(status.module);
                if (i >= self.cards.size() || status.allowed != self.policy.allowed[i]) continue;
                auto& card = self.cards[i];
                const char* state = !status.allowed ? (status.active ? "modules.stopping" : "modules.disabled") :
                    status.active ? "modules.active" : status.error == ERROR_IO_PENDING ? "modules.loading" :
                    status.error == ERROR_NOT_SUPPORTED ? "modules.unsupported" : status.error ? "modules.failed" : "modules.idle";
                card.state.Text(self.L(state));
                std::wstring details = self.L(kConflictKeys[i]);
                details += L"\n\n" + self.L(status.targets.empty() ?
                    (status.residencyKnown ? "modules.noResident" : "modules.unknownResident") : "modules.resident");
                for (const auto& target : status.targets)
                {
                    details += L"\n" + target.name + L" (" + std::to_wstring(target.process) + L")";
                    if (!status.allowed && target.explorer) explorer = true;
                }
                if (status.error && status.error != ERROR_IO_PENDING) details += L"\n" + std::to_wstring(status.error);
                card.details.Text(details);
            }
            self.restart.Visibility(explorer ? x::Visibility::Visible : x::Visibility::Collapsed);
        });
    }
};
RuntimeModulesPagePresenter::RuntimeModulesPagePresenter(std::function<std::wstring(std::string_view)> localize,
    const x::Style& style, DockPageActions actions, RuntimeModulesQuery query)
    : impl_(std::make_unique<Impl>(std::move(localize), style, std::move(actions), std::move(query))) {}
RuntimeModulesPagePresenter::~RuntimeModulesPagePresenter() = default;
x::UIElement RuntimeModulesPagePresenter::Content() const { return impl_->root; }
void RuntimeModulesPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->generation != snapshot.generation || impl_->policy != snapshot.values.general.runtimeModules)
    { ++impl_->serial; }
    impl_->generation = snapshot.generation; impl_->policy = snapshot.values.general.runtimeModules;
    impl_->Text(); if (impl_->active) impl_->Refresh();
}
void RuntimeModulesPagePresenter::Activate() { impl_->active = true; impl_->timer.Start(); impl_->Refresh(); }
void RuntimeModulesPagePresenter::Deactivate() { impl_->active = false; ++impl_->serial; impl_->timer.Stop(); }
x::FrameworkElement RuntimeModulesPagePresenter::FocusTarget(std::string_view id) const
{
    for (std::size_t i = 0; i < m::kCount; ++i)
        if (id == "runtimeModules." + std::string(m::kKeys[i])) return impl_->cards[i].button;
    return impl_->cards[0].button;
}
}
