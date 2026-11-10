#include "dock_process_discovery.h"
#include "dock_installed_application.h"
#include "common/utils.h"

#include <span>
#include <tlhelp32.h>

namespace snowdesktop::dock_process_discovery
{
Snapshot Capture()
{
    Snapshot result;
    const HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (handle == INVALID_HANDLE_VALUE) return result;
    const struct CloseSnapshot
    {
        HANDLE handle;
        ~CloseSnapshot() { CloseHandle(handle); }
    } snapshot{handle};

    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot.handle, &entry))
        do { result.parents.insert_or_assign(entry.th32ProcessID, entry.th32ParentProcessID); }
        while (Process32NextW(snapshot.handle, &entry));
    return result;
}

std::wstring ApplicationExecutablePath(DWORD processId, std::wstring imagePath)
{
    if (!processId || imagePath.empty()) return imagePath;
    using namespace dock_app_identity_rules;
    const auto product = ReadExecutableProduct(imagePath);
    if (product.company.empty() || product.product.empty()) return imagePath;

    const auto snapshot = Capture();
    auto ancestors = AncestorPaths(processId, snapshot.parents, [&](DWORD ancestor) {
        return snapshot.executablePaths.Read(ancestor, [](std::wstring path) { return path; });
    });
    const auto runningPath = ToUpperInvariant(imagePath);
    for (auto& ancestor : ancestors)
    {
        const auto candidate = ToUpperInvariant(ancestor);
        if (candidate == runningPath || !MatchesExecutableProcessFamily(candidate, runningPath,
                std::span<const std::wstring>(&candidate, 1))) continue;
        const auto parent = ReadExecutableProduct(ancestor);
        const auto parentProduct = ProductWithoutLauncher(parent.product);
        // A launcher can also start separate products installed beneath it.
        // Require publisher and product metadata before using its display name.
        if (parent.company == product.company && !parentProduct.empty() &&
            (product.product == parentProduct ||
                (product.product.starts_with(parentProduct) && product.product[parentProduct.size()] == L' ')))
            return std::move(ancestor);
    }
    return imagePath;
}
}
