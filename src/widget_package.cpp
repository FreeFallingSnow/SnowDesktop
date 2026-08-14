#include "widget_package.h"

#include "json_value.h"
#include "language_fallback.h"
#include "widget_permission_broker.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

namespace snowdesktop::widget
{
namespace
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string JsonEscape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[(ch >> 4) & 0x0f]);
                out.push_back(hex[ch & 0x0f]);
            }
            else out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.generic_wstring());
}

bool ReadFile(const std::filesystem::path& path, std::string& output,
    std::uint64_t maxBytes = kMaxExtractedBytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    output.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

bool AtomicWrite(const std::filesystem::path& target, const std::string& data,
    std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create parent directory: " + ec.message();
        return false;
    }
    const auto temporary = target.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "cannot open temporary file";
            return false;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file)
        {
            error = "cannot write temporary file";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot atomically replace file: " +
            std::to_string(GetLastError());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool DecimalIdentifier(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char character)
        {
            return character >= '0' && character <= '9';
        });
}

bool StartsWithPath(const std::filesystem::path& child,
    const std::filesystem::path& root)
{
    auto childIt = child.begin();
    auto rootIt = root.begin();
    for (; rootIt != root.end(); ++rootIt, ++childIt)
    {
        if (childIt == child.end() ||
            _wcsicmp(childIt->c_str(), rootIt->c_str()) != 0)
            return false;
    }
    return true;
}

bool HasReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsResourceName(std::string_view value)
{
    if (value.empty() || value.size() > 64 ||
        !std::islower(static_cast<unsigned char>(value.front())))
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '-' || ch == '_';
    });
}

std::uint16_t ReadBig16(const std::string& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(data[offset]) << 8) |
        static_cast<unsigned char>(data[offset + 1]));
}

std::uint32_t ReadBig32(const std::string& data, std::size_t offset)
{
    return (static_cast<std::uint32_t>(
        static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<unsigned char>(data[offset + 3]);
}

std::uint32_t ReadLittle32(const std::string& data, std::size_t offset)
{
    return static_cast<unsigned char>(data[offset]) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 3])) << 24);
}

std::uint16_t ReadLittle16(const std::string& data, std::size_t offset)
{
    return static_cast<unsigned char>(data[offset]) |
        (static_cast<std::uint16_t>(
            static_cast<unsigned char>(data[offset + 1])) << 8);
}

bool PixelCountAllowed(std::uint32_t width, std::uint32_t height)
{
    return width > 0 && height > 0 &&
        static_cast<std::uint64_t>(width) * height <= 64ull * 1024ull * 1024ull;
}

bool SkipGifSubBlocks(const std::string& data, std::size_t& offset)
{
    while (offset < data.size())
    {
        const std::size_t size = static_cast<unsigned char>(data[offset++]);
        if (size == 0) return true;
        if (size > data.size() - offset) return false;
        offset += size;
    }
    return false;
}

bool StaticGifIsValid(const std::string& data)
{
    if (data.size() < 13 ||
        (data.compare(0, 6, "GIF87a") != 0 &&
            data.compare(0, 6, "GIF89a") != 0) ||
        !PixelCountAllowed(ReadLittle16(data, 6), ReadLittle16(data, 8)))
        return false;
    std::size_t offset = 13;
    const unsigned char screenFlags =
        static_cast<unsigned char>(data[10]);
    if (screenFlags & 0x80)
    {
        const std::size_t colorTableBytes =
            3u << ((screenFlags & 0x07) + 1);
        if (colorTableBytes > data.size() - offset) return false;
        offset += colorTableBytes;
    }
    unsigned int imageCount = 0;
    while (offset < data.size())
    {
        const unsigned char block =
            static_cast<unsigned char>(data[offset++]);
        if (block == 0x3b) return imageCount == 1;
        if (block == 0x21)
        {
            if (offset >= data.size()) return false;
            ++offset;
            if (!SkipGifSubBlocks(data, offset)) return false;
            continue;
        }
        if (block != 0x2c || offset + 9 > data.size()) return false;
        if (++imageCount > 1 ||
            !PixelCountAllowed(ReadLittle16(data, offset + 4),
                ReadLittle16(data, offset + 6)))
            return false;
        const unsigned char imageFlags =
            static_cast<unsigned char>(data[offset + 8]);
        offset += 9;
        if (imageFlags & 0x80)
        {
            const std::size_t colorTableBytes =
                3u << ((imageFlags & 0x07) + 1);
            if (colorTableBytes > data.size() - offset) return false;
            offset += colorTableBytes;
        }
        if (offset >= data.size()) return false;
        ++offset;
        if (!SkipGifSubBlocks(data, offset)) return false;
    }
    return false;
}

bool IconDirectoryIsValid(const std::string& data)
{
    if (data.size() < 6 || ReadLittle16(data, 0) != 0 ||
        ReadLittle16(data, 2) != 1)
        return false;
    const std::uint16_t count = ReadLittle16(data, 4);
    if (count == 0 || count > 256 ||
        static_cast<std::size_t>(count) > (data.size() - 6) / 16)
        return false;
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::size_t entry = 6 + index * 16;
        const std::uint32_t width =
            static_cast<unsigned char>(data[entry]) == 0 ? 256 :
                static_cast<unsigned char>(data[entry]);
        const std::uint32_t height =
            static_cast<unsigned char>(data[entry + 1]) == 0 ? 256 :
                static_cast<unsigned char>(data[entry + 1]);
        const std::uint32_t bytes = ReadLittle32(data, entry + 8);
        const std::uint32_t imageOffset = ReadLittle32(data, entry + 12);
        if (!PixelCountAllowed(width, height) || bytes == 0 ||
            imageOffset > data.size() || bytes > data.size() - imageOffset)
            return false;
    }
    return true;
}

bool JpegDimensionsAllowed(const std::string& data)
{
    std::size_t offset = 2;
    while (offset + 4 <= data.size())
    {
        while (offset < data.size() &&
            static_cast<unsigned char>(data[offset]) != 0xff)
            ++offset;
        while (offset < data.size() &&
            static_cast<unsigned char>(data[offset]) == 0xff)
            ++offset;
        if (offset >= data.size()) break;
        const unsigned char marker =
            static_cast<unsigned char>(data[offset++]);
        if (marker == 0xd8 || marker == 0xd9 ||
            (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (offset + 2 > data.size()) break;
        const std::uint16_t length = ReadBig16(data, offset);
        if (length < 2 || offset + length > data.size()) break;
        const bool startOfFrame =
            (marker >= 0xc0 && marker <= 0xc3) ||
            (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) ||
            (marker >= 0xcd && marker <= 0xcf);
        if (startOfFrame && length >= 7)
        {
            const std::uint32_t height = ReadBig16(data, offset + 3);
            const std::uint32_t width = ReadBig16(data, offset + 5);
            return PixelCountAllowed(width, height);
        }
        offset += length;
    }
    return false;
}

bool ResourceContentIsValid(const std::filesystem::path& path,
    const PackageResource& resource)
{
    std::string data;
    if (!ReadFile(path, data, kMaxResourceBytes)) return false;
    const std::string extension = Lower(path.extension().string());
    if (resource.type == "font")
    {
        if (data.size() < 4) return false;
        return (extension == ".ttf" &&
                static_cast<unsigned char>(data[0]) == 0x00 &&
                static_cast<unsigned char>(data[1]) == 0x01 &&
                static_cast<unsigned char>(data[2]) == 0x00 &&
                static_cast<unsigned char>(data[3]) == 0x00) ||
            (extension == ".otf" && data.compare(0, 4, "OTTO") == 0);
    }
    if (resource.type != "image") return false;
    if (extension == ".png")
    {
        return data.size() >= 24 &&
            data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 &&
            data.compare(12, 4, "IHDR") == 0 &&
            PixelCountAllowed(ReadBig32(data, 16), ReadBig32(data, 20));
    }
    if (extension == ".jpg" || extension == ".jpeg")
    {
        return data.size() >= 4 &&
            static_cast<unsigned char>(data[0]) == 0xff &&
            static_cast<unsigned char>(data[1]) == 0xd8 &&
            JpegDimensionsAllowed(data);
    }
    if (extension == ".gif")
        return StaticGifIsValid(data);
    if (extension == ".bmp")
    {
        if (data.size() < 26 || data.compare(0, 2, "BM") != 0) return false;
        const std::int32_t width = static_cast<std::int32_t>(
            ReadLittle32(data, 18));
        const std::int32_t height = static_cast<std::int32_t>(
            ReadLittle32(data, 22));
        if (width <= 0 || height == 0 || height ==
            (std::numeric_limits<std::int32_t>::min)())
            return false;
        return PixelCountAllowed(static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height < 0 ? -height : height));
    }
    if (extension == ".ico")
        return IconDirectoryIsValid(data);
    return false;
}

bool ReadString(const JsonValue& object, const char* name, std::string& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsString()) return false;
    output = value->string;
    return true;
}

bool ReadInteger(const JsonValue& object, const char* name, int& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsNumber() || !std::isfinite(value->number) ||
        value->number != std::floor(value->number) ||
        value->number < static_cast<double>((std::numeric_limits<int>::min)()) ||
        value->number > static_cast<double>((std::numeric_limits<int>::max)()))
        return false;
    output = static_cast<int>(value->number);
    return true;
}

std::vector<std::string> ReadStringArray(const JsonValue& object,
    const char* name, bool& valid)
{
    std::vector<std::string> result;
    const JsonValue* value = object.Find(name);
    if (!value) return result;
    if (!value->IsArray())
    {
        valid = false;
        return result;
    }
    for (const auto& item : value->array)
    {
        if (!item.IsString())
        {
            valid = false;
            continue;
        }
        result.push_back(item.string);
    }
    return result;
}

void ReadSize(const JsonValue& root, const char* name, int& columns, int& rows)
{
    const JsonValue* value = root.Find(name);
    if (!value || !value->IsObject()) return;
    ReadInteger(*value, "columns", columns);
    ReadInteger(*value, "rows", rows);
}

std::string ManifestJson(const PackageManifest& manifest)
{
    auto appendArray = [](std::ostringstream& out,
        const std::vector<std::string>& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i) out << ", ";
            out << '"' << JsonEscape(values[i]) << '"';
        }
        out << ']';
    };
    auto appendStringMap = [](std::ostringstream& out,
        const std::unordered_map<std::string, std::string>& values) {
        bool first = true;
        std::vector<std::pair<std::string, std::string>> sorted(
            values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& [key, value] : sorted)
        {
            if (!first) out << ", ";
            first = false;
            out << '"' << JsonEscape(key) << "\": \""
                << JsonEscape(value) << '"';
        }
    };
    std::ostringstream out;
    out << "{\n"
        << "  \"schemaVersion\": " << manifest.schemaVersion << ",\n"
        << "  \"id\": \"" << JsonEscape(manifest.id) << "\",\n"
        << "  \"slug\": \"" << JsonEscape(manifest.slug) << "\",\n"
        << "  \"version\": \"" << JsonEscape(manifest.version) << "\",\n"
        << "  \"apiVersion\": " << manifest.apiVersion << ",\n"
        << "  \"dataVersion\": " << manifest.dataVersion << ",\n"
        << "  \"entry\": \"" << JsonEscape(manifest.entry) << "\",\n"
        << "  \"minHostVersion\": \"" << JsonEscape(manifest.minHostVersion) << "\",\n"
        << "  \"name\": \"" << JsonEscape(manifest.name) << "\",\n"
        << "  \"description\": \"" << JsonEscape(manifest.description) << "\",\n"
        << "  \"author\": \"" << JsonEscape(manifest.author) << "\",\n"
        << "  \"license\": \"" << JsonEscape(manifest.license) << "\",\n"
        << "  \"preview\": \"" << JsonEscape(manifest.preview) << "\",\n"
        << "  \"previewData\": {\"storage\": {";
    appendStringMap(out, manifest.previewStorage);
    out << "}, \"storageKeys\": {";
    appendStringMap(out, manifest.previewStorageKeys);
    out << "}, \"introduction\": \""
        << JsonEscape(manifest.previewIntroduction)
        << "\", \"introductionKey\": \""
        << JsonEscape(manifest.previewIntroductionKey)
        << "\", \"variants\": [";
    for (std::size_t i = 0; i < manifest.previewVariants.size(); ++i)
    {
        if (i) out << ", ";
        const PreviewVariant& variant = manifest.previewVariants[i];
        out << "{\"id\": \"" << JsonEscape(variant.id)
            << "\", \"title\": \"" << JsonEscape(variant.title)
            << "\", \"titleKey\": \"" << JsonEscape(variant.titleKey)
            << "\", \"description\": \""
            << JsonEscape(variant.description)
            << "\", \"descriptionKey\": \""
            << JsonEscape(variant.descriptionKey)
            << "\", \"size\": {\"columns\": " << variant.columns
            << ", \"rows\": " << variant.rows << "}, \"storage\": {";
        appendStringMap(out, variant.storage);
        out << "}, \"storageKeys\": {";
        appendStringMap(out, variant.storageKeys);
        out << "}}";
    }
    out << "]},\n"
        << "  \"defaultSize\": {\"columns\": " << manifest.defaultColumns
        << ", \"rows\": " << manifest.defaultRows << "},\n"
        << "  \"minSize\": {\"columns\": " << manifest.minColumns
        << ", \"rows\": " << manifest.minRows << "},\n"
        << "  \"maxSize\": {\"columns\": " << manifest.maxColumns
        << ", \"rows\": " << manifest.maxRows << "},\n"
        << "  \"resources\": {";
    {
        bool first = true;
        std::vector<std::string> resourceNames;
        resourceNames.reserve(manifest.resources.size());
        for (const auto& [name, resource] : manifest.resources)
        {
            (void)resource;
            resourceNames.push_back(name);
        }
        std::sort(resourceNames.begin(), resourceNames.end());
        for (const auto& name : resourceNames)
        {
            const auto& resource = manifest.resources.at(name);
            if (!first) out << ", ";
            first = false;
            out << '"' << JsonEscape(name) << "\": {\"type\": \""
                << JsonEscape(resource.type) << "\", \"path\": \""
                << JsonEscape(resource.path) << "\", \"license\": \""
                << JsonEscape(resource.license) << "\"}";
        }
    }
    out << "},\n"
        << "  \"permissions\": ";
    appendArray(out, manifest.permissions);
    out << ",\n  \"optionalPermissions\": ";
    appendArray(out, manifest.optionalPermissions);
    out << ",\n  \"networkDomains\": ";
    appendArray(out, manifest.networkDomains);
    out << ",\n  \"requiredFeatures\": ";
    appendArray(out, manifest.requiredFeatures);
    out << ",\n  \"optionalFeatures\": ";
    appendArray(out, manifest.optionalFeatures);
    out << "\n}\n";
    return out.str();
}

std::string Timestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u-%02u%02u%02u-%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return buffer;
}

struct BundledLegacyComponent
{
    const wchar_t* filename;
    const char* nameKey;
    const char* packageId;
};

constexpr std::array<BundledLegacyComponent, 8> kBundledLegacyComponents{ {
    { L"analog_clock.lua", "lua_widget.analog_clock.name",
        "64107f41-197a-426a-8f86-6eeb020f56b0" },
    { L"digital_clock.lua", "lua_widget.digital_clock.name",
        "b731dc11-92fa-404b-abf7-34741cd25277" },
    { L"media_control.lua", "lua_widget.media_control.name",
        "9ccc7bd1-2a5a-473a-9bf7-a7d9ed5605ef" },
    { L"pomodoro.lua", "lua_widget.pomodoro.name",
        "3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23" },
    { L"quick_launcher.lua", "lua_widget.quick_launcher.name",
        "5b22d9ef-3802-48a3-8632-dcf9a3c41668" },
    { L"rss_reader.lua", "lua_widget.rss_reader.name",
        "3a8b02f9-560c-49b9-9b22-bc4405d863ea" },
    { L"sticky_note.lua", "lua_widget.sticky_note.name",
        "4664f034-3c7c-4469-9a82-44db90f68094" },
    { L"system_monitor.lua", "lua_widget.system_monitor.name",
        "7d6803f2-63fd-428d-b947-6e07437ead2a" },
} };

std::uint32_t Crc32(const unsigned char* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Write16(std::ostream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    output.write(bytes, 2);
}

void Write32(std::ostream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    output.write(bytes, 4);
}

std::uint16_t Read16(const unsigned char* data)
{
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t Read32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

struct ZipItem
{
    std::string path;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
};

bool WriteStoreZip(const std::filesystem::path& root,
    const std::filesystem::path& output, std::string& error)
{
    std::vector<ZipItem> items;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        ZipItem item;
        item.path = PathUtf8(std::filesystem::relative(it->path(), root, ec));
        if (ec || item.path.empty())
        {
            error = "cannot create archive-relative path";
            return false;
        }
        std::replace(item.path.begin(), item.path.end(), '\\', '/');
        if (!ReadFile(it->path(), item.data))
        {
            error = "cannot read package file: " + item.path;
            return false;
        }
        item.crc = Crc32(reinterpret_cast<const unsigned char*>(item.data.data()),
            item.data.size());
        items.push_back(std::move(item));
    }
    if (ec)
    {
        error = "cannot enumerate package: " + ec.message();
        return false;
    }
    std::sort(items.begin(), items.end(),
        [](const ZipItem& a, const ZipItem& b) { return a.path < b.path; });

    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec)
    {
        error = "cannot create archive directory: " + ec.message();
        return false;
    }
    const auto temporary = output.wstring() + L".tmp";
    std::ofstream zip(temporary, std::ios::binary | std::ios::trunc);
    if (!zip)
    {
        error = "cannot create archive";
        return false;
    }
    for (auto& item : items)
    {
        const auto position = zip.tellp();
        if (position < 0 || position > std::numeric_limits<std::uint32_t>::max() ||
            item.data.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.path.size() > std::numeric_limits<std::uint16_t>::max())
        {
            error = "package is too large for ZIP32";
            return false;
        }
        item.offset = static_cast<std::uint32_t>(position);
        Write32(zip, 0x04034b50);
        Write16(zip, 20);
        Write16(zip, 0x0800);
        Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
        zip.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    }
    const auto centralOffsetValue = zip.tellp();
    if (centralOffsetValue < 0 ||
        centralOffsetValue > std::numeric_limits<std::uint32_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralOffset = static_cast<std::uint32_t>(centralOffsetValue);
    for (const auto& item : items)
    {
        Write32(zip, 0x02014b50);
        Write16(zip, 20); Write16(zip, 20);
        Write16(zip, 0x0800); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0); Write16(zip, 0); Write16(zip, 0); Write16(zip, 0);
        Write32(zip, 0);
        Write32(zip, item.offset);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
    }
    const auto endValue = zip.tellp();
    if (endValue < 0 || endValue > std::numeric_limits<std::uint32_t>::max() ||
        items.size() > std::numeric_limits<std::uint16_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralSize = static_cast<std::uint32_t>(endValue) - centralOffset;
    Write32(zip, 0x06054b50);
    Write16(zip, 0); Write16(zip, 0);
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write32(zip, centralSize); Write32(zip, centralOffset); Write16(zip, 0);
    zip.flush();
    if (!zip)
    {
        error = "cannot finish archive";
        zip.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    zip.close();
    std::filesystem::create_directories(output.parent_path(), ec);
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot atomically publish archive";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool IsKnownPermission(const std::string& permission)
{
    return IsKnownWidgetPermission(permission);
}

bool IsExplicitDnsName(const std::string& domain)
{
    if (domain.empty() || domain.size() > 253 ||
        domain != Lower(domain) || domain.find('.') == std::string::npos)
        return false;
    bool hasLetter = false;
    std::size_t begin = 0;
    while (begin < domain.size())
    {
        const auto end = domain.find('.', begin);
        const auto label = domain.substr(begin,
            end == std::string::npos ? domain.size() - begin : end - begin);
        if (label.empty() || label.size() > 63 ||
            !std::isalnum(static_cast<unsigned char>(label.front())) ||
            !std::isalnum(static_cast<unsigned char>(label.back())))
            return false;
        for (unsigned char ch : label)
        {
            if (std::isalpha(ch)) hasLetter = true;
            if (!(std::isalnum(ch) || ch == '-')) return false;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return hasLetter;
}

bool IsFeatureId(const std::string& feature)
{
    if (feature.size() < 3 || feature.size() > 96 ||
        feature != Lower(feature) || feature.find('.') == std::string::npos)
        return false;
    std::size_t begin = 0;
    while (begin < feature.size())
    {
        const auto end = feature.find('.', begin);
        const auto length = (end == std::string::npos
            ? feature.size() : end) - begin;
        if (length == 0 || length > 32 ||
            !std::isalpha(static_cast<unsigned char>(feature[begin])))
            return false;
        for (std::size_t index = begin; index < begin + length; ++index)
        {
            const unsigned char ch = feature[index];
            if (!(std::islower(ch) || std::isdigit(ch) || ch == '-'))
                return false;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool IsBcp47Tag(const std::string& locale)
{
    if (locale.size() < 2 || locale.size() > 63) return false;
    std::size_t begin = 0;
    int part = 0;
    while (begin < locale.size())
    {
        const auto end = locale.find('-', begin);
        const auto value = locale.substr(begin,
            end == std::string::npos ? locale.size() - begin : end - begin);
        if (value.empty() || value.size() > 8) return false;
        if (part == 0)
        {
            if (value.size() < 2 ||
                !std::all_of(value.begin(), value.end(),
                    [](unsigned char ch) { return std::isalpha(ch) != 0; }))
                return false;
        }
        else if (!std::all_of(value.begin(), value.end(),
            [](unsigned char ch) { return std::isalnum(ch) != 0; }))
            return false;
        ++part;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

PackageManifest LocalizedManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    if (requestedLocale.empty() || manifest.locales.empty()) return manifest;
    const LocalizedMetadata* selected = nullptr;
    std::vector<std::string> available;
    available.reserve(manifest.locales.size());
    for (const auto& [locale, metadata] : manifest.locales)
    {
        (void)metadata;
        available.push_back(locale);
    }
    const std::string selectedLocale =
        snowdesktop::localization::ResolveBestLanguage(
            available, requestedLocale);
    if (!selectedLocale.empty())
        selected = &manifest.locales.at(selectedLocale);
    if (selected)
    {
        if (!selected->title.empty()) manifest.name = selected->title;
        if (!selected->description.empty())
            manifest.description = selected->description;
        auto translate = [&](const std::string& key,
            std::string& value) {
            if (key.empty()) return;
            if (const auto found = selected->strings.find(key);
                found != selected->strings.end())
                value = found->second;
        };
        translate(manifest.previewIntroductionKey,
            manifest.previewIntroduction);
        for (const auto& [storageKey, localizationKey] :
            manifest.previewStorageKeys)
        {
            if (const auto value = manifest.previewStorage.find(storageKey);
                value != manifest.previewStorage.end())
                translate(localizationKey, value->second);
        }
        for (auto& variant : manifest.previewVariants)
        {
            translate(variant.titleKey, variant.title);
            translate(variant.descriptionKey, variant.description);
            for (const auto& [storageKey, localizationKey] :
                variant.storageKeys)
            {
                if (const auto value = variant.storage.find(storageKey);
                    value != variant.storage.end())
                    translate(localizationKey, value->second);
            }
        }
    }
    return manifest;
}

bool QueryMatches(const PackageManifest& manifest, const PackageQuery& query)
{
    const std::string needle = Lower(query.text);
    if (needle.empty()) return true;
    return Lower(manifest.name).find(needle) != std::string::npos ||
        Lower(manifest.slug).find(needle) != std::string::npos ||
        Lower(manifest.description).find(needle) != std::string::npos;
}

std::vector<std::string_view> SplitIdentifiers(std::string_view value)
{
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const auto end = value.find('.', begin);
        parts.push_back(value.substr(begin,
            end == std::string_view::npos ? value.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

int CompareNumericIdentifier(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    if (left == right) return 0;
    return left < right ? -1 : 1;
}

bool IsNumericIdentifier(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool IsSha256(std::string_view value)
{
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch)
        {
            return std::isxdigit(ch) != 0;
        });
}

bool IsNewerSemVer(std::string_view candidate, std::string_view current)
{
    const auto candidatePlus = candidate.find('+');
    const auto currentPlus = current.find('+');
    candidate = candidate.substr(0, candidatePlus);
    current = current.substr(0, currentPlus);

    const auto candidateDash = candidate.find('-');
    const auto currentDash = current.find('-');
    const auto candidateCore = SplitIdentifiers(candidate.substr(0, candidateDash));
    const auto currentCore = SplitIdentifiers(current.substr(0, currentDash));
    for (std::size_t i = 0; i < 3; ++i)
    {
        const int comparison =
            CompareNumericIdentifier(candidateCore[i], currentCore[i]);
        if (comparison != 0) return comparison > 0;
    }

    const bool candidateHasPre = candidateDash != std::string_view::npos;
    const bool currentHasPre = currentDash != std::string_view::npos;
    if (candidateHasPre != currentHasPre) return !candidateHasPre;
    if (!candidateHasPre) return false;

    const auto candidatePre =
        SplitIdentifiers(candidate.substr(candidateDash + 1));
    const auto currentPre = SplitIdentifiers(current.substr(currentDash + 1));
    const auto count = std::min(candidatePre.size(), currentPre.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (candidatePre[i] == currentPre[i]) continue;
        const bool candidateNumeric = IsNumericIdentifier(candidatePre[i]);
        const bool currentNumeric = IsNumericIdentifier(currentPre[i]);
        if (candidateNumeric != currentNumeric) return !candidateNumeric;
        if (candidateNumeric)
            return CompareNumericIdentifier(candidatePre[i], currentPre[i]) > 0;
        return candidatePre[i] > currentPre[i];
    }
    return candidatePre.size() > currentPre.size();
}

std::vector<PackageUpdate> FindAvailableUpdates(
    const std::vector<PackageDetails>& entries,
    const std::vector<PackageVersionRef>& installed)
{
    std::vector<PackageUpdate> updates;
    for (const auto& current : installed)
    {
        const PackageDetails* newest = nullptr;
        for (const auto& entry : entries)
        {
            if (entry.manifest.id != current.packageId ||
                !IsNewerSemVer(entry.manifest.version, current.version))
                continue;
            if (!newest || IsNewerSemVer(entry.manifest.version,
                    newest->manifest.version))
                newest = &entry;
        }
        if (newest) updates.push_back({ current, *newest });
    }
    return updates;
}

bool CopyArtifactWithProgress(const std::filesystem::path& source,
    const std::filesystem::path& target,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
    std::string& error)
{
    std::error_code ec;
    const auto total = std::filesystem::file_size(source, ec);
    if (ec || total > kMaxArchiveBytes)
    {
        error = "artifact is missing or exceeds 20 MiB";
        return false;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create catalog package directory: " + ec.message();
        return false;
    }
    const auto temporary =
        std::filesystem::path(target.wstring() + L".uploading");
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!input || !output)
    {
        error = "cannot open artifact copy streams";
        return false;
    }
    auto reportProgress = [&](std::uint64_t complete)
    {
        if (!progress) return true;
        try { return progress(complete, total); }
        catch (...) { return false; }
    };
    if (!reportProgress(0))
    {
        error = "publication cancelled";
        output.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t complete = 0;
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        output.write(buffer.data(), count);
        complete += static_cast<std::uint64_t>(count);
        if (!output || !reportProgress(complete))
        {
            error = output ? "publication cancelled" :
                "cannot copy catalog artifact";
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    output.flush();
    if (!input.eof() || !output)
    {
        error = "cannot finish catalog artifact copy";
        output.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    output.close();
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot atomically publish catalog artifact";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool MatchesExpectedManifest(const PackageManifest& actual,
    const PackageManifest& expected, std::string& error)
{
    if (actual.id != expected.id)
    {
        error = "package identity does not match the source metadata";
        return false;
    }
    if (actual.version != expected.version)
    {
        error = "package version does not match the source metadata";
        return false;
    }
    if (expected.schemaVersion > 0 &&
        actual.schemaVersion != expected.schemaVersion)
    {
        error = "package schema version does not match the source metadata";
        return false;
    }
    if (expected.apiVersion > 0 && actual.apiVersion != expected.apiVersion)
    {
        error = "package API version does not match the source metadata";
        return false;
    }
    const std::set<std::string> actualPermissions(
        actual.permissions.begin(), actual.permissions.end());
    const std::set<std::string> expectedPermissions(
        expected.permissions.begin(), expected.permissions.end());
    if (actualPermissions != expectedPermissions)
    {
        error = "package permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalPermissions(
        actual.optionalPermissions.begin(),
        actual.optionalPermissions.end());
    const std::set<std::string> expectedOptionalPermissions(
        expected.optionalPermissions.begin(),
        expected.optionalPermissions.end());
    if (actualOptionalPermissions != expectedOptionalPermissions)
    {
        error = "package optional permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualDomains(
        actual.networkDomains.begin(), actual.networkDomains.end());
    const std::set<std::string> expectedDomains(
        expected.networkDomains.begin(), expected.networkDomains.end());
    if (actualDomains != expectedDomains)
    {
        error = "package network domains do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualRequiredFeatures(
        actual.requiredFeatures.begin(), actual.requiredFeatures.end());
    const std::set<std::string> expectedRequiredFeatures(
        expected.requiredFeatures.begin(), expected.requiredFeatures.end());
    if (actualRequiredFeatures != expectedRequiredFeatures)
    {
        error = "package required features do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalFeatures(
        actual.optionalFeatures.begin(), actual.optionalFeatures.end());
    const std::set<std::string> expectedOptionalFeatures(
        expected.optionalFeatures.begin(), expected.optionalFeatures.end());
    if (actualOptionalFeatures != expectedOptionalFeatures)
    {
        error = "package optional features do not match the source metadata";
        return false;
    }
    if (!expected.resources.empty() && actual.resources != expected.resources)
    {
        error = "package resources do not match the source metadata";
        return false;
    }
    return true;
}
}

PackageManifest LocalizePackageManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    return LocalizedManifest(std::move(manifest), requestedLocale);
}

std::vector<std::string> DeclaredPermissions(
    const PackageManifest& manifest)
{
    std::vector<std::string> result = manifest.permissions;
    for (const auto& permission : manifest.optionalPermissions)
        if (std::find(result.begin(), result.end(), permission) ==
            result.end())
            result.push_back(permission);
    return result;
}

std::optional<PackageDetails> IWidgetPackageSource::GetVersionDetails(
    const std::string& externalItemId, const std::string& version,
    std::string& error)
{
    auto details = GetDetails(externalItemId, error);
    if (details && details->manifest.version == version)
        return details;
    if (details)
        error = "package source did not provide metadata for the requested version";
    return std::nullopt;
}

LegacyLooseImportResult ImportLegacyLooseWidgetPairs(
    const std::filesystem::path& sourceWidgets,
    const std::filesystem::path& destinationWidgets)
{
    LegacyLooseImportResult result;
    std::error_code ec;
    if (!std::filesystem::exists(sourceWidgets, ec))
    {
        result.ok = !ec;
        if (ec) result.error = "cannot inspect legacy component directory: " +
            ec.message();
        return result;
    }
    if (ec || !std::filesystem::is_directory(sourceWidgets, ec) ||
        HasReparsePoint(sourceWidgets))
    {
        result.error = ec
            ? "cannot inspect legacy component directory: " + ec.message()
            : "legacy component source is not a safe directory";
        return result;
    }

    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pairs;
    for (std::filesystem::directory_iterator it(sourceWidgets, ec), end;
        !ec && it != end; it.increment(ec))
    {
        const auto script = it->path();
        if (Lower(script.extension().string()) != ".lua")
            continue;
        const auto scriptStatus = it->symlink_status(ec);
        if (ec)
            break;
        if (!std::filesystem::is_regular_file(scriptStatus) ||
            HasReparsePoint(script))
        {
            result.error = "legacy component script is not a safe file: " +
                PathUtf8(script.filename());
            return result;
        }

        const auto manifest = script.parent_path() /
            (script.stem().wstring() + L".widget.json");
        const auto manifestStatus = std::filesystem::symlink_status(manifest, ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!std::filesystem::exists(manifestStatus))
            continue;
        if (!std::filesystem::is_regular_file(manifestStatus) ||
            HasReparsePoint(manifest))
        {
            result.error = "legacy component manifest is not a safe file: " +
                PathUtf8(manifest.filename());
            return result;
        }
        pairs.emplace_back(script, manifest);
    }
    if (ec)
    {
        result.error = "cannot enumerate legacy component directory: " +
            ec.message();
        return result;
    }
    if (pairs.empty())
    {
        result.ok = true;
        return result;
    }

    std::filesystem::create_directories(destinationWidgets, ec);
    if (ec)
    {
        result.error = "cannot create legacy component destination: " +
            ec.message();
        return result;
    }
    for (const auto& [script, manifest] : pairs)
    {
        std::filesystem::copy_file(script,
            destinationWidgets / script.filename(),
            std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec)
        {
            std::filesystem::copy_file(manifest,
                destinationWidgets / manifest.filename(),
                std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec)
        {
            result.error = "cannot copy legacy component pair: " +
                PathUtf8(script.filename()) + ": " + ec.message();
            return result;
        }
        ++result.copiedPairs;
    }
    result.ok = true;
    return result;
}

bool ValidationReport::Ok() const
{
    return std::none_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == ValidationSeverity::Error;
    });
}

void ValidationReport::Add(ValidationSeverity severity, std::string code,
    std::filesystem::path path, std::string message)
{
    issues.push_back({ severity, std::move(code), std::move(path),
        std::move(message) });
}

std::string ValidationReport::ToJson() const
{
    std::ostringstream out;
    out << "{\"ok\":" << (Ok() ? "true" : "false")
        << ",\"fileCount\":" << fileCount
        << ",\"totalBytes\":" << totalBytes << ",\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i)
    {
        if (i) out << ',';
        const char* severity = issues[i].severity == ValidationSeverity::Error
            ? "error" : issues[i].severity == ValidationSeverity::Warning
            ? "warning" : "info";
        out << "{\"severity\":\"" << severity << "\",\"code\":\""
            << JsonEscape(issues[i].code) << "\",\"path\":\""
            << JsonEscape(PathUtf8(issues[i].path)) << "\",\"message\":\""
            << JsonEscape(issues[i].message) << "\"}";
    }
    out << "]}";
    return out.str();
}

bool WidgetPackageValidator::IsUuid(std::string_view value)
{
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (value[i] != '-') return false;
        }
        else if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

bool WidgetPackageValidator::IsSemVer(std::string_view value)
{
    if (value.empty() || value.size() > 128) return false;
    const std::size_t plus = value.find('+');
    if (plus != std::string_view::npos &&
        value.find('+', plus + 1) != std::string_view::npos)
        return false;
    const std::size_t dash = value.find('-');
    if (dash != std::string_view::npos && plus != std::string_view::npos &&
        dash > plus)
        return false;
    const std::size_t coreEnd = std::min(
        plus == std::string_view::npos ? value.size() : plus,
        dash == std::string_view::npos ? value.size() : dash);

    auto numericIdentifier = [](std::string_view identifier) {
        if (identifier.empty() ||
            (identifier.size() > 1 && identifier.front() == '0'))
            return false;
        return std::all_of(identifier.begin(), identifier.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; });
    };
    std::size_t start = 0;
    for (int component = 0; component < 3; ++component)
    {
        const std::size_t end = component == 2
            ? coreEnd : value.find('.', start);
        if (end == std::string_view::npos || end > coreEnd ||
            !numericIdentifier(value.substr(start, end - start)))
            return false;
        start = end + 1;
    }
    if (start != coreEnd + 1) return false;

    auto validSuffix = [&](std::size_t begin, std::size_t end,
        bool enforceNumericLeadingZero) {
        if (begin >= end) return false;
        while (begin < end)
        {
            const std::size_t dot = value.find('.', begin);
            const std::size_t identifierEnd =
                dot == std::string_view::npos || dot > end ? end : dot;
            const auto identifier =
                value.substr(begin, identifierEnd - begin);
            if (identifier.empty() ||
                !std::all_of(identifier.begin(), identifier.end(),
                    [](unsigned char ch) {
                        return std::isalnum(ch) != 0 || ch == '-';
                    }))
                return false;
            const bool numeric = std::all_of(identifier.begin(),
                identifier.end(), [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                });
            if (enforceNumericLeadingZero && numeric &&
                identifier.size() > 1 && identifier.front() == '0')
                return false;
            begin = identifierEnd + 1;
        }
        return true;
    };
    if (dash != std::string_view::npos &&
        !validSuffix(dash + 1,
            plus == std::string_view::npos ? value.size() : plus, true))
        return false;
    if (plus != std::string_view::npos &&
        !validSuffix(plus + 1, value.size(), false))
        return false;
    return true;
}

bool WidgetPackageValidator::IsNewerSemVer(
    std::string_view candidate, std::string_view current)
{
    if (!IsSemVer(candidate) || !IsSemVer(current))
        return false;
    return snowdesktop::widget::IsNewerSemVer(candidate, current);
}

bool WidgetPackageValidator::IsSafeRelativePath(
    const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) return false;
    for (const auto& part : path)
    {
        if (part == L"." || part == L".." || part.empty()) return false;
    }
    return true;
}

bool WidgetPackageValidator::ReadManifest(
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report) const
{
    std::string text;
    if (!ReadFile(manifestPath, text, 1024 * 1024))
    {
        report.Add(ValidationSeverity::Error, "manifest.read", manifestPath,
            "widget.json is missing, too large, or unreadable");
        return false;
    }
    JsonValue root;
    std::string parseError;
    if (!ParseJson(text, root, &parseError) || !root.IsObject())
    {
        report.Add(ValidationSeverity::Error, "manifest.json", manifestPath,
            "invalid JSON: " + parseError);
        return false;
    }

    manifest = {};
    ReadInteger(root, "schemaVersion", manifest.schemaVersion);
    ReadString(root, "id", manifest.id);
    ReadString(root, "slug", manifest.slug);
    ReadString(root, "version", manifest.version);
    ReadInteger(root, "apiVersion", manifest.apiVersion);
    ReadInteger(root, "dataVersion", manifest.dataVersion);
    ReadString(root, "entry", manifest.entry);
    ReadString(root, "minHostVersion", manifest.minHostVersion);
    ReadString(root, "name", manifest.name);
    ReadString(root, "nameKey", manifest.nameKey);
    ReadString(root, "description", manifest.description);
    ReadString(root, "descriptionKey", manifest.descriptionKey);
    if (!ReadString(root, "author", manifest.author))
        ReadString(root, "publisher", manifest.author);
    ReadString(root, "license", manifest.license);
    ReadString(root, "preview", manifest.preview);
    if (const JsonValue* previewData = root.Find("previewData"))
    {
        if (!previewData->IsObject())
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewData", manifestPath,
                "previewData must be an object");
        }
        else
        {
            ReadString(*previewData, "introduction",
                manifest.previewIntroduction);
            ReadString(*previewData, "introductionKey",
                manifest.previewIntroductionKey);
            auto readPreviewStorage = [&](const JsonValue* storage,
                std::unordered_map<std::string, std::string>& output,
                const std::string& path) {
                if (!storage) return;
                if (!storage->IsObject())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.previewStorage", manifestPath,
                        path + " must be an object");
                    return;
                }
                std::size_t totalPreviewBytes = 0;
                for (const auto& [key, value] : storage->object)
                {
                    std::string serialized;
                    if (value.IsString()) serialized = value.string;
                    else if (value.IsBoolean())
                        serialized = value.boolean ? "true" : "false";
                    else if (value.IsNumber())
                    {
                        std::ostringstream stream;
                        stream << value.number;
                        serialized = stream.str();
                    }
                    else
                    {
                        report.Add(ValidationSeverity::Error,
                            "manifest.previewStorageValue", manifestPath,
                            path + " values must be strings, numbers, or booleans");
                        continue;
                    }
                    totalPreviewBytes += key.size() + serialized.size();
                    if (key.empty() || key.size() > 128 ||
                        serialized.size() > 16 * 1024)
                    {
                        report.Add(ValidationSeverity::Error,
                            "manifest.previewStorageSize", manifestPath,
                            path + " keys or values exceed their size limit");
                        continue;
                    }
                    output[key] = std::move(serialized);
                }
                if (output.size() > 64 ||
                    totalPreviewBytes > 64 * 1024)
                    report.Add(ValidationSeverity::Error,
                        "manifest.previewStorageQuota", manifestPath,
                        path + " exceeds the 64-entry or 64-KiB limit");
            };
            auto readPreviewStorageKeys = [&](const JsonValue* storageKeys,
                const std::unordered_map<std::string, std::string>& storage,
                std::unordered_map<std::string, std::string>& output,
                const std::string& path) {
                if (!storageKeys) return;
                if (!storageKeys->IsObject())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.previewStorageKeys", manifestPath,
                        path + " must be an object");
                    return;
                }
                for (const auto& [storageKey, value] : storageKeys->object)
                {
                    if (storageKey.empty() || storageKey.size() > 128 ||
                        !value.IsString() || value.string.empty() ||
                        value.string.size() > 256)
                    {
                        report.Add(ValidationSeverity::Error,
                            "manifest.previewStorageKey", manifestPath,
                            path + " must map valid preview storage names to non-empty localization keys");
                        continue;
                    }
                    if (!storage.contains(storageKey))
                    {
                        report.Add(ValidationSeverity::Error,
                            "manifest.previewStorageFallback", manifestPath,
                            path + " references a storage name without an English fallback value: " +
                                storageKey);
                        continue;
                    }
                    output[storageKey] = value.string;
                }
                if (output.size() > 64)
                    report.Add(ValidationSeverity::Error,
                        "manifest.previewStorageKeysQuota", manifestPath,
                        path + " supports at most 64 entries");
            };
            readPreviewStorage(previewData->Find("storage"),
                manifest.previewStorage, "previewData.storage");
            readPreviewStorageKeys(previewData->Find("storageKeys"),
                manifest.previewStorage, manifest.previewStorageKeys,
                "previewData.storageKeys");

            if (const JsonValue* variants = previewData->Find("variants"))
            {
                if (!variants->IsArray())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.previewVariants", manifestPath,
                        "previewData.variants must be an array");
                }
                else
                {
                    if (variants->array.size() > 4)
                    {
                        report.Add(ValidationSeverity::Error,
                            "manifest.previewVariantsQuota", manifestPath,
                            "previewData.variants supports at most four entries");
                    }
                    std::set<std::string> ids;
                    for (const JsonValue& value : variants->array)
                    {
                        if (!value.IsObject())
                        {
                            report.Add(ValidationSeverity::Error,
                                "manifest.previewVariant", manifestPath,
                                "each preview variant must be an object");
                            continue;
                        }
                        PreviewVariant variant;
                        ReadString(value, "id", variant.id);
                        ReadString(value, "title", variant.title);
                        ReadString(value, "titleKey", variant.titleKey);
                        ReadString(value, "description", variant.description);
                        ReadString(value, "descriptionKey",
                            variant.descriptionKey);
                        ReadSize(value, "size",
                            variant.columns, variant.rows);
                        if (variant.id.empty() || variant.id.size() > 64 ||
                            !std::all_of(variant.id.begin(), variant.id.end(),
                                [](unsigned char ch) {
                                    return std::isalnum(ch) || ch == '-' ||
                                        ch == '_';
                                }) || !ids.insert(variant.id).second)
                        {
                            report.Add(ValidationSeverity::Error,
                                "manifest.previewVariantId", manifestPath,
                                "preview variant ids must be unique and contain only letters, digits, '-' or '_'");
                        }
                        readPreviewStorage(value.Find("storage"),
                            variant.storage,
                            "previewData.variants[].storage");
                        readPreviewStorageKeys(value.Find("storageKeys"),
                            variant.storage, variant.storageKeys,
                            "previewData.variants[].storageKeys");
                        if (manifest.previewVariants.size() < 4)
                            manifest.previewVariants.push_back(
                                std::move(variant));
                    }
                }
            }
        }
    }
    ReadInteger(root, "refreshIntervalMs", manifest.refreshIntervalMs);
    ReadSize(root, "defaultSize", manifest.defaultColumns, manifest.defaultRows);
    ReadSize(root, "minSize", manifest.minColumns, manifest.minRows);
    ReadSize(root, "maxSize", manifest.maxColumns, manifest.maxRows);
    bool arraysValid = true;
    manifest.permissions = ReadStringArray(root, "permissions", arraysValid);
    manifest.optionalPermissions =
        ReadStringArray(root, "optionalPermissions", arraysValid);
    manifest.networkDomains =
        ReadStringArray(root, "networkDomains", arraysValid);
    manifest.requiredFeatures =
        ReadStringArray(root, "requiredFeatures", arraysValid);
    manifest.optionalFeatures =
        ReadStringArray(root, "optionalFeatures", arraysValid);
    if (const JsonValue* resources = root.Find("resources"))
    {
        if (!resources->IsObject())
        {
            report.Add(ValidationSeverity::Error, "manifest.resources",
                manifestPath, "resources must be an object");
        }
        else
        {
            if (resources->object.size() > kMaxPackageResources)
            {
                report.Add(ValidationSeverity::Error,
                    "manifest.resourceCount", manifestPath,
                    "resources cannot contain more than 32 entries");
            }
            for (const auto& [name, value] : resources->object)
            {
                if (!IsResourceName(name))
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceName", manifestPath,
                        "resource names must start with a lowercase letter and contain only lowercase letters, digits, '-' or '_': " +
                            name);
                    continue;
                }
                if (!value.IsObject())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resource", manifestPath,
                        "resource descriptors must be objects: " + name);
                    continue;
                }
                PackageResource resource;
                ReadString(value, "type", resource.type);
                ReadString(value, "path", resource.path);
                ReadString(value, "license", resource.license);
                if (resource.type != "image" && resource.type != "font")
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceType", manifestPath,
                        "resource type must be image or font: " + name);
                }
                const std::filesystem::path resourcePath =
                    Utf8ToWide(resource.path);
                const std::string extension = Lower(
                    resourcePath.extension().string());
                const bool extensionValid = resource.type == "image"
                    ? (extension == ".png" || extension == ".jpg" ||
                        extension == ".jpeg" || extension == ".bmp" ||
                        extension == ".gif" || extension == ".ico")
                    : (extension == ".ttf" || extension == ".otf");
                if (!IsSafeRelativePath(resourcePath) || !extensionValid)
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourcePath", manifestPath,
                        "resource path is unsafe or has an unsupported extension: " +
                            name);
                }
                if (resource.type == "font" && resource.license.empty())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceLicense", manifestPath,
                        "font resources must declare a license: " + name);
                }
                manifest.resources.emplace(name, std::move(resource));
            }
        }
    }

    if (const JsonValue* locales = root.Find("locales");
        locales && locales->IsObject())
    {
        for (const auto& [locale, values] : locales->object)
        {
            if (!IsBcp47Tag(locale))
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath, "invalid BCP-47 locale tag: " + locale);
                continue;
            }
            if (!values.IsObject())
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath, "locale values must be an object: " + locale);
                continue;
            }
            bool localeValuesValid = true;
            for (const auto& [key, value] : values.object)
                if (key.empty() || !value.IsString())
                    localeValuesValid = false;
            if (!localeValuesValid)
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath,
                    "locale entries must contain string translations: " +
                        locale);
                continue;
            }
            LocalizedMetadata localized;
            for (const auto& [key, value] : values.object)
                localized.strings.emplace(key, value.string);
            ReadString(values, "name", localized.title);
            if (localized.title.empty()) ReadString(values, "title", localized.title);
            ReadString(values, "description", localized.description);
            if (localized.title.empty() && !manifest.nameKey.empty())
                ReadString(values, manifest.nameKey.c_str(), localized.title);
            if (localized.description.empty() &&
                !manifest.descriptionKey.empty())
                ReadString(values, manifest.descriptionKey.c_str(),
                    localized.description);
            manifest.locales.emplace(locale, std::move(localized));
        }
    }
    const bool legacyContract =
        manifest.schemaVersion == kLegacyPackageSchemaVersion &&
        manifest.apiVersion == kLegacyApiVersion;
    const bool currentContract =
        manifest.schemaVersion == kPackageSchemaVersion &&
        manifest.apiVersion == kHostApiVersion;
    if (manifest.schemaVersion != kLegacyPackageSchemaVersion &&
        manifest.schemaVersion != kPackageSchemaVersion)
        report.Add(ValidationSeverity::Error, "manifest.schemaVersion",
            manifestPath, "schemaVersion must be 1 or 2");
    if (!IsUuid(manifest.id))
        report.Add(ValidationSeverity::Error, "manifest.id", manifestPath,
            "id must be an immutable UUID");
    if (manifest.slug.empty() ||
        std::any_of(manifest.slug.begin(), manifest.slug.end(),
            [](unsigned char ch) {
                return !(std::islower(ch) || std::isdigit(ch) || ch == '-');
            }))
        report.Add(ValidationSeverity::Error, "manifest.slug", manifestPath,
            "slug must contain lowercase letters, digits, and hyphens only");
    if (!IsSemVer(manifest.version))
        report.Add(ValidationSeverity::Error, "manifest.version", manifestPath,
            "version must be SemVer");
    if (manifest.apiVersion != kLegacyApiVersion &&
        manifest.apiVersion != kHostApiVersion)
        report.Add(ValidationSeverity::Error, "manifest.apiVersion",
            manifestPath, "apiVersion is not supported by this host");
    if (!legacyContract && !currentContract &&
        (manifest.schemaVersion == kLegacyPackageSchemaVersion ||
            manifest.schemaVersion == kPackageSchemaVersion) &&
        (manifest.apiVersion == kLegacyApiVersion ||
            manifest.apiVersion == kHostApiVersion))
    {
        report.Add(ValidationSeverity::Error, "manifest.contractVersion",
            manifestPath,
            "schemaVersion and apiVersion must both be 1 or both be 2");
    }
    if (manifest.dataVersion < 1)
        report.Add(ValidationSeverity::Error, "manifest.dataVersion",
            manifestPath, "dataVersion must be a positive integer");
    if (!IsSafeRelativePath(Utf8ToWide(manifest.entry)) ||
        Lower(std::filesystem::path(Utf8ToWide(manifest.entry))
            .extension().string()) != ".lua")
        report.Add(ValidationSeverity::Error, "manifest.entry", manifestPath,
            "entry must be a package-relative .lua path");
    if (manifest.name.empty())
        report.Add(ValidationSeverity::Error, "manifest.name", manifestPath,
            "an English fallback name is required");
    if (manifest.description.empty())
        report.Add(ValidationSeverity::Warning, "manifest.description",
            manifestPath, "an English fallback description is recommended");
    if (manifest.license.empty())
        report.Add(ValidationSeverity::Warning, "manifest.license",
            manifestPath, "a license identifier is recommended");
    for (auto& variant : manifest.previewVariants)
    {
        if (variant.columns == 0) variant.columns = manifest.defaultColumns;
        if (variant.rows == 0) variant.rows = manifest.defaultRows;
        if (variant.columns < manifest.minColumns ||
            variant.rows < manifest.minRows ||
            (manifest.maxColumns > 0 &&
                variant.columns > manifest.maxColumns) ||
            (manifest.maxRows > 0 && variant.rows > manifest.maxRows))
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewVariantSize", manifestPath,
                "preview variant sizes must stay within the component's minSize and maxSize");
        }
    }
    if (!arraysValid)
        report.Add(ValidationSeverity::Error, "manifest.array", manifestPath,
            "permissions, domains, and feature declarations must be string arrays");
    std::set<std::string> uniquePermissions;
    for (const auto& permission : manifest.permissions)
    {
        if (!IsKnownPermission(permission))
            report.Add(ValidationSeverity::Error, "manifest.permission",
                manifestPath, "unknown permission: " + permission);
        if (!uniquePermissions.insert(permission).second)
            report.Add(ValidationSeverity::Error, "manifest.permissionDuplicate",
                manifestPath, "duplicate permission: " + permission);
    }
    for (const auto& permission : manifest.optionalPermissions)
    {
        if (!IsKnownPermission(permission))
            report.Add(ValidationSeverity::Error,
                "manifest.optionalPermission", manifestPath,
                "unknown optional permission: " + permission);
        if (!uniquePermissions.insert(permission).second)
            report.Add(ValidationSeverity::Error,
                "manifest.permissionDuplicate", manifestPath,
                "required and optional permissions must be unique: " +
                    permission);
    }
    if (!manifest.networkDomains.empty() &&
        !uniquePermissions.contains("network.http"))
        report.Add(ValidationSeverity::Error, "manifest.networkDomains",
            manifestPath, "networkDomains requires network.http permission");
    std::set<std::string> uniqueDomains;
    for (const auto& domain : manifest.networkDomains)
    {
        if (!IsExplicitDnsName(domain))
            report.Add(ValidationSeverity::Error, "manifest.networkDomain",
                manifestPath, "network domains must be explicit DNS names");
        if (!uniqueDomains.insert(domain).second)
            report.Add(ValidationSeverity::Error,
                "manifest.networkDomainDuplicate", manifestPath,
                "duplicate network domain: " + domain);
    }
    std::set<std::string> uniqueFeatures;
    for (const auto& feature : manifest.requiredFeatures)
    {
        if (!IsFeatureId(feature))
            report.Add(ValidationSeverity::Error, "manifest.feature",
                manifestPath, "invalid required feature id: " + feature);
        if (!uniqueFeatures.insert(feature).second)
            report.Add(ValidationSeverity::Error,
                "manifest.featureDuplicate", manifestPath,
                "duplicate feature: " + feature);
    }
    for (const auto& feature : manifest.optionalFeatures)
    {
        if (!IsFeatureId(feature))
            report.Add(ValidationSeverity::Error,
                "manifest.optionalFeature", manifestPath,
                "invalid optional feature id: " + feature);
        if (!uniqueFeatures.insert(feature).second)
            report.Add(ValidationSeverity::Error,
                "manifest.featureDuplicate", manifestPath,
                "required and optional features must be unique: " + feature);
    }
    if (legacyContract && !uniqueFeatures.empty())
        report.Add(ValidationSeverity::Error, "manifest.featureVersion",
            manifestPath,
            "feature negotiation is only available to schema/API v2 packages");
    if (legacyContract && !manifest.resources.empty())
    {
        report.Add(ValidationSeverity::Error, "manifest.resourceVersion",
            manifestPath, "resource declarations require schema/API v2");
    }
    return report.Ok();
}

ValidationReport WidgetPackageValidator::ValidateDirectory(
    const std::filesystem::path& root, PackageManifest* outputManifest) const
{
    ValidationReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        report.Add(ValidationSeverity::Error, "package.directory", root,
            "package root is not a directory");
        return report;
    }
    if (HasReparsePoint(root))
    {
        report.Add(ValidationSeverity::Error, "package.reparse", root,
            "package root cannot be a reparse point");
        return report;
    }
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
    {
        report.Add(ValidationSeverity::Error, "package.canonical", root,
            "cannot canonicalize package root");
        return report;
    }
    PackageManifest manifest;
    ReadManifest(root / L"widget.json", manifest, report);

    for (std::filesystem::recursive_directory_iterator it(root,
        std::filesystem::directory_options::none, ec), end;
        !ec && it != end; it.increment(ec))
    {
        const auto& entry = *it;
        const auto path = entry.path();
        const auto relative = std::filesystem::relative(path, root, ec);
        if (ec || !IsSafeRelativePath(relative))
        {
            report.Add(ValidationSeverity::Error, "package.path", path,
                "package contains an unsafe path");
            ec.clear();
            continue;
        }
        if (entry.is_symlink(ec) || HasReparsePoint(path))
        {
            report.Add(ValidationSeverity::Error, "package.reparse", relative,
                "symbolic links, junctions, and reparse points are forbidden");
            if (entry.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec || !StartsWithPath(canonical, canonicalRoot))
        {
            report.Add(ValidationSeverity::Error, "package.escape", relative,
                "package entry escapes its root");
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        ++report.fileCount;
        const auto size = entry.file_size(ec);
        if (ec)
        {
            report.Add(ValidationSeverity::Error, "package.fileSize", relative,
                "cannot read file size");
            ec.clear();
            continue;
        }
        report.totalBytes += size;
        if (report.fileCount > kMaxPackageFiles)
            report.Add(ValidationSeverity::Error, "package.fileCount", relative,
                "package contains more than 512 files");
        if (report.totalBytes > kMaxExtractedBytes)
            report.Add(ValidationSeverity::Error, "package.totalBytes", relative,
                "package exceeds the 64 MiB extracted limit");
        if (relative == Utf8ToWide(manifest.entry) &&
            size > kMaxEntryLuaBytes)
            report.Add(ValidationSeverity::Error, "package.entrySize", relative,
                "Lua entry exceeds 1 MiB");
        if (!manifest.preview.empty() &&
            relative == Utf8ToWide(manifest.preview) &&
            size > kMaxPreviewBytes)
            report.Add(ValidationSeverity::Error, "package.previewSize", relative,
                "preview exceeds 2 MiB");
        const std::string extension = Lower(path.extension().string());
        if (extension == ".dll" || extension == ".exe" || extension == ".com" ||
            extension == ".bat" || extension == ".cmd" || extension == ".ps1" ||
            extension == ".msi" || extension == ".scr")
            report.Add(ValidationSeverity::Error, "package.executable", relative,
                "native libraries and executable files are forbidden");
    }
    if (ec)
        report.Add(ValidationSeverity::Error, "package.enumerate", root,
            "cannot enumerate package: " + ec.message());
    if (!std::filesystem::is_regular_file(
        root / Utf8ToWide(manifest.entry), ec))
        report.Add(ValidationSeverity::Error, "package.entryMissing",
            manifest.entry, "Lua entry file is missing");
    if (!manifest.preview.empty() &&
        (!IsSafeRelativePath(Utf8ToWide(manifest.preview)) ||
         !std::filesystem::is_regular_file(
            root / Utf8ToWide(manifest.preview), ec)))
        report.Add(ValidationSeverity::Error, "package.previewMissing",
            manifest.preview, "preview path is unsafe or missing");
    std::size_t fontCount = 0;
    for (const auto& [name, resource] : manifest.resources)
    {
        if (resource.type == "font") ++fontCount;
        const std::filesystem::path relative = Utf8ToWide(resource.path);
        const std::filesystem::path fullPath = root / relative;
        ec.clear();
        if (!IsSafeRelativePath(relative) ||
            !std::filesystem::is_regular_file(fullPath, ec))
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceMissing", relative,
                "declared resource is missing: " + name);
            continue;
        }
        const auto size = std::filesystem::file_size(fullPath, ec);
        if (ec || size > kMaxResourceBytes)
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceSize", relative,
                "resource is unreadable or exceeds the 8 MiB limit: " +
                    name);
            continue;
        }
        if (!ResourceContentIsValid(fullPath, resource))
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceContent", relative,
                "resource signature, dimensions, or declared type is invalid: " +
                    name);
        }
    }
    if (fontCount > 8)
    {
        report.Add(ValidationSeverity::Error, "package.resourceFontCount",
            root / L"widget.json",
            "a package cannot declare more than 8 fonts");
    }
    if (outputManifest) *outputManifest = std::move(manifest);
    return report;
}

ValidationReport WidgetPackageValidator::ValidateArchive(
    const std::filesystem::path& archive) const
{
    ValidationReport report;
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive, ec);
    if (ec || size > kMaxArchiveBytes)
    {
        report.Add(ValidationSeverity::Error, "archive.size", archive,
            "archive is missing or exceeds 20 MiB");
        return report;
    }
    if (Lower(archive.extension().string()) != ".snowwidget")
        report.Add(ValidationSeverity::Error, "archive.extension", archive,
            "package archive must use the .snowwidget extension");
    // Full entry validation happens during secure extraction. This function
    // deliberately never invokes the Windows shell ZIP handler.
    return report;
}

WidgetPackageManager::WidgetPackageManager(PackagePaths paths)
    : paths_(std::move(paths))
{
}

bool WidgetPackageManager::Initialize(std::string& error)
{
    std::error_code ec;
    for (const auto& path : { paths_.installed, paths_.development,
        paths_.staging, paths_.quarantine, paths_.migrations })
    {
        std::filesystem::create_directories(path, ec);
        if (ec)
        {
            error = "cannot create package directory: " + ec.message();
            return false;
        }
    }
    if (!LoadRegistry(error)) return false;
    if (!Refresh(error)) return false;
    MigrateBundledLegacyPackages();
    return true;
}

bool WidgetPackageManager::LoadRegistry(std::string& error)
{
    registry_.clear();
    permissionDecisions_.clear();
    developmentOverrides_.clear();
    legacyAliases_.clear();
    steamSubscriptionsByAccount_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(paths_.registry, ec)) return true;
    std::string text;
    if (!ReadFile(paths_.registry, text, 4 * 1024 * 1024))
    {
        error = "cannot read package registry";
        return false;
    }
    JsonValue root;
    std::string parseError;
    if (!ParseJson(text, root, &parseError) || !root.IsObject())
    {
        const auto quarantine = paths_.quarantine /
            (L"packages-" + Utf8ToWide(Timestamp()) + L".invalid.json");
        std::filesystem::rename(paths_.registry, quarantine, ec);
        // The registry is derived state. Quarantine it and rebuild from package
        // directories so a damaged JSON file cannot hide built-in packages.
        error.clear();
        return true;
    }
    const JsonValue* entries = root.Find("packages");
    if (entries && entries->IsArray())
    {
        for (const auto& value : entries->array)
        {
            if (!value.IsObject()) continue;
            RegistryEntry entry;
            ReadString(value, "packageId", entry.packageId);
            ReadString(value, "activeVersion", entry.activeVersion);
            ReadString(value, "providerId", entry.source.providerId);
            ReadString(value, "externalItemId", entry.source.externalItemId);
            if (const JsonValue* permissionState =
                    value.Find("permissionState");
                permissionState)
            {
                const auto parsed = permissionState->IsString()
                    ? ParsePermissionDecisionState(permissionState->string)
                    : std::nullopt;
                entry.permissionState = parsed.value_or(
                    PermissionDecisionState::Pending);
            }
            bool arraysValid = true;
            entry.grantedPermissions =
                ReadStringArray(value, "grantedPermissions", arraysValid);
            entry.grantedNetworkDomains =
                ReadStringArray(value, "grantedNetworkDomains", arraysValid);
            if (const JsonValue* enabled = value.Find("enabled");
                enabled && enabled->IsBoolean())
                entry.enabled = enabled->boolean;
            if (WidgetPackageValidator::IsUuid(entry.packageId) &&
                WidgetPackageValidator::IsSemVer(entry.activeVersion))
                registry_[entry.packageId] = std::move(entry);
        }
    }
    if (const JsonValue* decisions = root.Find("permissionDecisions");
        decisions && decisions->IsArray())
    {
        for (const auto& value : decisions->array)
        {
            if (!value.IsObject()) continue;
            PermissionDecisionRecord record;
            ReadString(value, "packageId", record.packageId);
            ReadString(value, "providerId", record.source.providerId);
            ReadString(value, "externalItemId",
                record.source.externalItemId);
            if (const JsonValue* state = value.Find("state"); state)
            {
                const auto parsed = state->IsString()
                    ? ParsePermissionDecisionState(state->string)
                    : std::nullopt;
                record.state = parsed.value_or(
                    PermissionDecisionState::Pending);
            }
            bool arraysValid = true;
            record.requestedPermissions =
                ReadStringArray(value, "requestedPermissions", arraysValid);
            record.requestedOptionalPermissions =
                ReadStringArray(value, "requestedOptionalPermissions",
                    arraysValid);
            record.requestedNetworkDomains =
                ReadStringArray(value, "requestedNetworkDomains", arraysValid);
            ReadString(value, "requestedScopeFingerprint",
                record.requestedScopeFingerprint);
            record.grantedPermissions =
                ReadStringArray(value, "grantedPermissions", arraysValid);
            record.grantedNetworkDomains =
                ReadStringArray(value, "grantedNetworkDomains", arraysValid);
            if (!WidgetPackageValidator::IsUuid(record.packageId) ||
                record.source.providerId.empty() ||
                record.source.externalItemId.empty())
                continue;
            const std::string key = record.packageId + "\n" +
                record.source.providerId + "\n" +
                record.source.externalItemId;
            permissionDecisions_[key] = std::move(record);
        }
    }
    if (const JsonValue* overrides = root.Find("developmentOverrides");
        overrides && overrides->IsArray())
    {
        for (const auto& value : overrides->array)
            if (value.IsString() &&
                WidgetPackageValidator::IsUuid(value.string))
                developmentOverrides_.insert(value.string);
    }
    if (const JsonValue* aliases = root.Find("legacyAliases");
        aliases && aliases->IsObject())
    {
        for (const auto& [name, value] : aliases->object)
            if (value.IsString() &&
                WidgetPackageValidator::IsUuid(value.string))
                legacyAliases_[name] = value.string;
    }
    if (const JsonValue* history = root.Find(
            "steamSubscriptionsByAccount");
        history && history->IsObject())
    {
        for (const auto& [accountId, value] : history->object)
        {
            if (!DecimalIdentifier(accountId) || !value.IsArray()) continue;
            auto& subscriptions = steamSubscriptionsByAccount_[accountId];
            for (const auto& item : value.array)
                if (item.IsString() && DecimalIdentifier(item.string))
                    subscriptions.insert(item.string);
        }
    }
    return true;
}

bool WidgetPackageManager::SaveRegistry(std::string& error) const
{
    std::vector<const RegistryEntry*> entries;
    entries.reserve(registry_.size());
    for (const auto& [id, entry] : registry_) entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
        [](const auto* a, const auto* b) { return a->packageId < b->packageId; });
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"packages\": [";
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (i) out << ',';
        const auto& entry = *entries[i];
        out << "\n    {\"packageId\":\"" << JsonEscape(entry.packageId)
            << "\",\"activeVersion\":\"" << JsonEscape(entry.activeVersion)
            << "\",\"providerId\":\"" << JsonEscape(entry.source.providerId)
            << "\",\"externalItemId\":\""
            << JsonEscape(entry.source.externalItemId)
            << "\",\"permissionState\":\""
            << PermissionDecisionStateName(entry.permissionState)
            << "\",\"enabled\":" << (entry.enabled ? "true" : "false")
            << ",\"grantedPermissions\":[";
        for (std::size_t permission = 0;
            permission < entry.grantedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(entry.grantedPermissions[permission]) << '"';
        }
        out << "],\"grantedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < entry.grantedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(entry.grantedNetworkDomains[domain]) << '"';
        }
        out << "]}";
    }
    if (!entries.empty()) out << '\n';
    out << "  ],\n  \"developmentOverrides\": [";
    std::vector<std::string> developmentOverrides(
        developmentOverrides_.begin(), developmentOverrides_.end());
    std::sort(developmentOverrides.begin(), developmentOverrides.end());
    for (std::size_t i = 0; i < developmentOverrides.size(); ++i)
    {
        if (i) out << ',';
        out << "\n    \"" << JsonEscape(developmentOverrides[i]) << '"';
    }
    if (!developmentOverrides.empty()) out << '\n';
    out << "  ],\n  \"permissionDecisions\": [";
    std::vector<const PermissionDecisionRecord*> decisions;
    decisions.reserve(permissionDecisions_.size());
    for (const auto& [key, decision] : permissionDecisions_)
    {
        (void)key;
        decisions.push_back(&decision);
    }
    std::sort(decisions.begin(), decisions.end(),
        [](const auto* a, const auto* b) {
            return std::tie(a->packageId, a->source.providerId,
                       a->source.externalItemId) <
                std::tie(b->packageId, b->source.providerId,
                       b->source.externalItemId);
        });
    for (std::size_t i = 0; i < decisions.size(); ++i)
    {
        if (i) out << ',';
        const auto& decision = *decisions[i];
        out << "\n    {\"packageId\":\""
            << JsonEscape(decision.packageId)
            << "\",\"providerId\":\""
            << JsonEscape(decision.source.providerId)
            << "\",\"externalItemId\":\""
            << JsonEscape(decision.source.externalItemId)
            << "\",\"state\":\""
            << PermissionDecisionStateName(decision.state)
            << "\",\"requestedScopeFingerprint\":\""
            << JsonEscape(decision.requestedScopeFingerprint)
            << "\",\"requestedPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.requestedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.requestedPermissions[permission]) << '"';
        }
        out << "],\"requestedOptionalPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.requestedOptionalPermissions.size();
            ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.requestedOptionalPermissions[permission]) << '"';
        }
        out << "],\"requestedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < decision.requestedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(
                decision.requestedNetworkDomains[domain]) << '"';
        }
        out << "],\"grantedPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.grantedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.grantedPermissions[permission]) << '"';
        }
        out << "],\"grantedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < decision.grantedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(
                decision.grantedNetworkDomains[domain]) << '"';
        }
        out << "]}";
    }
    if (!decisions.empty()) out << '\n';
    out << "  ],\n  \"legacyAliases\": {";
    std::vector<std::pair<std::string, std::string>> aliases(
        legacyAliases_.begin(), legacyAliases_.end());
    std::sort(aliases.begin(), aliases.end());
    for (std::size_t i = 0; i < aliases.size(); ++i)
    {
        if (i) out << ',';
        out << "\n    \"" << JsonEscape(aliases[i].first) << "\":\""
            << JsonEscape(aliases[i].second) << '"';
    }
    if (!aliases.empty()) out << '\n';
    out << "  },\n  \"steamSubscriptionsByAccount\": {";
    std::vector<std::string> accounts;
    accounts.reserve(steamSubscriptionsByAccount_.size());
    for (const auto& [accountId, subscriptions] :
        steamSubscriptionsByAccount_)
    {
        (void)subscriptions;
        accounts.push_back(accountId);
    }
    std::sort(accounts.begin(), accounts.end());
    for (std::size_t account = 0; account < accounts.size(); ++account)
    {
        if (account) out << ',';
        const auto& accountId = accounts[account];
        std::vector<std::string> subscriptions(
            steamSubscriptionsByAccount_.at(accountId).begin(),
            steamSubscriptionsByAccount_.at(accountId).end());
        std::sort(subscriptions.begin(), subscriptions.end());
        out << "\n    \"" << JsonEscape(accountId) << "\":[";
        for (std::size_t item = 0; item < subscriptions.size(); ++item)
        {
            if (item) out << ',';
            out << '\"' << JsonEscape(subscriptions[item]) << '\"';
        }
        out << ']';
    }
    if (!accounts.empty()) out << '\n';
    out << "  }\n}\n";
    return AtomicWrite(paths_.registry, out.str(), error);
}

bool WidgetPackageManager::Refresh(std::string& error)
{
    packages_.clear();
    invalidPackages_.clear();
    enum class ExplicitDecisionResult
    {
        NotFound,
        Applied,
        ScopeMismatch,
    };
    const auto applyExplicitDecision = [&](InstalledPackage& package) {
        const std::string key = package.manifest.id + "\n" +
            package.source.providerId + "\n" +
            package.source.externalItemId;
        const auto decision = permissionDecisions_.find(key);
        if (decision == permissionDecisions_.end())
            return ExplicitDecisionResult::NotFound;
        const std::set<std::string> requestedPermissions(
            decision->second.requestedPermissions.begin(),
            decision->second.requestedPermissions.end());
        const std::set<std::string> declaredPermissions(
            package.manifest.permissions.begin(),
            package.manifest.permissions.end());
        const std::set<std::string> requestedOptionalPermissions(
            decision->second.requestedOptionalPermissions.begin(),
            decision->second.requestedOptionalPermissions.end());
        const std::set<std::string> declaredOptionalPermissions(
            package.manifest.optionalPermissions.begin(),
            package.manifest.optionalPermissions.end());
        const std::set<std::string> requestedDomains(
            decision->second.requestedNetworkDomains.begin(),
            decision->second.requestedNetworkDomains.end());
        const std::set<std::string> declaredDomains(
            package.manifest.networkDomains.begin(),
            package.manifest.networkDomains.end());
        const std::string requestedScopeFingerprint =
            decision->second.requestedScopeFingerprint.empty()
            ? WidgetPermissionBroker::ScopeFingerprint(
                decision->second.requestedPermissions,
                decision->second.requestedOptionalPermissions,
                decision->second.requestedNetworkDomains)
            : decision->second.requestedScopeFingerprint;
        const std::string declaredScopeFingerprint =
            WidgetPermissionBroker::ScopeFingerprint(
                package.manifest.permissions,
                package.manifest.optionalPermissions,
                package.manifest.networkDomains);
        if (requestedPermissions != declaredPermissions ||
            requestedOptionalPermissions !=
                declaredOptionalPermissions ||
            requestedDomains != declaredDomains ||
            requestedScopeFingerprint != declaredScopeFingerprint)
            return ExplicitDecisionResult::ScopeMismatch;
        package.permissionState = decision->second.state;
        const auto grant = WidgetPermissionBroker::Evaluate(
            package.permissionState,
            package.manifest.permissions,
            package.manifest.optionalPermissions,
            package.manifest.networkDomains,
            decision->second.grantedPermissions,
            decision->second.grantedNetworkDomains);
        package.grantedPermissions = grant.permissions;
        package.grantedNetworkDomains = grant.networkDomains;
        return ExplicitDecisionResult::Applied;
    };
    const auto applyScopeMismatchBlock = [](
        InstalledPackage& package, ExplicitDecisionResult result) {
        if (result != ExplicitDecisionResult::ScopeMismatch) return;
        const auto declaredPermissions = DeclaredPermissions(
            package.manifest);
        const bool requiresConsent = !PermissionsRequiringConsent(
            declaredPermissions).empty();
        if (!requiresConsent)
        {
            package.permissionState = PermissionDecisionState::Granted;
            package.grantedPermissions = declaredPermissions;
            package.grantedNetworkDomains = package.manifest.networkDomains;
            return;
        }
        package.permissionState = PermissionDecisionState::Pending;
        package.grantedPermissions.clear();
        package.grantedNetworkDomains.clear();
    };
    auto scanRoot = [&](const std::filesystem::path& root, bool builtin,
        bool development) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        for (std::filesystem::directory_iterator it(root, ec), end;
            !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory(ec) || HasReparsePoint(it->path())) continue;
            if (builtin || development)
            {
                PackageManifest manifest;
                const auto report = validator_.ValidateDirectory(
                    it->path(), &manifest);
                if (!report.Ok())
                {
                    // The bundled Agent Skill intentionally shares the
                    // built-in root without being a component package.
                    if (!builtin || std::filesystem::is_regular_file(
                            it->path() / L"widget.json", ec))
                    {
                        InvalidPackage invalid;
                        invalid.manifest = std::move(manifest);
                        invalid.packageId = invalid.manifest.id;
                        invalid.root = it->path();
                        invalid.report = report;
                        invalid.builtin = builtin;
                        invalid.development = development;
                        invalid.source = {
                            builtin ? "builtin" : "local-directory",
                            PathUtf8(it->path().filename()) };
                        invalid.selected = builtin ||
                            developmentOverrides_.contains(
                                invalid.manifest.id);
                        invalidPackages_.push_back(std::move(invalid));
                    }
                    ec.clear();
                    continue;
                }
                InstalledPackage package;
                package.manifest = std::move(manifest);
                package.root = it->path();
                package.builtin = builtin;
                package.development = development;
                package.source = { builtin ? "builtin" : "local-directory",
                    PathUtf8(it->path().filename()) };
                package.permissionState =
                    PermissionDecisionState::LegacyImplicit;
                package.grantedPermissions =
                    DeclaredPermissions(package.manifest);
                package.grantedNetworkDomains =
                    package.manifest.networkDomains;
                package.enabled = true;
                applyScopeMismatchBlock(
                    package, applyExplicitDecision(package));
                package.active = builtin ||
                    developmentOverrides_.contains(package.manifest.id);
                package.selected = package.active;
                packages_.push_back(std::move(package));
                continue;
            }
            const std::string id = WideToUtf8(it->path().filename().wstring());
            if (!WidgetPackageValidator::IsUuid(id)) continue;
            for (std::filesystem::directory_iterator versionIt(it->path(), ec),
                versionEnd; !ec && versionIt != versionEnd;
                versionIt.increment(ec))
            {
                if (!versionIt->is_directory(ec)) continue;
                PackageManifest manifest;
                const auto report = validator_.ValidateDirectory(
                    versionIt->path(), &manifest);
                if (!report.Ok() || manifest.id != id)
                {
                    InvalidPackage invalid;
                    invalid.manifest = std::move(manifest);
                    invalid.packageId = id;
                    invalid.root = versionIt->path();
                    invalid.report = report;
                    const auto registryIt = registry_.find(id);
                    invalid.source = registryIt != registry_.end()
                        ? registryIt->second.source
                        : PackageSourceRef{ "local", id };
                    invalid.selected = registryIt != registry_.end() &&
                        registryIt->second.activeVersion ==
                            invalid.manifest.version;
                    if (report.Ok() && invalid.manifest.id != id)
                    {
                        invalid.report.Add(ValidationSeverity::Error,
                            "package.idDirectory", versionIt->path(),
                            "manifest id does not match the installed package directory");
                    }
                    invalidPackages_.push_back(std::move(invalid));
                    continue;
                }
                InstalledPackage package;
                package.manifest = std::move(manifest);
                package.root = versionIt->path();
                const auto registryIt = registry_.find(id);
                package.active = registryIt != registry_.end() &&
                    registryIt->second.activeVersion == package.manifest.version;
                package.selected = package.active;
                package.enabled = registryIt == registry_.end() ||
                    registryIt->second.enabled;
                if (registryIt != registry_.end())
                {
                    package.source = registryIt->second.source;
                    package.permissionState =
                        registryIt->second.permissionState;
                    const auto grant = WidgetPermissionBroker::Evaluate(
                        package.permissionState,
                        package.manifest.permissions,
                        package.manifest.optionalPermissions,
                        package.manifest.networkDomains,
                        registryIt->second.grantedPermissions,
                        registryIt->second.grantedNetworkDomains);
                    package.grantedPermissions = grant.permissions;
                    package.grantedNetworkDomains = grant.networkDomains;
                    applyScopeMismatchBlock(
                        package, applyExplicitDecision(package));
                }
                else
                {
                    package.source = { "local", id };
                    package.permissionState =
                        PermissionDecisionState::LegacyImplicit;
                    package.grantedPermissions =
                        DeclaredPermissions(package.manifest);
                    package.grantedNetworkDomains =
                        package.manifest.networkDomains;
                }
                packages_.push_back(std::move(package));
            }
        }
        if (ec && error.empty())
            error = "cannot enumerate package directory: " + ec.message();
    };
    scanRoot(paths_.builtin, true, false);
    scanRoot(paths_.installed, false, false);
    scanRoot(paths_.development, false, true);

    // Development packages are inert candidates until the user explicitly
    // activates an override for the shared package UUID.
    std::set<std::string> developmentIds;
    for (const auto& package : packages_)
        if (package.development && package.active)
            developmentIds.insert(package.manifest.id);
    for (auto& package : packages_)
        if (!package.development && developmentIds.contains(package.manifest.id))
            package.active = false;

    // Built-ins are the fallback when no installed active version exists.
    std::set<std::string> activeIds;
    for (const auto& package : packages_)
        if (!package.builtin && package.active) activeIds.insert(package.manifest.id);
    for (auto& package : packages_)
        if (package.builtin && activeIds.contains(package.manifest.id))
            package.active = false;
    return error.empty();
}

std::vector<InstalledPackage> WidgetPackageManager::ListPackages() const
{
    return packages_;
}

std::vector<InvalidPackage> WidgetPackageManager::ListInvalidPackages() const
{
    return invalidPackages_;
}

bool WidgetPackageManager::ContainsPackage(const std::string& packageId) const
{
    return std::any_of(packages_.begin(), packages_.end(),
        [&](const auto& package)
        {
            return package.manifest.id == packageId;
        });
}

std::unordered_map<std::string, std::vector<std::string>>
WidgetPackageManager::SteamSubscriptionHistory() const
{
    std::unordered_map<std::string, std::vector<std::string>> result;
    for (const auto& [accountId, subscriptions] :
        steamSubscriptionsByAccount_)
    {
        auto& values = result[accountId];
        values.assign(subscriptions.begin(), subscriptions.end());
        std::sort(values.begin(), values.end());
    }
    return result;
}

bool WidgetPackageManager::UpdateSteamSubscriptionHistory(
    const std::string& accountId,
    const std::vector<std::string>& publishedFileIds, std::string& error)
{
    if (!DecimalIdentifier(accountId) ||
        !std::all_of(publishedFileIds.begin(), publishedFileIds.end(),
            DecimalIdentifier))
    {
        error = "invalid Steam subscription history identity";
        return false;
    }

    const auto previous = steamSubscriptionsByAccount_.find(accountId);
    const bool existed = previous != steamSubscriptionsByAccount_.end();
    std::unordered_set<std::string> previousSubscriptions;
    if (existed) previousSubscriptions = previous->second;
    steamSubscriptionsByAccount_[accountId] =
        std::unordered_set<std::string>(publishedFileIds.begin(),
            publishedFileIds.end());
    if (SaveRegistry(error)) return true;
    if (existed)
        steamSubscriptionsByAccount_[accountId] =
            std::move(previousSubscriptions);
    else
        steamSubscriptionsByAccount_.erase(accountId);
    return false;
}

std::optional<InstalledPackage> WidgetPackageManager::Resolve(
    const std::string& packageId) const
{
    if (const auto registry = registry_.find(packageId);
        registry != registry_.end() && !registry->second.enabled &&
        !developmentOverrides_.contains(packageId))
        return std::nullopt;
    const InstalledPackage* builtinFallback = nullptr;
    for (const auto& package : packages_)
    {
        if (package.manifest.id != packageId) continue;
        if (package.active && package.enabled) return package;
        if (package.builtin && package.enabled && !builtinFallback)
            builtinFallback = &package;
    }
    if (builtinFallback) return *builtinFallback;
    return std::nullopt;
}

std::optional<std::filesystem::path> WidgetPackageManager::ResolveEntry(
    const std::string& packageId) const
{
    const auto package = Resolve(packageId);
    if (!package) return std::nullopt;
    const auto entry = package->root / Utf8ToWide(package->manifest.entry);
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(
        package->root, ec);
    const auto canonicalEntry = std::filesystem::weakly_canonical(entry, ec);
    if (ec || !StartsWithPath(canonicalEntry, canonicalRoot) ||
        HasReparsePoint(canonicalEntry))
        return std::nullopt;
    return canonicalEntry;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveEntryPath(
    const std::filesystem::path& entryPath) const
{
    std::error_code ec;
    const auto canonicalInput =
        std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) return std::nullopt;
    for (const auto& package : packages_)
    {
        if (!package.active || !package.enabled)
            continue;
        const auto resolved = ResolveEntry(package.manifest.id);
        if (!resolved)
            continue;
        if (std::filesystem::equivalent(*resolved, canonicalInput, ec))
            return package;
        ec.clear();
    }
    return std::nullopt;
}

ValidationReport WidgetPackageManager::ValidateDirectory(
    const std::filesystem::path& root, PackageManifest* manifest) const
{
    return validator_.ValidateDirectory(root, manifest);
}

ValidationReport WidgetPackageManager::ValidateArchive(
    const std::filesystem::path& archive, PackageManifest* manifest) const
{
    ValidationReport report = validator_.ValidateArchive(archive);
    if (!report.Ok()) return report;
    std::error_code ec;
    const auto temporaryRoot = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        report.Add(ValidationSeverity::Error, "archive.staging", archive,
            "cannot resolve the validation staging directory");
        return report;
    }
    const auto staging = temporaryRoot /
        Utf8ToWide("SnowDesktopWidgetValidation-" + GenerateUuid());
    std::string error;
    report = {};
    if (!ExtractArchive(archive, staging, report, error))
    {
        report.Add(ValidationSeverity::Error, "archive.content", archive,
            std::move(error));
    }
    else
    {
        report = validator_.ValidateDirectory(staging, manifest);
    }
    std::filesystem::remove_all(staging, ec);
    return report;
}

std::filesystem::path WidgetPackageManager::CreateStagingPath(
    const char* purpose) const
{
    return paths_.staging /
        (Utf8ToWide(std::string(purpose) + "-" + Timestamp() + "-" +
            GenerateUuid()));
}

bool WidgetPackageManager::CopyPackageTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error) const
{
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec)
    {
        error = "cannot create package destination: " + ec.message();
        return false;
    }
    std::filesystem::copy(source, destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "cannot copy package: " + ec.message();
        return false;
    }
    return true;
}

bool WidgetPackageManager::CommitStagedPackage(
    const std::filesystem::path& stagedRoot, const PackageManifest& manifest,
    const PackageSourceRef& sourceRef, bool allowSourceChange,
    bool allowPermissionExpansion, InstalledPackage& installed,
    std::string& error)
{
    const auto existing = registry_.find(manifest.id);
    if (existing != registry_.end() &&
        !(existing->second.source == sourceRef) &&
        !allowSourceChange)
    {
        error = "package is already owned by provider " +
            existing->second.source.providerId +
            "; source changes require explicit confirmation";
        return false;
    }
    if (existing != registry_.end() &&
        !IsNewerSemVer(manifest.version, existing->second.activeVersion) &&
        manifest.version != existing->second.activeVersion)
    {
        error = "package updates cannot downgrade the active version; use rollback";
        return false;
    }
    if (existing != registry_.end() && !allowPermissionExpansion)
    {
        std::vector<std::string> currentRequired;
        std::vector<std::string> currentDeclared;
        std::vector<std::string> currentDomains;
        if (const auto current = Resolve(manifest.id))
        {
            currentRequired = current->manifest.permissions;
            currentDeclared = DeclaredPermissions(current->manifest);
            currentDomains = current->manifest.networkDomains;
        }
        const std::set<std::string> previouslyRequired(
            currentRequired.begin(), currentRequired.end());
        const std::set<std::string> previouslyRequested(
            currentDeclared.begin(), currentDeclared.end());
        for (const auto& permission : manifest.permissions)
        {
            if (!previouslyRequired.contains(permission))
            {
                error = "update requests a new required permission: " +
                    permission;
                return false;
            }
        }
        for (const auto& permission : manifest.optionalPermissions)
        {
            if (!previouslyRequested.contains(permission))
            {
                error = "update requests a new optional permission: " +
                    permission;
                return false;
            }
        }
        const std::set<std::string> previouslyRequestedDomains(
            currentDomains.begin(), currentDomains.end());
        for (const auto& domain : manifest.networkDomains)
        {
            if (!previouslyRequestedDomains.contains(domain))
            {
                error = "update requests a new network domain: " + domain;
                return false;
            }
        }
    }
    const auto target = paths_.installed / Utf8ToWide(manifest.id) /
        Utf8ToWide(manifest.version);
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        error = "this package version is already installed";
        return false;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create package version directory: " + ec.message();
        return false;
    }
    std::filesystem::rename(stagedRoot, target, ec);
    if (ec)
    {
        error = "cannot atomically activate staged package: " + ec.message();
        return false;
    }
    RegistryEntry entry;
    entry.packageId = manifest.id;
    entry.activeVersion = manifest.version;
    entry.source = sourceRef;
    const auto declaredPermissions = DeclaredPermissions(manifest);
    const bool requiresConsent = !PermissionsRequiringConsent(
        declaredPermissions).empty();
    entry.permissionState = requiresConsent
        ? PermissionDecisionState::Pending
        : PermissionDecisionState::Granted;
    if (!requiresConsent)
    {
        entry.grantedPermissions = declaredPermissions;
        entry.grantedNetworkDomains = manifest.networkDomains;
    }
    entry.enabled = true;
    const auto oldEntry = registry_.find(manifest.id);
    const std::optional<RegistryEntry> previous = oldEntry == registry_.end()
        ? std::nullopt : std::optional<RegistryEntry>(oldEntry->second);
    registry_[manifest.id] = entry;
    if (!SaveRegistry(error))
    {
        if (previous) registry_[manifest.id] = *previous;
        else registry_.erase(manifest.id);
        const auto quarantine = paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + manifest.version + "-" + Timestamp()));
        std::filesystem::rename(target, quarantine, ec);
        return false;
    }
    std::string refreshError;
    Refresh(refreshError);
    const auto resolved = Resolve(manifest.id);
    if (!resolved)
    {
        error = "installed package could not be resolved";
        return false;
    }
    installed = *resolved;
    return true;
}

bool WidgetPackageManager::InstallDirectory(
    const std::filesystem::path& source, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
        return false;
    const auto staging = CreateStagingPath("install");
    if (!CopyPackageTree(source, staging, error)) return false;
    PackageManifest copiedManifest;
    report = validator_.ValidateDirectory(staging, &copiedManifest);
    if (!report.Ok() || copiedManifest.id != manifest.id ||
        copiedManifest.version != manifest.version)
    {
        error = "staged package failed validation";
        const auto quarantine = paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, copiedManifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(copiedManifest.id + "-" + copiedManifest.version +
                "-" + Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::ExtractArchive(
    const std::filesystem::path& archive,
    const std::filesystem::path& destination, ValidationReport& report,
    std::string& error) const
{
    std::string data;
    if (!ReadFile(archive, data, kMaxArchiveBytes))
    {
        error = "cannot read package archive";
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t position = 0;
    std::set<std::string> names;
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    while (position + 4 <= data.size() && Read32(bytes + position) == 0x04034b50)
    {
        if (position + 30 > data.size())
        {
            error = "truncated ZIP local header";
            return false;
        }
        const std::uint16_t flags = Read16(bytes + position + 6);
        const std::uint16_t method = Read16(bytes + position + 8);
        const std::uint32_t expectedCrc = Read32(bytes + position + 14);
        const std::uint32_t compressedSize = Read32(bytes + position + 18);
        const std::uint32_t uncompressedSize = Read32(bytes + position + 22);
        const std::uint16_t nameSize = Read16(bytes + position + 26);
        const std::uint16_t extraSize = Read16(bytes + position + 28);
        if ((flags & 0x0001) || (flags & 0x0008) || method != 0)
        {
            error = "v1 accepts unencrypted ZIP entries using the store method";
            return false;
        }
        const std::size_t payload = position + 30 + nameSize + extraSize;
        if (payload > data.size() ||
            compressedSize > data.size() - payload ||
            compressedSize != uncompressedSize)
        {
            error = "invalid ZIP entry bounds";
            return false;
        }
        const std::string name(data.data() + position + 30, nameSize);
        if (name.find('\0') != std::string::npos || name.ends_with('/'))
        {
            position = payload + compressedSize;
            continue;
        }
        const std::filesystem::path relative(Utf8ToWide(name));
        const std::string canonicalName =
            Lower(PathUtf8(relative.lexically_normal()));
        if (!WidgetPackageValidator::IsSafeRelativePath(relative) ||
            canonicalName.empty() || !names.insert(canonicalName).second)
        {
            error = "archive contains an unsafe or duplicate path: " + name;
            return false;
        }
        ++report.fileCount;
        report.totalBytes += uncompressedSize;
        if (report.fileCount > kMaxPackageFiles ||
            report.totalBytes > kMaxExtractedBytes)
        {
            error = "archive exceeds extraction quotas";
            return false;
        }
        const auto target = destination / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
        {
            error = "cannot create extracted directory: " + ec.message();
            return false;
        }
        const auto* payloadBytes = bytes + payload;
        if (Crc32(payloadBytes, uncompressedSize) != expectedCrc)
        {
            error = "archive entry CRC mismatch: " + name;
            return false;
        }
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(payloadBytes),
            static_cast<std::streamsize>(uncompressedSize));
        if (!file)
        {
            error = "cannot extract archive entry: " + name;
            return false;
        }
        position = payload + compressedSize;
    }
    if (report.fileCount == 0)
    {
        error = "archive contains no files";
        return false;
    }
    return true;
}

bool WidgetPackageManager::InstallArchive(
    const std::filesystem::path& archive, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    report = validator_.ValidateArchive(archive);
    if (!report.Ok())
    {
        error = "archive validation failed";
        return false;
    }
    const auto staging = CreateStagingPath("archive");
    report = {};
    if (!ExtractArchive(archive, staging, report, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    PackageManifest manifest;
    report = validator_.ValidateDirectory(staging, &manifest);
    if (!report.Ok())
    {
        error = "extracted package validation failed";
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, manifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + manifest.version + "-" +
                Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::InstallFromSource(IWidgetPackageSource& source,
    const std::string& externalItemId, const std::string& version,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion)
{
    const auto status = source.Status();
    if (!status.available)
    {
        error = status.message.empty() ? "package source is unavailable" :
            status.message;
        return false;
    }
    const auto details =
        source.GetVersionDetails(externalItemId, version, error);
    if (!details)
        return false;
    const auto staging = CreateStagingPath("provider");
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec)
    {
        error = "cannot create provider staging directory: " + ec.message();
        return false;
    }
    const auto destination = staging / L"artifact.snowwidget";
    const auto artifact =
        source.Materialize(externalItemId, version, destination, error);
    auto quarantineOrRemove = [&]()
    {
        bool empty = std::filesystem::is_empty(staging, ec);
        if (!ec && empty)
        {
            std::filesystem::remove_all(staging, ec);
            return;
        }
        ec.clear();
        std::filesystem::rename(staging, paths_.quarantine /
            Utf8ToWide("provider-" + Timestamp() + "-" + GenerateUuid()), ec);
        if (ec) std::filesystem::remove_all(staging, ec);
    };
    if (!artifact)
    {
        quarantineOrRemove();
        return false;
    }
    if (artifact->packageId != details->manifest.id ||
        !WidgetPackageValidator::IsUuid(artifact->packageId))
    {
        error = "provider returned an invalid package identity: artifact=" +
            (artifact->packageId.empty() ? "<empty>" : artifact->packageId) +
            ", expected=" +
            (details->manifest.id.empty() ? "<empty>" : details->manifest.id);
        quarantineOrRemove();
        return false;
    }
    if (artifact->version != version)
    {
        error = "provider returned a different package version";
        quarantineOrRemove();
        return false;
    }

    const PackageSourceRef sourceRef = details->source;
    if (sourceRef.providerId != source.ProviderId() ||
        sourceRef.externalItemId.empty())
    {
        error = "provider returned an invalid source identity";
        quarantineOrRemove();
        return false;
    }
    const bool ok = std::filesystem::is_directory(artifact->localPath, ec)
        ? InstallDirectory(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest)
        : InstallArchive(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest);
    if (!ok)
    {
        quarantineOrRemove();
        return false;
    }
    std::filesystem::remove_all(staging, ec);
    return true;
}

bool WidgetPackageManager::ExportArchive(const std::string& packageId,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    const auto package = Resolve(packageId);
    if (!package)
    {
        error = "package is not installed or is disabled";
        return false;
    }
    report = validator_.ValidateDirectory(package->root);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    if (!WriteStoreZip(package->root, output, error)) return false;
    std::error_code ec;
    if (std::filesystem::file_size(output, ec) > kMaxArchiveBytes)
    {
        error = "exported package exceeds 20 MiB";
        return false;
    }
    artifact.localPath = output;
    artifact.packageId = package->manifest.id;
    artifact.version = package->manifest.version;
    artifact.sha256 = Sha256File(output);
    return !artifact.sha256.empty();
}

bool WidgetPackageManager::ExportDirectory(
    const std::filesystem::path& source,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    if (!WriteStoreZip(source, output, error)) return false;
    std::error_code ec;
    if (std::filesystem::file_size(output, ec) > kMaxArchiveBytes)
    {
        error = "exported package exceeds 20 MiB";
        return false;
    }
    artifact.localPath = output;
    artifact.packageId = manifest.id;
    artifact.version = manifest.version;
    artifact.sha256 = Sha256File(output);
    return !artifact.sha256.empty();
}

bool WidgetPackageManager::SetEnabled(const std::string& packageId,
    bool enabled, std::string& error)
{
    auto found = registry_.find(packageId);
    if (found == registry_.end())
    {
        const auto package = Resolve(packageId);
        if (!package || package->builtin)
        {
            error = "built-in packages cannot be disabled in the v1 registry";
            return false;
        }
        return false;
    }
    found->second.enabled = enabled;
    if (!SaveRegistry(error)) return false;
    return Refresh(error);
}

bool WidgetPackageManager::SetPermissionDecision(
    const std::string& packageId, PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error)
{
    std::optional<InstalledPackage> package = Resolve(packageId);
    if (!package)
    {
        if (const auto registered = registry_.find(packageId);
            registered != registry_.end())
        {
            const auto managed = std::find_if(packages_.begin(),
                packages_.end(), [&](const auto& candidate) {
                    return candidate.manifest.id == packageId &&
                        candidate.manifest.version ==
                            registered->second.activeVersion &&
                        candidate.source == registered->second.source;
                });
            if (managed != packages_.end()) package = *managed;
        }
    }
    if (!package)
    {
        const auto selected = std::find_if(packages_.begin(),
            packages_.end(), [&](const auto& candidate) {
                return candidate.manifest.id == packageId &&
                    candidate.active;
            });
        if (selected != packages_.end()) package = *selected;
    }
    if (!package)
    {
        error = "package is unavailable";
        return false;
    }
    if (state == PermissionDecisionState::LegacyImplicit)
    {
        error = "legacy implicit permission state cannot be user-authored";
        return false;
    }
    if ((state == PermissionDecisionState::Pending ||
            state == PermissionDecisionState::Denied) &&
        (!grantedPermissions.empty() || !grantedNetworkDomains.empty()))
    {
        error = "pending or denied decisions cannot retain granted scopes";
        return false;
    }
    const std::vector<std::string> allDeclaredPermissions =
        DeclaredPermissions(package->manifest);
    const std::set<std::string> declaredPermissions(
        allDeclaredPermissions.begin(),
        allDeclaredPermissions.end());
    const std::set<std::string> declaredDomains(
        package->manifest.networkDomains.begin(),
        package->manifest.networkDomains.end());
    if (!std::all_of(grantedPermissions.begin(), grantedPermissions.end(),
            [&](const auto& permission) {
                return declaredPermissions.contains(permission);
            }) ||
        !std::all_of(grantedNetworkDomains.begin(),
            grantedNetworkDomains.end(), [&](const auto& domain) {
                return declaredDomains.contains(domain);
            }))
    {
        error = "granted scopes must be declared by the active package";
        return false;
    }

    PermissionDecisionRecord record;
    record.packageId = packageId;
    record.source = package->source;
    record.state = state;
    record.requestedPermissions = package->manifest.permissions;
    record.requestedOptionalPermissions =
        package->manifest.optionalPermissions;
    record.requestedNetworkDomains = package->manifest.networkDomains;
    record.requestedScopeFingerprint =
        WidgetPermissionBroker::ScopeFingerprint(
            record.requestedPermissions,
            record.requestedOptionalPermissions,
            record.requestedNetworkDomains);
    record.grantedPermissions = grantedPermissions;
    record.grantedNetworkDomains = grantedNetworkDomains;
    const std::string key = packageId + "\n" +
        record.source.providerId + "\n" + record.source.externalItemId;
    const auto previousDecision = permissionDecisions_.find(key);
    const std::optional<PermissionDecisionRecord> oldDecision =
        previousDecision == permissionDecisions_.end()
        ? std::nullopt
        : std::optional<PermissionDecisionRecord>(previousDecision->second);
    const auto registry = registry_.find(packageId);
    const bool updateRegistry = registry != registry_.end() &&
        registry->second.source == package->source;
    const std::optional<RegistryEntry> oldRegistry = updateRegistry
        ? std::optional<RegistryEntry>(registry->second)
        : std::nullopt;

    permissionDecisions_[key] = record;
    if (updateRegistry)
    {
        registry->second.permissionState = state;
        registry->second.grantedPermissions = grantedPermissions;
        registry->second.grantedNetworkDomains = grantedNetworkDomains;
    }
    if (SaveRegistry(error) && Refresh(error)) return true;

    const std::string originalError = error;
    if (oldDecision)
        permissionDecisions_[key] = *oldDecision;
    else
        permissionDecisions_.erase(key);
    if (oldRegistry) registry_[packageId] = *oldRegistry;
    std::string rollbackError;
    SaveRegistry(rollbackError);
    Refresh(rollbackError);
    error = originalError;
    return false;
}

bool WidgetPackageManager::CreateDevelopmentProject(
    const std::string& packageId, std::filesystem::path& projectRoot,
    std::string& error)
{
    projectRoot.clear();
    const auto existingDevelopment = std::find_if(
        packages_.begin(), packages_.end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId;
        });
    if (existingDevelopment != packages_.end())
    {
        error = "a development project already exists for this component";
        return false;
    }

    const auto installed = std::find_if(
        packages_.begin(), packages_.end(), [&](const auto& package)
        {
            return package.manifest.id == packageId && package.active &&
                !package.builtin && !package.development;
        });
    if (installed == packages_.end())
    {
        error = "the installed component is unavailable";
        return false;
    }

    PackageManifest sourceManifest;
    const ValidationReport sourceReport = validator_.ValidateDirectory(
        installed->root, &sourceManifest);
    if (!sourceReport.Ok() || sourceManifest.id != packageId)
    {
        error = "the installed component failed validation";
        return false;
    }

    const std::filesystem::path staging =
        CreateStagingPath("development");
    std::error_code ec;
    auto discardStaging = [&]
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
    };
    if (!CopyPackageTree(installed->root, staging, error))
    {
        discardStaging();
        return false;
    }

    PackageManifest copiedManifest;
    const ValidationReport copiedReport = validator_.ValidateDirectory(
        staging, &copiedManifest);
    if (!copiedReport.Ok() || copiedManifest.id != sourceManifest.id ||
        copiedManifest.version != sourceManifest.version)
    {
        discardStaging();
        error = "the development copy failed validation";
        return false;
    }

    const std::wstring baseName = Utf8ToWide(
        copiedManifest.slug.empty() ? copiedManifest.id : copiedManifest.slug);
    std::filesystem::path destination = paths_.development / baseName;
    for (unsigned suffix = 2; std::filesystem::exists(destination, ec);
        ++suffix)
    {
        if (suffix > 10000)
        {
            destination = paths_.development /
                (baseName + L"-" + Utf8ToWide(GenerateUuid()));
            break;
        }
        destination = paths_.development /
            (baseName + L"-" + std::to_wstring(suffix));
    }
    if (ec)
    {
        discardStaging();
        error = "cannot inspect the development workspace: " + ec.message();
        return false;
    }

    std::filesystem::rename(staging, destination, ec);
    if (ec)
    {
        discardStaging();
        error = "cannot create the development project: " + ec.message();
        return false;
    }
    if (!Refresh(error))
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(destination, cleanupError);
        std::string refreshError;
        Refresh(refreshError);
        return false;
    }

    const auto created = std::find_if(
        packages_.begin(), packages_.end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId &&
                package.root == destination;
        });
    if (created == packages_.end())
    {
        std::filesystem::remove_all(destination, ec);
        std::string refreshError;
        Refresh(refreshError);
        error = "the development project could not be loaded";
        return false;
    }
    projectRoot = destination;
    return true;
}

bool WidgetPackageManager::SetDevelopmentOverride(
    const std::string& packageId, bool active, std::string& error)
{
    const bool developmentExists = std::any_of(
        packages_.begin(), packages_.end(), [&](const auto& package)
        {
            return package.development &&
                package.manifest.id == packageId;
        });
    if (active && !developmentExists)
    {
        error = "development package is unavailable";
        return false;
    }

    const bool previous = developmentOverrides_.contains(packageId);
    if (active)
        developmentOverrides_.insert(packageId);
    else
        developmentOverrides_.erase(packageId);
    if (!SaveRegistry(error))
    {
        if (previous)
            developmentOverrides_.insert(packageId);
        else
            developmentOverrides_.erase(packageId);
        return false;
    }
    if (Refresh(error)) return true;

    const std::string refreshError = error;
    if (previous)
        developmentOverrides_.insert(packageId);
    else
        developmentOverrides_.erase(packageId);
    std::string rollbackError;
    SaveRegistry(rollbackError);
    Refresh(rollbackError);
    error = refreshError;
    return false;
}

bool WidgetPackageManager::Rollback(const std::string& packageId,
    const std::string& version, std::string& error)
{
    auto registry = registry_.find(packageId);
    if (registry == registry_.end())
    {
        error = "package has no installed version registry";
        return false;
    }
    const auto target = paths_.installed / Utf8ToWide(packageId) /
        Utf8ToWide(version);
    PackageManifest manifest;
    const auto report = validator_.ValidateDirectory(target, &manifest);
    if (!report.Ok() || manifest.id != packageId || manifest.version != version)
    {
        error = "requested rollback version is invalid or missing";
        return false;
    }
    const auto previous = registry->second.activeVersion;
    registry->second.activeVersion = version;
    if (!SaveRegistry(error))
    {
        registry->second.activeVersion = previous;
        return false;
    }
    return Refresh(error);
}

bool WidgetPackageManager::Uninstall(const std::string& packageId,
    std::string& error)
{
    if (!registry_.contains(packageId))
    {
        error = "package is not installed";
        return false;
    }
    const auto source = paths_.installed / Utf8ToWide(packageId);
    const auto quarantine = paths_.quarantine /
        (Utf8ToWide(packageId) + L"-uninstalled-" + Utf8ToWide(Timestamp()));
    std::error_code ec;
    std::filesystem::rename(source, quarantine, ec);
    if (ec)
    {
        error = "cannot move package to quarantine: " + ec.message();
        return false;
    }
    const RegistryEntry previous = registry_[packageId];
    registry_.erase(packageId);
    if (!SaveRegistry(error))
    {
        registry_[packageId] = previous;
        std::filesystem::rename(quarantine, source, ec);
        return false;
    }
    return Refresh(error);
}

std::vector<LegacyPackage> WidgetPackageManager::ScanLegacyPackages() const
{
    std::vector<LegacyPackage> result;
    for (const auto& root : { paths_.builtin, paths_.installed.parent_path() })
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(root, ec), end;
            !ec && it != end; it.increment(ec))
        {
            std::error_code entryError;
            if (!it->is_regular_file(entryError) ||
                Lower(it->path().extension().string()) != ".lua")
                continue;
            LegacyPackage package;
            package.scriptPath = it->path();
            package.legacyName = it->path().filename().wstring();
            package.manifestPath = it->path().parent_path() /
                (it->path().stem().wstring() + L".widget.json");
            if (!HasReparsePoint(package.scriptPath) &&
                std::filesystem::is_regular_file(
                    package.manifestPath, entryError) &&
                !HasReparsePoint(package.manifestPath))
                result.push_back(std::move(package));
        }
    }
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) {
            return _wcsicmp(a.legacyName.c_str(), b.legacyName.c_str()) < 0;
        });
    return result;
}

std::optional<std::string> WidgetPackageManager::BundledReplacementId(
    const LegacyPackage& legacy) const
{
    const BundledLegacyComponent* descriptor = nullptr;
    const auto filename = legacy.scriptPath.filename().wstring();
    for (const auto& candidate : kBundledLegacyComponents)
    {
        if (_wcsicmp(candidate.filename, filename.c_str()) == 0)
        {
            descriptor = &candidate;
            break;
        }
    }
    if (!descriptor) return std::nullopt;

    // Portable and older packaged builds stored shipped and user-authored
    // loose components in the same directory. The shipped localization key is
    // the provenance marker that prevents a custom file with a colliding name
    // from being silently replaced.
    std::string text;
    JsonValue root;
    std::string nameKey;
    if (!ReadFile(legacy.manifestPath, text, 1024 * 1024) ||
        !ParseJson(text, root) || !root.IsObject() ||
        !ReadString(root, "nameKey", nameKey) ||
        nameKey != descriptor->nameKey)
        return std::nullopt;

    const auto package = std::find_if(packages_.begin(), packages_.end(),
        [&](const InstalledPackage& installed) {
            return installed.builtin &&
                installed.manifest.id == descriptor->packageId;
        });
    if (package == packages_.end()) return std::nullopt;
    return std::string(descriptor->packageId);
}

std::vector<LegacyPackage> WidgetPackageManager::FindLegacyPackages() const
{
    std::vector<LegacyPackage> result;
    for (const auto& legacy : ScanLegacyPackages())
        if (!BundledReplacementId(legacy))
            result.push_back(legacy);
    return result;
}

LegacyMigrationResult WidgetPackageManager::ReplaceBundledLegacy(
    const LegacyPackage& legacy, const std::string& packageId)
{
    LegacyMigrationResult result;
    result.legacyName = legacy.legacyName;
    result.packageId = packageId;
    const auto retirementDirectory =
        CreateStagingPath("retired-builtin");

    std::error_code ec;
    std::filesystem::create_directories(retirementDirectory, ec);
    if (ec)
    {
        result.error = "cannot create retirement staging: " + ec.message();
        return result;
    }
    auto stage = [&](const std::filesystem::path& source) {
        ec.clear();
        const bool copied = std::filesystem::copy_file(source,
            retirementDirectory / source.filename(),
            std::filesystem::copy_options::overwrite_existing, ec);
        return copied && !ec;
    };
    if (!stage(legacy.scriptPath) || !stage(legacy.manifestPath))
    {
        result.error = "cannot stage retired built-in component";
        if (ec) result.error += ": " + ec.message();
        ec.clear();
        std::filesystem::remove_all(retirementDirectory, ec);
        return result;
    }

    const std::string alias = WideToUtf8(legacy.legacyName);
    const auto previous = legacyAliases_.find(alias);
    const std::optional<std::string> previousValue =
        previous == legacyAliases_.end()
        ? std::nullopt : std::optional<std::string>(previous->second);
    legacyAliases_[alias] = packageId;
    if (!SaveRegistry(result.error))
    {
        if (previousValue) legacyAliases_[alias] = *previousValue;
        else legacyAliases_.erase(alias);
        ec.clear();
        std::filesystem::remove_all(retirementDirectory, ec);
        return result;
    }

    std::string cleanupError;
    auto removeOldFile = [&](const std::filesystem::path& path) {
        ec.clear();
        const bool removed = std::filesystem::remove(path, ec);
        if (removed && !ec) return true;
        cleanupError = "cannot remove replaced built-in file " +
            PathUtf8(path) + ": " + (ec ? ec.message() : "file not removed");
        return false;
    };
    const bool removedScript = removeOldFile(legacy.scriptPath);
    const bool removedManifest = removeOldFile(legacy.manifestPath);
    if (!removedScript || !removedManifest)
    {
        // Restore the pair from the timestamped backup so the cleanup remains
        // retryable at the next launch instead of leaving a half-migrated pair.
        if (removedScript)
        {
            ec.clear();
            std::filesystem::copy_file(
                retirementDirectory / legacy.scriptPath.filename(),
                legacy.scriptPath,
                std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (removedManifest)
        {
            ec.clear();
            std::filesystem::copy_file(
                retirementDirectory / legacy.manifestPath.filename(),
                legacy.manifestPath,
                std::filesystem::copy_options::overwrite_existing, ec);
        }
        result.error = std::move(cleanupError);
        ec.clear();
        std::filesystem::remove_all(retirementDirectory, ec);
        return result;
    }

    ec.clear();
    std::filesystem::remove_all(retirementDirectory, ec);
    result.ok = true;
    result.report.Add(ValidationSeverity::Info, "migration.builtin",
        legacy.legacyName,
        "shipped legacy component was replaced by its folder package; "
        "instance storage is retained");
    return result;
}

std::filesystem::path WidgetPackageManager::PendingLegacyStoragePath() const
{
    return paths_.registry.parent_path() / L"legacy-storage.pending.json";
}

bool WidgetPackageManager::PrepareBundledLegacyStorage(std::string& error)
{
    const auto pending = PendingLegacyStoragePath();
    std::error_code ec;
    if (std::filesystem::is_regular_file(pending, ec))
        return true;

    const auto storage =
        paths_.registry.parent_path().parent_path() /
            L"SnowDesktop.storage.json";
    ec.clear();
    if (!std::filesystem::is_regular_file(storage, ec))
        return true;

    std::string text;
    if (!ReadFile(storage, text, 16 * 1024 * 1024))
    {
        error = "cannot read legacy component storage before replacement";
        return false;
    }
    return AtomicWrite(pending, text, error);
}

void WidgetPackageManager::MigrateBundledLegacyPackages()
{
    automaticLegacyMigrationResults_.clear();
    std::vector<std::pair<LegacyPackage, std::string>> bundled;
    for (const auto& legacy : ScanLegacyPackages())
    {
        const auto packageId = BundledReplacementId(legacy);
        if (packageId) bundled.emplace_back(legacy, *packageId);
    }
    if (bundled.empty()) return;

    std::string storageError;
    if (!PrepareBundledLegacyStorage(storageError))
    {
        for (const auto& [legacy, packageId] : bundled)
        {
            LegacyMigrationResult result;
            result.legacyName = legacy.legacyName;
            result.packageId = packageId;
            result.error = storageError;
            automaticLegacyMigrationResults_.push_back(std::move(result));
        }
        OutputDebugStringA(
            ("SnowDesktop: automatic built-in widget migration stopped: " +
                storageError + "\n").c_str());
        return;
    }

    for (const auto& [legacy, packageId] : bundled)
    {
        auto result = ReplaceBundledLegacy(legacy, packageId);
        std::ostringstream diagnostic;
        diagnostic << "SnowDesktop: automatic built-in widget migration "
            << (result.ok ? "succeeded for " : "failed for ")
            << WideToUtf8(legacy.legacyName);
        if (!result.ok) diagnostic << ": " << result.error;
        diagnostic << '\n';
        OutputDebugStringA(diagnostic.str().c_str());
        automaticLegacyMigrationResults_.push_back(std::move(result));
    }
}

std::optional<std::string> WidgetPackageManager::ResolveLegacyPackageId(
    const std::wstring& legacyName) const
{
    const std::wstring filename =
        std::filesystem::path(legacyName).filename().wstring();
    const std::string name = WideToUtf8(filename);
    for (const auto& [alias, packageId] : legacyAliases_)
        if (_stricmp(alias.c_str(), name.c_str()) == 0 &&
            Resolve(packageId).has_value())
            return packageId;
    for (const auto& package : packages_)
    {
        if (!package.active || !package.enabled ||
            package.source.providerId != "legacy-import")
            continue;
        if (_stricmp(package.source.externalItemId.c_str(), name.c_str()) == 0)
            return package.manifest.id;
    }

    // An MSIX upgrade replaces the read-only install directory atomically, so
    // shipped loose files from the previous version are already gone before
    // this process starts. Resolve their old layout names directly to the new
    // immutable built-in IDs. A real user-authored loose pair with the same
    // filename always wins and must still go through explicit migration.
    for (const auto& legacy : ScanLegacyPackages())
    {
        if (_wcsicmp(legacy.legacyName.c_str(),
                filename.c_str()) == 0 &&
            !BundledReplacementId(legacy))
        {
            return std::nullopt;
        }
    }
    for (const auto& descriptor : kBundledLegacyComponents)
    {
        if (_wcsicmp(descriptor.filename, filename.c_str()) != 0)
            continue;
        const auto replacement = Resolve(descriptor.packageId);
        if (replacement && replacement->builtin)
            return std::string(descriptor.packageId);
    }
    return std::nullopt;
}

LegacyMigrationResult WidgetPackageManager::MigrateLegacy(
    const LegacyPackage& legacy, const std::optional<std::string>& preferredId)
{
    if (const auto packageId = BundledReplacementId(legacy))
        return ReplaceBundledLegacy(legacy, *packageId);

    LegacyMigrationResult result;
    result.legacyName = legacy.legacyName;
    std::string text;
    JsonValue root;
    PackageManifest manifest;
    if (!ReadFile(legacy.manifestPath, text, 1024 * 1024) ||
        !ParseJson(text, root) || !root.IsObject())
    {
        result.error = "legacy manifest is missing or invalid";
        return result;
    }
    // Loose legacy scripts remain migration input until their entry point is
    // rewritten to the API v2 descriptor contract.
    manifest.schemaVersion = kLegacyPackageSchemaVersion;
    manifest.id = preferredId && WidgetPackageValidator::IsUuid(*preferredId)
        ? *preferredId : GenerateUuid();
    manifest.slug = Lower(WideToUtf8(legacy.scriptPath.stem().wstring()));
    std::replace_if(manifest.slug.begin(), manifest.slug.end(),
        [](unsigned char ch) {
            return !(std::islower(ch) || std::isdigit(ch) || ch == '-');
        }, '-');
    manifest.version = "1.0.0";
    ReadString(root, "version", manifest.version);
    if (!WidgetPackageValidator::IsSemVer(manifest.version))
        manifest.version = "1.0.0";
    manifest.apiVersion = kLegacyApiVersion;
    manifest.dataVersion = 1;
    manifest.entry = "main.lua";
    ReadString(root, "minHostVersion", manifest.minHostVersion);
    ReadString(root, "name", manifest.name);
    ReadString(root, "description", manifest.description);
    ReadString(root, "publisher", manifest.author);
    ReadString(root, "license", manifest.license);
    ReadSize(root, "defaultSize", manifest.defaultColumns, manifest.defaultRows);
    ReadSize(root, "minSize", manifest.minColumns, manifest.minRows);
    ReadSize(root, "maxSize", manifest.maxColumns, manifest.maxRows);
    bool arraysValid = true;
    manifest.permissions = ReadStringArray(root, "permissions", arraysValid);
    manifest.optionalPermissions =
        ReadStringArray(root, "optionalPermissions", arraysValid);
    manifest.networkDomains =
        ReadStringArray(root, "networkDomains", arraysValid);
    if (manifest.name.empty()) manifest.name = manifest.slug;

    const auto staging = CreateStagingPath("migration");
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    std::string error;
    if (ec || !std::filesystem::copy_file(legacy.scriptPath,
        staging / L"main.lua", std::filesystem::copy_options::overwrite_existing,
        ec) || !AtomicWrite(staging / L"widget.json",
        ManifestJson(manifest), error))
    {
        result.error = error.empty() ? "cannot stage legacy package" : error;
        return result;
    }
    result.report = validator_.ValidateDirectory(staging, &manifest);
    if (!result.report.Ok())
    {
        result.error = "migrated package failed validation";
        const auto quarantine = paths_.quarantine /
            (L"migration-" + legacy.scriptPath.stem().wstring() + L"-" +
                Utf8ToWide(Timestamp()));
        std::filesystem::rename(staging, quarantine, ec);
        return result;
    }

    result.backupDirectory = paths_.migrations /
        (Utf8ToWide(Timestamp()) + L"-" + legacy.scriptPath.stem().wstring());
    std::filesystem::create_directories(result.backupDirectory, ec);
    std::filesystem::copy_file(legacy.scriptPath,
        result.backupDirectory / legacy.scriptPath.filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::copy_file(legacy.manifestPath,
        result.backupDirectory / legacy.manifestPath.filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        result.error = "cannot create migration backup: " + ec.message();
        return result;
    }
    InstalledPackage installed;
    if (!CommitStagedPackage(staging, manifest,
        { "legacy-import", WideToUtf8(legacy.legacyName) }, false, false,
        installed, result.error))
        return result;
    result.ok = true;
    result.packageId = manifest.id;
    // Old files are never executable again. The timestamped backup remains the
    // recovery/import source if the author needs to repair the converted package.
    std::filesystem::remove(legacy.scriptPath, ec);
    ec.clear();
    std::filesystem::remove(legacy.manifestPath, ec);
    return result;
}

std::string WidgetPackageManager::Sha256File(
    const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
        nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &resultSize, 0) < 0)
    {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
        nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::array<char, 64 * 1024> buffer{};
    while (file)
    {
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount();
        if (count > 0 && BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(buffer.data()),
            static_cast<ULONG>(count), 0) < 0)
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }
    const bool ok = BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (UCHAR byte : digest) out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}

std::string WidgetPackageManager::GenerateUuid()
{
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) return {};
    char buffer[37]{};
    std::snprintf(buffer, sizeof(buffer),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
        guid.Data4[6], guid.Data4[7]);
    return Lower(buffer);
}

std::string BuiltinPackageSource::ProviderId() const { return "builtin"; }
ProviderCapabilities BuiltinPackageSource::Capabilities() const
{
    return { true, true, true, false, false, false };
}

ProviderStatus BuiltinPackageSource::Status()
{
    return { true, "built-in packages are available" };
}

std::vector<PackageDetails> BuiltinPackageSource::Query(
    const PackageQuery& query, std::string& error)
{
    error.clear();
    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    for (const auto& package : manager_.ListPackages())
    {
        if (!package.builtin) continue;
        auto manifest = LocalizedManifest(package.manifest, query.locale);
        if (!QueryMatches(manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back({ std::move(manifest), package.source,
            { package.manifest.version }, false });
    }
    return result;
}

std::optional<PackageDetails> BuiltinPackageSource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    error.clear();
    for (const auto& package : manager_.ListPackages())
        if (package.builtin &&
            (package.manifest.id == externalItemId ||
             package.source.externalItemId == externalItemId))
            return PackageDetails{ package.manifest, package.source,
                { package.manifest.version }, false };
    error = "built-in package not found";
    return std::nullopt;
}

std::optional<PackageArtifact> BuiltinPackageSource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    for (const auto& package : manager_.ListPackages())
    {
        if (!package.builtin || package.manifest.version != version ||
            (package.manifest.id != externalItemId &&
             package.source.externalItemId != externalItemId))
            continue;
        std::error_code ec;
        std::filesystem::copy(package.root, destination,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); return std::nullopt; }
        return PackageArtifact{ destination, package.manifest.id,
            package.manifest.version, {} };
    }
    error = "built-in package version not found";
    return std::nullopt;
}

std::vector<PackageUpdate> BuiltinPackageSource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}

LocalDirectorySource::LocalDirectorySource(std::filesystem::path root)
    : root_(std::move(root)) {}
std::string LocalDirectorySource::ProviderId() const { return "local-directory"; }
ProviderCapabilities LocalDirectorySource::Capabilities() const
{
    return { true, true, true, true, false, false };
}

ProviderStatus LocalDirectorySource::Status()
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root_, ec))
        return { false, ec ? ec.message() : "directory is unavailable" };
    return { true, "directory is available" };
}

std::vector<PackageDetails> LocalDirectorySource::Query(
    const PackageQuery& query, std::string& error)
{
    error.clear();
    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root_, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec)) continue;
        PackageManifest manifest;
        if (!validator_.ValidateDirectory(it->path(), &manifest).Ok())
            continue;
        manifest = LocalizedManifest(std::move(manifest), query.locale);
        if (!QueryMatches(manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back({ manifest,
            { ProviderId(), PathUtf8(it->path().filename()) },
            { manifest.version }, false });
    }
    if (ec) error = ec.message();
    return result;
}

std::optional<PackageDetails> LocalDirectorySource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    PackageManifest manifest;
    const auto directory = root_ / Utf8ToWide(externalItemId);
    if (!validator_.ValidateDirectory(directory, &manifest).Ok())
    {
        error = "local package not found or invalid";
        return std::nullopt;
    }
    return PackageDetails{ manifest, { ProviderId(), externalItemId },
        { manifest.version }, false };
}

std::optional<PackageArtifact> LocalDirectorySource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    const auto details = GetDetails(externalItemId, error);
    if (!details || details->manifest.version != version) return std::nullopt;
    std::error_code ec;
    std::filesystem::copy(root_ / Utf8ToWide(externalItemId), destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { error = ec.message(); return std::nullopt; }
    return PackageArtifact{ destination, details->manifest.id, version, {} };
}

std::vector<PackageUpdate> LocalDirectorySource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}

StaticCatalogSource::StaticCatalogSource(std::filesystem::path catalogPath)
    : catalogPath_(std::move(catalogPath)) {}
std::string StaticCatalogSource::ProviderId() const { return "static-catalog"; }
ProviderCapabilities StaticCatalogSource::Capabilities() const
{
    return { true, true, true, true, false, false };
}

ProviderStatus StaticCatalogSource::Status()
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    std::string error;
    if (!ReadCatalog(entries, artifacts, error)) return { false, error };
    return { true, "catalog is available" };
}

bool StaticCatalogSource::ReadCatalog(std::vector<PackageDetails>& entries,
    std::unordered_map<std::string, PackageArtifact>& artifacts,
    std::string& error) const
{
    std::string text;
    if (!ReadFile(catalogPath_, text, 8 * 1024 * 1024))
    {
        error = "catalog is offline or unreadable";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root, &error) || !root.IsObject())
    {
        error = "invalid static catalog: " + error;
        return false;
    }
    const JsonValue* packages = root.Find("packages");
    if (!packages || !packages->IsArray()) return true;
    for (const auto& value : packages->array)
    {
        if (!value.IsObject()) continue;
        PackageDetails detail;
        ReadString(value, "id", detail.manifest.id);
        ReadString(value, "slug", detail.manifest.slug);
        ReadString(value, "version", detail.manifest.version);
        ReadString(value, "name", detail.manifest.name);
        ReadString(value, "description", detail.manifest.description);
        ReadString(value, "author", detail.manifest.author);
        ReadString(value, "license", detail.manifest.license);
        ReadString(value, "minHostVersion", detail.manifest.minHostVersion);
        ReadInteger(value, "schemaVersion", detail.manifest.schemaVersion);
        ReadInteger(value, "apiVersion", detail.manifest.apiVersion);
        ReadInteger(value, "dataVersion", detail.manifest.dataVersion);
        bool arraysValid = true;
        detail.manifest.permissions =
            ReadStringArray(value, "permissions", arraysValid);
        detail.manifest.optionalPermissions =
            ReadStringArray(value, "optionalPermissions", arraysValid);
        detail.manifest.networkDomains =
            ReadStringArray(value, "networkDomains", arraysValid);
        detail.manifest.requiredFeatures =
            ReadStringArray(value, "requiredFeatures", arraysValid);
        detail.manifest.optionalFeatures =
            ReadStringArray(value, "optionalFeatures", arraysValid);
        if (const JsonValue* locales = value.Find("locales");
            locales && locales->IsObject())
        {
            for (const auto& [locale, metadata] : locales->object)
            {
                if (!IsBcp47Tag(locale) || !metadata.IsObject()) continue;
                LocalizedMetadata localized;
                ReadString(metadata, "title", localized.title);
                ReadString(metadata, "description", localized.description);
                detail.manifest.locales.emplace(
                    locale, std::move(localized));
            }
        }
        ReadString(value, "externalItemId", detail.source.externalItemId);
        detail.source.providerId = ProviderId();
        std::string artifact;
        std::string sha256;
        ReadString(value, "artifact", artifact);
        ReadString(value, "sha256", sha256);
        if (detail.source.externalItemId.empty())
            detail.source.externalItemId = detail.manifest.id;
        if (arraysValid &&
            WidgetPackageValidator::IsUuid(detail.manifest.id) &&
            WidgetPackageValidator::IsSemVer(detail.manifest.version) &&
            WidgetPackageValidator::IsSafeRelativePath(Utf8ToWide(artifact)) &&
            IsSha256(sha256))
        {
            detail.versions.push_back(detail.manifest.version);
            artifacts[detail.source.externalItemId + "\n" +
                detail.manifest.version] = {
                    catalogPath_.parent_path() / Utf8ToWide(artifact),
                    detail.manifest.id, detail.manifest.version, Lower(sha256) };
            entries.push_back(std::move(detail));
        }
    }
    return true;
}

std::vector<PackageDetails> StaticCatalogSource::Query(
    const PackageQuery& query, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return {};

    std::vector<PackageDetails> grouped;
    for (const auto& entry : entries)
    {
        auto existing = std::find_if(grouped.begin(), grouped.end(),
            [&](const PackageDetails& value)
            {
                return value.source.externalItemId ==
                    entry.source.externalItemId;
            });
        if (existing == grouped.end())
        {
            grouped.push_back(entry);
            continue;
        }
        if (std::find(existing->versions.begin(), existing->versions.end(),
                entry.manifest.version) == existing->versions.end())
            existing->versions.push_back(entry.manifest.version);
        if (IsNewerSemVer(entry.manifest.version, existing->manifest.version))
            existing->manifest = entry.manifest;
    }

    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    for (auto entry : grouped)
    {
        entry.manifest =
            LocalizedManifest(std::move(entry.manifest), query.locale);
        if (!QueryMatches(entry.manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back(entry);
    }
    return result;
}

std::optional<PackageDetails> StaticCatalogSource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    std::optional<PackageDetails> result;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId != externalItemId) continue;
        if (!result)
        {
            result = entry;
            continue;
        }
        if (std::find(result->versions.begin(), result->versions.end(),
                entry.manifest.version) == result->versions.end())
            result->versions.push_back(entry.manifest.version);
        if (IsNewerSemVer(entry.manifest.version, result->manifest.version))
            result->manifest = entry.manifest;
    }
    if (result) return result;
    error = "catalog item not found";
    return std::nullopt;
}

std::optional<PackageDetails> StaticCatalogSource::GetVersionDetails(
    const std::string& externalItemId, const std::string& version,
    std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId == externalItemId &&
            entry.manifest.version == version)
            return entry;
    }
    error = "catalog item version not found";
    return std::nullopt;
}

std::optional<PackageArtifact> StaticCatalogSource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId != externalItemId ||
            entry.manifest.version != version) continue;
        const auto artifact = artifacts.find(externalItemId + "\n" + version);
        if (artifact == artifacts.end()) break;
        std::error_code ec;
        std::filesystem::copy_file(artifact->second.localPath, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); return std::nullopt; }
        const auto sha256 = WidgetPackageManager::Sha256File(destination);
        if (sha256 != artifact->second.sha256)
        {
            std::filesystem::remove(destination, ec);
            error = "catalog artifact SHA-256 mismatch";
            return std::nullopt;
        }
        return PackageArtifact{ destination, entry.manifest.id, version, sha256 };
    }
    error = "catalog artifact not found";
    return std::nullopt;
}

std::vector<PackageUpdate> StaticCatalogSource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}

LocalCatalogPublisher::LocalCatalogPublisher(
    std::filesystem::path catalogDirectory)
    : catalogDirectory_(std::move(catalogDirectory)) {}
std::string LocalCatalogPublisher::ProviderId() const { return "local-catalog"; }
ProviderCapabilities LocalCatalogPublisher::Capabilities() const
{
    return { false, false, false, false, true, true };
}

PublishResult LocalCatalogPublisher::Publish(const PublishRequest& request)
{
    PublishResult result;
    if (!WidgetPackageValidator::IsUuid(request.artifact.packageId) ||
        !WidgetPackageValidator::IsSemVer(request.artifact.version))
    {
        result.error = "artifact identity is invalid";
        return result;
    }
    WidgetPackageManager validationManager(PackagePaths{});
    PackageManifest publishedManifest;
    const auto validation = validationManager.ValidateArchive(
        request.artifact.localPath, &publishedManifest);
    if (!validation.Ok() ||
        publishedManifest.id != request.artifact.packageId ||
        publishedManifest.version != request.artifact.version)
    {
        result.error = "publish artifact failed package validation";
        return result;
    }
    std::error_code ec;
    const std::string externalId = request.externalItemId.value_or(
        request.artifact.packageId);
    const auto relative = std::filesystem::path(L"packages") /
        Utf8ToWide(request.artifact.packageId + "-" +
            request.artifact.version + ".snowwidget");
    const auto target = catalogDirectory_ / relative;
    const std::string actualSha256 =
        WidgetPackageManager::Sha256File(request.artifact.localPath);
    if (actualSha256.empty())
    {
        result.error = "cannot hash the publish artifact";
        return result;
    }
    if (!request.artifact.sha256.empty() &&
        actualSha256 != Lower(request.artifact.sha256))
    {
        result.error = "artifact SHA-256 does not match the publish request";
        return result;
    }
    if (!CopyArtifactWithProgress(request.artifact.localPath, target,
        request.progress, result.error))
    {
        return result;
    }
    struct CatalogRecord
    {
        std::string id;
        std::string slug;
        std::string version;
        std::string externalId;
        std::string name;
        std::string description;
        std::string author;
        std::string license;
        std::string minHostVersion;
        int schemaVersion = 0;
        int apiVersion = 0;
        int dataVersion = 0;
        std::vector<std::string> permissions;
        std::vector<std::string> optionalPermissions;
        std::vector<std::string> networkDomains;
        std::vector<std::string> requiredFeatures;
        std::vector<std::string> optionalFeatures;
        std::unordered_map<std::string, LocalizedMetadata> locales;
        std::vector<std::string> tags;
        std::string changeNotes;
        std::string artifact;
        std::string sha256;
    };
    std::vector<CatalogRecord> records;
    const auto catalogPath = catalogDirectory_ / L"catalog.json";
    std::string existingText;
    JsonValue existingRoot;
    if (ReadFile(catalogPath, existingText, 8 * 1024 * 1024) &&
        ParseJson(existingText, existingRoot) && existingRoot.IsObject())
    {
        if (const JsonValue* packages = existingRoot.Find("packages");
            packages && packages->IsArray())
        {
            for (const auto& value : packages->array)
            {
                if (!value.IsObject()) continue;
                CatalogRecord record;
                ReadString(value, "id", record.id);
                ReadString(value, "slug", record.slug);
                ReadString(value, "version", record.version);
                ReadString(value, "externalItemId", record.externalId);
                ReadString(value, "name", record.name);
                ReadString(value, "description", record.description);
                ReadString(value, "author", record.author);
                ReadString(value, "license", record.license);
                ReadString(value, "minHostVersion", record.minHostVersion);
                ReadInteger(value, "schemaVersion", record.schemaVersion);
                ReadInteger(value, "apiVersion", record.apiVersion);
                ReadInteger(value, "dataVersion", record.dataVersion);
                bool arraysValid = true;
                record.permissions =
                    ReadStringArray(value, "permissions", arraysValid);
                record.optionalPermissions =
                    ReadStringArray(value, "optionalPermissions",
                        arraysValid);
                record.networkDomains =
                    ReadStringArray(value, "networkDomains", arraysValid);
                record.requiredFeatures =
                    ReadStringArray(value, "requiredFeatures", arraysValid);
                record.optionalFeatures =
                    ReadStringArray(value, "optionalFeatures", arraysValid);
                if (const JsonValue* locales = value.Find("locales");
                    locales && locales->IsObject())
                {
                    for (const auto& [locale, metadata] : locales->object)
                    {
                        if (!IsBcp47Tag(locale) || !metadata.IsObject())
                            continue;
                        LocalizedMetadata localized;
                        ReadString(metadata, "title", localized.title);
                        ReadString(metadata, "description",
                            localized.description);
                        record.locales.emplace(
                            locale, std::move(localized));
                    }
                }
                record.tags = ReadStringArray(value, "tags", arraysValid);
                ReadString(value, "changeNotes", record.changeNotes);
                ReadString(value, "artifact", record.artifact);
                ReadString(value, "sha256", record.sha256);
                if (WidgetPackageValidator::IsUuid(record.id) &&
                    WidgetPackageValidator::IsSemVer(record.version) &&
                    !record.externalId.empty())
                    records.push_back(std::move(record));
            }
        }
    }
    std::erase_if(records, [&](const CatalogRecord& record) {
        return record.externalId == externalId &&
            record.version == request.artifact.version;
    });
    records.push_back({ request.artifact.packageId,
        publishedManifest.slug, request.artifact.version, externalId,
        request.title, request.description, publishedManifest.author,
        publishedManifest.license, publishedManifest.minHostVersion,
        publishedManifest.schemaVersion,
        publishedManifest.apiVersion, publishedManifest.dataVersion,
        publishedManifest.permissions,
        publishedManifest.optionalPermissions,
        publishedManifest.networkDomains,
        publishedManifest.requiredFeatures,
        publishedManifest.optionalFeatures,
        publishedManifest.locales, request.tags, request.changeNotes,
        PathUtf8(relative), actualSha256 });
    std::sort(records.begin(), records.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.externalId, left.version) <
                std::tie(right.externalId, right.version);
        });

    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"packages\": [";
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        if (index) out << ',';
        const auto& record = records[index];
        out << "\n    {\"id\":\"" << JsonEscape(record.id)
            << "\",\"slug\":\"" << JsonEscape(record.slug)
            << "\",\"version\":\"" << JsonEscape(record.version)
            << "\",\"externalItemId\":\"" << JsonEscape(record.externalId)
            << "\",\"name\":\"" << JsonEscape(record.name)
            << "\",\"description\":\"" << JsonEscape(record.description)
            << "\",\"author\":\"" << JsonEscape(record.author)
            << "\",\"license\":\"" << JsonEscape(record.license)
            << "\",\"minHostVersion\":\""
            << JsonEscape(record.minHostVersion)
            << "\",\"schemaVersion\":" << record.schemaVersion
            << ",\"apiVersion\":" << record.apiVersion
            << ",\"dataVersion\":" << record.dataVersion
            << ",\"permissions\":[";
        for (std::size_t permission = 0;
            permission < record.permissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(record.permissions[permission]) << '"';
        }
        out << "],\"optionalPermissions\":[";
        for (std::size_t permission = 0;
            permission < record.optionalPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                record.optionalPermissions[permission]) << '"';
        }
        out << "],\"networkDomains\":[";
        for (std::size_t domain = 0;
            domain < record.networkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(record.networkDomains[domain]) << '"';
        }
        out << "],\"requiredFeatures\":[";
        for (std::size_t feature = 0;
            feature < record.requiredFeatures.size(); ++feature)
        {
            if (feature) out << ',';
            out << '"' << JsonEscape(record.requiredFeatures[feature]) << '"';
        }
        out << "],\"optionalFeatures\":[";
        for (std::size_t feature = 0;
            feature < record.optionalFeatures.size(); ++feature)
        {
            if (feature) out << ',';
            out << '"' << JsonEscape(record.optionalFeatures[feature]) << '"';
        }
        out << "],\"locales\":{";
        std::vector<std::string> localeNames;
        localeNames.reserve(record.locales.size());
        for (const auto& [locale, metadata] : record.locales)
            localeNames.push_back(locale);
        std::sort(localeNames.begin(), localeNames.end());
        for (std::size_t locale = 0; locale < localeNames.size(); ++locale)
        {
            if (locale) out << ',';
            const auto& name = localeNames[locale];
            const auto& metadata = record.locales.at(name);
            out << '"' << JsonEscape(name) << "\":{\"title\":\""
                << JsonEscape(metadata.title)
                << "\",\"description\":\""
                << JsonEscape(metadata.description) << "\"}";
        }
        out << "},\"tags\":[";
        for (std::size_t tag = 0; tag < record.tags.size(); ++tag)
        {
            if (tag) out << ',';
            out << '"' << JsonEscape(record.tags[tag]) << '"';
        }
        out << "],\"changeNotes\":\"" << JsonEscape(record.changeNotes)
            << "\",\"artifact\":\"" << JsonEscape(record.artifact)
            << "\",\"sha256\":\"" << JsonEscape(record.sha256) << "\"}";
    }
    if (!records.empty()) out << '\n';
    out << "  ]\n}\n";
    std::string error;
    if (!AtomicWrite(catalogPath, out.str(), error))
    {
        result.error = error;
        return result;
    }
    result.ok = true;
    result.externalItemId = externalId;
    return result;
}
}
