#pragma once

#include "external_drop_content.h"
#include "ole_async_operation.h"
#include "shell/shell_file_operation_completion.h"
#include "shell/shell_file_operation_worker.h"
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
    Completion completed;
};

class MarshaledSource
{
public:
    Microsoft::WRL::ComPtr<IStream> stream;
    ~MarshaledSource() { Take(); }
    Microsoft::WRL::ComPtr<IDataObject> Take()
    {
        Microsoft::WRL::ComPtr<IDataObject> source;
        if (stream)
            CoGetInterfaceAndReleaseStream(stream.Detach(), IID_PPV_ARGS(&source));
        return source;
    }
};

// This is the host's transfer coordinator: marshal -> worker read -> posted UI
// completion -> destination commit -> EndOperation. Decoders, destination
// mutation and resource ownership remain the caller's production services.
inline DWORD Transfer(IDataObject* source, DWORD effect, bool asynchronous,
    const Paths& knownPaths, std::shared_ptr<Content> value,
    ShellFileOperationWorker& reader, std::stop_token stop,
    HWND window, UINT message, Callbacks callbacks)
{
    if (!source) return DROPEFFECT_NONE;
    Completion oleCompletion;
    auto marshaled = std::make_shared<MarshaledSource>();
    if (asynchronous)
    {
        if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IDataObject,
                source, &marshaled->stream)) ||
            !ole_async_operation::Prepare(source,
                effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect, {}, oleCompletion))
            return DROPEFFECT_NONE;
    }
    else
    {
        try { *value = callbacks.read(source, false, knownPaths); }
        catch (...) { return DROPEFFECT_NONE; }
    }
    const auto completedOnce = std::make_shared<std::atomic_bool>(false);
    auto finished = [value, oleCompletion, completedOnce, completed = callbacks.completed](bool succeeded) {
        if (completedOnce->exchange(true)) return;
        if (completed) completed(succeeded);
        if (oleCompletion) oleCompletion(succeeded);
    };
    if (!asynchronous && value->pendingUrls.empty())
    {
        const bool committed = callbacks.commit(*value, finished, true);
        if (!committed) finished(false);
        return committed ? (effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect) : DROPEFFECT_NONE;
    }

    if (!window || !IsWindow(window)) return DROPEFFECT_NONE;
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [value, finished, commit = callbacks.commit](bool succeeded) {
            if (!succeeded || !commit(*value, finished, false)) finished(false);
        }, false};
    if (!completion) return DROPEFFECT_NONE;
    const bool queued = reader.Enqueue(ShellReadRequest{
        [asynchronous, marshaled, value, callbacks = std::move(callbacks), stop] {
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
                return !value->paths.empty();
            }
            catch (...) { return false; }
        }}, [window, message, completion](bool succeeded) {
            PostShellFileOperationCompletion(window, message,
                std::unique_ptr<ShellFileOperationUiCompletion>(completion), succeeded);
        });
    if (!queued) delete completion;
    return queued ? effect : DROPEFFECT_NONE;
}
}
