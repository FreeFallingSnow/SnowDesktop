#include "app.h"

#include <new>
#include <utility>

// URL-only network resources are identified and staged off the OLE/UI thread.

namespace
{
void DiscardDownloadedUrlDropFile(
    const snowdesktop::UrlDropDownloadResult& result)
{
    if (result.outcome ==
            snowdesktop::UrlDropDownloadOutcome::Downloaded &&
        !result.localPath.empty())
        DeleteFileW(result.localPath.c_str());
}

bool IsReplaceableInternetShortcutPath(const std::wstring& path)
{
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    return extension &&
        (_wcsicmp(extension, L".lnk") == 0 ||
         _wcsicmp(extension, L".url") == 0 ||
         _wcsicmp(extension, L".website") == 0);
}

std::wstring ReadInternetShortcutTarget(const std::wstring& path)
{
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension) return {};
    if (_wcsicmp(extension, L".url") == 0 ||
        _wcsicmp(extension, L".website") == 0)
    {
        std::array<wchar_t, 8192> target{};
        const DWORD length = GetPrivateProfileStringW(
            L"InternetShortcut", L"URL", L"",
            target.data(), static_cast<DWORD>(target.size()),
            path.c_str());
        return std::wstring(target.data(), length);
    }
    if (_wcsicmp(extension, L".lnk") != 0)
        return {};

    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) ||
        !shellLink)
        return {};
    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Load(path.c_str(), STGM_READ)))
        return {};
    std::array<wchar_t, 8192> target{};
    WIN32_FIND_DATAW findData{};
    if (FAILED(shellLink->GetPath(target.data(),
            static_cast<int>(target.size()), &findData,
            SLGP_RAWPATH)))
        return {};
    return target.data();
}

class StagedUrlDropFileLease final
{
public:
    explicit StagedUrlDropFileLease(std::wstring path)
        : path_(std::move(path))
    {
    }

    ~StagedUrlDropFileLease()
    {
        if (!path_.empty())
            DeleteFileW(path_.c_str());
    }

    StagedUrlDropFileLease(const StagedUrlDropFileLease&) = delete;
    StagedUrlDropFileLease& operator=(
        const StagedUrlDropFileLease&) = delete;

private:
    std::wstring path_;
};
}

bool DesktopApp::QueueUrlDropDownload(
    std::wstring url, DropPreviewList preview,
    std::vector<UrlDropReplacementShortcut> replacementShortcuts)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) ||
        url.empty() || preview.targetKind != DropTargetKind::Desktop ||
        replacementShortcuts.size() > 1)
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
    completion->replacementShortcuts =
        std::move(replacementShortcuts);

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
            {
                DiscardDownloadedUrlDropFile(completion->result);
                delete completion;
            }
        });
    if (!queued)
        delete completion;
    return queued;
}

void DesktopApp::OnUrlDropDownloadCompleted(LPARAM lParam)
{
    std::unique_ptr<UrlDropDownloadUiCompletion> completion(
        reinterpret_cast<UrlDropDownloadUiCompletion*>(lParam));
    if (!completion) return;
    if (exitRequested_)
    {
        DiscardDownloadedUrlDropFile(completion->result);
        return;
    }

    std::wstring stagedPath;
    if (completion->result.outcome ==
            snowdesktop::UrlDropDownloadOutcome::Downloaded)
        stagedPath = completion->result.localPath;
    else if (!completion->replacementShortcuts.empty())
    {
        ReloadItems(false);
        return;
    }
    else
        stagedPath = CreateUrlShortcut(
            completion->result.originalUrl);
    if (stagedPath.empty()) return;

    const bool downloadedResource = completion->result.outcome ==
        snowdesktop::UrlDropDownloadOutcome::Downloaded;
    if (downloadedResource &&
        !completion->replacementShortcuts.empty())
    {
        bool everyShortcutStillMatches = true;
        for (const auto& replacement :
            completion->replacementShortcuts)
        {
            if (!replacement.identityValid ||
                !IsReplaceableInternetShortcutPath(replacement.path) ||
                ReadInternetShortcutTarget(replacement.path) !=
                    completion->result.originalUrl)
            {
                everyShortcutStillMatches = false;
                break;
            }
            HANDLE shortcut = CreateFileW(
                replacement.path.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL |
                    FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (shortcut == INVALID_HANDLE_VALUE)
            {
                everyShortcutStillMatches = false;
                break;
            }
            BY_HANDLE_FILE_INFORMATION information{};
            const bool sameIdentity =
                GetFileInformationByHandle(shortcut, &information) &&
                (information.dwFileAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY |
                     FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                information.dwVolumeSerialNumber ==
                    replacement.volumeSerialNumber &&
                information.nFileIndexHigh ==
                    replacement.fileIndexHigh &&
                information.nFileIndexLow ==
                    replacement.fileIndexLow;
            CloseHandle(shortcut);
            if (!sameIdentity)
            {
                everyShortcutStillMatches = false;
                break;
            }
        }
        if (!everyShortcutStillMatches)
        {
            DiscardDownloadedUrlDropFile(completion->result);
            ReloadItems(false);
            return;
        }
    }

    DragSourceList sourceList;
    sourceList.hasExternalFiles = true;
    DragSourceEntry entry;
    entry.kind = DropSourceKind::ExternalFile;
    entry.sourceIndex = 0;
    entry.filePath = stagedPath;
    entry.displayName = FileNameFromPath(stagedPath);
    entry.originalSpan = {1, 1};
    sourceList.entries.push_back(std::move(entry));

    const std::wstring originalUrl = completion->result.originalUrl;
    const DropPreviewList requestedPreview = completion->preview;
    const auto existingDesktopKeysBeforeCopy = SnapshotDesktopKeys();
    const auto replacementShortcuts =
        completion->replacementShortcuts;
    auto stagedLease = std::make_shared<StagedUrlDropFileLease>(
        stagedPath);
    const bool executed = ExecuteDropPipeline(
        sourceList, completion->preview,
        [this, stagedLease, downloadedResource, originalUrl,
            requestedPreview, stagedPath,
            existingDesktopKeysBeforeCopy,
            replacementShortcuts](
                bool succeeded) mutable {
            if (!succeeded)
            {
                // A Shell-created link is already the recoverable fallback.
                // Leave it untouched when copying the downloaded file fails.
                if (!downloadedResource || originalUrl.empty() ||
                    !replacementShortcuts.empty())
                    return;

                const std::wstring shortcutPath =
                    CreateUrlShortcut(originalUrl);
                if (shortcutPath.empty())
                    return;
                auto shortcutLease =
                    std::make_shared<StagedUrlDropFileLease>(
                        shortcutPath);
                DragSourceList shortcutSources;
                shortcutSources.hasExternalFiles = true;
                DragSourceEntry shortcutEntry;
                shortcutEntry.kind = DropSourceKind::ExternalFile;
                shortcutEntry.sourceIndex = 0;
                shortcutEntry.filePath = shortcutPath;
                shortcutEntry.displayName =
                    FileNameFromPath(shortcutPath);
                shortcutEntry.originalSpan = {1, 1};
                shortcutSources.entries.push_back(
                    std::move(shortcutEntry));
                ExecuteDropPipeline(
                    shortcutSources, requestedPreview,
                    [shortcutLease](bool) {}, false);
                return;
            }

            if (!downloadedResource || replacementShortcuts.empty())
                return;

            bool removedEveryShortcut = true;
            for (const auto& replacement : replacementShortcuts)
            {
                // Read the target before opening a DELETE handle. Opening the
                // target through IPersistFile/GetPrivateProfileString while a
                // non-sharing DELETE handle is live can fail on Windows.
                if (!replacement.identityValid ||
                    !IsReplaceableInternetShortcutPath(replacement.path) ||
                    ReadInternetShortcutTarget(replacement.path) != originalUrl)
                {
                    removedEveryShortcut = false;
                    break;
                }
                HANDLE shortcut = CreateFileW(
                    replacement.path.c_str(),
                    DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
                if (shortcut == INVALID_HANDLE_VALUE)
                {
                    removedEveryShortcut = false;
                    break;
                }
                BY_HANDLE_FILE_INFORMATION information{};
                const bool sameIdentity =
                    GetFileInformationByHandle(shortcut, &information) &&
                    (information.dwFileAttributes &
                        (FILE_ATTRIBUTE_DIRECTORY |
                         FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                    information.dwVolumeSerialNumber ==
                        replacement.volumeSerialNumber &&
                    information.nFileIndexHigh ==
                        replacement.fileIndexHigh &&
                    information.nFileIndexLow ==
                        replacement.fileIndexLow;
                FILE_DISPOSITION_INFO disposition{ TRUE };
                const bool removed = sameIdentity &&
                    SetFileInformationByHandle(
                        shortcut, FileDispositionInfo,
                        &disposition, sizeof(disposition));
                CloseHandle(shortcut);
                if (!removed)
                {
                    removedEveryShortcut = false;
                    break;
                }
            }
            if (!removedEveryShortcut)
                return;

            // The first copy reload may have placed the resource beside the
            // still-present Shell link. Reapply the original landing after
            // deleting that link so the requested cell is available again.
            DragSourceList placementSource;
            placementSource.hasExternalFiles = true;
            DragSourceEntry placementEntry;
            placementEntry.kind = DropSourceKind::ExternalFile;
            placementEntry.sourceIndex = 0;
            placementEntry.filePath = stagedPath;
            placementEntry.displayName = FileNameFromPath(stagedPath);
            placementEntry.originalSpan = {1, 1};
            placementSource.entries.push_back(
                std::move(placementEntry));
            StorePendingLandingCache(
                placementSource, requestedPreview,
                existingDesktopKeysBeforeCopy, nullptr);
            ReloadItems(false);
        }, false);
    (void)executed;
    if (hwnd_ && IsWindow(hwnd_))
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
            auto* completion =
                reinterpret_cast<UrlDropDownloadUiCompletion*>(
                    message.lParam);
            if (completion)
                DiscardDownloadedUrlDropFile(completion->result);
            delete completion;
        }
    }
}
