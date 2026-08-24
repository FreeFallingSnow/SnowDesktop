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
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void TestPresenterAdapterBoundary(const std::string& header,
    const std::string& source)
{
    Check(header.find("SettingsController& controller") !=
                std::string::npos &&
            header.find("BackupDataPageActions Actions()") !=
                std::string::npos &&
            header.find("BackupDataPageSnapshot CurrentSnapshot()") !=
                std::string::npos &&
            header.find("SnapshotChangedCallback") != std::string::npos,
        "backend is a real SettingsController-to-presenter adapter");

    Check(source.find("actions.invoke") != std::string::npos &&
            source.find("actions.confirm") != std::string::npos &&
            source.find("actions.pickPath") != std::string::npos &&
            source.find("actions.cancel") != std::string::npos,
        "every presenter action boundary is implemented");

    Check(source.find("generation == snapshot.generation") !=
                std::string::npos &&
            source.find("revision == snapshot.revision") !=
                std::string::npos &&
            source.find("snapshot.operation.requestId") !=
                std::string::npos &&
            source.find("pendingCompletion->context.requestId") !=
                std::string::npos,
        "generation revision and request identity gate async completion");
}

void TestWorkerAndPickerOwnership(const std::string& header,
    const std::string& source)
{
    Check(header.find("std::function<HWND()> ownerWindow") !=
                std::string::npos &&
            header.find("HWND owner,") != std::string::npos &&
            source.find("options.ownerWindow()") != std::string::npos &&
            source.find("options.pickPath(owner") != std::string::npos,
        "picker receives the live settings owner HWND through injection");

    Check(header.find("postToUi") != std::string::npos &&
            source.find("std::jthread") != std::string::npos &&
            source.find("std::stop_token") != std::string::npos &&
            source.find("stop.stop_requested()") != std::string::npos &&
            source.find("worker.request_stop()") != std::string::npos &&
            source.find("DrainAnyCompletion") != std::string::npos,
        "storage work is marshalled and has cooperative cancellation points");
}

void TestExistingStorageFormatsAreReused(const std::string& source)
{
    for (const char* operation : {
             "manager.Create()",
             "manager.ImportAndQueue(",
             "manager.Export(",
             "manager.QueueRestore(",
             "manager.Delete(",
             "manager.QueueDirectory("})
    {
        Check(source.find(operation) != std::string::npos,
            "complete-data work reuses FullDataBackupManager");
    }

    Check(source.find("GetDataDirectoryPath()") != std::string::npos &&
            source.find("GetDataFilePath(kLayoutFileName)") !=
                std::string::npos &&
            source.find("GetDataSubdirectoryPath(L\"backups\")") !=
                std::string::npos,
        "default locations come from the existing data_paths API");
    Check(source.find("SnowDesktop.layout.json") != std::string::npos &&
            source.find("SnowDesktop.storage.json") != std::string::npos &&
            source.find(".storage.json") != std::string::npos,
        "layout backups retain the historical layout/storage companion pair");
    Check(source.find("backup.json") == std::string::npos &&
            source.find("snowbackup_manifest") == std::string::npos,
        "backend does not define a competing complete-backup format");
}

void TestReplacementNeverFlushesOldMemory(const std::string& header,
    const std::string& source)
{
    const auto completion = source.find("void CompleteQueuedReplacement");
    const auto discard = source.find(
        "PrepareForExternalDataReplacement()", completion);
    const auto restart = source.find(
        "SettingsHostActions::Action::RestartApplication", completion);
    Check(completion != std::string::npos &&
            discard != std::string::npos &&
            restart != std::string::npos && discard < restart,
        "queued restore discards dirty controller state before restart");
    const auto completionEnd = source.find("void Finish(", completion);
    const std::string replacementCompletion =
        completion != std::string::npos && completionEnd != std::string::npos
        ? source.substr(completion, completionEnd - completion)
        : std::string{};
    Check(replacementCompletion.find("FlushPending(") == std::string::npos &&
            replacementCompletion.find("FlushAll(") == std::string::npos,
        "replacement completion never flushes the pre-restore snapshot");
    Check(source.find("completion.result.replacementQueued") !=
                std::string::npos &&
            source.find("CompleteQueuedReplacement(completion)") !=
                std::string::npos &&
            source.find("Drain before marking closed") !=
                std::string::npos,
        "a late cancel or page close cannot abandon an already queued replacement");
    Check(source.find("snapshot.replacementPending = true") !=
                std::string::npos &&
            source.find("snapshot.replacementPending") !=
                std::string::npos,
        "failed restart leaves the backup backend in a terminal state");

    Check(source.find("options.commitLayoutRestore") !=
                std::string::npos &&
            source.find("layoutRestore") !=
                std::string::npos &&
            source.find("layout_storage::ValidateDocument") !=
                std::string::npos &&
            source.find("layout_storage::SaveDocument") ==
                std::string::npos &&
            source.find("atomic_file::WriteAll") ==
                std::string::npos,
        "layout worker validates a payload but never writes live files");
    Check(header.find("LayoutRestorePayload") != std::string::npos &&
            header.find("commitLayoutRestore") != std::string::npos,
        "layout replacement is committed through an application-owned STA seam");
}

void TestApplicationOwnedLayoutCommit(const std::string& application,
    const std::string& compositionRoot)
{
    const auto transaction = application.find(
        "DesktopApp::CommitLayoutRestore");
    const auto flush = application.find(
        "settingsController_->FlushAll()", transaction);
    const auto replace = application.find(
        "layout_storage::SaveDocument", transaction);
    const auto reload = application.find("ReloadItems(true)", transaction);
    const auto synchronizeGeneral = application.find(
        "SynchronizeGeneral", reload);
    const auto synchronizeDesktop = application.find(
        "SynchronizeDesktop", synchronizeGeneral);
    Check(transaction != std::string::npos &&
            flush != std::string::npos &&
            replace != std::string::npos &&
            reload != std::string::npos &&
            synchronizeGeneral != std::string::npos &&
            synchronizeDesktop != std::string::npos &&
            flush < replace && replace < reload &&
            reload < synchronizeGeneral &&
            synchronizeGeneral < synchronizeDesktop,
        "DesktopApp flushes, replaces, reloads and synchronizes in order");
    Check(application.find("previousLayout", transaction) !=
                std::string::npos &&
            application.find("layoutRolledBack", transaction) !=
                std::string::npos &&
            application.find("storageRolledBack", transaction) !=
                std::string::npos,
        "application layout transaction preserves rollback documents");
    Check(compositionRoot.find(
              "backupDataPage.commitLayoutRestore") != std::string::npos &&
            compositionRoot.find("CommitLayoutRestore(std::move(payload))") !=
                std::string::npos,
        "composition root injects the application-owned layout transaction");
}

void TestBackendContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(
        repository / "src/winui/backup_data_page_backend.h");
    const std::string source = ReadText(
        repository / "src/winui/backup_data_page_backend.cpp");
    const std::string application = ReadText(
        repository / "src/app/app_settings_apply.cpp");
    const std::string compositionRoot = ReadText(
        repository / "src/app/app_run.cpp");
    Check(!header.empty() && !source.empty() && !application.empty() &&
            !compositionRoot.empty(),
        "backup/data backend sources are readable");
    if (header.empty() || source.empty() || application.empty() ||
        compositionRoot.empty())
        return;

    TestPresenterAdapterBoundary(header, source);
    TestWorkerAndPickerOwnership(header, source);
    TestExistingStorageFormatsAreReused(source);
    TestReplacementNeverFlushesOldMemory(header, source);
    TestApplicationOwnedLayoutCommit(application, compositionRoot);
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the Backup/data backend contract");
    if (argc == 2)
        TestBackendContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " Backup/data backend check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Backup/data backend checks passed\n";
    return EXIT_SUCCESS;
}
