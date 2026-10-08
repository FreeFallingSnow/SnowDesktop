#pragma once

#include "shell_file_operation_worker.h"
#include <shobjidl.h>
#include <sherrors.h>
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
    Microsoft::WRL::ComPtr<IShellItem> sourceItem;
    std::shared_ptr<ShellFileOperationResult> result;
    bool completed = false;
    HRESULT failure = S_OK;

    HRESULT Record(HRESULT status, IShellItem* original, IShellItem* created)
    {
        // Move success can be COPYENGINE_S_DONT_PROCESS_CHILDREN, not S_OK.
        if (status == COPYENGINE_S_USER_IGNORED)
        {
            if (SUCCEEDED(failure)) failure = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return S_OK;
        }
        if (FAILED(status)) { if (SUCCEEDED(failure)) failure = status; return S_OK; }
        if (!original || !created || completed) return S_OK;
        // Shell expands 8.3 aliases (including the user's temporary path).
        // Compare the queued item identity, while retaining the caller's path
        // in outputs for landing/cleanup. The captured item also survives moves
        // and excludes recursive child notifications without reopening a file.
        int order = 0;
        const bool requestedItem = sourceItem &&
            SUCCEEDED(sourceItem->Compare(original, SICHINT_CANONICAL, &order)) && order == 0;
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
    const std::shared_ptr<ShellFileOperationResult>& result, HRESULT& failure,
    bool& shellHandledErrors)
{
    shellHandledErrors = false;
    using namespace Microsoft::WRL;
    ComPtr<IFileOperation> operation;
    failure = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (FAILED(failure)) return false;
    failure = operation->SetOperationFlags(step.flags);
    if (FAILED(failure)) return false;

    auto folder = std::filesystem::path(step.destination);
    std::wstring name;
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        // Exact copy names are used only for a single desktop duplicate.
        if (step.sources.size() != 1) { failure = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND); return false; }
        name = folder.filename().wstring();
        folder = folder.parent_path();
    }
    ComPtr<IShellItem> destination;
    failure = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&destination));
    if (FAILED(failure)) return false;

    bool scheduledAll = true;
    std::vector<ComPtr<DropFileProgress>> sinks;
    for (const auto& path : step.sources)
    {
        ComPtr<IShellItem> source;
        const HRESULT sourceStatus = SHCreateItemFromParsingName(path.c_str(), nullptr,
                IID_PPV_ARGS(&source));
        if (FAILED(sourceStatus))
        {
            if (SUCCEEDED(failure)) failure = sourceStatus;
            scheduledAll = false;
            continue;
        }
        auto sink = Make<DropFileProgress>();
        if (!sink) { failure = E_OUTOFMEMORY; scheduledAll = false; continue; }
        sink->source = path;
        sink->sourceItem = source;
        sink->result = result;
        const auto newName = name.empty() ? nullptr : name.c_str();
        const HRESULT queued = step.function == FO_MOVE
            ? operation->MoveItem(source.Get(), destination.Get(), newName, sink.Get())
            : operation->CopyItem(source.Get(), destination.Get(), newName, sink.Get());
        if (FAILED(queued)) { failure = queued; scheduledAll = false; }
        else sinks.push_back(std::move(sink));
    }
    if (sinks.empty()) return false;
    shellHandledErrors = SUCCEEDED(failure) && !(step.flags & FOF_NOERRORUI);
    const HRESULT performed = operation->PerformOperations();
    BOOL aborted = TRUE;
    const HRESULT queried = operation->GetAnyOperationsAborted(&aborted);
    for (const auto& sink : sinks)
        if (FAILED(sink->failure) && SUCCEEDED(failure)) failure = sink->failure;
    if (SUCCEEDED(failure) && FAILED(performed)) failure = performed;
    if (SUCCEEDED(failure) && FAILED(queried)) failure = queried;
    if (SUCCEEDED(failure) && aborted) failure = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const bool succeeded = SUCCEEDED(performed) && SUCCEEDED(queried) && !aborted && scheduledAll &&
        std::all_of(sinks.begin(), sinks.end(), [](const auto& sink) { return sink->completed; });
    if (!succeeded && SUCCEEDED(failure)) failure = E_FAIL;
    return succeeded;
}
}
