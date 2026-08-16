#include "widget_filesystem_task_executor.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path CreateProbeDirectory()
{
    wchar_t temporary[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporary) > 0,
        "temporary directory is available");
    const auto root = std::filesystem::path(temporary) /
        (L"SnowDesktopWidgetFilesystemTaskExecutorTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    Expect(std::filesystem::create_directories(root, error) && !error,
        "probe directory is created");
    return std::filesystem::weakly_canonical(root);
}

snowdesktop::widget_runtime::WidgetFilesystemTaskCompletion WaitFor(
    snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor& executor,
    std::uint64_t id)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (auto& completion : executor.DrainCompletions())
            if (completion.id == id) return std::move(completion);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("filesystem task completion timed out");
}
}

int main()
{
    using namespace snowdesktop::widget_runtime;
    const auto root = CreateProbeDirectory();
    const auto file = root / L"note.txt";
    const auto second = root / L"second.txt";
    const auto binaryFile = root / L"payload.bin";
    std::ofstream(file, std::ios::binary) << "hello";
    std::ofstream(second, std::ios::binary) << "second";
    const std::string binaryBytes("\x00\x01\x7f\xff", 4);
    {
        std::ofstream output(binaryFile, std::ios::binary);
        output.write(binaryBytes.data(),
            static_cast<std::streamsize>(binaryBytes.size()));
    }

    WidgetFilesystemTaskExecutor executor;
    WidgetFilesystemTaskRequest stat;
    stat.action = "filesystem.stat";
    stat.path = file;
    Expect(static_cast<bool>(executor.Start(1, "instance", stat)),
        "stat task starts");
    const auto statResult = WaitFor(executor, 1);
    Expect(statResult.ok && statResult.metadata.name == "note.txt" &&
            statResult.metadata.size == 5 &&
            !statResult.metadata.revision.empty(),
        "stat returns bounded file metadata and revision");

    WidgetFilesystemTaskRequest read;
    read.action = "filesystem.read";
    read.path = file;
    read.maxBytes = 16;
    Expect(static_cast<bool>(executor.Start(2, "instance", read)),
        "read task starts");
    const auto readResult = WaitFor(executor, 2);
    Expect(readResult.ok && readResult.text == "hello" &&
            readResult.metadata.revision == statResult.metadata.revision,
        "read returns UTF-8 text and the observed revision");

    WidgetFilesystemTaskRequest list;
    list.action = "filesystem.list";
    list.path = root;
    list.limit = 1;
    Expect(static_cast<bool>(executor.Start(3, "instance", list)),
        "list task starts");
    const auto listResult = WaitFor(executor, 3);
    Expect(listResult.ok && listResult.items.size() == 1 &&
            listResult.nextOffset == 1 && listResult.hasMore,
        "list paginates a bounded direct directory enumeration");

    WidgetFilesystemTaskRequest write;
    write.action = "filesystem.write";
    write.path = file;
    write.text = "updated";
    write.expectedRevision = statResult.metadata.revision;
    Expect(static_cast<bool>(executor.Start(4, "writer", write)),
        "write task starts");
    const auto writeResult = WaitFor(executor, 4);
    Expect(writeResult.ok && writeResult.metadata.size == 7 &&
            writeResult.metadata.revision != statResult.metadata.revision,
        "atomic write returns the new revision");

    WidgetFilesystemTaskRequest conflict = write;
    conflict.text = "stale";
    Expect(static_cast<bool>(executor.Start(
            5, "other-writer", conflict)),
        "conflicting write task starts");
    const auto conflictResult = WaitFor(executor, 5);
    Expect(!conflictResult.ok && conflictResult.error == "conflict",
        "stale expected revisions reject writes");

    WidgetFilesystemTaskRequest smallRead = read;
    smallRead.maxBytes = 2;
    Expect(static_cast<bool>(executor.Start(6, "instance", smallRead)),
        "bounded read task starts");
    const auto smallReadResult = WaitFor(executor, 6);
    Expect(!smallReadResult.ok && smallReadResult.error == "fileTooLarge",
        "read respects the caller byte ceiling");

    WidgetFilesystemTaskRequest invalidTextRead = read;
    invalidTextRead.path = binaryFile;
    Expect(static_cast<bool>(executor.Start(7, "instance", invalidTextRead)),
        "UTF-8 validation read starts");
    const auto invalidTextReadResult = WaitFor(executor, 7);
    Expect(!invalidTextReadResult.ok &&
            invalidTextReadResult.error == "invalidEncoding",
        "UTF-8 mode rejects binary payloads");

    WidgetFilesystemTaskRequest binaryRead = invalidTextRead;
    binaryRead.encoding = "binary";
    Expect(static_cast<bool>(executor.Start(8, "instance", binaryRead)),
        "binary read starts");
    const auto binaryReadResult = WaitFor(executor, 8);
    Expect(binaryReadResult.ok && binaryReadResult.encoding == "binary" &&
            binaryReadResult.text == binaryBytes,
        "binary read preserves embedded NUL and non-UTF-8 bytes");

    WidgetFilesystemTaskRequest binaryWrite;
    binaryWrite.action = "filesystem.write";
    binaryWrite.path = binaryFile;
    binaryWrite.encoding = "binary";
    binaryWrite.text = std::string("\xff\x00\x42", 3);
    binaryWrite.expectedRevision = binaryReadResult.metadata.revision;
    Expect(static_cast<bool>(executor.Start(9, "binary-writer", binaryWrite)),
        "binary write starts");
    const auto binaryWriteResult = WaitFor(executor, 9);
    Expect(binaryWriteResult.ok && binaryWriteResult.metadata.size == 3,
        "binary write atomically replaces the selected file");

    binaryRead.maxBytes = 3;
    Expect(static_cast<bool>(executor.Start(10, "instance", binaryRead)),
        "binary verification read starts");
    const auto binaryVerification = WaitFor(executor, 10);
    Expect(binaryVerification.ok &&
            binaryVerification.text == binaryWrite.text,
        "binary write round-trips exact bytes");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::cout << "widget filesystem task executor tests passed\n";
    return 0;
}
