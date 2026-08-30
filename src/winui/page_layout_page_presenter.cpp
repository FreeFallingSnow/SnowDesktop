#include "pch.h"

#include "page_layout_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace
{

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(12.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.root.Child(card.content);
    page.Children().Append(card.root);
}

std::wstring FormatText(
    std::wstring value,
    std::initializer_list<std::wstring> arguments)
{
    std::size_t index = 0;
    for (const auto& argument : arguments)
    {
        const std::wstring marker = L"{" + std::to_wstring(index++) + L"}";
        if (const auto position = value.find(marker);
            position != std::wstring::npos)
        {
            value.replace(position, marker.size(), argument);
        }
    }
    return value;
}

std::wstring PageIdFromItem(
    const winrt::Windows::Foundation::IInspectable& item)
{
    const auto element = item.try_as<mux::FrameworkElement>();
    if (!element)
        return {};
    return winrt::unbox_value_or<winrt::hstring>(
        element.Tag(), winrt::hstring{}).c_str();
}

struct ConfirmationBridge
{
    std::function<void(bool)> completed;
};

} // namespace

struct PageLayoutPagePresenter::Impl
{
    Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style),
          confirmation(std::make_shared<ConfirmationBridge>())
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
    }

    struct RowControls
    {
        muxc::Button moveUp{nullptr};
        muxc::Button moveDown{nullptr};
    };

    LocalizeCallback localize;
    PageLayoutPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    SettingsCard pagesCard;
    SettingsCard gridCard;
    muxc::InfoBar feedback{nullptr};
    muxc::InfoBar mappingNotice{nullptr};
    muxc::ListView pageList{nullptr};
    muxc::Button addPageButton{nullptr};
    muxc::TextBlock selectedPageText{nullptr};
    muxc::TextBlock columnsLabel{nullptr};
    muxc::NumberBox columnsBox{nullptr};
    muxc::TextBlock rowsLabel{nullptr};
    muxc::NumberBox rowsBox{nullptr};
    std::unordered_map<std::wstring, RowControls> rowControls;
    PageLayoutSnapshot snapshot;
    std::wstring selectedPageId;
    std::shared_ptr<ConfirmationBridge> confirmation;
    bool hasSnapshot = false;
    bool orderDirty = false;
    bool updating = false;
    bool active = false;
    bool closed = false;
    bool confirmationPending = false;

    winrt::event_token selectionChangedToken{};
    winrt::event_token dragCompletedToken{};
    winrt::event_token addPageToken{};
    winrt::event_token columnsChangedToken{};
    winrt::event_token rowsChangedToken{};

    [[nodiscard]] std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);
        feedback = muxc::InfoBar{};
        feedback.IsOpen(false);
        feedback.IsClosable(true);
        root.Children().Append(feedback);

        InitializeCard(pagesCard, cardStyle, root);
        mappingNotice = muxc::InfoBar{};
        mappingNotice.Severity(muxc::InfoBarSeverity::Informational);
        mappingNotice.IsClosable(false);
        mappingNotice.IsOpen(true);
        pagesCard.content.Children().Append(mappingNotice);

        pageList = muxc::ListView{};
        pageList.SelectionMode(muxc::ListViewSelectionMode::Single);
        pageList.CanDragItems(true);
        pageList.CanReorderItems(true);
        pageList.AllowDrop(true);
        pageList.IsSwipeEnabled(false);
        pageList.MinHeight(180.0);
        pageList.MaxHeight(360.0);
        pagesCard.content.Children().Append(pageList);

        addPageButton = muxc::Button{};
        addPageButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        pagesCard.content.Children().Append(addPageButton);

        InitializeCard(gridCard, cardStyle, root);
        selectedPageText = muxc::TextBlock{};
        selectedPageText.Opacity(0.72);
        selectedPageText.TextWrapping(mux::TextWrapping::Wrap);
        gridCard.content.Children().Append(selectedPageText);

        muxc::StackPanel dimensions;
        dimensions.Orientation(muxc::Orientation::Vertical);
        dimensions.Spacing(8.0);

        muxc::Grid columnsRow;
        columnsRow.ColumnSpacing(12.0);
        muxc::ColumnDefinition columnsLabelDefinition;
        columnsLabelDefinition.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition columnsInputDefinition;
        columnsInputDefinition.Width(mux::GridLengthHelper::Auto());
        columnsRow.ColumnDefinitions().Append(columnsLabelDefinition);
        columnsRow.ColumnDefinitions().Append(columnsInputDefinition);
        columnsLabel = muxc::TextBlock{};
        columnsLabel.TextWrapping(mux::TextWrapping::Wrap);
        columnsLabel.VerticalAlignment(mux::VerticalAlignment::Center);
        columnsRow.Children().Append(columnsLabel);
        columnsBox = muxc::NumberBox{};
        columnsBox.MinWidth(180.0);
        columnsBox.Minimum(1.0);
        columnsBox.Maximum(50.0);
        columnsBox.SmallChange(1.0);
        columnsBox.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Inline);
        columnsBox.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        muxc::Grid::SetColumn(columnsBox, 1);
        columnsRow.Children().Append(columnsBox);
        dimensions.Children().Append(columnsRow);

        muxc::Grid rowsRow;
        rowsRow.ColumnSpacing(12.0);
        muxc::ColumnDefinition rowsLabelDefinition;
        rowsLabelDefinition.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition rowsInputDefinition;
        rowsInputDefinition.Width(mux::GridLengthHelper::Auto());
        rowsRow.ColumnDefinitions().Append(rowsLabelDefinition);
        rowsRow.ColumnDefinitions().Append(rowsInputDefinition);
        rowsLabel = muxc::TextBlock{};
        rowsLabel.TextWrapping(mux::TextWrapping::Wrap);
        rowsLabel.VerticalAlignment(mux::VerticalAlignment::Center);
        rowsRow.Children().Append(rowsLabel);
        rowsBox = muxc::NumberBox{};
        rowsBox.MinWidth(180.0);
        rowsBox.Minimum(1.0);
        rowsBox.Maximum(50.0);
        rowsBox.SmallChange(1.0);
        rowsBox.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Inline);
        rowsBox.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        muxc::Grid::SetColumn(rowsBox, 1);
        rowsRow.Children().Append(rowsBox);
        dimensions.Children().Append(rowsRow);
        gridCard.content.Children().Append(dimensions);
    }

    void HookEvents()
    {
        selectionChangedToken = pageList.SelectionChanged(
            [this](const auto&, const auto&) {
                if (closed || updating)
                    return;
                selectedPageId = PageIdFromItem(pageList.SelectedItem());
                UpdateGridEditor();
            });
        dragCompletedToken = pageList.DragItemsCompleted(
            [this](const auto&, const auto&) {
                if (closed || updating)
                    return;
                UpdateOrderState();
                ApplyOrder();
            });
        addPageToken = addPageButton.Click(
            [this](const auto&, const auto&) { AddPage(); });
        columnsChangedToken = columnsBox.ValueChanged(
            [this](const auto&, const auto&) { ConfirmGrid(); });
        rowsChangedToken = rowsBox.ValueChanged(
            [this](const auto&, const auto&) { ConfirmGrid(); });
    }

    [[nodiscard]] std::vector<std::wstring> CurrentOrder() const
    {
        std::vector<std::wstring> order;
        const auto items = pageList.Items();
        order.reserve(items.Size());
        for (std::uint32_t index = 0; index < items.Size(); ++index)
            order.push_back(PageIdFromItem(items.GetAt(index)));
        return order;
    }

    [[nodiscard]] const PageLayoutEntry* SelectedPage() const
    {
        const auto found = std::ranges::find(
            snapshot.pages, selectedPageId, &PageLayoutEntry::id);
        return found == snapshot.pages.end() ? nullptr : &*found;
    }

    [[nodiscard]] int IntegerValue(const muxc::NumberBox& box) const
    {
        const double value = box.Value();
        if (std::isnan(value))
            return 1;
        return std::clamp(static_cast<int>(std::lround(value)), 1, 50);
    }

    [[nodiscard]] std::wstring PageLabel(std::size_t index) const
    {
        return FormatText(L("app.grid.page_label"),
            {std::to_wstring(index + 1)});
    }

    [[nodiscard]] std::wstring RoleLabel(
        const PageLayoutEntry& page) const
    {
        if (page.activeOnLastMonitor)
        {
            if (page.role == PageLayoutRole::Overflow)
                return L("settings.pages.role.currentLast");
            return FormatText(L("settings.pages.role.current"),
                {std::to_wstring(page.monitorOrdinal)});
        }
        switch (page.role)
        {
        case PageLayoutRole::FixedMonitor:
            return FormatText(L("settings.pages.role.fixed"),
                {std::to_wstring(page.monitorOrdinal)});
        case PageLayoutRole::LastMonitorDefault:
            return FormatText(L("settings.pages.role.lastDefault"),
                {std::to_wstring(page.monitorOrdinal)});
        case PageLayoutRole::Overflow:
        default:
            return L("settings.pages.role.overflow");
        }
    }

    muxc::Grid BuildPageRow(
        const PageLayoutEntry& page, std::size_t index)
    {
        muxc::Grid row;
        row.Tag(winrt::box_value(winrt::hstring(page.id)));
        row.ColumnSpacing(12.0);
        row.Padding({4.0, 4.0, 4.0, 4.0});
        for (const auto width : {
                 mux::GridLengthHelper::Auto(),
                 mux::GridLengthHelper::FromValueAndType(
                     1.0, mux::GridUnitType::Star),
                 mux::GridLengthHelper::Auto(),
                 mux::GridLengthHelper::Auto(),
                 mux::GridLengthHelper::Auto()})
        {
            muxc::ColumnDefinition definition;
            definition.Width(width);
            row.ColumnDefinitions().Append(definition);
        }

        muxc::TextBlock grip;
        grip.Text(L"⋮⋮");
        grip.Opacity(0.55);
        grip.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(grip);

        muxc::StackPanel identity;
        identity.Spacing(2.0);
        muxc::TextBlock title;
        title.Text(PageLabel(index));
        title.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        muxc::TextBlock role;
        role.Text(RoleLabel(page));
        role.Opacity(0.72);
        role.TextWrapping(mux::TextWrapping::Wrap);
        identity.Children().Append(title);
        identity.Children().Append(role);
        muxc::Grid::SetColumn(identity, 1);
        row.Children().Append(identity);

        muxc::StackPanel metrics;
        metrics.Spacing(2.0);
        metrics.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock dimensions;
        dimensions.Text(FormatText(L("settings.pages.gridValue"),
            {std::to_wstring(page.columns), std::to_wstring(page.rows)}));
        dimensions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        muxc::TextBlock content;
        content.Text(FormatText(L("settings.pages.contentCount"),
            {std::to_wstring(page.itemCount),
             std::to_wstring(page.widgetCount)}));
        content.Opacity(0.72);
        content.HorizontalAlignment(mux::HorizontalAlignment::Right);
        metrics.Children().Append(dimensions);
        metrics.Children().Append(content);
        muxc::Grid::SetColumn(metrics, 2);
        row.Children().Append(metrics);

        muxc::Button moveUp;
        muxc::FontIcon upIcon;
        upIcon.Glyph(L"\xE74A");
        moveUp.Content(upIcon);
        moveUp.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(moveUp, 3);
        row.Children().Append(moveUp);

        muxc::Button moveDown;
        muxc::FontIcon downIcon;
        downIcon.Glyph(L"\xE74B");
        moveDown.Content(downIcon);
        moveDown.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(moveDown, 4);
        row.Children().Append(moveDown);

        const std::wstring upText = FormatText(
            L("settings.pages.moveUp"), {PageLabel(index)});
        const std::wstring downText = FormatText(
            L("settings.pages.moveDown"), {PageLabel(index)});
        muxa::AutomationProperties::SetName(moveUp, upText);
        muxa::AutomationProperties::SetName(moveDown, downText);
        muxc::ToolTipService::SetToolTip(moveUp, winrt::box_value(upText));
        muxc::ToolTipService::SetToolTip(moveDown, winrt::box_value(downText));
        moveUp.Click([this, pageId = page.id](const auto&, const auto&) {
            MovePage(pageId, -1);
        });
        moveDown.Click([this, pageId = page.id](const auto&, const auto&) {
            MovePage(pageId, 1);
        });
        rowControls[page.id] = {moveUp, moveDown};
        muxa::AutomationProperties::SetName(row, PageLabel(index));
        return row;
    }

    void RebuildPageList(std::optional<std::size_t> preferredIndex = {})
    {
        const std::wstring previousSelection = selectedPageId;
        const int previousIndex = pageList.SelectedIndex();
        const bool wasUpdating = updating;
        updating = true;
        pageList.Items().Clear();
        rowControls.clear();
        for (std::size_t index = 0; index < snapshot.pages.size(); ++index)
            pageList.Items().Append(BuildPageRow(snapshot.pages[index], index));

        int selection = -1;
        if (!previousSelection.empty())
        {
            for (std::size_t index = 0; index < snapshot.pages.size(); ++index)
            {
                if (snapshot.pages[index].id == previousSelection)
                {
                    selection = static_cast<int>(index);
                    break;
                }
            }
        }
        if (selection < 0 && preferredIndex && !snapshot.pages.empty())
        {
            selection = static_cast<int>(std::min(
                *preferredIndex, snapshot.pages.size() - 1));
        }
        if (selection < 0 && previousIndex >= 0 && !snapshot.pages.empty())
            selection = std::min(previousIndex,
                static_cast<int>(snapshot.pages.size() - 1));
        if (selection < 0 && !snapshot.pages.empty())
            selection = 0;
        pageList.SelectedIndex(selection);
        selectedPageId = selection >= 0
            ? snapshot.pages[static_cast<std::size_t>(selection)].id
            : std::wstring{};
        updating = wasUpdating;
        orderDirty = false;
        UpdateOrderButtons();
        UpdateGridEditor();
    }

    void UpdateOrderButtons()
    {
        const auto order = CurrentOrder();
        for (std::size_t index = 0; index < order.size(); ++index)
        {
            const auto found = rowControls.find(order[index]);
            if (found == rowControls.end())
                continue;
            found->second.moveUp.IsEnabled(index > 0);
            found->second.moveDown.IsEnabled(index + 1 < order.size());
        }
    }

    void UpdateOrderState()
    {
        const auto current = CurrentOrder();
        std::vector<std::wstring> saved;
        saved.reserve(snapshot.pages.size());
        for (const auto& page : snapshot.pages)
            saved.push_back(page.id);
        orderDirty = current != saved;
        UpdateOrderButtons();
    }

    void MovePage(const std::wstring& pageId, int delta)
    {
        if (closed || !active || delta == 0)
            return;
        const auto items = pageList.Items();
        std::uint32_t index = 0;
        for (; index < items.Size(); ++index)
        {
            if (PageIdFromItem(items.GetAt(index)) == pageId)
                break;
        }
        if (index >= items.Size())
            return;
        const int target = static_cast<int>(index) + delta;
        if (target < 0 || target >= static_cast<int>(items.Size()))
            return;
        const auto item = items.GetAt(index);
        items.RemoveAt(index);
        items.InsertAt(static_cast<std::uint32_t>(target), item);
        pageList.SelectedItem(item);
        UpdateOrderState();
        ApplyOrder();
    }

    void UpdateInteractionState()
    {
        const bool enabled = active && hasSnapshot && !confirmationPending;
        pageList.IsEnabled(enabled);
        addPageButton.IsEnabled(enabled);
        const bool gridEnabled = enabled && SelectedPage() != nullptr;
        columnsBox.IsEnabled(gridEnabled);
        rowsBox.IsEnabled(gridEnabled);
    }

    void UpdateGridEditor()
    {
        const bool wasUpdating = updating;
        updating = true;
        const PageLayoutEntry* page = SelectedPage();
        if (page)
        {
            const auto index = static_cast<std::size_t>(
                page - snapshot.pages.data());
            selectedPageText.Text(FormatText(
                L("settings.pages.selected"), {PageLabel(index)}));
            columnsBox.Value(page->columns);
            rowsBox.Value(page->rows);
        }
        else
        {
            selectedPageText.Text(L("settings.pages.noSelection"));
            columnsBox.Value(1.0);
            rowsBox.Value(1.0);
        }
        updating = wasUpdating;
        UpdateInteractionState();
    }

    std::wstring ImpactMessage(const PageGridChangeImpact& impact) const
    {
        return FormatText(L("settings.pages.gridImpact"),
            {std::to_wstring(impact.displacedItemCount),
             std::to_wstring(impact.displacedWidgetCount),
             std::to_wstring(impact.resizedWidgetCount)});
    }

    void ShowFeedback(
        muxc::InfoBarSeverity severity,
        std::wstring title,
        std::wstring message)
    {
        feedback.Severity(severity);
        feedback.Title(std::move(title));
        feedback.Message(std::move(message));
        feedback.IsOpen(true);
    }

    void ApplyResult(
        PageLayoutOperationResult result,
        std::wstring successMessage,
        std::optional<std::size_t> preferredIndex = {})
    {
        snapshot = std::move(result.snapshot);
        hasSnapshot = !snapshot.pages.empty() || snapshot.revision != 0;
        RebuildPageList(preferredIndex);
        if (result.Succeeded())
        {
            ShowFeedback(muxc::InfoBarSeverity::Success,
                L("settings.pages.status.success"),
                std::move(successMessage));
        }
        else if (result.status == PageLayoutOperationStatus::Stale)
        {
            ShowFeedback(muxc::InfoBarSeverity::Warning,
                L("settings.pages.status.changed"),
                L("settings.pages.status.stale"));
        }
        else
        {
            ShowFeedback(muxc::InfoBarSeverity::Error,
                L("settings.pages.status.error"),
                L("settings.pages.status.failed"));
        }
    }

    void RequestConfirmation(
        std::wstring title,
        std::wstring message,
        std::wstring primary,
        std::function<void(bool)> completed)
    {
        if (!actions.confirm)
        {
            completed(true);
            return;
        }
        confirmationPending = true;
        UpdateInteractionState();
        confirmation->completed =
            [this, completed = std::move(completed)](bool accepted) mutable {
                if (closed)
                    return;
                confirmationPending = false;
                UpdateInteractionState();
                completed(accepted);
            };
        const auto bridge = confirmation;
        actions.confirm(std::move(title), std::move(message),
            std::move(primary), [bridge](bool accepted) {
                if (bridge->completed)
                {
                    auto completion = std::move(bridge->completed);
                    bridge->completed = {};
                    completion(accepted);
                }
            });
    }

    void ApplyOrder()
    {
        if (closed || !active || !orderDirty || !actions.applyOrder)
            return;
        const int selectedIndex = pageList.SelectedIndex();
        ApplyResult(actions.applyOrder(snapshot.revision, CurrentOrder()),
            L("settings.pages.order.applied"),
            selectedIndex < 0
                ? std::optional<std::size_t>{}
                : std::optional<std::size_t>{
                    static_cast<std::size_t>(selectedIndex)});
    }

    void ConfirmGrid()
    {
        if (closed || updating || confirmationPending)
            return;
        const PageLayoutEntry* page = SelectedPage();
        if (!active || !page || !actions.applyGrid)
            return;
        const int columns = IntegerValue(columnsBox);
        const int rows = IntegerValue(rowsBox);
        if (columns == page->columns && rows == page->rows)
            return;
        const auto impact = actions.analyzeGrid
            ? actions.analyzeGrid(page->id, columns, rows)
            : PageGridChangeImpact{};
        if (impact.valid && impact.RequiresConfirmation())
        {
            const std::wstring pageId = page->id;
            RequestConfirmation(
                L("settings.pages.grid.confirm.title"),
                ImpactMessage(impact),
                L("settings.dialog.confirm"),
                [this, pageId, columns, rows](bool accepted) {
                    if (accepted)
                        ApplyGrid(pageId, columns, rows);
                    else
                        UpdateGridEditor();
                });
            return;
        }
        ApplyGrid(page->id, columns, rows);
    }

    void ApplyGrid(
        const std::wstring& pageId, int columns, int rows)
    {
        if (closed || !active || !actions.applyGrid)
            return;
        const int selectedIndex = pageList.SelectedIndex();
        ApplyResult(actions.applyGrid(
            snapshot.revision, pageId, columns, rows),
            L("settings.pages.grid.applied"),
            selectedIndex < 0
                ? std::optional<std::size_t>{}
                : std::optional<std::size_t>{
                    static_cast<std::size_t>(selectedIndex)});
    }

    void AddPage()
    {
        if (!active || !hasSnapshot || !actions.addPage)
            return;
        const std::size_t previousCount = snapshot.pages.size();
        ApplyResult(actions.addPage(snapshot.revision),
            L("settings.pages.added"), previousCount);
    }

    void RefreshSnapshot()
    {
        if (closed || !actions.capture)
            return;
        try
        {
            snapshot = actions.capture();
            hasSnapshot = snapshot.revision != 0;
            RebuildPageList();
        }
        catch (...)
        {
            hasSnapshot = false;
            snapshot = {};
            RebuildPageList();
            ShowFeedback(muxc::InfoBarSeverity::Error,
                L("settings.pages.status.error"),
                L("settings.pages.status.failed"));
        }
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        pagesCard.title.Text(L("settings.pages.manage"));
        gridCard.title.Text(L("settings.pages.grid"));
        mappingNotice.Title(L("settings.pages.mapping.title"));
        mappingNotice.Message(L("settings.pages.mapping.message"));
        addPageButton.Content(winrt::box_value(L("app.menu.add_page")));

        columnsLabel.Text(L("settings.pages.columns"));
        rowsLabel.Text(L("settings.pages.rows"));
        muxa::AutomationProperties::SetName(
            pageList, L("settings.pages.manage"));
        muxa::AutomationProperties::SetName(
            columnsBox, L("settings.pages.columns"));
        muxa::AutomationProperties::SetName(
            rowsBox, L("settings.pages.rows"));
        muxa::AutomationProperties::SetName(
            addPageButton, L("app.menu.add_page"));
        if (hasSnapshot)
            RebuildPageList();
    }

    void Close() noexcept
    {
        if (closed)
            return;
        active = false;
        closed = true;
        confirmation->completed = {};
        confirmationPending = false;
        try
        {
            pageList.SelectionChanged(selectionChangedToken);
            pageList.DragItemsCompleted(dragCompletedToken);
            addPageButton.Click(addPageToken);
            columnsBox.ValueChanged(columnsChangedToken);
            rowsBox.ValueChanged(rowsChangedToken);
            pageList.Items().Clear();
        }
        catch (...)
        {
        }
        rowControls.clear();
        actions = {};
        localize = {};
    }
};

PageLayoutPagePresenter::PageLayoutPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

PageLayoutPagePresenter::~PageLayoutPagePresenter()
{
    Close();
}

void PageLayoutPagePresenter::SetActions(PageLayoutPageActions actions)
{
    if (!impl_ || impl_->closed)
        return;
    impl_->actions = std::move(actions);
    if (impl_->active)
        impl_->RefreshSnapshot();
}

muxc::StackPanel PageLayoutPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

void PageLayoutPagePresenter::RefreshLocalizedText()
{
    if (impl_)
        impl_->RefreshLocalizedText();
}

void PageLayoutPagePresenter::Activate()
{
    if (!impl_ || impl_->closed)
        return;
    impl_->active = true;
    impl_->RefreshSnapshot();
}

void PageLayoutPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->active = false;
    impl_->confirmation->completed = {};
    impl_->confirmationPending = false;
}

mux::FrameworkElement PageLayoutPagePresenter::FocusTarget(
    std::string_view focusId) const
{
    if (!impl_)
        return nullptr;
    if (focusId == "pages.add")
        return impl_->addPageButton;
    if (focusId == "pages.grid" || focusId == "pages.columns")
        return impl_->columnsBox;
    if (focusId == "pages.rows")
        return impl_->rowsBox;
    return impl_->pageList;
}

void PageLayoutPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui
