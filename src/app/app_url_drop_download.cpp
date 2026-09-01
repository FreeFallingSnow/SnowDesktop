#include "app.h"

#include <new>
#include <utility>

// URL-only network resources are identified and staged off the OLE/UI thread.

bool DesktopApp::QueueUrlDropDownload(
    std::wstring url, DropPreviewList preview)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) ||
        url.empty() || preview.targetKind != DropTargetKind::Desktop)
        return false;

    const std::filesystem::path destinationDirectory(
        GetDataSubdirectoryPath(L"DropContent"));
    const DWORD attributes = GetFileAttributesW(
        destinationDirectory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;

    auto* completion = new (std::nothrow)
        UrlDropDownloadUiCompletion{};
    if (!completion) return false;
    completion->preview = std::move(preview);

    snowdesktop::UrlDropDownloadRequest request;
    request.url = std::move(url);
    request.destinationDirectory = destinationDirectory;
    request.maximumBytes = 64ull * 1024ull * 1024ull;
    request.timeoutMs = 10000;
    const bool queued = urlDropDownloadWorker_.Enqueue(
        std::move(request),
        [completionWindow, completion](
            snowdesktop::UrlDropDownloadResult result) {
            completion->result = std::move(result);
            if (!PostMessageW(completionWindow,
                    kUrlDropDownloadCompletedMessage, 0,
                    reinterpret_cast<LPARAM>(completion)))
                delete completion;
        });
    if (!queued)
        delete completion;
    return queued;
}

void DesktopApp::OnUrlDropDownloadCompleted(LPARAM lParam)
{
    std::unique_ptr<UrlDropDownloadUiCompletion> completion(
        reinterpret_cast<UrlDropDownloadUiCompletion*>(lParam));
    if (!completion || exitRequested_) return;

    std::wstring stagedPath;
    if (completion->result.outcome ==
            snowdesktop::UrlDropDownloadOutcome::Downloaded)
        stagedPath = completion->result.localPath;
    else
        stagedPath = CreateUrlShortcut(
            completion->result.originalUrl);
    if (stagedPath.empty()) return;

    DragSourceList sourceList;
    sourceList.hasExternalFiles = true;
    DragSourceEntry entry;
    entry.kind = DropSourceKind::ExternalFile;
    entry.sourceIndex = 0;
    entry.filePath = stagedPath;
    entry.displayName = FileNameFromPath(stagedPath);
    entry.originalSpan = {1, 1};
    sourceList.entries.push_back(std::move(entry));

    const bool executed = ExecuteDropPipeline(
        sourceList, completion->preview, {}, false);
    if (!executed)
        DeleteFileW(stagedPath.c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::StopUrlDropDownloadWorker()
{
    urlDropDownloadWorker_.Stop();

    const HWND windows[] = { controlHwnd_, hwnd_ };
    HWND previous = nullptr;
    for (const HWND window : windows)
    {
        if (!window || !IsWindow(window) || window == previous)
            continue;
        previous = window;
        MSG message{};
        while (PeekMessageW(&message, window,
            kUrlDropDownloadCompletedMessage,
            kUrlDropDownloadCompletedMessage, PM_REMOVE))
        {
            delete reinterpret_cast<UrlDropDownloadUiCompletion*>(
                message.lParam);
        }
    }
}
