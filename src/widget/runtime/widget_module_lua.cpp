#include "widget_module_lua.h"

#include "common/utils.h"
#include "lua_runtime.h"
#include "widget/packages/widget_package_limits.h"
#include "widget/packages/widget_package_paths.h"

#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
static std::optional<std::wstring> ResolveCurrentPackageAsset(
    lua_State* state, const std::wstring& relativePath)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_package_root");
    size_t length = 0;
    const char* rootRaw = lua_tolstring(state, -1, &length);
    const std::filesystem::path root = rootRaw
        ? Utf8ToWide(std::string(rootRaw, length))
        : std::wstring{};
    lua_pop(state, 1);
    if (root.empty()) return std::nullopt;
    return widget::ResolvePackageAssetWithinRoot(root, relativePath);
}

static char kModuleLoadingMarker = 0;

static void ClearModuleCacheEntry(lua_State* L, int cache,
    const std::string& key)
{
    lua_pushnil(L);
    lua_setfield(L, cache, key.c_str());
}

// Return ordinary validation/load errors before raising them in Lua. Lua uses
// longjmp on this build, so a rejection must not unwind the C++ file/path/string
// locals below. Keep the worker out of the object-free Lua callback's frame.
static __declspec(noinline) int LoadRequiredWidgetModule(lua_State* L,
    const char* pathRaw, size_t pathLength, const char*& error)
{
    const std::wstring relativePath = Utf8ToWide(
        std::string(pathRaw, pathLength));
    if (relativePath.empty() ||
        _wcsicmp(std::filesystem::path(relativePath).extension().c_str(),
            L".lua") != 0)
    {
        error = "module.require: path must name a .lua file";
        return 0;
    }
    const auto fullPath = ResolveCurrentPackageAsset(L, relativePath);
    if (!fullPath)
    {
        error = "module.require: path is outside the component package";
        return 0;
    }

    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(*fullPath, fileError);
    if (fileError || fileSize > snowdesktop::widget::kMaxEntryLuaBytes)
    {
        error = "module.require: module is missing or exceeds the 1 MiB limit";
        return 0;
    }
    const std::string cacheKey = WideToUtf8(*fullPath);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_cache");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_cache");
    }
    const int cache = lua_absindex(L, -1);
    lua_getfield(L, cache, cacheKey.c_str());
    if (lua_touserdata(L, -1) == &kModuleLoadingMarker)
    {
        error = "module.require: circular dependency detected";
        return 0;
    }
    if (!lua_isnil(L, -1))
    {
        lua_remove(L, cache);
        return 1;
    }
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
    const lua_Integer moduleCount = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
    const lua_Integer moduleBytes = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (moduleCount >= 64 || moduleBytes < 0 ||
        moduleBytes > 4 * 1024 * 1024 ||
        fileSize > 4ull * 1024ull * 1024ull -
            static_cast<std::uint64_t>(moduleBytes))
    {
        error = "module.require: per-instance module count or byte quota exceeded";
        return 0;
    }

    lua_pushlightuserdata(L, &kModuleLoadingMarker);
    lua_setfield(L, cache, cacheKey.c_str());
    std::ifstream file(*fullPath, std::ios::binary);
    std::string source(static_cast<std::size_t>(fileSize), '\0');
    if (!file || (fileSize > 0 &&
        !file.read(source.data(), static_cast<std::streamsize>(fileSize))))
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        error = "module.require: module could not be read";
        return 0;
    }
    const std::string chunkName = "@module/" +
        WideToUtf8(std::filesystem::path(relativePath).
            lexically_normal().generic_wstring());
    if (luaL_loadbufferx(L, source.data(), source.size(),
            chunkName.c_str(), "t") != LUA_OK)
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        return 0;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_environment");
    if (!lua_istable(L, -1) || lua_setupvalue(L, -2, 1) == nullptr)
    {
        if (lua_isnil(L, -1)) lua_pop(L, 1);
        ClearModuleCacheEntry(L, cache, cacheKey);
        error = "module.require: sandbox environment is unavailable";
        return 0;
    }
    // Reserve this module before its code can load children. Failed parents
    // release only their own reservation; successful cached children remain.
    lua_pushinteger(L, moduleCount + 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
    lua_pushinteger(L, moduleBytes + static_cast<lua_Integer>(fileSize));
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
    if (snowdesktop::lua_runtime::ProtectedCall(L, 0, 1) != LUA_OK)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
        const lua_Integer currentCount = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
        const lua_Integer currentBytes = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, currentCount - 1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
        lua_pushinteger(L, currentBytes - static_cast<lua_Integer>(fileSize));
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
        ClearModuleCacheEntry(L, cache, cacheKey);
        return 0;
    }
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, cache, cacheKey.c_str());
    lua_remove(L, cache);
    return 1;
}

}

int lua_ModuleRequire(lua_State* L)
{
    size_t pathLength = 0;
    const char* pathRaw = luaL_checklstring(L, 1, &pathLength);
    if (pathLength == 0 || pathLength > 512)
        return luaL_error(L, "module.require: path length is invalid");

    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_loading");
    const bool loading = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!loading)
    {
        return luaL_error(L,
            "module.require: modules can only be loaded while the entry script is loading");
    }

    const char* error = nullptr;
    const int results = LoadRequiredWidgetModule(L, pathRaw, pathLength, error);
    if (results != 0) return results;
    if (error) return luaL_error(L, "%s", error);
    return lua_error(L);
}
}
