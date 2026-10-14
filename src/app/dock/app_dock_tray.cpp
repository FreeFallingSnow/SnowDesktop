#include "app/app.h"
#include "dock/dock_tray_targets.h"
#include "ui/menu/modern_menu.h"

namespace
{
struct TrayMenuImages
{
    std::vector<HBITMAP> images;
    ~TrayMenuImages() { for (auto image : images) DeleteObject(image); }
    HBITMAP Add(const snowdesktop::tray::Icon& icon)
    {
        if (!icon.width || !icon.height || icon.width > 64 || icon.height > 64 ||
            icon.pixels.size() != static_cast<size_t>(icon.width) * icon.height) return nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(icon.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(icon.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); return nullptr; }
        std::copy(icon.pixels.begin(), icon.pixels.end(), static_cast<std::uint32_t*>(pixels));
        images.push_back(bitmap);
        return bitmap;
    }
};
}

std::shared_ptr<snowdesktop::tray::Service> DesktopApp::EnsureDockTrayService()
{
    if (!dockTrayService_ && statusBar_) dockTrayService_ = statusBar_->Tray();
    if (!dockTrayService_) dockTrayService_ = std::make_shared<snowdesktop::tray::Service>();
    return dockTrayService_;
}

bool DesktopApp::HasDockTrayTarget(size_t itemIndex)
{
    if (!dockTrayService_ || itemIndex >= items_.size()) return false;
    return !snowdesktop::dock_tray::CollectTargets(dockTrayService_->Current(),
        dockSettings_.trayApplications, ResolveDockAppIdentity(itemIndex).executablePath).empty();
}

bool DesktopApp::TryActivateDockTray(const std::wstring& executable, size_t itemIndex,
    std::optional<RECT> capturedAnchor)
{
    namespace rules = snowdesktop::dock_tray;
    namespace menu = snowdesktop::modern_menu;
    const std::wstring path = executable;
    const auto* rule = rules::FindRule(dockSettings_.trayApplications, path);
    if (!generalSettings_.dockEnabled || !rule || !rule->enabled || exitRequested_) return false;
    DockAppIdentity identity;
    identity.kind = DockAppIdentityKind::Executable;
    identity.executablePath = path;
    if (IsDockAppClosePending(identity)) return true;
    const auto service = EnsureDockTrayService();
    const auto initial = service->Current();
    const auto targets = rules::CollectTargets(initial, dockSettings_.trayApplications, path);
    if (targets.empty()) return false;
    DismissDockWindowPreviewUntilLeave();
    RECT anchor = capturedAnchor.value_or(RECT{});
    DockContainer* dock = GetDockContainerAtPoint(lastMousePoint_);
    if (IsRectEmpty(&anchor) && dock)
    {
        RECT bounds{};
        if (auto* entry = dock->EntryAtPoint(lastMousePoint_)) bounds = entry->GetBounds();
        else if (auto* frequent = dock->FrequentItemAtPoint(lastMousePoint_)) bounds = frequent->GetBounds();
        else if (auto* running = dock->RunningItemAtPoint(lastMousePoint_)) bounds = running->GetBounds();
        if (!IsRectEmpty(&bounds))
        {
            anchor = dock->GetElementVisualRect(bounds, lastMousePoint_);
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
        }
    }
    if (IsRectEmpty(&anchor))
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    }
    const HWND owner = ShellLaunchOwnerHwnd();
    const auto activate = [&](const snowdesktop::tray::Icon& target, bool context, RECT bounds) {
        if (exitRequested_ || !generalSettings_.dockEnabled || !rules::IsCurrentTarget(service->Current(), target)) return;
        const auto* sourceRule = rules::FindRule(dockSettings_.trayApplications, path);
        const auto* currentRule = rules::FindRule(dockSettings_.trayApplications, target.application);
        if (!sourceRule || !sourceRule->enabled || !currentRule || !currentRule->enabled ||
            !rules::Matches(*sourceRule, target.application)) return;
        const POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        if (context)
        {
            service->Activate(target.key, snowdesktop::tray::Activation::RightDown, point, {owner, owner}, bounds, target.identity);
            service->Activate(target.key, snowdesktop::tray::Activation::RightUp, point, {owner, owner}, bounds, target.identity);
        }
        else service->ActivatePrimary(target.key, currentRule->doubleClick, point, {}, bounds, target.identity);
    };
    if (targets.size() == 1)
    {
        activate(targets.front(), false, anchor);
        if (itemIndex < items_.size()) RecordDockItemUsage(itemIndex);
        return true;
    }
    const std::wstring itemKey = itemIndex < items_.size() ? DockItemWindowKey(items_[itemIndex]) : L"";
    const std::wstring launchTarget = itemIndex < items_.size() ? items_[itemIndex].parsingName : path;
    constexpr UINT launchCommand = 0x7fffffff;
    TrayMenuImages images;
    // Bind commands to live icon identities, never tooltip text or row indices.
    // In particular a removed account cannot be replaced by a reordered row.
    std::vector<snowdesktop::tray::Icon> bindings;
    std::vector<HBITMAP> bindingImages;
    const auto makeItems = [&](const snowdesktop::tray::Snapshot& snapshot) {
        std::vector<menu::Item> entries;
        menu::Item heading;
        heading.label = rules::ExecutableName(path);
        heading.enabled = false;
        entries.push_back(std::move(heading));
        for (const auto& icon : rules::CollectTargets(snapshot, dockSettings_.trayApplications, path))
        {
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& existing) {
                return existing.key == icon.key && snowdesktop::tray::SameFocusIdentity(existing.identity, icon.identity);
            });
            const auto index = static_cast<size_t>(std::distance(bindings.begin(), found));
            if (found == bindings.end())
            {
                bindings.push_back(icon);
                bindingImages.push_back(images.Add(icon));
            }
            menu::Item entry;
            entry.command = static_cast<UINT>(index + 1);
            entry.label = icon.tip;
            entry.literalLabel = true;
            entry.image = bindingImages[index];
            entries.push_back(std::move(entry));
        }
        menu::Item separator;
        separator.separator = true;
        entries.push_back(std::move(separator));
        menu::Item launch;
        launch.command = launchCommand;
        launch.label = _LW("settings.dock.trayLaunch");
        entries.push_back(std::move(launch));
        return entries;
    };
    auto entries = makeItems(initial);
    menu::Options options;
    options.owner = owner;
    options.anchor = {(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
    options.anchorRect = anchor;
    options.dpi = std::max<UINT>(96, GetDpiForWindow(owner));
    options.lightTheme = IsLightContentTheme();
    options.appearance = options.lightTheme ? menu::Appearance::OpaqueLight : menu::Appearance::OpaqueDark;
    options.topmost = true;
    if (const auto* host = dock ? FindPersistentDockHost(dock) : nullptr;
        host && (host->container->IsMergedWithStatusBar() || IsPersistentDockHostEffectivelyFloating(*host)))
        options.zOrderOwner = host->hwnd;
    KeepDockTopmostForAction(anchor);
    switch (dockSettings_.position)
    {
    case DockPosition::Top: options.rootPlacement = menu::RootPlacement::BelowAnchorRect; break;
    case DockPosition::Left: options.rootPlacement = menu::RootPlacement::RightOfAnchorRect; break;
    case DockPosition::Right: options.rootPlacement = menu::RootPlacement::LeftOfAnchorRect; break;
    default: options.rootPlacement = menu::RootPlacement::AboveAnchorRect; break;
    }
    options.allowContextCommand = [&](UINT command) { return command && command != launchCommand; };
    std::uint64_t revision = initial.revision;
    options.pollItems = [&](const auto&, bool canApply) -> std::optional<std::vector<menu::Item>> {
        if (!canApply) return {};
        const auto snapshot = service->Current();
        if (snapshot.revision == revision) return {};
        revision = snapshot.revision;
        return makeItems(snapshot);
    };
    options.pollItemsFinished = [] { return false; };
    ConfigureModernMenuEventPump(options);
    const auto selected = menu::Show(entries, options);
    if (selected.command == launchCommand && !exitRequested_)
    {
        const auto current = FindItemIndexByKey(itemKey);
        if (!itemKey.empty() && current < items_.size() && items_[current].parsingName == launchTarget)
            LaunchDesktopItem(current, true, false);
        else if (itemKey.empty()) LaunchPathWithShortcutPolicy(owner, launchTarget);
    }
    else if (selected.command && selected.command <= bindings.size())
        activate(bindings[selected.command - 1], selected.contextCommand, selected.itemScreenRect);
    return true;
}
