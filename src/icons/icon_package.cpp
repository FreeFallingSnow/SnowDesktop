#include "icon_package.h"
#include "icon_library.h"
#include "icon_image_codec.h"
#include "icon_pack_archive.h"
#include "data/atomic_file.h"
#include "../../steam_bridge/src/bridge_json.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <set>
#include <unordered_map>

namespace snowdesktop::icon_package
{
namespace
{
using steam_bridge::JsonValue;
bool Fail(std::string& error, const char* code) { error = code; return false; }
bool Text(std::string_view text, std::size_t maximum, bool required = false)
{
    return text.size() <= maximum && (!required || text.find_first_not_of(" \t\r\n") != text.npos) &&
        text.find('\0') == text.npos && (!required || (std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32;})&&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)<=256)) && (text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0) > 0);
}
JsonValue Manifest(const Package& package)
{
    auto value = JsonValue::Object();
    value.object["format"] = JsonValue::String("snowdesktop-icon-pack");
    value.object["version"] = JsonValue::Number(1);
    value.object["id"] = JsonValue::String(package.id);
    value.object["title"] = JsonValue::String(package.title);
    value.object["description"] = JsonValue::String(package.description);
    value.object["license"] = JsonValue::String(package.license);
    auto images = JsonValue::Array();
    for (const auto& image : package.images)
    {
        auto item = JsonValue::Object(); item.object["id"] = JsonValue::String(image.id);
        item.object["name"] = JsonValue::String(image.name);
        item.object["path"] = JsonValue::String("assets/" + image.id);
        item.object["sha256"] = JsonValue::String(image.id.substr(0, 64));
        images.array.push_back(std::move(item));
    }
    value.object["icons"] = std::move(images); return value;
}
std::uint32_t Crc(std::string_view bytes)
{
    std::uint32_t crc = 0xffffffff;
    for (const unsigned char c : bytes) { crc ^= c; for (int n = 0; n < 8; ++n) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
void Put(std::string& bytes, std::uint32_t value, unsigned count)
{ for (unsigned n = 0; n < count; ++n) bytes += static_cast<char>((value >> (n * 8)) & 255); }
}
std::string Hash(std::string_view bytes)
{
    BCRYPT_ALG_HANDLE algorithm{}; std::array<unsigned char, 32> digest{};
    if (bytes.size() > MaxExpanded || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0); if (status < 0) return {};
    std::string out; constexpr char hex[] = "0123456789abcdef";
    for (const auto c : digest) { out += hex[c >> 4]; out += hex[c & 15]; } return out;
}
bool SafePath(const std::filesystem::path& path, bool directory)
{
    if (!path.is_absolute()) return false;
    for (auto current = path; !current.empty();)
    {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (!!(attributes & FILE_ATTRIBUTE_DIRECTORY) != (current == path ? directory : true))) return false;
        const auto next = current.parent_path(); if (next == current) break; current = next;
    }
    return true;
}
bool Validate(const Package& package, std::string& error)
{
    if (package.id.size() != 38 || !package.id.starts_with("group-") ||
        package.id.find_first_not_of("0123456789abcdef", 6) != package.id.npos ||
        !Text(package.title, 1024, true) || !Text(package.description, 32000) || !Text(package.license, 16384) ||
        package.images.empty() || package.images.size() >= 50000) return Fail(error, "iconLibrary.invalid");
    std::set<std::string> ids; std::uint64_t expanded = 0;
    for (const auto& image : package.images)
    {
        expanded += image.bytes.size();
        if (expanded > MaxExpanded || image.bytes.size() > icon_image::kMaxFileBytes) return Fail(error, "iconLibrary.packTooLarge");
        if (!IsIconAssetId(image.id) || !ids.insert(image.id).second || !Text(image.name, 1024, true) ||
            image.bytes.empty() || Hash(image.bytes) != image.id.substr(0, 64)) return Fail(error, "iconLibrary.invalid");
        icon_image::Image decoded;
        if (!icon_image::DecodeBytes(image.bytes, 64, decoded, error) || decoded.format != image.id.substr(65))
            return Fail(error, "iconLibrary.invalid");
    }
    error.clear(); return true;
}
std::string Fingerprint(const Package& package)
{ return Hash(steam_bridge::WriteJson(Manifest(package), -1)); }
bool Write(const std::filesystem::path& path, const Package& package, std::string& error)
{
    if (!Validate(package, error)) return false;
    const auto manifest = steam_bridge::WriteJson(Manifest(package), -1);
    if(manifest.size()>icon_image::kMaxFileBytes)return Fail(error,"iconLibrary.packTooLarge");
    struct File { std::string name; std::string_view bytes; std::uint32_t crc = 0, offset = 0; };
    std::vector<File> files{{"icon-pack.json", manifest}};
    for (const auto& image : package.images) files.push_back({"assets/" + image.id, image.bytes});
    std::string bytes;
    for (auto& file : files)
    {
        if (bytes.size() + file.bytes.size() + file.name.size() + 30 > MaxArchive) return Fail(error, "iconLibrary.packTooLarge");
        file.offset = static_cast<std::uint32_t>(bytes.size()); file.crc = Crc(file.bytes);
        Put(bytes,0x04034b50,4); Put(bytes,20,2); Put(bytes,0x800,2); Put(bytes,0,2); Put(bytes,0,4);
        Put(bytes,file.crc,4); Put(bytes,static_cast<std::uint32_t>(file.bytes.size()),4); Put(bytes,static_cast<std::uint32_t>(file.bytes.size()),4);
        Put(bytes,static_cast<std::uint32_t>(file.name.size()),2); Put(bytes,0,2); bytes += file.name; bytes += file.bytes;
    }
    const auto central = static_cast<std::uint32_t>(bytes.size());
    for (const auto& file : files)
    {
        Put(bytes,0x02014b50,4); Put(bytes,20,2); Put(bytes,20,2); Put(bytes,0x800,2); Put(bytes,0,2); Put(bytes,0,4);
        Put(bytes,file.crc,4); Put(bytes,static_cast<std::uint32_t>(file.bytes.size()),4); Put(bytes,static_cast<std::uint32_t>(file.bytes.size()),4);
        Put(bytes,static_cast<std::uint32_t>(file.name.size()),2); Put(bytes,0,2); Put(bytes,0,2); Put(bytes,0,2); Put(bytes,0,2); Put(bytes,0,4); Put(bytes,file.offset,4); bytes += file.name;
    }
    const auto centralSize = static_cast<std::uint32_t>(bytes.size()) - central;
    Put(bytes,0x06054b50,4); Put(bytes,0,4); Put(bytes,static_cast<std::uint32_t>(files.size()),2); Put(bytes,static_cast<std::uint32_t>(files.size()),2);
    Put(bytes,centralSize,4); Put(bytes,central,4); Put(bytes,0,2);
    if (bytes.size() > MaxArchive) return Fail(error, "iconLibrary.packTooLarge");
    return atomic_file::WriteAll(path, bytes) || Fail(error, "iconLibrary.saveFailed");
}
bool Read(const std::filesystem::path& path, Package& package, std::string& error)
{
    package = {}; icon_pack::Archive archive;
    if (!SafePath(path) || !icon_pack::Read(path, archive, error, true)) return Fail(error, "iconLibrary.invalid");
    if (archive.documents.size() != 1 || archive.documents.front().name != "icon-pack.json") return Fail(error, "iconLibrary.invalid");
    JsonValue value;
    if (!steam_bridge::ParseJson(archive.documents.front().bytes, value, error) ||
        steam_bridge::JsonString(value,"format") != "snowdesktop-icon-pack" || steam_bridge::JsonUnsigned(value,"version") != 1)
        return Fail(error, "iconLibrary.invalid");
    Package next; next.id = steam_bridge::JsonString(value,"id").value_or("");
    for(const auto* key:{"description","license"})if(const auto* field=value.Find(key);field&&!field->IsString())return Fail(error,"iconLibrary.invalid");
    next.title = steam_bridge::JsonString(value,"title").value_or("");
    next.description = steam_bridge::JsonString(value,"description").value_or(""); next.license = steam_bridge::JsonString(value,"license").value_or("");
    const auto* icons = value.Find("icons");
    if (!icons || !icons->IsArray() || icons->array.size() != archive.images.size()) return Fail(error, "iconLibrary.invalid");
    std::unordered_map<std::string,std::string*> resources;
    for(auto& entry:archive.images)resources.emplace(entry.name,&entry.bytes);
    for (const auto& item : icons->array)
    {
        Image image; image.id = steam_bridge::JsonString(item,"id").value_or(""); image.name = steam_bridge::JsonString(item,"name").value_or("");
        if (!IsIconAssetId(image.id) || steam_bridge::JsonString(item,"path") != "assets/" + image.id ||
            steam_bridge::JsonString(item,"sha256") != image.id.substr(0,64)) return Fail(error,"iconLibrary.invalid");
        const auto file=resources.find("assets/"+image.id);
        if (file == resources.end()) return Fail(error,"iconLibrary.invalid");
        image.bytes = std::move(*file->second); next.images.push_back(std::move(image));
    }
    if (!Validate(next,error)) return false;
    package = std::move(next); return true;
}
}
