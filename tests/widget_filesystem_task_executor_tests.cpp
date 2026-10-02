#include "widget_filesystem_task_executor.h"
#include "test_temporary_directory.h"

#include <windows.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
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
    const snowdesktop::test::TemporaryDirectory temporary;
    const auto root = std::filesystem::weakly_canonical(temporary.path);
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

    // A running cooperative action must relinquish the sole worker, allowing
    // the next action through. The runner has a bounded failure deadline so a
    // regression cannot hang teardown. No real file or device blocks here.
    {
        std::promise<void> entered;
        auto started = entered.get_future();
        WidgetFilesystemTaskExecutor cooperative({}, {},
            [&](const WidgetFilesystemTaskRequest& request, std::stop_token stop) {
                if (request.handle == "first")
                {
                    entered.set_value();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    WidgetFilesystemTaskRunResult result;
                    result.error = stop.stop_requested() ? "canceled" : "cancelNotDelivered";
                    return result;
                }
                WidgetFilesystemTaskRunResult result; result.ok = true; return result;
            });
        WidgetFilesystemTaskRequest request;
        request.action = "filesystem.stat"; request.path = file; request.handle = "first";
        Expect(static_cast<bool>(cooperative.Start(801, "cancel-test", request)), "cooperative action starts");
        Expect(started.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "first action is running");
        request.handle = "second";
        Expect(static_cast<bool>(cooperative.Start(802, "cancel-test", request)), "next action queues");
        Expect(cooperative.Cancel(801), "running action accepts cancellation");
        bool canceled = false, advanced = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while ((!canceled || !advanced) && std::chrono::steady_clock::now() < deadline)
        {
            for (auto& completion : cooperative.DrainCompletions())
            {
                if (completion.id == 801) canceled = !completion.ok && completion.error == "canceled";
                if (completion.id == 802) advanced = completion.ok;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Expect(canceled && advanced && cooperative.ActiveCount() == 0,
            "cancellation releases the running worker before500ms and advances the queue");
        std::cout << "cooperative cancellation: running action released; next queued action completed before500ms\n";
    }

    {
        std::promise<void> entered;
        auto started = entered.get_future();
        std::atomic<bool> observedStop{false};
        auto cooperative = std::make_unique<WidgetFilesystemTaskExecutor>(
            WidgetFilesystemTaskExecutor::Runner{}, WidgetFilesystemTaskExecutor::NowProvider{},
            [&](const WidgetFilesystemTaskRequest&, std::stop_token stop) {
                entered.set_value();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                observedStop.store(stop.stop_requested());
                return WidgetFilesystemTaskRunResult{};
            });
        WidgetFilesystemTaskRequest request;
        request.action = "filesystem.stat"; request.path = file;
        Expect(static_cast<bool>(cooperative->Start(803, "shutdown-test", request)), "shutdown action starts");
        Expect(started.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "shutdown action is running");
        const auto began = std::chrono::steady_clock::now();
        cooperative.reset();
        Expect(observedStop.load() && std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500),
            "destructor bridges stop to the active request and joins before500ms");
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

    // A completed image/file task must wake the UI instead of waiting for the
    // host's one-second maintenance timer. Draining in the callback also checks
    // that publication and notification happen outside the worker's lock.
    {
        std::promise<WidgetFilesystemTaskCompletion> result;
        auto future = result.get_future();
        WidgetFilesystemTaskExecutor notified;
        notified.SetCompletionCallback([&] {
            auto ready = notified.DrainCompletions();
            if (!ready.empty()) result.set_value(std::move(ready.front()));
        });
        Expect(static_cast<bool>(notified.Start(901, "wake-test", stat)),
            "notified filesystem task starts");
        Expect(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "completion notification must deliver without polling");
        const auto completed = future.get();
        Expect(completed.id == 901 && completed.ok && completed.metadata.size == 5,
            "the notification exposes the completed production file read");
    }

    {
        std::promise<void> entered, release;
        auto running = entered.get_future();
        auto resume = release.get_future();
        std::promise<WidgetFilesystemTaskCompletion> canceled;
        auto future = canceled.get_future();
        WidgetFilesystemTaskExecutor queued([&](const auto&) {
            entered.set_value();
            resume.wait();
            WidgetFilesystemTaskRunResult result; result.ok = true; return result;
        });
        queued.SetCompletionCallback([&] {
            for (auto& ready : queued.DrainCompletions())
                if (ready.id == 903) canceled.set_value(std::move(ready));
        });
        Expect(static_cast<bool>(queued.Start(902, "wake-test", stat)),
            "controlled filesystem task starts");
        const bool started = running.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        if (!started) release.set_value();
        Expect(started, "controlled filesystem task reaches its runner");
        const bool queuedOk = static_cast<bool>(queued.Start(903, "wake-test", stat));
        const bool canceledOk = queued.Cancel(903);
        release.set_value();
        Expect(queuedOk && canceledOk, "queued navigation can be canceled");
        Expect(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "cancellation before execution must also wake the host");
        const auto completed = future.get();
        Expect(!completed.ok && completed.error == "canceled",
            "the queued cancellation wake carries a canceled result");
    }

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

    // Real WIC decoding through the production worker must preserve these
    // independently specified red/green pixels, not just accept an extension.
    const auto photo = root / L"photo.bmp";
    {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + 8;
        BITMAPINFOHEADER bitmap{};
        bitmap.biSize = sizeof(bitmap);
        bitmap.biWidth = 2;
        bitmap.biHeight = 1;
        bitmap.biPlanes = 1;
        bitmap.biBitCount = 24;
        bitmap.biSizeImage = 8;
        const unsigned char bytes[] = { 0, 0, 255, 0, 255, 0, 0, 0 };
        std::ofstream output(photo, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
        output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    WidgetFilesystemTaskRequest image;
    image.action = "filesystem.image";
    image.path = photo;
    image.handle = "test-owner-file";
    Expect(static_cast<bool>(executor.Start(11, "instance", image)), "image task starts");
    const auto decoded = WaitFor(executor, 11);
    const std::vector<std::uint8_t> expectedPixels{ 0, 0, 255, 255, 0, 255, 0, 255 };
    Expect(decoded.ok && decoded.image && decoded.image->width == 2 && decoded.image->height == 1 &&
            decoded.image->bgraPremultiplied == expectedPixels &&
            decoded.metadata.handle == image.handle && IsWidgetRuntimeImageToken(decoded.resourceToken),
        "real file decoding preserves pixel colors and the authorizing handle");
    image.path = root;
    image.name = "photo.bmp";
    image.handle = "test-owner-folder";
    image.maxDimension = 1;
    Expect(static_cast<bool>(executor.Start(12, "instance", image)), "folder child image starts");
    const auto scaled = WaitFor(executor, 12);
    Expect(scaled.ok && scaled.image && scaled.image->width == 1 && scaled.image->height == 1 &&
            scaled.metadata.handle == "test-owner-folder",
        "folder image preserves authorization and fits requested output bounds");
    image.name = "note.txt";
    Expect(static_cast<bool>(executor.Start(13, "instance", image)), "nonimage decode starts");
    const auto corrupt = WaitFor(executor, 13);
    Expect(!corrupt.ok && corrupt.error == "imageDecodeFailed" && !corrupt.image,
        "unsupported content returns a decode failure without image pixels");
    for (const char* name : { "../photo.bmp", "..\\photo.bmp", "C:\\photo.bmp",
            "photo.bmp:secret", "sub/photo.bmp", ".", "..", "photo.bmp ", "photo.bmp." })
    {
        image.name = name;
        Expect(!WidgetFilesystemTaskExecutor::ValidateRequest(image),
            "image requests reject traversal, absolute paths, aliases, and alternate streams");
    }
    image.name = "photo.bmp";
    image.maxDimension = 2049;
    Expect(!WidgetFilesystemTaskExecutor::ValidateRequest(image), "image output ceiling is enforced");
    image.maxDimension = 0;
    Expect(!WidgetFilesystemTaskExecutor::ValidateRequest(image), "zero image size is rejected");
    list.limit = 100;
    list.grantHandles = false;
    Expect(static_cast<bool>(executor.Start(14, "instance", list)), "names-only list starts");
    const auto names = WaitFor(executor, 14);
    Expect(names.ok && !names.grantHandles && !names.items.empty() && names.items[0].handle.empty(),
        "names-only enumeration reaches the completion without child grants");
    const std::u8string unicodeName = u8"r\u00e9sum\u00e9-\u56fe\u50cf.bmp";
    std::filesystem::copy_file(photo, root / std::filesystem::path(unicodeName));
    image.path = root;
    image.name.assign(reinterpret_cast<const char*>(unicodeName.data()), unicodeName.size());
    image.maxDimension = 2;
    Expect(static_cast<bool>(executor.Start(30, "unicode-image", image)), "UTF-8 child image starts");
    const auto unicodeImage = WaitFor(executor, 30);
    Expect(unicodeImage.ok && unicodeImage.image &&
            unicodeImage.image->bgraPremultiplied == expectedPixels &&
            unicodeImage.metadata.handle == "test-owner-folder",
        "non-ASCII child filename preserves actual decoded pixels and folder authorization");

    const auto chunkedFile = root / L"chunked.txt";
    const std::string chunkedText = std::string(16 * 1024 - 1, 'a') +
        "\xe2\x82\xac" + std::string(20 * 1024, 'b');
    { std::ofstream output(chunkedFile, std::ios::binary); output.write(chunkedText.data(),
          static_cast<std::streamsize>(chunkedText.size())); }
    read.path = chunkedFile;
    read.maxBytes = chunkedText.size();
    Expect(static_cast<bool>(executor.Start(31, "chunked-read", read)), "exact-ceiling chunked UTF-8 read starts");
    const auto chunkedResult = WaitFor(executor, 31);
    Expect(chunkedResult.ok && chunkedResult.text == chunkedText,
        "multi-byte UTF-8 spanning a chunk boundary round-trips at the exact caller byte ceiling");
    std::cout << "widget filesystem task executor tests passed\n";
    return 0;
}
