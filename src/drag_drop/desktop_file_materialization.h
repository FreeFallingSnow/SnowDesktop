#pragma once

#include "core/drop_model.h"
#include "file_backed_drop_request.h"
#include "dock/dock_drop_rules.h"
#include "layout/item_location.h"
#include "shell/shell_file_operation_worker.h"
#include <shlwapi.h>
#include <functional>
#include <memory>
#include <unordered_map>

namespace snowdesktop::desktop_file_materialization
{
inline bool IsSelfContainedFolderDrop(const std::vector<std::wstring>& sourcePaths,
    const std::wstring& targetFolder)
{
    if (sourcePaths.empty() || targetFolder.empty())
        return false;

    std::vector<std::wstring> candidates;
    candidates.reserve(sourcePaths.size() * 2);
    for (const auto& path : sourcePaths)
    {
        if (path.empty())
            continue;
        candidates.push_back(path);
        // 仅对 .lnk 源做目标解析（普通目录直接用原路径比较），
        // 避免每次拖拽悬停都创建 IShellLink COM 对象。
        const wchar_t* extension =
            PathFindExtensionW(path.c_str());
        if (extension &&
            _wcsicmp(extension, L".lnk") == 0)
        {
            const snowdesktop::item_location::FolderTarget resolved =
                snowdesktop::item_location::
                    ResolveFolderTarget(path);
            if (!resolved.path.empty())
                candidates.push_back(
                    resolved.path);
        }
    }
    return snowdesktop::folder_self_drop_rules::
        IsSelfContainedFolderDrop(candidates, targetFolder);
}

using Completion = std::function<void(bool)>;
struct Services
{
    std::function<bool(const std::wstring&)> isStaging;
    std::function<bool(const std::wstring&, const std::wstring&)> pathsEqual;
    std::function<std::wstring(std::wstring)> normalize;
    std::function<std::wstring(const std::wstring&, const std::wstring&, int)> copyName;
    std::wstring shortcutName;
    std::function<bool(ShellFileOperationRequest, Completion)> enqueue;
};

// Desktop location, localized names and the UI queue are host services. Actual
// request construction, Shell IO and in-flight shortcut reservations are shared.
inline bool Materialize(const DragSourceList& sourceList, DropAction action,
    bool duplicateDesktopCopyNames, const std::wstring& desktopPath,
    dock_drop_rules::MaterializedPathReservations& reservations, const Services& services,
    std::unordered_map<size_t, std::wstring>* createdPathsBySource,
    Completion completion, bool executeSynchronously,
    std::shared_ptr<ShellFileOperationResult> result)
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty()) return false;
    // A Dock link cannot point to an OLE scratch file that is about to expire.
    if (action == DropAction::Link && std::any_of(paths.begin(), paths.end(),
            services.isStaging)) action = DropAction::Copy;
    if (createdPathsBySource)
        createdPathsBySource->clear();

    auto sameParentAsDesktop = [&](const std::wstring& path) -> bool {
        return services.pathsEqual(
            std::filesystem::path(path).parent_path().wstring(), desktopPath);
    };

    std::unordered_set<std::wstring> reservedDestinations;
    auto materializedPathReservations =
        std::make_shared<std::vector<std::wstring>>();
    auto makeUniqueCopyPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        DWORD attrs = GetFileAttributesW(path.c_str());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        std::wstring stem = fileName ? fileName : L"";
        std::wstring ext;
        if (!isDir)
        {
            wchar_t stemBuf[MAX_PATH]{};
            wcscpy_s(stemBuf, stem.c_str());
            PathRemoveExtensionW(stemBuf);
            stem = stemBuf;
            const wchar_t* extPtr = PathFindExtensionW(fileName);
            ext = extPtr ? extPtr : L"";
        }

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = services.copyName(stem, ext, i);
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            const std::wstring candidate = dst;
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES &&
                !reservedDestinations.contains(
                    services.normalize(candidate)))
            {
                reservedDestinations.insert(
                    services.normalize(candidate));
                return std::wstring(dst);
            }
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), services.copyName(stem, ext, 1000).c_str());
        return std::wstring(fallback);
    };

    auto makeUniqueShortcutPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, fileName ? fileName : L"");
        PathRemoveExtensionW(stemBuf);
        std::wstring stem = stemBuf[0] != L'\0' ? stemBuf : services.shortcutName;

        for (int i = 1; i <= 10000; ++i)
        {
            std::wstring name = i <= 1
                ? stem + L".lnk"
                : stem + L" (" + std::to_wstring(i) + L").lnk";
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            const std::wstring candidate = dst;
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES &&
                !reservedDestinations.contains(
                    services.normalize(candidate)) &&
                reservations.TryReserve(candidate))
            {
                reservedDestinations.insert(
                    services.normalize(candidate));
                materializedPathReservations->push_back(candidate);
                return candidate;
            }
        }
        return std::wstring{};
    };

    bool operated = false;
    if (action == DropAction::Link)
    {
        snowdesktop::ShellFileOperationRequest request;
    request.result = result;
        for (const auto& source : sourceList.entries)
        {
            const auto& path = source.filePath;
            if (path.empty()) continue;

            std::wstring dst = makeUniqueShortcutPath(path);
            if (dst.empty())
                continue;
            const auto folderTarget =
                snowdesktop::item_location::
                    ResolveFolderTarget(path);
            if (folderTarget.kind ==
                snowdesktop::item_location::
                    FolderTargetKind::Shortcut)
            {
                // Preserve a folder shortcut as a shortcut file. Creating a
                // new link whose target is the source .lnk produces a nested
                // link that no longer classifies as a Dock folder.
                request.exactFileCopies.push_back({ path, dst });
            }
            else
            {
                request.shortcuts.push_back({
                    path, dst, desktopPath, true });
            }
            if (createdPathsBySource)
                (*createdPathsBySource)[source.sourceIndex] = dst;
        }
        if (request.steps.empty() && request.exactFileCopies.empty() &&
            request.shortcuts.empty())
        {
            reservations.Release(
                *materializedPathReservations);
            return false;
        }
        if (executeSynchronously)
        {
            operated = snowdesktop::ShellFileOperationWorker::
                Execute(request);
            reservations.Release(
                *materializedPathReservations);
            if (completion)
                completion(operated);
        }
        else
        {
            auto reservationCompletion = [&reservations,
                materializedPathReservations,
                completion = std::move(completion)](
                    bool succeeded) mutable {
                reservations.Release(
                    *materializedPathReservations);
                if (completion)
                    completion(succeeded);
            };
            operated = services.enqueue(
                std::move(request),
                std::move(reservationCompletion));
            if (!operated)
                reservations.Release(
                    *materializedPathReservations);
        }
    }
    else if (action == DropAction::Copy)
    {
        std::vector<snowdesktop::ShellFileOperationStep> steps;
        std::vector<std::wstring> normalCopies;
        for (const auto& path : paths)
        {
            if (duplicateDesktopCopyNames && sameParentAsDesktop(path))
            {
                steps.push_back({
                    FO_COPY,
                    { path },
                    makeUniqueCopyPath(path),
                    static_cast<FILEOP_FLAGS>(
                        FOF_NOCONFIRMATION |
                        FOF_NOCONFIRMMKDIR |
                        FOF_NOERRORUI |
                        FOF_RENAMEONCOLLISION) });
            }
            else
                normalCopies.push_back(path);
        }
        if (!normalCopies.empty())
        {
            steps.push_back({
                FO_COPY,
                std::move(normalCopies),
                desktopPath,
                static_cast<FILEOP_FLAGS>(
                    FOF_NOCONFIRMATION |
                    FOF_NOCONFIRMMKDIR |
                    FOF_NOERRORUI |
                    FOF_RENAMEONCOLLISION) });
        }
        if (executeSynchronously)
        {
            snowdesktop::ShellFileOperationRequest request;
            request.result = result;
            request.steps = std::move(steps);
            operated = snowdesktop::ShellFileOperationWorker::Execute(request);
            if (completion)
                completion(operated);
        }
        else
        {
            snowdesktop::ShellFileOperationRequest request;
            request.steps = std::move(steps);
            request.result = result;
            operated = services.enqueue(std::move(request), std::move(completion));
        }
    }
    else
    {
        std::vector<snowdesktop::ShellFileOperationStep> steps;
        steps.push_back({
            FO_MOVE,
            std::move(paths),
            desktopPath,
            static_cast<FILEOP_FLAGS>(
                FOF_NOCONFIRMATION |
                FOF_NOCONFIRMMKDIR |
                FOF_NOERRORUI |
                FOF_RENAMEONCOLLISION) });
        if (executeSynchronously)
        {
            snowdesktop::ShellFileOperationRequest request;
            request.result = result;
            request.steps = std::move(steps);
            operated = snowdesktop::ShellFileOperationWorker::Execute(request);
            if (completion)
                completion(operated);
        }
        else
        {
            snowdesktop::ShellFileOperationRequest request;
            request.steps = std::move(steps);
            request.result = result;
            operated = services.enqueue(std::move(request), std::move(completion));
        }
    }
    return operated;
}
inline bool MaterializeFolder(const DragSourceList& sources, const std::wstring& folder,
    DropAction action, const Services& services, Completion completion, bool synchronously,
    std::shared_ptr<ShellFileOperationResult> result)
{
    const auto paths = sources.FilePaths();
    if (IsSelfContainedFolderDrop(paths, folder)) return false;
    const DWORD effect = action == DropAction::Link ? DROPEFFECT_LINK :
        action == DropAction::Move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    auto request = file_backed_drop::BuildRequest(paths, folder, effect, std::move(result));
    if (!request) return false;
    if (!synchronously) return services.enqueue(std::move(*request), std::move(completion));
    const bool succeeded = ShellFileOperationWorker::Execute(*request);
    if (completion) completion(succeeded);
    return succeeded;
}
}
