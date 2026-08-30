#include "../src/winui/backup_operation_control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;

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
    std::string source = text.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
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
            source.find("expectedActivationId == activationId") !=
                std::string::npos &&
            source.find("completion.context.activationId == activationId") !=
                std::string::npos &&
            source.find("snapshot.operation.requestId") !=
                std::string::npos &&
            source.find("inFlightTaskId == completion.context.requestId") !=
                std::string::npos &&
            source.find("pendingCompletion->context.requestId") !=
                std::string::npos,
        "generation activation revision and task identity gate async completion");
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
            source.find("worker.detach()") != std::string::npos &&
            source.find("DrainAnyCompletion") != std::string::npos,
        "storage work is marshalled and has cooperative cancellation points");

    const auto deactivate = source.find("void Deactivate() noexcept");
    const auto close = source.find("void Close() noexcept", deactivate);
    const auto stateEnd = source.find("};", close);
    const std::string lifecycle =
        deactivate != std::string::npos && stateEnd != std::string::npos
        ? source.substr(deactivate, stateEnd - deactivate)
        : std::string{};
    Check(!lifecycle.empty() &&
            lifecycle.find("RequestWorkerStop()") != std::string::npos &&
            lifecycle.find("worker.join(") == std::string::npos &&
            lifecycle.find(".join()") == std::string::npos &&
            lifecycle.find("snapshot.operation = {}") != std::string::npos,
        "deactivation and close request stop and invalidate view state without joining on the STA");

    Check(source.find("ListLayoutBackups(context.paths, stop)") !=
                std::string::npos &&
            source.find("BuildInventory(context, stop") !=
                std::string::npos &&
            source.find("storageLock.try_lock_for("
                        "std::chrono::milliseconds(25))") !=
                std::string::npos &&
            source.find("gExternalReplacementQueued.load("
                        "std::memory_order_acquire)") !=
                std::string::npos &&
            source.find("FindFullBackup(\n"
                        "            manager, context.request.subjectId, "
                        "stop, cancelled)") != std::string::npos,
        "enumeration and pre-storage lookup observe cancellation between cooperative stages");

    const auto startWork = source.find("bool StartWork(");
    const auto taskIdentity = source.find(
        "inFlightTaskId = context.requestId", startWork);
    const auto workerDetach = source.find("worker.detach()", taskIdentity);
    const auto publishRunning = source.find("Publish();", workerDetach);
    const auto enableCancellation = source.find(
        "operationControl->EnableCancellation()", publishRunning);
    Check(startWork != std::string::npos &&
            taskIdentity != std::string::npos &&
            workerDetach != std::string::npos &&
            publishRunning != std::string::npos &&
            enableCancellation != std::string::npos &&
            taskIdentity < workerDetach && workerDetach < publishRunning &&
            publishRunning < enableCancellation &&
            source.find("WaitUntilStarted()", taskIdentity) !=
                std::string::npos,
        "worker identity and stop source exist before the running state opens its cancellation gate");

    Check(source.find("std::shared_ptr<BackupOperationControl> "
                      "operationControl") != std::string::npos &&
            source.find("snapshot.operation.cancellable = true") !=
                std::string::npos &&
            source.find("operationControl->RequestCancellation()") !=
                std::string::npos &&
            source.find("operationControl->NonInterruptible()") !=
                std::string::npos,
        "every detached backup task uses the atomic cancellation/commit phase instead of a presentation boolean");

    const auto cancel = source.find("void Cancel(");
    const auto confirm = source.find("void Confirm(", cancel);
    const std::string cancelBody =
        cancel != std::string::npos && confirm != std::string::npos
        ? source.substr(cancel, confirm - cancel)
        : std::string{};
    Check(!cancelBody.empty() &&
            cancelBody.find("RequestCancellation()") != std::string::npos &&
            cancelBody.find("!snapshot.operation.cancellable") ==
                std::string::npos,
        "Cancel arbitrates against the commit phase rather than trusting a stale UI snapshot");

    Check(source.find("state->snapshot.generation != generation") !=
                std::string::npos &&
            source.find("state->activationId != expectedActivationId") !=
                std::string::npos &&
            source.find("state->inFlightTaskId != requestId") !=
                std::string::npos &&
            source.find("state->operationControl != control") !=
                std::string::npos,
        "the non-cancellable UI transition is guarded by exact generation activation task and control identity");
}

void TestExistingStorageFormatsAreReused(const std::string& source)
{
    for (const char* operation : {
             "manager.Create(",
             "manager.ImportAndQueue(",
             "manager.Export(",
             "manager.QueueRestore(",
             "manager.Delete(",
             "manager.QueueDirectory("})
    {
        Check(source.find(operation) != std::string::npos,
            "complete-data work reuses FullDataBackupManager");
    }
    Check(source.find("CancellationContext cancellation") !=
                std::string::npos &&
            source.find("manager.Create(cancellation)") !=
                std::string::npos &&
            source.find("context.request.selectedPath, cancellation") !=
                std::string::npos &&
            source.find("*backup, context.request.selectedPath, "
                        "cancellation") != std::string::npos &&
            source.find("manager.QueueRestore(*backup, cancellation)") !=
                std::string::npos &&
            source.find("manager.Delete(*backup, cancellation)") !=
                std::string::npos &&
            source.find("manager.QueueDirectory(source, cancellation)") !=
                std::string::npos &&
            source.find("storageResult.cancelled") != std::string::npos,
        "every complete-data command passes cooperative cancellation through the existing manager API");

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

void TestOperationFeedbackIsLocalized(const std::string& source)
{
    for (const char* key : {
             "settings.backup.progress.createLayout",
             "settings.backup.progress.restoreLayout",
             "settings.backup.progress.deleteLayout",
             "settings.backup.progress.createFull",
             "settings.backup.progress.importFull",
             "settings.backup.progress.exportFull",
             "settings.backup.progress.restoreFull",
             "settings.backup.progress.deleteFull",
             "settings.backup.progress.migrate",
             "settings.backup.progress.refresh",
             "settings.backup.success.createLayout",
             "settings.backup.success.restoreLayout",
             "settings.backup.success.deleteLayout",
             "settings.backup.success.deleteFull",
             "settings.backup.success.generic",
             "settings.backup.error.layoutOperation",
             "settings.backup.error.dispatcherUnavailable",
             "settings.backup.error.openLocation",
             "settings.backup.error.missing",
             "settings.backup.error.restoreServiceUnavailable"})
    {
        Check(source.find(key) != std::string::npos,
            "backup operation feedback uses a localization key");
    }
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
            source.find("replacementLifecycleTaskId == "
                        "completion.context.requestId") !=
                std::string::npos &&
            source.find("replacementLifecycleGeneration ==") !=
                std::string::npos &&
            source.find("replacementLifecycleActivationId ==") !=
                std::string::npos &&
            source.find("CompleteQueuedReplacement("
                        "completion, matchingRequest)") !=
                std::string::npos &&
            source.find("if (publishToCurrentActivation)") !=
                std::string::npos,
        "a late cancel or page close cannot abandon an already queued replacement");
    Check(source.find("CanFinalizeClosedLifecycle()") !=
                std::string::npos &&
            source.find("IsWindow(owner) != FALSE") !=
                std::string::npos &&
            source.find("lifecycleReplacement && "
                        "CanFinalizeClosedLifecycle()") !=
                std::string::npos,
        "closed-view replacement completion requires a live hidden host before touching controller state");
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

    const auto finish = source.find("void Finish(");
    const auto activationGate = source.find(
        "completion.context.activationId == activationId", finish);
    const auto staleReturn = source.find("if (!matchingRequest)", finish);
    const auto layoutCommit = source.find(
        "options.commitLayoutRestore", finish);
    Check(finish != std::string::npos &&
            activationGate != std::string::npos &&
            staleReturn != std::string::npos &&
            layoutCommit != std::string::npos &&
            activationGate < staleReturn && staleReturn < layoutCommit,
        "a stale layout payload is rejected before the STA live-layout commit seam");
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
    TestOperationFeedbackIsLocalized(source);
    TestReplacementNeverFlushesOldMemory(header, source);
    TestApplicationOwnedLayoutCommit(application, compositionRoot);
}

void TestAtomicCancellationBoundary()
{
    using snowdesktop::winui::BackupOperationControl;
    using snowdesktop::winui::BackupOperationPhase;

    {
        auto control = std::make_shared<BackupOperationControl>();
        std::promise<BackupOperationPhase> observed;
        auto ready = observed.get_future();
        std::jthread worker([control, &observed] {
            control->WaitUntilStarted();
            observed.set_value(control->Phase());
        });
        Check(ready.wait_for(20ms) == std::future_status::timeout,
            "a detached worker cannot outrun publication of its initial running snapshot");
        Check(control->RequestCancellation(),
            "a synchronous snapshot callback can cancel during Starting");
        Check(ready.wait_for(1s) == std::future_status::ready &&
                ready.get() == BackupOperationPhase::CancellationRequested &&
                !control->TryBeginNonInterruptible(),
            "Starting cancellation wakes the worker and permanently closes the commit gate");
    }

    {
        BackupOperationControl control;
        Check(control.EnableCancellation() &&
                control.TryBeginNonInterruptible() &&
                !control.RequestCancellation() &&
                control.NonInterruptible(),
            "an operation that wins the commit gate cannot later report cancellation as accepted");
        control.Finish();
        Check(control.Phase() == BackupOperationPhase::Finished,
            "completion seals the operation phase");
    }

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        BackupOperationControl control;
        Check(control.EnableCancellation(),
            "a fresh operation enters its cancellable phase");
        std::atomic_bool start{false};
        bool cancelled = false;
        bool committed = false;
        std::jthread cancelThread([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled = control.RequestCancellation();
        });
        std::jthread commitThread([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            committed = control.TryBeginNonInterruptible();
        });
        start.store(true, std::memory_order_release);
        cancelThread.join();
        commitThread.join();
        Check(cancelled != committed,
            "exactly one side wins a concurrent cancel-versus-commit race");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestAtomicCancellationBoundary();
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
