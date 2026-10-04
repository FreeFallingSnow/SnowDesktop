#include "theme_workshop.h"
#include "theme_preview.h"
#include "bridge_json.h"
#include "atomic_file.h"
#include "theme_workshop_tags.h"
#include <iostream>
#include <thread>
#include <future>

int RunThemeWorkshopTests(const std::filesystem::path& directory)
{
    using namespace snowdesktop;
    using namespace themes;
    namespace bridge = steam_bridge;
    int failures = 0, created = 0, uploaded = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << "FAIL theme Workshop: " << message << '\n'; } };
    std::string error;
    check(bridge::ThemeSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "production package hash matches known SHA256 vector");
    const std::string current = "{\"ok\":true,\"protocolVersion\":1,\"expectedAppId\":5080330,\"version\":\"1\",\"steamworksCompiled\":true,\"themeWorkflowProtocolVersion\":1,\"capabilities\":[\"workshop.theme.v1\",\"workshop.theme.tags.v1\",\"workshop.theme.gallery.v1\"]}";
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
    Theme root = Capture(Kind::Global,MakeAppearancePreset(kAppearancePresetDark)); root.id = "theme/workshop-root"; root.name = "Demo";
    root.quickPanel = "builtin/quickpanel/dark"; root.popup = "builtin/popup/dark";
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
        if (!id) { ++created; id=123; if (!request.persistCreatedItem(id)) return {}; }
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
    const auto installed=library.themes.begin()->first;
    check(library.references.empty() && Select(library,"global",installed,Kind::Global,All,error), "subscription import preserves explicit apply boundary");
    const auto originalSnapshot=EncodePackage(library.references.at("global").snapshot,error);
    auto edited=library.themes.at(installed); edited.appearance.widgetAlpha=.3f; std::string local;
    check(Save(library,edited,{},true,local,error,[]{return "theme/local-copy";}) && local!=installed && library.themes.at(installed).appearance.widgetAlpha!=.3f, "editing subscribed theme saves a local copy");
    auto update=snapshot; update.downloads.front().package.at(root.id).scopes=Dock;
    update.downloads.front().sha256=bridge::ThemeSha256(EncodePackage(update.downloads.front().package,error));
    check(workshop::Reconcile(library,update,error) && EncodePackage(library.references.at("global").snapshot,error)==originalSnapshot && library.themes.contains(installed), "scope-changing update preserves applied snapshot and old bindings");
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
    check(workshop::Reconcile(library,removed,error) && library.workshop.empty() && library.themes.contains(installed) &&
        EncodePackage(library.references.at("global").snapshot,error)==originalSnapshot, "unsubscribe converts managed themes to local without losing references");
    Library reload;
    check(DecodeLibrary(EncodeLibrary(library,error),reload,error), "new provenance codec round-trips old and local snapshots");
    auto deleted = snapshot; deleted.deleted = {"123"};
    check(workshop::Reconcile(library,deleted,error) && library.workshop.empty() && library.themes.contains(installed),
        "author deletion retains local values and reference snapshots");
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
