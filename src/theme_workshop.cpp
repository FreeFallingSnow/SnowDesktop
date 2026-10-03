#include "theme_workshop.h"
#include "theme_preview.h"
#include "steam_app_identity.h"
#include "json_value.h"
#include <fstream>
#include <chrono>

namespace snowdesktop::themes::workshop
{
namespace
{
bool Fail(std::string& error, const char* code) { error = code; return false; }
std::string String(const JsonValue& json, const char* key)
{ const auto* v = json.Find(key); return v && v->IsString() ? v->string : std::string{}; }
bool Boolean(const JsonValue& json, const char* key, bool fallback = false)
{ const auto* v = json.Find(key); return v && v->IsBoolean() ? v->boolean : fallback; }
double Number(const JsonValue& json, const char* key)
{ const auto* v = json.Find(key); return v && v->IsNumber() ? v->number : -1; }
bool Digits(std::string_view value)
{ return !value.empty() && value.size() <= 20 && value != "0" && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }); }
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
bool Final(std::string_view output, JsonValue& value)
{
    // A single response may contain JSON whitespace; progress streams retain
    // their existing last-result JSON Lines handling.
    JsonValue complete;
    if (ParseJson(output, complete) && complete.Find("ok"))
    { value = std::move(complete); return true; }
    bool found = false;
    while (!output.empty())
    {
        const auto end = output.find('\n'); const auto line = output.substr(0, end);
        JsonValue parsed; if (ParseJson(line, parsed) && parsed.Find("ok")) { value = std::move(parsed); found = true; }
        if (end == std::string_view::npos) break; output.remove_prefix(end + 1);
    }
    return found;
}
}
bool Capabilities(std::string_view configuration, std::string_view hostVersion)
{
    JsonValue json;
    if (!Final(configuration, json) || !Boolean(json, "ok") || !Boolean(json, "steamworksCompiled") ||
        Number(json, "protocolVersion") != 1 || Number(json, "expectedAppId") != kSnowDesktopSteamAppId ||
        Number(json, "themeWorkflowProtocolVersion") != 1 || String(json, "version") != hostVersion) return false;
    const auto* capabilities = json.Find("capabilities");
    return capabilities && capabilities->IsArray() && std::any_of(capabilities->array.begin(), capabilities->array.end(),
        [](const auto& value) { return value.IsString() && value.string == "workshop.theme.v1"; });
}
bool Available(const std::filesystem::path& bridge, std::string_view hostVersion)
{
    std::string output, error;
    return steam_bridge::ThemeSafePath(bridge) && preview::Run(bridge, {L"configuration"}, output, 3000, error) && Capabilities(output, hostVersion);
}
bool DecodeSubscriptions(std::string_view text, SubscriptionSnapshot& snapshot, std::string& error)
{
    JsonValue json;
    if (!Final(text, json) || !Boolean(json, "ok") || !Boolean(json, "authoritative") ||
        Number(json, "protocolVersion") != 1 || Number(json, "appId") != kSnowDesktopSteamAppId || !Digits(String(json, "steamId")))
        return Fail(error, "subscriptionsUnavailable");
    const auto* items = json.Find("items"); if (!items || !items->IsArray() || items->array.size() > 4096) return Fail(error, "subscriptionsUnavailable");
    SubscriptionSnapshot next; next.account = String(json, "steamId");
    for (const auto& item : items->array)
    {
        const auto id = String(item, "publishedFileId");
        if (!Digits(id) || !next.subscribed.insert(id).second || !Boolean(item, "subscribed")) return Fail(error, "subscriptionsUnavailable");
        const auto* details = item.Find("details");
        if (!details || !details->IsObject()) continue;
        if (Number(*details, "result") == 9) { next.deleted.insert(id); continue; }
        if (Number(*details, "result") != 1 || Number(*details, "consumerAppId") != kSnowDesktopSteamAppId || Boolean(*details, "banned", true)) continue;
        JsonValue metadata;
        if (!ParseJson(String(*details, "metadata"), metadata) || String(metadata, "format") != "snowdesktop-theme" ||
            String(metadata, "artifact") != "package.snowtheme" || Number(metadata, "themeWorkflowProtocolVersion") != 1) continue;
        if (Boolean(item, "downloading") || Boolean(item, "downloadPending") || Boolean(item, "needsUpdate", true) || !Boolean(item, "installed")) continue;
        const auto* install = item.Find("installInfo"); if (!install || !Boolean(*install, "available")) continue;
        const auto folder = String(*install, "folder");
        const auto directory = std::filesystem::path(std::u8string(folder.begin(), folder.end()));
        const auto package = directory / L"package.snowtheme";
        if (!steam_bridge::ThemeSafePath(directory, true) || !steam_bridge::ThemeSafePath(package)) continue;
        // Theme items have one authoritative file. Do not accept mixed widget
        // content or a partial Steam folder as a completed theme download.
        std::error_code ec; unsigned files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        { ++files; if (entry.path().filename() != L"package.snowtheme" || !entry.is_regular_file(ec)) { files = 2; break; } }
        if (ec || files != 1) continue;
        Download download; download.item = id; download.owner = String(*details, "ownerSteamId");
        download.sha256 = steam_bridge::ThemeFileSha256(package);
        if (!Digits(download.owner) || download.sha256.empty() || download.sha256 != String(metadata, "packageSha256") ||
            !ReadPackage(package, download.package, error) || steam_bridge::ThemeFileSha256(package) != download.sha256) continue;
        if (!Resolve(download.package, String(metadata, "themeId"))) continue;
        next.downloads.push_back(std::move(download));
    }
    next.authoritative = true; snapshot = std::move(next); error.clear(); return true;
}
bool Reconcile(Library& library, const SubscriptionSnapshot& snapshot, std::string& error, const NewId& newId)
{
    if (!snapshot.authoritative || !Digits(snapshot.account) ||
        !std::all_of(snapshot.subscribed.begin(), snapshot.subscribed.end(), Digits) ||
        !std::all_of(snapshot.deleted.begin(), snapshot.deleted.end(), Digits)) return Fail(error, "subscriptionsUnavailable");
    Library next = library;
    const bool known = next.subscriptionAccounts.contains(snapshot.account);
    next.subscriptionAccounts[snapshot.account] = snapshot.subscribed;
    for (const auto& download : snapshot.downloads)
    {
        if (!snapshot.subscribed.contains(download.item) || !Digits(download.item) || !Digits(download.owner) ||
            download.sha256.size() != 64 || !Validate(download.package, error)) return Fail(error, "invalidPackage");
        const auto previous = next.workshop.find(download.item);
        if (previous != next.workshop.end() && previous->second.owner != download.owner) return Fail(error, "authorMismatch");
        if (previous != next.workshop.end() && previous->second.sha256 == download.sha256)
        { previous->second.accounts.insert(snapshot.account); continue; }
        std::map<std::string, std::string> mapping;
        if (!Import(next, download.package, mapping, error, newId)) return false;
        auto& origin = next.workshop[download.item]; origin.owner = download.owner; origin.sha256 = download.sha256;
        origin.accounts.insert(snapshot.account); origin.ids.clear();
        for (const auto& [from, to] : mapping) { (void)from; if (!Builtin(to)) origin.ids.insert(to); }
        // Changed content receives new identities. Already bound/active old
        // identities remain local with their immutable last-success snapshots.
    }
    for (auto it = next.workshop.begin(); it != next.workshop.end();)
    {
        if (snapshot.deleted.contains(it->first)) { it = next.workshop.erase(it); continue; }
        if (known && !snapshot.subscribed.contains(it->first)) it->second.accounts.erase(snapshot.account);
        if (it->second.accounts.empty()) it = next.workshop.erase(it); else ++it;
        // Removal converts provenance to local. It never removes theme values,
        // saved global bindings, active references, or their complete snapshots.
    }
    if (!Validate(next.themes, error)) return false;
    library = std::move(next); return true;
}
bool Sync(const std::filesystem::path& bridge, const std::filesystem::path& library,
    Library& output, std::string& error, const std::atomic_bool* cancel)
{
    std::string text;
    if (!preview::Run(bridge, {L"workshop", L"list-subscribed", L"--details"}, text, 60000, error, cancel)) return false;
    SubscriptionSnapshot snapshot;
    if (!DecodeSubscriptions(text, snapshot, error)) return false;
    if (cancel && cancel->load()) return Fail(error, "cancelled");
    return Transact(library, [&](Library& into, std::string& detail) {
        if (cancel && cancel->load()) return Fail(detail, "cancelled");
        return Reconcile(into, snapshot, detail);
    }, output, error);
}
bool Prepare(const Package& package, std::string_view root, unsigned scope, const std::filesystem::path& directory,
    const std::filesystem::path& data, const std::filesystem::path& customCover, const Renderer& renderer,
    steam_bridge::ThemePublishPlan& plan, std::string& error, const std::atomic_bool* cancel)
{
    plan = {}; const auto theme = Resolve(package, root);
    if (!theme || !Validate(package, error) || !renderer) return Fail(error, "invalidPackage");
    std::error_code ec;
    if (std::filesystem::exists(directory, ec) || ec) return Fail(error, "writeFailed");
    struct Cleanup { std::filesystem::path directory; bool keep = false; ~Cleanup() { if (!keep) { std::error_code ec; std::filesystem::remove_all(directory, ec); } } } cleanup{directory};
    std::filesystem::path cover;
    if (!renderer(package, root, scope, directory, cover, error, cancel)) return false;
    Package rendered;
    if (!ReadPackage(directory / L"package.snowtheme", rendered, error) || EncodePackage(rendered, error) != EncodePackage(package, error)) return Fail(error, "stalePreparation");
    if (!customCover.empty())
    {
        if (!preview::NormalizeCover(customCover, cover, error)) return false;
    }
    if (cancel && cancel->load()) return Fail(error, "cancelled");
    if (!steam_bridge::WriteThemePreparation(directory, root, theme->name, Now(), error) ||
        !steam_bridge::BuildThemePublishPlan(directory, data, plan, error)) return false;
    cleanup.keep = true; return true;
}
bool Publish(const std::filesystem::path& bridge, const steam_bridge::ThemePublishPlan& plan,
    const std::filesystem::path& data, std::string& output, std::string& error, const std::atomic_bool* cancel)
{
    if (!preview::Run(bridge, {L"workshop", L"theme-publish", L"--prepared", plan.directory.wstring(),
        L"--data-directory", data.wstring(), L"--package-sha256", std::wstring(plan.packageSha256.begin(), plan.packageSha256.end()),
        L"--cover-sha256", std::wstring(plan.coverSha256.begin(), plan.coverSha256.end()),
        plan.publishedFileId ? L"--confirm-update" : L"--confirm-create"}, output, 31 * 60 * 1000, error, cancel))
    { JsonValue failed; if (Final(output, failed) && !String(failed, "error").empty()) error = String(failed, "error"); return false; }
    JsonValue json; return (Final(output, json) && Boolean(json, "ok") && !Boolean(json, "needsLegalAgreement")) || Fail(error, "publishFailed");
}
}
