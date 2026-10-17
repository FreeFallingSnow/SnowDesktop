#include "app/app.h"
#include "icons/icon_library.h"
#include "icons/icon_workshop.h"
#include "../../../steam_bridge/src/icon_pack_workshop_publish.h"
#include "icons/icon_image_codec.h"
#include "icons/icon_override_rules.h"
#include "icons/icon_control_source.h"
#include "icons/large_icon_edit_rules.h"

struct IconLibraryHostState
{
    std::shared_ptr<snowdesktop::IconLibrary> library;
    snowdesktop::BackgroundWork operations{1}, images{2};
    snowdesktop::IconLibrarySnapshot snapshot;
    bool initialized = false, busy = false;
    std::wstring target;
    std::uint64_t session = 0;
    std::shared_ptr<std::atomic_bool> workshopCancel=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_uint> workshopProgress=std::make_shared<std::atomic_uint>(0);
    std::string validPreparation;
    std::optional<snowdesktop::LargeIconSettingsRequest> pendingLargeIcon;
    std::unordered_map<std::string, std::shared_ptr<snowdesktop::BackgroundBitmap>> bitmaps;
};

IconLibraryHostState& DesktopApp::EnsureIconLibrary()
{
    if (!iconLibraryState_)
    {
        iconLibraryState_ = std::make_shared<IconLibraryHostState>();
        iconLibraryState_->library = std::make_shared<snowdesktop::IconLibrary>(GetDataSubdirectoryPath(L"icon-library"));
    }
    return *iconLibraryState_;
}
void DesktopApp::DrainIconLibrary()
{
    if (!iconLibraryState_) return;
    iconLibraryState_->operations.Drain();
    iconLibraryState_->images.Drain();
}

snowdesktop::IconLibrarySnapshot DesktopApp::EditIconLibrary(snowdesktop::IconLibraryRequest request)
{
    auto& state = EnsureIconLibrary(); DrainIconLibrary();
    std::unordered_map<std::string, std::vector<std::wstring>> uses;
    const auto retain = [&](const std::string& id, const std::wstring& name) {
        if (!id.empty()) uses[id].push_back(name);
    };
    for (const auto& [target, asset] : iconOverrides_) retain(asset, IconOverrideTargetName(target));
    for (const auto& widget : widgets_) retain(widget.dockIconAssetId, widget.title);
    const auto retainLarge = [&](const auto& config, const std::wstring& name) {
        retain(config.imageAssetId, name); retain(config.foregroundImageAssetId, name);
    };
    for (const auto& item : items_) if (item.largeIcon) retainLarge(*item.largeIcon, item.name);
    for (const auto& [key, record] : layoutRecords_) if (record.largeIcon && FindItemIndexByKey(key) >= items_.size())
        retainLarge(*record.largeIcon, key);
    for (const auto& [key, config] : largeIconEdit_.previews) retainLarge(config, key);
    if (largeIconEdit_.preview) retainLarge(*largeIconEdit_.preview, largeIconEdit_.key);

    if (request.action == "begin" &&
        (request.target == snowdesktop::icon_override::start || request.target == snowdesktop::icon_override::search))
    { state.target = request.target; ++state.session; request.action = "read"; }
    if (request.action == "restore" &&
        (request.target == snowdesktop::icon_override::start || request.target == snowdesktop::icon_override::search))
    {
        auto result = state.snapshot; result.succeeded = SetIconOverride(request.target, {});
        result.error = result.succeeded ? "" : "iconLibrary.saveFailed"; return result;
    }
    if(request.action=="cancelWorkshop")
    {state.workshopCancel->store(true);state.validPreparation.clear();state.snapshot.preparationId.clear();auto result=state.snapshot;result.busy=state.busy;return result;}
    if(request.action=="openWorkshop"||request.action=="openPublished")
    {
        std::string url="https://steamcommunity.com/workshop/browse/?appid=5080330&requiredtags%5B0%5D=Icon%20Pack";
        if(request.action=="openPublished")
        {
            const auto group=std::find_if(state.snapshot.groups.begin(),state.snapshot.groups.end(),[&](const auto& value){return value.id==request.groupId;});
            if(group==state.snapshot.groups.end())return state.snapshot;
            url=group->publishedUrl;if(url.empty())return state.snapshot;
        }
        const auto wide=Utf8ToWide(url);ShellExecuteW(nullptr,L"open",wide.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return state.snapshot;
    }
    if((request.action=="delete"||request.action=="rename"||request.action=="deleteGroupWithAssets")&&
        !(request.action=="rename"&&std::any_of(state.snapshot.groups.begin(),state.snapshot.groups.end(),[&](const auto& group){return group.id==request.groupId&&group.workshopItem.empty();}))&&
        std::any_of(state.snapshot.groups.begin(),state.snapshot.groups.end(),[&](const auto& group){
            if(group.workshopItem.empty())return false;
            return std::any_of(state.snapshot.entries.begin(),state.snapshot.entries.end(),[&](const auto& entry){return
                (request.action=="deleteGroupWithAssets"?std::find(entry.groupIds.begin(),entry.groupIds.end(),request.groupId)!=entry.groupIds.end():
                std::find(request.ids.begin(),request.ids.end(),entry.id)!=request.ids.end())&&std::find(entry.groupIds.begin(),entry.groupIds.end(),group.id)!=entry.groupIds.end();});}))
    {auto result=state.snapshot;result.succeeded=false;result.error="iconWorkshop.managedGroup";return result;}
    const bool workshopOperation=request.action=="preparePreview"||request.action=="prepareShare"||request.action=="publishPrepared"||request.action=="syncSubscriptions"||request.action=="autoSyncSubscriptions";
    if(request.action=="publishPrepared"&&(state.workshopCancel->load()||request.preparationId.empty()||request.preparationId!=state.validPreparation))
    {auto result=state.snapshot;result.succeeded=false;result.error="iconWorkshop.stalePreparation";return result;}
    auto response = state.snapshot;
    response.busy = state.busy; response.target.clear(); response.targetName.clear(); response.session = 0;
    response.progress=state.busy?state.workshopProgress->load():0;
    if (!request.target.empty())
    {
        if (request.target != state.target || !IsIconOverrideTarget(request.target))
        { response.succeeded = false; response.error = "iconLibrary.stale"; return response; }
        response.target = state.target; response.session = state.session; response.targetName = IconOverrideTargetName(state.target);
    }
    if (request.action == "cancel")
    {
        if (state.target == request.target && state.session == request.session) { state.target.clear(); ++state.session; }
        response.succeeded = true;
    }
    else if (request.action == "apply")
    {
        response.succeeded = !state.busy && request.target == state.target && request.session == state.session &&
            request.ids.size() == 1 && SetIconOverride(request.target, request.ids[0]);
        response.error = response.succeeded ? "" : "iconLibrary.stale";
        if (response.succeeded) { state.target.clear(); ++state.session; }
    }
    else if ((!state.initialized || request.action == "refresh" || request.action == "import" ||
        request.action == "rename" || request.action == "delete" || request.action == "export" ||
        workshopOperation || request.action=="copyLocalGroup" || request.action=="saveSharing" || request.action=="collectRetired" || request.action == "createGroup" || request.action == "renameGroup" || request.action == "deleteGroup" || request.action == "deleteGroupWithAssets" || request.action == "moveGroup") && !state.busy)
    {
        if (request.action != "read" && request.action != "refresh" && request.action != "import" && request.action!="autoSyncSubscriptions" && !state.initialized)
        { response.error = "iconLibrary.working"; response.succeeded = false; return response; }
        if(request.action=="delete"||request.action=="deleteGroupWithAssets")
        {
            auto ids=request.ids;
            if(request.action=="deleteGroupWithAssets")
            {
                if(std::none_of(state.snapshot.groups.begin(),state.snapshot.groups.end(),[&](const auto& group){return group.id==request.groupId;}))
                {response.succeeded=false;response.error="iconLibrary.stale";return response;}
                ids.clear();for(const auto& entry:state.snapshot.entries)
                    if(std::find(entry.groupIds.begin(),entry.groupIds.end(),request.groupId)!=entry.groupIds.end())ids.push_back(entry.id);
            }
            const auto selected=[&](const auto& id){return !id.empty()&&std::find(ids.begin(),ids.end(),id)!=ids.end();};
            const auto previousOverrides=iconOverrides_;const auto previousRecords=layoutRecords_;const auto previousEdit=largeIconEdit_;
            std::vector<std::string> previousWidgetIcons;std::vector<std::optional<snowdesktop::LargeIconConfig>> previousItemConfigs;
            std::vector<size_t> changedLargeIcons;bool changed=false;
            for(const auto& item:items_)previousItemConfigs.push_back(item.largeIcon);
            for(const auto& widget:widgets_)previousWidgetIcons.push_back(widget.dockIconAssetId);
            changed=std::erase_if(iconOverrides_,[&](const auto& value){return selected(value.second);})>0;
            for(auto& widget:widgets_)if(selected(widget.dockIconAssetId)){widget.dockIconAssetId.clear();changed=true;}
            for(size_t i=0;i<items_.size();++i)if(items_[i].largeIcon&&snowdesktop::RestoreIconLibraryAssetReferences(*items_[i].largeIcon,ids))
            {changed=true;changedLargeIcons.push_back(i);}
            for(auto& [key,record]:layoutRecords_)
            {(void)key;if(record.largeIcon)changed=snowdesktop::RestoreIconLibraryAssetReferences(*record.largeIcon,ids)||changed;}
            bool edited=false;
            if(largeIconEdit_.preview)edited=snowdesktop::RestoreIconLibraryAssetReferences(*largeIconEdit_.preview,ids);
            for(auto& [key,config]:largeIconEdit_.previews){(void)key;edited=snowdesktop::RestoreIconLibraryAssetReferences(config,ids)||edited;}
            if(edited)++largeIconEdit_.revision;
            if(changed&&!SaveLayoutSlots())
            {
                iconOverrides_=previousOverrides;layoutRecords_=previousRecords;largeIconEdit_=previousEdit;
                for(size_t i=0;i<items_.size();++i)items_[i].largeIcon=previousItemConfigs[i];
                for(size_t i=0;i<widgets_.size();++i)widgets_[i].dockIconAssetId=previousWidgetIcons[i];
                response.succeeded=false;response.error="iconLibrary.saveFailed";return response;
            }
            for(const auto& id:ids)uses.erase(id);
            for(const auto i:changedLargeIcons)RequestLargeIconAsset(i);
            if(changed||edited)
            {
                InvalidateDragStaticScene();InvalidateDockRects();
                if(hwnd_)InvalidateRect(hwnd_,nullptr,FALSE);
                if(floatingPopupHwnd_)InvalidateRect(floatingPopupHwnd_,nullptr,FALSE);
            }
        }
        if (request.action == "delete" || request.action == "deleteGroupWithAssets" || request.action == "refresh" || request.action == "import" || request.action=="collectRetired")
        {
            state.images.Cancel();
            for (const auto& [key, bitmap] : state.bitmaps)
            {
                (void)key;
                if (bitmap && bitmap->bitmap) EraseD2DIconCacheForBitmap(bitmap->bitmap);
            }
            state.bitmaps.clear();
        }
        std::vector<std::string> protectedIds;
        for (const auto& [id, names] : uses) { (void)names; protectedIds.push_back(id); }
        const auto weak = std::weak_ptr<IconLibraryHostState>(iconLibraryState_);
        if(workshopOperation)state.workshopCancel=std::make_shared<std::atomic_bool>(false);
        if(request.action=="preparePreview"||request.action=="prepareShare")state.validPreparation.clear();
        const auto cancellation=state.workshopCancel;
        const auto progress=state.workshopProgress;progress->store(0);
        const auto bridge=std::filesystem::path(GetExecutableDirectoryPath())/L"SnowDesktopSteamBridge.exe";
        const auto data=std::filesystem::path(GetDataDirectoryPath());
        const bool previousAvailability=state.snapshot.workshopAvailable;
        if(workshopOperation){response.coverPath.clear();response.completedAction.clear();response.operationId=request.operationId;}

        state.busy = true;
        response.error.clear(); response.failures.clear(); response.busy = true; response.succeeded = true;
        const bool queued = state.operations.Submit(L"library-operation",
            [library = state.library, request, protectedIds = std::move(protectedIds),bridge,data,cancellation,progress,workshopOperation,previousAvailability] {
                if(workshopOperation)
                {
                    auto result=snowdesktop::icon_workshop::Run(*library,request,bridge,data,cancellation.get(),progress.get());
                    result.workshopAvailable=snowdesktop::icon_workshop::Available(bridge);result.operationId=request.operationId;result.completedAction=request.action;
                    return result;
                }
                if(request.action=="collectRetired") {auto result=library->CollectRetired(protectedIds);result.workshopAvailable=previousAvailability;return result;}
                if (request.action == "refresh" || request.action=="read")
                {auto result=request.action=="refresh"?library->Refresh():library->Read();result.workshopAvailable=snowdesktop::icon_workshop::Available(bridge);return result;}
                return request.action == "read" ? library->Read() : library->Invoke(request, protectedIds);
            }, [this, weak, request, data, workshopOperation, previousAvailability](auto result) {
                const auto current = weak.lock(); if (!current) return;
                current->busy = false; current->initialized = true;
                if(!workshopOperation&&request.action!="read"&&request.action!="refresh")result.workshopAvailable=previousAvailability;
                if(workshopOperation&&current->workshopCancel->load()){result.coverPath.clear();result.preparationId.clear();result.completedAction.clear();}
                if(!result.preparationId.empty())current->validPreparation=result.preparationId;
                if(request.action=="publishPrepared"&&result.succeeded)current->validPreparation.clear();
                for(auto& group:result.groups)group.publishedUrl=group.workshopItem.empty()?
                    snowdesktop::steam_bridge::IconPackPublishedUrl(data,group.id):"https://steamcommunity.com/sharedfiles/filedetails/?id="+group.workshopItem;
                current->snapshot = std::move(result);
                const auto pending = std::exchange(current->pendingLargeIcon, {});
                if (pending && current->snapshot.imported.size() == 1)
                {
                    if (!ApplyLargeIconLibraryAsset(*pending, current->snapshot.imported.front()))
                        current->snapshot.error = "iconLibrary.stale";
                }
                else if (pending)
                    largeIconRuntime_[pending->key].error = current->snapshot.error.empty() ? "iconLibrary.invalid" : current->snapshot.error;
                InvalidateDockRects();
                if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
                if((request.action=="syncSubscriptions"||request.action=="autoSyncSubscriptions")&&current->snapshot.succeeded)
                {snowdesktop::IconLibraryRequest collect;collect.action="collectRetired";(void)EditIconLibrary(collect);}
            }, controlHwnd_, kLargeIconAssetsReadyMessage);
        if (!queued) { state.busy = false; response.busy = false; response.succeeded = false; response.error = "iconLibrary.unavailable"; }
    }
    else if (request.action == "read") response.succeeded = true;
    else if (state.busy) { response.succeeded = false; response.error = "iconLibrary.working"; }
    for (auto& entry : response.entries) entry.uses = uses[entry.id];
    return response;
}

void DesktopApp::OpenDockIconLibrary(const std::wstring& widgetId)
{
    const auto index = FindWidgetIndexById(widgetId);
    if (index >= widgets_.size() || !IsWidgetDockEntryType(DockEntryTypeForWidget(widgets_[index].type))) return;
    OpenIconOverrideLibrary(L"widget:" + widgetId);
}

bool DesktopApp::SetDockIconAsset(const std::wstring& widgetId, const std::string& assetId)
{
    const auto index = FindWidgetIndexById(widgetId);
    if (index >= widgets_.size()) return false;
    auto& widget = widgets_[index];
    if (widget.type != DesktopWidgetType::Collection && widget.type != DesktopWidgetType::FileCategories &&
        widget.type != DesktopWidgetType::CollectionGroup && widget.type != DesktopWidgetType::FileGroup &&
        widget.type != DesktopWidgetType::FolderMapping) return false;
    if (!assetId.empty())
    {
        auto& state = EnsureIconLibrary();
        if (state.busy || !snowdesktop::IsIconAssetId(assetId) ||
            std::none_of(state.snapshot.entries.begin(), state.snapshot.entries.end(), [&](const auto& entry) { return entry.id == assetId && !entry.missing; })) return false;
    }
    const auto previous = widget.dockIconAssetId; widget.dockIconAssetId = assetId;
    if (!SaveLayoutSlots()) { widget.dockIconAssetId = previous; return false; }
    ClearDockAppearanceImages();
    InvalidateDragStaticScene(); InvalidateDockRects();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool DesktopApp::ApplyLargeIconLibraryAsset(const snowdesktop::LargeIconSettingsRequest& request, const std::string& id)
{
    auto& state = EnsureIconLibrary();
    if (state.busy || !CanEditLargeIcons() || !snowdesktop::IsIconAssetId(id) ||
        !snowdesktop::large_icon_edit_rules::CheckRequest(largeIconEdit_, request, true).empty() ||
        std::none_of(state.snapshot.entries.begin(), state.snapshot.entries.end(), [&](const auto& entry) { return entry.id == id && !entry.missing; })) return false;
    const auto keys = largeIconEdit_.keys.empty() ? std::vector<std::wstring>{request.key} : largeIconEdit_.keys;
    std::vector<std::pair<size_t, std::optional<snowdesktop::LargeIconConfig>>> changes;
    for (const auto& key : keys)
    {
        const auto index = FindItemIndexByKey(key);
        if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index]) || items_[index].gridCell.pageId == kDockPageId) return false;
        auto config = *items_[index].largeIcon; config.version = 3;
        if (snowdesktop::IsLargeIconFill(config)) { config.content = 1; config.image.clear(); config.imageAssetId = id; }
        else { config.foregroundContent = 1; config.foregroundImage.clear(); config.foregroundImageAssetId = id; }
        changes.emplace_back(index, std::move(config));
    }
    return SetLargeIconConfigs(changes);
}
bool DesktopApp::ImportLargeIconLibraryAsset(const snowdesktop::LargeIconSettingsRequest& request)
{
    auto& state = EnsureIconLibrary();
    if (state.busy || request.path.empty()) return false;
    state.pendingLargeIcon = request;
    snowdesktop::IconLibraryRequest operation; operation.action = "import"; operation.paths = {request.path};
    if (EditIconLibrary(std::move(operation)).succeeded) return true;
    state.pendingLargeIcon.reset(); return false;
}

ID2D1Bitmap* DesktopApp::GetDockCustomIcon(ID2D1RenderTarget* context, const std::string& id, int pixels, std::wstring_view identity)
{
    if (!snowdesktop::IsIconAssetId(id)) return nullptr;
    auto& state = EnsureIconLibrary();
    const int edge = snowdesktop::icon_control::ReplacementSourcePixels(identity, pixels);
    const auto key = id + ':' + std::to_string(edge);
    const auto found = state.bitmaps.find(key);
    if (found != state.bitmaps.end()) return found->second && found->second->bitmap ?
        GetStyledD2DBitmap(context, found->second->bitmap, ResolveIconAppearance(identity), true) : nullptr;
    if (state.busy) return nullptr;
    if (state.bitmaps.size() >= 256)
    {
        state.images.Cancel();
        for (const auto& [cacheKey, bitmap] : state.bitmaps)
        { (void)cacheKey; if (bitmap && bitmap->bitmap) EraseD2DIconCacheForBitmap(bitmap->bitmap); }
        state.bitmaps.clear();
    }
    state.bitmaps.emplace(key, nullptr);
    const auto path = snowdesktop::IconAssetPath(GetDataSubdirectoryPath(L"icon-library"), id);
    const auto weak = std::weak_ptr<IconLibraryHostState>(iconLibraryState_);
    state.images.Submit(Utf8ToWide(key), [path, edge] {
        auto result = std::make_shared<snowdesktop::BackgroundBitmap>(); snowdesktop::icon_image::Image image; std::string error;
        if (snowdesktop::icon_image::Decode(path, edge, image, error) && snowdesktop::icon_image::PadSquare(image))
        { result->bitmap = snowdesktop::icon_image::CreateBitmap(image); result->size = {image.width, image.height}; }
        return result;
    }, [this, weak, key](auto bitmap) {
        if (auto current = weak.lock())
        {
            current->bitmaps[key] = std::move(bitmap);
            ClearDockAppearanceImages();
            InvalidateDragStaticScene(); InvalidateDockRects();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            if (floatingPopupHwnd_) InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
        }
    }, controlHwnd_, kLargeIconAssetsReadyMessage);
    return nullptr;
}


bool DesktopApp::IsIconOverrideTarget(const std::wstring& target) const
{
    using namespace snowdesktop::icon_override;
    if (target == start || target == search) return true;
    if (target.starts_with(L"widget:"))
    {
        const auto index = FindWidgetIndexById(target.substr(7));
        return index < widgets_.size() && IsWidgetDockEntryType(DockEntryTypeForWidget(widgets_[index].type));
    }
    const auto colon = target.find(L':');
    if (colon == std::wstring::npos) return false;
    const auto prefix = target.substr(0, colon);
    if (prefix != L"item" && prefix != L"recycle-empty" && prefix != L"recycle-full") return false;
    const auto key = target.substr(colon + 1);
    for (const auto& item : items_)
        if (ToUpperInvariant(item.layoutKey) == key)
            return prefix == L"item" || _wcsicmp(item.desktopIconClsid.c_str(), kDesktopIconClsidRecycleBin) == 0;
    if (prefix != L"item") return false;
    for (const auto& widget : widgets_)
        for (const auto& entry : widget.folderEntries)
            if (ToUpperInvariant(entry.fullPath) == key) return true;
    for (const auto& entry : dockFolderPopupWidget_.folderEntries)
        if (ToUpperInvariant(entry.fullPath) == key) return true;
    return false;
}
std::wstring DesktopApp::IconOverrideTargetName(const std::wstring& target) const
{
    if (target == snowdesktop::icon_override::start) return _LW("iconLibrary.startIcon");
    if (target == snowdesktop::icon_override::search) return _LW("iconLibrary.searchIcon");
    if (target.starts_with(L"widget:"))
    {
        const auto index = FindWidgetIndexById(target.substr(7));
        return index < widgets_.size() ? widgets_[index].title : target.substr(7);
    }
    if (target.starts_with(L"recycle-empty:")) return _LW("iconLibrary.recycleEmpty");
    if (target.starts_with(L"recycle-full:")) return _LW("iconLibrary.recycleFull");
    const auto key = target.substr(target.find(L':') + 1);
    for (const auto& item : items_) if (ToUpperInvariant(item.layoutKey) == key) return item.name;
    return key;
}
void DesktopApp::OpenIconOverrideLibrary(const std::wstring& target)
{
    if (!IsIconOverrideTarget(target)) return;
    auto& state = EnsureIconLibrary(); state.target = target; ++state.session;
    auto route = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::IconLibrary);
    route.itemKey = target; ShowSettingsWindow(std::move(route));
}
bool DesktopApp::SetIconOverride(const std::wstring& target, const std::string& assetId)
{
    if (!IsIconOverrideTarget(target)) return false;
    if (target.starts_with(L"widget:")) return SetDockIconAsset(target.substr(7), assetId);
    auto& state = EnsureIconLibrary();
    if (!assetId.empty() && (state.busy || !snowdesktop::IsIconAssetId(assetId) ||
        std::none_of(state.snapshot.entries.begin(), state.snapshot.entries.end(),
            [&](const auto& entry) { return entry.id == assetId && !entry.missing; }))) return false;
    const auto found = iconOverrides_.find(target);
    const auto previous = found == iconOverrides_.end() ? std::string{} : found->second;
    if (assetId.empty()) iconOverrides_.erase(target); else iconOverrides_[target] = assetId;
    if (!SaveLayoutSlots())
    {
        if (previous.empty()) iconOverrides_.erase(target); else iconOverrides_[target] = previous;
        return false;
    }
    ClearDockAppearanceImages();
    InvalidateDragStaticScene(); InvalidateDockRects();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (floatingPopupHwnd_) InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
    return true;
}
ID2D1Bitmap* DesktopApp::GetSmallCustomIcon(ID2D1RenderTarget* context, std::wstring_view identity, int pixels)
{
    if (identity.empty()) return nullptr;
    const auto target = ResolveIconTarget(identity);
    const auto found = iconOverrides_.find(target);
    if (found != iconOverrides_.end())
        if (auto* custom = GetDockCustomIcon(context, found->second, pixels, identity)) return custom;
    if (snowdesktop::icon_control::UsesBitmap(identity, iconAppearanceOverrides_.contains(target), ResolveIconAppearance(identity)))
    {
        const bool light = CurrentDockAppearance().contentTheme == 1;
        const int edge = std::max(128, snowdesktop::icon_render_rules::SourcePixelsForTarget(pixels));
        const auto sourceKey = target + (light ? L":light:" : L":dark:") + std::to_wstring(edge);
        auto& source = iconControlSources_[sourceKey];
        if (!source)
        {
            source = std::make_shared<snowdesktop::BackgroundBitmap>();
            source->bitmap = snowdesktop::icon_image::CreateBitmap(snowdesktop::icon_control::MakeSource(target, light, edge));
        }
        return GetStyledD2DBitmap(context, source->bitmap, ResolveIconAppearance(identity), true, true);
    }
    return nullptr;
}
HMENU DesktopApp::AppendIconOverrideMenu(HMENU menu, const std::wstring& identity)
{
    using namespace snowdesktop::icon_override;
    const auto key = ToUpperInvariant(identity);
    const bool recycle = key.find(ToUpperInvariant(kDesktopIconClsidRecycleBin)) != std::wstring::npos;
    const auto add = [&](HMENU parent, UINT command, const std::wstring& target) {
        AppendMenuW(parent, MF_STRING, command, _LW("iconLibrary.change"));
        SetMenuItemIcon(parent, command, L"\uF489", MenuIconFont::FluentRegular);
        if (iconOverrides_.contains(target))
        {
            AppendMenuW(parent, MF_STRING, command + 1, _LW("iconLibrary.restore"));
            SetMenuItemIcon(parent, command + 1, L"\uF19A", MenuIconFont::FluentRegular);
        }
        const auto state = command == emptyChange ? 1u : command == fullChange ? 2u : 0u;
        AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(parent, MF_STRING, beautify + state * 2, _LW("iconAppearance.edit"));
        SetMenuItemIcon(parent, beautify + state * 2, L"\uF592", MenuIconFont::FluentRegular);
        if (iconAppearanceOverrides_.contains(target))
        {
            AppendMenuW(parent, MF_STRING, inherit + state * 2, _LW("iconAppearance.inherit"));
            SetMenuItemIcon(parent, inherit + state * 2, L"\uF19A", MenuIconFont::FluentRegular);
        }
    };
    const auto personal = CreatePopupMenu(); if (!personal) return nullptr;
    if(menu)
    {
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(personal), _LW("iconLibrary.personalization"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(personal), L"\uF489", MenuIconFont::FluentRegular);
    }
    if (!recycle) add(personal, change, Target(key));
    else for (int state = 0; state < 2; ++state)
    {
        const auto child = CreatePopupMenu(); if (!child) continue;
        AppendMenuW(personal, MF_POPUP, reinterpret_cast<UINT_PTR>(child),
            _LW(state ? "iconLibrary.recycleFull" : "iconLibrary.recycleEmpty"));
        SetMenuItemIcon(personal, reinterpret_cast<UINT_PTR>(child), L"\uF489", MenuIconFont::FluentRegular);
        add(child, state ? fullChange : emptyChange, Target(key, state));
    }
    return personal;
}
bool DesktopApp::ExecuteIconOverrideMenu(UINT command, const std::wstring& identity)
{
    using namespace snowdesktop::icon_override;
    if (command >= beautify && command <= inherit + 4)
    {
        const int state = command >= beautify + 4 ? 1 : command >= beautify + 2 ? 0 : -1;
        const auto target = Target(ToUpperInvariant(identity), state);
        if ((command - beautify) % 2 == 0) OpenIconAppearance(target);
        else { snowdesktop::IconAppearanceRequest request; request.action = "inherit"; request.target = target; (void)EditIconAppearance(request); }
        return true;
    }
    if (command < change || command > fullRestore) return false;
    const int state = command >= fullChange ? 1 : command >= emptyChange ? 0 : -1;
    const auto target = Target(ToUpperInvariant(identity), state);
    if ((command - change) % 2 == 0) OpenIconOverrideLibrary(target);
    else if (!SetIconOverride(target, {})) MessageBeep(MB_ICONWARNING);
    return true;
}
