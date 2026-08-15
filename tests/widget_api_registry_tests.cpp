#include "widget_api_registry.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

namespace
{
using snowdesktop::widget_api::FunctionDescriptor;
using snowdesktop::widget_api::CatalogValidationError;
using snowdesktop::widget_api::LibraryDescriptor;
using snowdesktop::widget_api::LibraryValidationError;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int ReturnFortyTwo(lua_State* state)
{
    lua_pushinteger(state, 42);
    return 1;
}

int Add(lua_State* state)
{
    const lua_Integer left = luaL_checkinteger(state, 1);
    const lua_Integer right = luaL_checkinteger(state, 2);
    lua_pushinteger(state, left + right);
    return 1;
}

int Noop(lua_State*)
{
    return 0;
}

class LuaState
{
public:
    LuaState()
        : state_(luaL_newstate())
    {
        if (!state_)
            throw std::runtime_error("failed to create Lua state");
    }

    ~LuaState()
    {
        lua_close(state_);
    }

    operator lua_State*() const noexcept
    {
        return state_;
    }

private:
    lua_State* state_ = nullptr;
};

void TestValidation()
{
    constexpr FunctionDescriptor valid[] = {
        { "answer", ReturnFortyTwo },
        { "add", Add },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", valid) == LibraryValidationError::None,
        "valid library must pass validation");
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            nullptr, valid) ==
                LibraryValidationError::MissingLibraryName,
        "null library name must be rejected");
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "", valid) ==
                LibraryValidationError::MissingLibraryName,
        "empty library name must be rejected");

    constexpr FunctionDescriptor missingName[] = {
        { nullptr, ReturnFortyTwo },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", missingName) ==
                LibraryValidationError::MissingFunctionName,
        "missing function name must be rejected");

    constexpr FunctionDescriptor missingCallback[] = {
        { "answer", nullptr },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", missingCallback) ==
                LibraryValidationError::MissingCallback,
        "missing callback must be rejected");

    constexpr FunctionDescriptor invalidApiVersion[] = {
        { "answer", ReturnFortyTwo, 0 },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", invalidApiVersion) ==
                LibraryValidationError::InvalidApiVersion,
        "API version zero must be rejected");

    constexpr FunctionDescriptor invertedApiRange[] = {
        { "answer", ReturnFortyTwo, 2, nullptr, 1 },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", invertedApiRange) ==
                LibraryValidationError::InvalidApiVersion,
        "an API version range ending before it starts must be rejected");

    constexpr FunctionDescriptor emptyPermission[] = {
        { "answer", ReturnFortyTwo, 1, "" },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", emptyPermission) ==
                LibraryValidationError::EmptyRequiredPermission,
        "empty required permission must be rejected");

    constexpr FunctionDescriptor duplicate[] = {
        { "answer", ReturnFortyTwo },
        { "answer", Add },
    };
    Check(
        snowdesktop::widget_api::ValidateLibrary(
            "sample", duplicate) ==
                LibraryValidationError::DuplicateFunctionName,
        "duplicate function names must be rejected");
}

void TestRegistration()
{
    LuaState state;
    constexpr FunctionDescriptor functions[] = {
        { "answer", ReturnFortyTwo },
        { "add", Add },
        { "legacyOnly", Noop, 1, nullptr, 1 },
    };

    lua_pushliteral(state, "sentinel");
    const int entryTop = lua_gettop(state);
    snowdesktop::widget_api::RegisterLibrary(
        state, "sample", functions);
    Check(
        lua_gettop(state) == entryTop,
        "successful registration must preserve stack height");

    lua_getglobal(state, "sample");
    Check(lua_istable(state, -1), "registered global must be a table");

    lua_getfield(state, -1, "answer");
    Check(lua_isfunction(state, -1), "answer must be registered");
    Check(
        lua_pcall(state, 0, 1, 0) == LUA_OK,
        "answer callback must execute");
    Check(
        lua_tointeger(state, -1) == 42,
        "answer callback must return its value");
    lua_pop(state, 1);

    lua_getfield(state, -1, "legacyOnly");
    Check(lua_isfunction(state, -1),
        "unversioned registration must retain the complete descriptor set");
    lua_pop(state, 1);

    lua_getfield(state, -1, "add");
    lua_pushinteger(state, 19);
    lua_pushinteger(state, 23);
    Check(
        lua_pcall(state, 2, 1, 0) == LUA_OK,
        "add callback must execute");
    Check(
        lua_tointeger(state, -1) == 42,
        "add callback must receive arguments");
    lua_pop(state, 2);

    Check(
        lua_gettop(state) == entryTop,
        "test cleanup must restore original stack height");
    Check(
        std::string(lua_tostring(state, -1)) == "sentinel",
        "registration must preserve existing stack values");
}

void TestVersionedRegistration()
{
    LuaState state;
    constexpr FunctionDescriptor functions[] = {
        { "answer", ReturnFortyTwo, 1 },
        { "add", Add, 2 },
        { "legacyOnly", Noop, 1, nullptr, 1 },
    };
    snowdesktop::widget_api::RegisterLibrary(
        state, "sample", functions, 1);
    lua_getglobal(state, "sample");
    lua_getfield(state, -1, "answer");
    Check(lua_isfunction(state, -1),
        "API v1 registration must expose v1 functions");
    lua_pop(state, 1);
    lua_getfield(state, -1, "add");
    Check(lua_isnil(state, -1),
        "API v1 registration must not expose v2 functions");
    lua_pop(state, 2);

    snowdesktop::widget_api::RegisterLibrary(
        state, "sample", functions, 2);
    lua_getglobal(state, "sample");
    lua_getfield(state, -1, "answer");
    Check(lua_isfunction(state, -1),
        "API v2 registration must retain unbounded v1 functions");
    lua_pop(state, 1);
    lua_getfield(state, -1, "legacyOnly");
    Check(lua_isnil(state, -1),
        "API v2 registration must hide functions capped at v1");
    lua_pop(state, 2);
}

void TestV2Contract()
{
    const auto v2Libraries =
        snowdesktop::widget_api::SandboxLibraries(2);
    const auto hasV2Library = [&](std::string_view name) {
        return std::find(v2Libraries.begin(), v2Libraries.end(), name) !=
            v2Libraries.end();
    };
    Check(hasV2Library("control") && hasV2Library("interaction") &&
            hasV2Library("view") &&
            hasV2Library("task") && hasV2Library("calendar") &&
            hasV2Library("ui") && hasV2Library("l10n") &&
            !hasV2Library("http") && !hasV2Library("desktop"),
        "API v2 sandbox must expose implemented libraries and hide legacy ones");
    const auto v1Libraries =
        snowdesktop::widget_api::SandboxLibraries(1);
    Check(std::find(v1Libraries.begin(), v1Libraries.end(), "http") !=
            v1Libraries.end() &&
            std::find(v1Libraries.begin(), v1Libraries.end(), "control") ==
                v1Libraries.end(),
        "API v1 sandbox library catalog must remain isolated from v2");

    Check(snowdesktop::widget_api::SupportsFeature(
                "data.app.indexStatus") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.audio.output.analysis") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.audio.output.default") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.audio.output.volume") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.audio.output.control") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.system.openSettings") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.clipboard.text") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.clipboard.image") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.clipboard.fileReference") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.filesystem.picker") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.filesystem.access") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.filesystem.watch") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.media.artwork") &&
            snowdesktop::widget_api::SupportsFeature(
                "calendar.dateMath") &&
            snowdesktop::widget_api::SupportsFeature(
                "calendar.selection") &&
            snowdesktop::widget_api::SupportsFeature(
                "control.focus") &&
            snowdesktop::widget_api::SupportsFeature(
                "control.textArea") &&
            snowdesktop::widget_api::SupportsFeature(
                "control.textInput") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.calendar.events") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.calendar.selectedDate") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.desktop.items") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.desktop.selection") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.desktop.changes") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.media.sessions") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.media.current") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.media.timeline") &&
            snowdesktop::widget_api::SupportsFeature("data.subscribe") &&
            snowdesktop::widget_api::SupportsFeature("data.system.cpu") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.display.topology") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.display.current") &&
            snowdesktop::widget_api::SupportsFeature("data.system.gpu") &&
            snowdesktop::widget_api::SupportsFeature("data.system.memory") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.network.status") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.network.traffic") &&
            snowdesktop::widget_api::SupportsFeature("data.system.power") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.storage.volumes") &&
            snowdesktop::widget_api::SupportsFeature(
                "data.system.storage.io") &&
            snowdesktop::widget_api::SupportsFeature("draw.immediate") &&
            snowdesktop::widget_api::SupportsFeature(
                "interaction.pointerActions") &&
            snowdesktop::widget_api::SupportsFeature(
                "interaction.contextMenu") &&
            snowdesktop::widget_api::SupportsFeature(
                "interaction.region") &&
            snowdesktop::widget_api::SupportsFeature(
                "interaction.scroll") &&
            snowdesktop::widget_api::SupportsFeature("l10n.format") &&
            snowdesktop::widget_api::SupportsFeature("module.package") &&
            snowdesktop::widget_api::SupportsFeature("resource.package") &&
            snowdesktop::widget_api::SupportsFeature(
                "schedule.absolute") &&
            snowdesktop::widget_api::SupportsFeature("schedule.basic") &&
            snowdesktop::widget_api::SupportsFeature("schedule.visibility") &&
            snowdesktop::widget_api::SupportsFeature(
                "settings.appSearch") &&
            snowdesktop::widget_api::SupportsFeature(
                "settings.select.localizedOptions") &&
            snowdesktop::widget_api::SupportsFeature("lifecycle.event") &&
            snowdesktop::widget_api::SupportsFeature("lifecycle.model") &&
            snowdesktop::widget_api::SupportsFeature("state.transient") &&
            snowdesktop::widget_api::SupportsFeature(
                "storage.transaction") &&
            snowdesktop::widget_api::SupportsFeature("system.uptime") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.media.control") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.desktop.refresh") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.desktop.search") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.everything.search") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.network.request") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.notification.show") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.app.launch") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.app.search") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.calendar.write") &&
            snowdesktop::widget_api::SupportsFeature("task.start") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.shell.openUri") &&
            snowdesktop::widget_api::SupportsFeature(
                "task.shell.item") &&
            snowdesktop::widget_api::SupportsFeature("time.calendar") &&
            snowdesktop::widget_api::SupportsFeature("widget.context") &&
            snowdesktop::widget_api::SupportsFeature("widget.panel") &&
            snowdesktop::widget_api::SupportsFeature(
                "system.environment") &&
            snowdesktop::widget_api::SupportsFeature("view.font") &&
            snowdesktop::widget_api::SupportsFeature("view.dataSeries") &&
            snowdesktop::widget_api::SupportsFeature(
                "view.grid.uniform") &&
            snowdesktop::widget_api::SupportsFeature("view.image") &&
            snowdesktop::widget_api::SupportsFeature(
                "view.selectionControls") &&
            snowdesktop::widget_api::SupportsFeature(
                "view.statusVisuals") &&
            snowdesktop::widget_api::SupportsFeature(
                "view.tree.core") &&
            !snowdesktop::widget_api::SupportsFeature("view.tree"),
        "host feature lookup must distinguish supported features");
    const std::vector<std::string> required = {
        "draw.immediate", "view.tree", "view.tree"
    };
    const auto missing = snowdesktop::widget_api::MissingFeatures(required);
    Check(missing.size() == 1 && missing[0] == "view.tree",
        "missing feature diagnostics must be stable and deduplicated");

    LuaState state;
    constexpr FunctionDescriptor functions[] = {
        { "define", snowdesktop::widget_api::LuaDefineWidget, 2 },
        { "apiInfo", snowdesktop::widget_api::LuaApiInfo, 2 },
        { "hasFeature", snowdesktop::widget_api::LuaHasFeature, 2 },
    };
    snowdesktop::widget_api::RegisterLibrary(
        state, "widget", functions, 2);

    lua_getglobal(state, "widget");
    lua_getfield(state, -1, "apiInfo");
    Check(lua_pcall(state, 0, 1, 0) == LUA_OK &&
            lua_istable(state, -1),
        "widget.apiInfo must return the API contract table");
    lua_getfield(state, -1, "current");
    Check(lua_tointeger(state, -1) == 2,
        "widget.apiInfo current version must be v2");
    lua_pop(state, 2);

    lua_getfield(state, -1, "hasFeature");
    lua_pushliteral(state, "draw.immediate");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            lua_toboolean(state, -1) != 0,
        "widget.hasFeature must expose the host feature catalog");
    lua_pop(state, 1);

    lua_getfield(state, -1, "define");
    lua_newtable(state);
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "render");
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "setup");
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "dispose");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            snowdesktop::widget_api::IsDefinedWidget(state, -1),
        "widget.define must accept the setup-model-dispose lifecycle");
    lua_pop(state, 2);

    lua_getglobal(state, "widget");
    lua_getfield(state, -1, "define");
    lua_newtable(state);
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "render");
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "event");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            snowdesktop::widget_api::IsDefinedWidget(state, -1),
        "widget.define must accept event callbacks when dispatch exists");
    lua_pop(state, 2);

    lua_getglobal(state, "widget");
    lua_getfield(state, -1, "define");
    lua_newtable(state);
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "render");
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "menu");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            snowdesktop::widget_api::IsDefinedWidget(state, -1),
        "widget.define must accept menu callbacks when dispatch exists");
    lua_pop(state, 2);

    lua_getglobal(state, "widget");
    lua_getfield(state, -1, "define");
    lua_newtable(state);
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "render");
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "panel");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            snowdesktop::widget_api::IsDefinedWidget(state, -1),
        "widget.define must accept a panel surface callback");
    lua_pop(state, 2);

    lua_getglobal(state, "widget");
    lua_getfield(state, -1, "define");
    lua_newtable(state);
    lua_pushcfunction(state, Noop);
    lua_setfield(state, -2, "view");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            snowdesktop::widget_api::IsDefinedWidget(state, -1),
        "widget.define must accept core declarative view callbacks");
    lua_pop(state, 2);

    constexpr FunctionDescriptor systemFunctions[] = {
        { "capabilities",
            snowdesktop::widget_api::LuaSystemCapabilities, 2 },
    };
    snowdesktop::widget_api::RegisterLibrary(
        state, "system", systemFunctions, 2);
    lua_getglobal(state, "system");
    lua_getfield(state, -1, "capabilities");
    lua_pushliteral(state, "view.tree");
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK &&
            lua_istable(state, -1),
        "system.capabilities must return a status object");
    lua_getfield(state, -1, "available");
    const bool unavailable = lua_toboolean(state, -1) == 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "reason");
    Check(unavailable && lua_isstring(state, -1) &&
            std::string(lua_tostring(state, -1)) == "unsupported",
        "unsupported capabilities must return a stable reason");
    lua_pop(state, 3);
}

void TestTransientState()
{
    LuaState state;
    luaL_openlibs(state);
    constexpr FunctionDescriptor functions[] = {
        { "get", snowdesktop::widget_api::LuaTransientStateGet, 2 },
        { "set", snowdesktop::widget_api::LuaTransientStateSet, 2 },
        { "remove", snowdesktop::widget_api::LuaTransientStateRemove, 2 },
        { "has", snowdesktop::widget_api::LuaTransientStateHas, 2 },
        { "keys", snowdesktop::widget_api::LuaTransientStateKeys, 2 },
        { "clear", snowdesktop::widget_api::LuaTransientStateClear, 2 },
    };
    snowdesktop::widget_api::RegisterLibrary(
        state, "state", functions, 2);
    Check(!snowdesktop::widget_api::ConsumeTransientStateDirty(state),
        "new transient state must start clean");

    constexpr char script[] = R"lua(
local source = { label = "ready", nested = { 1, 2, 3 } }
assert(state.set("model", source))
assert(not state.set("model", { nested = { 1, 2, 3 }, label = "ready" }))
source.nested[1] = 99
local first = state.get("model")
assert(first.label == "ready" and first.nested[1] == 1)
first.nested[2] = 88
assert(state.get("model").nested[2] == 2)
assert(state.has("model"))
assert(state.get("missing", { available = false }).available == false)
local keys = state.keys()
assert(#keys == 1 and keys[1] == "model")
assert(state.remove("model") and not state.remove("model"))
assert(not state.has("model"))

local cyclic = {}
cyclic.self = cyclic
assert(not pcall(function() state.set("cyclic", cyclic) end))
assert(not pcall(function() state.set("mixed", { [1] = "a", key = "b" }) end))

for index = 1, 256 do
    assert(state.set("key-" .. index, index))
end
assert(not pcall(function() state.set("overflow", 1) end))
assert(#state.keys() == 256)
assert(state.clear() and not state.clear())
)lua";
    Check(luaL_loadbuffer(state, script, sizeof(script) - 1,
            "@transient-state-test") == LUA_OK &&
            lua_pcall(state, 0, 0, 0) == LUA_OK,
        lua_gettop(state) > 0 && lua_isstring(state, -1)
            ? lua_tostring(state, -1)
            : "transient state script must execute");
    Check(snowdesktop::widget_api::ConsumeTransientStateDirty(state) &&
            !snowdesktop::widget_api::ConsumeTransientStateDirty(state),
        "transient state changes must coalesce into one dirty signal");
}

void TestCatalogValidation()
{
    static constexpr FunctionDescriptor firstFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor secondFunctions[] = {
        { "add", Add },
    };
    static constexpr LibraryDescriptor valid[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "first", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "second", secondFunctions),
    };
    Check(
        snowdesktop::widget_api::ValidateCatalog(valid).error ==
            CatalogValidationError::None,
        "valid catalog must pass validation");

    static constexpr LibraryDescriptor duplicateLibraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "sample", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "sample", secondFunctions),
    };
    const auto duplicateResult =
        snowdesktop::widget_api::ValidateCatalog(duplicateLibraries);
    Check(
        duplicateResult.error ==
            CatalogValidationError::DuplicateLibraryName &&
            duplicateResult.libraryIndex == 1,
        "duplicate library names must identify the later library");

    static constexpr FunctionDescriptor invalidFunctions[] = {
        { nullptr, ReturnFortyTwo },
    };
    static constexpr LibraryDescriptor invalidLibrary[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "sample", invalidFunctions),
    };
    const auto invalidResult =
        snowdesktop::widget_api::ValidateCatalog(invalidLibrary);
    Check(
        invalidResult.error == CatalogValidationError::InvalidLibrary &&
            invalidResult.libraryIndex == 0 &&
            invalidResult.libraryError ==
                LibraryValidationError::MissingFunctionName,
        "catalog validation must preserve library validation details");

    const FunctionDescriptor* found =
        snowdesktop::widget_api::FindFunction(
            valid, "second", "add");
    Check(
        found == &secondFunctions[0] &&
            found->sinceApi == 1 &&
            found->requiredPermission == nullptr,
        "catalog lookup must return function contract metadata");
    Check(
        snowdesktop::widget_api::FindFunction(
            valid, "missing", "add") == nullptr &&
            snowdesktop::widget_api::FindFunction(
                valid, "second", "missing") == nullptr &&
            snowdesktop::widget_api::FindFunction(
                valid, "", "add") == nullptr,
        "catalog lookup must reject missing or empty names");
}

void TestCatalogRegistration()
{
    LuaState state;
    static constexpr FunctionDescriptor firstFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor secondFunctions[] = {
        { "add", Add },
    };
    static constexpr LibraryDescriptor libraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "first", firstFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "second", secondFunctions),
    };

    lua_pushliteral(state, "sentinel");
    const int entryTop = lua_gettop(state);
    snowdesktop::widget_api::RegisterLibraries(state, libraries);
    Check(
        lua_gettop(state) == entryTop,
        "catalog registration must preserve stack height");
    lua_getglobal(state, "first");
    lua_getfield(state, -1, "answer");
    Check(
        lua_pcall(state, 0, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42,
        "first catalog library must be callable");
    lua_pop(state, 2);
    lua_getglobal(state, "second");
    lua_getfield(state, -1, "add");
    lua_pushinteger(state, 20);
    lua_pushinteger(state, 22);
    Check(
        lua_pcall(state, 2, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42,
        "second catalog library must be callable");
    lua_pop(state, 2);
    Check(
        lua_gettop(state) == entryTop,
        "catalog test cleanup must restore stack height");
}

void TestInvalidRegistrationIsAtomic()
{
    LuaState state;
    lua_pushinteger(state, 7);
    const int entryTop = lua_gettop(state);
    constexpr FunctionDescriptor duplicate[] = {
        { "answer", ReturnFortyTwo },
        { "answer", Add },
    };

    bool threw = false;
    try
    {
        snowdesktop::widget_api::RegisterLibrary(
            state, "invalid", duplicate);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "invalid registration must throw");
    Check(
        lua_gettop(state) == entryTop &&
            lua_tointeger(state, -1) == 7,
        "invalid registration must not modify the stack");
    lua_getglobal(state, "invalid");
    Check(lua_isnil(state, -1), "invalid library must not be published");
    lua_pop(state, 1);

    threw = false;
    try
    {
        constexpr FunctionDescriptor valid[] = {
            { "answer", ReturnFortyTwo },
        };
        snowdesktop::widget_api::RegisterLibrary(
            nullptr, "sample", valid);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "null Lua state must throw");
}


void TestInvalidCatalogRegistrationIsAtomic()
{
    LuaState state;
    static constexpr FunctionDescriptor validFunctions[] = {
        { "answer", ReturnFortyTwo },
    };
    static constexpr FunctionDescriptor invalidFunctions[] = {
        { nullptr, Add },
    };
    static constexpr LibraryDescriptor libraries[] = {
        snowdesktop::widget_api::DescribeLibrary(
            "wouldPublish", validFunctions),
        snowdesktop::widget_api::DescribeLibrary(
            "invalid", invalidFunctions),
    };

    lua_pushinteger(state, 7);
    const int entryTop = lua_gettop(state);
    bool threw = false;
    try
    {
        snowdesktop::widget_api::RegisterLibraries(
            state, libraries);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "invalid catalog registration must throw");
    Check(
        lua_gettop(state) == entryTop &&
            lua_tointeger(state, -1) == 7,
        "invalid catalog registration must preserve the stack");
    lua_getglobal(state, "wouldPublish");
    Check(
        lua_isnil(state, -1),
        "catalog must be fully validated before publishing globals");
    lua_pop(state, 1);
}
}

int main()
{
    TestValidation();
    TestRegistration();
    TestVersionedRegistration();
    TestV2Contract();
    TestTransientState();
    TestCatalogValidation();
    TestCatalogRegistration();
    TestInvalidRegistrationIsAtomic();
    TestInvalidCatalogRegistrationIsAtomic();
    std::cout << "widget API registry tests passed\n";
    return 0;
}
