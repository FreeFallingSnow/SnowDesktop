#include "icons/icon_workshop.h"
#include "icons/icon_image_codec.h"
#include "theme/theme_preview.h"
#include "icon_pack_workshop_publish.h"
#include "bridge_json.h"
#include "data/atomic_file.h"
#include <chrono>
#include <ctime>
#include <iostream>

int RunIconWorkshopWorkflowTests(const std::filesystem::path& root)
{
    using namespace snowdesktop;
    namespace bridge=steam_bridge;
    int failures=0;
    const auto check=[&](bool ok,const char* message){if(!ok){++failures;std::cerr<<"FAILED: "<<message<<'\n';}};
    const auto data=root/L"icon-workflow";
    IconLibrary library(data/L"icon-library");
    IconLibraryRequest request;request.action="createGroup";request.name=L"同名图标 / アイコン / Icons";
    auto snapshot=library.Invoke(request);const auto group=snapshot.groups.front().id;
    request={};request.action="import";request.groupId=group;request.paths={(root/L"lossless.webp").wstring()};
    check(library.Invoke(request).succeeded,"import real transparent non-square source for preview workflow");
    request={};request.action="saveSharing";request.groupId=group;request.title=L"同名图标 / アイコン / Icons";
    check(library.Invoke(request).succeeded,"optional license and unchecked rights do not block preparing a share");
    request={};request.action="preparePreview";request.groupId=group;
    const auto prepared=icon_workshop::Run(library,request,{},data,nullptr);
    check(prepared.succeeded&&!prepared.preparationId.empty(),"offline preview freezes an entire actual group");
    if(prepared.preparationId.empty())return failures+1;
    const auto directory=std::filesystem::path(prepared.coverPath).parent_path();
    icon_image::Image image;std::string error;
    check(icon_image::Decode(prepared.coverPath,1024,image,error)&&image.width==1024&&image.height==1024&&
        std::filesystem::file_size(prepared.coverPath)<1024*1024,"generated cover decodes at 1024 square below strict Steam byte limit");
    if(image.pixels.size()==1024*1024)
    {
        const auto clear=image.pixels[540*1024+200],opaque=image.pixels[540*1024+750];
        check(clear==image.pixels[20*1024+20]&&((opaque>>16)&255)>((clear>>16)&255)+8,
            "cover paints real non-square icon pixels while fully transparent margins preserve its background");
    }
    icon_package::Package package;check(library.ExportGroup(group,package,error),"export frozen group uses original image bytes");
    IconLibraryRequest sharing;sharing.action="saveSharing";sharing.groupId=group;sharing.title=L"同名图标 / アイコン / Icons";sharing.rightsConfirmed=true;
    library.Invoke(sharing);
    IconLibraryRequest stale;stale.action="publishPrepared";stale.groupId=group;stale.preparationId=prepared.preparationId;
    check(icon_workshop::Run(library,stale,{},data,nullptr).error=="iconWorkshop.stalePreparation",
        "optional sharing form changes invalidate the same preview without making confirmation mandatory");
    sharing.rightsConfirmed=false;library.Invoke(sharing);
    auto many=package;many.title=std::string(150,'W')+" 图标 アイコン 아이콘";
    while(many.images.size()<40)many.images.push_back(many.images.front());
    check(icon_workshop::RenderCover(many,data/L"many.png",error)&&std::filesystem::file_size(data/L"many.png")<1024*1024,
        "cover bounds real icon grid to 36 while rendering long multilingual title and total count");
    check(themes::preview::NormalizeCover(prepared.coverPath,data/L"custom.png",error)&&
        icon_image::Decode(data/L"custom.png",1024,image,error)&&image.width==1024&&image.height==1024,
        "custom cover uses shared normalization and retains a readable square output");
    bridge::IconPackPublishPlan plan;
    check(bridge::BuildIconPackPublishPlan(directory,data,plan,error)&&!plan.publishedFileId,
        "offline publication planning validates hashes without allocating a Steam item");
    std::string descriptor;atomic_file::ReadAll(directory/L"icon-pack-preparation.json",descriptor);
    bridge::JsonValue changed;std::string parseError;bridge::ParseJson(descriptor,changed,parseError);
    changed.object["title"]=bridge::JsonValue::String(std::string(129,'x'));
    atomic_file::WriteAll(directory/L"icon-pack-preparation.json",bridge::WriteJson(changed));
    bridge::IconPackPublishPlan oversizedTitle;
    check(!bridge::BuildIconPackPublishPlan(directory,data,oversizedTitle,error),"prepared titles enforce the existing Steam UTF-8 byte boundary");
    atomic_file::WriteAll(directory/L"icon-pack-preparation.json",descriptor);
    bridge::ThemePublishTransport transport;
    transport.status=[](auto&)->std::optional<bridge::SteamStatus>{bridge::SteamStatus s;s.loggedOn=true;s.appId=5080330;s.steamId="321";return s;};
    transport.agreement=[](auto&)->std::optional<bridge::WorkshopEulaStatus>{bridge::WorkshopEulaStatus s;s.available=s.accepted=true;return s;};
    transport.item=[](auto id,auto&)->std::optional<bridge::PublishedItem>{bridge::PublishedItem i;i.publishedFileId=id;i.ownerSteamId=321;i.consumerAppId=5080330;return i;};
    int creates=0,uploads=0;bool failUpload=true;
    transport.publish=[&](const bridge::PublishRequest& upload,const auto&,bridge::CoreError&)->std::optional<bridge::PublishResult>{
        ++uploads;auto id=upload.publishedFileId.value_or(0);
        if(!id){check(upload.prepareCreateItem&&upload.prepareCreateItem(),"persist pending creation before remote allocation");++creates;id=777;
            check(upload.persistCreatedItem(id),"persist created item ID before uploading content");}
        check(upload.contentKind==bridge::WorkshopContentKind::IconPack&&upload.tags&&*upload.tags==std::vector<std::string>{"Icon Pack"}&&
            upload.additionalPreviews.empty()&&upload.validateStagedArtifacts(upload.package,*upload.preview),"icon upload uses exact tag, own format and frozen artifact hashes");
        if(failUpload)return {};
        bridge::PublishResult result;result.publishedFileId=id;return result;
    };
    bridge::PublishResult published;bridge::CoreError detail;
    const auto now=static_cast<std::int64_t>(std::time(nullptr));
    const auto leases=data/L"IconPackWorkshop"/L"locks";std::filesystem::create_directory(leases);
    const auto leasePath=leases/(std::wstring(group.begin(),group.end())+L".lock");
    HANDLE held=CreateFileW(leasePath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(held!=INVALID_HANDLE_VALUE,"independent publisher fixture holds the same-profile file lease");
    if(held!=INVALID_HANDLE_VALUE)
    {
        check(!bridge::ExecuteIconPackPublishPlan(plan,true,false,transport,{},published,detail,now)&&detail.code=="publishBusy"&&creates==0,
            "a concurrent publisher cannot allocate another item across Windows sessions");
        CloseHandle(held);
    }
    check(!bridge::ExecuteIconPackPublishPlan(plan,false,false,transport,{},published,detail,now)&&uploads==0,
        "generating and planning cannot upload without explicit create/update confirmation");
    std::atomic_bool cancel=true;
    check(!bridge::ExecuteIconPackPublishPlan(plan,true,false,transport,{},published,detail,now,&cancel)&&creates==0,
        "cancelled or closed settings session cannot allocate a late Workshop item");
    check(!bridge::ExecuteIconPackPublishPlan(plan,true,false,transport,{},published,detail,now)&&creates==1,
        "upload failure retains a single allocated item");
    check(bridge::BuildIconPackPublishPlan(directory,data,plan,error)&&plan.publishedFileId==777&&
        bridge::IconPackPublishedUrl(data,group).find("777")!=std::string::npos,"created item binding survives upload failure");
    failUpload=false;
    check(bridge::ExecuteIconPackPublishPlan(plan,false,true,transport,{},published,detail,now)&&creates==1&&published.publishedFileId==777,
        "retry updates the allocated item without creating a duplicate");
    const auto successfulUploads=uploads;
    check(!bridge::ExecuteIconPackPublishPlan(plan,false,true,transport,{},published,detail,now+1801)&&uploads==successfulUploads,
        "expired preparation never starts an upload");
    transport.agreement=[](auto&)->std::optional<bridge::WorkshopEulaStatus>{bridge::WorkshopEulaStatus s;s.available=true;s.needsAction=true;return s;};
    check(!bridge::ExecuteIconPackPublishPlan(plan,false,true,transport,{},published,detail,now)&&detail.code=="agreementRequired"&&uploads==successfulUploads,
        "Steam's agreement remains required independently from optional icon license and rights fields");
    transport.agreement=[](auto&)->std::optional<bridge::WorkshopEulaStatus>{bridge::WorkshopEulaStatus s;s.available=s.accepted=true;return s;};
    transport.status=[](auto&)->std::optional<bridge::SteamStatus>{bridge::SteamStatus s;s.loggedOn=true;s.appId=5080330;s.steamId="999";return s;};
    const auto previousUploads=uploads;
    check(!bridge::ExecuteIconPackPublishPlan(plan,false,true,transport,{},published,detail,now)&&detail.code=="authorMismatch"&&uploads==previousUploads,
        "a different Steam account cannot update the author's bound publication");
    const auto association=plan.association;
    std::string original;atomic_file::ReadAll(directory/L"package.snowicons",original);
    atomic_file::WriteAll(directory/L"package.snowicons",original+"changed");
    check(!bridge::BuildIconPackPublishPlan(directory,data,plan,error),"changed package bytes invalidate prepared publication before Steam calls");
    atomic_file::WriteAll(directory/L"package.snowicons",original);
    atomic_file::WriteAll(association,"{\"version\":1,\"owner\":\"321\",\"publishedFileId\":\"0\",\"creationPending\":true}");
    bridge::IconPackPublishPlan uncertain;
    check(!bridge::BuildIconPackPublishPlan(directory,data,uncertain,error)&&error=="creationUncertain",
        "ambiguous allocation without an item ID blocks automatic duplicate creation");
    check(icon_workshop::Run(library,stale,{},data,nullptr).error=="iconWorkshop.creationUncertain",
        "uncertain creation reaches the UI without being collapsed into a generic stale-preview error");
    atomic_file::WriteAll(association,
        "{\"version\":1,\"owner\":\"321\",\"publishedFileId\":\"777\",\"creationPending\":false}");
    request={};request.action="renameGroup";request.groupId=group;request.name=L"Changed title";library.Invoke(request);
    request={};request.action="publishPrepared";request.groupId=group;request.preparationId=prepared.preparationId;
    check(icon_workshop::Run(library,request,{},data,nullptr).error=="iconWorkshop.stalePreparation","group edits after preview invalidate publication before checking bridge availability");

    const auto itemDirectory=data/L"download";std::filesystem::create_directory(itemDirectory);
    check(icon_package::Write(itemDirectory/L"package.snowicons",package,error),"download fixture contains only one portable artifact");
    using bridge::JsonValue;
    auto metadata=JsonValue::Object();metadata.object["format"]=JsonValue::String("snowdesktop-icon-pack");
    metadata.object["artifact"]=JsonValue::String("package.snowicons");metadata.object["packageId"]=JsonValue::String(package.id);
    metadata.object["packageSha256"]=JsonValue::String(bridge::IconPackFileHash(itemDirectory/L"package.snowicons"));
    metadata.object["iconPackWorkflowProtocolVersion"]=JsonValue::Number(1);
    auto details=JsonValue::Object();details.object["result"]=JsonValue::Number(1);details.object["banned"]=JsonValue::Boolean(false);
    details.object["consumerAppId"]=JsonValue::Number(5080330);details.object["ownerSteamId"]=JsonValue::String("321");
    details.object["metadata"]=JsonValue::String(bridge::WriteJson(metadata,-1));
    auto install=JsonValue::Object();install.object["available"]=JsonValue::Boolean(true);
    const auto folder=itemDirectory.u8string();install.object["folder"]=JsonValue::String(std::string(folder.begin(),folder.end()));
    auto item=JsonValue::Object();item.object["publishedFileId"]=JsonValue::String("777");item.object["subscribed"]=JsonValue::Boolean(true);
    item.object["installed"]=JsonValue::Boolean(true);item.object["needsUpdate"]=JsonValue::Boolean(false);item.object["details"]=details;item.object["installInfo"]=install;
    auto answer=JsonValue::Object();answer.object["ok"]=JsonValue::Boolean(true);answer.object["authoritative"]=JsonValue::Boolean(true);
    answer.object["appId"]=JsonValue::Number(5080330);answer.object["protocolVersion"]=JsonValue::Number(1);answer.object["steamId"]=JsonValue::String("321");
    answer.object["items"]=JsonValue::Array();answer.object["items"].array.push_back(item);
    IconPackSubscriptions decoded;
    check(icon_workshop::DecodeSubscriptions(bridge::WriteJson(answer,-1),decoded,error)&&decoded.downloads.size()==1,"complete installed metadata-matching package is accepted");
    answer.object["items"].array.front().object["needsUpdate"]=JsonValue::Boolean(true);
    check(icon_workshop::DecodeSubscriptions(bridge::WriteJson(answer,-1),decoded,error)&&decoded.downloads.empty()&&decoded.subscribed.contains("777"),
        "unfinished download retains subscription identity without replacing valid installed contents");
    answer.object["items"].array.front().object["needsUpdate"]=JsonValue::Boolean(false);
    atomic_file::WriteAll(itemDirectory/L"package.snowicons","damaged");std::vector<std::wstring> rejected;
    check(icon_workshop::DecodeSubscriptions(bridge::WriteJson(answer,-1),decoded,error,&rejected)&&decoded.downloads.empty()&&decoded.subscribed.contains("777")&&rejected==std::vector<std::wstring>{L"#777"},
        "corrupt package cannot be mistaken for unsubscribe");
    answer.object["items"].array.front().object["details"].object["result"]=JsonValue::Number(9);
    check(icon_workshop::DecodeSubscriptions(bridge::WriteJson(answer,-1),decoded,error,&rejected)&&decoded.subscribed.contains("777")&&decoded.deleted.empty(),
        "an unavailable/private item lookup cannot remove another account's retained subscription");
    answer.object["authoritative"]=JsonValue::Boolean(false);
    check(!icon_workshop::DecodeSubscriptions(bridge::WriteJson(answer,-1),decoded,error)&&!decoded.authoritative,"incomplete query cannot produce authoritative removals");
    check(!icon_workshop::Available(data/L"missing-bridge.exe"),"missing compatible bridge keeps Workshop entry unavailable");
    const std::string configuration=R"({"ok":true,"protocolVersion":1,"steamworksCompiled":true,"expectedAppId":5080330,"iconPackWorkflowProtocolVersion":1,"capabilities":["workshop.icon-pack.v1"]})";
    check(icon_workshop::CompatibleConfiguration(configuration),"compatible bridge capability exposes icon pack sharing");
    check(!icon_workshop::CompatibleConfiguration(R"({"ok":true,"protocolVersion":1,"steamworksCompiled":true,"expectedAppId":5080330,"capabilities":["workshop.theme.v1"]})"),
        "old bridge without independent icon workflow stays hidden");
    auto wrongApp=configuration;wrongApp.replace(wrongApp.find("5080330"),7,"480");
    check(!icon_workshop::CompatibleConfiguration(wrongApp)&&!icon_workshop::CompatibleConfiguration(configuration+"\n{\"ok\":false}\n"),
        "wrong application and terminal bridge failures do not expose forward entries");
    return failures;
}
