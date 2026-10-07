#include "app/app.h"
#include "icons/large_icon_steam.h"
#include "desktop/desktop_source.h"

void DesktopApp::SynchronizeLargeIconAssets()
{
    if (exitRequested_ || !dataCleanupHasCompleteModel_) return;
    if (!largeIconAssets_)
        largeIconAssets_ = std::make_unique<snowdesktop::LargeIconAssets>(std::filesystem::path(GetDataDirectoryPath()) / L"large-icons",
            [window = controlHwnd_] { PostMessageW(window, kLargeIconAssetsReadyMessage, 0, 0); });
    // Release allocations and pending imports belonging to deleted/restored
    // objects before supplying the complete ownership set to the collector.
    for (auto it = largeIconRuntime_.begin(); it != largeIconRuntime_.end();)
    {
        const auto index = FindItemIndexByKey(it->first);
        if (index < items_.size() && items_[index].largeIcon) { ++it; continue; }
        desktopBackdropCompositor_.RemovePanel(it->second.backdropFrame);
        largeIconAssets_->Cancel(it->first);
        if (it->second.asset) EraseD2DIconCacheForBitmap(it->second.asset->bitmap);
        it = largeIconRuntime_.erase(it);
    }
    std::vector<std::string> retained;
    std::vector<snowdesktop::LargeIconAssetRequest> sources;
    for (const auto& item : items_)
    {
        if (!item.largeIcon) continue;
        retained.push_back(item.largeIcon->image);
        retained.push_back(item.largeIcon->foregroundImage);
        retained.push_back(item.largeIcon->cachedCover);
        snowdesktop::LargeIconAssetRequest source;
        source.parsingName = item.parsingName;
        source.sourceStamp = item.modifiedTime ? (std::uint64_t(item.modifiedTime->dwHighDateTime) << 32) |
            item.modifiedTime->dwLowDateTime : 0;
        source.sourceIconIndex = item.sysIconIndex;
        wchar_t url[2048]{};
        if (_wcsicmp(std::filesystem::path(item.parsingName).extension().c_str(), L".url") == 0)
        {
            GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, static_cast<DWORD>(std::size(url)), item.parsingName.c_str());
            source.appId = snowdesktop::large_icon_steam::AppId(url).value_or(0);
        }
        sources.push_back(std::move(source));
    }
    largeIconAssets_->RetainReferences(std::move(retained));
    largeIconAssets_->RetainSources(std::move(sources));
}

void DesktopApp::ScheduleDataCleanup()
{
    if (exitRequested_ || !dataCleanupHasCompleteModel_) return;
    // Fetch may have written a new GUID ICO that has not yet been attached.
    // It cancels collection before starting, and reschedules after Apply.
    if (!websiteIconPendingPath_.empty()) return;
    std::vector<std::filesystem::path> paths;
    paths.emplace_back(snowdesktop::desktop_source::Directory());
    if (!snowdesktop::debug_profile::Enabled())
        for (const auto& path : snowdesktop::desktop_source::SystemDesktops()) paths.push_back(path);
    for (const auto& item : items_) if (!item.parsingName.empty()) paths.emplace_back(item.parsingName);
    for (const auto& widget : widgets_)
    {
        if (!widget.sourceFolderPath.empty()) paths.emplace_back(widget.sourceFolderPath);
        for (const auto& path : widget.itemKeys) paths.emplace_back(path);
        for (const auto& entry : widget.folderEntries) paths.emplace_back(entry.fullPath);
    }
    for (const auto& entry : dockEntries_)
        if (!entry.reference.empty()) paths.emplace_back(entry.reference);
    const std::filesystem::path data(GetDataDirectoryPath());
    dataCleanupWorker_.Submit([data, paths = std::move(paths)](std::stop_token stop) {
        const auto icons = snowdesktop::website_icon::CollectUnused(data / L"website-icons", paths, stop);
        const auto transient = snowdesktop::data_cleanup::Collect(data, paths, stop);
        if ((icons || transient) && !stop.stop_requested())
            WriteDiagnosticLogEntry((L"Data cleanup: website-icons=" + std::to_wstring(icons) +
                L" transient=" + std::to_wstring(transient)).c_str());
    });
}
