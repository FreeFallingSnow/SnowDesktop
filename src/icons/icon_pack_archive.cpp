#include "icon_pack_archive.h"
#include "icon_image_codec.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <string_view>
#include <set>
#include <map>
extern "C" {
#include "puff.h"
}
namespace snowdesktop::icon_pack
{
namespace
{
constexpr std::uint64_t maxArchive = 128ull * 1024 * 1024, maxExpanded = 256ull * 1024 * 1024;
unsigned U16(std::string_view b, size_t p)
{ return static_cast<unsigned char>(b[p]) | (static_cast<unsigned char>(b[p + 1]) << 8); }
std::uint32_t U32(std::string_view b, size_t p)
{ return U16(b, p) | (static_cast<std::uint32_t>(U16(b, p + 2)) << 16); }
std::uint32_t Crc(std::string_view bytes)
{
    std::uint32_t crc = 0xffffffff;
    for (const unsigned char c : bytes) { crc ^= c; for (int n = 0; n < 8; ++n) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
std::string Lower(std::string text)
{ for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return text; }
bool ImageName(const std::string& name)
{
    const auto dot = name.rfind('.'); if (dot == name.npos) return false;
    const auto extension = name.substr(dot);
    return extension == ".png" || extension == ".ico" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".svg" || extension == ".webp";
}
bool SafeName(std::string_view name)
{
    if (name.empty() || name.front() == '/' || name.find_first_of("\\:\0", 0, 3) != name.npos) return false;
    for (size_t start = 0; start < name.size();)
    {
        const auto end = name.find('/', start); const auto part = name.substr(start, end == name.npos ? name.size() - start : end - start);
        if (part == ".." || part == ".") return false;
        if (end == name.npos) break; start = end + 1;
    }
    return true;
}
std::string Utf8Name(std::string_view name, bool utf8)
{
    const UINT page = utf8 ? CP_UTF8 : 437;
    const int count = MultiByteToWideChar(page, utf8 ? MB_ERR_INVALID_CHARS : 0,
        name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (!count) return {};
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(page, utf8 ? MB_ERR_INVALID_CHARS : 0, name.data(), static_cast<int>(name.size()), wide.data(), count);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, result.data(), bytes, nullptr, nullptr);
    return result;
}
}
bool Read(const std::filesystem::path& path, Archive& result, std::string& error, bool strictIconPackage)
{
    result = {}; error.clear();
    const auto fail = [&](const char* why) { error = why; result.images.clear(); return false; };
    std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 22 || size > maxArchive) return fail("iconLibrary.packTooLarge");
    std::ifstream input(path, std::ios::binary); std::string bytes(static_cast<size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(size)) || input.peek() != EOF) return fail("iconLibrary.invalid");
    size_t end = bytes.size() - 22; bool found = false;
    const size_t first = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (;; --end)
    {
        if (U32(bytes, end) == 0x06054b50 && end + 22 + U16(bytes, end + 20) == bytes.size()) { found = true; break; }
        if (end == first) break;
    }
    if (!found || U16(bytes, end + 4) || U16(bytes, end + 6) || U16(bytes, end + 8) != U16(bytes, end + 10)) return fail("iconLibrary.invalid");
    const auto count = U16(bytes, end + 10); size_t pos = U32(bytes, end + 16);
    if (!count || count > 50000 || pos > end || U32(bytes, end + 12) != end - pos) return fail("iconLibrary.invalid");
    const bool apk = Lower(path.extension().string()) == ".apk";
    const auto centralStart=pos;
    std::map<std::size_t,std::size_t> localRanges;
    std::uint64_t expanded = 0;
    std::set<std::string> names;
    for (unsigned i = 0; i < count; ++i)
    {
        if (pos > end || end - pos < 46 || U32(bytes, pos) != 0x02014b50) return fail("iconLibrary.invalid");
        const auto flags = U16(bytes, pos + 8), method = U16(bytes, pos + 10);
        const auto crc = U32(bytes, pos + 16), packed = U32(bytes, pos + 20), unpacked = U32(bytes, pos + 24);
        const size_t nameSize = U16(bytes, pos + 28), extra = U16(bytes, pos + 30), comment = U16(bytes, pos + 32);
        const size_t local = U32(bytes, pos + 42);
        if(strictIconPackage&&((flags&~0x808u)||((U32(bytes,pos+38)>>16)&0170000)==0120000))return fail("iconLibrary.invalid");
        if (46 + nameSize + extra + comment > end - pos || U16(bytes, pos + 34)) return fail("iconLibrary.invalid");
        const std::string name = bytes.substr(pos + 46, nameSize), lower = Lower(name);
        pos += 46 + nameSize + extra + comment;
        if (!SafeName(name)) return fail("iconLibrary.invalid");
        if (strictIconPackage && (!names.insert(lower).second || (name != "icon-pack.json" &&
            (!name.starts_with("assets/") || !ImageName(lower))))) return fail("iconLibrary.invalid");
        if (name.back() == '/') continue;
        const bool resource = lower.starts_with("res/") || lower == "resources.arsc" || lower == "assets/appfilter.xml" || lower == "assets/drawable.xml";
        const bool document = (apk && (lower.ends_with(".xml") || lower == "resources.arsc")) || (strictIconPackage && name == "icon-pack.json");
        if (apk && !resource) continue;
        if (!ImageName(lower) && !document) continue;
        // Launcher UI nine-patches are not mapped icons. A mapped reference to
        // one is reported by the Android converter using its icon identity.
        if (apk && lower.ends_with(".9.png")) continue;
        expanded += unpacked;
        if (unpacked > icon_image::kMaxFileBytes || expanded > maxExpanded) return fail("iconLibrary.packTooLarge");
        if (!unpacked || flags & 1 || (method != 0 && method != 8)) { if (strictIconPackage) return fail("iconLibrary.invalid"); result.skipped.push_back(name); continue; }
        if (local > end || end - local < 30 || U32(bytes, local) != 0x04034b50 ||
            U16(bytes, local + 8) != method || U16(bytes, local + 6) != flags) return fail("iconLibrary.invalid");
        const size_t data = local + 30 + U16(bytes, local + 26) + U16(bytes, local + 28);
        if (data > end || packed > end - data || U16(bytes, local + 26) != nameSize || bytes.compare(local + 30, nameSize, name)) return fail("iconLibrary.invalid");
        if(strictIconPackage)
        {
            const auto next=localRanges.lower_bound(local);
            if(data>centralStart||packed>centralStart-data||(next!=localRanges.end()&&next->first<data+packed)||
                (next!=localRanges.begin()&&std::prev(next)->second>local))return fail("iconLibrary.invalid");
            localRanges.emplace(local,data+packed);
            if(!(flags&8)&&(U32(bytes,local+14)!=crc||U32(bytes,local+18)!=packed||U32(bytes,local+22)!=unpacked))return fail("iconLibrary.invalid");
        }
        Entry entry; entry.name = Utf8Name(name, (flags & 0x800) != 0); entry.bytes.resize(unpacked);
        if (entry.name.empty()) return fail("iconLibrary.invalid");
        if (method == 0)
        { if (packed != unpacked) return fail("iconLibrary.invalid"); entry.bytes.assign(bytes, data, packed); }
        else
        {
            unsigned long outputSize = unpacked, inputSize = packed;
            if (puff(reinterpret_cast<unsigned char*>(entry.bytes.data()), &outputSize,
                reinterpret_cast<const unsigned char*>(bytes.data() + data), &inputSize) != 0 || outputSize != unpacked || inputSize != packed) return fail("iconLibrary.invalid");
        }
        if (Crc(entry.bytes) != crc) return fail("iconLibrary.invalid");
        (document ? result.documents : result.images).push_back(std::move(entry));
    }
    if (pos != end) return fail("iconLibrary.invalid");
    if (result.images.empty() && result.documents.empty()) return fail("iconLibrary.packUnsupported");
    result.original = std::move(bytes);
    return true;
}
}
