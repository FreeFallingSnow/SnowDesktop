#pragma once

#include <windows.h>
#include <oleidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace snowdesktop
{

/** @brief One path-based Shell operation executed by the worker. */
struct ShellFileOperationStep
{
    UINT function = 0;
    std::vector<std::wstring> sources;
    std::wstring destination;
    FILEOP_FLAGS flags = 0;
};

/** @brief One Shell-link creation executed by the worker STA. */
struct ShellShortcutOperationStep
{
    std::wstring source;
    std::wstring destination;
    std::wstring workingDirectory;
    // Atomically claim a previously selected path before saving. A collision
    // fails instead of overwriting a shortcut created by an earlier request.
    bool failIfDestinationExists = false;
};

/** @brief One exact-path file copy that must not rename or overwrite. */
struct ShellExactFileCopyOperationStep
{
    std::wstring source;
    std::wstring destination;
};

struct ShellFileOperationRequest
{
    std::vector<ShellFileOperationStep> steps;
    std::vector<ShellExactFileCopyOperationStep> exactFileCopies;
    std::vector<ShellShortcutOperationStep> shortcuts;
};

/** A rename owns only copied paths/PIDL bytes, never UI-thread COM objects. */
struct ShellRenameRequest
{
    std::wstring sourcePath;
    std::wstring newName;
    std::vector<BYTE> desktopChildId;
};

struct ShellRenameResult
{
    HRESULT status = E_ABORT;
    std::wstring sourcePath;
    std::wstring path;
    std::wstring displayName;
    std::wstring typeName;
    std::vector<BYTE> absoluteId;
    std::vector<BYTE> desktopChildId;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    int sysIconIndex = -1;
    bool metadataComplete = false;
    ULONGLONG elapsedMs = 0;
};

/** Value-only metadata capture on the worker STA; must not access UI state. */
struct ShellReadRequest
{
    std::function<bool()> read;
};

/**
 * @brief Build a recoverable delete request for path-backed Recycle Bin drops.
 *
 * The permanent-delete warning remains enabled for paths that the Shell cannot
 * place in the Recycle Bin (for example, unsupported volumes).
 */
ShellFileOperationRequest CreateRecycleBinDeleteRequest(
    std::vector<std::wstring> sources);

/** @brief Path-backed Shell IDropTarget handoff executed on the worker STA. */
struct ShellDropRequest
{
    std::vector<std::wstring> sources;
    // External OLE sources that opt into asynchronous extraction are
    // marshaled instead of flattened to paths, preserving every Shell format
    // and the source's IDataObjectAsyncCapability feedback channel.
    Microsoft::WRL::ComPtr<IStream> marshaledDataObject;
    Microsoft::WRL::ComPtr<IStream> marshaledAsyncCapability;
    std::wstring targetParsingName;
    DWORD keyState = 0;
    POINTL screenPoint{};
    DWORD allowedEffects = DROPEFFECT_COPY | DROPEFFECT_MOVE |
        DROPEFFECT_LINK;
    // Optional bounded materialization performed on this worker after the
    // caller's StartOperation and before Shell/EndOperation. Returning true
    // means the preflight fully handled the data object and Shell is skipped.
    std::function<bool(IDataObject*)> dataObjectPreflight;
};

/**
 * @brief Serial STA worker for Shell drops and file/link operations.
 *
 * SHFileOperationW owns and pumps its progress UI on this worker thread. It is
 * deliberately called without a desktop-window owner so a modal progress or
 * confirmation window cannot disable SnowDesktop's full-screen input surface.
 */
class ShellFileOperationWorker
{
public:
    using Completion = std::function<void(bool)>;
    using RenameCompletion = std::function<void(ShellRenameResult)>;

    ShellFileOperationWorker() = default;
    ~ShellFileOperationWorker();

    ShellFileOperationWorker(const ShellFileOperationWorker&) = delete;
    ShellFileOperationWorker& operator=(const ShellFileOperationWorker&) = delete;

    bool Enqueue(ShellFileOperationRequest request, Completion completion);
    bool Enqueue(ShellDropRequest request, Completion completion);
    bool Enqueue(ShellRenameRequest request, RenameCompletion completion);
    bool Enqueue(ShellReadRequest request, Completion completion);
    void Stop();

    /** @brief Execute a request synchronously on the calling STA. */
    static bool Execute(const ShellFileOperationRequest& request);
    /** @brief Execute a path-backed IDropTarget handoff on the calling STA. */
    static bool Execute(ShellDropRequest request);
    static ShellRenameResult Execute(const ShellRenameRequest& request);

private:
    struct Task
    {
        std::variant<ShellFileOperationRequest, ShellDropRequest,
            ShellRenameRequest, ShellReadRequest> request;
        Completion completion;
        RenameCompletion renameCompletion;
    };

    void Run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool started_ = false;
    bool stopping_ = false;
};

} // namespace snowdesktop
