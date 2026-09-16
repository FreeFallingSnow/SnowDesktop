#include "pch.h"
#include "start_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <array>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxa = mux::Automation;
using onboarding::Task;

struct StartPagePresenter::Impl
{
    struct TaskCard
    {
        muxc::Expander root;
        muxc::TextBlock title, status, description, instructions;
        muxc::Button action;
        muxc::HyperlinkButton later;
        winrt::event_token actionToken{}, laterToken{};
    };
    struct ExploreCard
    {
        std::string key;
        SettingsRoute route;
        muxc::Button root;
        muxc::TextBlock title, description;
        winrt::event_token token{};
    };

    HomeAboutPagePresenter::LocalizeCallback localize;
    HomeAboutPageActions actions;
    muxc::StackPanel root, basics, explore;
    muxc::SelectorBar tabs;
    muxc::SelectorBarItem basicsTab, exploreTab;
    muxc::TextBlock progress, basicsDescription, exploreDescription;
    muxc::InfoBar experimentNotice;
    muxc::Expander collectionsGuide;
    muxc::TextBlock collectionsTitle, collectionsText;
    muxc::Button collectionsAction;
    winrt::event_token collectionsToken{};
    std::array<TaskCard, 4> tasks;
    std::vector<ExploreCard> discoveries;
    winrt::event_token tabToken{};
    std::uint64_t generation = 0, revision = 0;
    std::uint32_t steps = 0, deferred = 0;
    bool hasSnapshot = false, hasStatus = false, active = false, closed = false;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }

    static void Body(const muxc::TextBlock& text)
    {
        text.TextWrapping(mux::TextWrapping::Wrap);
    }

    Impl(HomeAboutPagePresenter::LocalizeCallback callback, const mux::Style& cardStyle)
        : localize(std::move(callback))
    {
        root.Spacing(16);
        tabs.Items().Append(basicsTab);
        tabs.Items().Append(exploreTab);
        tabs.SelectedItem(basicsTab);
        root.Children().Append(tabs);
        experimentNotice.IsClosable(false);
        experimentNotice.Severity(muxc::InfoBarSeverity::Informational);
        root.Children().Append(experimentNotice);
        basics.Spacing(10);
        explore.Spacing(10);
        Body(progress);
        progress.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        Body(basicsDescription);
        Body(exploreDescription);
        basics.Children().Append(progress);
        basics.Children().Append(basicsDescription);
        explore.Children().Append(exploreDescription);
        root.Children().Append(basics);
        root.Children().Append(explore);
        explore.Visibility(mux::Visibility::Collapsed);
        tabToken = tabs.SelectionChanged([this](auto&&, auto&&) {
            if (closed) return;
            const bool basic = tabs.SelectedItem() == basicsTab;
            basics.Visibility(basic ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            explore.Visibility(basic ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        });
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            auto& card = tasks[i];
            card.root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            card.root.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            muxc::StackPanel header;
            header.Spacing(4);
            for (const auto& text : {card.title, card.description, card.status}) Body(text);
            card.title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            card.description.Opacity(0.78);
            card.status.Opacity(0.78);
            header.Children().Append(card.title);
            header.Children().Append(card.description);
            header.Children().Append(card.status);
            card.root.Header(header);
            muxc::StackPanel content;
            content.Spacing(12);
            Body(card.instructions);
            content.Children().Append(card.instructions);
            // Stacked commands remain reachable with large text and long locales.
            card.action.HorizontalAlignment(mux::HorizontalAlignment::Left);
            card.later.HorizontalAlignment(mux::HorizontalAlignment::Left);
            content.Children().Append(card.action);
            content.Children().Append(card.later);
            card.root.Content(content);
            const auto task = static_cast<Task>(i);
            card.actionToken = card.action.Click([this, task](auto&&, auto&&) {
                if (active && hasSnapshot && !closed && actions.onboardingTask)
                    actions.onboardingTask(generation, task, false);
            });
            card.laterToken = card.later.Click([this, task, i](auto&&, auto&&) {
                if (active && hasSnapshot && !closed && actions.onboardingTask)
                {
                    actions.onboardingTask(generation, task, true);
                    tasks[i].root.IsExpanded(false);
                }
            });
            muxa::AutomationProperties::SetAutomationId(card.root,
                winrt::to_hstring("start." + std::string(onboarding::TaskKey(task))));
            basics.Children().Append(card.root);
        }
        tasks.front().root.IsExpanded(true);
        const auto add = [&](const char* key, SettingsPage page, const char* focus = "") {
            ExploreCard card;
            card.key = key;
            card.route = SettingsRoute::ForPage(page, focus);
            card.root.Style(cardStyle);
            card.root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            card.root.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            muxc::StackPanel content;
            content.Spacing(5);
            Body(card.title);
            Body(card.description);
            card.title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            card.description.Opacity(0.78);
            content.Children().Append(card.title);
            content.Children().Append(card.description);
            card.root.Content(content);
            const auto route = card.route;
            card.token = card.root.Click([this, route](auto&&, auto&&) {
                if (active && hasSnapshot && !closed && actions.navigate) actions.navigate(route);
            });
            explore.Children().Append(card.root);
            discoveries.push_back(std::move(card));
        };
        add("organize", SettingsPage::DesktopCategories);
        collectionsGuide.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        collectionsGuide.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        Body(collectionsTitle);
        Body(collectionsText);
        collectionsTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        collectionsGuide.Header(collectionsTitle);
        muxc::StackPanel collectionsBody;
        collectionsBody.Spacing(12);
        collectionsBody.Children().Append(collectionsText);
        collectionsAction.HorizontalAlignment(mux::HorizontalAlignment::Left);
        collectionsBody.Children().Append(collectionsAction);
        collectionsGuide.Content(collectionsBody);
        collectionsToken = collectionsAction.Click([this](auto&&, auto&&) {
            if (active && hasSnapshot && !closed && actions.invoke)
                actions.invoke(generation, HomeAboutCommand::OpenWidgetMenu);
        });
        explore.Children().Append(collectionsGuide);
        add("dock", SettingsPage::Dock);
        add("navigation", SettingsPage::General, "general.quickNavigation");
        add("pages", SettingsPage::DesktopPages);
        add("appearance", SettingsPage::AppearanceTheme);
        add("widgets", SettingsPage::Widgets);
        add("backup", SettingsPage::BackupAndData);
        RefreshLocalizedText();
        Render();
    }

    void RefreshLocalizedText()
    {
        basicsTab.Text(L("start.basics"));
        exploreTab.Text(L("start.explore"));
        basicsDescription.Text(L("start.basics.description"));
        exploreDescription.Text(L("start.explore.description"));
        experimentNotice.Title(L("start.experiment"));
        experimentNotice.Message(L("start.experiment.description"));
        collectionsTitle.Text(L("start.explore.collections"));
        collectionsText.Text(L("start.explore.collections.description"));
        collectionsAction.Content(winrt::box_value(L("start.explore.collections.action")));
        muxa::AutomationProperties::SetName(collectionsGuide, collectionsTitle.Text());
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            auto& card = tasks[i];
            const std::string prefix = "start." + std::string(onboarding::TaskKey(static_cast<Task>(i)));
            card.title.Text(std::to_wstring(i + 1) + L". " + L(prefix + ".title"));
            card.description.Text(L(prefix + ".description"));
            card.instructions.Text(L(prefix + ".instructions"));
            card.action.Content(winrt::box_value(L(prefix + ".action")));
            card.later.Content(winrt::box_value(L("start.later")));
            muxa::AutomationProperties::SetName(card.root, card.title.Text());
        }
        for (auto& card : discoveries)
        {
            card.title.Text(L("start.explore." + card.key));
            card.description.Text(L("start.explore." + card.key + ".description"));
            muxa::AutomationProperties::SetName(card.root, card.title.Text());
            muxa::AutomationProperties::SetHelpText(card.root, card.description.Text());
        }
        Render();
    }

    void Render()
    {
        if (closed) return;
        unsigned completed = 0;
        bool next = true;
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            auto& card = tasks[i];
            const auto task = static_cast<Task>(i);
            const bool done = onboarding::Completed(steps, task);
            const bool later = (deferred & (1u << i)) != 0;
            if (done) ++completed;
            std::string key = done ? "start.completed" : later ? "start.deferred"
                : next ? "start.next" : "start.notStarted";
            if (!done && !later) next = false;
            std::wstring status = L(key);
            if (task == Task::Layout && !done)
            {
                if (steps & onboarding::kMoved) status += L" · " + L("start.layout.resizeRemaining");
                else if (steps & onboarding::kResized) status += L" · " + L("start.layout.moveRemaining");
            }
            card.status.Text(status);
            card.later.Visibility(done || later ? mux::Visibility::Collapsed : mux::Visibility::Visible);
            card.action.IsEnabled(hasSnapshot);
            card.later.IsEnabled(hasSnapshot);
            muxa::AutomationProperties::SetHelpText(card.root, status);
        }
        std::wstring summary = L("start.progress");
        if (const auto pos = summary.find(L"{0}"); pos != std::wstring::npos)
            summary.replace(pos, 3, std::to_wstring(completed));
        progress.Text(summary);
    }

    void Close()
    {
        if (closed) return;
        closed = true;
        active = false;
        tabs.SelectionChanged(tabToken);
        collectionsAction.Click(collectionsToken);
        for (auto& task : tasks)
        {
            task.action.Click(task.actionToken);
            task.later.Click(task.laterToken);
        }
        for (auto& card : discoveries) card.root.Click(card.token);
        actions = {};
    }
};

StartPagePresenter::StartPagePresenter(HomeAboutPagePresenter::LocalizeCallback localize,
    const mux::Style& style) : impl_(std::make_unique<Impl>(std::move(localize), style)) {}
StartPagePresenter::~StartPagePresenter() { Close(); }
void StartPagePresenter::SetActions(HomeAboutPageActions actions) { impl_->actions = std::move(actions); }
void StartPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    if (!impl_->hasSnapshot || impl_->generation != snapshot.generation)
    {
        impl_->steps = impl_->deferred = 0;
        impl_->hasStatus = false;
    }
    impl_->generation = snapshot.generation;
    impl_->hasSnapshot = true;
    impl_->Render();
}
void StartPagePresenter::ApplyStatusPatch(const HomeAboutStatusPatch& patch)
{
    if (impl_->closed || !impl_->hasSnapshot || patch.generation != impl_->generation ||
        (impl_->hasStatus && patch.revision <= impl_->revision)) return;
    impl_->hasStatus = true;
    impl_->revision = patch.revision;
    if (patch.onboardingSteps) impl_->steps = *patch.onboardingSteps;
    if (patch.onboardingDeferred) impl_->deferred = *patch.onboardingDeferred;
    if (patch.temporaryInitializationEnabled)
    {
        if (*patch.temporaryInitializationEnabled && !impl_->experimentNotice.IsOpen())
            for (std::size_t i = 0; i < impl_->tasks.size(); ++i)
                impl_->tasks[i].root.IsExpanded(i == 0);
        impl_->experimentNotice.IsOpen(*patch.temporaryInitializationEnabled);
    }
    impl_->Render();
}
void StartPagePresenter::RefreshLocalizedText() { impl_->RefreshLocalizedText(); }
void StartPagePresenter::Activate(bool active) { impl_->active = active; }
void StartPagePresenter::Close() { if (impl_) impl_->Close(); }
mux::UIElement StartPagePresenter::Content() const { return impl_->root; }
void StartPagePresenter::SelectRoute(std::string_view id)
{
    if (id.empty()) return;
    if (id == "start.explore" || id == "start.collections")
    {
        impl_->tabs.SelectedItem(impl_->exploreTab);
        return;
    }
    impl_->tabs.SelectedItem(impl_->basicsTab);
    for (std::size_t i = 0; i < impl_->tasks.size(); ++i)
        if (id == "start." + std::string(onboarding::TaskKey(static_cast<Task>(i))))
        {
            impl_->tasks[i].root.IsExpanded(true);
            return;
        }
}
mux::FrameworkElement StartPagePresenter::FocusTarget(std::string_view id)
{
    if (id == "start.explore") return impl_->exploreTab;
    for (std::size_t i = 0; i < impl_->tasks.size(); ++i)
        if (id == "start." + std::string(onboarding::TaskKey(static_cast<Task>(i))))
            return impl_->tasks[i].root;
    return impl_->basicsTab;
}
}
