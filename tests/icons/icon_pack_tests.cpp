#include "icons/icon_library.h"
#include "icons/icon_pack_archive.h"
#include "icons/android_icon_pack.h"
#include "icons/icon_image_codec.h"
#include "icons/icon_beautify.h"
#include "data/atomic_file.h"
#include <algorithm>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <tuple>
#include <unordered_map>

namespace
{
std::string Utf8(std::wstring_view text)
{
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(count,'\0');if(count)WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count,nullptr,nullptr);return out;
}
std::filesystem::path PathUtf8(std::string_view text)
{
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(count<=0)throw std::runtime_error("invalid UTF-8 probe path");
    std::wstring wide(count,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),count);
    return std::filesystem::path(wide);
}
void U16(std::string& out,unsigned value){out+=static_cast<char>(value);out+=static_cast<char>(value>>8);}
void U32(std::string& out,unsigned value){U16(out,value&65535);U16(out,value>>16);}
std::string Hex(std::string_view text)
{std::string result;for(size_t i=0;i<text.size();i+=2)result+=static_cast<char>(std::stoul(std::string(text.substr(i,2)),nullptr,16));return result;}
std::string Zip(std::string name,std::string bytes,unsigned crc,unsigned expanded,unsigned method=8,unsigned flags=0x800)
{
    std::string out;U32(out,0x04034b50);U16(out,20);U16(out,flags);U16(out,method);U32(out,0);U32(out,crc);
    U32(out,static_cast<unsigned>(bytes.size()));U32(out,expanded);U16(out,static_cast<unsigned>(name.size()));U16(out,0);out+=name;out+=bytes;
    const auto central=static_cast<unsigned>(out.size());
    U32(out,0x02014b50);U16(out,20);U16(out,20);U16(out,flags);U16(out,method);U32(out,0);U32(out,crc);
    U32(out,static_cast<unsigned>(bytes.size()));U32(out,expanded);U16(out,static_cast<unsigned>(name.size()));U16(out,0);U16(out,0);U16(out,0);U16(out,0);U32(out,0);U32(out,0);out+=name;
    const auto centralSize=static_cast<unsigned>(out.size())-central;
    U32(out,0x06054b50);U16(out,0);U16(out,0);U16(out,1);U16(out,1);U32(out,centralSize);U32(out,central);U16(out,0);return out;
}
std::string Pool(const std::vector<std::string>& strings)
{
    std::string data,offsets;std::unordered_map<std::string,unsigned> positions;
    for(const auto& text:strings)
    {
        const auto [found,inserted]=positions.emplace(text,static_cast<unsigned>(data.size()));U32(offsets,found->second);if(!inserted)continue;
        const auto length=[&]{if(text.size()>=128)data+=static_cast<char>(128|(text.size()>>8));data+=static_cast<char>(text.size()&255);};
        length();length();data+=text;data+='\0';
    }
    while(data.size()%4)data+='\0';std::string out;U16(out,1);U16(out,28);U32(out,static_cast<unsigned>(28+offsets.size()+data.size()));
    U32(out,static_cast<unsigned>(strings.size()));U32(out,0);U32(out,256);U32(out,static_cast<unsigned>(28+offsets.size()));U32(out,0);return out+offsets+data;
}
std::string Type(unsigned id,const std::vector<std::tuple<unsigned,unsigned,unsigned>>& entries,bool compact,bool sparse)
{
    std::string out;U16(out,0x201);U16(out,84);const auto valueSize=compact?8u:16u;
    U32(out,84+static_cast<unsigned>(entries.size())*(4+valueSize));out+=static_cast<char>(id);out+=static_cast<char>(sparse?1:0);U16(out,0);U32(out,static_cast<unsigned>(entries.size()));U32(out,84+static_cast<unsigned>(entries.size())*4);U32(out,64);out.append(60,'\0');
    for(unsigned i=0;i<entries.size();++i)if(sparse){U16(out,i);U16(out,i*valueSize/4);}else U32(out,i*valueSize);
    for(const auto& [key,type,data]:entries)
    {
        U16(out,compact?key:8);U16(out,compact?(8|(type<<8)):0);U32(out,compact?data:key);
        if(!compact){U16(out,8);out+='\0';out+=static_cast<char>(type);U32(out,data);}
    }
    return out;
}
std::string Table(bool compact=false,bool sparse=false,bool cycle=false,bool bitmap=false,std::string globals={})
{
    if(globals.empty())globals=Pool({"res/sample.xml",bitmap?"res/foreground.png":"res/foreground.xml"});
    const auto types=Pool({"drawable","color"}),keys=Pool({"sample","foreground","back"});
    const auto entries=Type(1,{{0,3,0},{1,3,1}},compact,sparse)+Type(2,{{2,cycle?1u:28u,cycle?0x7f020000u:0xffff0000u}},compact,sparse);
    std::string package;U16(package,0x200);U16(package,288);U32(package,static_cast<unsigned>(288+types.size()+keys.size()+entries.size()));U32(package,127);package.append(256,'\0');
    U32(package,288);U32(package,0);U32(package,static_cast<unsigned>(288+types.size()));U32(package,0);U32(package,0);package+=types+keys+entries;
    std::string out;U16(out,2);U16(out,12);U32(out,static_cast<unsigned>(12+globals.size()+package.size()));U32(out,1);return out+globals+package;
}
}
int RunIconPackTests(const std::filesystem::path& root)
{
    using namespace snowdesktop;int failures=0;
    const auto check=[&](bool value,const char* label){if(!value){++failures;std::cerr<<"FAILED: "<<label<<'\n';}};
    const auto packed=Hex("1d8c510a83301005afb23cffddb5855624c9875709c606529528597bfb4661e0c1f018b39799ce6f5a768bcf716c03b3aab6fa6cd73cf34344b83e40254e3aaea7859050f7aac0191fb34f13f9aa7b90ffdd932ddea01053b26842a805013b7365dc1f");
    const auto pack=root/L"图标包.zip";atomic_file::WriteAll(pack,Zip("icons/red.svg",packed,0x7e42e513,110));
    icon_pack::Archive archive;std::string error;
    check(icon_pack::Read(pack,archive,error)&&archive.images.size()==1&&archive.images[0].bytes.size()==110,"read independently deflated SVG ZIP with checked CRC");
    IconLibrary library(root/L"group-library");IconLibraryRequest request;request.action="import";request.paths={pack.wstring()};const auto imported=library.Invoke(request);
    check(imported.succeeded&&imported.groups.size()==1&&imported.entries.size()==1&&imported.entries[0].groupIds.size()==1,"ZIP import creates one persistent group and real decoded asset");
    if(!imported.entries.empty()&&!imported.groups.empty())
    {
        const auto id=imported.entries[0].id,packGroup=imported.groups[0].id;
        check(library.Invoke(request).groups.size()==1,"reimporting identical pack reuses its existing group");
        request={};request.action="createGroup";request.name=L"自定义分组";auto created=library.Invoke(request);check(created.succeeded&&created.groups.size()==2,"create local group");
        if(created.groups.size()==2)
        {
            const auto group=created.groups[1].id;request={};request.action="moveGroup";request.ids={id};request.groupId=group;
            check(library.Invoke(request).entries[0].groupIds==std::vector<std::string>{group},"move uses resource identity without copying original bytes");
            request={};request.action="renameGroup";request.groupId=group;request.name=L"Renamed / 重命名";
            check(library.Invoke(request).succeeded,"rename group");
            IconLibrary reopened(root/L"group-library");auto saved=reopened.Read();check(saved.groups.size()==2&&saved.groups[1].name==request.name&&saved.entries[0].groupIds==std::vector<std::string>{group},"group identity, Unicode name and membership survive reopening");
            request={};request.action="deleteGroup";request.groupId=group;const auto removed=library.Invoke(request);
            check(removed.succeeded&&removed.entries.size()==1&&removed.entries[0].groupIds.empty()&&std::filesystem::exists(IconAssetPath(root/L"group-library",id)),"deleting a group preserves its icons and original bytes");
        }
        check(std::filesystem::exists(root/L"group-library"/L"packs"/(std::wstring(imported.groups[0].packageId.begin(),imported.groups[0].packageId.end())+L".zip")),"original pack is retained for backup and future provenance");
        (void)packGroup;
    }
    atomic_file::WriteAll(pack,Zip("dynamic.png",Hex("edc13101000000c2a06ceb5fca147e4001000000006f03"),0x131a7bbe,5000));
    check(icon_pack::Read(pack,archive,error)&&archive.images[0].bytes==std::string(5000,'A'),"decode independent dynamic-Huffman deflate stream");
    atomic_file::WriteAll(pack,Zip("../escape.svg",packed,0x7e42e513,110));check(!icon_pack::Read(pack,archive,error),"reject parent-traversal archive names before any extraction");
    atomic_file::WriteAll(pack,Zip("safe.svg",packed,0x7e42e512,110));check(!icon_pack::Read(pack,archive,error),"reject mismatched archive CRC");
    atomic_file::WriteAll(pack,Zip("caf\x82.svg",packed,0x7e42e513,110,8,0));
    check(icon_pack::Read(pack,archive,error)&&archive.images[0].name=="caf\xc3\xa9.svg","decode legacy ZIP CP437 member names separately from declared UTF-8");
    atomic_file::WriteAll(pack,Zip("bad\xc0.svg",packed,0x7e42e513,110));check(!icon_pack::Read(pack,archive,error),"invalid declared UTF-8 file names fail explicitly");
    atomic_file::WriteAll(pack,Zip("large.svg",packed,0x7e42e513,17000000));check(!icon_pack::Read(pack,archive,error)&&error=="iconLibrary.packTooLarge","reject decompression output above the per-image budget");
    for(const auto compact:{false,true})for(const auto sparse:{false,true})
    {
        icon_pack::Archive apk;
        apk.documents={{"resources.arsc",Table(compact,sparse)},
            {"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},
            {"res/sample.xml",R"(<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android"><background android:drawable="@color/back"/><foreground><inset android:inset="25%"><vector android:viewportWidth="24" android:viewportHeight="24"><path android:fillColor="#80ffffff" android:pathData="M0,0H24V24H0Z"/></vector></inset></foreground><monochrome><vector android:viewportWidth="24" android:viewportHeight="24"><path android:fillColor="#ff000000" android:pathData="M6,6H18V18H6Z"/></vector></monochrome></adaptive-icon>)"}};
        check(android_icon_pack::Convert(apk,error)&&apk.images.size()==2,"resolve Android full/compact/sparse tables and separate adaptive monochrome variant");
        if(apk.images.size()==2)
        {
            icon_image::Image color,mono;check(icon_image::DecodeBytes(apk.images[0].bytes,108,color,error)&&icon_image::DecodeBytes(apk.images[1].bytes,108,mono,error),"rasterize composed Android assets through production SVG codec");
            if(color.pixels.size()==108*108&&mono.pixels.size()==108*108)
            { const auto center=color.pixels[54*108+54];check(color.pixels[0]==0xffff0000&&(center>>24)==255&&((center>>16)&255)==255&&((center>>8)&255)>=127&&((center>>8)&255)<=129&&((color.pixels[54*108+20]>>8)&255)>=127,"adaptive foreground composites once over its background with inset and the Android 72-of-108 visible viewport");check(mono.pixels[0]==0&&mono.pixels[54*108+54]==0xff000000,"monochrome is a transparent independent variant rather than an extra colored layer"); }
        }
    }
    {
        icon_pack::Archive bitmap;
        bitmap.documents={{"resources.arsc",Table(false,false,false,true)},
            {"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},
            {"res/sample.xml","<adaptive-icon><background drawable=\"@color/back\"/><foreground drawable=\"@drawable/foreground\"/><monochrome drawable=\"@drawable/foreground\"/></adaptive-icon>"}};
        bitmap.images={{"res/foreground.png",Hex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c63f8cfc0d00000048101802c55ceb00000000049454e44ae426082")}};
        check(android_icon_pack::Convert(bitmap,error)&&bitmap.images.size()==2,"adaptive bitmap layers become bounded self-contained SVG images");
        if(bitmap.images.size()==2)
        {
            icon_image::Image color,mono;
            check(icon_image::DecodeBytes(bitmap.images[0].bytes,32,color,error)&&icon_image::DecodeBytes(bitmap.images[1].bytes,32,mono,error)&&
                !color.pixels.empty()&&color.pixels[16*32+16]==0xffff0000&&!mono.pixels.empty()&&mono.pixels[16*32+16]==0x80000000,
                "bitmap composition retains source alpha while the separate monochrome variant uses only its alpha mask");
        }
        icon_pack::Archive rectangle;
        rectangle.documents={{"resources.arsc",Table()},{"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},
            {"res/sample.xml","<vector width=\"48\" height=\"24\" viewportWidth=\"24\" viewportHeight=\"12\"><path fillColor=\"#ff0000\" pathData=\"M0,0H24V12H0Z\"/></vector>"}};
        icon_image::Image rectangularVector;
        check(android_icon_pack::Convert(rectangle,error)&&rectangle.images.size()==1&&icon_image::DecodeBytes(rectangle.images[0].bytes,160,rectangularVector,error)&&
            rectangularVector.width==160&&rectangularVector.height==80&&rectangularVector.sourceWidth==48&&rectangularVector.sourceHeight==24,
            "non-adaptive Android vectors retain intrinsic aspect ratio instead of stretching to a square");
    }
    icon_pack::Archive invalid;invalid.documents={{"resources.arsc",Table(false,false,true)},{"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},{"res/sample.xml","<adaptive-icon><background drawable=\"@color/back\"/></adaptive-icon>"}};
    check(!android_icon_pack::Convert(invalid,error),"cyclic Android resource references terminate with an explicit unsupported result");
    invalid.documents = {{"resources.arsc",Table()}}; invalid.documents[0].bytes.resize(17);check(!android_icon_pack::Convert(invalid,error),"truncated Android table cannot access outside its bounded input");
    {
        // Compact repeated offsets remain valid Android data. Reject their
        // expanded materialization instead of relying on an allocation failure.
        std::vector<std::string> repeated(1102,std::string(16384,'x'));repeated[0]="res/sample.xml";repeated[1]="res/foreground.xml";
        icon_pack::Archive excessive;
        excessive.documents={{"resources.arsc",Table(false,false,false,false,Pool(repeated))},
            {"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},
            {"res/sample.xml","<vector viewportWidth=\"24\" viewportHeight=\"24\"><path fillColor=\"#ff0000\" pathData=\"M0,0H24V24H0Z\"/></vector>"}};
        check(!android_icon_pack::Convert(excessive,error)&&error=="iconLibrary.packTooLarge","reject repeated string offsets exceeding the materialized string budget");
        std::string layers="<layer-list>";for(unsigned i=0;i<300;++i)layers+="<item drawable=\"@drawable/foreground\"/>";layers+="</layer-list>";
        std::string path="M0,0";for(unsigned i=0;i<10000;++i)path+="L24,24";
        excessive={};excessive.documents={{"resources.arsc",Table()},
            {"assets/appfilter.xml","<resources><item drawable=\"sample\"/></resources>"},{"res/sample.xml",layers},
            {"res/foreground.xml","<vector viewportWidth=\"24\" viewportHeight=\"24\"><path fillColor=\"#ff0000\" pathData=\""+path+"\"/></vector>"}};
        check(!android_icon_pack::Convert(excessive,error)&&error=="iconLibrary.packTooLarge","reject drawable reference fanout exceeding the generated SVG budget");
    }
    icon_image::Image rectangular;rectangular.width=40;rectangular.height=20;rectangular.sourceWidth=40;rectangular.sourceHeight=20;rectangular.pixels.assign(800,0x80800000);
    check(icon_image::PadSquare(rectangular)&&rectangular.width==40&&rectangular.height==40&&rectangular.pixels[0]==0&&rectangular.pixels[20*40+20]==0x80800000,"non-square custom images are centered on a transparent square without stretching or alpha changes");
    IconBeautifySettings settings;settings.enabled=true;settings.mode=1;settings.outlineEnabled=true;settings.shape=IconBeautifyShape::LegacyRounded;
    const auto decorated=icon_beautify::Render(rectangular.pixels,40,40,settings);
    check(decorated.size()==1600&&(decorated[20*40+1]>>24)>0&&(decorated[1*40+20]>>24)>0,"portrait and landscape art share the same square beautification frame");
    std::string encoded;icon_image::Image restored;
    check(icon_image::EncodePng(rectangular,encoded,error)&&icon_image::DecodeBytes(encoded,0,restored,error)&&restored.pixels==rectangular.pixels,"in-memory PNG used by Android bitmap layers preserves premultiplied color and alpha");
    return failures?1:0;
}

int RunIconPackProbe(const char* source,const char* destination)
{
    using namespace snowdesktop;const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int result=0;
    try
    {
        const auto output=std::filesystem::absolute(PathUtf8(destination));std::filesystem::create_directories(output);
        IconLibrary library(output/L"library");IconLibraryRequest request;request.action="import";request.paths={PathUtf8(source).wstring()};
        const auto snapshot=library.Invoke(request);std::ofstream report(output/L"import.tsv");
        report<<"assets\t"<<snapshot.entries.size()<<"\ngroups\t"<<snapshot.groups.size()<<"\nfailed\t"<<snapshot.failures.size()<<"\nerror\t"<<snapshot.error<<'\n';
        for(const auto& failure:snapshot.failures)report<<"failed-resource\t"<<Utf8(failure)<<'\n';
        if(snapshot.entries.empty()||snapshot.groups.size()!=1)result=1;
        unsigned samples=0;std::ofstream names(output/L"assets.tsv");
        for(const auto& entry:snapshot.entries)
        {
            names<<entry.id<<'\t'<<Utf8(entry.name)<<'\n';
            const auto name=Utf8(entry.name);
            if(samples>=12|| (samples>=3&&name.find("chrome")==name.npos&&name.find("github")==name.npos&&name.find("firefox")==name.npos&&name.find("telegram")==name.npos))continue;
            std::string error;const auto file=output/("sample-"+std::to_string(samples++)+".png");
            if(!icon_image::ExportPng(IconAssetPath(output/L"library",entry.id),file,error))result=1;
            report<<"sample\t"<<file.filename().string()<<'\t'<<name<<'\n';
        }
        IconLibrary reopened(output/L"library");const auto restored=reopened.Read();if(restored.entries.size()!=snapshot.entries.size()||restored.groups.size()!=snapshot.groups.size())result=1;
        std::cout<<"assets="<<snapshot.entries.size()<<" groups="<<snapshot.groups.size()<<" skipped="<<snapshot.failures.size()<<" result="<<result<<'\n';
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}
    if(SUCCEEDED(initialized))CoUninitialize();return result;
}
