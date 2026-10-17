#include "icon_workshop.h"
#include "icon_image_codec.h"
#include "theme/theme_preview.h"
#include "data/atomic_file.h"
#include "steam/steam_app_identity.h"
#include "steam/steam_workshop_cache.h"
#include "../../steam_bridge/src/icon_pack_workshop_publish.h"
#include "../../steam_bridge/src/bridge_json.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace snowdesktop::icon_workshop
{
namespace
{
using steam_bridge::JsonValue;
std::wstring Wide(std::string_view text)
{
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring out(count,L'\0');if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count);return out;
}
std::string String(const JsonValue& value,std::string_view name){return steam_bridge::JsonString(value,name).value_or("");}
std::size_t Utf8Bytes(const std::wstring& text)
{return text.empty()?0:static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr));}
bool Boolean(const JsonValue& value,std::string_view name,bool fallback=false){return steam_bridge::JsonBoolean(value,name).value_or(fallback);}
bool Digits(std::string_view value){return !value.empty()&&value.find_first_not_of("0123456789")==value.npos;}
std::string SnapshotHash(const icon_package::Package& package,const IconLibrarySnapshot& snapshot)
{
    auto frozen=icon_package::Fingerprint(package);
    for(const auto& group:snapshot.groups)if(group.id==package.id)
    {
        // Sharing form state also invalidates a preparation when its rendered
        // title happens to stay identical (default title or optional checkbox).
        for(const auto* text:{&group.name,&group.shareTitle,&group.description,&group.license})
        {frozen+='\0';frozen.append(reinterpret_cast<const char*>(text->data()),text->size()*sizeof(wchar_t));}
        frozen+=group.rightsConfirmed?'1':'0';break;
    }
    return icon_package::Hash(frozen);
}
void RemovePreparation(const std::filesystem::path& directory)
{
    if(!icon_package::SafePath(directory,true)||directory.parent_path().filename()!=L"prepared"||directory.parent_path().parent_path().filename()!=L"IconPackWorkshop")return;
    std::error_code ec;
    for(const auto* name:{L"package.snowicons",L"cover.png",L"icon-pack-preparation.json"})
    {const auto path=directory/name;if(icon_package::SafePath(path))std::filesystem::remove(path,ec);}
    std::filesystem::remove(directory,ec);
}
bool Final(std::string_view output,JsonValue& value)
{
    bool found=false;
    while(!output.empty())
    {
        const auto end=output.find('\n');const auto line=output.substr(0,end);JsonValue next;std::string error;
        if(steam_bridge::ParseJson(line,next,error)&&next.Find("ok")){value=std::move(next);found=true;}
        if(end==output.npos)break;output.remove_prefix(end+1);
    }
    return found;
}
IconLibrarySnapshot Error(IconLibrary& library,std::string error)
{
    auto result=library.Read();result.succeeded=false;
    if(error.starts_with("iconLibrary.")||error.starts_with("iconWorkshop."))result.error=std::move(error);
    else if(error=="cancelled")result.error.clear();
    else if(error=="bridgeUnavailable"||error=="subscriptionsUnavailable"||error=="previewFailed"||error=="stalePreparation"||
        error=="authorMismatch"||error=="agreementRequired"||error=="creationUncertain")result.error="iconWorkshop."+error;
    else result.error="iconWorkshop.operationFailed";
    return result;
}
bool Child(const std::filesystem::path& bridge,const std::vector<std::wstring>& args,std::string& output,std::string& error,
    unsigned timeout,const std::atomic_bool* cancel)
{
    if(cancel&&cancel->load()){error="cancelled";return false;}
    if(themes::preview::Run(bridge,args,output,timeout,error,cancel))return true;
    JsonValue value;if(Final(output,value)&&!String(value,"error").empty())error=String(value,"error");return false;
}
}
bool Available(const std::filesystem::path& bridge)
{
    std::string output,error;
    return Child(bridge,{L"configuration"},output,error,5000,nullptr)&&CompatibleConfiguration(output);
}
bool CompatibleConfiguration(std::string_view output)
{
    JsonValue value;
    if(!Final(output,value)||!Boolean(value,"ok")||
        !Boolean(value,"steamworksCompiled")||steam_bridge::JsonUnsigned(value,"protocolVersion")!=1||
        steam_bridge::JsonUnsigned(value,"iconPackWorkflowProtocolVersion")!=1||steam_bridge::JsonUnsigned(value,"expectedAppId")!=kSnowDesktopSteamAppId)return false;
    const auto* capabilities=value.Find("capabilities");
    return capabilities&&capabilities->IsArray()&&std::any_of(capabilities->array.begin(),capabilities->array.end(),
        [](const auto& item){return item.IsString()&&item.string=="workshop.icon-pack.v1";});
}
bool DecodeSubscriptions(std::string_view output,IconPackSubscriptions& subscriptions,std::string& error,std::vector<std::wstring>* rejected)
{
    if(rejected)rejected->clear();
    subscriptions={};JsonValue value;
    if(!Final(output,value)||!Boolean(value,"ok")||!Boolean(value,"authoritative")||
        steam_bridge::JsonUnsigned(value,"appId")!=kSnowDesktopSteamAppId||steam_bridge::JsonUnsigned(value,"protocolVersion")!=1||
        !Digits(String(value,"steamId"))){error="subscriptionsUnavailable";return false;}
    const auto* items=value.Find("items");if(!items||!items->IsArray()||items->array.size()>4096){error="subscriptionsUnavailable";return false;}
    IconPackSubscriptions next;next.account=String(value,"steamId");
    for(const auto& item:items->array)
    {
        const auto id=String(item,"publishedFileId");
        if(!Digits(id)||!Boolean(item,"subscribed")||!next.subscribed.insert(id).second){error="subscriptionsUnavailable";return false;}
        const auto* details=item.Find("details");if(!details||!details->IsObject())continue;
        // A failed/private/deleted item lookup is not an unsubscribe result.
        // Only a later complete subscription enumeration may remove ownership.
        JsonValue metadata;std::string detail;
        if(steam_bridge::JsonUnsigned(*details,"result")!=1||Boolean(*details,"banned",true)||
            steam_bridge::JsonUnsigned(*details,"consumerAppId")!=kSnowDesktopSteamAppId||
            !steam_bridge::ParseJson(String(*details,"metadata"),metadata,detail)||String(metadata,"format")!="snowdesktop-icon-pack")continue;
        const auto reject=[&]{if(rejected)rejected->push_back(L"#"+Wide(id));};
        if(String(metadata,"artifact")!="package.snowicons"||steam_bridge::JsonUnsigned(metadata,"iconPackWorkflowProtocolVersion")!=1){reject();continue;}
        if(!Boolean(item,"installed")||Boolean(item,"needsUpdate",true)||Boolean(item,"downloading")||Boolean(item,"downloadPending"))continue;
        const auto* install=item.Find("installInfo");if(!install||!Boolean(*install,"available"))continue;
        const auto folder=std::filesystem::path(Wide(String(*install,"folder")));const auto path=folder/L"package.snowicons";
        if(!icon_package::SafePath(folder,true)){reject();continue;}
        std::error_code ec;unsigned files=0;
        for(const auto& file:std::filesystem::directory_iterator(folder,ec))
        {++files;if(file.path().filename()!=L"package.snowicons"||!file.is_regular_file(ec)){files=2;break;}}
        if(ec||files!=1){reject();continue;}
        IconPackDownload download;download.item=id;download.owner=String(*details,"ownerSteamId");download.author=String(*details,"ownerName");
        download.sha256=steam_bridge::IconPackFileHash(path);
        if(!Digits(download.owner)||download.sha256.empty()||download.sha256!=String(metadata,"packageSha256")||
            !icon_package::Read(path,download.package,detail)||download.package.id!=String(metadata,"packageId")||
            steam_bridge::IconPackFileHash(path)!=download.sha256){reject();continue;}
        next.downloads.push_back(std::move(download));
    }
    next.authoritative=true;subscriptions=std::move(next);error.clear();return true;
}
bool RenderCover(const icon_package::Package& package,const std::filesystem::path& output,std::string& error)
{
    using Microsoft::WRL::ComPtr;
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Cleanup{HRESULT hr;~Cleanup(){if(SUCCEEDED(hr))CoUninitialize();}}cleanup{initialized};
    ComPtr<IWICImagingFactory> wic;ComPtr<IWICBitmap> bitmap;ComPtr<ID2D1Factory> factory;ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> write;ComPtr<IDWriteTextFormat> title,caption;ComPtr<ID2D1SolidColorBrush> brush;
    const auto fail=[&]{error="previewFailed";return false;};
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))||
        FAILED(wic->CreateBitmap(1024,1024,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))||
        FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(),&target))||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf())))||
        FAILED(write->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,38,L"",&title))||
        FAILED(write->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,22,L"",&caption))||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(.12f,.14f,.18f),&brush)))return fail();
    target->BeginDraw();target->Clear(D2D1::ColorF(.94f,.95f,.97f));
    const auto name=Wide(package.title);ComPtr<IDWriteTextLayout> heading;ComPtr<IDWriteInlineObject> ellipsis;
    if(FAILED(write->CreateTextLayout(name.c_str(),static_cast<UINT32>(name.size()),title.Get(),928,90,&heading))||
        FAILED(write->CreateEllipsisTrimmingSign(title.Get(),&ellipsis))){target->EndDraw();return fail();}
    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};heading->SetTrimming(&trim,ellipsis.Get());
    target->DrawTextLayout(D2D1::Point2F(48,30),heading.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const auto shown=std::min<std::size_t>(package.images.size(),36);
    const auto count=std::to_wstring(shown)+L" / "+std::to_wstring(package.images.size());
    target->DrawTextW(count.c_str(),static_cast<UINT32>(count.size()),caption.Get(),D2D1::RectF(48,962,976,1000),brush.Get());
    unsigned columns=1;while(columns*columns<shown)++columns;
    const float cell=832.f/static_cast<float>(columns);
    for(std::size_t i=0;i<shown;++i)
    {
        icon_image::Image image;if(!icon_image::DecodeBytes(package.images[i].bytes,256,image,error)){target->EndDraw();return false;}
        ComPtr<ID2D1Bitmap> source;
        if(FAILED(target->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(image.width),static_cast<UINT32>(image.height)),image.pixels.data(),
            static_cast<UINT32>(image.width*4),D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),&source)))
            {target->EndDraw();return fail();}
        const float edge=cell-24,scale=edge/static_cast<float>(std::max(image.width,image.height));
        const float width=static_cast<float>(image.width)*scale,height=static_cast<float>(image.height)*scale;
        const float left=96.f+static_cast<float>(i%columns)*cell+(cell-width)/2;
        const float top=124.f+static_cast<float>(i/columns)*cell+(cell-height)/2;
        target->DrawBitmap(source.Get(),D2D1::RectF(left,top,left+width,top+height));
    }
    if(FAILED(target->EndDraw()))return fail();
    widget_preview::Wallpaper pixels;pixels.width=1024;pixels.height=1024;pixels.pixels.resize(1024*1024);
    if(FAILED(bitmap->CopyPixels(nullptr,4096,1024*1024*4,reinterpret_cast<BYTE*>(pixels.pixels.data()))))return fail();
    return themes::preview::SaveCover(output,std::move(pixels),error);
}
IconLibrarySnapshot Run(IconLibrary& library,const IconLibraryRequest& request,const std::filesystem::path& bridge,
    const std::filesystem::path& data,const std::atomic_bool* cancel,std::atomic_uint* progress)
{
    std::string error,output;
    if(request.action=="autoSyncSubscriptions")
    {
        const auto snapshot=library.Read();bool relevant=std::any_of(snapshot.groups.begin(),snapshot.groups.end(),[](const auto& group){return !group.workshopItem.empty();});
        const auto roots=widget::DiscoverSteamLibraryRoots(kSnowDesktopSteamAppId,error);
        const auto cache=widget::ReadSteamWorkshopLocalCache(roots,kSnowDesktopSteamAppId);
        for(const auto& item:cache.readyItems){std::error_code ec;relevant=relevant||std::filesystem::is_regular_file(item.contentDirectory/L"package.snowicons",ec);}
        if(!relevant)return snapshot;
    }
    if(request.action=="syncSubscriptions"||request.action=="autoSyncSubscriptions")
    {
        if(progress)progress->store(4);
        if(!Available(bridge))return Error(library,"bridgeUnavailable");
        if(!Child(bridge,{L"workshop",L"list-subscribed",L"--details"},output,error,60000,cancel))return Error(library,error);
        IconPackSubscriptions subscriptions;std::vector<std::wstring> rejected;
        if(!DecodeSubscriptions(output,subscriptions,error,&rejected))return Error(library,error);
        if(cancel&&cancel->load())return Error(library,"cancelled");
        auto result=library.Reconcile(subscriptions);result.failures=std::move(rejected);
        if(!result.failures.empty()){result.succeeded=false;if(result.error.empty())result.error="iconWorkshop.operationFailed";}return result;
    }
    icon_package::Package package;
    if(progress)progress->store(1);
    if(request.action=="prepareShare")
    {
        if(!Available(bridge))return Error(library,"bridgeUnavailable");
        if(Utf8Bytes(request.title)>128)return Error(library,"iconWorkshop.titleTooLong");
        if(Utf8Bytes(request.description)>=8000)return Error(library,"iconWorkshop.descriptionTooLong");
        auto save=request;save.action="saveSharing";if(!library.Invoke(save).succeeded)return Error(library,"iconLibrary.saveFailed");
    }
    if(!library.ExportGroup(request.groupId,package,error))return Error(library,error);
    if(cancel&&cancel->load())return Error(library,"cancelled");
    if(request.action=="publishPrepared")
    {
        if(progress)progress->store(3);
        if(request.preparationId.empty()||request.preparationId.size()>96||request.preparationId.find_first_not_of("0123456789-")!=std::string::npos)return Error(library,"stalePreparation");
        const auto directory=data/L"IconPackWorkshop"/L"prepared"/Wide(request.preparationId);
        steam_bridge::IconPackPublishPlan plan;
        if(!steam_bridge::BuildIconPackPublishPlan(directory,data,plan,error))return Error(library,error);
        if(plan.packageId!=request.groupId||plan.title!=package.title||plan.description!=package.description||
            plan.snapshotHash!=SnapshotHash(package,library.Read()))return Error(library,"stalePreparation");
        if(!Available(bridge))return Error(library,"bridgeUnavailable");
        if(!Child(bridge,{L"workshop",L"icon-pack-publish",L"--prepared",directory.wstring(),L"--data-directory",data.wstring(),
            L"--package-sha256",Wide(plan.packageSha256),L"--cover-sha256",Wide(plan.coverSha256),L"--preparation-sha256",Wide(plan.preparationSha256),
            plan.publishedFileId?L"--confirm-update":L"--confirm-create"},output,error,1800000,cancel))return Error(library,error);
        auto result=library.Read();result.publishedUrl=steam_bridge::IconPackPublishedUrl(data,request.groupId);return result;
    }
    const auto token=std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory=data/L"IconPackWorkshop"/L"prepared"/Wide(token);std::error_code ec;
    std::filesystem::create_directories(directory,ec);if(ec||!icon_package::SafePath(directory,true))return Error(library,"unsafePath");
    struct PreparationCleanup{std::filesystem::path path;bool retain=false;~PreparationCleanup(){if(!retain)RemovePreparation(path);}}cleanup{directory};
    for(const auto& entry:std::filesystem::directory_iterator(directory.parent_path(),ec))
    {
        const auto stamp=entry.last_write_time(ec);if(ec){ec.clear();continue;}
        if(entry.path()!=directory&&std::filesystem::file_time_type::clock::now()-stamp>std::chrono::hours(24))RemovePreparation(entry.path());
    }
    if(!icon_package::Write(directory/L"package.snowicons",package,error))return Error(library,error);
    if(progress)progress->store(2);
    if(!RenderCover(package,directory/L"cover.png",error))return Error(library,error);
    if(!request.coverPath.empty()&&!themes::preview::NormalizeCover(request.coverPath,directory/L"cover.png",error))return Error(library,error);
    if(cancel&&cancel->load())return Error(library,"cancelled");
    auto preparation=JsonValue::Object();preparation.object["format"]=JsonValue::String("snowdesktop.icon-pack-preparation");
    preparation.object["version"]=JsonValue::Number(1);preparation.object["packageId"]=JsonValue::String(package.id);
    preparation.object["title"]=JsonValue::String(package.title);preparation.object["description"]=JsonValue::String(package.description);
    preparation.object["snapshotHash"]=JsonValue::String(SnapshotHash(package,library.Read()));
    preparation.object["packageSha256"]=JsonValue::String(steam_bridge::IconPackFileHash(directory/L"package.snowicons"));
    preparation.object["coverSha256"]=JsonValue::String(steam_bridge::IconPackFileHash(directory/L"cover.png"));
    preparation.object["preparedAt"]=JsonValue::Number(static_cast<double>(std::time(nullptr)));
    if(!atomic_file::WriteAll(directory/L"icon-pack-preparation.json",steam_bridge::WriteJson(preparation)))return Error(library,"writeFailed");
    auto result=library.Read();result.coverPath=(directory/L"cover.png").wstring();result.preparationId=token;
    cleanup.retain=true;
    result.publishedUrl=steam_bridge::IconPackPublishedUrl(data,request.groupId);return result;
}
}
