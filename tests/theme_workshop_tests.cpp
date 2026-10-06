#include "theme/theme_workshop.h"
#include "theme/theme_preview.h"
#include "bridge_json.h"
#include "data/atomic_file.h"
#include "theme/theme_workshop_tags.h"
#include "theme/theme_bridge_availability_cache.h"
#include "widget/runtime/widget_theme_selection.h"
#include <iostream>
#include <thread>
#include <future>

namespace
{
template<class Check>
void CheckAvailabilityCache(const std::filesystem::path& directory, Check check)
{
    namespace detail = snowdesktop::themes::workshop::detail;
    using Cache = detail::BridgeAvailabilityCache;
    const auto path = directory / L"cache-bridge.exe";
    check(snowdesktop::atomic_file::WriteAll(path, "offline bridge fixture"), "cache fixture is a safe regular file");
    Cache cache;
    detail::BridgeFileIdentity identity{1, 0, 1, 0, 22, 1, 1};
    const auto read = [&](const auto&) { return std::optional{identity}; };
    const auto now = Cache::Clock::time_point{};
    int probes = 0;
    const auto probe = [&] { ++probes; return detail::BridgeAvailability{true, true}; };
    for (int snapshot = 0; snapshot != 20; ++snapshot)
        check(cache.Query(path, "1", now, probe, read).sharing, "unchanged settings snapshots retain compatible sharing");
    check(probes == 1, "repeated settings snapshots launch one capability probe");
    check(cache.Query(path, "2", now, probe, read).workshop && probes == 2, "host version changes invalidate capability results");
    ++identity.indexLow;
    check(cache.Query(path, "2", now, probe, read).workshop && probes == 3, "replacement file identity invalidates cached capabilities");
    ++identity.changed;
    check(cache.Query(path, "2", now, probe, read).workshop && probes == 4, "same-size timestamp-preserved edits invalidate through change time");
    ++identity.sizeLow;
    check(cache.Query(path, "2", now, probe, read).workshop && probes == 5, "file size changes invalidate cached capabilities");
    check(cache.Query(path, "2", now + std::chrono::seconds(31), probe, read).workshop && probes == 6,
        "successful capabilities have a bounded refresh lifetime");
    std::filesystem::remove(path);
    check(!cache.Query(path, "2", now, probe, read).workshop && probes == 6, "each query rechecks the safe-path boundary even after a cache hit");
    check(snowdesktop::atomic_file::WriteAll(path, "offline bridge fixture") &&
        cache.Query(path, "2", now, probe, read).workshop && probes == 7, "delete and recreate cannot reuse an earlier cached result");

    Cache failed;
    int failures = 0;
    bool recovered = false;
    const auto retry = [&] { ++failures; return detail::BridgeAvailability{recovered, recovered}; };
    check(!failed.Query(path, "1", now, retry, read).workshop, "failed capability probe stays unavailable");
    recovered = true;
    check(!failed.Query(path, "1", now, retry, read).workshop && failures == 1, "one failed snapshot does not probe again for every selector");
    check(failed.Query(path, "1", now + std::chrono::seconds(2), retry, read).workshop && failures == 2,
        "transient probe failure recovers after a bounded retry delay");

    Cache unsupported;
    int unsupportedProbes = 0;
    const auto noIdentity = [](const auto&) -> std::optional<detail::BridgeFileIdentity> { return {}; };
    const auto compatible = [&] { ++unsupportedProbes; return detail::BridgeAvailability{true, true}; };
    check(unsupported.Query(path, "1", now, compatible, noIdentity).sharing &&
        unsupported.Query(path, "1", now, compatible, noIdentity).sharing && unsupportedProbes == 2,
        "filesystems without strong identity keep the original uncached compatible probe");

    Cache changedDuringProbe;
    check(!changedDuringProbe.Query(path, "1", now, [&] {
        ++identity.indexLow; return detail::BridgeAvailability{true, true};
    }, read).workshop && changedDuringProbe.Query(path, "1", now, probe, read).workshop,
        "a replacement during probing is not cached and the next stable query recovers");
    Cache concurrent;
    std::atomic_int concurrentProbes = 0;
    const auto concurrentProbe = [&] { ++concurrentProbes; return detail::BridgeAvailability{true, true}; };
    auto first = std::async(std::launch::async, [&] { return concurrent.Query(path, "1", now, concurrentProbe, read); });
    auto second = std::async(std::launch::async, [&] { return concurrent.Query(path, "1", now, concurrentProbe, read); });
    check(first.get().sharing && second.get().sharing && concurrentProbes == 1, "concurrent snapshot readers share one capability probe");
}

template<class Check>
void CheckWidgetThemeTransaction(const std::filesystem::path& directory,
    snowdesktop::themes::Theme theme, Check check)
{
    using namespace snowdesktop;
    using namespace themes;
    const auto path = directory / L"widget-selection.json";
    const std::string target = "widget/offline-instance";
    Library library;
    std::string error;
    library.themes.emplace(theme.id, theme);
    check(Select(library, target, "builtin/global/light", Kind::Global, Components, error) &&
        atomic_file::WriteAll(path, EncodeLibrary(library, error)), "widget-selection fixture has a prior reference");
    const auto oldTheme = Resolve(library.themes, theme.id);
    check(Transact(path, [&](auto& current, auto&) {
        current.themes.at(theme.id).appearance.widgetAlpha = .73f; return true;
    }, library, error), "a subscribed update replaces the same theme ID before widget selection");
    widget_runtime::WidgetHostAppearancePatch applied;
    bool beforeCommit = false;
    const auto selected = detail::SelectWidgetTheme(path, target, theme.id, [&](const auto& patch) {
        Library disk; std::string readError;
        beforeCommit = Load(path, disk, readError) && disk.references.at(target).id == "builtin/global/light";
        Library competing; std::string competingError;
        check(!Transact(path, [](auto&, auto&) { return true; }, competing, competingError) && competingError == "libraryBusy",
            "the selected snapshot stays locked while its host patch is applied");
        applied = patch; return detail::WidgetThemeMutation{true, true};
    }, [] { return false; }, library, error);
    const auto frozen = Resolve(library.references.at(target).snapshot, library.references.at(target).id);
    check(selected.committed && beforeCommit && frozen && applied == WidgetPatch(*frozen),
        "host patch and durable reference use the same latest snapshot and commit after mutation");
    check(oldTheme && frozen && WidgetPatch(*oldTheme) != WidgetPatch(*frozen),
        "negative control: applying the earlier separate read would detach the newly selected theme");

    const auto durable = EncodeLibrary(library, error);
    bool rollbackCalled = false;
    const auto rejected = detail::SelectWidgetTheme(path, target, "builtin/global/light",
        [](const auto&) { return detail::WidgetThemeMutation{}; }, [&] { rollbackCalled = true; return true; }, library, error);
    Library disk;
    check(!rejected.committed && !rejected.rollbackAttempted && !rollbackCalled &&
        Load(path, disk, error) && EncodeLibrary(disk, error) == durable,
        "failed guarded host mutation leaves the prior library bytes and reference untouched");

    const auto backup = directory / L"widget-selection-backup.json";
    const auto blockWrite = [&] {
        std::filesystem::rename(path, backup);
        std::filesystem::create_directory(path);
    };
    const auto restoreFile = [&] {
        std::filesystem::remove(path);
        std::filesystem::rename(backup, path);
    };
    auto host = applied;
    const auto previous = host;
    unsigned revision = 1, mutationRevision = 0;
    const auto writeFailed = detail::SelectWidgetTheme(path, target, "builtin/global/light", [&](const auto& patch) {
        host = patch; mutationRevision = ++revision; blockWrite();
        return detail::WidgetThemeMutation{true, true};
    }, [&] {
        if (revision != mutationRevision) return false;
        host = previous; ++revision; return true;
    }, library, error);
    check(!writeFailed.committed && writeFailed.rollbackAttempted && writeFailed.rollbackSucceeded && host == previous &&
        Load(backup, disk, error) && EncodeLibrary(disk, error) == durable,
        "library write failure restores only the successful guarded host revision and keeps the old reference");
    restoreFile();

    const auto concurrentUpdate = detail::SelectWidgetTheme(path, target, "builtin/global/light", [&](const auto& patch) {
        host = patch; mutationRevision = ++revision;
        host.backgroundOpacity = .19f; ++revision; blockWrite();
        return detail::WidgetThemeMutation{true, true};
    }, [&] {
        if (revision != mutationRevision) return false;
        host = previous; ++revision; return true;
    }, library, error);
    check(!concurrentUpdate.committed && concurrentUpdate.rollbackAttempted && !concurrentUpdate.rollbackSucceeded &&
        host.backgroundOpacity == .19f, "failed rollback is explicit and never overwrites a later concurrent host appearance");
    restoreFile();
    rollbackCalled = false;
    const auto unchanged = detail::SelectWidgetTheme(path, target, theme.id, [&](const auto&) {
        blockWrite(); return detail::WidgetThemeMutation{true, false};
    }, [&] { rollbackCalled = true; return true; }, library, error);
    check(!unchanged.committed && !unchanged.rollbackAttempted && !rollbackCalled, "an unchanged host mutation needs no rollback after a library failure");
    restoreFile();

    widget_runtime::WidgetHostAppearanceState appearance;
    appearance.followPersonalization = true; appearance.presetId = "prior";
    appearance.backgroundOpacity = .27f; appearance.edgeLight.direction = 120.f;
    const auto restore = detail::WidgetAppearanceSnapshotPatch(appearance);
    check(restore.followPersonalization == appearance.followPersonalization && restore.presetId == appearance.presetId &&
        restore.backgroundOpacity == appearance.backgroundOpacity && restore.edgeLight == appearance.edgeLight &&
        restore.contentTheme == appearance.contentTheme && restore.panelGradient == appearance.panelGradient,
        "rollback includes the previous source, independent foreground, edge light and gradient");
}
}

int RunThemeWorkshopTests(const std::filesystem::path& directory)
{
    using namespace snowdesktop;
    using namespace themes;
    namespace bridge = steam_bridge;
    int failures = 0, created = 0, uploaded = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << "FAIL theme Workshop: " << message << '\n'; } };
    std::string error;
    CheckAvailabilityCache(directory, check);
    check(bridge::ThemeSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "production package hash matches known SHA256 vector");
    const std::string current = "{\"ok\":true,\"protocolVersion\":1,\"expectedAppId\":5080330,\"version\":\"1\",\"steamworksCompiled\":true,\"themeWorkflowProtocolVersion\":1,\"capabilities\":[\"workshop.theme.v1\",\"workshop.theme.tags.v1\",\"workshop.theme.gallery.v1\",\"workshop.theme.color-alpha.v1\"]}";
    check(workshop::Capabilities(current,"1") && !workshop::Capabilities(current,"2"), "bridge capability and compatible version are independent requirements");
    check(workshop::Capabilities("{\"progress\":\"starting\"}\n" + current + "\n","1") &&
        !workshop::Capabilities(current + "\n{\"ok\":false}\n","1"),
        "JSON Lines retains the last result and rejects a terminal failure");
    auto old = current; old.replace(old.find("workshop.theme.v1"),17,"workshop.widget.v1");
    check(!workshop::Capabilities(old,"1") && !workshop::Capabilities("{\"ok\":true,\"version\":\"1\"}","1"), "old and missing-capability bridges do not enable sharing");
    auto missingTagsCapability = current;
    missingTagsCapability.erase(missingTagsCapability.find(",\"workshop.theme.tags.v1\""), 25);
    check(!workshop::Capabilities(missingTagsCapability,"1"), "a theme bridge without required classification support cannot silently drop tags");
    auto missingGalleryCapability = current;
    const std::string galleryCapability = ",\"workshop.theme.gallery.v1\"";
    missingGalleryCapability.erase(missingGalleryCapability.find(galleryCapability), galleryCapability.size());
    check(!workshop::Capabilities(missingGalleryCapability,"1"), "old single-cover bridges cannot silently drop the gallery");
    auto missingAlphaCapability = current;
    const std::string alphaCapability = ",\"workshop.theme.color-alpha.v1\"";
    missingAlphaCapability.erase(missingAlphaCapability.find(alphaCapability), alphaCapability.size());
    check(!workshop::Capabilities(missingAlphaCapability,"1"), "bridges without RGBA parsing cannot offer incompatible theme sharing");
    check(workshop::BridgeCapabilities(missingAlphaCapability,"1") && !workshop::BridgeCapabilities(current,"2"),
        "opening Workshop needs a compatible Steam bridge independently of theme upload capabilities");
    const std::string compiledField = "\"steamworksCompiled\":true";
    auto portableBridge = current; portableBridge.replace(portableBridge.find(compiledField), compiledField.size(), "\"steamworksCompiled\":false");
    check(!workshop::BridgeCapabilities(portableBridge,"1") && !workshop::BridgeCapabilities("{}","1") &&
        !workshop::BridgeCapabilities(current + "\n{\"ok\":false}\n","1"),
        "unavailable, SDK-free and terminally failed bridges do not expose the Workshop entry");
    Theme root = Capture(Kind::Global,MakeAppearancePreset(kAppearancePresetDark)); root.id = "theme/workshop-root"; root.name = "Demo";
    root.quickPanel = "builtin/quickpanel/dark"; root.popup = "builtin/popup/dark";
    CheckWidgetThemeTransaction(directory, root, check);
    Package package{{root.id,root}};
    const auto prepared = directory / L"prepared", data = directory / L"data";
    std::filesystem::create_directory(prepared); std::filesystem::create_directory(data);
    check(WritePackage(prepared / L"package.snowtheme",package,error), "immutable theme snapshot writes through production atomic codec");
    check(preview::SaveCover(prepared / L"cover.png",widget_preview::GenerateWallpaper(1024,1024,false),error), "bridge preview is a real encoded cover");
    for (const auto& part : preview::Parts(package, root.id, root.scopes, error))
        std::filesystem::copy_file(prepared / L"cover.png", prepared / preview::GalleryFilename(bridge::ThemeSha256(root.id), part.component));
    constexpr std::int64_t now = 1700000000;
    const auto classification = tags::Applicable(root);
    check(classification == std::vector<std::string>{"Global Theme", "Dock Theme", "Status Bar Theme", "Taskbar Theme"}, "global classification uses fixed Steamworks names for every applicable base scope");
    auto partial = root; partial.scopes = Dock | Taskbar;
    check(tags::Applicable(partial) == std::vector<std::string>{"Dock Theme", "Taskbar Theme"}, "partial multi-bar classification does not invent combination tags");
    check(!bridge::WriteThemePreparation(prepared,root.id,root.name,now,error,{}) && error=="tagsRequired", "missing mandatory classification cannot prepare a publish");
    check(!tags::Valid(root,{"Global Theme"}) && !tags::Valid(root,{"Utilities"}) && !tags::Valid(partial,{"Dock Theme","Dock Theme","Taskbar Theme"}), "missing scopes, widget tags and duplicates cannot bypass theme classification");
    check(bridge::WriteThemePreparation(prepared,root.id,root.name,now,error,classification), "preparation binds package, cover and classification hashes");
    bridge::ThemePublishPlan plan;
    check(bridge::BuildThemePublishPlan(prepared,data,plan,error) && plan.publishedFileId == 0 && plan.gallery.size() == 7, "offline theme-plan validates complete seven-image preparation without Steam");
    const auto galleryManifest = plan.gallery.front().path;
    const auto savedGallery = prepared / L"saved-gallery.png";
    std::filesystem::rename(galleryManifest, savedGallery);
    bridge::ThemePublishPlan rejectedGallery;
    check(!bridge::BuildThemePublishPlan(prepared,data,rejectedGallery,error) && error == "stalePreparation", "missing gallery image cannot prepare a publish");
    std::filesystem::rename(savedGallery, galleryManifest);
    bridge::ThemePublishTransport transport;
    transport.status = [](auto&) -> std::optional<bridge::SteamStatus> { bridge::SteamStatus s; s.loggedOn=true; s.appId=5080330; s.steamId="321"; return s; };
    transport.agreement = [](auto&) -> std::optional<bridge::WorkshopEulaStatus> { bridge::WorkshopEulaStatus s; s.available=s.accepted=true; return s; };
    transport.item = [](auto id,auto&) -> std::optional<bridge::PublishedItem> { bridge::PublishedItem i; i.publishedFileId=id; i.ownerSteamId=321; i.consumerAppId=5080330; return i; };
    bool failUpload = true;
    transport.publish = [&](const bridge::PublishRequest& request,const auto&,bridge::CoreError& detail) -> std::optional<bridge::PublishResult> {
        std::uint64_t id = request.publishedFileId.value_or(0);
        if (!id)
        {
            if (!request.prepareCreateItem || !request.prepareCreateItem()) { detail.code="writeFailed"; return {}; }
            ++created; id=123; if (!request.persistCreatedItem(id)) return {};
        }
        check(request.contentKind == bridge::WorkshopContentKind::Theme && request.validateStagedArtifacts(request.package,*request.preview), "theme transport validates exact upload snapshot");
        check(request.additionalPreviews.size() == 7 && request.validateStagedPreviews(request.additionalPreviews), "upload receives seven immutable separately hashed images");
        auto reordered = request.additionalPreviews; std::swap(reordered[0], reordered[1]);
        check(!request.validateStagedPreviews(reordered), "staged gallery identity and order cannot be substituted");
        auto expectedTags = classification; expectedTags.emplace_back("Theme");
        check(request.tags && *request.tags == expectedTags, "upload receives canonical classification, never localized labels or an unapproved source category");
        ++uploaded;
        if (failUpload) { detail.code="simulated_upload_failed"; return {}; }
        return bridge::PublishResult{!request.publishedFileId,id,false,{}};
    };
    bridge::CoreError detail; bridge::PublishResult result;
    auto changedTags = plan; changedTags.tags.pop_back();
    check(!bridge::ExecuteThemePublishPlan(changedTags,true,false,transport,{},result,detail,now) && detail.code=="stalePreparation" && created==0,
        "classification changed after confirmation cannot reach Steam");
    auto changedGallery = plan; changedGallery.gallery.front().sha256 = "changed";
    check(!bridge::ExecuteThemePublishPlan(changedGallery,true,false,transport,{},result,detail,now) && detail.code=="stalePreparation" && created==0,
        "gallery changed after confirmation cannot reach Steam");
    check(atomic_file::WriteAll(galleryManifest,"tampered") &&
        !bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) && detail.code=="stalePreparation" && created==0,
        "tampered gallery file is rejected before the Steam boundary");
    std::filesystem::copy_file(prepared / L"cover.png", galleryManifest, std::filesystem::copy_options::overwrite_existing);
    check(!bridge::ExecuteThemePublishPlan(plan,false,false,transport,{},result,detail,now) && created==0, "confirmation is required before any remote create");
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now+901) && created==0, "expired preparation never reaches Steam");
    std::atomic_bool cancel{true};
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now,&cancel) && created==0, "cancel before publish has no remote effect");
    const auto accepted = transport.agreement;
    transport.agreement = [](auto&) -> std::optional<bridge::WorkshopEulaStatus> { bridge::WorkshopEulaStatus s; s.available=true; s.needsAction=true; return s; };
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) && detail.code=="agreementRequired" && created==0, "unaccepted agreement is surfaced without accepting it");
    transport.agreement=accepted;
    const auto normalPublish = transport.publish;
    transport.publish = [](const auto&,const auto&,bridge::CoreError& failure) -> std::optional<bridge::PublishResult> {
        failure.code="invalid_package"; return {};
    };
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) &&
        detail.code=="invalid_package" && !std::filesystem::exists(plan.association) &&
        bridge::BuildThemePublishPlan(prepared,data,plan,error) && plan.publishedFileId==0,
        "local core rejection before CreateItem writes no uncertain journal and leaves retry available");
    transport.publish = normalPublish;
    std::filesystem::create_directories(plan.association.parent_path());
    std::filesystem::create_directory(plan.association);
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) &&
        detail.code=="stalePreparation" && created==0, "an unreadable journal cannot create a remote item");
    std::filesystem::remove(plan.association);
    transport.publish = [&](const auto& request,const auto&,bridge::CoreError& failure) -> std::optional<bridge::PublishResult> {
        std::filesystem::create_directory(plan.association);
        check(request.prepareCreateItem && !request.prepareCreateItem(), "journal write failure is reported at the creation boundary");
        failure.code="writeFailed"; return {};
    };
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) && detail.code=="writeFailed" && created==0,
        "a failed pending-journal write prevents remote creation");
    std::filesystem::remove(plan.association);
    transport.publish = [&](const auto& request,const auto&,bridge::CoreError& failure) -> std::optional<bridge::PublishResult> {
        check(request.prepareCreateItem && request.prepareCreateItem(), "creation boundary durably records pending before the remote call");
        failure.code="create_item_failed"; return {};
    };
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) &&
        !bridge::BuildThemePublishPlan(prepared,data,rejectedGallery,error) && error=="creationUncertain",
        "a lost response after entering the remote creation boundary remains blocked against duplicate creation");
    std::filesystem::remove(plan.association);
    transport.publish = normalPublish;
    check(!bridge::ExecuteThemePublishPlan(plan,true,false,transport,{},result,detail,now) && result.publishedFileId==123 && created==1, "created ID is durable even if upload fails");
    check(bridge::BuildThemePublishPlan(prepared,data,plan,error) && plan.publishedFileId==123, "retry reloads durable association");
    check(bridge::ThemePublishedUrl(data, root.id) == "https://steamcommunity.com/sharedfiles/filedetails/?id=123" &&
        bridge::ThemePublishedUrl(data,"theme/new-id").empty(), "published address binds to local ID, including a failed upload retry, and never leaks to another theme");
    failUpload=false;
    check(bridge::ExecuteThemePublishPlan(plan,false,true,transport,{},result,detail,now) && created==1 && uploaded==2, "retry updates existing ID without duplicate creation");
    auto noAppAgreement = transport;
    noAppAgreement.agreement = [](auto&) -> std::optional<bridge::WorkshopEulaStatus> { return bridge::WorkshopEulaStatus{}; };
    bool noAgreementReached = false;
    noAppAgreement.publish = [&](const auto&,const auto&,auto&) -> std::optional<bridge::PublishResult> { noAgreementReached = true; return bridge::PublishResult{false,123,false,{}}; };
    check(bridge::ExecuteThemePublishPlan(plan,false,true,noAppAgreement,{},result,detail,now) && noAgreementReached,
        "apps without a custom Workshop EULA reach publish and rely on authoritative Steam callbacks");
    const auto managedPrefix = preview::GalleryPrefix(bridge::ThemeSha256(root.id));
    check(preview::ManagedGalleryFilename(managedPrefix + "popup.png", managedPrefix) &&
        !preview::ManagedGalleryFilename("manual-preview.png", managedPrefix) &&
        !preview::ManagedGalleryFilename(preview::GalleryFilename(bridge::ThemeSha256("other"),"popup"),managedPrefix) &&
        !preview::ManagedGalleryFilename(managedPrefix + "unrecognized.png", managedPrefix),
        "gallery replacement preserves manual files and other theme identities");
    const auto owner = transport.item;
    transport.item = [](auto id,auto&) -> std::optional<bridge::PublishedItem> { bridge::PublishedItem i; i.publishedFileId=id; i.ownerSteamId=999; i.consumerAppId=5080330; return i; };
    check(!bridge::ExecuteThemePublishPlan(plan,false,true,transport,{},result,detail,now) && detail.code=="authorMismatch" && uploaded==2, "another account cannot update the authored item");
    transport.item=owner;
    transport.item = [](auto id,auto&) -> std::optional<bridge::PublishedItem> {
        bridge::PublishedItem i; i.publishedFileId=id; i.ownerSteamId=321; i.consumerAppId=5080330;
        i.metadata="{\"format\":\"snowdesktop-widget\",\"themeId\":\"another\",\"themeWorkflowProtocolVersion\":1}"; return i;
    };
    check(!bridge::ExecuteThemePublishPlan(plan,false,true,transport,{},result,detail,now) && detail.code=="itemMismatch" && uploaded==2,
        "owned widget or another theme cannot be overwritten through a theme association");
    transport.item=owner;
    auto lockPath=plan.association;lockPath+=L".lock";
    HANDLE lock=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(!bridge::ExecuteThemePublishPlan(plan,false,true,transport,{},result,detail,now) && detail.code=="publicationBusy", "duplicate concurrent publication is blocked by the real association lock");
    if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
    auto corrupted=package; corrupted.at(root.id).name="Changed";
    check(WritePackage(plan.package,corrupted,error) && !bridge::ExecuteThemePublishPlan(plan,false,true,transport,{},result,detail,now) && detail.code=="stalePreparation", "post-confirmation package mutation cannot upload an old cover");
    check(WritePackage(plan.package,package,error), "restore fixture package");
    auto response = bridge::JsonValue::Object(), entry = bridge::JsonValue::Object(), metadata = bridge::JsonValue::Object();
    response.object["ok"] = bridge::JsonValue::Boolean(true); response.object["authoritative"] = bridge::JsonValue::Boolean(true);
    response.object["protocolVersion"] = bridge::JsonValue::Number(1); response.object["appId"] = bridge::JsonValue::Number(5080330);
    response.object["steamId"] = bridge::JsonValue::String("321");
    entry.object["publishedFileId"] = bridge::JsonValue::String("123"); entry.object["subscribed"] = bridge::JsonValue::Boolean(true);
    entry.object["installed"] = bridge::JsonValue::Boolean(true); entry.object["needsUpdate"] = bridge::JsonValue::Boolean(false);
    metadata.object["format"] = bridge::JsonValue::String("snowdesktop-theme"); metadata.object["artifact"] = bridge::JsonValue::String("package.snowtheme");
    metadata.object["themeWorkflowProtocolVersion"] = bridge::JsonValue::Number(1); metadata.object["themeId"] = bridge::JsonValue::String(root.id);
    metadata.object["packageSha256"] = bridge::JsonValue::String(bridge::ThemeFileSha256(plan.package));
    auto details = bridge::JsonValue::Object(); details.object["result"] = bridge::JsonValue::Number(1);
    details.object["consumerAppId"] = bridge::JsonValue::Number(5080330); details.object["banned"] = bridge::JsonValue::Boolean(false);
    details.object["ownerSteamId"] = bridge::JsonValue::String("321"); details.object["metadata"] = bridge::JsonValue::String(bridge::WriteJson(metadata));
    std::string remoteTags = "Theme"; for (const auto& tag : classification) remoteTags += "," + tag;
    details.object["tags"] = bridge::JsonValue::String(remoteTags);
    auto bindingStatus = bridge::JsonValue::Object(); bindingStatus.object["ok"] = bridge::JsonValue::Boolean(true);
    bindingStatus.object["loggedOn"] = bridge::JsonValue::Boolean(true); bindingStatus.object["appId"] = bridge::JsonValue::Number(5080330);
    bindingStatus.object["steamId"] = bridge::JsonValue::String("321");
    auto bindingItem = bridge::JsonValue::Object(); bindingItem.object["ok"] = bridge::JsonValue::Boolean(true);
    bindingItem.object["publishedFileId"] = bridge::JsonValue::String("123"); bindingItem.object["details"] = details;
    const auto bindingData = directory / L"binding-data"; std::filesystem::create_directory(bindingData);
    auto localRoot = root; localRoot.id = "theme/local-binding";
    check(workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(bindingItem), error) &&
        bridge::ThemePublishedUrl(bindingData, localRoot.id) == "https://steamcommunity.com/sharedfiles/filedetails/?id=123",
        "nested production item-details response verifies ownership and binds an existing authored theme to a different local UUID without publishing");
    const auto bindingFile = bindingData / L"ThemeWorkshop" / (bridge::ThemeSha256(localRoot.id) + ".json");
    std::string beforeBinding; atomic_file::ReadAll(bindingFile, beforeBinding);
    auto wrongBinding = bindingItem; wrongBinding.object["details"].object["ownerSteamId"] = bridge::JsonValue::String("999");
    check(!workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(wrongBinding), error) &&
        error == "authorMismatch", "another owner's item cannot establish or replace a local publication binding");
    wrongBinding = bindingItem; wrongBinding.object["details"].object["tags"] = bridge::JsonValue::String("Theme,Popup Theme");
    check(!workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(wrongBinding), error) &&
        error == "itemMismatch", "an authored item of another theme category cannot be bound for overwrite");
    std::atomic_bool bindCancelled{true};
    check(!workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(bindingItem), error, &bindCancelled) &&
        error == "cancelled", "cancelled ownership verification leaves the existing binding unchanged");
    auto bindingLock = bindingFile; bindingLock += L".lock";
    const auto heldBindingLock = CreateFileW(bindingLock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(heldBindingLock != INVALID_HANDLE_VALUE && !workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(bindingItem), error) &&
        error == "publicationBusy", "binding and uploading share the same exclusive journal lock");
    if (heldBindingLock != INVALID_HANDLE_VALUE) CloseHandle(heldBindingLock);
    std::string afterBinding; atomic_file::ReadAll(bindingFile, afterBinding);
    check(beforeBinding == afterBinding, "wrong owner, wrong category, cancellation and lock failure do not rewrite the prior binding");
    const auto localPreparation = directory / L"bound-prepared";
    std::filesystem::create_directory(localPreparation);
    Package boundPackage{{localRoot.id, localRoot}};
    check(WritePackage(localPreparation / L"package.snowtheme", boundPackage, error), "bound local UUID has its own immutable package");
    std::filesystem::copy_file(prepared / L"cover.png", localPreparation / L"cover.png");
    for (const auto& part : preview::Parts(boundPackage, localRoot.id, localRoot.scopes, error))
        std::filesystem::copy_file(prepared / L"cover.png", localPreparation / preview::GalleryFilename(bridge::ThemeSha256(localRoot.id), part.component));
    bridge::ThemePublishPlan boundPlan;
    check(bridge::WriteThemePreparation(localPreparation, localRoot.id, localRoot.name, now, error, classification) &&
        bridge::BuildThemePublishPlan(localPreparation, bindingData, boundPlan, error) && boundPlan.publishedFileId == 123,
        "a verified existing-item binding prepares an update rather than creating another Steam item");
    auto boundTransport = transport;
    boundTransport.item = [&](auto id, auto&) -> std::optional<bridge::PublishedItem> {
        bridge::PublishedItem item; item.publishedFileId = id; item.ownerSteamId = 321; item.consumerAppId = 5080330;
        item.metadata = bridge::WriteJson(metadata); return item;
    };
    bool replacedBothIdentities = false;
    boundTransport.publish = [&](const bridge::PublishRequest& request, const auto&, auto&) -> std::optional<bridge::PublishResult> {
        const auto localPrefix = preview::GalleryPrefix(bridge::ThemeSha256(localRoot.id));
        replacedBothIdentities = request.publishedFileId == 123 && request.managedPreviewPrefix == localPrefix &&
            request.previousManagedPreviewPrefix == managedPrefix &&
            preview::ReplaceableGalleryFilename(managedPrefix + "control-panel.png", request.managedPreviewPrefix, request.previousManagedPreviewPrefix) &&
            preview::ReplaceableGalleryFilename(localPrefix + "popup.png", request.managedPreviewPrefix, request.previousManagedPreviewPrefix) &&
            !preview::ReplaceableGalleryFilename("manual-preview.png", request.managedPreviewPrefix, request.previousManagedPreviewPrefix) &&
            !preview::ReplaceableGalleryFilename(preview::GalleryFilename(bridge::ThemeSha256("unrelated"), "popup"), request.managedPreviewPrefix, request.previousManagedPreviewPrefix) &&
            request.validateStagedPreviews(request.additionalPreviews);
        return bridge::PublishResult{false, 123, false, {}};
    };
    check(bridge::ExecuteThemePublishPlan(boundPlan, false, true, boundTransport, {}, result, detail, now) && replacedBothIdentities,
        "updating a different local UUID removes only current and verified previous generated galleries, preserving manual and unrelated images");
    std::string publicationBefore; atomic_file::ReadAll(bindingFile, publicationBefore);
    const auto unbindLock = CreateFileW(bindingLock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(unbindLock != INVALID_HANDLE_VALUE && !bridge::UnbindThemePublication(bindingData, localRoot.id, error) && error == "publicationBusy",
        "offline unbinding cannot race a binding or publication holding the same journal lock");
    if (unbindLock != INVALID_HANDLE_VALUE) CloseHandle(unbindLock);
    atomic_file::ReadAll(bindingFile, afterBinding);
    check(afterBinding == publicationBefore, "a busy unbind leaves the authored association unchanged");
    const auto boundPackageBefore = bridge::ThemeFileSha256(localPreparation / L"package.snowtheme");
    const auto otherUrl = bridge::ThemePublishedUrl(data, root.id);
    check(bridge::UnbindThemePublication(bindingData, localRoot.id, error) && bridge::ThemePublishedUrl(bindingData, localRoot.id).empty() &&
        bridge::ThemeFileSha256(localPreparation / L"package.snowtheme") == boundPackageBefore && bridge::ThemePublishedUrl(data, root.id) == otherUrl,
        "unbinding clears only the local publication link without changing the theme package or another association");
    const auto uploadsBeforeUnbind = uploaded;
    check(!bridge::ExecuteThemePublishPlan(boundPlan, false, true, boundTransport, {}, result, detail, now) && detail.code == "confirmationRequired" &&
        uploaded == uploadsBeforeUnbind && bridge::BuildThemePublishPlan(localPreparation, bindingData, boundPlan, error) && boundPlan.publishedFileId == 0,
        "an old update confirmation cannot publish after unbinding; the next plan requires creation confirmation");
    check(bridge::UnbindThemePublication(bindingData, localRoot.id, error) &&
        workshop::BindResponses(bindingData, localRoot, "123", bridge::WriteJson(bindingStatus), bridge::WriteJson(bindingItem), error) &&
        bridge::ThemePublishedUrl(bindingData, localRoot.id) == "https://steamcommunity.com/sharedfiles/filedetails/?id=123",
        "repeated unbinding is harmless and the authored item can be explicitly rebound");
    check(!bridge::UnbindThemePublication(bindingData, "", error) && error == "invalidSelection" &&
        !bridge::UnbindThemePublication(bindingData, "builtin/popup/dark", error) && error == "invalidSelection",
        "empty and builtin identities cannot clear a publication journal");
    entry.object["details"] = details;
    const auto downloaded = directory / L"downloaded"; std::filesystem::create_directory(downloaded);
    check(WritePackage(downloaded / L"package.snowtheme",package,error), "download fixture contains only the published artifact");
    auto install = bridge::JsonValue::Object(); install.object["available"] = bridge::JsonValue::Boolean(true);
    const auto utf8 = downloaded.generic_u8string(); install.object["folder"] = bridge::JsonValue::String(std::string(utf8.begin(),utf8.end()));
    entry.object["installInfo"] = install; response.object["items"] = bridge::JsonValue::Array(); response.object["items"].array.push_back(entry);
    workshop::SubscriptionSnapshot decoded;
    check(workshop::DecodeSubscriptions(bridge::WriteJson(response),decoded,error) && decoded.downloads.size()==1,
        "bridge response decoder validates ready folder, metadata and whole package hash");
    response.object["items"].array.front().object["needsUpdate"] = bridge::JsonValue::Boolean(true);
    check(workshop::DecodeSubscriptions(bridge::WriteJson(response),decoded,error) && decoded.downloads.empty() && decoded.subscribed.contains("123"),
        "pending download preserves subscription without replacing installed snapshot");
    response.object["authoritative"] = bridge::JsonValue::Boolean(false);
    check(!workshop::DecodeSubscriptions(bridge::WriteJson(response),decoded,error), "incomplete enumeration cannot reconcile installations");
    Library library; workshop::SubscriptionSnapshot snapshot; snapshot.authoritative=true; snapshot.account="321"; snapshot.subscribed={"123"};
    snapshot.downloads.push_back({"123","321",bridge::ThemeFileSha256(plan.package),package});
    check(workshop::Reconcile(library,snapshot,error), "validated subscription installs without applying a theme");
    const auto installed=std::find_if(library.themes.begin(), library.themes.end(), [](const auto& pair) { return pair.second.kind == Kind::Global; })->first;
    check(library.references.empty() && Select(library,"global",installed,Kind::Global,All,error), "subscription import preserves explicit apply boundary");
    const auto subscribedBytes = EncodeLibrary(library, error);
    Library copyLibrary = library; std::string copied;
    auto customQuick = Capture(Kind::QuickPanel, MakeAppearancePreset(kAppearancePresetDark));
    customQuick.id = "theme/subscribed-quick"; customQuick.name = "Subscribed quick";
    auto customPopup = Capture(Kind::Popup, MakeAppearancePreset(kAppearancePresetDark));
    customPopup.id = "theme/subscribed-popup"; customPopup.name = "Subscribed popup";
    copyLibrary.themes.emplace(customQuick.id, customQuick);
    copyLibrary.themes.emplace(customPopup.id, customPopup);
    copyLibrary.themes.at(installed).quickPanel = customQuick.id;
    copyLibrary.themes.at(installed).popup = customPopup.id;
    copyLibrary.workshop.at("123").ids.insert(customQuick.id);
    copyLibrary.workshop.at("123").ids.insert(customPopup.id);
    const auto sourceIds = copyLibrary.workshop.at("123").ids;
    const auto originalCopySource = EncodePackage(copyLibrary.themes, error);
    check(workshop::CopyLocal(copyLibrary, installed, copied, error) && copied != installed &&
        copyLibrary.themes.at(copied).quickPanel != copyLibrary.themes.at(installed).quickPanel &&
        copyLibrary.themes.at(copied).popup != copyLibrary.themes.at(installed).popup &&
        copyLibrary.themes.at(copyLibrary.themes.at(copied).quickPanel).appearance == customQuick.appearance &&
        copyLibrary.themes.at(copyLibrary.themes.at(copied).popup).appearance == customPopup.appearance &&
        copyLibrary.workshop.at("123").ids == sourceIds && EncodeLibrary(library, error) == subscribedBytes,
        "copy-to-local duplicates the complete dependency closure with new IDs without editing the subscribed source");
    Package unchangedSource;
    for (const auto& [id, theme] : copyLibrary.themes)
        if (id == installed || id == customQuick.id || id == customPopup.id) unchangedSource.emplace(id, theme);
    check(EncodePackage(unchangedSource, error) == originalCopySource,
        "copying remaps only the new closure and leaves every original subscribed dependency byte-for-byte unchanged");
    auto copiedTheme = copyLibrary.themes.at(copied); copiedTheme.appearance.widgetAlpha = .17f;
    std::string updatedCopy;
    check(Save(copyLibrary, copiedTheme, {}, true, updatedCopy, error) && updatedCopy == copied &&
        copyLibrary.themes.at(installed).appearance.widgetAlpha != .17f,
        "a newly created local copy can subsequently update its own ID without changing the Workshop subscription");
    Library sameLocal; sameLocal.themes = package;
    check(workshop::Reconcile(sameLocal, snapshot, error) && sameLocal.themes.at(root.id).quickPanel == root.quickPanel &&
        !sameLocal.workshop.at("123").ids.contains(root.id) && !sameLocal.workshop.at("123").ids.contains(root.quickPanel) &&
        !sameLocal.workshop.at("123").ids.contains(root.popup),
        "subscribing to an identical authored package never turns the editable local root or children into subscriptions");
    const auto isolatedId = *sameLocal.workshop.at("123").ids.begin();
    check(sameLocal.workshop.at("123").sourceIds.at(isolatedId) == root.id,
        "isolated subscription retains authored UUID for management grouping without aliasing local data");
    Library groupedReload;
    check(DecodeLibrary(EncodeLibrary(sameLocal,error),groupedReload,error) &&
        groupedReload.workshop.at("123").sourceIds == sameLocal.workshop.at("123").sourceIds,
        "authored UUID mapping round-trips through the production local library codec");
    bridge::JsonValue legacyOrigins; check(bridge::ParseJson(EncodeLibrary(sameLocal,error),legacyOrigins,error), "origin fixture is valid JSON");
    legacyOrigins.object["workshop"].object["123"].object.erase("sourceIds");
    Library legacyLibrary;
    check(DecodeLibrary(bridge::WriteJson(legacyOrigins),legacyLibrary,error) && legacyLibrary.workshop.at("123").sourceIds.empty(),
        "existing libraries without original identity mapping remain readable without changing theme package format");
    const auto legacyThemes = EncodePackage(legacyLibrary.themes,error);
    check(workshop::Reconcile(legacyLibrary,snapshot,error) && legacyLibrary.workshop.at("123").sourceIds.at(isolatedId) == root.id &&
        EncodePackage(legacyLibrary.themes,error) == legacyThemes,
        "refresh restores missing origin mapping from a matching verified package without rewriting any theme values or IDs");
    auto incompatibleLegacy = sameLocal; incompatibleLegacy.workshop.at("123").sourceIds.clear();
    incompatibleLegacy.themes.at(isolatedId).appearance.widgetAlpha = .321f;
    check(workshop::Reconcile(incompatibleLegacy,snapshot,error) && incompatibleLegacy.workshop.at("123").sourceIds.empty(),
        "legacy provenance is not guessed when the installed material differs from the verified source");
    auto malformedOrigin = legacyOrigins;
    auto identityMap = bridge::JsonValue::Object(); identityMap.object[isolatedId] = bridge::JsonValue::String("builtin/popup/dark");
    malformedOrigin.object["workshop"].object["123"].object["sourceIds"] = identityMap;
    const auto beforeMalformedOrigin = EncodeLibrary(groupedReload,error);
    check(!DecodeLibrary(bridge::WriteJson(malformedOrigin),groupedReload,error) && EncodeLibrary(groupedReload,error) == beforeMalformedOrigin,
        "malformed original identity cannot replace a valid library");
    auto graphRoot = root; graphRoot.quickPanel = customQuick.id; graphRoot.popup = customPopup.id;
    Package authoredGraph{{graphRoot.id,graphRoot},{customQuick.id,customQuick},{customPopup.id,customPopup}};
    auto graphSnapshot = snapshot; graphSnapshot.downloads.front().package = authoredGraph;
    graphSnapshot.downloads.front().sha256 = bridge::ThemeSha256(EncodePackage(authoredGraph,error));
    Library graphLibrary; graphLibrary.themes = authoredGraph;
    check(workshop::Reconcile(graphLibrary,graphSnapshot,error) && graphLibrary.workshop.at("123").sourceIds.size() == 3,
        "a global package retains all three authored identities while isolating every subscribed node");
    const auto graphIds = graphLibrary.workshop.at("123").sourceIds;
    graphLibrary.workshop.at("123").sourceIds.clear(); const auto beforeGraphRecovery = EncodePackage(graphLibrary.themes,error);
    check(workshop::Reconcile(graphLibrary,graphSnapshot,error) && graphLibrary.workshop.at("123").sourceIds == graphIds &&
        EncodePackage(graphLibrary.themes,error) == beforeGraphRecovery,
        "legacy graph recovery verifies child edges and restores the complete global, popup and quick-panel identity map atomically");
    const auto originalSnapshot=EncodePackage(library.references.at("global").snapshot,error);
    auto edited=library.themes.at(installed); edited.appearance.widgetAlpha=.3f; std::string local;
    check(Save(library,edited,{},true,local,error,[]{return "theme/local-copy";}) && local!=installed && library.themes.at(installed).appearance.widgetAlpha!=.3f, "editing subscribed theme saves a local copy");
    auto update=snapshot; update.downloads.front().package.at(root.id).scopes=Dock;
    update.downloads.front().sha256=bridge::ThemeSha256(EncodePackage(update.downloads.front().package,error));
    check(workshop::Reconcile(library,update,error) && EncodePackage(library.references.at("global").snapshot,error)==originalSnapshot &&
        library.themes.contains(installed) && library.themes.at(installed).scopes == Dock && library.references.at("global").id.empty() &&
        library.workshop.at("123").ids.size() == 1,
        "scope-changing update retains one installed identity and detaches excluded active references with their original snapshot intact");
    const auto before=EncodeLibrary(library,error); auto failed=update; failed.authoritative=false; failed.subscribed.clear();
    check(!workshop::Reconcile(library,failed,error) && EncodeLibrary(library,error)==before, "query failure cannot remove installed contents");
    auto invalid=update; invalid.downloads.front().package.at(root.id).quickPanel="theme/missing";
    check(!workshop::Reconcile(library,invalid,error) && EncodeLibrary(library,error)==before, "missing dependency rolls back whole subscription transaction");
    auto wrongAuthor=update; wrongAuthor.downloads.front().owner="999";
    check(!workshop::Reconcile(library,wrongAuthor,error) && EncodeLibrary(library,error)==before, "changed author cannot overwrite known provenance");
    Library conflictLibrary; auto other=root; other.name="Different"; conflictLibrary.themes.emplace(root.id,other);
    const auto conflictBefore=EncodeLibrary(conflictLibrary,error);
    check(!workshop::Reconcile(conflictLibrary,snapshot,error,[&]{return root.id;}) && EncodeLibrary(conflictLibrary,error)==conflictBefore,
        "unresolvable identity conflict rolls back package and provenance together");
    auto removed=update; removed.subscribed.clear(); removed.downloads.clear();
    check(workshop::Reconcile(library,removed,error) && library.workshop.empty() && !library.themes.contains(installed) &&
        library.themes.size() == 1 && library.themes.contains(local) && library.references.at("global").id.empty() &&
        EncodePackage(library.references.at("global").snapshot,error)==originalSnapshot,
        "unsubscribe removes installed items while preserving the explicit local copy and complete active appearance snapshot");
    Library reload;
    check(DecodeLibrary(EncodeLibrary(library,error),reload,error), "new provenance codec round-trips old and local snapshots");
    auto deleted = snapshot; deleted.deleted = {"123"}; deleted.downloads.clear();
    auto deletedLibrary = sameLocal;
    check(workshop::Reconcile(deletedLibrary,deleted,error) && deletedLibrary.workshop.empty() &&
        EncodePackage(deletedLibrary.themes,error) == EncodePackage(package,error),
        "author deletion removes the subscribed version while preserving the preinstallation local project");
    auto preexisting = graphLibrary;
    auto graphRemoved = graphSnapshot; graphRemoved.subscribed.clear(); graphRemoved.downloads.clear();
    check(workshop::Reconcile(preexisting,graphRemoved,error) && preexisting.workshop.empty() &&
        EncodePackage(preexisting.themes,error) == EncodePackage(authoredGraph,error),
        "unsubscribe removes all three isolated nodes and retains the exact preinstallation local dependency graph");
    auto multiAccount = graphLibrary; auto secondAccount = graphSnapshot; secondAccount.account = "654";
    check(workshop::Reconcile(multiAccount,secondAccount,error) && workshop::Reconcile(multiAccount,graphRemoved,error) &&
        multiAccount.workshop.at("123").accounts == std::set<std::string>{"654"} && multiAccount.themes.size() == 6,
        "one account unsubscribing cannot delete a version still subscribed by another known account");
    auto secondRemoved = graphRemoved; secondRemoved.account = "654";
    check(workshop::Reconcile(multiAccount,secondRemoved,error) && multiAccount.workshop.empty() && multiAccount.themes.size() == 3,
        "the final account unsubscribing removes the complete installed graph without leaving local residue");
    auto boundLocal = graphLibrary;
    auto localParent = graphRoot; localParent.id = "theme/local-dependent";
    for (const auto& [id, authored] : graphIds)
    { if (authored == customQuick.id) localParent.quickPanel = id; if (authored == customPopup.id) localParent.popup = id; }
    boundLocal.themes.emplace(localParent.id,localParent);
    check(Select(boundLocal,"global",localParent.id,Kind::Global,All,error),"local global can explicitly bind installed children");
    const auto boundSnapshot = EncodePackage(boundLocal.references.at("global").snapshot,error);
    const auto beforeRetirement = EncodeLibrary(boundLocal,error);
    check(!workshop::Reconcile(boundLocal,graphRemoved,error,[&]{return graphRoot.id;}) &&
        EncodeLibrary(boundLocal,error) == beforeRetirement,
        "failure to preserve a local parent's needed child rolls back removal and provenance atomically");
    check(workshop::Reconcile(boundLocal,graphRemoved,error) && boundLocal.workshop.empty() &&
        boundLocal.themes.at(localParent.id).quickPanel != localParent.quickPanel &&
        boundLocal.themes.at(boundLocal.themes.at(localParent.id).quickPanel).appearance == customQuick.appearance &&
        boundLocal.references.at("global").id.empty() && EncodePackage(boundLocal.references.at("global").snapshot,error) == boundSnapshot &&
        Validate(boundLocal.themes,error) && std::none_of(graphIds.begin(),graphIds.end(),[&](const auto& entry){return boundLocal.themes.contains(entry.first);}),
        "unsubscribe deletes installed IDs, retains only dependencies needed by an existing local parent and preserves its active snapshot");
    bridge::ThemePublishPlan rejected;
    check(!workshop::Prepare(package,root.id,All,directory / L"bad-render",data,{},
        [](const auto&,auto,unsigned,const auto& out,auto& cover,auto&,auto*) {
            std::filesystem::create_directory(out); cover=out / L"cover.png"; return false;
        },rejected,error,nullptr,classification) && rejected.package.empty() && !std::filesystem::exists(directory / L"bad-render"),
        "failed preparation releases its request directory and returns no reusable plan");
    const auto conflict = [](const Package& into, const std::filesystem::path& out, std::filesystem::path& cover, std::string& detail) {
        std::filesystem::create_directory(out); auto changed=into; changed.begin()->second.name="Other";
        cover=out / L"cover.png";
        return WritePackage(out / L"package.snowtheme",changed,detail) &&
            preview::SaveCover(cover,widget_preview::GenerateWallpaper(1024,1024,false),detail);
    };
    check(!workshop::Prepare(package,root.id,All,directory / L"mismatched-render",data,{},
        [&](const auto& into,auto,unsigned,const auto& out,auto& cover,auto& detail,auto*) { return conflict(into,out,cover,detail); },
        rejected,error,nullptr,classification) && error=="stalePreparation" && rejected.package.empty(), "negative control rejects renderer/package snapshot mismatch");
    std::string association;
    check(atomic_file::ReadAll(plan.association,association), "durable journal can be read");
    check(atomic_file::WriteAll(plan.association,"{\"version\":1,\"owner\":\"321\",\"publishedFileId\":\"0\",\"creationPending\":true}") &&
        !bridge::BuildThemePublishPlan(prepared,data,plan,error) && error=="creationUncertain", "unknown creation outcome never silently creates another item");
    return failures;
}
