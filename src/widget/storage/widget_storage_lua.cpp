#include "widget_storage_lua.h"

#include "widget/runtime/lua_runtime.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget_storage_runtime.h"
#include "widget_storage_transaction.h"
#include "widget_storage_value.h"
#include "widget_storage_write_budget.h"

#include <utility>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
using lua_runtime::DeferLuaError;

namespace
{
const char kWidgetStorageLuaHostKey = 0;

const WidgetStorageLuaHost* StorageHost(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kWidgetStorageLuaHostKey);
    const auto* host = static_cast<const WidgetStorageLuaHost*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return host;
}

std::string BoundStoragePrefix(lua_State* state)
{
    const auto* host = StorageHost(state);
    return host ? host->prefix(state) : std::string{};
}
}

void BindWidgetStorageContext(lua_State* state, const WidgetStorageLuaHost* host)
{
    lua_pushlightuserdata(state, const_cast<WidgetStorageLuaHost*>(host));
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kWidgetStorageLuaHostKey);
}

namespace
{
constexpr char kStorageTransactionMetatable[] =
    "SnowDesktop.StorageTransaction";
char kActiveStorageTransactionRegistryKey = 0;

struct LuaStorageTransactionHandle
{
    snowdesktop::widget_runtime::WidgetStorageTransaction* transaction =
        nullptr;
};

static LuaStorageTransactionHandle* ActiveLuaStorageTransactionHandle(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX,
        &kActiveStorageTransactionRegistryKey);
    auto* handle = static_cast<LuaStorageTransactionHandle*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return handle;
}

static snowdesktop::widget_runtime::WidgetStorageTransaction*
ActiveLuaStorageTransaction(lua_State* state)
{
    const auto* handle = ActiveLuaStorageTransactionHandle(state);
    return handle ? handle->transaction : nullptr;
}

static void ClearActiveLuaStorageTransaction(lua_State* state)
{
    auto* handle = ActiveLuaStorageTransactionHandle(state);
    if (!handle) return;
    handle->transaction = nullptr;
    lua_pushnil(state);
    lua_rawsetp(state, LUA_REGISTRYINDEX,
        &kActiveStorageTransactionRegistryKey);
}

static std::string ReadExactStorageString(lua_State* state, int index,
    const char* api, const char* field)
{
    if (lua_type(state, index) != LUA_TSTRING)
    {
        luaL_error(state, "%s: %s must be a string", api, field);
        return {};
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    return std::string(value ? value : "", length);
}

static bool ReadTypedStorageValue(lua_State* state, int index,
    std::string& encoded, bool& typed, std::string& error)
{
    typed = false;
    error.clear();
    if (lua_type(state, index) == LUA_TSTRING)
    {
        encoded = ReadExactStorageString(
            state, index, "storage.set", "value");
        return true;
    }
    snowdesktop::widget_runtime::InteractionValue value;
    std::size_t nodes = 0;
    std::size_t stringBytes = 0;
    std::unordered_set<const void*> ancestors;
    if (!ReadInteractionValue(state, index, value, 0,
            nodes, stringBytes, ancestors, error))
        return false;
    typed = true;
    return snowdesktop::widget_runtime::EncodeTypedStorageValue(
        value, encoded, error);
}

static int PushStoredStorageValue(lua_State* state,
    std::string_view encoded, bool typed, const char* api)
{
    if (!typed)
    {
        lua_pushlstring(state, encoded.data(), encoded.size());
        return 1;
    }
    snowdesktop::widget_runtime::InteractionValue value;
    std::string error;
    if (!snowdesktop::widget_runtime::DecodeTypedStorageValue(
            encoded, value, error))
    {
        return luaL_error(state, "%s: persisted typed value is invalid: %s",
            api, error.c_str());
    }
    PushInteractionValue(state, value);
    return 1;
}

static LuaStorageTransactionHandle* CheckStorageTransactionHandle(
    lua_State* state)
{
    auto* handle = static_cast<LuaStorageTransactionHandle*>(
        luaL_checkudata(state, 1, kStorageTransactionMetatable));
    if (!handle || !handle->transaction ||
        ActiveLuaStorageTransactionHandle(state) != handle)
    {
        luaL_error(state, "storage transaction is closed");
        return nullptr;
    }
    return handle;
}

static bool IsReservedStorageTransactionKey(const std::string& key)
{
    return IsRemovedPanelEffectSettingKey(key) ||
        key == "__host" || key.starts_with("__host.");
}

static bool BoundSecretSetting(lua_State* state, const std::string& key,
    std::string* reference = nullptr)
{
    const auto* host = StorageHost(state);
    std::string value;
    const bool secret = host && host->secretReference(state, key, value);
    if (secret && reference) *reference = std::move(value);
    return secret;
}

static bool BoundFilesystemHandleSetting(lua_State* state,
    const std::string& key, std::string* handle = nullptr)
{
    const auto* host = StorageHost(state);
    std::string value;
    const bool declared = host && host->filesystemHandle(state, key, value);
    if (declared && handle) *handle = std::move(value);
    return declared;
}

static bool BoundEntityReferenceSetting(lua_State* state,
    const std::string& key)
{
    const auto* host = StorageHost(state);
    return host && host->entityReference(state, key);
}

static int RejectHostManagedSettingWrite(lua_State* state,
    const std::string& key, const char* api)
{
    if (BoundSecretSetting(state, key))
        return luaL_error(state,
            "%s: password settings are host-managed and read-only to Lua",
            api);
    if (BoundFilesystemHandleSetting(state, key))
        return luaL_error(state,
            "%s: filesystem handle settings are host-managed and read-only to Lua",
            api);
    if (BoundEntityReferenceSetting(state, key))
        return luaL_error(state,
            "%s: entity reference settings are host-managed and read-only to Lua",
            api);
    return 0;
}

static int lua_StorageTransactionGet(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    const std::string key = ReadExactStorageString(
        state, 2, "storage.transaction", "key");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    std::string secretReference;
    if (BoundSecretSetting(state, key, &secretReference))
    {
        if (secretReference.empty()) lua_pushnil(state);
        else lua_pushlstring(state, secretReference.data(),
            secretReference.size());
        return 1;
    }
    std::string filesystemHandle;
    if (BoundFilesystemHandleSetting(state, key, &filesystemHandle))
    {
        if (filesystemHandle.empty()) lua_pushnil(state);
        else lua_pushlstring(state, filesystemHandle.data(),
            filesystemHandle.size());
        return 1;
    }
    if (BoundEntityReferenceSetting(state, key))
    {
        lua_pushnil(state);
        return 1;
    }
    std::string error;
    const auto value = handle->transaction->Get(key, error);
    if (!error.empty())
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    if (!value)
    {
        lua_pushnil(state);
        return 1;
    }
    const auto marker = handle->transaction->GetHostMetadata(
        snowdesktop::widget_runtime::TypedStorageMetadataKey(key), error);
    if (!error.empty())
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    return PushStoredStorageValue(state, *value,
        marker == snowdesktop::widget_runtime::TypedStorageMarker,
        "storage.transaction");
}

static int lua_StorageTransactionSet(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    std::string key = ReadExactStorageString(
        state, 2, "storage.transaction", "key");
    std::string value;
    bool typed = false;
    std::string valueError;
    if (!ReadTypedStorageValue(
            state, 3, value, typed, valueError))
    {
        return luaL_error(state,
            "storage.transaction: value must be bounded JSON-like data: %s",
            valueError.c_str());
    }
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    if (const int rejected = RejectHostManagedSettingWrite(
            state, key, "storage.transaction"))
        return rejected;
    bool valueChanged = false;
    std::string error;
    if (!handle->transaction->Set(
            key, std::move(value), valueChanged, error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    const std::string metadataKey =
        snowdesktop::widget_runtime::TypedStorageMetadataKey(key);
    bool metadataChanged = false;
    const bool metadataOk = typed
        ? handle->transaction->SetHostMetadata(metadataKey,
            std::string(snowdesktop::widget_runtime::TypedStorageMarker),
            metadataChanged, error)
        : handle->transaction->RemoveHostMetadata(metadataKey,
            metadataChanged, error);
    if (!metadataOk)
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    lua_pushboolean(state,
        valueChanged || metadataChanged ? 1 : 0);
    return 1;
}

static int lua_StorageTransactionRemove(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    const std::string key = ReadExactStorageString(
        state, 2, "storage.transaction", "key");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    if (const int rejected = RejectHostManagedSettingWrite(
            state, key, "storage.transaction"))
        return rejected;
    bool valueChanged = false;
    std::string error;
    if (!handle->transaction->Remove(key, valueChanged, error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    bool metadataChanged = false;
    if (!handle->transaction->RemoveHostMetadata(
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key),
            metadataChanged, error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    lua_pushboolean(state,
        valueChanged || metadataChanged ? 1 : 0);
    return 1;
}

static void PushStorageTransactionHandle(lua_State* state,
    snowdesktop::widget_runtime::WidgetStorageTransaction* transaction)
{
    auto* handle = static_cast<LuaStorageTransactionHandle*>(
        lua_newuserdatauv(state, sizeof(LuaStorageTransactionHandle), 0));
    handle->transaction = transaction;
    luaL_getmetatable(state, kStorageTransactionMetatable);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_createtable(state, 0, 2);
        lua_newtable(state);
        lua_pushcfunction(state, lua_StorageTransactionGet);
        lua_setfield(state, -2, "get");
        lua_pushcfunction(state, lua_StorageTransactionSet);
        lua_setfield(state, -2, "set");
        lua_pushcfunction(state, lua_StorageTransactionRemove);
        lua_setfield(state, -2, "remove");
        lua_setfield(state, -2, "__index");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
        // Publish only a complete metatable. An allocation failure above must
        // not leave a half-initialized handle type for the next transaction.
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, kStorageTransactionMetatable);
    }
    lua_setmetatable(state, -2);
}

static int RejectStorageAccessInsideTransaction(lua_State* state,
    const char* api)
{
    if (ActiveLuaStorageTransaction(state))
    {
        return luaL_error(state,
            "%s cannot be used inside storage.transaction; use tx instead",
            api);
    }
    return 0;
}

static int RejectStorageWriteDuringRender(lua_State* state,
    const char* api)
{
    const auto* host = StorageHost(state);
    if (host && !host->canWrite(state))
    {
        return luaL_error(state,
            "%s cannot write persistent storage during render", api);
    }
    return 0;
}

static int RejectStorageWriteFrequency(lua_State* state, const char* api)
{
    if (snowdesktop::widget_runtime::HasStorageOverlay()) return 0;
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_preview");
    const bool preview = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    if (preview) return 0;

    lua_getfield(state, LUA_REGISTRYINDEX,
        "__storage_write_budget_ptr");
    auto* budget = static_cast<
        snowdesktop::widget_runtime::WidgetStorageWriteBudget*>(
            lua_touserdata(state, -1));
    lua_pop(state, 1);
    if (!budget) return 0;
    const auto decision = budget->Consume(
        snowdesktop::widget_runtime::WidgetStorageWriteBudget::Clock::now());
    if (decision.allowed) return 0;
    return luaL_error(state,
        "%s: persistent storage write frequency quota exceeded; retry after %llu ms",
        api, static_cast<unsigned long long>(decision.retryAfter.count()));
}
}

int lua_StorageGet(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.get"))
        return rejected;
    const std::string key = ReadExactStorageString(
        L, 1, "storage.get", "key");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(L, "storage.get: key is reserved");
    const auto* host = StorageHost(L);
    if (!host) { lua_pushnil(L); return 1; }
    std::string secretReference;
    if (BoundSecretSetting(L, key, &secretReference))
    {
        if (secretReference.empty()) lua_pushnil(L);
        else lua_pushlstring(L, secretReference.data(),
            secretReference.size());
        return 1;
    }
    std::string filesystemHandle;
    if (BoundFilesystemHandleSetting(L, key, &filesystemHandle))
    {
        if (filesystemHandle.empty()) lua_pushnil(L);
        else lua_pushlstring(L, filesystemHandle.data(),
            filesystemHandle.size());
        return 1;
    }
    if (BoundEntityReferenceSetting(L, key))
    {
        lua_pushnil(L);
        return 1;
    }
    const std::string prefix = BoundStoragePrefix(L);
    const auto& storage = ReadStorage();
    const auto value = storage.find(prefix + "." + key);
    if (value != storage.end())
    {
        const auto marker = storage.find(prefix + "." +
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
        return PushStoredStorageValue(L, value->second,
            marker != storage.end() &&
                marker->second == snowdesktop::widget_runtime::
                    TypedStorageMarker,
            "storage.get");
    }
    InteractionValue typedDefault;
    if (host->typedDefault(L, key, typedDefault))
    {
        PushInteractionValue(L, typedDefault);
        return 1;
    }
    const std::string defaultValue = host->defaultValue(L, key);
    if (!defaultValue.empty())
        lua_pushstring(L, defaultValue.c_str());
    else
        lua_pushnil(L);
    return 1;
}

int lua_StorageSet(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.set"))
        return rejected;
    if (const int rejected = RejectStorageWriteDuringRender(
            L, "storage.set"))
        return rejected;
    std::string key = ReadExactStorageString(
        L, 1, "storage.set", "key");
    std::string value;
    bool typed = false;
    std::string valueError;
    if (!ReadTypedStorageValue(L, 2, value, typed, valueError))
    {
        return luaL_error(L,
            "storage.set: value must be bounded JSON-like data: %s",
            valueError.c_str());
    }
    const auto* host = StorageHost(L);
    if (!host) return 0;
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(L, "storage.set: key is reserved");
    if (const int rejected = RejectHostManagedSettingWrite(
            L, key, "storage.set"))
        return rejected;
    const std::string prefix = BoundStoragePrefix(L);
    auto& storage = ActiveStorage();
    snowdesktop::widget_runtime::WidgetStorageTransaction transaction(
        storage, prefix);
    bool changed = false;
    std::string error;
    if (!transaction.Set(key, std::move(value), changed, error))
        return luaL_error(L, "storage.set: %s", error.c_str());
    const std::string metadataKey =
        snowdesktop::widget_runtime::TypedStorageMetadataKey(key);
    bool metadataChanged = false;
    const bool metadataOk = typed
        ? transaction.SetHostMetadata(metadataKey,
            std::string(snowdesktop::widget_runtime::TypedStorageMarker),
            metadataChanged, error)
        : transaction.RemoveHostMetadata(metadataKey,
            metadataChanged, error);
    if (!metadataOk || !transaction.ValidateCommit(error))
        return luaL_error(L, "storage.set: %s", error.c_str());
    if (!transaction.Changed()) return 0;
    if (const int rejected = RejectStorageWriteFrequency(L, "storage.set"))
        return rejected;
    auto candidate = transaction.TakeCandidate();
    storage.swap(candidate);
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage.swap(candidate);
        return luaL_error(L, "storage.set: failed to persist storage");
    }
    return 0;
}

int lua_StorageRemove(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.remove"))
        return rejected;
    if (const int rejected = RejectStorageWriteDuringRender(
            L, "storage.remove"))
        return rejected;
    const std::string key = ReadExactStorageString(
        L, 1, "storage.remove", "key");
    const auto* host = StorageHost(L);
    if (!host) return 0;
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(L, "storage.remove: key is reserved");
    if (const int rejected = RejectHostManagedSettingWrite(
            L, key, "storage.remove"))
        return rejected;
    const std::string prefix = BoundStoragePrefix(L);
    auto& storage = ActiveStorage();
    snowdesktop::widget_runtime::WidgetStorageTransaction transaction(
        storage, prefix);
    bool changed = false;
    std::string error;
    if (!transaction.Remove(key, changed, error))
        return luaL_error(L, "storage.remove: %s", error.c_str());
    bool metadataChanged = false;
    if (!transaction.RemoveHostMetadata(
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key),
            metadataChanged, error) || !transaction.ValidateCommit(error))
        return luaL_error(L, "storage.remove: %s", error.c_str());
    if (!transaction.Changed()) return 0;
    if (const int rejected = RejectStorageWriteFrequency(
            L, "storage.remove"))
        return rejected;
    auto candidate = transaction.TakeCandidate();
    storage.swap(candidate);
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage.swap(candidate);
        return luaL_error(L, "storage.remove: failed to persist storage");
    }
    return 0;
}

int lua_StorageKeys(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.keys"))
        return rejected;
    const auto* host = StorageHost(L);
    std::string prefix = host ? BoundStoragePrefix(L) + "." : "";
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : ReadStorage())
    {
        if (!prefix.empty() && kv.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string key = prefix.empty() ? kv.first : kv.first.substr(prefix.size());
        if (IsReservedStorageTransactionKey(key)) continue;
        if (BoundSecretSetting(L, key) ||
            BoundFilesystemHandleSetting(L, key) ||
            BoundEntityReferenceSetting(L, key))
            continue;
        lua_pushstring(L, key.c_str());
        lua_rawseti(L, -2, idx++);
    }
    if (host)
    {
        for (const auto& key : host->managedKeys(L))
        {
            lua_pushlstring(L, key.data(), key.size());
            lua_rawseti(L, -2, idx++);
        }
    }
    return 1;
}

static int LuaStorageTransactionWithDeferredError(lua_State* state)
{
    if (lua_gettop(state) != 1 || !lua_isfunction(state, 1))
        return luaL_error(state,
            "storage.transaction: expected one callback function");
    if (ActiveLuaStorageTransaction(state))
        return luaL_error(state,
            "storage.transaction: nested transactions are not supported");
    if (const int rejected = RejectStorageWriteDuringRender(
            state, "storage.transaction"))
        return rejected;

    auto& activeStorage = ActiveStorage();
    snowdesktop::widget_runtime::WidgetStorageTransaction transaction(
        activeStorage, BoundStoragePrefix(state));
    std::string error;
    const bool callbackSucceeded = snowdesktop::lua_runtime::ProtectedHostCall(state,
        +[](lua_State* inner) {
            auto* active = static_cast<snowdesktop::widget_runtime::WidgetStorageTransaction*>(
                lua_touserdata(inner, 1));
            lua_pushvalue(inner, 2);
            PushStorageTransactionHandle(inner, active);
            // Root the actual userdata, not the stack transaction address.
            // Its identity cannot be reused while an old handle is retained.
            lua_pushvalue(inner, -1);
            lua_rawsetp(inner, LUA_REGISTRYINDEX, &kActiveStorageTransactionRegistryKey);
            lua_call(inner, 1, 0);
            return 0;
        }, &transaction, 0, error, 1);
    ClearActiveLuaStorageTransaction(state);
    if (!callbackSucceeded) return DeferLuaError(state, error);

    if (!transaction.ValidateCommit(error))
        return DeferLuaError(state, "storage.transaction: " + error);
    const bool changed = transaction.Changed();
    if (!changed)
    {
        lua_pushboolean(state, 0);
        return 1;
    }
    if (!snowdesktop::lua_runtime::ProtectedHostCall(state,
            +[](lua_State* inner) {
                return RejectStorageWriteFrequency(inner, "storage.transaction");
            }, nullptr, 0, error))
        return DeferLuaError(state, error);

    auto candidate = transaction.TakeCandidate();
    activeStorage.swap(candidate);
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        activeStorage.swap(candidate);
        return DeferLuaError(state,
            "storage.transaction: failed to persist storage");
    }
    lua_pushboolean(state, 1);
    return 1;
}

int lua_StorageTransaction(lua_State* state)
{
    const int result = LuaStorageTransactionWithDeferredError(state);
    return result < 0 ? lua_error(state) : result;
}

}
