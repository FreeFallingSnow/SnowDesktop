#include "icon_library.h"
#include "icon_image_codec.h"
#include "icon_pack_archive.h"
#include "android_icon_pack.h"
#include "large_icon_config.h"
#include "data/atomic_file.h"
#include "common/json_value.h"
#include "ui/preview/preview_png_writer.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace snowdesktop
{
namespace
{
std::wstring Wide(std::string_view text)
{
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string Utf8(std::wstring_view text)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string Quote(std::string_view text)
{
    std::string result = "\"";
    const char* digits = "0123456789abcdef";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::string Hash(std::string_view bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<unsigned char, 32> hash{};
    const auto result = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) return {};
    std::ostringstream text; text << std::hex << std::setfill('0');
    for (const unsigned c : hash) text << std::setw(2) << c;
    return text.str();
}
bool ValidName(std::wstring_view name)
{ return !name.empty() && name.size() <= 256 && std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }) && name.find_first_not_of(L" ") != name.npos; }
std::string NewGroupId()
{
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    std::ostringstream text; text << "group-" << std::hex << std::setfill('0');
    for (const unsigned c : bytes) text << std::setw(2) << c;
    return text.str();
}
bool SafeDirectory(const std::filesystem::path& path)
{
    std::error_code error; std::filesystem::create_directories(path, error);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return !error && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool SafeFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
}

std::filesystem::path IconAssetPath(const std::filesystem::path& directory, std::string_view id)
{ return IsIconAssetId(id) ? directory / L"originals" / Wide(id) : std::filesystem::path{}; }

IconLibrary::IconLibrary(std::filesystem::path directory) : directory_(std::move(directory)) {}
bool RestoreIconLibraryAssetReferences(LargeIconConfig& config,const std::vector<std::string>& ids)
{
    const auto contains=[&](const auto& id){return !id.empty()&&std::find(ids.begin(),ids.end(),id)!=ids.end();};
    bool changed=false;
    if(contains(config.imageAssetId))
    {
        config.imageAssetId.clear();config.image.clear();config.content=0;
        if(config.backgroundStyle==-2)config.backgroundStyle=-1;
        changed=true;
    }
    if(contains(config.foregroundImageAssetId))
    {config.foregroundImageAssetId.clear();config.foregroundImage.clear();config.foregroundContent=0;changed=true;}
    return changed;
}
bool IconLibrary::Load()
{
    if (loaded_) return valid_;
    loaded_ = true;
    if (!SafeDirectory(directory_) || !SafeDirectory(directory_ / L"originals") || !SafeDirectory(directory_ / L"previews") || !SafeDirectory(directory_ / L"packs")) return valid_ = false;
    const auto index = directory_ / L"library.json";
    std::error_code ec;
    if (!std::filesystem::exists(index, ec)) return valid_ = !ec;
    std::string text; JsonValue root;
    if (!SafeFile(index) || !atomic_file::ReadAll(index, text) || !ParseJson(text, root)) return valid_ = false;
    const auto* version = root.Find("version"); const auto* entries = root.Find("entries");
    if (!version || !version->IsNumber() || (version->number != 1 && version->number != 2 && version->number != 3) || !entries || !entries->IsArray()) return valid_ = false;
    std::unordered_set<std::string> groupIds;
    if (const auto* groups = root.Find("groups"))
    {
        if (!groups->IsArray()) return valid_ = false;
        for (const auto& value : groups->array)
        {
            const auto* id = value.Find("id"); const auto* name = value.Find("name");
            if (!id || !id->IsString() || id->string.size() != 38 || !id->string.starts_with("group-") ||
                id->string.find_first_not_of("0123456789abcdef", 6) != std::string::npos ||
                !groupIds.insert(id->string).second || !name || !name->IsString() || !ValidName(Wide(name->string))) return valid_ = false;
            IconLibraryGroup group; group.id = id->string; group.name = Wide(name->string);
            for (auto [key, destination] : {std::pair{"source", &group.source}, {"packageId", &group.packageId},
                {"packageVersion", &group.packageVersion}, {"sourceUri", &group.sourceUri}})
                if (const auto* field = value.Find(key); field && field->IsString()) *destination = field->string;
            for (auto [key, destination] : {std::pair{"shareTitle", &group.shareTitle}, {"description", &group.description}, {"license", &group.license}, {"author", &group.author}})
                if (const auto* field = value.Find(key); field && field->IsString()) *destination = Wide(field->string);
            for (auto [key, destination] : {std::pair{"workshopItem", &group.workshopItem}, {"workshopOwner", &group.workshopOwner}, {"workshopHash", &group.workshopHash}})
                if (const auto* field = value.Find(key); field && field->IsString()) *destination = field->string;
            if (const auto* field=value.Find("rightsConfirmed"); field && field->IsBoolean()) group.rightsConfirmed=field->boolean;
            if (const auto* field=value.Find("accounts"); field && field->IsArray()) for(const auto& account:field->array)
                { if(!account.IsString()) return valid_=false; group.accounts.push_back(account.string); }
            groups_.push_back(std::move(group));
        }
    }
    std::unordered_set<std::string> ids;
    for (const auto& value : entries->array)
    {
        const auto* id = value.Find("id"); const auto* name = value.Find("name");
        const auto* width = value.Find("width"); const auto* height = value.Find("height");
        if (!id || !id->IsString() || !IsIconAssetId(id->string) || !ids.insert(id->string).second || !name || !name->IsString() ||
            !width || !width->IsNumber() || width->number < 1 || width->number > 16000000 || std::floor(width->number) != width->number ||
            !height || !height->IsNumber() || height->number < 1 || height->number > 16000000 || std::floor(height->number) != height->number)
            return valid_ = false;
        IconLibraryEntry entry;
        entry.id = id->string; entry.format = entry.id.substr(65); entry.name = Wide(name->string);
        entry.width = static_cast<int>(width->number); entry.height = static_cast<int>(height->number);
        for (auto [key, destination] : {std::pair{"source", &entry.source}, {"packageId", &entry.packageId},
                {"packageVersion", &entry.packageVersion}, {"packageEntryId", &entry.packageEntryId}})
            if (const auto* field = value.Find(key); field && field->IsString()) *destination = field->string;
        if (const auto* groups = value.Find("groupIds"))
        {
            if (!groups->IsArray()) return valid_ = false;
            for (const auto& idValue : groups->array)
            {
                if (!idValue.IsString() || !groupIds.contains(idValue.string) ||
                    std::find(entry.groupIds.begin(), entry.groupIds.end(), idValue.string) != entry.groupIds.end()) return valid_ = false;
                entry.groupIds.push_back(idValue.string);
            }
        }
        if (const auto* field=value.Find("localOwned"); field && field->IsBoolean()) entry.localOwned=field->boolean;
        if (const auto* field=value.Find("groupNames"); field && field->IsObject()) for(const auto& [key,label]:field->object)
            { if(!groupIds.contains(key)||!label.IsString()) return valid_=false; entry.groupNames.emplace(key,Wide(label.string)); }
        entries_.push_back(std::move(entry));
    }
    if(const auto* accounts=root.Find("subscriptionAccounts"); accounts && accounts->IsObject())
        for(const auto& [account,items]:accounts->object) { if(!items.IsArray()) return valid_=false;
            auto& ownedItems=subscriptionAccounts_[account];
            for(const auto& item:items.array) { if(!item.IsString()) return valid_=false; ownedItems.insert(item.string); } }
    return true;
}
bool IconLibrary::Save()
{
    if (!valid_) return false;
    std::string text = "{\"version\":3,\"groups\":[";
    bool firstGroup = true;
    for (const auto& group : groups_)
    {
        if (!firstGroup) text += ','; firstGroup = false;
        text += "{\"id\":" + Quote(group.id) + ",\"name\":" + Quote(Utf8(group.name)) +
            ",\"source\":" + Quote(group.source) + ",\"packageId\":" + Quote(group.packageId) +
            ",\"packageVersion\":" + Quote(group.packageVersion) + ",\"sourceUri\":" + Quote(group.sourceUri) +
            ",\"shareTitle\":"+Quote(Utf8(group.shareTitle))+",\"description\":"+Quote(Utf8(group.description))+
            ",\"license\":"+Quote(Utf8(group.license))+",\"author\":"+Quote(Utf8(group.author))+
            ",\"rightsConfirmed\":"+(group.rightsConfirmed?"true":"false")+
            ",\"workshopItem\":"+Quote(group.workshopItem)+",\"workshopOwner\":"+Quote(group.workshopOwner)+
            ",\"workshopHash\":"+Quote(group.workshopHash)+",\"accounts\":[";
        for(size_t i=0;i<group.accounts.size();++i){if(i)text+=',';text+=Quote(group.accounts[i]);} text+="]}";
    }
    text += "],\"entries\":[";
    bool first = true;
    for (const auto& entry : entries_)
    {
        if (!first) text += ',';
        first = false;
        text += "{\"id\":" + Quote(entry.id) + ",\"name\":" + Quote(Utf8(entry.name)) +
            ",\"width\":" + std::to_string(entry.width) + ",\"height\":" + std::to_string(entry.height) +
            ",\"source\":" + Quote(entry.source) + ",\"packageId\":" + Quote(entry.packageId) +
            ",\"packageVersion\":" + Quote(entry.packageVersion) + ",\"packageEntryId\":" + Quote(entry.packageEntryId) + ",\"groupIds\":[";
        for (size_t i = 0; i < entry.groupIds.size(); ++i) { if (i) text += ','; text += Quote(entry.groupIds[i]); }
        text += "],\"localOwned\":"; text += entry.localOwned ? "true" : "false"; text+=",\"groupNames\":{";
        bool firstName=true;for(const auto& [id,name]:entry.groupNames){if(!firstName)text+=',';firstName=false;text+=Quote(id)+':'+Quote(Utf8(name));} text+="}}";
    }
    text += "],\"subscriptionAccounts\":{";
    bool firstAccount=true;for(const auto& [account,items]:subscriptionAccounts_){if(!firstAccount)text+=',';firstAccount=false;text+=Quote(account)+":[";
        bool firstItem=true;for(const auto& item:items){if(!firstItem)text+=',';firstItem=false;text+=Quote(item);}text+=']';}
    text += "}}\n";
    return atomic_file::WriteAll(directory_ / L"library.json", text);
}
IconLibrarySnapshot IconLibrary::Read()
{
    IconLibrarySnapshot result;
    if (!Load()) { result.error = "iconLibrary.readFailed"; return result; }
    for (auto& entry : entries_)
    {
        const auto original = IconAssetPath(directory_, entry.id);
        const auto preview = directory_ / L"previews" / (Wide(entry.id) + L"-v" + std::to_wstring(icon_image::kCacheVersion) + L".png");
        entry.missing = !SafeFile(original);
        entry.previewPath.clear();
        if (!entry.missing && !SafeFile(preview))
        {
            icon_image::Image image; std::string error;
            if (icon_image::Decode(original, 256, image, error))
            { if (!preview_png::Save(preview, image.width, image.height, image.pixels, error)) entry.missing = true; }
            else entry.missing = true;
        }
        if (SafeFile(preview)) entry.previewPath = preview.wstring();
    }
    result.entries = entries_; result.groups = groups_; result.revision = revision_; result.succeeded = true;
    return result;
}
IconLibrarySnapshot IconLibrary::Refresh()
{
    loaded_ = false; valid_ = true; entries_.clear(); groups_.clear(); subscriptionAccounts_.clear(); ++revision_;
    return Read();
}
std::string IconLibrary::Import(const std::filesystem::path& path, std::string& error, const std::string& groupId, std::wstring name)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > icon_image::kMaxFileBytes) { error = "iconLibrary.tooLarge"; return {}; }
    std::ifstream input(path, std::ios::binary); std::string bytes(static_cast<size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(size)) || input.peek() != EOF) { error = "iconLibrary.invalid"; return {}; }
    // The selected source may already be our hashed original. Release its
    // Windows read handle before atomically refreshing that same resource.
    input.close();
    const auto hash = Hash(bytes);
    if (hash.empty()) { error = "iconLibrary.saveFailed"; return {}; }
    const auto stage = directory_ / (L".import-" + Wide(hash) + path.extension().wstring());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{stage};
    if (!atomic_file::WriteAll(stage, bytes)) { error = "iconLibrary.saveFailed"; return {}; }
    icon_image::Image image;
    if (!icon_image::Decode(stage, 256, image, error)) return {};
    const auto id = hash + '.' + image.format;
    const auto original = IconAssetPath(directory_, id);
    if (!atomic_file::WriteAll(original, bytes)) { error = "iconLibrary.saveFailed"; return {}; }
    // Reuse the actual decoded pixels for the rebuildable thumbnail; a large
    // pack must not decode every vector again during the following Read().
    const auto preview = directory_ / L"previews" / (Wide(id) + L"-v" + std::to_wstring(icon_image::kCacheVersion) + L".png");
    std::string previewError;
    if (!preview_png::Save(preview, image.width, image.height, image.pixels, previewError)) { error = "iconLibrary.saveFailed"; return {}; }
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == entries_.end())
    {
        IconLibraryEntry entry; entry.id = id; entry.format = image.format; entry.name = name.empty() ? path.stem().wstring() : std::move(name);
        entry.width = image.sourceWidth; entry.height = image.sourceHeight;
        entries_.push_back(std::move(entry));
        found = entries_.end() - 1;
    }
    found->localOwned = true;
    if (!groupId.empty() && std::find(found->groupIds.begin(), found->groupIds.end(), groupId) == found->groupIds.end()) found->groupIds.push_back(groupId);
    return id;
}
bool IconLibrary::ExportGroup(std::string_view id, icon_package::Package& package, std::string& error)
{
    package={}; if(!Load()) {error="iconLibrary.readFailed";return false;}
    const auto group=std::find_if(groups_.begin(),groups_.end(),[&](const auto& value){return value.id==id;});
    if(group==groups_.end() || !group->workshopItem.empty()){error="iconWorkshop.invalidGroup";return false;}
    package.id=group->id;package.title=Utf8(group->shareTitle.empty()?group->name:group->shareTitle);
    package.description=Utf8(group->description);package.license=Utf8(group->license);
    for(const auto& entry:entries_)if(std::find(entry.groupIds.begin(),entry.groupIds.end(),id)!=entry.groupIds.end())
    {
        icon_package::Image image;image.id=entry.id;
        image.name=Utf8(entry.groupNames.contains(std::string(id))?entry.groupNames.at(std::string(id)):entry.name);
        const auto file=IconAssetPath(directory_,entry.id);std::error_code ec;const auto size=std::filesystem::file_size(file,ec);
        if(ec||size>icon_image::kMaxFileBytes||!icon_package::SafePath(file)||!atomic_file::ReadAll(file,image.bytes))
        {error="iconLibrary.missing";return false;}
        package.images.push_back(std::move(image));
    }
    std::sort(package.images.begin(),package.images.end(),[](const auto& a,const auto& b){return a.name==b.name?a.id<b.id:a.name<b.name;});
    return icon_package::Validate(package,error);
}
IconLibrarySnapshot IconLibrary::Reconcile(const IconPackSubscriptions& subscriptions)
{
    if(!Load()){IconLibrarySnapshot result;result.error="iconLibrary.readFailed";return result;}
    const auto digits=[](std::string_view value){return !value.empty()&&value.find_first_not_of("0123456789")==value.npos;};
    if(!subscriptions.authoritative || !digits(subscriptions.account))
    {auto result=Read();result.succeeded=false;result.error="iconWorkshop.subscriptionsUnavailable";return result;}
    const auto oldGroups=groups_;const auto oldEntries=entries_;const auto oldAccounts=subscriptionAccounts_;
    const bool known=subscriptionAccounts_.contains(subscriptions.account);
    bool changed=!known||subscriptionAccounts_.at(subscriptions.account)!=subscriptions.subscribed;
    subscriptionAccounts_[subscriptions.account]=subscriptions.subscribed;
    std::string error;
    for(const auto& download:subscriptions.downloads)
    {
        std::string packageError;
        if(!digits(download.item)||(!subscriptions.localCache&&!digits(download.owner))||!subscriptions.subscribed.contains(download.item)||
            !icon_package::Validate(download.package,packageError)) {if(!packageError.empty())error=packageError;continue;}
        auto group=std::find_if(groups_.begin(),groups_.end(),[&](const auto& value){return value.workshopItem==download.item;});
        if(group!=groups_.end()&&!subscriptions.localCache&&!group->workshopOwner.empty()&&group->workshopOwner!=download.owner){error="iconWorkshop.authorMismatch";continue;}
        const auto beforeGroups=groups_;const auto beforeEntries=entries_;
        if(group==groups_.end())
        {
            IconLibraryGroup value;value.id=NewGroupId();value.workshopItem=download.item;value.workshopOwner=download.owner;
            if(value.id.empty()){error="iconLibrary.saveFailed";continue;}
            value.source="steam-workshop";groups_.push_back(std::move(value));group=std::prev(groups_.end());changed=true;
        }
        if(std::find(group->accounts.begin(),group->accounts.end(),subscriptions.account)==group->accounts.end()){group->accounts.push_back(subscriptions.account);changed=true;}
        if(!subscriptions.localCache&&group->workshopOwner.empty()){group->workshopOwner=download.owner;changed=true;}
        if(group->workshopHash==download.sha256)continue;
        changed=true;
        const auto groupId=group->id;bool installed=true;
        for(auto& entry:entries_){std::erase(entry.groupIds,groupId);entry.groupNames.erase(groupId);}
        for(const auto& image:download.package.images)
        {
            const auto path=IconAssetPath(directory_,image.id);
            if(!atomic_file::WriteAll(path,image.bytes)){installed=false;error="iconLibrary.saveFailed";break;}
            auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const auto& value){return value.id==image.id;});
            if(entry==entries_.end())
            {
                icon_image::Image decoded;
                if(!icon_image::DecodeBytes(image.bytes,64,decoded,error)){installed=false;break;}
                IconLibraryEntry value;value.id=image.id;value.name=Wide(image.name);value.format=image.id.substr(65);
                value.width=decoded.sourceWidth;value.height=decoded.sourceHeight;value.localOwned=false;value.source="steam-workshop";
                entries_.push_back(std::move(value));entry=std::prev(entries_.end());
            }
            entry->groupIds.push_back(groupId);entry->groupNames[groupId]=Wide(image.name);
        }
        if(!installed){groups_=beforeGroups;entries_=beforeEntries;continue;}
        group->name=Wide(download.package.title);group->shareTitle=group->name;group->description=Wide(download.package.description);
        group->license=Wide(download.package.license);group->author=Wide(download.author.empty()?download.owner:download.author);
        group->packageId=download.package.id;group->workshopHash=download.sha256;
        group->sourceUri="https://steamcommunity.com/sharedfiles/filedetails/?id="+download.item;
    }
    std::set<std::string> retired;
    std::erase_if(groups_,[&](auto& group){
        if(group.workshopItem.empty())return false;
        if(subscriptions.subscribed.contains(group.workshopItem)&&std::find(group.accounts.begin(),group.accounts.end(),subscriptions.account)==group.accounts.end()){group.accounts.push_back(subscriptions.account);changed=true;}
        if(known&&!subscriptions.subscribed.contains(group.workshopItem)&&std::erase(group.accounts,subscriptions.account))changed=true;
        if(subscriptions.deleted.contains(group.workshopItem)||group.accounts.empty()){retired.insert(group.id);changed=true;return true;}return false;
    });
    for(auto& entry:entries_)for(const auto& id:retired){std::erase(entry.groupIds,id);entry.groupNames.erase(id);}
    if(changed)
    {
        if(!Save()){groups_=oldGroups;entries_=oldEntries;subscriptionAccounts_=oldAccounts;error="iconLibrary.saveFailed";}
        else ++revision_;
    }
    auto result=Read();result.error=error;result.succeeded=error.empty();return result;
}
IconLibrarySnapshot IconLibrary::CollectRetired(const std::vector<std::string>& protectedIds)
{
    if(!Load()){IconLibrarySnapshot result;result.error="iconLibrary.readFailed";return result;}
    const auto previous=entries_;std::vector<std::string> removed;
    std::erase_if(entries_,[&](const auto& entry){
        if(!entry.localOwned&&entry.groupIds.empty()&&std::find(protectedIds.begin(),protectedIds.end(),entry.id)==protectedIds.end())
        {removed.push_back(entry.id);return true;}return false;});
    if(!removed.empty()&&!Save()){entries_=previous;auto result=Read();result.succeeded=false;result.error="iconLibrary.saveFailed";return result;}
    if(!removed.empty())++revision_;
    // The host freezes new applies while this queue operation runs and supplies
    // freshly collected layout/editor references after subscription reconciliation.
    for(const auto& id:removed)
    {
        std::error_code ec;std::filesystem::remove(IconAssetPath(directory_,id),ec);
        std::filesystem::remove(directory_/L"previews"/(Wide(id)+L"-v"+std::to_wstring(icon_image::kCacheVersion)+L".png"),ec);
    }
    return Read();
}
IconLibrarySnapshot IconLibrary::Invoke(const IconLibraryRequest& request, const std::vector<std::string>& protectedIds)
{
    if (!Load()) { IconLibrarySnapshot result; result.error = "iconLibrary.readFailed"; return result; }
    auto deletionIds=request.ids;
    if(request.action=="deleteGroupWithAssets")
    {
        deletionIds.clear();
        for(const auto& entry:entries_)if(std::find(entry.groupIds.begin(),entry.groupIds.end(),request.groupId)!=entry.groupIds.end())deletionIds.push_back(entry.id);
    }
    if ((request.action == "delete"||request.action=="deleteGroupWithAssets") && std::any_of(deletionIds.begin(), deletionIds.end(), [&](const auto& id) {
            return std::find(protectedIds.begin(), protectedIds.end(), id) != protectedIds.end(); }))
    { auto result = Read(); result.succeeded = false; result.error = "iconLibrary.deleteInUse"; return result; }
    IconLibrarySnapshot operation;
    const auto group = std::find_if(groups_.begin(), groups_.end(), [&](const auto& value) { return value.id == request.groupId; });
    if (group != groups_.end() && !group->workshopItem.empty() &&
        (request.action=="renameGroup" || request.action=="deleteGroup" || request.action=="deleteGroupWithAssets" || request.action=="saveSharing" || request.action=="import"))
    { auto result=Read();result.succeeded=false;result.error="iconWorkshop.managedGroup";return result; }
    const bool renameLocal=request.action=="rename"&&group!=groups_.end()&&group->workshopItem.empty();
    if (request.action=="delete" || request.action=="deleteGroupWithAssets" || (request.action=="rename"&&!renameLocal)) for(const auto& entry:entries_)
        if(std::find(deletionIds.begin(),deletionIds.end(),entry.id)!=deletionIds.end() &&
            std::any_of(groups_.begin(),groups_.end(),[&](const auto& g){return !g.workshopItem.empty() && std::find(entry.groupIds.begin(),entry.groupIds.end(),g.id)!=entry.groupIds.end();}))
        {auto result=Read();result.succeeded=false;result.error="iconWorkshop.managedGroup";return result;}
    if (request.action=="saveSharing" || request.action=="copyLocalGroup")
    {
        if(group==groups_.end()){auto result=Read();result.error="iconLibrary.stale";result.succeeded=false;return result;}
        const auto oldGroups=groups_;const auto oldEntries=entries_;
        if(request.action=="saveSharing")
        {
            if(!ValidName(request.title)||Utf8(request.title).size()>1024||Utf8(request.description).size()>32000||Utf8(request.license).size()>16384)
            {auto result=Read();result.succeeded=false;result.error="iconLibrary.invalid";return result;}
            group->shareTitle=request.title;group->description=request.description;group->license=request.license;group->rightsConfirmed=request.rightsConfirmed;
        }
        else
        {
            auto copy=*group;copy.id=NewGroupId();copy.workshopItem.clear();copy.workshopOwner.clear();copy.workshopHash.clear();copy.accounts.clear();copy.publishedUrl.clear();copy.source="local";
            if(copy.id.empty()){auto result=Read();result.error="iconLibrary.saveFailed";result.succeeded=false;return result;}
            const auto source=group->id;const auto fresh=copy.id;groups_.push_back(std::move(copy));
            for(auto& entry:entries_)if(std::find(entry.groupIds.begin(),entry.groupIds.end(),source)!=entry.groupIds.end())
            {entry.groupIds.push_back(fresh);entry.localOwned=true;if(entry.groupNames.contains(source))entry.groupNames[fresh]=entry.groupNames.at(source);}
            operation.importedGroupId=fresh;
        }
        if(!Save()){groups_=oldGroups;entries_=oldEntries;operation.error="iconLibrary.saveFailed";}else ++revision_;
    }
    else if (request.action == "createGroup" || request.action == "renameGroup" || request.action == "deleteGroup" || request.action == "deleteGroupWithAssets" || request.action == "moveGroup")
    {
        const auto oldGroups = groups_; const auto oldEntries = entries_;
        if (request.action=="moveGroup" && group!=groups_.end() && !group->workshopItem.empty())
            operation.error="iconWorkshop.managedGroup";
        else if (request.action == "createGroup")
        {
            if (!ValidName(request.name)) operation.error = "iconLibrary.invalid";
            else { IconLibraryGroup value; value.id = NewGroupId(); value.name = request.name;
                if (value.id.empty()) operation.error = "iconLibrary.saveFailed"; else groups_.push_back(std::move(value)); }
        }
        else if (group == groups_.end() && !(request.action == "moveGroup" && request.groupId.empty())) operation.error = "iconLibrary.stale";
        else if (request.action == "renameGroup")
        { if (ValidName(request.name)) group->name = request.name; else operation.error = "iconLibrary.invalid"; }
        else if (request.action == "deleteGroup" || request.action == "deleteGroupWithAssets")
        {
            groups_.erase(group);
            if(request.action=="deleteGroupWithAssets")std::erase_if(entries_,[&](const auto& entry){return std::find(deletionIds.begin(),deletionIds.end(),entry.id)!=deletionIds.end();});
            else for (auto& entry : entries_) {std::erase(entry.groupIds, request.groupId);entry.groupNames.erase(request.groupId);}
        }
        else for (auto& entry : entries_)
            if (std::find(request.ids.begin(), request.ids.end(), entry.id) != request.ids.end())
            { std::erase_if(entry.groupIds,[&](const auto& id){return std::none_of(groups_.begin(),groups_.end(),[&](const auto& g){return g.id==id&&!g.workshopItem.empty();});});
              if (!request.groupId.empty()) entry.groupIds.push_back(request.groupId); entry.localOwned=true; }
        if (operation.error.empty() && !Save()) { groups_ = oldGroups; entries_ = oldEntries; operation.error = "iconLibrary.saveFailed"; }
        if (operation.error.empty())
        {
            ++revision_;
            if(request.action=="deleteGroupWithAssets")for(const auto& id:deletionIds)
            {
                std::error_code ec;std::filesystem::remove(IconAssetPath(directory_,id),ec);
                std::filesystem::remove(directory_/L"previews"/(Wide(id)+L"-v"+std::to_wstring(icon_image::kCacheVersion)+L".png"),ec);
            }
        }
    }
    else if (request.action == "import")
    {
        if (!request.groupId.empty() && group == groups_.end()) { auto result = Read(); result.succeeded = false; result.error = "iconLibrary.stale"; return result; }
        const auto oldEntries = entries_; const auto oldGroups = groups_;
        for (const auto& pathText : request.paths)
        {
            const std::filesystem::path path(pathText);
            auto extension = path.extension().wstring(); for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
            if (extension == L".zip" || extension == L".apk")
            {
                icon_pack::Archive archive; std::string error;
                if (!icon_pack::Read(path, archive, error) ||
                    (extension == L".apk" && !android_icon_pack::Convert(archive, error)))
                { operation.error = error; operation.failures.push_back(path.filename().wstring()); continue; }
                const auto packageId = Hash(archive.original);
                if (packageId.empty() || !atomic_file::WriteAll(directory_ / L"packs" / (Wide(packageId) + extension), archive.original))
                { operation.error = "iconLibrary.saveFailed"; operation.failures.push_back(path.filename().wstring()); continue; }
                archive.original.clear(); archive.original.shrink_to_fit();
                IconLibraryGroup pack; pack.id = NewGroupId(); pack.name = path.stem().wstring(); pack.packageId = packageId;
                pack.source = extension == L".apk" ? "android-apk" : "zip";
                pack.sourceUri = Utf8(path.filename().wstring());
                if (pack.id.empty()) { operation.error = "iconLibrary.saveFailed"; continue; }
                const auto existing = std::find_if(groups_.begin(), groups_.end(), [&](const auto& value) { return value.source == pack.source && value.packageId == packageId; });
                const bool created = existing == groups_.end(); const auto packId = created ? pack.id : existing->id;
                const auto source = pack.source; const auto before = operation.imported.size();
                if (created) groups_.push_back(std::move(pack));
                for (const auto& item : archive.images)
                {
                    const auto file = std::filesystem::path(Wide(item.name));
                    const auto temporary = directory_ / (L".pack-" + Wide(packId) + file.extension().wstring());
                    std::string importError;
                    const auto id = atomic_file::WriteAll(temporary, item.bytes) ? Import(temporary, importError, packId, file.stem().wstring()) : std::string{};
                    std::error_code removeError; std::filesystem::remove(temporary, removeError);
                    if (id.empty()) { operation.failures.push_back(Wide(item.name)); operation.error = importError.empty() ? "iconLibrary.invalid" : importError; }
                    else
                    {
                        operation.imported.push_back(id);
                        auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& value) { return value.id == id; });
                        if (entry != entries_.end() && entry->packageId.empty() &&
                            std::none_of(oldEntries.begin(), oldEntries.end(), [&](const auto& value) { return value.id == id; }))
                        { entry->source = source; entry->packageId = packageId; entry->packageEntryId = item.name; }
                    }
                }
                for (const auto& skipped : archive.skipped) operation.failures.push_back(Wide(skipped));
                if (!archive.skipped.empty() && operation.error.empty()) operation.error = "iconLibrary.packUnsupported";
                if (created && before == operation.imported.size()) groups_.pop_back();
                else if (request.paths.size() == 1) operation.importedGroupId = packId;
            }
            else
            {
                std::string error; const auto id = Import(path, error, request.groupId);
                if (!id.empty()) operation.imported.push_back(id); else operation.failures.push_back(path.filename().wstring());
                if (!error.empty()) operation.error = error;
            }
        }
        if (!Save()) { entries_ = oldEntries; groups_ = oldGroups; operation.imported.clear(); operation.error = "iconLibrary.saveFailed"; }
        else ++revision_;
    }
    else if (request.action == "rename")
    {
        auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return request.ids.size() == 1 && entry.id == request.ids[0]; });
        if (found == entries_.end() || !ValidName(request.name) || (renameLocal&&std::find(found->groupIds.begin(),found->groupIds.end(),request.groupId)==found->groupIds.end()))
            operation.error = "iconLibrary.invalid";
        else { const auto old = *found; if(renameLocal)found->groupNames[request.groupId]=request.name;else found->name=request.name;
            if (!Save()) { *found = old; operation.error = "iconLibrary.saveFailed"; } else ++revision_; }
    }
    else if (request.action == "delete")
    {
        const auto previous = entries_;
        std::erase_if(entries_, [&](const auto& entry) { return std::find(request.ids.begin(), request.ids.end(), entry.id) != request.ids.end(); });
        if (!Save()) { entries_ = previous; operation.error = "iconLibrary.saveFailed"; }
        else
        {
            ++revision_;
            for (const auto& id : request.ids) if (IsIconAssetId(id))
            {
                std::error_code ec; std::filesystem::remove(IconAssetPath(directory_, id), ec);
                std::filesystem::remove(directory_ / L"previews" / (Wide(id) + L"-v" + std::to_wstring(icon_image::kCacheVersion) + L".png"), ec);
            }
        }
    }
    else if (request.action == "export")
    {
        for (const auto& id : request.ids)
        {
            const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.id == id; });
            std::string error;
            if (found == entries_.end()) { operation.error = "iconLibrary.invalid"; continue; }
            auto name = found->name;
            for (auto& c : name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
            while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
            if (name.empty()) name = L"icon";
            const auto extension = request.format == "ico" ? L".ico" : L".png";
            auto output = std::filesystem::path(request.destination) / (name + extension);
            std::error_code ec;
            for (unsigned n = 2; std::filesystem::exists(output, ec) && !ec; ++n)
                output = std::filesystem::path(request.destination) / (name + L" (" + std::to_wstring(n) + L")" + extension);
            const bool ok = request.format == "ico" ? icon_image::ExportIco(IconAssetPath(directory_, id), output, error) :
                icon_image::ExportPng(IconAssetPath(directory_, id), output, error);
            if (!ok) { operation.failures.push_back(found->name); operation.error = "iconLibrary.exportFailed"; }
        }
    }
    auto result = Read();
    result.importedGroupId = operation.importedGroupId;
    result.error = operation.error; result.failures = std::move(operation.failures); result.imported = std::move(operation.imported);
    result.succeeded = result.error.empty();
    return result;
}
}
