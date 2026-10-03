#include "theme_workshop_publish.h"
#include "bridge_json.h"
#include "steam_app_identity.h"
#include "../../src/theme_library.h"
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
bool ReadAssociation(const std::filesystem::path& path, std::string& owner, std::uint64_t& id, bool& pending)
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
    pending = *p; return true;
}
bool WriteAssociation(const std::filesystem::path& path, const std::string& owner, std::uint64_t id, bool pending)
{
    auto value = JsonValue::Object(); value.object["version"] = JsonValue::Number(1);
    value.object["owner"] = JsonValue::String(owner); value.object["publishedFileId"] = JsonValue::String(std::to_string(id));
    value.object["creationPending"] = JsonValue::Boolean(pending);
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
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetContainerFormat(&format)) && format == GUID_ContainerFormatPng &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width == 1024 && height == 1024;
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
    std::string_view title, std::int64_t now, std::string& error)
{
    const auto package = ThemeFileSha256(directory / L"package.snowtheme"), cover = ThemeFileSha256(directory / L"cover.png");
    if (package.empty() || cover.empty()) return Fail(error, "previewFailed");
    auto value = JsonValue::Object(); value.object["format"] = JsonValue::String("snowdesktop.theme-preparation");
    value.object["version"] = JsonValue::Number(kThemeWorkflowProtocolVersion);
    value.object["rootId"] = JsonValue::String(std::string(rootId)); value.object["title"] = JsonValue::String(std::string(title));
    value.object["packageSha256"] = JsonValue::String(package); value.object["coverSha256"] = JsonValue::String(cover);
    value.object["preparedAt"] = JsonValue::Number(static_cast<double>(now));
    return atomic_file::WriteAll(directory / L"theme.json", WriteJson(value), {}, &error);
}
bool BuildThemePublishPlan(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory,
    ThemePublishPlan& plan, std::string& error)
{
    if (!ThemeSafePath(directory, true) || !ThemeSafePath(dataDirectory, true)) return Fail(error, "unsafePath");
    JsonValue value;
    if (!ParseJson(Read(directory / L"theme.json", 4096), value, error) ||
        JsonString(value, "format") != "snowdesktop.theme-preparation" || JsonUnsigned(value, "version") != kThemeWorkflowProtocolVersion)
        return Fail(error, "invalidPreparation");
    ThemePublishPlan next; next.directory = directory; next.package = directory / L"package.snowtheme"; next.preview = directory / L"cover.png";
    next.rootId = JsonString(value, "rootId").value_or(""); next.title = JsonString(value, "title").value_or("");
    next.packageSha256 = JsonString(value, "packageSha256").value_or(""); next.coverSha256 = JsonString(value, "coverSha256").value_or("");
    next.preparedAt = static_cast<std::int64_t>(JsonUnsigned(value, "preparedAt").value_or(0));
    themes::Package package;
    if (next.title.empty() || next.title.size() >= 129 || !next.preparedAt ||
        !themes::ReadPackage(next.package, package, error) || !themes::Resolve(package, next.rootId) ||
        ThemeFileSha256(next.package) != next.packageSha256 || ThemeFileSha256(next.preview) != next.coverSha256 ||
        next.packageSha256.empty() || next.coverSha256.empty()) return Fail(error, "stalePreparation");
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
std::string ThemePublishPlanJson(const ThemePublishPlan& plan)
{
    auto value = JsonValue::Object(); value.object["ok"] = JsonValue::Boolean(true);
    value.object["themeWorkflowProtocolVersion"] = JsonValue::Number(kThemeWorkflowProtocolVersion);
    value.object["rootId"] = JsonValue::String(plan.rootId); value.object["title"] = JsonValue::String(plan.title);
    value.object["packageSha256"] = JsonValue::String(plan.packageSha256); value.object["coverSha256"] = JsonValue::String(plan.coverSha256);
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
    std::string owner; std::uint64_t id = 0; bool pending = false;
    if (!ReadAssociation(plan.association, owner, id, pending)) return Failure(error, "invalidAssociation");
    if (pending && !id) return Failure(error, "creationUncertain");
    if (id != plan.publishedFileId || confirmCreate == confirmUpdate || (id ? !confirmUpdate : !confirmCreate))
        return Failure(error, "confirmationRequired");
    if (!transport.status || !transport.agreement || !transport.item || !transport.publish) return Failure(error, "bridgeUnavailable");
    const auto status = transport.status(error);
    if (!status || !status->loggedOn || status->appId != kSteamAppId || status->steamId.empty()) return Failure(error, "steamUnavailable");
    const auto agreement = transport.agreement(error);
    if (!agreement || !agreement->available || !agreement->accepted || agreement->needsAction) return Failure(error, "agreementRequired");
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
                JsonString(identity, "themeId") != plan.rootId || JsonUnsigned(identity, "themeWorkflowProtocolVersion") != 1)
                return Failure(error, "itemMismatch");
        }
    }
    if (cancel && cancel->load()) return Failure(error, "cancelled");
    PublishRequest request; request.package = plan.package; request.preview = plan.preview;
    request.contentKind = WorkshopContentKind::Theme; request.title = plan.title;
    request.tags = std::vector<std::string>{"Theme"};
    auto metadata = JsonValue::Object(); metadata.object["format"] = JsonValue::String("snowdesktop-theme");
    metadata.object["artifact"] = JsonValue::String("package.snowtheme"); metadata.object["themeWorkflowProtocolVersion"] = JsonValue::Number(1);
    metadata.object["themeId"] = JsonValue::String(plan.rootId); metadata.object["packageSha256"] = JsonValue::String(plan.packageSha256);
    request.metadata = WriteJson(metadata, -1);
    if (id) request.publishedFileId = id;
    else if (!WriteAssociation(plan.association, status->steamId, 0, true)) return Failure(error, "writeFailed");
    request.persistCreatedItem = [&](std::uint64_t created) {
        result.created = true; result.publishedFileId = created;
        return WriteAssociation(plan.association, status->steamId, created, false);
    };
    request.validateStagedArtifacts = [&](const std::filesystem::path& package, const std::filesystem::path& cover) {
        return ThemeFileSha256(package) == plan.packageSha256 && ThemeFileSha256(cover) == plan.coverSha256;
    };
    const auto published = transport.publish(request, progress, error);
    if (!published) return false;
    result = *published;
    if (result.needsLegalAgreement) return Failure(error, "agreementRequired");
    return result.publishedFileId != 0 || Failure(error, "publishFailed");
}
}
