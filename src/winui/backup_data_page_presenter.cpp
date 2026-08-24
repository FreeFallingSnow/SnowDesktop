#include "pch.h"

#include "backup_data_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <atomic>
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
    muxc::TextBlock description{nullptr};
};

struct AsyncGate
{
    std::atomic_bool alive{true};
    std::atomic_bool active{false};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> revision{0};
};

[[nodiscard]] bool IsCallbackCurrent(
    const std::shared_ptr<AsyncGate>& gate,
    std::uint64_t generation,
    std::uint64_t revision) noexcept
{
    return gate && gate->alive.load(std::memory_order_acquire) &&
        gate->active.load(std::memory_order_acquire) &&
        gate->generation.load(std::memory_order_acquire) == generation &&
        gate->revision.load(std::memory_order_acquire) == revision;
}

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    if (style)
        card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(12.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.description = muxc::TextBlock{};
    card.description.Opacity(0.72);
    card.description.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.content.Children().Append(card.description);
    card.root.Child(card.content);
    page.Children().Append(card.root);
}

muxc::Button NewButton()
{
    muxc::Button button{};
    button.HorizontalAlignment(mux::HorizontalAlignment::Left);
    button.UseSystemFocusVisuals(true);
    return button;
}

muxc::TextBlock NewEmptyMessage()
{
    muxc::TextBlock message{};
    message.Opacity(0.62);
    message.TextWrapping(mux::TextWrapping::Wrap);
    return message;
}

muxc::Grid NewTwoColumnGrid()
{
    muxc::Grid grid{};
    muxc::ColumnDefinition contentColumn{};
    contentColumn.Width(mux::GridLengthHelper::FromValueAndType(
        1.0, mux::GridUnitType::Star));
    muxc::ColumnDefinition actionColumn{};
    actionColumn.Width(mux::GridLengthHelper::Auto());
    grid.ColumnDefinitions().Append(contentColumn);
    grid.ColumnDefinitions().Append(actionColumn);
    return grid;
}

muxc::InfoBarSeverity ToInfoBarSeverity(
    BackupDataNoticeSeverity severity) noexcept
{
    switch (severity)
    {
    case BackupDataNoticeSeverity::Success:
        return muxc::InfoBarSeverity::Success;
    case BackupDataNoticeSeverity::Warning:
        return muxc::InfoBarSeverity::Warning;
    case BackupDataNoticeSeverity::Error:
        return muxc::InfoBarSeverity::Error;
    case BackupDataNoticeSeverity::Informational:
    default:
        return muxc::InfoBarSeverity::Informational;
    }
}

} // namespace

struct BackupDataPagePresenter::Impl
{
    struct LayoutRow
    {
        LayoutBackupEntry entry;
        muxc::ListViewItem item{nullptr};
        muxc::TextBlock title{nullptr};
        muxc::TextBlock metadata{nullptr};
        muxc::Button restore{nullptr};
        muxc::Button remove{nullptr};
        winrt::event_token restoreToken{};
        winrt::event_token removeToken{};
    };

    struct FullBackupRow
    {
        FullDataBackupEntry entry;
        muxc::ListViewItem item{nullptr};
        muxc::TextBlock title{nullptr};
        muxc::TextBlock metadata{nullptr};
        muxc::Button restore{nullptr};
        muxc::Button exportArchive{nullptr};
        muxc::Button open{nullptr};
        muxc::Button remove{nullptr};
        winrt::event_token restoreToken{};
        winrt::event_token exportToken{};
        winrt::event_token openToken{};
        winrt::event_token removeToken{};
    };

    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style),
          asyncGate(std::make_shared<AsyncGate>())
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
        RenderState();
    }

    LocalizeCallback localize;
    BackupDataPageActions actions;
    mux::Style cardStyle{nullptr};
    std::shared_ptr<AsyncGate> asyncGate;

    muxc::StackPanel root{nullptr};
    muxc::InfoBar infoBar{nullptr};
    muxc::Border progressCard{nullptr};
    muxc::StackPanel progressContent{nullptr};
    muxc::ProgressRing progressRing{nullptr};
    muxc::TextBlock progressMessage{nullptr};
    muxc::Button cancelButton{nullptr};

    SettingsCard layoutCard;
    muxc::TextBox layoutName{nullptr};
    muxc::Button createLayoutButton{nullptr};
    muxc::TextBox dataDirectoryPath{nullptr};
    muxc::Button openDataDirectoryButton{nullptr};
    muxc::TextBlock noLayoutBackups{nullptr};
    muxc::ListView layoutList{nullptr};

    SettingsCard fullBackupCard;
    muxc::Button createFullBackupButton{nullptr};
    muxc::Button importFullBackupButton{nullptr};
    muxc::TextBox fullBackupDirectoryPath{nullptr};
    muxc::Button openFullBackupDirectoryButton{nullptr};
    muxc::TextBlock noFullBackups{nullptr};
    muxc::ListView fullBackupList{nullptr};

    SettingsCard migrationCard;
    muxc::Button migrateButton{nullptr};

    std::vector<LayoutBackupEntry> layoutEntries;
    std::vector<FullDataBackupEntry> fullEntries;
    std::vector<LayoutRow> layoutRows;
    std::vector<FullBackupRow> fullRows;
    BackupDataOperationState operation;
    std::optional<BackupDataNotice> notice;
    std::wstring dataDirectory;
    std::wstring fullBackupDirectory;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool hasSnapshot = false;
    bool active = false;
    bool closed = false;

    winrt::event_token createLayoutToken{};
    winrt::event_token openDataDirectoryToken{};
    winrt::event_token createFullBackupToken{};
    winrt::event_token importFullBackupToken{};
    winrt::event_token openFullBackupDirectoryToken{};
    winrt::event_token migrateToken{};
    winrt::event_token cancelToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback) const
    {
        if (localize)
        {
            std::wstring translated = localize(key);
            const std::wstring untranslated(key.begin(), key.end());
            if (!translated.empty() && translated != untranslated)
                return translated;
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] bool CanInteract() const noexcept
    {
        return !closed && active && hasSnapshot && !operation.running;
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        infoBar = muxc::InfoBar{};
        infoBar.IsClosable(true);
        infoBar.IsOpen(false);
        root.Children().Append(infoBar);

        progressCard = muxc::Border{};
        if (cardStyle)
            progressCard.Style(cardStyle);
        progressContent = muxc::StackPanel{};
        progressContent.Spacing(10.0);
        progressRing = muxc::ProgressRing{};
        progressRing.Width(32.0);
        progressRing.Height(32.0);
        progressRing.Minimum(0.0);
        progressRing.Maximum(1.0);
        progressMessage = muxc::TextBlock{};
        progressMessage.TextWrapping(mux::TextWrapping::Wrap);
        cancelButton = NewButton();
        progressContent.Children().Append(progressRing);
        progressContent.Children().Append(progressMessage);
        progressContent.Children().Append(cancelButton);
        progressCard.Child(progressContent);
        root.Children().Append(progressCard);

        InitializeCard(layoutCard, cardStyle, root);
        muxc::Grid createLayoutRow = NewTwoColumnGrid();
        layoutName = muxc::TextBox{};
        layoutName.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        layoutName.UseSystemFocusVisuals(true);
        createLayoutButton = NewButton();
        createLayoutButton.Margin({12.0, 0.0, 0.0, 0.0});
        muxc::Grid::SetColumn(createLayoutButton, 1);
        createLayoutRow.Children().Append(layoutName);
        createLayoutRow.Children().Append(createLayoutButton);
        dataDirectoryPath = muxc::TextBox{};
        dataDirectoryPath.IsReadOnly(true);
        dataDirectoryPath.TextWrapping(mux::TextWrapping::Wrap);
        dataDirectoryPath.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        openDataDirectoryButton = NewButton();
        noLayoutBackups = NewEmptyMessage();
        layoutList = muxc::ListView{};
        layoutList.SelectionMode(muxc::ListViewSelectionMode::None);
        layoutList.IsItemClickEnabled(false);
        layoutList.MaxHeight(280.0);
        layoutCard.content.Children().Append(createLayoutRow);
        layoutCard.content.Children().Append(dataDirectoryPath);
        layoutCard.content.Children().Append(openDataDirectoryButton);
        layoutCard.content.Children().Append(noLayoutBackups);
        layoutCard.content.Children().Append(layoutList);

        InitializeCard(fullBackupCard, cardStyle, root);
        createFullBackupButton = NewButton();
        importFullBackupButton = NewButton();
        fullBackupDirectoryPath = muxc::TextBox{};
        fullBackupDirectoryPath.IsReadOnly(true);
        fullBackupDirectoryPath.TextWrapping(mux::TextWrapping::Wrap);
        fullBackupDirectoryPath.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        openFullBackupDirectoryButton = NewButton();
        noFullBackups = NewEmptyMessage();
        fullBackupList = muxc::ListView{};
        fullBackupList.SelectionMode(muxc::ListViewSelectionMode::None);
        fullBackupList.IsItemClickEnabled(false);
        fullBackupList.MaxHeight(360.0);
        fullBackupCard.content.Children().Append(createFullBackupButton);
        fullBackupCard.content.Children().Append(importFullBackupButton);
        fullBackupCard.content.Children().Append(fullBackupDirectoryPath);
        fullBackupCard.content.Children().Append(
            openFullBackupDirectoryButton);
        fullBackupCard.content.Children().Append(noFullBackups);
        fullBackupCard.content.Children().Append(fullBackupList);

        InitializeCard(migrationCard, cardStyle, root);
        migrateButton = NewButton();
        migrationCard.content.Children().Append(migrateButton);
    }

    void HookEvents()
    {
        createLayoutToken = createLayoutButton.Click(
            [this](const auto&, const auto&) {
                if (!CanInteract())
                    return;
                BackupDataActionRequest request;
                request.command = BackupDataCommand::CreateLayoutBackup;
                request.displayName = layoutName.Text();
                request.completionPolicy =
                    BackupDataCompletionPolicy::RefreshBackupLists;
                Invoke(std::move(request));
            });
        openDataDirectoryToken = openDataDirectoryButton.Click(
            [this](const auto&, const auto&) {
                if (closed || !active || !hasSnapshot)
                    return;
                BackupDataActionRequest request;
                request.command = BackupDataCommand::OpenDataDirectory;
                request.completionPolicy = BackupDataCompletionPolicy::None;
                Invoke(std::move(request), true);
            });
        createFullBackupToken = createFullBackupButton.Click(
            [this](const auto&, const auto&) {
                if (!CanInteract())
                    return;
                BackupDataActionRequest request;
                request.command = BackupDataCommand::CreateFullBackup;
                request.completionPolicy =
                    BackupDataCompletionPolicy::RefreshBackupLists;
                Invoke(std::move(request));
            });
        importFullBackupToken = importFullBackupButton.Click(
            [this](const auto&, const auto&) {
                if (!CanInteract())
                    return;
                BackupDataPickerRequest picker;
                picker.kind =
                    BackupDataPickerKind::ImportFullBackupArchive;
                BackupDataActionRequest action;
                action.command =
                    BackupDataCommand::ImportAndRestoreFullBackup;
                action.completionPolicy = BackupDataCompletionPolicy::
                    ClearDirtyThenRestartApplication;
                PickThenInvoke(std::move(picker), std::move(action),
                    BackupDataConfirmationKind::ImportAndRestoreFullBackup);
            });
        openFullBackupDirectoryToken =
            openFullBackupDirectoryButton.Click(
                [this](const auto&, const auto&) {
                    if (closed || !active || !hasSnapshot)
                        return;
                    BackupDataActionRequest request;
                    request.command =
                        BackupDataCommand::OpenFullBackupDirectory;
                    request.completionPolicy =
                        BackupDataCompletionPolicy::None;
                    Invoke(std::move(request), true);
                });
        migrateToken = migrateButton.Click(
            [this](const auto&, const auto&) {
                if (!CanInteract())
                    return;
                BackupDataPickerRequest picker;
                picker.kind =
                    BackupDataPickerKind::MigrationSourceDirectory;
                BackupDataActionRequest action;
                action.command = BackupDataCommand::MigrateData;
                action.completionPolicy = BackupDataCompletionPolicy::
                    ClearDirtyThenRestartApplication;
                PickThenInvoke(std::move(picker), std::move(action),
                    BackupDataConfirmationKind::MigrateData);
            });
        cancelToken = cancelButton.Click(
            [this](const auto&, const auto&) {
                if (closed || !active || !hasSnapshot ||
                    !operation.running || !operation.cancellable ||
                    operation.requestId == 0 || !actions.cancel)
                {
                    return;
                }
                actions.cancel(
                    generation, revision, operation.requestId);
            });
    }

    void Invoke(
        BackupDataActionRequest request,
        bool allowWhileBusy = false)
    {
        if (closed || !active || !hasSnapshot || !actions.invoke ||
            (!allowWhileBusy && operation.running))
        {
            return;
        }
        actions.invoke(generation, revision, std::move(request));
    }

    void ConfirmThenInvoke(
        BackupDataConfirmationKind kind,
        BackupDataActionRequest action)
    {
        if (!CanInteract() || !actions.confirm || !actions.invoke)
            return;

        BackupDataConfirmationRequest confirmation;
        confirmation.kind = kind;
        confirmation.subjectId = action.subjectId;
        confirmation.subjectLabel = action.displayName;
        confirmation.completionPolicy = action.completionPolicy;

        const auto expectedGeneration = generation;
        const auto expectedRevision = revision;
        const auto gate = asyncGate;
        auto invoke = actions.invoke;
        actions.confirm(expectedGeneration, expectedRevision,
            std::move(confirmation),
            [gate, expectedGeneration, expectedRevision,
             invoke = std::move(invoke),
             action = std::move(action)](bool confirmed) mutable {
                if (!confirmed ||
                    !IsCallbackCurrent(gate, expectedGeneration,
                        expectedRevision) || !invoke)
                {
                    return;
                }
                invoke(expectedGeneration, expectedRevision,
                    std::move(action));
            });
    }

    void PickThenInvoke(
        BackupDataPickerRequest picker,
        BackupDataActionRequest action,
        std::optional<BackupDataConfirmationKind> confirmationKind)
    {
        if (!CanInteract() || !actions.pickPath || !actions.invoke ||
            (confirmationKind && !actions.confirm))
        {
            return;
        }

        const auto expectedGeneration = generation;
        const auto expectedRevision = revision;
        const auto gate = asyncGate;
        auto invoke = actions.invoke;
        auto confirm = actions.confirm;
        actions.pickPath(expectedGeneration, expectedRevision,
            std::move(picker),
            [gate, expectedGeneration, expectedRevision,
             confirmationKind, invoke = std::move(invoke),
             confirm = std::move(confirm),
             action = std::move(action)](
                std::optional<std::filesystem::path> selected) mutable {
                if (!selected ||
                    !IsCallbackCurrent(gate, expectedGeneration,
                        expectedRevision) || !invoke)
                {
                    return;
                }
                action.selectedPath = std::move(*selected);
                if (!confirmationKind)
                {
                    invoke(expectedGeneration, expectedRevision,
                        std::move(action));
                    return;
                }
                if (!confirm)
                    return;

                BackupDataConfirmationRequest confirmation;
                confirmation.kind = *confirmationKind;
                confirmation.subjectId = action.subjectId;
                confirmation.subjectLabel = action.displayName;
                confirmation.completionPolicy = action.completionPolicy;
                confirm(expectedGeneration, expectedRevision,
                    std::move(confirmation),
                    [gate, expectedGeneration, expectedRevision,
                     invoke = std::move(invoke),
                     action = std::move(action)](bool accepted) mutable {
                        if (!accepted ||
                            !IsCallbackCurrent(gate,
                                expectedGeneration, expectedRevision) ||
                            !invoke)
                        {
                            return;
                        }
                        invoke(expectedGeneration, expectedRevision,
                            std::move(action));
                    });
            });
    }

    void BuildLayoutRows()
    {
        ClearLayoutRows();
        layoutRows.reserve(layoutEntries.size());
        for (const auto& entry : layoutEntries)
        {
            LayoutRow row;
            row.entry = entry;
            row.item = muxc::ListViewItem{};
            row.item.HorizontalContentAlignment(
                mux::HorizontalAlignment::Stretch);
            row.item.IsTabStop(false);

            muxc::Grid grid = NewTwoColumnGrid();
            muxc::StackPanel text{};
            text.Spacing(3.0);
            row.title = muxc::TextBlock{};
            row.title.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            row.title.TextWrapping(mux::TextWrapping::Wrap);
            row.metadata = muxc::TextBlock{};
            row.metadata.Opacity(0.66);
            row.metadata.TextWrapping(mux::TextWrapping::Wrap);
            text.Children().Append(row.title);
            text.Children().Append(row.metadata);

            muxc::StackPanel buttons{};
            buttons.Orientation(muxc::Orientation::Horizontal);
            buttons.Spacing(8.0);
            buttons.Margin({12.0, 0.0, 0.0, 0.0});
            row.restore = NewButton();
            row.remove = NewButton();
            buttons.Children().Append(row.restore);
            buttons.Children().Append(row.remove);
            muxc::Grid::SetColumn(buttons, 1);
            grid.Children().Append(text);
            grid.Children().Append(buttons);
            row.item.Content(grid);

            const std::wstring id = row.entry.id;
            const std::wstring label = row.entry.displayName;
            row.restoreToken = row.restore.Click(
                [this, id, label](const auto&, const auto&) {
                    if (!CanInteract())
                        return;
                    BackupDataActionRequest action;
                    action.command =
                        BackupDataCommand::RestoreLayoutBackup;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy =
                        BackupDataCompletionPolicy::ReloadDesktopLayout;
                    ConfirmThenInvoke(
                        BackupDataConfirmationKind::RestoreLayoutBackup,
                        std::move(action));
                });
            row.removeToken = row.remove.Click(
                [this, id, label](const auto&, const auto&) {
                    if (!CanInteract())
                        return;
                    BackupDataActionRequest action;
                    action.command =
                        BackupDataCommand::DeleteLayoutBackup;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy =
                        BackupDataCompletionPolicy::RefreshBackupLists;
                    ConfirmThenInvoke(
                        BackupDataConfirmationKind::DeleteLayoutBackup,
                        std::move(action));
                });
            layoutList.Items().Append(row.item);
            layoutRows.push_back(std::move(row));
        }
        RefreshLayoutRows();
    }

    void BuildFullBackupRows()
    {
        ClearFullRows();
        fullRows.reserve(fullEntries.size());
        for (const auto& entry : fullEntries)
        {
            FullBackupRow row;
            row.entry = entry;
            row.item = muxc::ListViewItem{};
            row.item.HorizontalContentAlignment(
                mux::HorizontalAlignment::Stretch);
            row.item.IsTabStop(false);

            muxc::StackPanel container{};
            container.Spacing(8.0);
            row.title = muxc::TextBlock{};
            row.title.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            row.title.TextWrapping(mux::TextWrapping::Wrap);
            row.metadata = muxc::TextBlock{};
            row.metadata.Opacity(0.66);
            row.metadata.TextWrapping(mux::TextWrapping::Wrap);
            muxc::StackPanel buttons{};
            // A vertical action stack remains usable when the settings
            // content column narrows under high DPI or a snapped window.
            buttons.Orientation(muxc::Orientation::Vertical);
            buttons.Spacing(8.0);
            row.restore = NewButton();
            row.exportArchive = NewButton();
            row.open = NewButton();
            row.remove = NewButton();
            buttons.Children().Append(row.restore);
            buttons.Children().Append(row.exportArchive);
            buttons.Children().Append(row.open);
            buttons.Children().Append(row.remove);
            container.Children().Append(row.title);
            container.Children().Append(row.metadata);
            container.Children().Append(buttons);
            row.item.Content(container);

            const std::wstring id = row.entry.id;
            const std::wstring label = row.entry.displayName;
            row.restoreToken = row.restore.Click(
                [this, id, label](const auto&, const auto&) {
                    if (!CanInteract())
                        return;
                    BackupDataActionRequest action;
                    action.command = BackupDataCommand::RestoreFullBackup;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy = BackupDataCompletionPolicy::
                        ClearDirtyThenRestartApplication;
                    ConfirmThenInvoke(
                        BackupDataConfirmationKind::RestoreFullBackup,
                        std::move(action));
                });
            row.exportToken = row.exportArchive.Click(
                [this, id, label](const auto&, const auto&) {
                    if (!CanInteract())
                        return;
                    BackupDataPickerRequest picker;
                    picker.kind =
                        BackupDataPickerKind::ExportFullBackupArchive;
                    picker.subjectId = id;
                    picker.suggestedFileName =
                        L"SnowDesktop-" + id + L".snowbackup";
                    BackupDataActionRequest action;
                    action.command = BackupDataCommand::ExportFullBackup;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy =
                        BackupDataCompletionPolicy::ShowResultOnly;
                    PickThenInvoke(std::move(picker), std::move(action),
                        std::nullopt);
                });
            row.openToken = row.open.Click(
                [this, id, label](const auto&, const auto&) {
                    if (closed || !active || !hasSnapshot)
                        return;
                    BackupDataActionRequest action;
                    action.command = BackupDataCommand::OpenFullBackupItem;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy =
                        BackupDataCompletionPolicy::None;
                    Invoke(std::move(action), true);
                });
            row.removeToken = row.remove.Click(
                [this, id, label](const auto&, const auto&) {
                    if (!CanInteract())
                        return;
                    BackupDataActionRequest action;
                    action.command = BackupDataCommand::DeleteFullBackup;
                    action.subjectId = id;
                    action.displayName = label;
                    action.completionPolicy =
                        BackupDataCompletionPolicy::RefreshBackupLists;
                    ConfirmThenInvoke(
                        BackupDataConfirmationKind::DeleteFullBackup,
                        std::move(action));
                });
            fullBackupList.Items().Append(row.item);
            fullRows.push_back(std::move(row));
        }
        RefreshFullRows();
    }

    void RefreshLayoutRows()
    {
        const std::wstring restoreText =
            L("app.settings.restore", L"Restore");
        const std::wstring deleteText =
            L("app.settings.delete", L"Delete");
        for (auto& row : layoutRows)
        {
            row.title.Text(row.entry.displayName);
            row.metadata.Text(row.entry.createdAt);
            row.metadata.Visibility(row.entry.createdAt.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
            row.restore.Content(winrt::box_value(restoreText));
            row.remove.Content(winrt::box_value(deleteText));
            muxa::AutomationProperties::SetName(row.item,
                row.entry.displayName);
            muxa::AutomationProperties::SetName(row.restore,
                restoreText + L" " + row.entry.displayName);
            muxa::AutomationProperties::SetName(row.remove,
                deleteText + L" " + row.entry.displayName);
            muxa::AutomationProperties::SetHelpText(row.restore,
                layoutCard.description.Text());
            muxa::AutomationProperties::SetHelpText(row.remove,
                layoutCard.description.Text());
        }
    }

    void RefreshFullRows()
    {
        const std::wstring restoreText =
            L("app.settings.restore", L"Restore");
        const std::wstring exportText =
            L("app.settings.export_backup", L"Export");
        const std::wstring openText = L("app.settings.open", L"Open");
        const std::wstring deleteText =
            L("app.settings.delete", L"Delete");
        for (auto& row : fullRows)
        {
            row.title.Text(row.entry.displayName);
            row.metadata.Text(row.entry.createdAt);
            row.metadata.Visibility(row.entry.createdAt.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
            row.restore.Content(winrt::box_value(restoreText));
            row.exportArchive.Content(winrt::box_value(exportText));
            row.open.Content(winrt::box_value(openText));
            row.remove.Content(winrt::box_value(deleteText));
            muxa::AutomationProperties::SetName(row.item,
                row.entry.displayName);
            for (const auto& [button, text] :
                 std::initializer_list<std::pair<muxc::Button, std::wstring>>{
                     {row.restore, restoreText},
                     {row.exportArchive, exportText},
                     {row.open, openText},
                     {row.remove, deleteText}})
            {
                muxa::AutomationProperties::SetName(button,
                    text + L" " + row.entry.displayName);
                muxa::AutomationProperties::SetHelpText(button,
                    fullBackupCard.description.Text());
            }
        }
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        layoutCard.title.Text(
            L("app.settings.layout_backups", L"Layout backups"));
        layoutCard.description.Text(L(
            "settings.backup.layout.description",
            L"Create and restore desktop-layout backups."));
        layoutName.PlaceholderText(L(
            "app.settings.backup_name_hint", L"Backup name (optional)"));
        createLayoutButton.Content(winrt::box_value(L(
            "app.settings.save_backup", L"Save backup")));
        openDataDirectoryButton.Content(winrt::box_value(L(
            "app.settings.open_data_folder", L"Open data folder")));
        noLayoutBackups.Text(
            L("app.settings.no_backups", L"No backups yet"));

        fullBackupCard.title.Text(L(
            "app.settings.full_data_backups", L"Complete data backups"));
        fullBackupCard.description.Text(L(
            "app.settings.full_data_backup_description",
            L"Back up layouts, settings, widgets, and widget storage."));
        createFullBackupButton.Content(winrt::box_value(L(
            "app.settings.create_full_backup", L"Create complete backup")));
        importFullBackupButton.Content(winrt::box_value(L(
            "app.settings.restore_from_backup_file",
            L"Restore from backup file…")));
        openFullBackupDirectoryButton.Content(winrt::box_value(L(
            "app.settings.open_full_backup_folder",
            L"Open complete backup folder")));
        noFullBackups.Text(L(
            "app.settings.no_full_data_backups",
            L"No complete or pre-migration backups"));

        migrationCard.title.Text(
            L("app.settings.data_migration", L"Data migration"));
        migrationCard.description.Text(L(
            "app.settings.data_migration_description",
            L"Move complete data from another SnowDesktop copy."));
        migrateButton.Content(winrt::box_value(L(
            "app.settings.migrate_all_data", L"Move in complete data…")));
        cancelButton.Content(winrt::box_value(
            L("app.settings.cancel", L"Cancel")));

        SetCardAutomation(layoutCard);
        SetCardAutomation(fullBackupCard);
        SetCardAutomation(migrationCard);
        SetButtonAutomation(createLayoutButton,
            L("app.settings.save_backup", L"Save backup"),
            layoutCard.description.Text());
        SetButtonAutomation(openDataDirectoryButton,
            L("app.settings.open_data_folder", L"Open data folder"),
            L("settings.backup.directory.description",
                L"Open the current SnowDesktop data directory."));
        SetButtonAutomation(createFullBackupButton,
            L("app.settings.create_full_backup", L"Create complete backup"),
            fullBackupCard.description.Text());
        SetButtonAutomation(importFullBackupButton,
            L("app.settings.restore_from_backup_file",
                L"Restore from backup file…"),
            fullBackupCard.description.Text());
        SetButtonAutomation(openFullBackupDirectoryButton,
            L("app.settings.open_full_backup_folder",
                L"Open complete backup folder"),
            fullBackupCard.description.Text());
        SetButtonAutomation(migrateButton,
            L("app.settings.migrate_all_data", L"Move in complete data…"),
            migrationCard.description.Text());
        SetButtonAutomation(cancelButton,
            L("app.settings.cancel", L"Cancel"), progressMessage.Text());
        muxa::AutomationProperties::SetName(layoutName,
            L("app.settings.backup_name_hint", L"Backup name (optional)"));
        muxa::AutomationProperties::SetHelpText(layoutName,
            layoutCard.description.Text());
        muxa::AutomationProperties::SetName(dataDirectoryPath,
            L("settings.backup.directory", L"Data directory"));
        muxa::AutomationProperties::SetHelpText(dataDirectoryPath,
            L("settings.backup.directory.description",
                L"Open the current SnowDesktop data directory."));
        muxa::AutomationProperties::SetName(fullBackupDirectoryPath,
            L("app.settings.open_full_backup_folder",
                L"Open complete backup folder"));
        muxa::AutomationProperties::SetHelpText(fullBackupDirectoryPath,
            fullBackupCard.description.Text());

        RefreshLayoutRows();
        RefreshFullRows();
        RenderState();
    }

    static void SetCardAutomation(const SettingsCard& card)
    {
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
        muxa::AutomationProperties::SetHelpText(
            card.root, card.description.Text());
    }

    static void SetButtonAutomation(
        const muxc::Button& button,
        const winrt::param::hstring& name,
        const winrt::param::hstring& help)
    {
        muxa::AutomationProperties::SetName(button, name);
        muxa::AutomationProperties::SetHelpText(button, help);
    }

    void RenderState()
    {
        if (closed)
            return;
        const bool running = operation.running;
        dataDirectoryPath.Text(dataDirectory);
        dataDirectoryPath.Visibility(dataDirectory.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        fullBackupDirectoryPath.Text(fullBackupDirectory);
        fullBackupDirectoryPath.Visibility(fullBackupDirectory.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        progressCard.Visibility(running
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        progressRing.IsActive(running);
        progressRing.IsIndeterminate(operation.indeterminate);
        if (!operation.indeterminate)
            progressRing.Value(std::clamp(operation.progress, 0.0, 1.0));
        progressMessage.Text(operation.message.empty()
            ? L("settings.home.backup", L"Backup status")
            : operation.message);
        cancelButton.Visibility(operation.cancellable
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        cancelButton.IsEnabled(running && operation.cancellable &&
            operation.requestId != 0);
        muxa::AutomationProperties::SetName(progressRing,
            progressMessage.Text());
        muxa::AutomationProperties::SetName(cancelButton,
            L("app.settings.cancel", L"Cancel"));
        muxa::AutomationProperties::SetHelpText(cancelButton,
            progressMessage.Text());

        if (notice && !notice->message.empty())
        {
            infoBar.Severity(ToInfoBarSeverity(notice->severity));
            infoBar.Title(notice->title.empty()
                ? L("settings.nav.backup", L"Backup & data")
                : notice->title);
            infoBar.Message(notice->message);
            infoBar.IsOpen(true);
            muxa::AutomationProperties::SetName(infoBar,
                infoBar.Title());
            muxa::AutomationProperties::SetHelpText(infoBar,
                infoBar.Message());
        }
        else
        {
            infoBar.IsOpen(false);
        }

        const bool mutationsEnabled = active && hasSnapshot && !running;
        layoutName.IsEnabled(mutationsEnabled);
        createLayoutButton.IsEnabled(mutationsEnabled);
        layoutList.IsEnabled(mutationsEnabled);
        createFullBackupButton.IsEnabled(mutationsEnabled);
        importFullBackupButton.IsEnabled(mutationsEnabled);
        fullBackupList.IsEnabled(mutationsEnabled);
        migrateButton.IsEnabled(mutationsEnabled);
        openDataDirectoryButton.IsEnabled(active && hasSnapshot);
        openFullBackupDirectoryButton.IsEnabled(active && hasSnapshot);
        noLayoutBackups.Visibility(layoutEntries.empty()
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        layoutList.Visibility(layoutEntries.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
        noFullBackups.Visibility(fullEntries.empty()
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        fullBackupList.Visibility(fullEntries.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
    }

    [[nodiscard]] bool ApplySnapshot(
        const BackupDataPageSnapshot& snapshot)
    {
        if (closed || !snapshot.initialized)
            return false;
        if (hasSnapshot)
        {
            if (snapshot.generation < generation)
                return false;
            if (snapshot.generation == generation &&
                snapshot.revision <= revision)
            {
                return false;
            }
        }

        const bool layoutListChanged = !hasSnapshot ||
            layoutEntries != snapshot.layoutBackups;
        const bool fullListChanged = !hasSnapshot ||
            fullEntries != snapshot.fullBackups;
        generation = snapshot.generation;
        revision = snapshot.revision;
        if (layoutListChanged)
            layoutEntries = snapshot.layoutBackups;
        if (fullListChanged)
            fullEntries = snapshot.fullBackups;
        dataDirectory = snapshot.dataDirectory;
        fullBackupDirectory = snapshot.fullBackupDirectory;
        operation = snapshot.operation;
        notice = snapshot.notice;
        hasSnapshot = true;
        asyncGate->generation.store(generation, std::memory_order_release);
        asyncGate->revision.store(revision, std::memory_order_release);
        if (layoutListChanged)
            BuildLayoutRows();
        if (fullListChanged)
            BuildFullBackupRows();
        RenderState();
        return true;
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        if (id == "backup.layout" || id == "backup.layout.name")
            return layoutName;
        if (id == "backup.full")
            return createFullBackupButton;
        if (id == "backup.directory")
            return openDataDirectoryButton;
        if (id == "backup.migration")
            return migrateButton;
        return nullptr;
    }

    void ClearLayoutRows() noexcept
    {
        for (auto& row : layoutRows)
        {
            try
            {
                row.restore.Click(row.restoreToken);
                row.remove.Click(row.removeToken);
            }
            catch (...)
            {
            }
        }
        layoutRows.clear();
        try
        {
            layoutList.Items().Clear();
        }
        catch (...)
        {
        }
    }

    void ClearFullRows() noexcept
    {
        for (auto& row : fullRows)
        {
            try
            {
                row.restore.Click(row.restoreToken);
                row.exportArchive.Click(row.exportToken);
                row.open.Click(row.openToken);
                row.remove.Click(row.removeToken);
            }
            catch (...)
            {
            }
        }
        fullRows.clear();
        try
        {
            fullBackupList.Items().Clear();
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        active = false;
        closed = true;
        asyncGate->active.store(false, std::memory_order_release);
        asyncGate->alive.store(false, std::memory_order_release);
        ClearLayoutRows();
        ClearFullRows();
        try
        {
            createLayoutButton.Click(createLayoutToken);
            openDataDirectoryButton.Click(openDataDirectoryToken);
            createFullBackupButton.Click(createFullBackupToken);
            importFullBackupButton.Click(importFullBackupToken);
            openFullBackupDirectoryButton.Click(
                openFullBackupDirectoryToken);
            migrateButton.Click(migrateToken);
            cancelButton.Click(cancelToken);
            progressRing.IsActive(false);
        }
        catch (...)
        {
        }
        actions = {};
        localize = {};
    }
};

BackupDataPagePresenter::BackupDataPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

BackupDataPagePresenter::~BackupDataPagePresenter()
{
    Close();
}

void BackupDataPagePresenter::SetActions(BackupDataPageActions actions)
{
    if (impl_ && !impl_->closed)
        impl_->actions = std::move(actions);
}

mux::UIElement BackupDataPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

bool BackupDataPagePresenter::ApplySnapshot(
    const BackupDataPageSnapshot& snapshot)
{
    return impl_ && impl_->ApplySnapshot(snapshot);
}

void BackupDataPagePresenter::RefreshLocalizedText()
{
    if (impl_)
        impl_->RefreshLocalizedText();
}

void BackupDataPagePresenter::Activate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->active = true;
    impl_->asyncGate->active.store(true, std::memory_order_release);
    impl_->RenderState();
}

void BackupDataPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->active = false;
    impl_->asyncGate->active.store(false, std::memory_order_release);
    impl_->RenderState();
}

mux::FrameworkElement BackupDataPagePresenter::FocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(focusId) : nullptr;
}

std::uint64_t BackupDataPagePresenter::Generation() const noexcept
{
    return impl_ ? impl_->generation : 0;
}

std::uint64_t BackupDataPagePresenter::Revision() const noexcept
{
    return impl_ ? impl_->revision : 0;
}

void BackupDataPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui
