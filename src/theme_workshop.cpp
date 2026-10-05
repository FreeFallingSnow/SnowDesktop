#include "theme_workshop.h"
#include "theme_preview.h"
#include "theme_workshop_tags.h"
#include "theme_bridge_availability_cache.h"
#include "steam_app_identity.h"
#include "json_value.h"
#include <fstream>
#include <chrono>
#include <charconv>

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
std::map<std::string, std::string> RecoverSourceIds(const Library& library,
    const Library::WorkshopOrigin& origin, const Package& source)
{
    // A matching verified package hash is required by the caller. Reconstruct
    // only a unique, complete graph with identical material and dependency edges.
    std::map<std::string, std::string> installed;
    std::set<std::string> used;
    for (const auto kind : {Kind::QuickPanel, Kind::Popup, Kind::Global})
        for (const auto& [authoredId, authored] : source)
        {
            if (Builtin(authoredId) || authored.kind != kind) continue;
            std::string match;
            for (const auto& id : origin.ids)
            {
                const auto saved = Find(library.themes, id);
                if (!saved || used.contains(id) || saved->kind != authored.kind || saved->scopes != authored.scopes ||
                    saved->name != authored.name || saved->appearance != authored.appearance ||
                    saved->layout != authored.layout || saved->colors != authored.colors) continue;
                const auto binding = [&](const std::string& key) {
                    if (key.empty() || Builtin(key)) return key;
                    const auto found = installed.find(key);
                    return found == installed.end() ? std::string{} : found->second;
                };
                if (saved->quickPanel != binding(authored.quickPanel) || saved->popup != binding(authored.popup)) continue;
                if (!match.empty()) return {};
                match = id;
            }
            if (match.empty()) return {};
            installed.emplace(authoredId, match); used.insert(match);
        }
    if (used != origin.ids) return {};
    std::map<std::string, std::string> result;
    for (const auto& [authored, id] : installed) result.emplace(id, authored);
    return result;
}
bool RetireInstalled(Library& library, std::set<std::string> ids, std::string& error, const NewId& newId)
{
    // Storage identities belong to the subscription, never its editable local
    // counterpart. Other accounts/items still owning an identity retain it.
    for (const auto& [item, origin] : library.workshop)
    { (void)item; for (const auto& id : origin.ids) ids.erase(id); }
    std::map<std::string, std::string> retainedBindings;
    std::set<std::string> needed;
    for (const auto& [id, theme] : library.themes)
        if (!ids.contains(id) && theme.kind == Kind::Global)
            for (const auto* binding : {&theme.quickPanel, &theme.popup})
                if (ids.contains(*binding)) needed.insert(*binding);
    for (const auto& id : needed)
    {
        const auto child = Resolve(library.themes, id);
        if (!child || child->kind == Kind::Global) return Fail(error, "missingDependency");
        std::string saved;
        if (!Save(library, *child, {}, false, saved, error, newId)) return false;
        retainedBindings.emplace(id, saved);
    }
    std::set<std::string> reboundGlobals;
    for (auto& [id, theme] : library.themes)
    {
        if (ids.contains(id) || theme.kind != Kind::Global) continue;
        for (auto* binding : {&theme.quickPanel, &theme.popup})
        {
            if (!ids.contains(*binding)) continue;
            *binding = retainedBindings.at(*binding); reboundGlobals.insert(id);
        }
    }
    for (auto& [target, reference] : library.references)
        if (ids.contains(reference.id) || (target == "global" && reboundGlobals.contains(reference.id)))
            reference.id.clear(); // Complete last-success snapshot stays intact.
    for (const auto& id : ids) library.themes.erase(id);
    return true;
}
}
bool BridgeCapabilities(std::string_view configuration, std::string_view hostVersion)
{
    JsonValue json;
    return Final(configuration, json) && Boolean(json, "ok") && Boolean(json, "steamworksCompiled") &&
        Number(json, "protocolVersion") == 1 && Number(json, "expectedAppId") == kSnowDesktopSteamAppId &&
        String(json, "version") == hostVersion;
}
bool Capabilities(std::string_view configuration, std::string_view hostVersion)
{
    JsonValue json;
    if (!BridgeCapabilities(configuration, hostVersion) || !Final(configuration, json) ||
        Number(json, "themeWorkflowProtocolVersion") != 1) return false;
    const auto* capabilities = json.Find("capabilities");
    const auto has = [&](std::string_view key) { return capabilities && capabilities->IsArray() &&
        std::any_of(capabilities->array.begin(), capabilities->array.end(),
            [key](const auto& value) { return value.IsString() && value.string == key; }); };
    return has("workshop.theme.v1") && has("workshop.theme.tags.v1") && has("workshop.theme.gallery.v1") &&
        has("workshop.theme.color-alpha.v1");
}
bool Available(const std::filesystem::path& bridge, std::string_view hostVersion)
{
    bool sharing = false; (void)Availability(bridge, hostVersion, sharing); return sharing;
}
bool Availability(const std::filesystem::path& bridge, std::string_view hostVersion, bool& sharing)
{
    static detail::BridgeAvailabilityCache cache;
    const auto available = cache.Query(bridge, hostVersion, detail::BridgeAvailabilityCache::Clock::now(), [&] {
        std::string output, error;
        if (!preview::Run(bridge, {L"configuration"}, output, 3000, error)) return detail::BridgeAvailability{};
        return detail::BridgeAvailability{BridgeCapabilities(output, hostVersion), Capabilities(output, hostVersion)};
    });
    sharing = available.sharing;
    return available.workshop;
}
bool CopyLocal(Library& library, std::string_view id, std::string& savedId, std::string& error, const NewId& newId)
{
    Package package; if (!Export(library, id, package, error)) return false;
    std::map<std::string, std::string> ids;
    for (const auto& [key, theme] : package)
    {
        (void)theme; if (Builtin(key)) continue;
        const auto fresh = newId();
        if (fresh.empty() || library.themes.contains(fresh) || package.contains(fresh) ||
            std::any_of(ids.begin(), ids.end(), [&](const auto& pair) { return pair.second == fresh; })) return Fail(error, "idConflict");
        ids.emplace(key, fresh);
    }
    if (!ids.contains(std::string(id))) return Fail(error, "invalidSelection");
    Package copied;
    for (auto [key, theme] : package)
    {
        if (Builtin(key)) continue;
        theme.id = ids.at(key);
        if (ids.contains(theme.quickPanel)) theme.quickPanel = ids.at(theme.quickPanel);
        if (ids.contains(theme.popup)) theme.popup = ids.at(theme.popup);
        copied.emplace(theme.id, std::move(theme));
    }
    Library next = library; std::map<std::string, std::string> mapping;
    if (!Import(next, copied, mapping, error, newId)) return false;
    savedId = ids.at(std::string(id)); library = std::move(next); return true;
}
bool Bind(const std::filesystem::path& bridge, const std::filesystem::path& data, const Theme& theme,
    std::string_view itemId, std::string& error, const std::atomic_bool* cancel)
{
    std::uint64_t id = 0;
    const auto converted = std::from_chars(itemId.data(), itemId.data() + itemId.size(), id);
    if (!Digits(itemId) || converted.ec != std::errc{} || converted.ptr != itemId.data() + itemId.size() || !id)
        return Fail(error, "invalidSelection");
    std::string statusText, itemText;
    if (!preview::Run(bridge, {L"status"}, statusText, 30000, error, cancel) ||
        !preview::Run(bridge, {L"workshop", L"item-details", L"--item", std::wstring(itemId.begin(), itemId.end())}, itemText, 60000, error, cancel)) return false;
    return BindResponses(data, theme, itemId, statusText, itemText, error, cancel);
}
bool BindResponses(const std::filesystem::path& data, const Theme& theme, std::string_view itemId,
    std::string_view statusText, std::string_view itemText, std::string& error, const std::atomic_bool* cancel)
{
    std::uint64_t id = 0;
    const auto converted = std::from_chars(itemId.data(), itemId.data() + itemId.size(), id);
    if (!Digits(itemId) || converted.ec != std::errc{} || converted.ptr != itemId.data() + itemId.size() || !id)
        return Fail(error, "invalidSelection");
    JsonValue statusJson, itemJson;
    if (!Final(statusText, statusJson) || !Boolean(statusJson, "ok") || !Final(itemText, itemJson) ||
        !Boolean(itemJson, "ok") || String(itemJson, "publishedFileId") != itemId)
        return Fail(error, "authorMismatch");
    const auto details = itemJson.Find("details");
    if (!details || !details->IsObject() || Number(*details, "result") != 1 ||
        Number(statusJson, "appId") != kSnowDesktopSteamAppId || Number(*details, "consumerAppId") != kSnowDesktopSteamAppId)
        return Fail(error, "authorMismatch");
    steam_bridge::SteamStatus status; status.loggedOn = Boolean(statusJson, "loggedOn");
    status.appId = kSnowDesktopSteamAppId; status.steamId = String(statusJson, "steamId");
    steam_bridge::PublishedItem item; item.publishedFileId = id; const auto owner = String(*details, "ownerSteamId");
    const auto parsed = std::from_chars(owner.data(), owner.data() + owner.size(), item.ownerSteamId);
    if (!Digits(owner) || parsed.ec != std::errc{} || parsed.ptr != owner.data() + owner.size()) return Fail(error, "authorMismatch");
    item.consumerAppId = kSnowDesktopSteamAppId; item.banned = Boolean(*details, "banned", true);
    item.metadata = String(*details, "metadata");
    const auto tagText = String(*details, "tags"); std::set<std::string> classifications;
    for (std::size_t start = 0; start < tagText.size();)
    { const auto end = tagText.find(',', start); classifications.insert(tagText.substr(start, end - start)); if (end == std::string::npos) break; start = end + 1; }
    for (const auto& tag : tags::Applicable(theme)) if (!classifications.contains(tag)) return Fail(error, "itemMismatch");
    if (cancel && cancel->load()) return Fail(error, "cancelled");
    return steam_bridge::BindThemePublication(data, theme.id, status, item, error);
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
        {
            if (previous->second.sourceIds.empty()) previous->second.sourceIds = RecoverSourceIds(next, previous->second, download.package);
            previous->second.accounts.insert(snapshot.account); continue;
        }
        // Subscription identities must never alias editable local publications,
        // even when their downloaded package is byte-for-byte identical.
        Package isolated;
        std::map<std::string, std::string> remap;
        for (const auto& [id, theme] : download.package)
        {
            if (Builtin(id)) continue;
            if (previous != next.workshop.end())
                for (const auto& [installed, authored] : previous->second.sourceIds)
                    if (authored == id)
                    {
                        const auto old = Find(next.themes, installed);
                        if (old && old->kind == theme.kind) remap.emplace(id, installed);
                        break;
                    }
            if (remap.contains(id)) continue;
            const auto fresh = newId();
            if (fresh.empty() || next.themes.contains(fresh) || download.package.contains(fresh) ||
                std::any_of(remap.begin(), remap.end(), [&](const auto& pair) { return pair.second == fresh; })) return Fail(error, "idConflict");
            remap.emplace(id, fresh);
        }
        for (auto [id, theme] : download.package)
        {
            if (Builtin(id)) continue;
            theme.id = remap.at(id);
            if (remap.contains(theme.quickPanel)) theme.quickPanel = remap.at(theme.quickPanel);
            if (remap.contains(theme.popup)) theme.popup = remap.at(theme.popup);
            isolated.emplace(theme.id, std::move(theme));
        }
        std::set<std::string> retired;
        if (previous != next.workshop.end()) retired = previous->second.ids;
        auto& origin = next.workshop[download.item]; origin.owner = download.owner; origin.sha256 = download.sha256;
        origin.accounts.insert(snapshot.account); origin.ids.clear(); origin.sourceIds.clear();
        for (const auto& [authored, installed] : remap)
        { origin.ids.insert(installed); origin.sourceIds.emplace(installed, authored); retired.erase(installed); }
        for (auto& [id, theme] : isolated) next.themes[id] = std::move(theme);
        if (!RetireInstalled(next, std::move(retired), error, newId)) return false;
        // Stable authored identities retain their isolated installed IDs. Active
        // references still use their immutable snapshots until explicitly applied.
        for (auto& [target, reference] : next.references)
        {
            (void)target;
            if (const auto current = Find(next.themes, reference.id); current && current->kind == Kind::Global &&
                (current->scopes & reference.scope) != reference.scope) reference.id.clear();
        }
    }
    std::set<std::string> removed;
    for (auto it = next.workshop.begin(); it != next.workshop.end();)
    {
        if (known && !snapshot.subscribed.contains(it->first)) it->second.accounts.erase(snapshot.account);
        if (snapshot.deleted.contains(it->first) || it->second.accounts.empty())
        {
            removed.insert(it->second.ids.begin(), it->second.ids.end()); it = next.workshop.erase(it);
        }
        else ++it;
    }
    if (!RetireInstalled(next, std::move(removed), error, newId)) return false;
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
        for (auto& [item, origin] : into.workshop)
        {
            std::set<std::string> local;
            for (const auto& id : origin.ids)
                if (steam_bridge::ThemePublishedUrl(library.parent_path(), id) ==
                    "https://steamcommunity.com/sharedfiles/filedetails/?id=" + item)
                {
                    Package closure;
                    if (!Export(into, id, closure, detail)) return false;
                    for (const auto& [key, theme] : closure) { (void)theme; local.insert(key); }
                }
            if (!local.empty()) origin.sha256 = std::string(64, '0');
            for (const auto& id : local) { origin.ids.erase(id); origin.sourceIds.erase(id); }
        }
        return Reconcile(into, snapshot, detail);
    }, output, error);
}
bool Prepare(const Package& package, std::string_view root, unsigned scope, const std::filesystem::path& directory,
    const std::filesystem::path& data, const std::filesystem::path& customCover, const Renderer& renderer,
    steam_bridge::ThemePublishPlan& plan, std::string& error, const std::atomic_bool* cancel, const std::vector<std::string>& selected)
{
    plan = {}; const auto theme = Resolve(package, root);
    if (!theme || !Validate(package, error) || !renderer) return Fail(error, "invalidPackage");
    if (!tags::Valid(*theme, selected)) return Fail(error, "tagsRequired");
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
    if (!steam_bridge::WriteThemePreparation(directory, root, theme->name, Now(), error, selected) ||
        !steam_bridge::BuildThemePublishPlan(directory, data, plan, error)) return false;
    cleanup.keep = true; return true;
}
bool Publish(const std::filesystem::path& bridge, const steam_bridge::ThemePublishPlan& plan,
    const std::filesystem::path& data, std::string& output, std::string& error, const std::atomic_bool* cancel)
{
    if (!preview::Run(bridge, {L"workshop", L"theme-publish", L"--prepared", plan.directory.wstring(),
        L"--data-directory", data.wstring(), L"--package-sha256", std::wstring(plan.packageSha256.begin(), plan.packageSha256.end()),
        L"--cover-sha256", std::wstring(plan.coverSha256.begin(), plan.coverSha256.end()),
        L"--tags-sha256", std::wstring(plan.tagsSha256.begin(), plan.tagsSha256.end()),
        L"--gallery-sha256", std::wstring(plan.gallerySha256.begin(), plan.gallerySha256.end()),
        plan.publishedFileId ? L"--confirm-update" : L"--confirm-create"}, output, 31 * 60 * 1000, error, cancel))
    { JsonValue failed; if (Final(output, failed) && !String(failed, "error").empty()) error = String(failed, "error"); return false; }
    JsonValue json; return (Final(output, json) && Boolean(json, "ok") && !Boolean(json, "needsLegalAgreement")) || Fail(error, "publishFailed");
}
}
