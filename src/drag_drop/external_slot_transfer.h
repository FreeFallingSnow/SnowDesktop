#pragma once

#include "external_drop_content.h"
#include "ole_async_operation.h"
#include "shell/shell_file_operation_completion.h"
#include "shell/shell_file_operation_worker.h"
#include <exception>
#include <new>
#include <stop_token>

namespace snowdesktop::external_slot_transfer
{
using Content = external_drop_content::Content;
using Paths = external_drop_content::Paths;
using Completion = ShellFileOperationWorker::Completion;

struct Callbacks
{
    std::function<Content(IDataObject*, bool, const Paths&)> read;
    std::function<Content(const Paths&)> resolveUrls;
    // The commit owns its destination snapshot. It must complete after the
    // actual file/model operation, not when that operation is merely queued.
    std::function<bool(const Content&, Completion, bool)> commit;
    // A default verb may be known only after delayed file paths are read.
    // Report the actual committed verb when ending the existing source lease.
    std::function<DWORD()> committedEffect;
    Completion completed;
};

class MarshaledSource
{
public:
    Microsoft::WRL::ComPtr<IStream> stream;
    std::uint64_t observationTask = 0;
    std::uint64_t observationWork = 0;
    ~MarshaledSource() { Take(); }
    Microsoft::WRL::ComPtr<IDataObject> Take()
    {
        Microsoft::WRL::ComPtr<IDataObject> source;
        if (stream)
        {
            const auto object = reinterpret_cast<std::uintptr_t>(this);
            const auto streamIdentity = reinterpret_cast<std::uintptr_t>(stream.Get());
            external_transfer_observation::Record(observationTask,
                external_transfer_observation::Phase::source_unmarshal_enter, object, 0, streamIdentity);
            const HRESULT result = CoGetInterfaceAndReleaseStream(stream.Detach(), IID_PPV_ARGS(&source));
            external_transfer_observation::Record(observationTask,
                external_transfer_observation::Phase::source_unmarshal_return, object,
                static_cast<std::int64_t>(result), reinterpret_cast<std::uintptr_t>(source.Get()));
        }
        return source;
    }
};

// This is the host's transfer coordinator: marshal -> worker read -> posted UI
// completion -> destination commit -> EndOperation. Decoders, destination
// mutation and resource ownership remain the caller's production services.
inline DWORD Transfer(IDataObject* source, DWORD effect, bool asynchronous,
    const Paths& knownPaths, std::shared_ptr<Content> value,
    ShellFileOperationWorker& reader, std::stop_token stop,
    HWND window, UINT message, Callbacks callbacks,
    external_transfer_observation::AdapterScopeIdentity adapterScope = {},
    std::uint64_t observationCallsite = 0, ShellFileOperationResult* observationResult = nullptr)
{
    if (!source) return DROPEFFECT_NONE;
    const auto observationTask = external_transfer_observation::NewTask();
    external_transfer_observation::Scope observationScope(observationTask);
    external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::transfer_enter,
        reinterpret_cast<std::uintptr_t>(source), 0, 0, adapterScope);
    if (observationCallsite)
        external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::desktop_file_transfer_origin,
            reinterpret_cast<std::uintptr_t>(source), 0, observationCallsite, adapterScope);
    Completion oleCompletion;
    auto marshaled = std::make_shared<MarshaledSource>();
    marshaled->observationTask = observationTask;
    if (asynchronous)
    {
        std::function<DWORD()> resolvedEffect;
        if (callbacks.committedEffect)
            resolvedEffect = [resolve = callbacks.committedEffect] {
                const DWORD selected = resolve();
                return selected == DROPEFFECT_MOVE ? DROPEFFECT_NONE : selected;
            };
        const auto marshalIdentity = reinterpret_cast<std::uintptr_t>(marshaled.get());
        external_transfer_observation::Record(observationTask,
            external_transfer_observation::Phase::source_marshal_enter, marshalIdentity, 0,
            reinterpret_cast<std::uintptr_t>(source));
        const HRESULT marshalResult = CoMarshalInterThreadInterfaceInStream(IID_IDataObject,
                source, &marshaled->stream);
        external_transfer_observation::Record(observationTask,
            external_transfer_observation::Phase::source_marshal_return, marshalIdentity,
            static_cast<std::int64_t>(marshalResult), reinterpret_cast<std::uintptr_t>(marshaled->stream.Get()));
        if (FAILED(marshalResult) ||
            !ole_async_operation::Prepare(source,
                effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect, {}, oleCompletion, std::move(resolvedEffect)))
            return DROPEFFECT_NONE;
    }
    else
    {
        try { *value = callbacks.read(source, false, knownPaths); }
        catch (...) { return DROPEFFECT_NONE; }
    }
    const auto completedOnce = std::make_shared<std::atomic_bool>(false);
    auto finished = [value, oleCompletion, completedOnce, completed = callbacks.completed, observationTask](bool succeeded) {
        if (completedOnce->exchange(true))
        {
            if (observationTask)
                external_transfer_observation::Record(observationTask,
                    external_transfer_observation::Phase::transfer_finished_duplicate,
                    reinterpret_cast<std::uintptr_t>(completedOnce.get()), succeeded ? 1 : 0,
                    external_transfer_observation::currentWork);
            return;
        }
        const auto work = observationTask ? external_transfer_observation::currentWork : 0;
        const auto identity = observationTask ? reinterpret_cast<std::uintptr_t>(completedOnce.get()) : 0;
        if (observationTask && (external_transfer_observation::currentTask != observationTask || !work))
            external_transfer_observation::Record(observationTask,
                external_transfer_observation::Phase::transfer_finished_context_mismatch, identity, succeeded ? 1 : 0, work);
        if (observationTask)
            external_transfer_observation::Record(observationTask,
                external_transfer_observation::Phase::transfer_finished_enter, identity, succeeded ? 1 : 0, work);
        struct ObserveFinishedReturn final
        {
            std::uint64_t task, work;
            std::uintptr_t identity;
            bool succeeded;
            int uncaught;
            ~ObserveFinishedReturn() { if (task) external_transfer_observation::Record(task,
                external_transfer_observation::Phase::transfer_finished_return, identity,
                std::uncaught_exceptions() > uncaught ? -1 : succeeded ? 1 : 0, work); }
        } observeReturn{observationTask, work, identity, succeeded, observationTask ? std::uncaught_exceptions() : 0};
        if (completed) completed(succeeded);
        if (oleCompletion) oleCompletion(succeeded);
    };
    if (!asynchronous && value->pendingUrls.empty())
    {
        const bool committed = callbacks.commit(*value, finished, true);
        if (!committed) finished(false);
        const DWORD selected = callbacks.committedEffect ? callbacks.committedEffect() : effect;
        return committed ? (selected == DROPEFFECT_MOVE ? DROPEFFECT_NONE : selected) : DROPEFFECT_NONE;
    }

    if (!window || !IsWindow(window)) return DROPEFFECT_NONE;
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [value, finished, commit = callbacks.commit](bool succeeded) {
            if (!succeeded) { finished(false); return; }
            const auto task = external_transfer_observation::currentTask;
            if (task)
            {
                external_transfer_observation::Record(task,
                    external_transfer_observation::Phase::commit_content_paths,
                    reinterpret_cast<std::uintptr_t>(value.get()),
                    external_transfer_observation::ObservedSize(value->paths.size()),
                    external_transfer_observation::currentWork);
                for (std::size_t i = 0; i < value->paths.size(); ++i)
                {
                    const auto& path = value->paths[i];
                    const auto size = path.size() > external_transfer_observation::BlobLimit() / sizeof(wchar_t)
                        ? external_transfer_observation::BlobLimit() + 1 : path.size() * sizeof(wchar_t);
                    external_transfer_observation::RecordTaskBlob(task,
                        external_transfer_observation::Phase::commit_content_path_value,
                        reinterpret_cast<std::uintptr_t>(value.get()), static_cast<std::uint64_t>(i), path.data(), size);
                }
            }
            if (!commit(*value, finished, false)) finished(false);
        }, false};
    if (!completion) return DROPEFFECT_NONE;
    completion->observationTask = observationTask;
    const auto observationWork = observationTask ? external_transfer_observation::NewTask() : 0;
    if (observationTask && observationCallsite && observationWork && observationResult)
    {
        observationResult->observationTask = observationTask;
        observationResult->observationReadWork = observationWork;
        external_transfer_observation::Record(observationTask,
            external_transfer_observation::Phase::desktop_file_result_read_binding,
            reinterpret_cast<std::uintptr_t>(observationResult), 0, observationWork, adapterScope);
    }
    completion->observationWork = observationWork;
    marshaled->observationWork = observationWork;
    const auto observationCompletion = reinterpret_cast<std::uintptr_t>(completion);
    external_transfer_observation::Record(observationTask, external_transfer_observation::Phase::read_queue_enter,
        observationCompletion, 0, observationWork);
    const bool queued = reader.Enqueue(ShellReadRequest{
        [asynchronous, marshaled, value, callbacks = std::move(callbacks), stop] {
            const auto task = marshaled->observationTask;
            const auto work = marshaled->observationWork;
            // Propagate only diagnostic context through the actual read;
            // nested scopes remain ambiguous and do not establish ownership.
            external_transfer_observation::Scope observationScope(task, work);
            external_transfer_observation::Record(task, external_transfer_observation::Phase::read_worker_enter, 0, 0, work);
            struct ObserveReturn final
            {
                std::uint64_t task;
                std::uint64_t work;
                ~ObserveReturn() { external_transfer_observation::Record(task, external_transfer_observation::Phase::read_worker_return, 0, 0, work); }
            } observeReturn{task, work};
            try
            {
                if (stop.stop_requested()) return false;
                if (asynchronous)
                {
                    auto data = marshaled->Take();
                    if (!data) return false;
                    *value = callbacks.read(data.Get(), true, {});
                }
                else
                    *value = callbacks.resolveUrls(value->pendingUrls);
                if (task)
                {
                    external_transfer_observation::Record(task, external_transfer_observation::Phase::read_content_paths,
                        reinterpret_cast<std::uintptr_t>(value.get()),
                        external_transfer_observation::ObservedSize(value->paths.size()), work);
                    for (std::size_t i = 0; i < value->paths.size(); ++i)
                    {
                        const auto& path = value->paths[i];
                        const auto size = path.size() > external_transfer_observation::BlobLimit() / sizeof(wchar_t)
                            ? external_transfer_observation::BlobLimit() + 1 : path.size() * sizeof(wchar_t);
                        external_transfer_observation::RecordTaskBlob(task,
                            external_transfer_observation::Phase::read_content_path_value,
                            reinterpret_cast<std::uintptr_t>(value.get()), static_cast<std::uint64_t>(i), path.data(), size);
                    }
                }
                return !value->paths.empty();
            }
            catch (...) { return false; }
        }}, [window, message, completion](bool succeeded) {
            external_transfer_observation::Record(completion->observationTask,
                external_transfer_observation::Phase::read_completion_callback,
                reinterpret_cast<std::uintptr_t>(completion), succeeded ? 1 : 0, completion->observationWork);
            PostShellFileOperationCompletion(window, message,
                std::unique_ptr<ShellFileOperationUiCompletion>(completion), succeeded);
        }, observationWork, observationTask);
    external_transfer_observation::Record(observationTask, queued ? external_transfer_observation::Phase::read_queued :
        external_transfer_observation::Phase::read_queue_failed, observationCompletion, queued ? 1 : 0, observationWork);
    if (!queued) delete completion;
    return queued ? effect : DROPEFFECT_NONE;
}
}
