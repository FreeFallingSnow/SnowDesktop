#include "pch.h"

#include "page_layout_page_presenter.h"
#include "settings_presenter_controls.h"
#include "layout/page_management_rules.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <chrono>
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
    muxc::StackPanel root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    muxc::Border surface{nullptr};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::StackPanel{};
    card.root.Spacing(8.0);
    card.content = muxc::StackPanel{};
    card.content.Spacing(0.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.root.Children().Append(card.title);
    card.surface = muxc::Border{};
    card.surface.Style(style);
    card.surface.Padding({0.0, 0.0, 0.0, 0.0});
    card.surface.Child(card.content);
    card.root.Children().Append(card.surface);
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
        muxc::Grid root{nullptr};
        muxc::TextBlock title{nullptr};
        muxc::TextBlock role{nullptr};
        muxc::TextBlock dimensions{nullptr};
        muxc::TextBlock content{nullptr};
        muxc::Button moveUp{nullptr};
        muxc::Button moveDown{nullptr};
    };

    LocalizeCallback localize;
    PageLayoutPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    SettingsCard pagesCard;
    muxc::TextBlock gridTitle{nullptr};
    muxc::InfoBar feedback{nullptr};
    muxc::TextBlock mappingText{nullptr};
    muxc::ListView pageList{nullptr};
    muxc::HyperlinkButton addPageButton{nullptr};
    muxc::HyperlinkButton deletePageButton{nullptr};
    muxc::TextBox nameBox{nullptr};
    presenter_controls::SettingRow nameRow;
    presenter_controls::SettingRow columnsRow;
    presenter_controls::SettingRow rowsRow;
    muxc::Button saveNameButton{nullptr};
    muxc::Button cancelNameButton{nullptr};
    muxc::TextBlock selectedPageText{nullptr};
    muxc::TextBlock columnsLabel{nullptr};
    muxc::NumberBox columnsBox{nullptr};
    muxc::TextBlock rowsLabel{nullptr};
    muxc::NumberBox rowsBox{nullptr};
    mux::DispatcherTimer refreshTimer{nullptr};
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
    bool pageDragActive = false;
    bool nameDirty = false;
    std::uint64_t nameEditRevision = 0;

    winrt::event_token selectionChangedToken{};
    winrt::event_token dragStartingToken{};
    winrt::event_token dragCompletedToken{};
    winrt::event_token addPageToken{};
    winrt::event_token deletePageToken{};
    winrt::event_token nameChangedToken{};
    winrt::event_token saveNameToken{};
    winrt::event_token cancelNameToken{};
    winrt::event_token columnsChangedToken{};
    winrt::event_token rowsChangedToken{};
    winrt::event_token refreshTickToken{};

    [[nodiscard]] std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(24.0);
        InitializeCard(pagesCard, cardStyle, root);
        feedback = muxc::InfoBar{};
        feedback.IsOpen(false);
        feedback.IsClosable(true);
        feedback.Margin({16.0, 16.0, 16.0, 0.0});
        presenter_controls::BindInfoBarVisibility(feedback);
        pagesCard.content.Children().Append(feedback);

        muxc::Border listRegion;
        listRegion.Padding({16.0, 16.0, 16.0, 8.0});
        muxc::StackPanel listContent;
        listContent.Spacing(8.0);
        listRegion.Child(listContent);
        pagesCard.content.Children().Append(listRegion);

        pageList = muxc::ListView{};
        pageList.SelectionMode(muxc::ListViewSelectionMode::Single);
        pageList.CanDragItems(true);
        pageList.CanReorderItems(true);
        pageList.AllowDrop(true);
        pageList.IsSwipeEnabled(false);
        pageList.MinHeight(68.0);
        pageList.MaxHeight(360.0);
        listContent.Children().Append(pageList);
        mappingText = muxc::TextBlock{};
        mappingText.FontSize(12.0);
        mappingText.Opacity(0.68);
        mappingText.TextWrapping(mux::TextWrapping::Wrap);
        mappingText.Visibility(mux::Visibility::Collapsed);
        listContent.Children().Append(mappingText);

        addPageButton = muxc::HyperlinkButton{};
        presenter_controls::ConfigureSettingsHeadingLink(addPageButton);
        muxc::StackPanel pageActions;
        pageActions.Orientation(muxc::Orientation::Horizontal);
        pageActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        pageActions.Spacing(8.0);
        deletePageButton = muxc::HyperlinkButton{};
        presenter_controls::ConfigureSettingsHeadingLink(deletePageButton);
        pageActions.Children().Append(deletePageButton);
        pageActions.Children().Append(addPageButton);
        muxc::Grid pageHeader;
        pageHeader.ColumnSpacing(12.0);
        muxc::ColumnDefinition titleColumn, addColumn;
        titleColumn.Width(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
        addColumn.Width(mux::GridLengthHelper::Auto());
        pageHeader.ColumnDefinitions().Append(titleColumn);
        pageHeader.ColumnDefinitions().Append(addColumn);
        pagesCard.root.Children().RemoveAt(0);
        pagesCard.title.VerticalAlignment(mux::VerticalAlignment::Center);
        pageHeader.Children().Append(pagesCard.title);
        muxc::Grid::SetColumn(pageActions, 1);
        pageHeader.Children().Append(pageActions);
        pagesCard.root.Children().InsertAt(0, pageHeader);

        gridTitle = muxc::TextBlock{};
        gridTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        gridTitle.TextWrapping(mux::TextWrapping::NoWrap);
        selectedPageText = muxc::TextBlock{};
        selectedPageText.Opacity(0.72);
        selectedPageText.TextWrapping(mux::TextWrapping::NoWrap);
        selectedPageText.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        selectedPageText.VerticalAlignment(mux::VerticalAlignment::Center);
        selectedPageText.HorizontalAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid gridHeading;
        gridHeading.ColumnSpacing(16.0);
        presenter_controls::StyleSettingsSubrow(gridHeading, false, 0.0);
        gridHeading.Padding({16.0, 12.0, 16.0, 8.0});
        gridHeading.MinHeight(0.0);
        muxc::ColumnDefinition headingTitle, headingContext;
        headingTitle.Width(mux::GridLengthHelper::Auto());
        headingContext.Width(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
        gridHeading.ColumnDefinitions().Append(headingTitle);
        gridHeading.ColumnDefinitions().Append(headingContext);
        gridTitle.VerticalAlignment(mux::VerticalAlignment::Center);
        gridHeading.Children().Append(gridTitle);
        muxc::Grid::SetColumn(selectedPageText, 1);
        gridHeading.Children().Append(selectedPageText);
        pagesCard.content.Children().Append(gridHeading);
        nameBox = muxc::TextBox{};
        nameBox.MaxLength(128); // Final shared validation counts Unicode scalars.
        nameBox.AcceptsReturn(false);
        nameBox.TextWrapping(mux::TextWrapping::NoWrap);
        muxc::Grid nameEditor;
        nameEditor.ColumnSpacing(8.0);
        muxc::ColumnDefinition nameColumn, nameActionsColumn;
        nameActionsColumn.Width(mux::GridLengthHelper::Auto());
        nameEditor.ColumnDefinitions().Append(nameColumn);
        nameEditor.ColumnDefinitions().Append(nameActionsColumn);
        nameEditor.Children().Append(nameBox);
        muxc::StackPanel nameActions;
        nameActions.Orientation(muxc::Orientation::Horizontal);
        nameActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        nameActions.Spacing(8.0);
        saveNameButton = muxc::Button{};
        cancelNameButton = muxc::Button{};
        nameActions.Children().Append(saveNameButton);
        nameActions.Children().Append(cancelNameButton);
        muxc::Grid::SetColumn(nameActions, 1);
        nameEditor.Children().Append(nameActions);
        nameRow.Initialize(nameEditor, 420.0);
        presenter_controls::StyleSettingsSubrow(nameRow.root, true, 0.0);
        pagesCard.content.Children().Append(nameRow.root);
        columnsBox = muxc::NumberBox{};
        columnsBox.Width(presenter_controls::kSettingNumberWidth);
        columnsBox.Minimum(1.0);
        columnsBox.Maximum(50.0);
        columnsBox.SmallChange(1.0);
        columnsBox.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        columnsBox.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        columnsRow.Initialize(columnsBox, presenter_controls::kSettingNumberWidth);
        presenter_controls::StyleSettingsSubrow(columnsRow.root, false, 0.0);
        columnsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        columnsRow.SetCompactEditor();
        columnsLabel = columnsRow.label;
        pagesCard.content.Children().Append(columnsRow.root);
        rowsBox = muxc::NumberBox{};
        rowsBox.Width(presenter_controls::kSettingNumberWidth);
        rowsBox.Minimum(1.0);
        rowsBox.Maximum(50.0);
        rowsBox.SmallChange(1.0);
        rowsBox.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        rowsBox.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        rowsRow.Initialize(rowsBox, presenter_controls::kSettingNumberWidth);
        presenter_controls::StyleSettingsSubrow(rowsRow.root, false, 0.0);
        rowsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        rowsRow.SetCompactEditor();
        rowsLabel = rowsRow.label;
        pagesCard.content.Children().Append(rowsRow.root);
    }

    void HookEvents()
    {
        selectionChangedToken = pageList.SelectionChanged(
            [this](const auto&, const auto&) {
                if (closed || updating)
                    return;
                selectedPageId = PageIdFromItem(pageList.SelectedItem());
                nameDirty = false;
                UpdateGridEditor();
            });
        dragStartingToken = pageList.DragItemsStarting(
            [this](const auto&, const auto&) {
                if (!closed)
                    pageDragActive = true;
            });
        dragCompletedToken = pageList.DragItemsCompleted(
            [this](const auto&, const auto&) {
                pageDragActive = false;
                if (closed || updating)
                    return;
                UpdateOrderState();
                ApplyOrder();
            });
        addPageToken = addPageButton.Click(
            [this](const auto&, const auto&) { AddPage(); });
        deletePageToken = deletePageButton.Click(
            [this](const auto&, const auto&) { DeletePage(); });
        nameChangedToken = nameBox.TextChanged([this](const auto&, const auto&) {
            if (closed || updating) return;
            if (!nameDirty) nameEditRevision = snapshot.revision;
            nameDirty = true;
            UpdateInteractionState();
        });
        saveNameToken = saveNameButton.Click([this](const auto&, const auto&) { SaveName(); });
        cancelNameToken = cancelNameButton.Click([this](const auto&, const auto&) {
            nameDirty = false;
            PresentSnapshot(snapshot);
            RefreshSnapshot();
        });
        columnsChangedToken = columnsBox.ValueChanged(
            [this](const auto&, const auto&) { ConfirmGrid(); });
        rowsChangedToken = rowsBox.ValueChanged(
            [this](const auto&, const auto&) { ConfirmGrid(); });

        refreshTimer = mux::DispatcherTimer{};
        refreshTimer.Interval(std::chrono::milliseconds(500));
        refreshTickToken = refreshTimer.Tick(
            [this](const auto&, const auto&) {
                if (closed || !active || updating || confirmationPending ||
                    pageDragActive || nameDirty)
                {
                    return;
                }
                RefreshSnapshot();
            });
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
        return page_management::DisplayName(FormatText(L("app.grid.page_label"),
            {std::to_wstring(index + 1)}), index < snapshot.pages.size()
            ? snapshot.pages[index].name : std::wstring_view{});
    }

    [[nodiscard]] std::wstring RoleLabel(
        const PageLayoutEntry& page) const
    {
        switch (page.role)
        {
        case PageLayoutRole::FixedMonitor:
            if (page.monitorOrdinal == 1)
                return L("settings.pages.role.first");
            return FormatText(L("settings.pages.role.fixed"),
                {std::to_wstring(page.monitorOrdinal)});
        case PageLayoutRole::LastMonitorDefault:
            return L("settings.pages.role.lastDefault");
        case PageLayoutRole::Overflow:
        default:
            return page.activeOnLastMonitor
                ? L("settings.pages.role.currentLast")
                : L("settings.pages.role.overflow");
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
        title.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        title.TextWrapping(mux::TextWrapping::Wrap);
        title.MaxLines(2);
        muxc::ToolTipService::SetToolTip(title, winrt::box_value(PageLabel(index)));
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
        dimensions.TextWrapping(mux::TextWrapping::Wrap);
        dimensions.MaxWidth(160);
        muxc::TextBlock content;
        content.Text(FormatText(L("settings.pages.contentCount"),
            {std::to_wstring(page.itemCount),
             std::to_wstring(page.widgetCount)}));
        content.Opacity(0.72);
        content.HorizontalAlignment(mux::HorizontalAlignment::Right);
        content.TextWrapping(mux::TextWrapping::Wrap);
        content.MaxWidth(160);
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
        rowControls[page.id] = {
            row, title, role, dimensions, content, moveUp, moveDown};
        muxa::AutomationProperties::SetName(row, PageLabel(index));
        return row;
    }

    [[nodiscard]] bool PageListMatchesSnapshot(
        const PageLayoutSnapshot& candidate) const
    {
        const auto items = pageList.Items();
        if (items.Size() != candidate.pages.size())
            return false;
        for (std::uint32_t index = 0; index < items.Size(); ++index)
        {
            if (PageIdFromItem(items.GetAt(index)) !=
                candidate.pages[index].id)
            {
                return false;
            }
        }
        return true;
    }

    void RefreshPageRowsInPlace()
    {
        const bool wasUpdating = updating;
        updating = true;
        for (std::size_t index = 0; index < snapshot.pages.size(); ++index)
        {
            const auto& page = snapshot.pages[index];
            const auto found = rowControls.find(page.id);
            if (found == rowControls.end())
                continue;
            auto& controls = found->second;
            controls.title.Text(PageLabel(index));
            muxc::ToolTipService::SetToolTip(controls.title, winrt::box_value(PageLabel(index)));
            controls.role.Text(RoleLabel(page));
            controls.dimensions.Text(FormatText(
                L("settings.pages.gridValue"),
                {std::to_wstring(page.columns),
                 std::to_wstring(page.rows)}));
            controls.content.Text(FormatText(
                L("settings.pages.contentCount"),
                {std::to_wstring(page.itemCount),
                 std::to_wstring(page.widgetCount)}));

            const std::wstring upText = FormatText(
                L("settings.pages.moveUp"), {PageLabel(index)});
            const std::wstring downText = FormatText(
                L("settings.pages.moveDown"), {PageLabel(index)});
            muxa::AutomationProperties::SetName(
                controls.moveUp, upText);
            muxa::AutomationProperties::SetName(
                controls.moveDown, downText);
            muxc::ToolTipService::SetToolTip(
                controls.moveUp, winrt::box_value(upText));
            muxc::ToolTipService::SetToolTip(
                controls.moveDown, winrt::box_value(downText));
            muxa::AutomationProperties::SetName(
                controls.root, PageLabel(index));
        }
        updating = wasUpdating;
        orderDirty = false;
        UpdateOrderButtons();
        UpdateGridEditor();
    }

    void PresentSnapshot(
        PageLayoutSnapshot candidate,
        std::optional<std::size_t> preferredIndex = {})
    {
        const bool updateInPlace = PageListMatchesSnapshot(candidate);
        snapshot = std::move(candidate);
        hasSnapshot = !snapshot.pages.empty() || snapshot.revision != 0;
        if (updateInPlace)
        {
            if (!nameDirty && preferredIndex && !snapshot.pages.empty())
            {
                const auto index = std::min(*preferredIndex, snapshot.pages.size() - 1);
                const bool wasUpdating = updating;
                updating = true;
                selectedPageId = snapshot.pages[index].id;
                pageList.SelectedIndex(static_cast<int>(index));
                updating = wasUpdating;
            }
            RefreshPageRowsInPlace();
        }
        else
            RebuildPageList(preferredIndex);
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
        if (selection < 0 && !nameDirty && preferredIndex && !snapshot.pages.empty())
        {
            selection = static_cast<int>(std::min(
                *preferredIndex, snapshot.pages.size() - 1));
        }
        if (selection < 0 && !nameDirty && previousIndex >= 0 && !snapshot.pages.empty())
            selection = std::min(previousIndex,
                static_cast<int>(snapshot.pages.size() - 1));
        if (selection < 0 && !nameDirty && !snapshot.pages.empty())
            selection = 0;
        pageList.SelectedIndex(selection);
        selectedPageId = selection >= 0
            ? snapshot.pages[static_cast<std::size_t>(selection)].id
            : nameDirty ? previousSelection : std::wstring{};
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
        auto order = CurrentOrder();
        const auto found = std::ranges::find(order, pageId);
        if (found == order.end())
            return;
        const auto index = static_cast<std::size_t>(
            std::distance(order.begin(), found));
        const auto target = static_cast<std::ptrdiff_t>(index) + delta;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(order.size()))
            return;

        // The button belongs to the row whose Click event is still being
        // dispatched. Removing and reinserting that live XAML row here can
        // detach the event source, and the synchronous backend refresh may
        // then clear it a second time. Submit the requested identity order
        // without mutating the active Items collection.
        std::swap(order[index], order[static_cast<std::size_t>(target)]);
        ApplyOrder(order, static_cast<std::size_t>(target));
    }

    void UpdateInteractionState()
    {
        const bool enabled = active && hasSnapshot && snapshot.editable && !confirmationPending;
        pageList.IsEnabled(enabled && !nameDirty);
        addPageButton.IsEnabled(enabled && !nameDirty);
        const bool gridEnabled = enabled && SelectedPage() != nullptr;
        columnsBox.IsEnabled(gridEnabled && !nameDirty);
        rowsBox.IsEnabled(gridEnabled && !nameDirty);
        nameBox.IsEnabled(gridEnabled);
        saveNameButton.IsEnabled(gridEnabled && nameDirty);
        cancelNameButton.IsEnabled(enabled && nameDirty);
        nameRow.help.Text(L("settings.pages.unsavedName"));
        nameRow.help.Visibility(nameDirty ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        deletePageButton.IsEnabled(gridEnabled && !nameDirty &&
            snapshot.pages.size() > std::max<std::size_t>(1, snapshot.monitorCount));
        muxc::ToolTipService::SetToolTip(deletePageButton, winrt::box_value(
            deletePageButton.IsEnabled() ? L("settings.pages.delete") : L("settings.pages.deleteUnavailable")));
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
            presenter_controls::SyncNumberBoxValue(columnsBox, page->columns);
            presenter_controls::SyncNumberBoxValue(rowsBox, page->rows);
            if (!nameDirty) nameBox.Text(page->name);
        }
        else
        {
            selectedPageText.Text(L("settings.pages.noSelection"));
            presenter_controls::SyncNumberBoxValue(columnsBox, 1.0);
            presenter_controls::SyncNumberBoxValue(rowsBox, 1.0);
            if (!nameDirty) nameBox.Text(L"");
        }
        muxc::ToolTipService::SetToolTip(selectedPageText, winrt::box_value(selectedPageText.Text()));
        muxa::AutomationProperties::SetName(selectedPageText, selectedPageText.Text());
        mappingText.Visibility(snapshot.monitorCount > 1 || snapshot.pages.size() > 1
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
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
        PresentSnapshot(std::move(result.snapshot), preferredIndex);
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
                result.message.empty() ? L("settings.pages.status.failed") : result.message);
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

    void ApplyOrder(
        const std::vector<std::wstring>& order,
        std::optional<std::size_t> preferredIndex = {})
    {
        if (closed || !active || !actions.applyOrder)
            return;
        std::vector<std::wstring> saved;
        saved.reserve(snapshot.pages.size());
        for (const auto& page : snapshot.pages)
            saved.push_back(page.id);
        if (order == saved)
            return;
        ApplyResult(actions.applyOrder(snapshot.revision, order),
            L("settings.pages.order.applied"), preferredIndex);
    }

    void ApplyOrder()
    {
        if (!orderDirty)
            return;
        const int selectedIndex = pageList.SelectedIndex();
        ApplyOrder(CurrentOrder(), selectedIndex < 0
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

    void SaveName()
    {
        if (closed || !active || !nameDirty || !actions.renamePage || !SelectedPage()) return;
        const std::wstring text = nameBox.Text().c_str();
        if (!page_management::NormalizeName(text))
        {
            ShowFeedback(muxc::InfoBarSeverity::Error,
                L("settings.pages.status.error"), L("settings.pages.nameInvalid"));
            return;
        }
        auto result = actions.renamePage(nameEditRevision, selectedPageId, text);
        if (!result.Succeeded())
        {
            if (result.status == PageLayoutOperationStatus::Stale)
            {
                // Keep this object's draft; the next explicit Save uses the
                // refreshed revision and never renames a different page.
                snapshot = std::move(result.snapshot);
                nameEditRevision = snapshot.revision;
                UpdateGridEditor();
            }
            ShowFeedback(result.status == PageLayoutOperationStatus::Stale
                    ? muxc::InfoBarSeverity::Warning : muxc::InfoBarSeverity::Error,
                L("settings.pages.status.error"), result.message.empty()
                    ? L(result.status == PageLayoutOperationStatus::Stale ? "settings.pages.status.stale" : "settings.pages.status.failed")
                    : result.message);
            return;
        }
        nameDirty = false;
        ApplyResult(std::move(result), L("settings.pages.renamed"));
    }

    void DeletePage()
    {
        if (closed || !active || nameDirty || !SelectedPage() ||
            !actions.analyzeRemoval || !actions.removePage) return;
        const auto id = selectedPageId;
        const auto revision = snapshot.revision;
        const auto index = static_cast<std::size_t>(SelectedPage() - snapshot.pages.data());
        const auto impact = actions.analyzeRemoval(id);
        if (!impact.valid)
        {
            ShowFeedback(muxc::InfoBarSeverity::Warning,
                L("settings.pages.status.error"), L("settings.pages.deleteUnavailable"));
            RefreshSnapshot();
            return;
        }
        auto remove = [this, id, revision, index](bool accepted) {
            if (!accepted || closed || !active) return;
            selectedPageId.clear();
            ApplyResult(actions.removePage(revision, id),
                L("settings.pages.deleted"), index == 0 ? 0 : index - 1);
        };
        if (!impact.RequiresConfirmation()) remove(true);
        else if (actions.confirm)
            RequestConfirmation(L("settings.pages.deleteConfirm.title"),
                FormatText(L("settings.pages.deleteConfirm.message"),
                    {PageLabel(index), std::to_wstring(impact.itemCount),
                     std::to_wstring(impact.widgetCount)}),
                L("settings.pages.delete"), std::move(remove));
    }

    void RefreshSnapshot()
    {
        if (closed || !actions.capture)
            return;
        try
        {
            auto candidate = actions.capture();
            if (hasSnapshot && candidate.revision == snapshot.revision)
                return;
            PresentSnapshot(std::move(candidate));
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
        gridTitle.Text(L("settings.pages.details"));
        mappingText.Text(L("settings.pages.mapping.message"));
        addPageButton.Content(winrt::box_value(L("app.menu.add_page")));
        deletePageButton.Content(winrt::box_value(L("settings.pages.delete")));
        nameRow.SetText(L("settings.pages.name"));
        nameBox.PlaceholderText(L("settings.pages.nameHint"));
        saveNameButton.Content(winrt::box_value(L("settings.pages.saveName")));
        cancelNameButton.Content(winrt::box_value(L("app.settings.cancel")));
        muxa::AutomationProperties::SetName(nameBox, L("settings.pages.name"));

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
            refreshTimer.Stop();
            refreshTimer.Tick(refreshTickToken);
            pageList.SelectionChanged(selectionChangedToken);
            pageList.DragItemsStarting(dragStartingToken);
            pageList.DragItemsCompleted(dragCompletedToken);
            addPageButton.Click(addPageToken);
            deletePageButton.Click(deletePageToken);
            nameBox.TextChanged(nameChangedToken);
            saveNameButton.Click(saveNameToken);
            cancelNameButton.Click(cancelNameToken);
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
    if (!impl_->nameDirty) impl_->RefreshSnapshot();
    impl_->UpdateGridEditor();
    impl_->refreshTimer.Start();
}

void PageLayoutPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->refreshTimer.Stop();
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
    if (focusId == "pages.name")
        return impl_->nameBox;
    if (focusId == "pages.delete")
        return impl_->deletePageButton;
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
