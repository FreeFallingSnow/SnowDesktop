#include "widget_resource_lua.h"

#include "common/utils.h"
#include "widget/packages/widget_package.h"
#include "widget/packages/widget_package_image_cache.h"
#include "widget/runtime/widget_runtime_image.h"

#include <string_view>

namespace snowdesktop::widget_runtime
{
namespace
{
std::optional<snowdesktop::widget::PackageResource>
CurrentPackageResource(lua_State* state, const std::string& name,
    const char* expectedType = nullptr)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_resources");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return std::nullopt;
    }
    lua_getfield(state, -1, name.c_str());
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 2);
        return std::nullopt;
    }

    snowdesktop::widget::PackageResource resource;
    auto readField = [&](const char* field, std::string& output) {
        lua_getfield(state, -1, field);
        size_t fieldLength = 0;
        const char* value = lua_tolstring(state, -1, &fieldLength);
        if (value) output.assign(value, fieldLength);
        lua_pop(state, 1);
    };
    readField("type", resource.type);
    readField("path", resource.path);
    readField("license", resource.license);
    lua_pop(state, 2);
    if (resource.type.empty() || resource.path.empty() ||
        (expectedType && resource.type != expectedType))
        return std::nullopt;
    return resource;
}

std::optional<std::wstring> CurrentPackageResourcePath(
    lua_State* state, const std::string& name, const char* expectedType)
{
    if (!CurrentPackageResource(state, name, expectedType))
        return std::nullopt;
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_resource_paths");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return std::nullopt;
    }
    lua_getfield(state, -1, name.c_str());
    size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::optional<std::wstring> path = value
        ? std::optional<std::wstring>(Utf8ToWide(
            std::string(value, length)))
        : std::nullopt;
    lua_pop(state, 2);
    return path;
}

} // namespace

std::optional<std::wstring> ResolveResourceHandlePath(
    lua_State* state, int index, LuaResourceType expected)
{
    const auto* handle = TestResourceHandle(state, index);
    if (!handle || handle->type != expected)
        return std::nullopt;
    const char* type = expected == LuaResourceType::Image
        ? "image" : expected == LuaResourceType::Audio ? "audio" : "font";
    return CurrentPackageResourcePath(state, handle->name, type);
}

static std::optional<std::string> CurrentPackageImageContentKey(
    lua_State* state, const std::string& name)
{
    if (!CurrentPackageResource(state, name, "image"))
        return std::nullopt;
    lua_getfield(state, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return std::nullopt;
    }
    lua_getfield(state, -1, name.c_str());
    size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::optional<std::string> key = value
        ? std::optional<std::string>(std::string(value, length))
        : std::nullopt;
    lua_pop(state, 2);
    return key;
}

std::optional<std::string> ResolveResourceHandleImageContentKey(
    lua_State* state, int index)
{
    const auto* handle = TestResourceHandle(state, index);
    if (!handle || handle->type != LuaResourceType::Image ||
        snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(handle->name))
        return std::nullopt;
    return CurrentPackageImageContentKey(state, handle->name);
}

namespace
{
static bool ResourceCreationAllowed(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_loading");
    const bool loading = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return loading;
}

static int ResourceApiError(
    lua_State* state, const char* functionName, const char* code)
{
    return luaL_error(state, "%s: %s", functionName, code);
}

__declspec(noinline) bool AcquirePackageAudioResource(lua_State* state,
    const char* nameRaw, size_t length)
{
    const std::string name(nameRaw, length);
    if (!CurrentPackageResourcePath(state, name, "audio"))
        return false;
    PushResourceHandle(state, LuaResourceType::Audio, name);
    return true;
}

} // namespace

int lua_ResourceExists(lua_State* L)
{
    size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    const bool exists = length <= 64 &&
        CurrentPackageResourcePath(
            L, std::string(name, length), nullptr).has_value();
    lua_pushboolean(L, exists ? 1 : 0);
    return 1;
}

int lua_ResourceAudio(lua_State* L)
{
    if (!ResourceCreationAllowed(L))
        return ResourceApiError(L, "resource.audio", "loadPhaseRequired");
    size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    if (!length || length > 64 || std::memchr(name, '\0', length))
        return ResourceApiError(L, "resource.audio", "invalidName");
    if (!AcquirePackageAudioResource(L, name, length))
        return ResourceApiError(L, "resource.audio", "notDeclared");
    return 1;
}

namespace
{
// Expected rejections leave this C++ frame normally. The object-free Lua
// callback raises the error afterwards, outside string/path/optional lifetimes.
static __declspec(noinline) int AcquirePackageImageResource(lua_State* L,
    const char* nameRaw, size_t length, bool lazy, bool onDemand,
    const LuaResourceHost& host, const char*& error)
{
    const auto reject = [&error](const char* code) { error = code; return 0; };
    const std::string name(nameRaw, length);
    const auto declaration = CurrentPackageResource(L, name);
    if (!declaration)
        return reject("notDeclared");
    if (declaration->type != "image")
        return reject("typeMismatch");
    auto* cache = host.imageCache(L);
    if (!cache)
        return reject("hostUnavailable");
    const auto existingContentKey =
        CurrentPackageImageContentKey(L, name);
    if (existingContentKey)
    {
        if (onDemand != cache->IsOnDemand(*existingContentKey))
            return reject("invalidOptions");
        PackageImageAcquireError validationError = PackageImageAcquireError::None;
        if (!lazy && !onDemand &&
            !cache->Validate(*existingContentKey, &validationError))
            return reject(validationError == PackageImageAcquireError::AllocationFailed
                ? "unavailable" : "decodeFailed");
        if (!cache->Find(*existingContentKey) ||
            (!lazy && !onDemand && !host.ensureImageBitmap(L, *existingContentKey)))
            return reject("unavailable");
        PushResourceHandle(L, LuaResourceType::Image, name);
        return 1;
    }
    const auto path = CurrentPackageResourcePath(L, name, "image");
    if (!path)
        return reject("unavailable");
    std::string contentKey;
    if (onDemand)
    {
        // Namespace separately from content digests. Each Lua VM binds to
        // its own asset generation, including during same-path hot reload.
        contentKey = "disk:" + std::to_string(reinterpret_cast<std::uintptr_t>(L)) +
            ":" + name;
    }
    else
        contentKey = host.hashFile(*path);
    if (contentKey.empty())
        return reject("unavailable");
    PackageImageAcquireError acquireError = PackageImageAcquireError::None;
    if (onDemand)
    {
        if (!cache->AcquireOnDemand(L, contentKey, *path))
            return reject("unavailable");
    }
    else if (!cache->Acquire(
            L, contentKey, *path, &acquireError, lazy, host.hashBytes))
    {
        return reject(acquireError == PackageImageAcquireError::QuotaExceeded
                ? "quotaExceeded" :
            (acquireError == PackageImageAcquireError::InvalidInput ||
                acquireError == PackageImageAcquireError::AllocationFailed)
                ? "unavailable" : "decodeFailed");
    }
    lua_getfield(L, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return reject("hostUnavailable");
    }
    lua_pushlstring(L, contentKey.data(), contentKey.size());
    lua_setfield(L, -2, name.c_str());
    lua_pop(L, 1);
    if (!lazy && !onDemand && !host.ensureImageBitmap(L, contentKey))
        return reject("deviceUnavailable");
    PushResourceHandle(L, LuaResourceType::Image, name);
    return 1;
}

} // namespace

int LoadLuaPackageImage(lua_State* L, const LuaResourceHost& host)
{
    if (!ResourceCreationAllowed(L))
        return ResourceApiError(L, "resource.image", "loadPhaseRequired");
    bool lazy = false;
    bool onDemand = false;
    if (!lua_isnoneornil(L, 2))
    {
        if (!lua_istable(L, 2))
            return ResourceApiError(L, "resource.image", "invalidOptions");
        lua_pushnil(L);
        while (lua_next(L, 2) != 0)
        {
            const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
            if (!key || !lua_isboolean(L, -1))
                return ResourceApiError(L, "resource.image", "invalidOptions");
            const std::string_view option(key);
            if (option == "lazy") lazy = lua_toboolean(L, -1) != 0;
            else if (option == "onDemand") onDemand = lua_toboolean(L, -1) != 0;
            else return ResourceApiError(L, "resource.image", "invalidOptions");
            lua_pop(L, 1);
        }
    }
    if (lazy && onDemand)
        return ResourceApiError(L, "resource.image", "invalidOptions");
    size_t length = 0;
    const char* nameRaw = luaL_checklstring(L, 1, &length);
    if (length == 0 || length > 64)
        return ResourceApiError(L, "resource.image", "invalidName");
    const char* error = nullptr;
    const int results = AcquirePackageImageResource(L, nameRaw, length, lazy, onDemand, host, error);
    return error ? ResourceApiError(L, "resource.image", error) : results;
}

namespace
{
__declspec(noinline) const char* AcquirePackageFontResource(lua_State* state,
    const char* nameRaw, size_t length, const LuaResourceHost& host)
{
    const std::string name(nameRaw, length);
    const auto declaration = CurrentPackageResource(state, name);
    if (!declaration)
        return "notDeclared";
    if (declaration->type != "font")
        return "typeMismatch";
    if (!host.fontDeviceAvailable(state))
        return "hostUnavailable";
    const auto path = CurrentPackageResourcePath(state, name, "font");
    if (!path)
        return "unavailable";
    if (!host.preloadFont(state, *path))
        return "fontLoadFailed";
    PushResourceHandle(state, LuaResourceType::Font, name);
    return nullptr;
}
} // namespace

int LoadLuaPackageFont(lua_State* state, const LuaResourceHost& host)
{
    if (!ResourceCreationAllowed(state))
        return ResourceApiError(state, "resource.font", "loadPhaseRequired");
    size_t length = 0;
    const char* name = luaL_checklstring(state, 1, &length);
    if (length == 0 || length > 64)
        return ResourceApiError(state, "resource.font", "invalidName");
    if (const char* error = AcquirePackageFontResource(state, name, length, host))
        return ResourceApiError(state, "resource.font", error);
    return 1;
}

namespace
{
__declspec(noinline) int PushResourceStatus(lua_State* L,
    const LuaResourceHandle& handle, const LuaResourceHost& host)
{
    const char* type = handle.type == LuaResourceType::Image
        ? "image" : handle.type == LuaResourceType::Audio ? "audio" : "font";
    const auto* cache = host.imageCache(L);
    const bool runtimeImage = handle.type == LuaResourceType::Image &&
        IsWidgetRuntimeImageToken(handle.name);
    const auto path = runtimeImage
        ? std::optional<std::wstring>{}
        : ResolveResourceHandlePath(L, 1, handle.type);
    const auto imageContentKey = runtimeImage ||
            handle.type != LuaResourceType::Image
        ? std::optional<std::string>{}
        : ResolveResourceHandleImageContentKey(L, 1);
    bool ready = false;
    bool pending = false;
    bool resident = false;
    if (runtimeImage && cache)
    {
        ready = host.hasRuntimeImage(L, handle.name);
        resident = ready;
    }
    else if (path && cache)
    {
        if (handle.type == LuaResourceType::Image && imageContentKey &&
            cache->Find(*imageContentKey))
        {
            ready = cache->Ready(*imageContentKey);
            pending = !ready && !cache->DecodeFailed(*imageContentKey);
            resident = !cache->Find(*imageContentKey)->pixels.empty();
        }
        else
            ready = handle.type == LuaResourceType::Audio ||
                (handle.type == LuaResourceType::Font && host.hasPrivateFont(L, *path));
    }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, ready ? "ready" : pending ? "pending" : "error");
    lua_setfield(L, -2, "state");
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    lua_pushstring(L, handle.name);
    lua_setfield(L, -2, "name");
    if (handle.type == LuaResourceType::Image)
    {
        lua_pushboolean(L, resident);
        lua_setfield(L, -2, "resident");
    }
    if (!ready && !pending)
    {
        lua_pushliteral(L, "unavailable");
        lua_setfield(L, -2, "error");
    }
    return 1;
}
} // namespace

int PushLuaResourceStatus(lua_State* state, const LuaResourceHost& host)
{
    const auto* handle = TestResourceHandle(state, 1);
    if (!handle)
        return ResourceApiError(state, "resource.status", "invalidHandle");
    return PushResourceStatus(state, *handle, host);
}

} // namespace snowdesktop::widget_runtime
