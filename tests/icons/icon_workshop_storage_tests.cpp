#include "icons/icon_library.h"
#include "icons/icon_package.h"
#include "data/atomic_file.h"
#include <algorithm>
#include <fstream>
#include <iostream>

namespace
{
std::string StoredZip(const std::vector<std::pair<std::string,std::string>>& files)
{
    std::string bytes,central;
    const auto put=[](std::string& out,std::uint32_t value,unsigned count){while(count--){out+=static_cast<char>(value&255);value>>=8;}};
    for(const auto& [name,payload]:files)
    {
        std::uint32_t crc=~0u;for(const unsigned char c:payload){crc^=c;for(int n=0;n<8;++n)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}crc=~crc;
        const auto offset=static_cast<std::uint32_t>(bytes.size());const auto size=static_cast<std::uint32_t>(payload.size());
        put(bytes,0x04034b50,4);put(bytes,20,2);put(bytes,0x800,2);put(bytes,0,2);put(bytes,0,4);put(bytes,crc,4);
        put(bytes,size,4);put(bytes,size,4);put(bytes,static_cast<std::uint32_t>(name.size()),2);put(bytes,0,2);bytes+=name;bytes+=payload;
        put(central,0x02014b50,4);put(central,20,2);put(central,20,2);put(central,0x800,2);put(central,0,2);put(central,0,4);put(central,crc,4);
        put(central,size,4);put(central,size,4);put(central,static_cast<std::uint32_t>(name.size()),2);put(central,0,2);put(central,0,2);
        put(central,0,2);put(central,0,2);put(central,0,4);put(central,offset,4);central+=name;
    }
    const auto offset=static_cast<std::uint32_t>(bytes.size());bytes+=central;put(bytes,0x06054b50,4);put(bytes,0,4);
    put(bytes,static_cast<std::uint32_t>(files.size()),2);put(bytes,static_cast<std::uint32_t>(files.size()),2);
    put(bytes,static_cast<std::uint32_t>(central.size()),4);put(bytes,offset,4);put(bytes,0,2);return bytes;
}
}

int RunIconWorkshopStorageTests(const std::filesystem::path& root)
{
    using namespace snowdesktop;
    int failures=0;const auto check=[&](bool value,const char* message){if(!value){++failures;std::cerr<<"FAILED: "<<message<<'\n';}};
    std::string bytes,error;
    check(atomic_file::ReadAll(root/L"lossless.webp",bytes),"read independent WebP fixture");
    icon_package::Package package;package.id="group-"+std::string(32,'a');package.title="Same title";
    const auto asset=icon_package::Hash(bytes)+".webp";package.images.push_back({asset,"Authored image",bytes});
    const auto path=root/L"roundtrip.snowicons";
    check(icon_package::Write(path,package,error),"write icon package with no license and no mandatory rights checkbox");
    icon_package::Package read;
    check(icon_package::Read(path,read,error)&&read.license.empty()&&read.images.size()==1&&read.images[0].bytes==bytes&&
        icon_package::Fingerprint(read)==icon_package::Fingerprint(package),"roundtrip retains exact original bytes and optional license");
    package.license="Permission notice with attribution";
    check(icon_package::Write(path,package,error)&&icon_package::Read(path,read,error)&&read.license==package.license,"license text survives package roundtrip");
    auto invalid=package;invalid.images[0].id=std::string(64,'b')+".webp";
    check(!icon_package::Write(root/L"bad.snowicons",invalid,error),"incorrect content hashes are rejected before writing");
    invalid=package;invalid.images.push_back(invalid.images.front());
    check(!icon_package::Validate(invalid,error),"duplicate assets cannot alias two manifest entries");
    invalid=package;invalid.images[0].bytes="not an image";invalid.images[0].id=icon_package::Hash(invalid.images[0].bytes)+".png";
    check(!icon_package::Validate(invalid,error),"hash-valid invalid images are rejected");
    std::string zip;atomic_file::ReadAll(path,zip);
    const auto central=zip.find(std::string("PK\1\2",4),zip.find(std::string("PK\1\2",4))+4);
    if(central!=std::string::npos)zip.replace(central+46,6,"../bad");
    atomic_file::WriteAll(root/L"traversal.snowicons",zip);
    check(!icon_package::Read(root/L"traversal.snowicons",read,error),"central directory traversal is refused");
    {
        std::ofstream sparse(root/L"large.snowicons",std::ios::binary);sparse.seekp(static_cast<std::streamoff>(icon_package::MaxArchive));sparse.put('x');
    }
    check(!icon_package::Read(root/L"large.snowicons",read,error),"archive limit is checked before allocating archive bytes");
    const std::string manifest="{\"format\":\"snowdesktop-icon-pack\",\"version\":1,\"id\":\""+package.id+
        "\",\"title\":\"Same title\",\"icons\":[{\"id\":\""+asset+"\",\"name\":\"Independent fixture\",\"path\":\"assets/"+asset+"\",\"sha256\":\""+asset.substr(0,64)+"\"}]}";
    const auto archive=[&](const auto& files){atomic_file::WriteAll(root/L"independent.snowicons",StoredZip(files));return icon_package::Read(root/L"independent.snowicons",read,error);};
    using Files=std::vector<std::pair<std::string,std::string>>;
    check(archive(Files{{"icon-pack.json",manifest},{"assets/"+asset,bytes}}),"independently built package validates without relying on production writer");
    auto future=manifest;future.replace(future.find("\"version\":1"),11,"\"version\":9");
    check(!archive(Files{{"icon-pack.json",future},{"assets/"+asset,bytes}}),"unknown package version is rejected with a valid ZIP CRC");
    check(!archive(Files{{"icon-pack.json",manifest}}),"missing image rejects entire manifest");
    check(!archive(Files{{"icon-pack.json",manifest},{"assets/"+asset,bytes},{"assets/"+asset,bytes}}),"duplicate ZIP resource paths reject the entire package");
    check(!archive(Files{{"icon-pack.json",manifest},{"assets/"+asset,bytes+"wrong"}}),"independently CRC-valid wrong content hash rejects entire package");
    check(!archive(Files{{"icon-pack.json",manifest},{"../outside.webp",bytes}}),"independently CRC-valid path traversal is rejected");

    IconLibrary library(root/L"workshop-library");
    IconLibraryRequest request;request.action="createGroup";request.name=L"Same title";
    auto result=library.Invoke(request);check(result.succeeded,"create local group with subscribed display name");
    const auto localId=result.groups.front().id;
    request={};request.action="import";request.paths={(root/L"lossless.webp").wstring()};request.groupId=localId;
    check(library.Invoke(request).succeeded,"establish local ownership before subscribing to identical bytes");
    IconPackSubscriptions snapshot;snapshot.authoritative=true;snapshot.account="111";snapshot.subscribed={"101","202"};
    snapshot.downloads={{"101","301","Author A",std::string(64,'c'),package},{"202","302","Author B",std::string(64,'d'),package}};
    result=library.Reconcile(snapshot);
    check(result.succeeded&&result.groups.size()==3&&result.entries.size()==1,"same-name same-package subscriptions remain separate groups with deduplicated bytes");
    const auto groupA=std::find_if(result.groups.begin(),result.groups.end(),[](const auto& group){return group.workshopItem=="101";});
    const auto groupB=std::find_if(result.groups.begin(),result.groups.end(),[](const auto& group){return group.workshopItem=="202";});
    check(groupA!=result.groups.end()&&groupB!=result.groups.end()&&groupA->id!=groupB->id&&groupA->id!=localId,"names never determine group identity");
    const auto localName=result.entries.front().name;
    snapshot.downloads.front().package.images.front().name="Updated remote name";snapshot.downloads.front().sha256=std::string(64,'e');
    result=library.Reconcile(snapshot);
    check(result.groups.size()==3&&result.entries.size()==1&&result.entries.front().name==localName,"author edits preserve local names and stable subscription group identity");
    const auto unchanged=library.Reconcile(snapshot);
    check(unchanged.groups.size()==3&&unchanged.revision==result.revision,"repeated identical synchronization preserves groups and immutable UI revision");
    const auto updatedGroup=std::find_if(result.groups.begin(),result.groups.end(),[](const auto& g){return g.workshopItem=="101";});
    const auto sourceId=updatedGroup==result.groups.end()?std::string{}:updatedGroup->id;
    request={};request.action="deleteGroupWithAssets";request.groupId=localId;
    check(!library.Invoke(request).succeeded,"local group deletion cannot erase resources also owned by a managed subscription");
    request={};request.action="deleteGroupWithAssets";request.groupId=sourceId;
    check(!library.Invoke(request).succeeded&&library.Read().groups.size()==3,"subscribed group cannot be removed by ordinary deletion");
    request.action="copyLocalGroup";result=library.Invoke(request);
    check(result.succeeded&&!result.importedGroupId.empty()&&result.groups.size()==4,"local copy gets a new independent group identity");
    const auto copyId=result.importedGroupId;
    request={};request.action="rename";request.groupId=copyId;request.ids={asset};request.name=L"My local name";
    result=library.Invoke(request);
    check(result.succeeded&&result.entries.front().groupNames.at(copyId)==L"My local name"&&result.entries.front().groupNames.at(sourceId)!=L"My local name",
        "renaming a saved local copy cannot rewrite the subscribed package name");
    IconLibrary reloaded(root/L"workshop-library");result=reloaded.Read();
    check(result.groups.size()==4&&result.entries.front().localOwned&&result.entries.front().groupNames.contains(sourceId),"v3 persistence retains ownership and per-group labels");
    snapshot.downloads.clear();snapshot.subscribed.clear();result=reloaded.Reconcile(snapshot);
    check(result.groups.size()==2&&result.entries.size()==1&&result.entries.front().localOwned,"unsubscribe preserves local group and explicitly saved local copy");
    IconPackSubscriptions failed;failed.account="111";
    check(!reloaded.Reconcile(failed).succeeded&&reloaded.Read().groups.size()==2,"non-authoritative query never removes retained data");
    IconLibrary isolated(root/L"subscriptions-only");snapshot.subscribed={"101"};snapshot.downloads={{"101","301","Author A",std::string(64,'c'),package}};
    result=isolated.Reconcile(snapshot);check(result.groups.size()==1&&!result.entries.front().localOwned,"subscription imports do not create local ownership");
    snapshot.account="222";snapshot.subscribed.clear();snapshot.downloads.clear();result=isolated.Reconcile(snapshot);
    check(result.groups.size()==1,"a newly observed account cannot unsubscribe another account's pack");
    snapshot.account="111";result=isolated.Reconcile(snapshot);result=isolated.CollectRetired({asset});
    check(result.groups.empty()&&result.entries.size()==1,"in-use snapshot remains available after unsubscribe");
    check(isolated.CollectRetired({}).entries.empty(),"unreferenced subscription-only entries can retire");
    std::string index;atomic_file::ReadAll(root/L"workshop-library/library.json",index);
    check(index.find("\"version\":3")!=index.npos,"new index format is version three");
    return failures;
}
