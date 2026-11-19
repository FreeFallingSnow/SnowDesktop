#include "widget_logical_slot_lua.h"

#include "widget_lua_binding_context.h"
#include "widget_engine.h"

#include <algorithm>
#include <array>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr char kLogicalSlotMetatable[] = "SnowDesktop.LogicalSlot";

struct LuaLogicalSlotHandle
{
    std::uint64_t ownerToken = 0;
    snowdesktop::widget_runtime::LogicalSlotKind kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    std::array<char, 65> id{};
};

LuaLogicalSlotHandle* CheckLogicalSlotHandle(lua_State* state,
    snowdesktop::widget_runtime::LogicalSlotKind* requiredKind = nullptr)
{
    auto* handle = static_cast<LuaLogicalSlotHandle*>(
        luaL_checkudata(state, 1, kLogicalSlotMetatable));
    if (!handle || handle->ownerToken == 0 || handle->id[0] == '\0')
    {
        luaL_error(state, "logical slot handle is closed");
        return nullptr;
    }
    if (requiredKind && handle->kind != *requiredKind)
    {
        luaL_error(state, "logical slot handle has the wrong kind");
        return nullptr;
    }
    return handle;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotSnapshot>
LogicalSlotHandleSnapshot(lua_State* state,
    LuaLogicalSlotHandle* handle)
{
    auto* host = BoundWidgetEngine(state);
    if (!host || !handle) return std::nullopt;
    return host->RuntimeLogicalSlotSnapshot(
        BoundWidgetId(state), handle->ownerToken, handle->id.data(),
        handle->kind);
}

void PushLogicalSlotItem(lua_State* state,
    const snowdesktop::widget_runtime::LogicalSlotItem& item)
{
    lua_createtable(state, 0, 7);
    lua_pushlstring(state, item.id.data(), item.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, item.reference.data(), item.reference.size());
    lua_setfield(state, -2, "reference");
    lua_pushlstring(state, item.kind.data(), item.kind.size());
    lua_setfield(state, -2, "kind");
    lua_pushlstring(state, item.title.data(), item.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, item.source.data(), item.source.size());
    lua_setfield(state, -2, "source");
    lua_pushlstring(state, item.type.data(), item.type.size());
    lua_setfield(state, -2, "type");
    lua_pushstring(state, item.available ? "available" : "unavailable");
    lua_setfield(state, -2, "availability");
}

void PushLogicalSlotChange(lua_State* state,
    const snowdesktop::widget_runtime::LogicalSlotChange& change)
{
    lua_createtable(state, 0, 5);
    lua_pushlstring(state, change.slotId.data(), change.slotId.size());
    lua_setfield(state, -2, "slotId");
    lua_pushstring(state,
        snowdesktop::widget_runtime::LogicalSlotKindName(change.kind));
    lua_setfield(state, -2, "kind");
    lua_pushinteger(state, static_cast<lua_Integer>(change.revision));
    lua_setfield(state, -2, "revision");
    lua_pushlstring(state, change.operation.data(), change.operation.size());
    lua_setfield(state, -2, "operation");
    lua_createtable(state, static_cast<int>(change.itemIds.size()), 0);
    for (std::size_t index = 0; index < change.itemIds.size(); ++index)
    {
        lua_pushlstring(state, change.itemIds[index].data(),
            change.itemIds[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    lua_setfield(state, -2, "itemIds");
    if (!change.relatedSlotId.empty())
    {
        lua_pushlstring(state, change.relatedSlotId.data(),
            change.relatedSlotId.size());
        lua_setfield(state, -2, "relatedSlotId");
        lua_pushinteger(state,
            static_cast<lua_Integer>(change.relatedRevision));
        lua_setfield(state, -2, "relatedRevision");
    }
}

static int lua_LogicalSlotId(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    lua_pushstring(state, handle->id.data());
    return 1;
}

static int lua_LogicalSlotRevision(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_pushinteger(state, static_cast<lua_Integer>(snapshot->revision));
    return 1;
}

static int lua_LogicalSlotState(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    const bool unavailable = std::any_of(snapshot->items.begin(),
        snapshot->items.end(), [](const auto& item) {
            return !item.available;
        });
    lua_pushstring(state, unavailable ? "unavailable" :
        (snapshot->items.empty() ? "empty" : "bound"));
    return 1;
}

static int lua_LogicalSlotCapacity(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_pushinteger(state, static_cast<lua_Integer>(snapshot->capacity));
    return 1;
}

static int lua_LogicalSlotItem(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    if (snapshot->items.empty()) lua_pushnil(state);
    else PushLogicalSlotItem(state, snapshot->items.front());
    return 1;
}

static int lua_LogicalSlotItems(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_createtable(state, static_cast<int>(snapshot->items.size()), 0);
    for (std::size_t index = 0; index < snapshot->items.size(); ++index)
    {
        PushLogicalSlotItem(state, snapshot->items[index]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

static int lua_LogicalSlotBind(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    std::size_t length = 0;
    const char* reference = luaL_checklstring(state, 2, &length);
    if (length == 0 || length > 128)
        return luaL_error(state, "logical slot reference is invalid");
    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeBindLogicalSlot(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(reference, length), change, error))
        return luaL_error(state, "slots.bind: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotClear(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeClearLogicalSlot(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(), change, error))
        return luaL_error(state, "slots.clear: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotRemove(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    std::size_t length = 0;
    const char* itemId = luaL_checklstring(state, 2, &length);
    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeRemoveLogicalSlotItem(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(itemId, length), change, error))
        return luaL_error(state, "slots.remove: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotMove(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    std::size_t length = 0;
    const char* itemId = luaL_checklstring(state, 2, &length);
    const lua_Integer oneBasedIndex = luaL_checkinteger(state, 3);
    if (oneBasedIndex <= 0)
        return luaL_error(state, "slots.move: index must be positive");
    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeMoveLogicalSlotItem(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(itemId, length),
            static_cast<std::size_t>(oneBasedIndex - 1), change, error))
        return luaL_error(state, "slots.move: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotPick(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "logical slot host is unavailable");
    std::string error;
    if (!host->RuntimeOpenHostLogicalSlotPicker(
            BoundWidgetId(state), handle->ownerToken,
            handle->id.data(), error))
        return luaL_error(state, "slots.pick: %s", error.c_str());
    lua_pushboolean(state, 1);
    return 1;
}

// Keep the owning snapshot in a native frame so an undeclared slot
// can raise a Lua error without unwinding its temporary through the C API.
__declspec(noinline) bool IsDeclaredLogicalSlot(WidgetEngine& host,
    lua_State* state, std::uint64_t ownerToken, std::string_view id,
    LogicalSlotKind kind)
{
    return host.RuntimeLogicalSlotSnapshot(
        BoundWidgetId(state), ownerToken, id, kind).has_value();
}

void PushLogicalSlotHandle(lua_State* state,
    snowdesktop::widget_runtime::LogicalSlotKind kind)
{
    std::size_t length = 0;
    const char* id = luaL_checklstring(state, 1, &length);
    if (length == 0 || length > 64)
    {
        luaL_error(state, "logical slot id is invalid");
        return;
    }
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    auto* host = BoundWidgetEngine(state);
    if (!host || ownerToken == 0 ||
        !IsDeclaredLogicalSlot(*host, state, ownerToken,
            std::string_view(id, length), kind))
    {
        luaL_error(state, "logical slot is undeclared or has the wrong kind");
        return;
    }
    auto* handle = static_cast<LuaLogicalSlotHandle*>(
        lua_newuserdatauv(state, sizeof(LuaLogicalSlotHandle), 0));
    *handle = {};
    handle->ownerToken = ownerToken;
    handle->kind = kind;
    std::copy_n(id, length, handle->id.begin());
    handle->id[length] = '\0';
    if (luaL_newmetatable(state, kLogicalSlotMetatable) != 0)
    {
        lua_newtable(state);
        const luaL_Reg methods[] = {
            { "id", lua_LogicalSlotId },
            { "revision", lua_LogicalSlotRevision },
            { "state", lua_LogicalSlotState },
            { "capacity", lua_LogicalSlotCapacity },
            { "item", lua_LogicalSlotItem },
            { "items", lua_LogicalSlotItems },
            { "bind", lua_LogicalSlotBind },
            { "add", lua_LogicalSlotBind },
            { "clear", lua_LogicalSlotClear },
            { "remove", lua_LogicalSlotRemove },
            { "move", lua_LogicalSlotMove },
            { "pick", lua_LogicalSlotPick },
            { nullptr, nullptr },
        };
        luaL_setfuncs(state, methods, 0);
        lua_setfield(state, -2, "__index");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

}

int lua_SlotsBinding(lua_State* state)
{
    PushLogicalSlotHandle(state,
        snowdesktop::widget_runtime::LogicalSlotKind::Binding);
    return 1;
}

int lua_SlotsCollection(lua_State* state)
{
    PushLogicalSlotHandle(state,
        snowdesktop::widget_runtime::LogicalSlotKind::Collection);
    return 1;
}

int lua_SlotsCanUndo(lua_State* state)
{
    auto* host = BoundWidgetEngine(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    lua_pushboolean(state, host && ownerToken != 0 &&
        host->RuntimeCanUndoLogicalSlot(
            BoundWidgetId(state), ownerToken));
    return 1;
}

int lua_SlotsCanRedo(lua_State* state)
{
    auto* host = BoundWidgetEngine(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    lua_pushboolean(state, host && ownerToken != 0 &&
        host->RuntimeCanRedoLogicalSlot(
            BoundWidgetId(state), ownerToken));
    return 1;
}

int lua_SlotsUndo(lua_State* state)
{
    auto* host = BoundWidgetEngine(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    if (!host || ownerToken == 0)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeUndoLogicalSlot(BoundWidgetId(state),
            ownerToken, change, error))
        return luaL_error(state, "slots.undo: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

int lua_SlotsRedo(lua_State* state)
{
    auto* host = BoundWidgetEngine(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    if (!host || ownerToken == 0)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!host->RuntimeRedoLogicalSlot(BoundWidgetId(state),
            ownerToken, change, error))
        return luaL_error(state, "slots.redo: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}
}
