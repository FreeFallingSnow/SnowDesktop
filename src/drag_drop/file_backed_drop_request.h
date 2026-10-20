#pragma once

#include "layout/folder_self_drop_rules.h"
#include "shell/shell_file_operation_worker.h"
#include <filesystem>
#include <optional>
#include <unordered_set>

namespace snowdesktop::file_backed_drop
{
// This is the host's complete request-construction boundary. Every newly
// materialized link requires atomic no-overwrite execution by the worker.
inline std::optional<ShellFileOperationRequest> BuildRequest(
    const std::vector<std::wstring>& paths, std::wstring folder, DWORD effect,
    std::shared_ptr<ShellFileOperationResult> result = {})
{
    if (paths.empty() || folder.empty() ||
        folder_self_drop_rules::IsSelfContainedFolderDrop(paths, folder) ||
        (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE && effect != DROPEFFECT_LINK))
        return std::nullopt;
    if (folder.back() != L'\\' && folder.back() != L'/') folder += L'\\';
    ShellFileOperationRequest request;
    request.result = std::move(result);
    if (effect == DROPEFFECT_LINK)
    {
        std::unordered_set<std::wstring> reserved;
        for (const auto& path : paths)
        {
            bool assigned = false;
            auto stem = std::filesystem::path(path).filename().wstring();
            if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".lnk") == 0)
                stem.resize(stem.size() - 4);
            for (int suffix = 1; suffix <= 10000; ++suffix)
            {
                const auto destination = folder + stem +
                    (suffix == 1 ? L".lnk" : L" (" + std::to_wstring(suffix) + L").lnk");
                const auto key = folder_self_drop_rules::NormalizeFolderPath(destination);
                if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES || reserved.contains(key))
                    continue;
                reserved.insert(key);
                request.shortcuts.push_back({path, destination, folder, true});
                assigned = true;
                break;
            }
            if (!assigned) return std::nullopt;
        }
        if (request.shortcuts.empty()) return std::nullopt;
    }
    else
    {
        request.steps.push_back({static_cast<UINT>(effect == DROPEFFECT_MOVE ? FO_MOVE : FO_COPY),
            paths, folder, static_cast<FILEOP_FLAGS>(
                FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_RENAMEONCOLLISION)});
    }
    return request;
}
}
