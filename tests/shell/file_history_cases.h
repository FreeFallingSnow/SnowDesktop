#pragma once

#include "shell/shell_file_history.h"
#include "shell/shell_file_operation_worker.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace file_history_cases
{
inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
inline bool Apply(snowdesktop::ShellFileOperationWorker& worker, bool redo)
{
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    auto result = std::make_shared<snowdesktop::file_history::Result>();
    Require(worker.EnqueueHistory(redo, result,
        [done](bool succeeded) { done->set_value(succeeded); }), "file history request is queued");
    Require(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready, "file history has bounded completion");
    const bool succeeded = future.get();
    if (!succeeded) std::wcerr << L"history " << (redo ? L"redo" : L"undo") << L" status=0x"
        << std::hex << static_cast<unsigned long>(result->status) << std::dec << L" path=" << result->failedPath << L'\n';
    return succeeded;
}
inline snowdesktop::ShellRenameResult Rename(snowdesktop::ShellFileOperationWorker& worker,
    const std::filesystem::path& source, const wchar_t* name)
{
    auto done = std::make_shared<std::promise<snowdesktop::ShellRenameResult>>();
    auto future = done->get_future();
    Require(worker.Enqueue(snowdesktop::ShellRenameRequest{source.wstring(), name, {}},
        [done](auto result) { done->set_value(std::move(result)); }), "production rename is queued");
    Require(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready, "rename has bounded completion");
    auto result = future.get();
    Require(SUCCEEDED(result.status), "production rename succeeds before undo");
    return result;
}
inline bool Delete(snowdesktop::ShellFileOperationWorker& worker, std::vector<std::wstring> paths)
{
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    auto request = snowdesktop::CreateRecycleBinDeleteRequest(std::move(paths));
    request.steps.front().flags |= FOF_SILENT | FOF_NOERRORUI;
    Require(worker.Enqueue(std::move(request), [done](bool ok) { done->set_value(ok); }), "production recycle request is queued");
    Require(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready, "recycling has bounded completion");
    return future.get();
}
inline std::string Contents(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
inline void Run(const std::filesystem::path& parent)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "history fixtures own an STA");
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    const auto root = parent / L"file-history";
    std::filesystem::create_directories(root);
    snowdesktop::ShellFileOperationWorker worker;
    const auto original = root / L"original.txt";
    const auto renamed = root / L"renamed.txt";
    std::ofstream(original) << "unchanged file contents";
    Rename(worker, original, L"renamed.txt");
    Require(worker.CanUndo() && !worker.CanRedo(), "successful queued rename publishes undo availability");
    Require(Apply(worker, false) && std::filesystem::exists(original) && !std::filesystem::exists(renamed),
        "Issue 32: production worker undo restores the original filename");
    Require(worker.CanRedo() && Apply(worker, true) && Contents(renamed) == "unchanged file contents",
        "redo reapplies the exact rename without changing contents");
    std::ofstream(original) << "new unrelated file";
    Require(!Apply(worker, false) && Contents(original) == "new unrelated file" && std::filesystem::exists(renamed),
        "undo refuses to overwrite an occupied original path");
    std::filesystem::remove(original);
    Require(Apply(worker, false), "a collision failure leaves undo retryable");
    Rename(worker, original, L"different.txt");
    Require(!worker.CanRedo(), "a new rename invalidates the redo branch");
    Require(Apply(worker, false), "new rename is undone before older history");
    // Retain the old file elsewhere, preventing file-ID reuse from weakening
    // this replacement test. The same filename is insufficient authority.
    std::filesystem::rename(original, root / L"retained.txt");
    std::ofstream(original) << "replacement must survive";
    Require(!Apply(worker, true) && Contents(original) == "replacement must survive",
        "redo never renames a replacement file at the same path");
    auto copied = std::make_shared<std::promise<bool>>();
    auto copyDone = copied->get_future();
    snowdesktop::ShellFileOperationRequest copy;
    copy.exactFileCopies.push_back({original.wstring(), (root / L"copied.txt").wstring()});
    Require(worker.Enqueue(std::move(copy), [copied](bool ok) { copied->set_value(ok); }), "untracked copy is queued");
    Require(copyDone.wait_for(std::chrono::seconds(20)) == std::future_status::ready && copyDone.get() &&
        !worker.CanUndo() && !worker.CanRedo(), "untracked mutation is a barrier rather than undoing an older unrelated rename");
    const auto unicode = Rename(worker, original, L"文档-测试.txt");
    Require(Apply(worker, false) && Apply(worker, true) && Contents(unicode.path) == "replacement must survive",
        "Unicode names survive undo and redo");
    worker.Stop();

    snowdesktop::ShellFileOperationWorker deletion;
    const auto one = root / L"delete-one.txt";
    const auto two = root / L"delete-folder";
    std::ofstream(one) << "recover exactly this file";
    std::filesystem::create_directory(two);
    std::ofstream(two / L"child.txt") << "nested contents";
    try
    {
        Require(Delete(deletion, {one.wstring(), two.wstring()}), "file and nonempty folder enter the Recycle Bin");
        Require(!std::filesystem::exists(one) && !std::filesystem::exists(two) && deletion.CanUndo(),
            "recycle completion records an undoable group");
        Require(Apply(deletion, false) && Contents(one) == "recover exactly this file" &&
            Contents(two / L"child.txt") == "nested contents", "whole-group undo restores files and nonempty directories");
        Require(Apply(deletion, true) && !std::filesystem::exists(one) && !std::filesystem::exists(two),
            "whole-group redo recycles the same files and nonempty directories");
        std::ofstream(one) << "occupied restore destination";
        Require(!Apply(deletion, false) && Contents(one) == "occupied restore destination" &&
            Contents(two / L"child.txt") == "nested contents", "partial undo restores only unblocked members");
        std::filesystem::remove(one);
        Require(Apply(deletion, false) && Contents(one) == "recover exactly this file", "retry restores only the remaining exact recycle item");
        Require(Apply(deletion, true) && !std::filesystem::exists(one), "redo recycles the successfully restored file again");
        Require(Apply(deletion, false) && Contents(one) == "recover exactly this file", "repeated delete undo uses the new recycle identity");
    }
    catch (...)
    {
        // Only this test's temporary paths are touched. Attempt exact restores
        // before the outer fixture removes its directory on failure.
        if (Contents(one) == "occupied restore destination") std::filesystem::remove(one);
        if (deletion.CanUndo()) Apply(deletion, false);
        deletion.Stop();
        throw;
    }
    deletion.Stop();

    snowdesktop::ShellFileOperationWorker replacedDeletion;
    const auto replacement = root / L"redo-delete.txt";
    std::ofstream(replacement) << "the restored object";
    Require(Delete(replacedDeletion, {replacement.wstring()}) && Apply(replacedDeletion, false),
        "delete redo replacement test first restores its own file");
    std::filesystem::rename(replacement, root / L"retained-delete.txt");
    std::ofstream(replacement) << "unrelated replacement";
    Require(!Apply(replacedDeletion, true) && Contents(replacement) == "unrelated replacement",
        "delete redo refuses a different file with the same name");
    replacedDeletion.Stop();

    const auto noParent = root / L"restore-parent";
    std::filesystem::create_directory(noParent);
    const auto child = noParent / L"child.txt";
    std::ofstream(child) << "do not invent parent directories";
    snowdesktop::file_history::Group captured;
    Require(SUCCEEDED(snowdesktop::file_history::Recycle({child.wstring()},
        FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION, captured)) && captured.size() == 1,
        "Shell supplies an exact recycle identity for the missing-parent scenario");
    std::filesystem::remove(noParent);
    const HRESULT missingParent = snowdesktop::file_history::Restore(captured.front());
    const bool inventedParent = std::filesystem::exists(noParent);
    // Restore this fixture before checking the failure, including on a failing
    // candidate. No deleted temporary file is intentionally left in the bin.
    std::filesystem::create_directory(noParent);
    auto staleRecycle = captured.front();
    Require(SUCCEEDED(snowdesktop::file_history::Restore(captured.front())), "missing-parent fixture is recovered for cleanup");
    Require(FAILED(missingParent) && !inventedParent, "undo does not recreate a removed destination directory");
    Require(FAILED(snowdesktop::file_history::Restore(staleRecycle)), "an externally restored recycle identity becomes unavailable");

    // A disappeared recycle item must never be replaced by a similarly named
    // one. The restore helper only accepts its captured item identity.
    snowdesktop::file_history::Entry absent{snowdesktop::file_history::Entry::Kind::Delete,
        (root / L"missing-original.txt").wstring(), (root / L"missing-recycled.txt").wstring(), {}, {}};
    Require(FAILED(snowdesktop::file_history::Restore(absent)) && !std::filesystem::exists(absent.original),
        "missing recycle identity is rejected without recreating data or directories");
}
}
