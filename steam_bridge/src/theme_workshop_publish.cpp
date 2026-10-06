#include "theme_workshop_publish.h"
#include "bridge_json.h"
#include "steam_app_identity.h"
#include "../../src/theme_library.h"
#include "../../src/theme_workshop_tags.h"
#include "../../src/theme_preview_parts.h"
#include "../../src/atomic_file.h"
#include <windows.h>
#include <bcrypt.h>
#include <charconv>
#include <fstream>
#include <array>
#include <limits>
#include <wincodec.h>
#include <wrl/client.h>

namespace snowdesktop::steam_bridge
{
namespace
{
bool Fail(std::string& error, const char* message) { error = message; return false; }
bool Failure(CoreError& error, const char* code) { error = {kSteamOperationFailed, code, code}; return false; }
std::string Read(const std::filesystem::path& path, std::size_t maximum)
{
    if (!ThemeSafePath(path)) return {};
    std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > maximum) return {};
    std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {});
}
bool ReadAssociation(const std::filesystem::path& path, std::string& owner, std::uint64_t& id, bool& pending, std::string* remoteRoot = nullptr)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return !ec;
    JsonValue value; std::string error;
    if (!ParseJson(Read(path, 4096), value, error) || JsonUnsigned(value, "version") != 1) return false;
    owner = JsonString(value, "owner").value_or("");
    const auto item = JsonString(value, "publishedFileId").value_or("");
    const auto converted = std::from_chars(item.data(), item.data() + item.size(), id);
    const auto p = JsonBoolean(value, "creationPending");
    if (converted.ec != std::errc{} || converted.ptr != item.data() + item.size() || !p) return false;
    if (remoteRoot) *remoteRoot = JsonString(value, "boundThemeId").value_or("");
    pending = *p; return true;
}
bool WriteAssociation(const std::filesystem::path& path, const std::string& owner, std::uint64_t id, bool pending, std::string_view remoteRoot = {})
{
    auto value = JsonValue::Object(); value.object["version"] = JsonValue::Number(1);
    value.object["owner"] = JsonValue::String(owner); value.object["publishedFileId"] = JsonValue::String(std::to_string(id));
    value.object["creationPending"] = JsonValue::Boolean(pending);
    if (!remoteRoot.empty()) value.object["boundThemeId"] = JsonValue::String(std::string(remoteRoot));
    return atomic_file::WriteAll(path, WriteJson(value));
}
bool ValidCover(const std::filesystem::path& path)
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Cleanup { HRESULT value; ~Cleanup() { if (SUCCEEDED(value)) CoUninitialize(); } } cleanup{initialized};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    GUID format{}; UINT width = 0, height = 0;
    const bool valid = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetContainerFormat(&format)) && format == GUID_ContainerFormatPng &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width == 1024 && height == 1024;
    if (!valid) return false;
    ComPtr<IWICFormatConverter> convert; std::vector<BYTE> pixels(1024 * 1024 * 4);
    return SUCCEEDED(factory->CreateFormatConverter(&convert)) && SUCCEEDED(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(convert->CopyPixels(nullptr, 1024 * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}
}
bool ThemeSafePath(const std::filesystem::path& path, bool directory)
{
    if (!path.is_absolute()) return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(attributes & FILE_ATTRIBUTE_DIRECTORY) != directory) return false;
    for (auto parent = path.parent_path(); !parent.empty();)
    {
        const auto attrs = GetFileAttributesW(parent.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
        const auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
    return true;
}
std::string ThemeSha256(std::string_view bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    struct Cleanup { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE* h; ~Cleanup() { if (*h) BCryptDestroyHash(*h); BCryptCloseAlgorithmProvider(a, 0); } } cleanup{algorithm, &hash};
    DWORD size = 0, received = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &received, 0) < 0) return {};
    std::vector<UCHAR> storage(size); std::array<UCHAR, 32> result{};
    if (BCryptCreateHash(algorithm, &hash, storage.data(), size, nullptr, 0, 0) < 0 ||
        bytes.size() > std::numeric_limits<ULONG>::max() || BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0) return {};
    std::string out; constexpr char hex[] = "0123456789abcdef";
    for (auto c : result) { out += hex[c >> 4]; out += hex[c & 15]; } return out;
}
std::string ThemeFileSha256(const std::filesystem::path& path)
{
    const auto bytes = Read(path, 4 * 1024 * 1024); return bytes.empty() ? std::string{} : ThemeSha256(bytes);
}
bool WriteThemePreparation(const std::filesystem::path& directory, std::string_view rootId,
    std::string_view title, std::int64_t now, std::string& error, const std::vector<std::string>& tags)
{
    themes::Package snapshot;
    if (!themes::ReadPackage(directory / L"package.snowtheme", snapshot, error)) return false;
    const auto theme = themes::Resolve(snapshot, rootId);
    if (!theme || !themes::tags::Valid(*theme, tags)) return Fail(error, "tagsRequired");
    const auto package = ThemeFileSha256(directory / L"package.snowtheme"), cover = ThemeFileSha256(directory / L"cover.png");
    if (package.empty() || cover.empty()) return Fail(error, "previewFailed");
    auto value = JsonValue::Object(); value.object["format"] = JsonValue::String("snowdesktop.theme-preparation");
    value.object["version"] = JsonValue::Number(kThemeWorkflowProtocolVersion);
    value.object["rootId"] = JsonValue::String(std::string(rootId)); value.object["title"] = JsonValue::String(std::string(title));
    value.object["packageSha256"] = JsonValue::String(package); value.object["coverSha256"] = JsonValue::String(cover);
    value.object["preparedAt"] = JsonValue::Number(static_cast<double>(now));
    auto selected = JsonValue::Array(); for (const auto& tag : tags) selected.array.push_back(JsonValue::String(tag));
    value.object["tags"] = selected; value.object["tagsSha256"] = JsonValue::String(ThemeSha256(WriteJson(selected, -1)));
    auto gallery = JsonValue::Array();
    const auto parts = themes::preview::Parts(snapshot, rootId, theme->kind == themes::Kind::Global ? theme->scopes : themes::All, error);
    if (parts.empty()) return false;
    for (const auto& part : parts)
    {
        const auto filename = themes::preview::GalleryFilename(ThemeSha256(rootId), part.component);
        const auto path = directory / filename;
        std::error_code ec;
        const auto hash = ThemeFileSha256(path);
        if (!ThemeSafePath(path) || hash.empty() || !ValidCover(path) || std::filesystem::file_size(path, ec) >= 1024 * 1024 || ec)
            return Fail(error, "previewFailed");
        auto image = JsonValue::Object(); image.object["component"] = JsonValue::String(part.component);
        image.object["file"] = JsonValue::String(filename); image.object["sha256"] = JsonValue::String(hash);
        gallery.array.push_back(std::move(image));
    }
    value.object["gallery"] = gallery; value.object["gallerySha256"] = JsonValue::String(ThemeSha256(WriteJson(gallery, -1)));
    return atomic_file::WriteAll(directory / L"theme.json", WriteJson(value), {}, &error);
}
bool BuildThemePublishPlan(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory,
    ThemePublishPlan& plan, std::string& error)
{
    if (!ThemeSafePath(directory, true) || !ThemeSafePath(dataDirectory, true)) return Fail(error, "unsafePath");
    JsonValue value;
    if (!ParseJson(Read(directory / L"theme.json", 8192), value, error) ||
        JsonString(value, "format") != "snowdesktop.theme-preparation" || JsonUnsigned(value, "version") != kThemeWorkflowProtocolVersion)
        return Fail(error, "invalidPreparation");
    ThemePublishPlan next; next.directory = directory; next.package = directory / L"package.snowtheme"; next.preview = directory / L"cover.png";
    next.rootId = JsonString(value, "rootId").value_or(""); next.title = JsonString(value, "title").value_or("");
    next.packageSha256 = JsonString(value, "packageSha256").value_or(""); next.coverSha256 = JsonString(value, "coverSha256").value_or("");
    next.preparedAt = static_cast<std::int64_t>(JsonUnsigned(value, "preparedAt").value_or(0));
    const auto selected = value.Find("tags");
    if (!selected || !selected->IsArray()) return Fail(error, "tagsRequired");
    for (const auto& tag : selected->array) { if (!tag.IsString()) return Fail(error, "tagsRequired"); next.tags.push_back(tag.string); }
    next.tagsSha256 = JsonString(value, "tagsSha256").value_or("");
    if (next.tagsSha256 != ThemeSha256(WriteJson(*selected, -1))) return Fail(error, "stalePreparation");
    themes::Package package;
    if (next.title.empty() || next.title.size() >= 129 || !next.preparedAt ||
        !themes::ReadPackage(next.package, package, error) || !themes::Resolve(package, next.rootId) ||
        ThemeFileSha256(next.package) != next.packageSha256 || ThemeFileSha256(next.preview) != next.coverSha256 ||
        next.packageSha256.empty() || next.coverSha256.empty()) return Fail(error, "stalePreparation");
    if (!themes::tags::Valid(*themes::Resolve(package, next.rootId), next.tags)) return Fail(error, "tagsRequired");
    const auto gallery = value.Find("gallery");
    const auto root = themes::Resolve(package, next.rootId);
    const auto parts = themes::preview::Parts(package, next.rootId, root->kind == themes::Kind::Global ? root->scopes : themes::All, error);
    next.gallerySha256 = JsonString(value, "gallerySha256").value_or("");
    if (!gallery || !gallery->IsArray() || parts.empty() || gallery->array.size() != parts.size() ||
        next.gallerySha256 != ThemeSha256(WriteJson(*gallery, -1))) return Fail(error, "stalePreparation");
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        const auto& image = gallery->array[index];
        const auto filename = themes::preview::GalleryFilename(ThemeSha256(next.rootId), parts[index].component);
        const auto path = directory / filename;
        const auto hash = JsonString(image, "sha256").value_or("");
        std::error_code ec;
        if (JsonString(image, "component") != parts[index].component || JsonString(image, "file") != filename ||
            !ThemeSafePath(path) || hash.empty() || ThemeFileSha256(path) != hash || !ValidCover(path) ||
            std::filesystem::file_size(path, ec) >= 1024 * 1024 || ec) return Fail(error, "stalePreparation");
        next.gallery.push_back({parts[index].component, hash, path});
    }
    std::error_code ec;
    if (std::filesystem::file_size(next.preview, ec) >= 1024 * 1024 || ec || !ValidCover(next.preview)) return Fail(error, "previewTooLarge");
    const auto store = dataDirectory / L"ThemeWorkshop";
    std::filesystem::create_directories(store, ec); if (ec || !ThemeSafePath(store, true)) return Fail(error, "writeFailed");
    next.association = store / (ThemeSha256(next.rootId) + ".json");
    std::string owner; bool pending = false;
    if (!ReadAssociation(next.association, owner, next.publishedFileId, pending)) return Fail(error, "invalidAssociation");
    if (pending && !next.publishedFileId) return Fail(error, "creationUncertain");
    plan = std::move(next); return true;
}
std::string ThemePublishedUrl(const std::filesystem::path& dataDirectory, std::string_view rootId)
{
    const auto path = dataDirectory / L"ThemeWorkshop" / (ThemeSha256(rootId) + ".json");
    std::string owner; std::uint64_t id = 0; bool pending = false;
    if (!ThemeSafePath(path) || !ReadAssociation(path, owner, id, pending) || !id) return {};
    return "https://steamcommunity.com/sharedfiles/filedetails/?id=" + std::to_string(id);
}
bool BindThemePublication(const std::filesystem::path& dataDirectory, std::string_view rootId,
    const SteamStatus& status, const PublishedItem& item, std::string& error)
{
    if (!status.loggedOn || status.appId != kSteamAppId || status.steamId.empty() ||
        std::to_string(item.ownerSteamId) != status.steamId || item.consumerAppId != kSteamAppId ||
        !item.publishedFileId || item.banned) return Fail(error, "authorMismatch");
    JsonValue identity;
    if (rootId.empty() || !ParseJson(item.metadata, identity, error) ||
        JsonString(identity, "format") != "snowdesktop-theme" || JsonUnsigned(identity, "themeWorkflowProtocolVersion") != 1 ||
        JsonString(identity, "themeId").value_or("").empty()) return Fail(error, "itemMismatch");
    if (!ThemeSafePath(dataDirectory, true)) return Fail(error, "unsafePath");
    const auto store = dataDirectory / L"ThemeWorkshop"; std::error_code ec;
    std::filesystem::create_directories(store, ec);
    if (ec || !ThemeSafePath(store, true)) return Fail(error, "writeFailed");
    const auto path = store / (ThemeSha256(rootId) + ".json");
    auto lockPath = path; lockPath += L".lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return Fail(error, "publicationBusy");
    struct Cleanup { HANDLE h; ~Cleanup() { CloseHandle(h); } } cleanup{lock};
    std::string owner; std::uint64_t id = 0; bool pending = false;
    if (!ReadAssociation(path, owner, id, pending)) return Fail(error, "invalidAssociation");
    if (id && (id != item.publishedFileId || owner != status.steamId)) return Fail(error, "itemMismatch");
    return WriteAssociation(path, status.steamId, item.publishedFileId, false,
        JsonString(identity, "themeId").value_or("")) || Fail(error, "writeFailed");
}
bool UnbindThemePublication(const std::filesystem::path& dataDirectory, std::string_view rootId, std::string& error)
{
    if (rootId.empty() || themes::Builtin(rootId)) return Fail(error, "invalidSelection");
    if (!ThemeSafePath(dataDirectory, true)) return Fail(error, "unsafePath");
    const auto hash = ThemeSha256(rootId);
    if (hash.empty()) return Fail(error, "writeFailed");
    const auto store = dataDirectory / L"ThemeWorkshop";
    std::error_code ec;
    if (!std::filesystem::exists(store, ec)) return !ec || Fail(error, "readFailed");
    if (!ThemeSafePath(store, true)) return Fail(error, "unsafePath");
    const auto path = store / (hash + ".json");
    auto lockPath = path; lockPath += L".lock";
    if (std::filesystem::exists(lockPath, ec) && !ThemeSafePath(lockPath)) return Fail(error, "unsafePath");
    if (ec) return Fail(error, "readFailed");
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return Fail(error, "publicationBusy");
    struct Cleanup { HANDLE h; ~Cleanup() { CloseHandle(h); } } cleanup{lock};
    if (!std::filesystem::exists(path, ec)) return !ec || Fail(error, "readFailed");
    if (!ThemeSafePath(path)) return Fail(error, "unsafePath");
    // A neutral journal keeps the existing private format and permits a fresh
    // confirmed publication or explicit rebind without touching any remote item.
    return WriteAssociation(path, "", 0, false) || Fail(error, "writeFailed");
}
std::string ThemePublishPlanJson(const ThemePublishPlan& plan)
{
    auto value = JsonValue::Object(); value.object["ok"] = JsonValue::Boolean(true);
    value.object["themeWorkflowProtocolVersion"] = JsonValue::Number(kThemeWorkflowProtocolVersion);
    value.object["rootId"] = JsonValue::String(plan.rootId); value.object["title"] = JsonValue::String(plan.title);
    value.object["packageSha256"] = JsonValue::String(plan.packageSha256); value.object["coverSha256"] = JsonValue::String(plan.coverSha256);
    value.object["tagsSha256"] = JsonValue::String(plan.tagsSha256);
    value.object["gallerySha256"] = JsonValue::String(plan.gallerySha256);
    value.object["tags"] = JsonValue::Array(); for (const auto& tag : plan.tags) value.object["tags"].array.push_back(JsonValue::String(tag));
    value.object["publishedFileId"] = JsonValue::String(std::to_string(plan.publishedFileId));
    value.object["requiredConfirmation"] = JsonValue::String(plan.publishedFileId ? "--confirm-update" : "--confirm-create");
    return WriteJson(value, -1);
}
bool ExecuteThemePublishPlan(const ThemePublishPlan& plan, bool confirmCreate, bool confirmUpdate,
    ThemePublishTransport& transport, const PublishProgressCallback& progress, PublishResult& result, CoreError& error,
    std::int64_t now, const std::atomic_bool* cancel)
{
    auto lockPath = plan.association; lockPath += L".lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return Failure(error, "publicationBusy");
    struct Cleanup { HANDLE h; ~Cleanup() { CloseHandle(h); } } cleanup{lock};
    if (cancel && cancel->load()) return Failure(error, "cancelled");
    if (now < plan.preparedAt || now - plan.preparedAt > 15 * 60 ||
        ThemeFileSha256(plan.package) != plan.packageSha256 || ThemeFileSha256(plan.preview) != plan.coverSha256)
        return Failure(error, "stalePreparation");
    ThemePublishPlan current; std::string preparationError;
    if (!BuildThemePublishPlan(plan.directory, plan.association.parent_path().parent_path(), current, preparationError) ||
        current.tags != plan.tags || current.tagsSha256 != plan.tagsSha256 || current.gallerySha256 != plan.gallerySha256 ||
        current.gallery.size() != plan.gallery.size()) return Failure(error, "stalePreparation");
    for (std::size_t index = 0; index < current.gallery.size(); ++index)
        if (current.gallery[index].component != plan.gallery[index].component || current.gallery[index].path != plan.gallery[index].path ||
            current.gallery[index].sha256 != plan.gallery[index].sha256) return Failure(error, "stalePreparation");
    std::string owner; std::uint64_t id = 0; bool pending = false;
    std::string remoteRoot;
    if (!ReadAssociation(plan.association, owner, id, pending, &remoteRoot)) return Failure(error, "invalidAssociation");
    if (pending && !id) return Failure(error, "creationUncertain");
    if (id != plan.publishedFileId || confirmCreate == confirmUpdate || (id ? !confirmUpdate : !confirmCreate))
        return Failure(error, "confirmationRequired");
    if (!transport.status || !transport.agreement || !transport.item || !transport.publish) return Failure(error, "bridgeUnavailable");
    const auto status = transport.status(error);
    if (!status || !status->loggedOn || status->appId != kSteamAppId || status->steamId.empty()) return Failure(error, "steamUnavailable");
    const auto agreement = transport.agreement(error);
    // Apps without an app-specific EULA report unavailable. Create/Submit
    // callbacks remain authoritative for Steam's Workshop legal agreement.
    if (!agreement || (agreement->available && (!agreement->accepted || agreement->needsAction))) return Failure(error, "agreementRequired");
    std::string previousGalleryPrefix;
    if (id)
    {
        const auto item = transport.item(id, error);
        if (!item || item->publishedFileId != id || std::to_string(item->ownerSteamId) != status->steamId ||
            (!owner.empty() && owner != status->steamId) || item->consumerAppId != kSteamAppId || item->banned)
            return Failure(error, "authorMismatch");
        if (!item->metadata.empty())
        {
            JsonValue identity; std::string detail;
            if (!ParseJson(item->metadata, identity, detail) || JsonString(identity, "format") != "snowdesktop-theme" ||
                (JsonString(identity, "themeId") != plan.rootId && (remoteRoot.empty() || JsonString(identity, "themeId") != remoteRoot)) ||
                JsonUnsigned(identity, "themeWorkflowProtocolVersion") != 1)
                return Failure(error, "itemMismatch");
            previousGalleryPrefix = themes::preview::GalleryPrefix(ThemeSha256(JsonString(identity, "themeId").value_or("")));
        }
    }
    if (cancel && cancel->load()) return Failure(error, "cancelled");
    PublishRequest request; request.package = plan.package; request.preview = plan.preview;
    request.contentKind = WorkshopContentKind::Theme; request.title = plan.title;
    for (const auto& image : plan.gallery) request.additionalPreviews.push_back(image.path);
    request.managedPreviewPrefix = themes::preview::GalleryPrefix(ThemeSha256(plan.rootId));
    request.previousManagedPreviewPrefix = std::move(previousGalleryPrefix);
    request.tags = plan.tags; request.tags->emplace_back(themes::tags::Content);
    auto metadata = JsonValue::Object(); metadata.object["format"] = JsonValue::String("snowdesktop-theme");
    metadata.object["artifact"] = JsonValue::String("package.snowtheme"); metadata.object["themeWorkflowProtocolVersion"] = JsonValue::Number(1);
    metadata.object["themeId"] = JsonValue::String(plan.rootId); metadata.object["packageSha256"] = JsonValue::String(plan.packageSha256);
    request.metadata = WriteJson(metadata, -1);
    if (id) request.publishedFileId = id;
    else request.prepareCreateItem = [&] { return WriteAssociation(plan.association, status->steamId, 0, true); };
    request.persistCreatedItem = [&](std::uint64_t created) {
        result.created = true; result.publishedFileId = created;
        return WriteAssociation(plan.association, status->steamId, created, false);
    };
    request.validateStagedArtifacts = [&](const std::filesystem::path& package, const std::filesystem::path& cover) {
        return ThemeFileSha256(package) == plan.packageSha256 && ThemeFileSha256(cover) == plan.coverSha256;
    };
    request.validateStagedPreviews = [&](const std::vector<std::filesystem::path>& paths) {
        if (paths.size() != plan.gallery.size()) return false;
        for (std::size_t index = 0; index < paths.size(); ++index)
            if (paths[index].filename() != plan.gallery[index].path.filename() ||
                ThemeFileSha256(paths[index]) != plan.gallery[index].sha256) return false;
        return true;
    };
    const auto published = transport.publish(request, progress, error);
    if (!published) return false;
    result = *published;
    if (result.needsLegalAgreement) return Failure(error, "agreementRequired");
    return result.publishedFileId != 0 || Failure(error, "publishFailed");
}
}
