#include "shell_file_operation_worker.h"

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
    const std::filesystem::path root = CreateTemporaryDirectory();
    const std::filesystem::path sourceDirectory = root / L"source";
    const std::filesystem::path firstDirectory = root / L"first";
    const std::filesystem::path secondDirectory = root / L"second";
    std::filesystem::create_directories(sourceDirectory);
    std::filesystem::create_directories(firstDirectory);
    std::filesystem::create_directories(secondDirectory);

    const std::filesystem::path source =
        sourceDirectory / L"payload.txt";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "SnowDesktop async Shell worker";
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

    snowdesktop::ShellFileOperationWorker worker;
    std::promise<bool> copyPromise;
    std::promise<bool> movePromise;
    auto copyFuture = copyPromise.get_future();
    auto moveFuture = movePromise.get_future();

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
    Expect(std::chrono::steady_clock::now() - enqueueStart <
            std::chrono::seconds(1),
        "enqueueing file operations does not wait for Shell completion");

    Expect(copyFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && copyFuture.get(),
        "copy operation completes successfully");
    Expect(moveFuture.wait_for(std::chrono::seconds(15)) ==
            std::future_status::ready && moveFuture.get(),
        "serial move observes the preceding copied file");
    worker.Stop();

    Expect(std::filesystem::exists(source),
        "copy preserves the original source");
    Expect(!std::filesystem::exists(
            firstDirectory / source.filename()),
        "move removes the intermediate copy");
    Expect(std::filesystem::exists(
            secondDirectory / source.filename()),
        "move creates the final destination");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    Expect(!cleanupError, "worker-test directory is removed");

    std::cout << "shell file-operation worker tests passed\n";
    return 0;
}
