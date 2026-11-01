#pragma once

#include "shell_file_operation_worker.h"
#include "drag_drop/external_transfer_observation.h"
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
    // Value-only identity for an enabled diagnostic session; no COM object
    // or file handle is acquired by this observation.
    std::uint64_t observationTask = 0, observationCall = 0;
    bool observationMove = false;

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
        const HRESULT compared = sourceItem ? sourceItem->Compare(original, SICHINT_CANONICAL, &order) : E_POINTER;
        if (observationCall)
        {
            const auto object = reinterpret_cast<std::uintptr_t>(this);
            external_transfer_observation::Record(observationTask, observationMove
                ? external_transfer_observation::Phase::tracked_move_post_compare_status
                : external_transfer_observation::Phase::tracked_copy_post_compare_status,
                object, compared, observationCall);
            external_transfer_observation::Record(observationTask, observationMove
                ? external_transfer_observation::Phase::tracked_move_post_compare_order
                : external_transfer_observation::Phase::tracked_copy_post_compare_order,
                object, order, observationCall);
        }
        const bool requestedItem = sourceItem && SUCCEEDED(compared) && order == 0;
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
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* original, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override
    {
        const auto object = reinterpret_cast<std::uintptr_t>(this);
        if (observationCall && observationMove)
        {
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_move_post_enter,
                object, hr, observationCall);
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_move_post_source,
                object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(original)), observationCall);
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_move_post_created,
                object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(item)), observationCall);
        }
        const HRESULT returned = Record(hr, original, item);
        if (observationCall && observationMove)
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_move_post_return,
                object, returned, observationCall);
        return returned;
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* original, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override
    {
        const auto object = reinterpret_cast<std::uintptr_t>(this);
        if (observationCall)
        {
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_copy_post_enter,
                object, hr, observationCall);
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_copy_post_source,
                object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(original)), observationCall);
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_copy_post_created,
                object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(item)), observationCall);
        }
        const HRESULT returned = Record(hr, original, item);
        if (observationCall)
            external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::tracked_copy_post_return,
                object, returned, observationCall);
        return returned;
    }
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
    // No-error-UI otherwise means Skip/Continue to Shell. Stop its remaining
    // operations at the first failure; already committed outputs stay recorded.
    failure = operation->SetOperationFlags(static_cast<DWORD>(step.flags) |
        ((step.flags & FOF_NOERRORUI) ? FOFX_EARLYFAILURE : 0u));
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
            break;
        }
        auto sink = Make<DropFileProgress>();
        if (!sink) { failure = E_OUTOFMEMORY; scheduledAll = false; break; }
        sink->source = path;
        sink->sourceItem = source;
        sink->result = result;
        if ((step.function == FO_COPY || step.function == FO_MOVE) && external_transfer_observation::currentTask &&
            external_transfer_observation::currentWork && external_transfer_observation::Enabled())
        {
            const DWORD saved = GetLastError();
            sink->observationTask = external_transfer_observation::currentTask;
            sink->observationMove = step.function == FO_MOVE;
            sink->observationCall = external_transfer_observation::NewTask();
            if (sink->observationCall)
            {
                const auto object = reinterpret_cast<std::uintptr_t>(sink.Get());
                external_transfer_observation::Record(sink->observationTask, sink->observationMove
                    ? external_transfer_observation::Phase::tracked_move_item_source : external_transfer_observation::Phase::tracked_copy_item_source,
                    object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(source.Get())), sink->observationCall);
                external_transfer_observation::Record(sink->observationTask, sink->observationMove
                    ? external_transfer_observation::Phase::tracked_move_item_result : external_transfer_observation::Phase::tracked_copy_item_result,
                    object, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(result.get())), sink->observationCall);
                const auto bytes = path.size() <= external_transfer_observation::BlobLimit() / sizeof(wchar_t)
                    ? path.size() * sizeof(wchar_t) : external_transfer_observation::BlobLimit() + 1;
                external_transfer_observation::RecordTaskBlob(sink->observationTask,
                    sink->observationMove ? external_transfer_observation::Phase::tracked_move_item_path
                        : external_transfer_observation::Phase::tracked_copy_item_path, object, sink->observationCall,
                    path.data(), bytes);
            }
            SetLastError(saved);
        }
        const auto newName = name.empty() ? nullptr : name.c_str();
        const HRESULT queued = step.function == FO_MOVE
            ? operation->MoveItem(source.Get(), destination.Get(), newName, sink.Get())
            : operation->CopyItem(source.Get(), destination.Get(), newName, sink.Get());
        if (sink->observationCall)
            external_transfer_observation::Record(sink->observationTask, sink->observationMove
                ? external_transfer_observation::Phase::tracked_move_queue_return : external_transfer_observation::Phase::tracked_copy_queue_return,
                reinterpret_cast<std::uintptr_t>(sink.Get()), queued, sink->observationCall);
        if (FAILED(queued)) { failure = queued; scheduledAll = false; break; }
        else sinks.push_back(std::move(sink));
    }
    // Nothing has entered filesystem execution if preparation already failed.
    // Releasing the operation leaves every queued-but-unstarted source intact.
    if (sinks.empty() || !scheduledAll) return false;
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
