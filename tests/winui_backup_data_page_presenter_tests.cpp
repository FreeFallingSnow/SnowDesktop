#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream content;
    content << input.rdbuf();
    std::string source = content.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
}

void TestPresenterContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/backup_data_page_presenter.h");
    const std::string source = ReadText(
        repository / "src/winui/backup_data_page_presenter.cpp");

    Check(!header.empty() && !source.empty(),
        "Backup/data presenter sources are readable");

    for (const char* model : {
             "LayoutBackupEntry", "FullDataBackupEntry",
             "BackupDataPageSnapshot", "BackupDataOperationState",
             "replacementPending",
             "BackupDataNotice", "BackupDataActionRequest",
             "BackupDataConfirmationRequest", "BackupDataPickerRequest"})
    {
        Check(header.find(model) != std::string::npos,
            "backup and operation state crosses a typed host boundary");
    }
    Check(header.find("std::uint64_t generation") != std::string::npos &&
            header.find("std::uint64_t revision") != std::string::npos &&
            source.find("snapshot.generation < generation") !=
                std::string::npos &&
            source.find("snapshot.revision <= revision") !=
                std::string::npos &&
            source.find("IsCallbackCurrent") != std::string::npos &&
            source.find("gate->generation.load") != std::string::npos &&
            source.find("gate->revision.load") != std::string::npos,
        "snapshots and picker/dialog callbacks reject stale generations and revisions");

    for (const char* command : {
             "BackupDataCommand::CreateLayoutBackup",
             "BackupDataCommand::RestoreLayoutBackup",
             "BackupDataCommand::DeleteLayoutBackup",
             "BackupDataCommand::CreateFullBackup",
             "BackupDataCommand::ImportAndRestoreFullBackup",
             "BackupDataCommand::ExportFullBackup",
             "BackupDataCommand::RestoreFullBackup",
             "BackupDataCommand::DeleteFullBackup",
             "BackupDataCommand::MigrateData",
             "BackupDataCommand::OpenDataDirectory",
             "BackupDataCommand::OpenFullBackupDirectory",
             "BackupDataCommand::OpenFullBackupItem"})
    {
        Check(source.find(command) != std::string::npos,
            "every backup/data action is represented by a typed command");
    }

    for (const char* danger : {
             "BackupDataConfirmationKind::RestoreLayoutBackup",
             "BackupDataConfirmationKind::DeleteLayoutBackup",
             "BackupDataConfirmationKind::ImportAndRestoreFullBackup",
             "BackupDataConfirmationKind::RestoreFullBackup",
             "BackupDataConfirmationKind::DeleteFullBackup",
             "BackupDataConfirmationKind::MigrateData"})
    {
        Check(source.find(danger) != std::string::npos,
            "dangerous action is assigned a host ContentDialog purpose");
    }
    Check(source.find("ConfirmThenInvoke") != std::string::npos &&
            source.find("actions.confirm") != std::string::npos &&
            source.find("MessageBox") == std::string::npos &&
            source.find("ContentDialog{}") == std::string::npos,
        "dangerous work cannot bypass the host-owned confirmation gate");

    Check(source.find("BackupDataPickerKind::ImportFullBackupArchive") !=
                std::string::npos &&
            source.find("BackupDataPickerKind::ExportFullBackupArchive") !=
                std::string::npos &&
            source.find("BackupDataPickerKind::MigrationSourceDirectory") !=
                std::string::npos &&
            source.find("actions.pickPath") != std::string::npos &&
            source.find("IFileOpenDialog") == std::string::npos &&
            source.find("IFileSaveDialog") == std::string::npos,
        "file and folder selection is delegated to an HWND-owned host picker");

    Check(header.find("ClearDirtyThenRestartApplication") !=
                std::string::npos &&
            source.find("ImportAndRestoreFullBackup;\n"
                        "                action.completionPolicy = "
                        "BackupDataCompletionPolicy::\n"
                        "                    ClearDirtyThenRestartApplication") !=
                std::string::npos &&
            source.find("action.command = BackupDataCommand::MigrateData;") !=
                std::string::npos &&
            source.find("action.command = BackupDataCommand::RestoreFullBackup;") !=
                std::string::npos &&
            source.find("ClearDirtyThenRestartApplication") !=
                std::string::npos,
        "full restore/import/migration explicitly clear dirty settings before restart");
    Check(source.find("BackupDataCompletionPolicy::ReloadDesktopLayout") !=
                std::string::npos &&
            source.find("BackupDataCompletionPolicy::RefreshBackupLists") !=
                std::string::npos,
        "layout restoration and mutations have non-restart completion policies");

    for (const char* control : {
             "muxc::TextBox", "muxc::ListView", "muxc::Button",
             "muxc::InfoBar", "muxc::ProgressRing",
             "muxc::CommandBar", "muxc::AppBarButton"})
    {
        Check(source.find(control) != std::string::npos,
            "backup/data page uses real native WinUI controls");
    }
    Check(source.find("progressRing.IsActive(running)") !=
                std::string::npos &&
            source.find("progressRing.IsIndeterminate") !=
                std::string::npos &&
            source.find("actions.cancel(") != std::string::npos &&
            source.find("operation.requestId") != std::string::npos &&
            source.find("infoBar.Severity") != std::string::npos &&
            source.find("infoBar.IsOpen(true)") != std::string::npos,
        "long tasks expose progress, cooperative cancellation and InfoBar feedback");

    Check(source.find("RefreshLocalizedText()") != std::string::npos &&
            source.find("app.settings.layout_backups") !=
                std::string::npos &&
            source.find("app.settings.full_data_backups") !=
                std::string::npos &&
            source.find("app.settings.data_migration") !=
                std::string::npos &&
            source.find("x:Uid") == std::string::npos,
        "all static page text uses the dynamic JSON localizer");
    Check(source.find("settings_presenter_controls.h") !=
                std::string::npos &&
            source.find("controls::SettingRow layoutCreateRow") !=
                std::string::npos &&
            source.find("controls::SettingRow fullBackupActionsRow") !=
                std::string::npos &&
            source.find("controls::SettingRow migrationActionRow") !=
                std::string::npos &&
            source.find("row.commandBar.Content(row.title)") !=
                std::string::npos &&
            source.find("row.item.Content(row.commandBar)") !=
                std::string::npos &&
            source.find("migrationActionRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find(
              "button.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "button.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos,
        "backup rows keep native focus visuals and include titles in the expanded command surface");
    Check(source.find("app.settings.save_current_layout") !=
                std::string::npos &&
            source.find("layoutActions.Children().Append(layoutName)") !=
                std::string::npos &&
            source.find("layoutActions.Children().Append(layoutActionBar)") !=
                std::string::npos &&
            source.find(
              "layoutActionBar.PrimaryCommands().Append(createLayoutButton)") !=
                std::string::npos &&
            source.find(
              "layoutActionBar.SecondaryCommands().Append(") !=
                std::string::npos &&
            source.find("openDataDirectoryButton);") !=
                std::string::npos &&
            source.find("layoutList.MaxHeight(132.0)") !=
                std::string::npos &&
            source.find("layoutBackupSaveRunning") != std::string::npos &&
            source.find("layoutName.Text(L\"\")") != std::string::npos,
        "layout backup keeps the name field and primary save action while moving folder access to overflow");
    Check(source.find("fullBackupActionBar = NewCommandBar()") !=
                std::string::npos &&
            source.find(
              "fullBackupActionBar.PrimaryCommands().Append(") !=
                std::string::npos &&
            source.find("muxc::Grid fullActions") ==
                std::string::npos &&
            source.find(
              "createFullBackupButton);") != std::string::npos &&
            source.find(
              "importFullBackupButton);") != std::string::npos &&
            source.find(
              "fullBackupActionBar.SecondaryCommands().Append(") !=
                std::string::npos &&
            source.find("openFullBackupDirectoryButton);") !=
                std::string::npos &&
            source.find("fullBackupList.MaxHeight(172.0)") !=
                std::string::npos &&
            source.find("app.settings.full_backup_unknown_time") !=
                std::string::npos &&
            source.find("app.settings.migration_backup_item") !=
                std::string::npos &&
            source.find("app.settings.full_backup_item") !=
                std::string::npos &&
            source.find("FormatBackupSize") != std::string::npos,
        "complete-backup keeps create/restore primary and folder access in overflow without losing list metadata");
    Check(source.find("commandBar.DefaultLabelPosition(") !=
                std::string::npos &&
            source.find(
              "muxc::CommandBarDefaultLabelPosition::Right") !=
                std::string::npos &&
            source.find("commandBar.IsDynamicOverflowEnabled(true)") !=
                std::string::npos &&
            source.find(
              "row.commandBar.PrimaryCommands().Append(row.restore)") !=
                std::string::npos &&
            source.find(
              "row.commandBar.SecondaryCommands().Append(row.exportArchive)") !=
                std::string::npos &&
            source.find(
              "row.commandBar.SecondaryCommands().Append(row.open)") !=
                std::string::npos &&
            source.find(
              "row.commandBar.SecondaryCommands().Append(row.remove)") !=
                std::string::npos &&
            source.find("muxc::AppBarSeparator{}") !=
                std::string::npos &&
            source.find("row.metadata") == std::string::npos &&
            source.find("dataDirectoryPath") == std::string::npos &&
            source.find("fullBackupDirectoryPath") == std::string::npos,
        "native dynamic overflow keeps one row-level restore action visible and groups infrequent or destructive actions in a separated menu");
    Check(source.find("AutomationProperties::SetName") !=
                std::string::npos &&
            source.find("AutomationProperties::SetHelpText") !=
                std::string::npos &&
            source.find("UseSystemFocusVisuals(true)") !=
                std::string::npos &&
            source.find("ToolTipService::SetToolTip") !=
                std::string::npos &&
            source.find("backup.layout") != std::string::npos &&
            source.find("backup.full") != std::string::npos &&
            source.find("backup.directory") != std::string::npos &&
            source.find("backup.migration") != std::string::npos,
        "automation, keyboard focus visuals and search focus targets are present");

    Check(source.find("FullDataBackupManager") == std::string::npos &&
            source.find("CopyDataTree") == std::string::npos &&
            source.find("ShellExecute") == std::string::npos &&
            source.find("std::thread") == std::string::npos &&
            source.find("CreateThread") == std::string::npos &&
            source.find("std::filesystem::remove") == std::string::npos,
        "the presenter performs no backup, migration, shell or private worker work");
    Check(source.find("row.restore.Click(row.restoreToken)") !=
                std::string::npos &&
            source.find("row.exportArchive.Click(row.exportToken)") !=
                std::string::npos &&
            source.find("cancelButton.Click(cancelToken)") !=
                std::string::npos &&
            source.find("asyncGate->alive.store(false") !=
                std::string::npos,
        "static and dynamic event handlers and async bridges are closed safely");
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Backup/data presenter contract");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI Backup/data presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI Backup/data presenter checks passed\n";
    return EXIT_SUCCESS;
}
