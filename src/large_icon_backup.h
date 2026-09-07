#pragma once
#include "full_data_backup.h"

namespace snowdesktop
{
inline backup::OperationResult EnsureLargeIconUpgradeBackup(const std::filesystem::path& state,
    const std::filesystem::path& data, const std::string& version)
{
    try
    {
        backup::FullDataBackupManager manager(state / L"LargeIconUpgradeBackup", data, version, "large-icon-upgrade");
        const auto existing = manager.List();
        if (!existing.empty()) return {true, false, existing.front(), {}};
        return manager.Create();
    }
    catch (const std::exception& error) { return {false, false, {}, error.what()}; }
}
}
