#include "widget_desktop_host.h"

#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "navigation/search_match.h"
#include "navigation/name_pinyin.h"
#include "widget/view/widget_interaction_lua_value.h"

#include <algorithm>
#include <utility>

using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::RequirePermission;

static void PushDesktopItem(lua_State* L, const LuaDesktopItemInfo& item)
{
    lua_createtable(L, 0, 6);
    lua_pushstring(L, item.id.c_str()); lua_setfield(L, -2, "id");
    lua_pushstring(L, item.title.c_str()); lua_setfield(L, -2, "title");
    lua_pushstring(L, item.path.c_str()); lua_setfield(L, -2, "path");
    lua_pushstring(L, item.source.c_str()); lua_setfield(L, -2, "source");
    lua_pushstring(L, item.type.c_str()); lua_setfield(L, -2, "type");
    lua_pushboolean(L, item.selected); lua_setfield(L, -2, "selected");
}

static std::wstring ReadLuaPathArg(lua_State* L, int index)
{
    if (lua_istable(L, index))
    {
        lua_getfield(L, index, "path");
        const char* path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        std::wstring result = Utf8ToWide(path ? path : "");
        lua_pop(L, 1);
        return result;
    }
    const char* path = luaL_checkstring(L, index);
    return Utf8ToWide(path ? path : "");
}

int lua_DesktopItems(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int lua_DesktopSelection(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopSelection();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int lua_DesktopFind(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    std::wstring query = Utf8ToWide(queryRaw ? queryRaw : "");
    const int requestedLimit = static_cast<int>(
        luaL_optinteger(L, 2, 0));
    const size_t resultLimit = requestedLimit <= 0
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(std::clamp(requestedLimit, 1, 1000));

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    if (query.empty())
    {
        const size_t count = std::min(items.size(), resultLimit);
        lua_createtable(L, static_cast<int>(count), 0);
        for (size_t index = 0; index < count; ++index)
        {
            PushDesktopItem(L, items[index]);
            lua_rawseti(L, -2, static_cast<int>(index + 1));
        }
        return 1;
    }

    std::array<std::vector<LuaDesktopItemInfo>,
        kNameSearchNoMatchRank> buckets;
    for (auto& item : items)
    {
        const int rank = NameSearchMatchRank(
            Utf8ToWide(item.title), query);
        if (rank >= 0 && rank < kNameSearchNoMatchRank)
            buckets[static_cast<size_t>(rank)].push_back(
                std::move(item));
    }

    size_t resultCount = 0;
    for (const auto& bucket : buckets)
        resultCount += bucket.size();
    resultCount = std::min(resultCount, resultLimit);
    lua_createtable(L, static_cast<int>(resultCount), 0);
    int i = 1;
    for (const auto& bucket : buckets)
    {
        for (const auto& item : bucket)
        {
            if (static_cast<size_t>(i) > resultCount)
                return 1;
            PushDesktopItem(L, item);
            lua_rawseti(L, -2, i++);
        }
    }
    return 1;
}

int lua_DesktopFindApplications(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    const std::string query = queryRaw ? queryRaw : "";
    int maxResults = static_cast<int>(
        luaL_optinteger(L, 2, 40));
    maxResults = std::clamp(maxResults, 1, 200);

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s && s->engine
        ? s->engine->RuntimeApplicationSearch(
            query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
    lua_createtable(
        L, static_cast<int>(items.size()), 0);
    int index = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int lua_EverythingSearch(lua_State* L)
{
    if (!RequirePermission(L, "everything.search")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    std::string query = queryRaw ? queryRaw : "";
    int maxResults = static_cast<int>(luaL_optinteger(L, 2, 40));
    maxResults = std::clamp(maxResults, 1, 200);

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s && s->engine
        ? s->engine->RuntimeEverythingSearch(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int lua_DesktopOpen(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeOpenDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

int lua_DesktopReveal(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeRevealDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

int lua_DesktopRefresh(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeRefreshDesktop();
    return 0;
}

namespace
{
constexpr char kAppDiscoveryPermission[] = "app.discovery";
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopItems() const
{
    if (snowdesktop::widget_runtime::IsDryLoad() || previewOnly_)
    {
        const std::string document =
            _L("app.widget_preview.api.desktop_document");
        const std::string folder =
            _L("app.widget_preview.api.desktop_folder");
        const std::string shortcut =
            _L("app.widget_preview.api.desktop_shortcut");
        return {
            { "preview-document", document,
                "C:\\Users\\Maya\\Desktop\\" + document,
                "desktop", "file", false },
            { "preview-folder", folder,
                "C:\\Users\\Maya\\Desktop\\" + folder,
                "desktop", "folder", false },
            { "preview-shortcut", shortcut,
                "C:\\Users\\Maya\\Desktop\\" + shortcut + ".lnk",
                "desktop", "shortcut", false },
        };
    }
    return desktopSnapshotProvider_ ? desktopSnapshotProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopSelection() const
{
    if (snowdesktop::widget_runtime::IsDryLoad() || previewOnly_)
    {
        const std::string document =
            _L("app.widget_preview.api.desktop_document");
        return { { "preview-document", document,
            "C:\\Users\\Maya\\Desktop\\" + document,
            "desktop", "file", true } };
    }
    return selectionProvider_ ? selectionProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeApplicationSearch(
    const std::string& query, int maxResults) const
{
    if ((snowdesktop::widget_runtime::IsDryLoad() || previewOnly_) &&
        !query.empty() && maxResults > 0)
        return { { "preview-app",
            _L("app.widget_preview.api.application"),
            "C:\\Program Files\\Travel Journal\\TravelJournal.exe",
            "startmenu", "application", false } };
    return applicationSearchProvider_
        ? applicationSearchProvider_(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeEverythingSearch(const std::string& query, int maxResults) const
{
    if ((snowdesktop::widget_runtime::IsDryLoad() || previewOnly_) &&
        !query.empty() && maxResults > 0)
    {
        const std::string title =
            _L("app.widget_preview.api.search_result");
        return { { "preview-search", title,
            "C:\\Users\\Maya\\Documents\\" + title,
            "everything", "file", false } };
    }
    return everythingSearchProvider_
        ? everythingSearchProvider_(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
}

bool WidgetEngine::RuntimeOpenDesktopPath(const std::wstring& path)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || path.empty()) return false;
    return desktopOpenCallback_ ? desktopOpenCallback_(path) : false;
}

bool WidgetEngine::RuntimeRevealDesktopPath(const std::wstring& path)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || path.empty()) return false;
    return desktopRevealCallback_ ? desktopRevealCallback_(path) : false;
}

std::optional<std::wstring> WidgetEngine::RuntimeResolveItemReference(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    const std::string& reference) const
{
    if (reference.empty() || ownerToken == 0) return std::nullopt;
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end()) return std::nullopt;
    if (const auto item = widget->itemReferences.find(reference);
        item != widget->itemReferences.end())
    {
        if (!item->second.available) return std::nullopt;
        if (!previewOnly_ && !item->second.persistent &&
            item->second.sourceTask == "desktop.search" &&
            item->second.revision != desktopDataRevision_)
            return std::nullopt;
        const std::wstring target = Utf8ToWide(item->second.target);
        return target.empty()
            ? std::nullopt : std::optional<std::wstring>(target);
    }
    if (const auto app = widget->applicationReferences.find(reference);
        app != widget->applicationReferences.end())
    {
        if (!app->second.available) return std::nullopt;
        if (!previewOnly_ && !app->second.persistent &&
            app->second.catalogRevision != appIndexRevision_)
            return std::nullopt;
        const std::wstring target = Utf8ToWide(
            app->second.launchTarget);
        return target.empty()
            ? std::nullopt : std::optional<std::wstring>(target);
    }
    return std::nullopt;
}

std::optional<std::wstring> WidgetEngine::RuntimeResolveViewReference(
    const std::wstring& widgetId, const std::string& reference) const
{
    if (reference.empty()) return std::nullopt;
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end()) return std::nullopt;
    return RuntimeResolveItemReference(
        widgetId, widget->runtimeToken, reference);
}

void WidgetEngine::RuntimeRefreshDesktop()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (desktopRefreshCallback_)
        desktopRefreshCallback_();
}

void WidgetEngine::NotifyDesktopChanged(const std::string& reason)
{
    ++desktopDataRevision_;
    desktopDataTimestampMs_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    desktopDataChangeReason_ = reason.substr(0, 64);
    if (desktopDataChangeReason_.empty())
        desktopDataChangeReason_ = "changed";
    if (reason == "applications")
    {
        ++appIndexRevision_;
        for (const auto& widget : widgets_)
        {
            if (!widget.valid || widget.preview || !dataBroker_ ||
                !RuntimeHasPermission(
                    widget.widgetId, kAppDiscoveryPermission))
                continue;
            const bool subscribed = std::any_of(
                widget.dataSubscriptions.begin(),
                widget.dataSubscriptions.end(),
                [this](const auto& entry) {
                    if (entry.second != "app.indexStatus") return false;
                    const auto binding =
                        dataBroker_->SubscriptionSnapshot(entry.first);
                    return binding && binding->eligible;
                });
            if (subscribed)
                RuntimeInvalidateHost(widget.widgetId);
        }
    }
    RefreshLogicalSlotAvailability();
    if (d2dState_)
    {
        ClearShellIconBitmaps(d2dState_);
        d2dState_->shellIconFailures.clear();
    }
    std::vector<std::wstring> targets;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || !RuntimeHasPermission(widget.widgetId, "desktop.read"))
            continue;
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        lua_State* state = widgets_[index].state;
        if (!state) continue;
        const int widgetRef = widgets_[index].ref;
        const RECT bounds = widgets_[index].lastBounds;
        WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, bounds);
        lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
        if (!lua_istable(state, -1)) { lua_pop(state, 1); continue; }
        lua_getfield(state, -1, "onDesktopChanged");
        if (lua_isfunction(state, -1))
        {
            lua_pushstring(state, reason.c_str());
            if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
            {
                const char* err = lua_tostring(state, -1);
                RuntimeRecordError(widgetId, err ? err : "(onDesktopChanged error)");
                lua_pop(state, 1);
            }
        }
        else
        {
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        const bool subscribed = dataBroker_ && std::any_of(
            widgets_[index].dataSubscriptions.begin(),
            widgets_[index].dataSubscriptions.end(),
            [this](const auto& entry) {
                if (!entry.second.starts_with("desktop.")) return false;
                const auto binding =
                    dataBroker_->SubscriptionSnapshot(entry.first);
                return binding && binding->eligible;
            });
        if (subscribed)
            RuntimeInvalidateHost(widgetId);
    }
}
