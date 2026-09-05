#include "shell_file_operation_worker.h"
#include "item_location.h"
#include "app/shell_change_notification.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <stdexcept>

namespace
{

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path CreateTemporaryDirectory()
{
    wchar_t tempPath[MAX_PATH]{};
    wchar_t probePath[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, tempPath) != 0,
        "Windows temporary directory is available");
    Expect(GetTempFileNameW(
            tempPath, L"sdf", 0, probePath) != 0,
        "a unique worker-test path can be allocated");
    std::filesystem::path root(probePath);
    std::error_code error;
    std::filesystem::remove(root, error);
    Expect(!error && std::filesystem::create_directory(root, error),
        "worker-test directory can be created");
    wchar_t longPath[MAX_PATH]{};
    const DWORD length = GetLongPathNameW(root.c_str(), longPath, MAX_PATH);
    Expect(length != 0 && length < MAX_PATH,
        "temporary fixture paths normalize 8.3 aliases before Shell comparisons");
    return std::filesystem::path(longPath);
}

void TestAsyncRenames(const std::filesystem::path& root)
{
    constexpr UINT notificationMessage = WM_APP + 1;
    const wchar_t* className = L"SnowDesktopRenameNotificationTest";
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        if (message == WM_APP + 1)
        {
            auto* changes = reinterpret_cast<std::vector<ShellChangeNotification>*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (const auto change = ReadShellChangeNotification(wp, lp); change && changes)
                changes->push_back(*change);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    };
    Expect(RegisterClassW(&windowClass) != 0, "notification test class is registered");
    std::vector<ShellChangeNotification> notifications;
    const HWND notificationWindow = CreateWindowExW(0, className, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, &notifications);
    PIDLIST_ABSOLUTE rootId = nullptr;
    Expect(notificationWindow && SUCCEEDED(SHParseDisplayName(
        root.c_str(), nullptr, &rootId, 0, nullptr)), "isolated notification root is available");
    SHChangeNotifyEntry watch{rootId, FALSE};
    const ULONG registration = SHChangeNotifyRegister(notificationWindow,
        SHCNRF_ShellLevel | SHCNRF_NewDelivery, SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER,
        notificationMessage, 1, &watch);
    ILFree(rootId);
    Expect(registration != 0, "real Shell rename notifications are registered");
    const auto source = root / L"rename-source.txt";
    const auto collision = root / L"occupied.txt";
    { std::ofstream(source) << "rename payload"; }
    { std::ofstream(collision) << "keep original"; }
    snowdesktop::ShellFileOperationWorker worker;
    std::promise<snowdesktop::ShellRenameResult> firstPromise;
    auto firstFuture = firstPromise.get_future();
    std::promise<void> releaseWorker;
    auto gate = releaseWorker.get_future().share();
    std::atomic<DWORD> workerThread{0};
    std::atomic<bool> workerIsSta{false};
    const DWORD callerThread = GetCurrentThreadId();
    const auto enqueueStart = std::chrono::steady_clock::now();
    Expect(worker.Enqueue(
        snowdesktop::ShellRenameRequest{source.wstring(), L"renamed.txt", {}},
        [&](snowdesktop::ShellRenameResult result) {
            workerThread = GetCurrentThreadId();
            APTTYPE apartment{};
            APTTYPEQUALIFIER qualifier{};
            workerIsSta = SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
                (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
            firstPromise.set_value(std::move(result));
            gate.wait();
        }), "rename request is accepted without waiting for Shell");
    const auto enqueueMs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - enqueueStart).count();
    Expect(firstFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
        "background rename completes within the test deadline");
    const auto first = firstFuture.get();
    Expect(SUCCEEDED(first.status), "background Shell rename reports success");
    Expect(first.metadataComplete, "background Shell rename returns complete metadata");
    Expect(!std::filesystem::exists(source) &&
            std::filesystem::equivalent(first.path, root / L"renamed.txt"),
        "rename returns the actual Shell destination, including canonicalized paths");
    Expect(workerThread != callerThread && workerIsSta,
        "rename executes on a separate STA");

    // Deliberately hold the worker callback. A second enqueue must still
    // return, while its dependent operation waits in the serial queue.
    std::promise<snowdesktop::ShellRenameResult> secondPromise;
    auto secondFuture = secondPromise.get_future();
    Expect(worker.Enqueue(
        snowdesktop::ShellRenameRequest{first.path, L"occupied.txt", {}},
        [&](snowdesktop::ShellRenameResult result) {
            secondPromise.set_value(std::move(result));
        }), "a blocked worker still accepts another rename");
    Expect(secondFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
        "dependent renames wait for the preceding operation without blocking the caller");
    releaseWorker.set_value();
    Expect(secondFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
        "queued rename completes after releasing the preceding callback");
    const auto second = secondFuture.get();
    Expect(SUCCEEDED(second.status) &&
            std::filesystem::equivalent(second.path, root / L"occupied (2).txt"),
        "collision renaming reports its actual suffixed destination");
    std::string contents;
    { std::ifstream stream(collision); std::getline(stream, contents); }
    Expect(contents == "keep original", "rename must not overwrite the colliding file");
    worker.Stop();

    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool receivedRename = false;
    while (!receivedRename && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        receivedRename = std::any_of(notifications.begin(), notifications.end(),
            [&](const auto& change) {
                return change.event == SHCNE_RENAMEITEM &&
                    _wcsicmp(change.source.c_str(), source.c_str()) == 0 &&
                    _wcsicmp(change.target.c_str(), first.path.c_str()) == 0;
            });
        if (!receivedRename)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    Expect(receivedRename,
        "NewDelivery decoding recovers the actual old/new paths and releases the notification");
    SHChangeNotifyDeregister(registration);
    DestroyWindow(notificationWindow);
    UnregisterClassW(className, windowClass.hInstance);

    const auto caseOnly = snowdesktop::ShellFileOperationWorker::Execute(
        snowdesktop::ShellRenameRequest{second.path, L"OCCUPIED (2).txt", {}});
    Expect(SUCCEEDED(caseOnly.status) &&
            std::filesystem::path(caseOnly.path).filename() == L"OCCUPIED (2).txt",
        "case-only rename must not add an unnecessary collision suffix");
    const auto directory = root / L"rename-directory";
    std::filesystem::create_directory(directory);
    { std::ofstream(directory / L"child.txt") << "preserve child"; }
    const auto folder = snowdesktop::ShellFileOperationWorker::Execute(
        snowdesktop::ShellRenameRequest{directory.wstring(), L"renamed-directory", {}});
    Expect(SUCCEEDED(folder.status) && folder.metadataComplete &&
            (folder.attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            std::filesystem::exists(std::filesystem::path(folder.path) / L"child.txt"),
        "directory renames retain their children and return directory metadata");
    const auto invalid = snowdesktop::ShellFileOperationWorker::Execute(
        snowdesktop::ShellRenameRequest{caseOnly.path, L"invalid/name.txt", {}});
    Expect(FAILED(invalid.status) && std::filesystem::exists(caseOnly.path),
        "invalid names fail without changing the source");
    Expect(!worker.Enqueue(
        snowdesktop::ShellRenameRequest{caseOnly.path, L"after-stop.txt", {}},
        [](snowdesktop::ShellRenameResult) {}),
        "shutdown rejects new rename requests");
    std::cout << "rename enqueue us=" << enqueueMs
              << " worker ms=" << first.elapsedMs << '\n';
}

} // namespace

void TestBackgroundReadsDoNotBlockFileOperations(const std::filesystem::path& root)
{
    const auto temporaryFile = root / L"read-worker-delete.txt";
    std::ofstream(temporaryFile) << "temporary read fixture";
    snowdesktop::ShellFileOperationWorker reader;
    snowdesktop::ShellFileOperationWorker operations;
    const DWORD callerThread = GetCurrentThreadId();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    std::promise<bool> completed;
    auto completedFuture = completed.get_future();
    const auto started = std::chrono::steady_clock::now();
    Expect(reader.Enqueue(snowdesktop::ShellReadRequest{[&] {
        APTTYPE apartment{};
        APTTYPEQUALIFIER qualifier{};
        const bool separateSta = GetCurrentThreadId() != callerThread &&
            SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            apartment == APTTYPE_STA;
        entered.set_value();
        releaseFuture.wait();
        return separateSta && !std::filesystem::exists(temporaryFile);
    }}, [&](bool success) { completed.set_value(success); }),
        "metadata work can be queued without waiting for its I/O");
    const auto enqueueUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    Expect(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "the metadata read reaches its deterministic I/O gate");
    Expect(completedFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
        "the caller remains available while metadata I/O is deliberately blocked");
    std::promise<bool> deleted;
    auto deletedFuture = deleted.get_future();
    snowdesktop::ShellFileOperationRequest request;
    request.steps.push_back({FO_DELETE, {temporaryFile.wstring()}, {},
        static_cast<FILEOP_FLAGS>(FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI)});
    Expect(operations.Enqueue(std::move(request), [&](bool success) { deleted.set_value(success); }),
        "file deletion can be queued while the independent reader is blocked");
    Expect(deletedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            deletedFuture.get(),
        "a slow directory read must not block later file operations");
    release.set_value();
    Expect(completedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            completedFuture.get(),
        "the STA read completes with data after the concurrent deletion");
    std::promise<bool> exceptionResult, nextResult;
    auto exceptionFuture = exceptionResult.get_future();
    auto nextFuture = nextResult.get_future();
    Expect(reader.Enqueue(snowdesktop::ShellReadRequest{[]() -> bool {
        throw std::runtime_error("fixture read failed");
    }}, [&](bool success) { exceptionResult.set_value(success); }),
        "a failing read can report failure through the completion channel");
    Expect(reader.Enqueue(snowdesktop::ShellReadRequest{[] { return true; }},
        [&](bool success) { nextResult.set_value(success); }),
        "a later read remains queued after a failed read");
    Expect(exceptionFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            !exceptionFuture.get() &&
            nextFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready && nextFuture.get(),
        "a read exception neither terminates the worker nor falsely reports success");
    reader.Stop();
    operations.Stop();
    Expect(!reader.Enqueue(snowdesktop::ShellReadRequest{[] { return true; }}, {}),
        "a stopped metadata worker cannot accept new reads");
    std::cout << "metadata read enqueue us=" << enqueueUs << '\n';
}

int wmain()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Expect(SUCCEEDED(comResult),
        "COM initializes for copied folder-shortcut validation");
    const std::filesystem::path root = CreateTemporaryDirectory();
    TestAsyncRenames(root);
    TestBackgroundReadsDoNotBlockFileOperations(root);
    const std::filesystem::path sourceDirectory = root / L"source";
    const std::filesystem::path firstDirectory = root / L"first";
    const std::filesystem::path secondDirectory = root / L"second";
    const std::filesystem::path handoffDirectory = root / L"handoff";
    const std::filesystem::path alternateSourceDirectory =
        root / L"alternate-source";
    const std::filesystem::path multiHandoffDirectory =
        root / L"multi-handoff";
    const std::filesystem::path rejectedHandoffDirectory =
        root / L"rejected-handoff";
    const std::filesystem::path noneEffectDirectory =
        root / L"none-effect";
    const std::filesystem::path preflightDirectory =
        root / L"preflight";
    const std::filesystem::path shortcutDirectory =
        root / L"shortcuts";
    const std::filesystem::path folderShortcutTarget =
        root / L"folder-shortcut-target";
    const std::filesystem::path secondFolderShortcutTarget =
        root / L"second-folder-shortcut-target";
    std::filesystem::create_directories(sourceDirectory);
    std::filesystem::create_directories(firstDirectory);
    std::filesystem::create_directories(secondDirectory);
    std::filesystem::create_directories(handoffDirectory);
    std::filesystem::create_directories(alternateSourceDirectory);
    std::filesystem::create_directories(multiHandoffDirectory);
    std::filesystem::create_directories(rejectedHandoffDirectory);
    std::filesystem::create_directories(noneEffectDirectory);
    std::filesystem::create_directories(preflightDirectory);
    std::filesystem::create_directories(shortcutDirectory);
    std::filesystem::create_directories(folderShortcutTarget);
    std::filesystem::create_directories(secondFolderShortcutTarget);

    const std::filesystem::path source =
        sourceDirectory / L"payload.txt";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "SnowDesktop async Shell worker";
    }
    const std::filesystem::path handoffSource =
        sourceDirectory / L"handoff.txt";
    {
        std::ofstream stream(handoffSource, std::ios::binary);
        stream << "SnowDesktop async IDropTarget worker";
    }
    const std::filesystem::path alternateHandoffSource =
        alternateSourceDirectory / L"alternate.txt";
    {
        std::ofstream stream(alternateHandoffSource, std::ios::binary);
        stream << "SnowDesktop multi-parent IDropTarget worker";
    }
    const std::filesystem::path rejectedHandoffSource =
        sourceDirectory / L"must-not-copy.txt";
    {
        std::ofstream stream(rejectedHandoffSource, std::ios::binary);
        stream << "SnowDesktop all-or-nothing IDropTarget worker";
    }

    constexpr FILEOP_FLAGS kTestFlags =
        FOF_SILENT | FOF_NOCONFIRMATION |
        FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    const std::vector<std::wstring> recycleSources = {
        (sourceDirectory / L"recycle-one.txt").wstring(),
        (sourceDirectory / L"recycle-two.txt").wstring() };
    const auto recycleRequest =
        snowdesktop::CreateRecycleBinDeleteRequest(recycleSources);
    Expect(recycleRequest.steps.size() == 1 &&
            recycleRequest.steps[0].function == FO_DELETE &&
            recycleRequest.steps[0].sources == recycleSources &&
            recycleRequest.steps[0].destination.empty(),
        "Recycle Bin drops build one path-backed delete operation");
    Expect((recycleRequest.steps[0].flags & FOF_ALLOWUNDO) != 0 &&
            (recycleRequest.steps[0].flags & FOF_WANTNUKEWARNING) != 0,
        "Recycle Bin deletes stay recoverable and warn before permanent deletion");
    Expect(snowdesktop::CreateRecycleBinDeleteRequest({}).steps.empty(),
        "an empty Recycle Bin drop does not create a delete operation");
    snowdesktop::ShellFileOperationRequest copyRequest;
    copyRequest.steps.push_back({
        FO_COPY,
        { source.wstring() },
        firstDirectory.wstring(),
        kTestFlags });
    snowdesktop::ShellFileOperationRequest moveRequest;
    moveRequest.steps.push_back({
        FO_MOVE,
        { (firstDirectory / source.filename()).wstring() },
        secondDirectory.wstring(),
        kTestFlags });
    snowdesktop::ShellDropRequest handoffRequest;
    handoffRequest.sources = { handoffSource.wstring() };
    handoffRequest.targetParsingName = handoffDirectory.wstring();
    handoffRequest.keyState = MK_LBUTTON | MK_CONTROL;
    handoffRequest.allowedEffects = DROPEFFECT_COPY;
    snowdesktop::ShellDropRequest multiHandoffRequest;
    multiHandoffRequest.sources = {
        source.wstring(), alternateHandoffSource.wstring() };
    multiHandoffRequest.targetParsingName =
        multiHandoffDirectory.wstring();
    multiHandoffRequest.keyState = MK_LBUTTON | MK_CONTROL;
    multiHandoffRequest.allowedEffects = DROPEFFECT_COPY;
    snowdesktop::ShellDropRequest partialSourceRequest;
    partialSourceRequest.sources = {
        rejectedHandoffSource.wstring(),
        (sourceDirectory / L"missing.txt").wstring() };
    partialSourceRequest.targetParsingName =
        rejectedHandoffDirectory.wstring();
    partialSourceRequest.keyState = MK_LBUTTON | MK_CONTROL;
    partialSourceRequest.allowedEffects = DROPEFFECT_COPY;
    snowdesktop::ShellDropRequest noneEffectRequest;
    noneEffectRequest.sources = { rejectedHandoffSource.wstring() };
    noneEffectRequest.targetParsingName = noneEffectDirectory.wstring();
    noneEffectRequest.keyState = MK_LBUTTON;
    noneEffectRequest.allowedEffects = DROPEFFECT_NONE;
    std::atomic<int> preflightCalls{0};
    snowdesktop::ShellDropRequest preflightRequest;
    preflightRequest.sources = { handoffSource.wstring() };
    preflightRequest.targetParsingName = preflightDirectory.wstring();
    preflightRequest.keyState = MK_LBUTTON | MK_CONTROL;
    preflightRequest.allowedEffects = DROPEFFECT_COPY;
    preflightRequest.dataObjectPreflight =
        [&preflightCalls](IDataObject* dataObject) {
            ++preflightCalls;
            return dataObject != nullptr;
        };
    snowdesktop::ShellFileOperationRequest shortcutRequest;
    const std::filesystem::path shortcutPath =
        shortcutDirectory / L"handoff.lnk";
    shortcutRequest.shortcuts.push_back({
        handoffSource.wstring(), shortcutPath.wstring(),
        shortcutDirectory.wstring() });
    snowdesktop::ShellFileOperationRequest partialShortcutRequest;
    const std::filesystem::path partialShortcutPath =
        shortcutDirectory / L"partial.lnk";
    partialShortcutRequest.shortcuts.push_back({
        handoffSource.wstring(), partialShortcutPath.wstring(),
        shortcutDirectory.wstring() });
    partialShortcutRequest.shortcuts.push_back({
        handoffSource.wstring(),
        (root / L"missing-directory" / L"failed.lnk").wstring(),
        shortcutDirectory.wstring() });
    const std::filesystem::path sourceFolderShortcut =
        shortcutDirectory / L"source-folder.lnk";
    const std::filesystem::path copiedFolderShortcut =
        shortcutDirectory / L"copied-folder.lnk";
    snowdesktop::ShellFileOperationRequest
        createFolderShortcutRequest;
    createFolderShortcutRequest.shortcuts.push_back({
        folderShortcutTarget.wstring(),
        sourceFolderShortcut.wstring(),
        shortcutDirectory.wstring() });
    snowdesktop::ShellFileOperationRequest
        copyFolderShortcutRequest;
    copyFolderShortcutRequest.exactFileCopies.push_back({
        sourceFolderShortcut.wstring(),
        copiedFolderShortcut.wstring() });
    const std::filesystem::path atomicShortcutPath =
        shortcutDirectory / L"atomic-folder.lnk";
    snowdesktop::ShellFileOperationRequest
        firstAtomicShortcutRequest;
    firstAtomicShortcutRequest.shortcuts.push_back({
        folderShortcutTarget.wstring(),
        atomicShortcutPath.wstring(),
        shortcutDirectory.wstring(), true });
    snowdesktop::ShellFileOperationRequest
        collidingAtomicShortcutRequest;
    collidingAtomicShortcutRequest.shortcuts.push_back({
        secondFolderShortcutTarget.wstring(),
        atomicShortcutPath.wstring(),
        shortcutDirectory.wstring(), true });

    snowdesktop::ShellFileOperationWorker worker;
    std::promise<bool> copyPromise;
    std::promise<bool> movePromise;
    std::promise<bool> handoffPromise;
    std::promise<bool> multiHandoffPromise;
    std::promise<bool> partialSourcePromise;
    std::promise<bool> noneEffectPromise;
    std::promise<bool> preflightPromise;
    std::promise<bool> shortcutPromise;
    std::promise<bool> partialShortcutPromise;
    std::promise<bool> createFolderShortcutPromise;
    std::promise<bool> copyFolderShortcutPromise;
    std::promise<bool> firstAtomicShortcutPromise;
    std::promise<bool> collidingAtomicShortcutPromise;
    auto copyFuture = copyPromise.get_future();
    auto moveFuture = movePromise.get_future();
    auto handoffFuture = handoffPromise.get_future();
    auto multiHandoffFuture = multiHandoffPromise.get_future();
    auto partialSourceFuture = partialSourcePromise.get_future();
    auto noneEffectFuture = noneEffectPromise.get_future();
    auto preflightFuture = preflightPromise.get_future();
    auto shortcutFuture = shortcutPromise.get_future();
    auto partialShortcutFuture =
        partialShortcutPromise.get_future();
    auto createFolderShortcutFuture =
        createFolderShortcutPromise.get_future();
    auto copyFolderShortcutFuture =
        copyFolderShortcutPromise.get_future();
    auto firstAtomicShortcutFuture =
        firstAtomicShortcutPromise.get_future();
    auto collidingAtomicShortcutFuture =
        collidingAtomicShortcutPromise.get_future();

    const auto enqueueStart = std::chrono::steady_clock::now();
    Expect(worker.Enqueue(
            std::move(copyRequest),
            [&copyPromise](bool succeeded) {
                copyPromise.set_value(succeeded);
            }),
        "copy request is accepted");
    Expect(worker.Enqueue(
            std::move(moveRequest),
            [&movePromise](bool succeeded) {
                movePromise.set_value(succeeded);
            }),
        "dependent move request is accepted");
    Expect(worker.Enqueue(
            std::move(handoffRequest),
            [&handoffPromise](bool succeeded) {
                handoffPromise.set_value(succeeded);
            }),
        "path-backed IDropTarget request is accepted");
    Expect(worker.Enqueue(
            std::move(multiHandoffRequest),
            [&multiHandoffPromise](bool succeeded) {
                multiHandoffPromise.set_value(succeeded);
            }),
        "multi-parent IDropTarget request is accepted");
    Expect(worker.Enqueue(
            std::move(partialSourceRequest),
            [&partialSourcePromise](bool succeeded) {
                partialSourcePromise.set_value(succeeded);
            }),
        "partially unavailable IDropTarget request is accepted");
    Expect(worker.Enqueue(
            std::move(noneEffectRequest),
            [&noneEffectPromise](bool succeeded) {
                noneEffectPromise.set_value(succeeded);
            }),
        "none-effect IDropTarget request is accepted for rejection");
    Expect(worker.Enqueue(
            std::move(preflightRequest),
            [&preflightPromise](bool succeeded) {
                preflightPromise.set_value(succeeded);
            }),
        "data-object preflight request is accepted");
    Expect(worker.Enqueue(
            std::move(shortcutRequest),
            [&shortcutPromise](bool succeeded) {
                shortcutPromise.set_value(succeeded);
            }),
        "Shell shortcut request is accepted");
    Expect(worker.Enqueue(
            std::move(partialShortcutRequest),
            [&partialShortcutPromise](bool succeeded) {
                partialShortcutPromise.set_value(succeeded);
            }),
        "partial shortcut request is accepted for failure reporting");
    Expect(worker.Enqueue(
            std::move(createFolderShortcutRequest),
            [&createFolderShortcutPromise](bool succeeded) {
                createFolderShortcutPromise.set_value(succeeded);
            }),
        "folder shortcut creation is accepted");
    Expect(worker.Enqueue(
            std::move(copyFolderShortcutRequest),
            [&copyFolderShortcutPromise](bool succeeded) {
                copyFolderShortcutPromise.set_value(succeeded);
            }),
        "folder shortcut preservation copy is accepted");
    Expect(worker.Enqueue(
            std::move(firstAtomicShortcutRequest),
            [&firstAtomicShortcutPromise](bool succeeded) {
                firstAtomicShortcutPromise.set_value(succeeded);
            }),
        "first exact-name shortcut request is accepted");
    Expect(worker.Enqueue(
            std::move(collidingAtomicShortcutRequest),
            [&collidingAtomicShortcutPromise](bool succeeded) {
                collidingAtomicShortcutPromise.set_value(succeeded);
            }),
        "colliding exact-name shortcut request is accepted");
    Expect(std::chrono::steady_clock::now() - enqueueStart <
            std::chrono::seconds(1),
        "enqueueing file operations does not wait for Shell completion");

    Expect(copyFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && copyFuture.get(),
        "copy operation completes successfully");
    Expect(moveFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && moveFuture.get(),
        "serial move observes the preceding copied file");
    Expect(handoffFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && handoffFuture.get(),
        "path-backed IDropTarget handoff completes successfully");
    Expect(multiHandoffFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && multiHandoffFuture.get(),
        "multi-parent IDropTarget handoff completes successfully");
    Expect(partialSourceFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && !partialSourceFuture.get(),
        "a missing source rejects the complete IDropTarget handoff");
    Expect(noneEffectFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && !noneEffectFuture.get(),
        "DROPEFFECT_NONE remains rejected");
    Expect(preflightFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && preflightFuture.get() &&
            preflightCalls.load() == 1,
        "a successful data-object preflight bypasses Shell on the worker");
    Expect(shortcutFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && shortcutFuture.get(),
        "Shell shortcut creation completes successfully");
    Expect(partialShortcutFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready &&
            !partialShortcutFuture.get(),
        "a partially failed shortcut batch reports failure");
    Expect(createFolderShortcutFuture.wait_for(
            std::chrono::seconds(15)) ==
            std::future_status::ready &&
            createFolderShortcutFuture.get(),
        "folder shortcut creation completes successfully");
    Expect(copyFolderShortcutFuture.wait_for(
            std::chrono::seconds(15)) ==
            std::future_status::ready &&
            copyFolderShortcutFuture.get(),
        "folder shortcut preservation copy completes successfully");
    Expect(firstAtomicShortcutFuture.wait_for(
            std::chrono::seconds(15)) ==
            std::future_status::ready &&
            firstAtomicShortcutFuture.get(),
        "the first exact-name shortcut request succeeds");
    Expect(collidingAtomicShortcutFuture.wait_for(
            std::chrono::seconds(15)) ==
            std::future_status::ready &&
            !collidingAtomicShortcutFuture.get(),
        "a queued same-name shortcut cannot overwrite the first result");
    worker.Stop();

    Expect(std::filesystem::exists(source),
        "copy preserves the original source");
    Expect(!std::filesystem::exists(
            firstDirectory / source.filename()),
        "move removes the intermediate copy");
    Expect(std::filesystem::exists(
            secondDirectory / source.filename()),
        "move creates the final destination");
    Expect(std::filesystem::exists(
            handoffDirectory / handoffSource.filename()),
        "IDropTarget handoff creates the copied file");
    Expect(std::filesystem::exists(
            multiHandoffDirectory / source.filename()) &&
        std::filesystem::exists(
            multiHandoffDirectory /
                alternateHandoffSource.filename()),
        "multi-parent IDropTarget handoff copies every source");
    Expect(!std::filesystem::exists(
            rejectedHandoffDirectory /
                rejectedHandoffSource.filename()),
        "a rejected multi-source handoff copies no valid subset");
    Expect(!std::filesystem::exists(
            noneEffectDirectory / rejectedHandoffSource.filename()),
        "a none-effect handoff performs no operation");
    Expect(!std::filesystem::exists(
            preflightDirectory / handoffSource.filename()),
        "a handled preflight does not also execute the Shell drop");
    Expect(std::filesystem::exists(shortcutPath),
        "Shell shortcut creation writes the link file");
    Expect(std::filesystem::exists(partialShortcutPath),
        "a partial shortcut batch may retain successful files");
    const auto copiedFolderTarget =
        snowdesktop::item_location::ResolveFolderTarget(
            copiedFolderShortcut.wstring());
    std::error_code equivalentError;
    Expect(copiedFolderTarget.available &&
            copiedFolderTarget.kind ==
                snowdesktop::item_location::
                    FolderTargetKind::Shortcut &&
            std::filesystem::equivalent(
                copiedFolderTarget.path,
                folderShortcutTarget,
                equivalentError) &&
            !equivalentError,
        "copied folder shortcut still resolves directly to its original folder");
    const auto atomicFolderTarget =
        snowdesktop::item_location::ResolveFolderTarget(
            atomicShortcutPath.wstring());
    equivalentError.clear();
    Expect(atomicFolderTarget.available &&
            atomicFolderTarget.kind ==
                snowdesktop::item_location::
                    FolderTargetKind::Shortcut &&
            std::filesystem::equivalent(
                atomicFolderTarget.path,
                folderShortcutTarget,
                equivalentError) &&
            !equivalentError,
        "a colliding queued shortcut leaves the first target unchanged");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    Expect(!cleanupError, "worker-test directory is removed");
    CoUninitialize();

    std::cout << "shell file-operation worker tests passed\n";
    return 0;
}
