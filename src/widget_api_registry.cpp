#include "widget_api_registry.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_api
{
namespace
{
constexpr std::uint32_t kCurrentApiVersion = 2;
constexpr std::array<std::string_view, 8> kHostFeatures = {
    "draw.immediate",
    "l10n.basic",
    "l10n.format",
    "module.package",
    "system.environment",
    "system.uptime",
    "time.basic",
    "time.calendar",
};
char kDefinedWidgetMarker = 0;

bool FieldIsNilOrFunction(lua_State* state, int tableIndex,
    const char* field)
{
    lua_getfield(state, tableIndex, field);
    const bool valid = lua_isnil(state, -1) || lua_isfunction(state, -1);
    lua_pop(state, 1);
    return valid;
}

bool HasNonNilField(lua_State* state, int tableIndex, const char* field)
{
    lua_getfield(state, tableIndex, field);
    const bool present = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return present;
}
}

std::span<const std::string_view> HostFeatures() noexcept
{
    return kHostFeatures;
}

bool SupportsFeature(std::string_view feature) noexcept
{
    return std::find(kHostFeatures.begin(), kHostFeatures.end(), feature) !=
        kHostFeatures.end();
}

std::vector<std::string> MissingFeatures(
    std::span<const std::string> requiredFeatures)
{
    std::vector<std::string> result;
    for (const auto& feature : requiredFeatures)
    {
        if (!SupportsFeature(feature) &&
            std::find(result.begin(), result.end(), feature) == result.end())
            result.push_back(feature);
    }
    return result;
}

int LuaDefineWidget(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    for (const char* callback : {
        "render", "view", "setup", "event", "dispose", "menu" })
    {
        if (!FieldIsNilOrFunction(state, descriptor, callback))
        {
            return luaL_error(state,
                "widget.define: '%s' must be a function when present",
                callback);
        }
    }

    const bool hasRender = HasNonNilField(state, descriptor, "render");
    const bool hasView = HasNonNilField(state, descriptor, "view");
    if (hasRender == hasView)
    {
        return luaL_error(state,
            "widget.define: choose exactly one of 'render' or 'view'");
    }
    if (hasView && !SupportsFeature("view.tree"))
    {
        return luaL_error(state,
            "widget.define: unsupported host feature 'view.tree'");
    }
    for (const char* pending : { "setup", "event", "dispose", "menu" })
    {
        if (HasNonNilField(state, descriptor, pending))
        {
            return luaL_error(state,
                "widget.define: lifecycle callback '%s' is not available in this host build",
                pending);
        }
    }

    lua_pushlightuserdata(state, &kDefinedWidgetMarker);
    lua_pushboolean(state, 1);
    lua_rawset(state, descriptor);
    lua_settop(state, 1);
    return 1;
}

int LuaApiInfo(lua_State* state)
{
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_setfield(state, -2, "current");
    lua_createtable(state, 1, 0);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_rawseti(state, -2, 1);
    lua_setfield(state, -2, "supported");
    lua_createtable(state, static_cast<int>(kHostFeatures.size()), 0);
    int index = 1;
    for (const auto feature : kHostFeatures)
    {
        lua_pushlstring(state, feature.data(), feature.size());
        lua_rawseti(state, -2, index++);
    }
    lua_setfield(state, -2, "features");
    return 1;
}

int LuaHasFeature(lua_State* state)
{
    std::size_t length = 0;
    const char* feature = luaL_checklstring(state, 1, &length);
    lua_pushboolean(state,
        SupportsFeature(std::string_view(feature, length)) ? 1 : 0);
    return 1;
}

int LuaSystemCapabilities(lua_State* state)
{
    if (!lua_isnoneornil(state, 1))
    {
        std::size_t length = 0;
        const char* value = luaL_checklstring(state, 1, &length);
        const std::string_view feature(value, length);
        const bool available = SupportsFeature(feature);
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, feature.data(), feature.size());
        lua_setfield(state, -2, "id");
        lua_pushboolean(state, available ? 1 : 0);
        lua_setfield(state, -2, "available");
        if (available)
        {
            lua_pushinteger(state, 1);
            lua_setfield(state, -2, "version");
        }
        else
        {
            lua_pushliteral(state, "unsupported");
            lua_setfield(state, -2, "reason");
        }
        return 1;
    }

    lua_createtable(state, 0, 2);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_setfield(state, -2, "apiVersion");
    lua_createtable(state, static_cast<int>(kHostFeatures.size()), 0);
    int index = 1;
    for (const auto feature : kHostFeatures)
    {
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, feature.data(), feature.size());
        lua_setfield(state, -2, "id");
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "available");
        lua_pushinteger(state, 1);
        lua_setfield(state, -2, "version");
        lua_rawseti(state, -2, index++);
    }
    lua_setfield(state, -2, "features");
    return 1;
}

bool IsDefinedWidget(lua_State* state, int index) noexcept
{
    if (!state || !lua_istable(state, index)) return false;
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &kDefinedWidgetMarker);
    lua_rawget(state, index);
    const bool defined = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return defined;
}

LibraryValidationError ValidateLibrary(
    const char* libraryName,
    std::span<const FunctionDescriptor> functions) noexcept
{
    if (!libraryName || libraryName[0] == '\0')
        return LibraryValidationError::MissingLibraryName;

    for (std::size_t index = 0; index < functions.size(); ++index)
    {
        const FunctionDescriptor& function = functions[index];
        if (!function.name || function.name[0] == '\0')
            return LibraryValidationError::MissingFunctionName;
        if (!function.callback)
            return LibraryValidationError::MissingCallback;
        if (function.sinceApi == 0)
            return LibraryValidationError::InvalidApiVersion;
        if (function.requiredPermission &&
            function.requiredPermission[0] == '\0')
        {
            return LibraryValidationError::EmptyRequiredPermission;
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(functions[previous].name) ==
                function.name)
            {
                return LibraryValidationError::DuplicateFunctionName;
            }
        }
    }
    return LibraryValidationError::None;
}

const char* DescribeValidationError(
    LibraryValidationError error) noexcept
{
    switch (error)
    {
    case LibraryValidationError::None:
        return "none";
    case LibraryValidationError::MissingLibraryName:
        return "missing library name";
    case LibraryValidationError::MissingFunctionName:
        return "missing function name";
    case LibraryValidationError::MissingCallback:
        return "missing callback";
    case LibraryValidationError::InvalidApiVersion:
        return "invalid API version";
    case LibraryValidationError::EmptyRequiredPermission:
        return "empty required permission";
    case LibraryValidationError::DuplicateFunctionName:
        return "duplicate function name";
    }
    return "unknown validation error";
}

CatalogValidationResult ValidateCatalog(
    std::span<const LibraryDescriptor> libraries) noexcept
{
    for (std::size_t index = 0; index < libraries.size(); ++index)
    {
        const LibraryDescriptor& library = libraries[index];
        const LibraryValidationError libraryError =
            ValidateLibrary(library.name, library.functions);
        if (libraryError != LibraryValidationError::None)
        {
            return {
                CatalogValidationError::InvalidLibrary,
                index,
                libraryError,
            };
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(libraries[previous].name) ==
                library.name)
            {
                return {
                    CatalogValidationError::DuplicateLibraryName,
                    index,
                    LibraryValidationError::None,
                };
            }
        }
    }
    return {};
}

const char* DescribeValidationError(
    CatalogValidationError error) noexcept
{
    switch (error)
    {
    case CatalogValidationError::None:
        return "none";
    case CatalogValidationError::InvalidLibrary:
        return "invalid library";
    case CatalogValidationError::DuplicateLibraryName:
        return "duplicate library name";
    }
    return "unknown validation error";
}

const FunctionDescriptor* FindFunction(
    std::span<const LibraryDescriptor> libraries,
    std::string_view libraryName,
    std::string_view functionName) noexcept
{
    if (libraryName.empty() || functionName.empty())
        return nullptr;

    for (const LibraryDescriptor& library : libraries)
    {
        if (!library.name || std::string_view(library.name) != libraryName)
            continue;
        for (const FunctionDescriptor& function : library.functions)
        {
            if (function.name &&
                std::string_view(function.name) == functionName)
            {
                return &function;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions)
{
    RegisterLibrary(state, libraryName, functions,
        (std::numeric_limits<std::uint32_t>::max)());
}

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions,
    std::uint32_t apiVersion)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API library on a null Lua state");

    const LibraryValidationError validation =
        ValidateLibrary(libraryName, functions);
    if (validation != LibraryValidationError::None)
    {
        throw std::invalid_argument(
            std::string("invalid widget API library '") +
            (libraryName ? libraryName : "") + "': " +
            DescribeValidationError(validation));
    }

    const int entryTop = lua_gettop(state);
    const auto exposed = std::count_if(functions.begin(), functions.end(),
        [apiVersion](const FunctionDescriptor& function) {
            return function.sinceApi <= apiVersion;
        });
    lua_createtable(state, 0, static_cast<int>(exposed));
    for (const FunctionDescriptor& function : functions)
    {
        if (function.sinceApi > apiVersion) continue;
        lua_pushcfunction(state, function.callback);
        lua_setfield(state, -2, function.name);
    }
    lua_setglobal(state, libraryName);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API registration did not preserve the Lua stack");
    }
}

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries)
{
    RegisterLibraries(state, libraries,
        (std::numeric_limits<std::uint32_t>::max)());
}

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries,
    std::uint32_t apiVersion)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API catalog on a null Lua state");

    const CatalogValidationResult validation =
        ValidateCatalog(libraries);
    if (validation.error != CatalogValidationError::None)
    {
        const LibraryDescriptor& library =
            libraries[validation.libraryIndex];
        std::string message =
            std::string("invalid widget API catalog at library '") +
            (library.name ? library.name : "") + "': " +
            DescribeValidationError(validation.error);
        if (validation.libraryError != LibraryValidationError::None)
        {
            message += ": ";
            message += DescribeValidationError(validation.libraryError);
        }
        throw std::invalid_argument(message);
    }

    const int entryTop = lua_gettop(state);
    for (const LibraryDescriptor& library : libraries)
        RegisterLibrary(state, library.name, library.functions, apiVersion);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API catalog registration did not preserve the Lua stack");
    }
}
}
