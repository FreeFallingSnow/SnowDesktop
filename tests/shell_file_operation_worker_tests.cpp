#include "shell_file_operation_worker.h"
#include "item_location.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

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
    return root;
}

} // namespace

int wmain()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Expect(SUCCEEDED(comResult),
        "COM initializes for copied folder-shortcut validation");
    const std::filesystem::path root = CreateTemporaryDirectory();
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
    const std::filesystem::path shortcutDirectory =
        root / L"shortcuts";
    const std::filesystem::path folderShortcutTarget =
        root / L"folder-shortcut-target";
    std::filesystem::create_directories(sourceDirectory);
    std::filesystem::create_directories(firstDirectory);
    std::filesystem::create_directories(secondDirectory);
    std::filesystem::create_directories(handoffDirectory);
    std::filesystem::create_directories(alternateSourceDirectory);
    std::filesystem::create_directories(multiHandoffDirectory);
    std::filesystem::create_directories(rejectedHandoffDirectory);
    std::filesystem::create_directories(noneEffectDirectory);
    std::filesystem::create_directories(shortcutDirectory);
    std::filesystem::create_directories(folderShortcutTarget);

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
    copyFolderShortcutRequest.steps.push_back({
        FO_COPY,
        { sourceFolderShortcut.wstring() },
        copiedFolderShortcut.wstring(),
        kTestFlags });

    snowdesktop::ShellFileOperationWorker worker;
    std::promise<bool> copyPromise;
    std::promise<bool> movePromise;
    std::promise<bool> handoffPromise;
    std::promise<bool> multiHandoffPromise;
    std::promise<bool> partialSourcePromise;
    std::promise<bool> noneEffectPromise;
    std::promise<bool> shortcutPromise;
    std::promise<bool> partialShortcutPromise;
    std::promise<bool> createFolderShortcutPromise;
    std::promise<bool> copyFolderShortcutPromise;
    auto copyFuture = copyPromise.get_future();
    auto moveFuture = movePromise.get_future();
    auto handoffFuture = handoffPromise.get_future();
    auto multiHandoffFuture = multiHandoffPromise.get_future();
    auto partialSourceFuture = partialSourcePromise.get_future();
    auto noneEffectFuture = noneEffectPromise.get_future();
    auto shortcutFuture = shortcutPromise.get_future();
    auto partialShortcutFuture =
        partialShortcutPromise.get_future();
    auto createFolderShortcutFuture =
        createFolderShortcutPromise.get_future();
    auto copyFolderShortcutFuture =
        copyFolderShortcutPromise.get_future();

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

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    Expect(!cleanupError, "worker-test directory is removed");
    CoUninitialize();

    std::cout << "shell file-operation worker tests passed\n";
    return 0;
}
