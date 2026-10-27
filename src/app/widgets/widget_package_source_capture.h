#pragma once

#include "common/types.h"
#include "widget/packages/widget_package_source_snapshot.h"

namespace snowdesktop::widget_source_capture
{
// Shared by single-widget discovery and the transaction-local layout snapshot.
template<class Encode, class Decode, class ItemUrl>
inline void Apply(DesktopWidget& widget,
    const std::optional<snowdesktop::widget::PackageSourceRef>& source,
    Encode encode, Decode decode, ItemUrl itemUrl)
{
    if (!source)
    {
        if (widget.packageSourceProvider != L"steam-workshop")
        {
            widget.packageSourceProvider.clear();
            widget.packageSourceExternalItemId.clear();
            widget.packageSourceUrl.clear();
        }
        return;
    }

    const bool hasRememberedWorkshopSource =
        widget.packageSourceProvider == L"steam-workshop" &&
        !snowdesktop::widget::SteamPublishedFileId(
            encode(widget.packageSourceExternalItemId)).empty();
    if (hasRememberedWorkshopSource && source->providerId != "steam-workshop")
    {
        // Keep the durable Workshop recovery address when only a local
        // development override remains after the managed copy disappears.
        return;
    }

    if (source->providerId != "steam-workshop")
    {
        widget.packageSourceProvider.clear();
        widget.packageSourceExternalItemId.clear();
        widget.packageSourceUrl.clear();
        return;
    }

    widget.packageSourceProvider = decode(source->providerId);
    widget.packageSourceExternalItemId = decode(source->externalItemId);
    widget.packageSourceUrl.clear();
    const std::string publishedFileId =
        snowdesktop::widget::SteamPublishedFileId(source->externalItemId);
    if (!publishedFileId.empty())
        widget.packageSourceUrl = itemUrl(publishedFileId);
}

template<class ListPackages, class Encode, class Decode, class ItemUrl>
inline void CaptureForSave(std::vector<DesktopWidget>& widgets,
    ListPackages listPackages, Encode encode, Decode decode, ItemUrl itemUrl)
{
    std::optional<snowdesktop::widget::PackageSourceSnapshot> snapshot;
    for (auto& widget : widgets)
    {
        if (widget.type != DesktopWidgetType::LuaScript || widget.packageId.empty())
            continue;
        const std::string packageId = encode(widget.packageId);
        if (!packageId.empty() && !snapshot)
            snapshot.emplace(listPackages());
        Apply(widget, snapshot ? snapshot->Find(packageId) : std::nullopt,
            encode, decode, itemUrl);
    }
}
}
