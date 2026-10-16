#include "icon_library.h"
#include "icon_image_codec.h"
#include "data/atomic_file.h"
#include "common/json_value.h"
#include "ui/preview/preview_png_writer.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
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
bool IconLibrary::Load()
{
    if (loaded_) return valid_;
    loaded_ = true;
    if (!SafeDirectory(directory_) || !SafeDirectory(directory_ / L"originals") || !SafeDirectory(directory_ / L"previews")) return valid_ = false;
    const auto index = directory_ / L"library.json";
    std::error_code ec;
    if (!std::filesystem::exists(index, ec)) return valid_ = !ec;
    std::string text; JsonValue root;
    if (!SafeFile(index) || !atomic_file::ReadAll(index, text) || !ParseJson(text, root)) return valid_ = false;
    const auto* version = root.Find("version"); const auto* entries = root.Find("entries");
    if (!version || !version->IsNumber() || version->number != 1 || !entries || !entries->IsArray()) return valid_ = false;
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
        entries_.push_back(std::move(entry));
    }
    return true;
}
bool IconLibrary::Save()
{
    if (!valid_) return false;
    std::string text = "{\"version\":1,\"entries\":[";
    bool first = true;
    for (const auto& entry : entries_)
    {
        if (!first) text += ',';
        first = false;
        text += "{\"id\":" + Quote(entry.id) + ",\"name\":" + Quote(Utf8(entry.name)) +
            ",\"width\":" + std::to_string(entry.width) + ",\"height\":" + std::to_string(entry.height) +
            ",\"source\":" + Quote(entry.source) + ",\"packageId\":" + Quote(entry.packageId) +
            ",\"packageVersion\":" + Quote(entry.packageVersion) + ",\"packageEntryId\":" + Quote(entry.packageEntryId) + '}';
    }
    text += "]}\n";
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
        if (SafeFile(preview)) entry.previewPath = L"file:///" + preview.generic_wstring();
    }
    result.entries = entries_; result.revision = revision_; result.succeeded = true;
    return result;
}
IconLibrarySnapshot IconLibrary::Refresh()
{
    loaded_ = false; valid_ = true; entries_.clear(); ++revision_;
    return Read();
}
std::string IconLibrary::Import(const std::filesystem::path& path, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > icon_image::kMaxFileBytes) { error = "iconLibrary.tooLarge"; return {}; }
    std::ifstream input(path, std::ios::binary); std::string bytes(static_cast<size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(size)) || input.peek() != EOF) { error = "iconLibrary.invalid"; return {}; }
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
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == entries_.end())
    {
        IconLibraryEntry entry; entry.id = id; entry.format = image.format; entry.name = path.stem().wstring();
        entry.width = image.sourceWidth; entry.height = image.sourceHeight;
        entries_.push_back(std::move(entry));
        if (!Save()) { entries_.pop_back(); error = "iconLibrary.saveFailed"; return {}; }
    }
    ++revision_;
    return id;
}
IconLibrarySnapshot IconLibrary::Invoke(const IconLibraryRequest& request, const std::vector<std::string>& protectedIds)
{
    if (!Load()) { IconLibrarySnapshot result; result.error = "iconLibrary.readFailed"; return result; }
    if (request.action == "delete" && std::any_of(request.ids.begin(), request.ids.end(), [&](const auto& id) {
            return std::find(protectedIds.begin(), protectedIds.end(), id) != protectedIds.end(); }))
    { auto result = Read(); result.succeeded = false; result.error = "iconLibrary.deleteInUse"; return result; }
    IconLibrarySnapshot operation;
    if (request.action == "import")
    {
        for (const auto& path : request.paths)
        {
            std::string error; const auto id = Import(path, error);
            if (!id.empty()) operation.imported.push_back(id);
            else operation.failures.push_back(std::filesystem::path(path).filename().wstring());
            if (!error.empty()) operation.error = error;
        }
    }
    else if (request.action == "rename")
    {
        auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return request.ids.size() == 1 && entry.id == request.ids[0]; });
        if (found == entries_.end() || request.name.empty() || request.name.size() > 256 || request.name.find_first_of(L"\r\n\t") != request.name.npos)
            operation.error = "iconLibrary.invalid";
        else { const auto old = found->name; found->name = request.name; if (!Save()) { found->name = old; operation.error = "iconLibrary.saveFailed"; } else ++revision_; }
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
    result.error = operation.error; result.failures = std::move(operation.failures); result.imported = std::move(operation.imported);
    result.succeeded = result.error.empty();
    return result;
}
}
