#pragma once

#include "shell_file_operation_worker.h"
#include <shobjidl.h>
#include <wrl/implements.h>
#include <filesystem>
#include <algorithm>

namespace snowdesktop
{
// Per-item sink, so recursive child notifications cannot claim another drop's
// landing. PostCopy/MoveItem supplies the actual collision-renamed output.
class DropFileProgress final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IFileOperationProgressSink>
{
public:
    std::wstring source;
    std::shared_ptr<ShellFileOperationResult> result;
    bool completed = false;

    HRESULT Record(HRESULT status, IShellItem* original, IShellItem* created)
    {
        // Move success can be COPYENGINE_S_DONT_PROCESS_CHILDREN, not S_OK.
        if (FAILED(status) || !original || !created || completed) return S_OK;
        PWSTR originalPath = nullptr;
        const HRESULT originalStatus = original->GetDisplayName(SIGDN_FILESYSPATH, &originalPath);
        const bool requestedItem = SUCCEEDED(originalStatus) && originalPath &&
            _wcsicmp(originalPath, source.c_str()) == 0;
        CoTaskMemFree(originalPath);
        if (!requestedItem) return S_OK;
        PWSTR path = nullptr;
        if (SUCCEEDED(created->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
        {
            result->outputs.push_back({source, path, false});
            completed = true;
        }
        CoTaskMemFree(path);
        return S_OK;
    }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* original, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override { return Record(hr, original, item); }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* original, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override { return Record(hr, original, item); }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};

inline bool ExecuteTrackedDropStep(const ShellFileOperationStep& step,
    const std::shared_ptr<ShellFileOperationResult>& result)
{
    using namespace Microsoft::WRL;
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&operation))) ||
        FAILED(operation->SetOperationFlags(step.flags))) return false;

    auto folder = std::filesystem::path(step.destination);
    std::wstring name;
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        // Exact copy names are used only for a single desktop duplicate.
        if (step.sources.size() != 1) return false;
        name = folder.filename().wstring();
        folder = folder.parent_path();
    }
    ComPtr<IShellItem> destination;
    if (FAILED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
            IID_PPV_ARGS(&destination)))) return false;

    bool scheduledAll = true;
    std::vector<ComPtr<DropFileProgress>> sinks;
    for (const auto& path : step.sources)
    {
        ComPtr<IShellItem> source;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                IID_PPV_ARGS(&source))))
        {
            scheduledAll = false;
            continue;
        }
        auto sink = Make<DropFileProgress>();
        if (!sink) { scheduledAll = false; continue; }
        sink->source = path;
        sink->result = result;
        const auto newName = name.empty() ? nullptr : name.c_str();
        const HRESULT queued = step.function == FO_MOVE
            ? operation->MoveItem(source.Get(), destination.Get(), newName, sink.Get())
            : operation->CopyItem(source.Get(), destination.Get(), newName, sink.Get());
        if (FAILED(queued)) scheduledAll = false;
        else sinks.push_back(std::move(sink));
    }
    if (sinks.empty()) return false;
    const HRESULT performed = operation->PerformOperations();
    BOOL aborted = TRUE;
    const HRESULT queried = operation->GetAnyOperationsAborted(&aborted);
    return SUCCEEDED(performed) && SUCCEEDED(queried) && !aborted && scheduledAll &&
        std::all_of(sinks.begin(), sinks.end(), [](const auto& sink) { return sink->completed; });
}
}
