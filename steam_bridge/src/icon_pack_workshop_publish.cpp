// SPDX-License-Identifier: MIT
#include "icon_pack_workshop_publish.h"
#include "bridge_json.h"
#include "workshop_upload_validation.h"
#include "steam_app_identity.h"
#include "../../src/data/atomic_file.h"
#include <windows.h>
#include <fstream>

namespace snowdesktop::steam_bridge
{
namespace
{
bool Fail(CoreError& error,const char* code){error={kSteamOperationFailed,code,code,{}};return false;}
std::string Read(const std::filesystem::path& path,std::uint64_t maximum)
{
    if(!ThemeSafePath(path))return {};
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
    if(ec||!size||size>maximum)return {};
    std::ifstream input(path,std::ios::binary);std::string bytes(static_cast<std::size_t>(size),'\0');
    if(!input.read(bytes.data(),static_cast<std::streamsize>(size))||input.peek()!=EOF)return {};
    return bytes;
}
bool Association(const std::filesystem::path& path,std::string& owner,std::uint64_t& id,bool& pending)
{
    std::error_code ec;if(!std::filesystem::exists(path,ec))return !ec;
    JsonValue value;std::string error;
    if(!ParseJson(Read(path,4096),value,error)||JsonUnsigned(value,"version")!=1)return false;
    owner=JsonString(value,"owner").value_or("");
    const auto text=JsonString(value,"publishedFileId").value_or("");
    const auto converted=std::from_chars(text.data(),text.data()+text.size(),id);
    const auto state=JsonBoolean(value,"creationPending");
    if(converted.ec!=std::errc{}||converted.ptr!=text.data()+text.size()||!state)return false;
    pending=*state;return true;
}
bool SaveAssociation(const std::filesystem::path& path,std::string_view owner,std::uint64_t id,bool pending)
{
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    if(ec||!ThemeSafePath(path.parent_path(),true)||(std::filesystem::exists(path,ec)&&!ThemeSafePath(path)))return false;
    auto value=JsonValue::Object();value.object["version"]=JsonValue::Number(1);
    value.object["owner"]=JsonValue::String(std::string(owner));value.object["publishedFileId"]=JsonValue::String(std::to_string(id));
    value.object["creationPending"]=JsonValue::Boolean(pending);return atomic_file::WriteAll(path,WriteJson(value));
}
}
std::string IconPackFileHash(const std::filesystem::path& path)
{const auto bytes=Read(path,128ull*1024*1024);return bytes.empty()?std::string{}:ThemeSha256(bytes);}
std::string IconPackPublishedUrl(const std::filesystem::path& data,std::string_view groupId)
{
    if(groupId.size()!=38||!groupId.starts_with("group-")||groupId.find_first_not_of("0123456789abcdef",6)!=groupId.npos)return {};
    std::string owner;std::uint64_t id=0;bool pending=false;
    if(!Association(data/L"IconPackWorkshop"/L"publications"/(std::wstring(groupId.begin(),groupId.end())+L".json"),owner,id,pending)||!id)return {};
    return "https://steamcommunity.com/sharedfiles/filedetails/?id="+std::to_string(id);
}
bool BuildIconPackPublishPlan(const std::filesystem::path& directory,const std::filesystem::path& data,IconPackPublishPlan& plan,std::string& error)
{
    plan={};JsonValue value;
    const auto preparation=Read(directory/L"icon-pack-preparation.json",65536);
    if(!ThemeSafePath(directory,true)||!ThemeSafePath(data,true)||!ParseJson(preparation,value,error)||
        JsonString(value,"format")!="snowdesktop.icon-pack-preparation"||JsonUnsigned(value,"version")!=1)
    {error="stalePreparation";return false;}
    plan.directory=directory;plan.package=directory/L"package.snowicons";plan.preview=directory/L"cover.png";
    plan.preparationSha256=ThemeSha256(preparation);
    plan.packageId=JsonString(value,"packageId").value_or("");plan.title=JsonString(value,"title").value_or("");
    plan.description=JsonString(value,"description").value_or("");plan.snapshotHash=JsonString(value,"snapshotHash").value_or("");
    plan.packageSha256=JsonString(value,"packageSha256").value_or("");plan.coverSha256=JsonString(value,"coverSha256").value_or("");
    plan.preparedAt=static_cast<std::int64_t>(JsonUnsigned(value,"preparedAt").value_or(0));
    if(plan.packageId.size()!=38||!plan.packageId.starts_with("group-")||plan.packageId.find_first_not_of("0123456789abcdef",6)!=plan.packageId.npos||
        plan.title.empty()||plan.title.size()>128||plan.description.size()>=8000||plan.snapshotHash.size()!=64||
        IconPackFileHash(plan.package)!=plan.packageSha256||plan.packageSha256.size()!=64||
        IconPackFileHash(plan.preview)!=plan.coverSha256||plan.coverSha256.size()!=64)
    {error="stalePreparation";return false;}
    CoreError boundary;
    if(!ValidateWorkshopUploadFile(plan.package,false,WorkshopContentKind::IconPack,boundary)||
        !ValidateWorkshopUploadFile(plan.preview,true,WorkshopContentKind::IconPack,boundary)){error=boundary.code;return false;}
    const auto cover=Read(plan.preview,1024*1024);
    if(cover.size()<24||cover.substr(0,8)!=std::string("\x89PNG\r\n\x1a\n",8)||
        cover.substr(16,8)!=std::string("\0\0\4\0\0\0\4\0",8)){error="invalid_preview";return false;}
    plan.association=data/L"IconPackWorkshop"/L"publications"/(std::wstring(plan.packageId.begin(),plan.packageId.end())+L".json");
    std::string owner;bool pending=false;
    if(!Association(plan.association,owner,plan.publishedFileId,pending)){error="invalidAssociation";return false;}
    if(pending&&!plan.publishedFileId){error="creationUncertain";return false;}
    error.clear();return true;
}
std::string IconPackPublishPlanJson(const IconPackPublishPlan& plan)
{
    auto value=JsonValue::Object();value.object["ok"]=JsonValue::Boolean(true);value.object["iconPackWorkflowProtocolVersion"]=JsonValue::Number(1);
    value.object["packageId"]=JsonValue::String(plan.packageId);value.object["title"]=JsonValue::String(plan.title);
    value.object["publishedFileId"]=JsonValue::String(std::to_string(plan.publishedFileId));
    value.object["packageSha256"]=JsonValue::String(plan.packageSha256);value.object["coverSha256"]=JsonValue::String(plan.coverSha256);
    value.object["preparationSha256"]=JsonValue::String(plan.preparationSha256);
    return WriteJson(value,-1);
}
bool ExecuteIconPackPublishPlan(const IconPackPublishPlan& plan,bool confirmCreate,bool confirmUpdate,ThemePublishTransport& transport,
    const PublishProgressCallback& progress,PublishResult& result,CoreError& error,std::int64_t now,const std::atomic_bool* cancel)
{
    result={};
    const auto lockName=L"Local\\SnowDesktop.IconPack."+std::wstring(plan.packageId.begin(),plan.packageId.end());
    HANDLE mutex=CreateMutexW(nullptr,FALSE,lockName.c_str());
    if(!mutex)return Fail(error,"publishBusy");
    const DWORD wait=WaitForSingleObject(mutex,0);
    struct Lock{HANDLE handle;bool owned;~Lock(){if(owned)ReleaseMutex(handle);CloseHandle(handle);}}lock{mutex,wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED};
    if(!lock.owned)return Fail(error,"publishBusy");
    IconPackPublishPlan current;std::string detail;
    if(!BuildIconPackPublishPlan(plan.directory,plan.association.parent_path().parent_path().parent_path(),current,detail)||
        current.packageId!=plan.packageId||current.association!=plan.association||
        current.packageSha256!=plan.packageSha256||current.coverSha256!=plan.coverSha256||current.snapshotHash!=plan.snapshotHash||
        current.preparationSha256!=plan.preparationSha256||
        current.title!=plan.title||current.description!=plan.description||current.publishedFileId!=plan.publishedFileId||
        now<plan.preparedAt||now-plan.preparedAt>1800)return Fail(error,"stalePreparation");
    if(confirmCreate==confirmUpdate||(plan.publishedFileId?!confirmUpdate:!confirmCreate))return Fail(error,"confirmationRequired");
    const auto leases=plan.association.parent_path().parent_path()/L"locks";
    std::error_code ec;std::filesystem::create_directories(leases,ec);
    if(ec||!ThemeSafePath(leases,true))return Fail(error,"writeFailed");
    const auto lockPath=leases/(std::wstring(plan.packageId.begin(),plan.packageId.end())+L".lock");
    HANDLE lease=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(lease==INVALID_HANDLE_VALUE)return Fail(error,"publishBusy");
    struct FileLease{HANDLE handle;~FileLease(){CloseHandle(handle);}}fileLease{lease};
    BY_HANDLE_FILE_INFORMATION information{};
    if(!GetFileInformationByHandle(lease,&information)||(information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return Fail(error,"unsafePath");
    if(!transport.status||!transport.agreement||!transport.item||!transport.publish)return Fail(error,"bridgeUnavailable");
    const auto status=transport.status(error);
    if(!status||!status->loggedOn||status->appId!=kSteamAppId||status->steamId.empty())return Fail(error,"steamUnavailable");
    const auto agreement=transport.agreement(error);
    if(!agreement||(agreement->available&&(!agreement->accepted||agreement->needsAction)))return Fail(error,"agreementRequired");
    std::string owner;std::uint64_t id=0;bool pending=false;
    if(!Association(plan.association,owner,id,pending))return Fail(error,"invalidAssociation");
    if(id!=plan.publishedFileId)return Fail(error,"stalePreparation");
    if(!owner.empty()&&owner!=status->steamId)return Fail(error,"authorMismatch");
    if(id)
    {
        const auto item=transport.item(id,error);JsonValue metadata;
        if(!item||std::to_string(item->ownerSteamId)!=status->steamId||item->consumerAppId!=kSteamAppId||item->banned)return Fail(error,"authorMismatch");
        // A newly allocated but not yet uploaded ID can have empty metadata.
        if(!item->metadata.empty()&&(!ParseJson(item->metadata,metadata,detail)||JsonString(metadata,"format")!="snowdesktop-icon-pack"||
            JsonString(metadata,"packageId")!=plan.packageId||JsonUnsigned(metadata,"iconPackWorkflowProtocolVersion")!=1))return Fail(error,"itemMismatch");
    }
    if(cancel&&cancel->load())return Fail(error,"cancelled");
    PublishRequest request;request.contentKind=WorkshopContentKind::IconPack;request.package=plan.package;request.preview=plan.preview;
    request.title=plan.title;request.description=plan.description;request.tags=std::vector<std::string>{"Icon Pack"};
    auto metadata=JsonValue::Object();metadata.object["format"]=JsonValue::String("snowdesktop-icon-pack");
    metadata.object["artifact"]=JsonValue::String("package.snowicons");metadata.object["iconPackWorkflowProtocolVersion"]=JsonValue::Number(1);
    metadata.object["packageId"]=JsonValue::String(plan.packageId);metadata.object["packageSha256"]=JsonValue::String(plan.packageSha256);
    request.metadata=WriteJson(metadata,-1);
    if(id)request.publishedFileId=id;else request.prepareCreateItem=[&]{return SaveAssociation(plan.association,status->steamId,0,true);};
    request.persistCreatedItem=[&](std::uint64_t created){result.created=true;result.publishedFileId=created;return SaveAssociation(plan.association,status->steamId,created,false);};
    request.validateStagedArtifacts=[&](const auto& package,const auto& cover){return IconPackFileHash(package)==plan.packageSha256&&IconPackFileHash(cover)==plan.coverSha256;};
    const auto published=transport.publish(request,progress,error);if(!published)return false;result=*published;
    if(result.needsLegalAgreement)return Fail(error,"agreementRequired");
    return result.publishedFileId!=0||Fail(error,"publishFailed");
}
}
