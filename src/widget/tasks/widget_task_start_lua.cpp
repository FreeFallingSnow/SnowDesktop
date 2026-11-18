#include "widget_task_start_lua.h"
#include "widget_audio_output_task_executor.h"
#include "widget_audio_playback_lua.h"
#include "widget_clipboard_task_executor.h"
#include "widget_filesystem_task_executor.h"
#include "widget_location_task_executor.h"
#include "widget_media_task_executor.h"
#include "widget_native_task_executor.h"
#include "widget_system_control_tasks.h"
#include "platform/http_runtime.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/runtime/widget_runtime_image.h"
#include "widget/settings/widget_system_settings.h"
#include "widget/storage/widget_secret_store.h"
#include "system/calendar/calendar_service.h"
#include "widget_multiplayer_wire.h"

extern "C" {
#include <lauxlib.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace
{
static const char* ReadClipboardTaskArguments(lua_State* state,
    std::string_view taskName, bool hasArguments,
    std::unordered_map<std::string, std::string>& arguments,
    bool (*isValidUtf8)(const std::string&))
{
    using snowdesktop::widget_runtime::WidgetClipboardTaskExecutor;
    const bool clear = taskName == "clipboard.clear";
    const bool write = taskName == "clipboard.write";
    if (!hasArguments)
        return clear ? nullptr : "clipboard read and write require an arguments table";

    std::string format;
    const char* payload = nullptr;
    if (!clear)
    {
        lua_getfield(state, 2, "format");
        size_t length = 0;
        if (lua_type(state, -1) == LUA_TSTRING)
        {
            const char* value = lua_tolstring(state, -1, &length);
            if (length <= std::string_view("file-reference").size())
                format.assign(value, length);
        }
        lua_pop(state, 1);
        if (!WidgetClipboardTaskExecutor::SupportsFormat(format))
            return "clipboard format must be text, html, image, or file-reference";
        arguments.emplace("format", format);
        if (write)
            payload = format == "file-reference" ? "items" :
                format == "text" ? "text" : format == "html" ? "html" : "image";
    }

    lua_pushnil(state);
    while (lua_next(state, 2) != 0)
    {
        size_t length = 0;
        const char* raw = lua_type(state, -2) == LUA_TSTRING
            ? lua_tolstring(state, -2, &length) : nullptr;
        const std::string_view key(raw ? raw : "", length);
        if (clear || (key != "format" && (!payload || key != payload)))
        {
            lua_pop(state, 2);
            return "unknown clipboard argument";
        }
        lua_pop(state, 1);
    }
    if (!payload) return nullptr;

    lua_getfield(state, 2, payload);
    const char* error = nullptr;
    if (format == "image")
    {
        const auto* handle = TestResourceHandle(state, -1);
        if (!handle || handle->type != LuaResourceType::Image)
            error = "clipboard.write image must be a SnowImageResource handle";
        else
            arguments.emplace("image", handle->name);
    }
    else if (format == "file-reference")
    {
        const auto count = lua_istable(state, -1) ? lua_rawlen(state, -1) : 0;
        error = "clipboard.write items must be a dense array of 1 to 32 opaque file references or handles";
        if (count > 0 && count <= WidgetClipboardTaskExecutor::MaximumFileReferences)
        {
            std::size_t seen = 0;
            lua_pushnil(state);
            while (lua_next(state, -2) != 0)
            {
                const auto index = lua_isinteger(state, -2) ? lua_tointeger(state, -2) : 0;
                size_t length = 0;
                const char* raw = lua_type(state, -1) == LUA_TSTRING
                    ? lua_tolstring(state, -1, &length) : nullptr;
                const std::string_view reference(raw ? raw : "", length);
                const bool valid = index > 0 && static_cast<std::size_t>(index) <= count &&
                    ((reference.starts_with("item:") && reference.size() == 37) ||
                        snowdesktop::widget_runtime::WidgetFilesystemHandleStore::
                            IsOpaqueHandle(reference));
                if (!valid)
                {
                    lua_pop(state, 3);
                    return error;
                }
                arguments.emplace("item" + std::to_string(index), reference);
                ++seen;
                lua_pop(state, 1);
            }
            if (seen == count) error = nullptr;
        }
    }
    else
    {
        size_t length = 0;
        const char* raw = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &length) : nullptr;
        if (!raw || length > WidgetClipboardTaskExecutor::MaximumTextBytes)
            error = "clipboard.write text or html must be a string of at most 262144 bytes";
        else
        {
            std::string value(raw, length);
            if (value.find('\0') != std::string::npos ||
                (!value.empty() && !isValidUtf8(value)))
                error = "clipboard.write text or html must be valid UTF-8 without NUL";
            else
                arguments.emplace(payload, std::move(value));
        }
    }
    lua_pop(state, 1);
    return error;
}

__declspec(noinline) int LuaTaskStartWithDeferredSecretError(
    lua_State* state, const LuaTaskStartHost& host)
{
    if (lua_gettop(state) < 1 || lua_gettop(state) > 2)
        return luaL_error(state,
            "task.start: expected a task name and optional arguments");
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength > 128)
        return luaL_error(state,
            "task.start: task name must contain 1 to 128 bytes");
    const std::string taskName(name, nameLength);
    std::unordered_map<std::string, std::string> arguments;
    const bool hasArguments = !lua_isnoneornil(state, 2);
    if (hasArguments)
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        if (lua_getmetatable(state, 2) != 0)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: arguments must be a plain table");
        }
    }

    if (taskName == "multiplayer.send")
    {
        if (!hasArguments) return luaL_error(state, "task.start: multiplayer.send requires text");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING)
                return luaL_error(state, "task.start: multiplayer.send requires text");
            size_t keyLength = 0, length = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            const char* text = lua_tolstring(state, -1, &length);
            if (std::string_view(key, keyLength) != "text" ||
                !snowdesktop::steam_widget_multiplayer::IsUtf8Text({text, length}))
                return luaL_error(state, "task.start: multiplayer.send requires 1 to 1024 UTF-8 bytes");
            arguments.emplace("text", std::string(text, length));
            lua_pop(state, 1);
        }
        if (arguments.empty()) return luaL_error(state, "task.start: multiplayer.send requires text");
    }
    else if (taskName == "native.request" || taskName == "native.watch")
    {
        if (!hasArguments) return luaL_error(state, "task.start: native task requires arguments");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
                return luaL_error(state, "task.start: native task keys must be strings");
            size_t length = 0;
            const char* key = lua_tolstring(state, -2, &length);
            const std::string field(key, length);
            if (field == "timeoutMs" && lua_isinteger(state, -1))
                arguments[field] = std::to_string(lua_tointeger(state, -1));
            else if ((field == "helper" || field == "operation" || field == "payload") && lua_type(state, -1) == LUA_TSTRING)
            {
                const char* value = lua_tolstring(state, -1, &length);
                if (length > 8192) return luaL_error(state, "task.start: native task value is too large");
                arguments[field] = std::string(value, length);
            }
            else return luaL_error(state, "task.start: native task received an invalid argument");
            lua_pop(state, 1);
        }
        snowdesktop::widget_runtime::NativeRequestOptions options;
        if (!snowdesktop::widget_runtime::ParseNativeRequestOptions(arguments, options))
            return luaL_error(state, "task.start: invalid native task arguments");
    }
    else if (taskName == "location.current")
    {
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING || !lua_isinteger(state, -1))
                    return luaL_error(state, "task.start: location.current requires integer options");
                size_t length = 0;
                const char* key = lua_tolstring(state, -2, &length);
                arguments[std::string(key, length)] = std::to_string(lua_tointeger(state, -1));
                lua_pop(state, 1);
            }
        }
        snowdesktop::widget_runtime::LocationOptions options;
        if (!snowdesktop::widget_runtime::ParseLocationOptions(arguments, options))
            return luaL_error(state, "task.start: invalid location.current options");
    }
    else if (snowdesktop::widget_runtime::IsSystemControlTask(taskName))
    {
        using snowdesktop::widget_runtime::SystemControlArgumentKind;
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                    return luaL_error(state, "task.start: system control keys must be strings");
                size_t size = 0; const auto* keyText = lua_tolstring(state, -2, &size);
                if (!size || size > 128) return luaL_error(state, "task.start: invalid system control key");
                const std::string key(keyText, size);
                const auto kind = snowdesktop::widget_runtime::SystemControlArgumentType(key);
                if (kind == SystemControlArgumentKind::Boolean && lua_type(state, -1) == LUA_TBOOLEAN)
                    arguments[key] = lua_toboolean(state, -1) ? "1" : "0";
                else if (kind == SystemControlArgumentKind::Number && lua_type(state, -1) == LUA_TNUMBER)
                {
                    const auto value = lua_tonumber(state, -1);
                    if (!std::isfinite(value)) return luaL_error(state, "task.start: expected a finite number");
                    char encoded[128]{};
                    const auto formatted = std::to_chars(std::begin(encoded), std::end(encoded), value,
                        std::chars_format::general, std::numeric_limits<double>::max_digits10);
                    if (formatted.ec != std::errc{}) return luaL_error(state, "task.start: number cannot be encoded");
                    arguments[key] = std::string(encoded, formatted.ptr);
                }
                else if (kind == SystemControlArgumentKind::Text && lua_type(state, -1) == LUA_TSTRING)
                {
                    const auto* text = lua_tolstring(state, -1, &size);
                    if (!size || size > 4096) return luaL_error(state, "task.start: system control text must contain 1 to 4096 bytes");
                    arguments[key] = std::string(text, size);
                    if (!host.isValidUtf8(arguments[key])) return luaL_error(state, "task.start: expected UTF-8 text");
                }
                else return luaL_error(state, "task.start: unknown system control argument or incorrect type");
                lua_pop(state, 1);
            }
        }
        snowdesktop::system_control::Request request;
        if (!snowdesktop::widget_runtime::MakeSystemControlRequest(taskName, arguments, request))
            return luaL_error(state, "task.start: invalid system control arguments");
    }
    else if (snowdesktop::widget_runtime::WidgetAudioPlayback::SupportsAction(taskName))
    {
        if (!snowdesktop::widget_runtime::ReadAudioPlaybackArguments(state, taskName, arguments))
            return luaL_error(state, "task.start: invalid audio playback arguments");
    }
    else if (snowdesktop::widget_runtime::WidgetMediaTaskExecutor::
            SupportsAction(taskName))
    {
        const bool requiresValue = taskName == "media.seek" ||
            taskName == "media.setRate" ||
            taskName == "media.setShuffle" ||
            taskName == "media.setRepeat";
        if (requiresValue && !hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: media argument keys must be strings");
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                const bool allowed = key == "sessionId" ||
                    (taskName == "media.seek" && key == "positionMs") ||
                    (taskName == "media.setRate" && key == "rate") ||
                    (taskName == "media.setShuffle" && key == "shuffle") ||
                    (taskName == "media.setRepeat" && key == "mode");
                if (!allowed)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: %s received an unknown argument",
                        taskName.c_str());
                }
                lua_pop(state, 1);
            }

            lua_getfield(state, 2, "sessionId");
            if (!lua_isnil(state, -1))
            {
                if (lua_type(state, -1) != LUA_TSTRING)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: media sessionId must be a string");
                }
                size_t length = 0;
                const char* value = lua_tolstring(state, -1, &length);
                std::string sessionId(value ? value : "", length);
                lua_pop(state, 1);
                if (sessionId.empty() || sessionId.size() > 128 ||
                    sessionId.find('\0') != std::string::npos ||
                    !host.isValidUtf8(sessionId))
                {
                    return luaL_error(state,
                        "task.start: media sessionId must contain 1 to 128 bytes of valid UTF-8");
                }
                arguments.emplace("sessionId", std::move(sessionId));
            }
            else
            {
                lua_pop(state, 1);
            }
        }

        if (taskName == "media.seek")
        {
            lua_getfield(state, 2, "positionMs");
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.seek positionMs must be an integer");
            }
            const lua_Integer position = lua_tointeger(state, -1);
            lua_pop(state, 1);
            if (position < 0 || static_cast<std::uint64_t>(position) >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max() / 10000))
            {
                return luaL_error(state,
                    "task.start: media.seek positionMs is out of range");
            }
            arguments.emplace("positionMs", std::to_string(position));
        }
        else if (taskName == "media.setRate")
        {
            lua_getfield(state, 2, "rate");
            if (lua_type(state, -1) != LUA_TNUMBER)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setRate rate must be a number");
            }
            const double rate = lua_tonumber(state, -1);
            lua_pop(state, 1);
            if (!std::isfinite(rate) || rate <= 0.0)
                return luaL_error(state,
                    "task.start: media.setRate rate must be finite and positive");
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), rate,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{})
                return luaL_error(state,
                    "task.start: media.setRate rate cannot be represented");
            arguments.emplace("rate", std::string(
                buffer.data(), converted.ptr));
        }
        else if (taskName == "media.setShuffle")
        {
            lua_getfield(state, 2, "shuffle");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setShuffle shuffle must be a boolean");
            }
            const bool shuffle = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            arguments.emplace("shuffle", shuffle ? "1" : "0");
        }
        else if (taskName == "media.setRepeat")
        {
            lua_getfield(state, 2, "mode");
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setRepeat mode must be a string");
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            std::string mode(value ? value : "", length);
            lua_pop(state, 1);
            if (mode != "none" && mode != "track" && mode != "list")
                return luaL_error(state,
                    "task.start: media.setRepeat mode must be none, track, or list");
            arguments.emplace("mode", std::move(mode));
        }
    }
    else if (snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor::
            SupportsAction(taskName))
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        const std::string_view expectedKey =
            taskName == "audio.output.setVolume" ? "volume" : "muted";
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: audio output argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(
                state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            if (key != expectedKey &&
                !(taskName == "audio.output.setVolume" && key == "coalesce"))
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s accepts only %s",
                    taskName.c_str(), expectedKey.data());
            }
            lua_pop(state, 1);
        }

        if (taskName == "audio.output.setVolume")
        {
            lua_getfield(state, 2, "coalesce");
            if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state, "task.start: audio.output.setVolume coalesce must be boolean");
            }
            if (lua_toboolean(state, -1)) arguments.emplace("coalesce", "1");
            lua_pop(state, 1);
            lua_getfield(state, 2, "volume");
            if (lua_type(state, -1) != LUA_TNUMBER)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume must be a number");
            }
            const double volume = lua_tonumber(state, -1);
            lua_pop(state, 1);
            if (!std::isfinite(volume))
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume must be finite");
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), volume,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{})
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume cannot be represented");
            arguments.emplace("volume", std::string(
                buffer.data(), converted.ptr));
        }
        else
        {
            lua_getfield(state, 2, "muted");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: audio.output.setMute muted must be a boolean");
            }
            const bool muted = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            arguments.emplace("muted", muted ? "1" : "0");
        }
    }
    else if (taskName == "system.openSettings")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: system.openSettings requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING ||
                std::string_view(lua_tostring(state, -2)) != "page")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: system.openSettings accepts only page");
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "page");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: system.openSettings page must be a string");
        }
        size_t length = 0;
        const char* value = lua_tolstring(state, -1, &length);
        const std::string page(value ? value : "", length);
        lua_pop(state, 1);
        if (!snowdesktop::widget_runtime::SystemSettingsUri(page) &&
            !snowdesktop::widget_runtime::HostSettingsPage(page))
            return luaL_error(state,
                "task.start: system.openSettings page is not supported");
        arguments.emplace("page", page);
    }
    else if (snowdesktop::widget_runtime::WidgetClipboardTaskExecutor::
            SupportsAction(taskName))
    {
        if (const char* error = ReadClipboardTaskArguments(
                state, taskName, hasArguments, arguments, host.isValidUtf8))
            return luaL_error(state, "task.start: %s", error);
    }
    else if (IsFilesystemPickerTask(taskName))
    {
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: filesystem picker argument keys must be strings");
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                const bool allowed =
                    (taskName == "filesystem.pickOpen" &&
                        (key == "extensions" || key == "multiple")) ||
                    (taskName == "filesystem.pickSave" &&
                        (key == "extensions" || key == "suggestedName")) ||
                    (taskName == "filesystem.pickFolder" &&
                        (key == "access" || key == "multiple"));
                if (!allowed)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: %s received an unknown argument",
                        taskName.c_str());
                }
                lua_pop(state, 1);
            }
        }

        if (taskName != "filesystem.pickSave" && hasArguments)
        {
            lua_getfield(state, 2, "multiple");
            if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
                return luaL_error(state, "task.start: multiple must be boolean");
            arguments.emplace("multiple", lua_toboolean(state, -1) ? "true" : "false");
            lua_pop(state, 1);
        }
        if (taskName != "filesystem.pickFolder")
        {
            std::vector<std::string> extensions;
            if (hasArguments)
            {
                lua_getfield(state, 2, "extensions");
                if (!lua_isnil(state, -1))
                {
                    if (lua_type(state, -1) != LUA_TTABLE)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem extensions must be an array");
                    }
                    const int extensionTable = lua_absindex(state, -1);
                    const std::size_t count = lua_rawlen(
                        state, extensionTable);
                    if (count > 16)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem extensions accepts at most 16 entries");
                    }
                    lua_pushnil(state);
                    while (lua_next(state, extensionTable) != 0)
                    {
                        if (!lua_isinteger(state, -2))
                        {
                            lua_pop(state, 3);
                            return luaL_error(state,
                                "task.start: filesystem extensions must be an array");
                        }
                        const lua_Integer index = lua_tointeger(state, -2);
                        if (index < 1 ||
                            static_cast<std::size_t>(index) > count)
                        {
                            lua_pop(state, 3);
                            return luaL_error(state,
                                "task.start: filesystem extensions must be contiguous");
                        }
                        lua_pop(state, 1);
                    }
                    extensions.reserve(count);
                    for (std::size_t index = 1; index <= count; ++index)
                    {
                        lua_rawgeti(state, extensionTable,
                            static_cast<lua_Integer>(index));
                        size_t length = 0;
                        const char* value = lua_type(state, -1) == LUA_TSTRING
                            ? lua_tolstring(state, -1, &length) : nullptr;
                        std::string extension(
                            value ? value : "", length);
                        lua_pop(state, 1);
                        if (!extension.empty() && extension.front() == '.')
                            extension.erase(extension.begin());
                        const bool valid = !extension.empty() &&
                            extension.size() <= 32 &&
                            extension.front() != '.' &&
                            extension.back() != '.' &&
                            std::all_of(extension.begin(), extension.end(),
                                [](const unsigned char character) {
                                    return std::isalnum(character) ||
                                        character == '.';
                                }) &&
                            extension.find("..") == std::string::npos;
                        if (!valid)
                        {
                            lua_pop(state, 1);
                            return luaL_error(state,
                                "task.start: filesystem extensions must contain safe extension names");
                        }
                        std::transform(extension.begin(), extension.end(),
                            extension.begin(), [](const unsigned char character) {
                                return static_cast<char>(std::tolower(character));
                            });
                        if (std::find(extensions.begin(), extensions.end(),
                                extension) == extensions.end())
                            extensions.push_back(std::move(extension));
                    }
                }
                lua_pop(state, 1);
            }
            if (!extensions.empty())
            {
                std::string encoded;
                for (const auto& extension : extensions)
                {
                    if (!encoded.empty()) encoded.push_back(';');
                    encoded += extension;
                }
                arguments.emplace("extensions", std::move(encoded));
            }

            if (taskName == "filesystem.pickSave" && hasArguments)
            {
                lua_getfield(state, 2, "suggestedName");
                if (!lua_isnil(state, -1))
                {
                    size_t length = 0;
                    const char* value = lua_type(state, -1) == LUA_TSTRING
                        ? lua_tolstring(state, -1, &length) : nullptr;
                    std::string suggestedName(
                        value ? value : "", length);
                    const bool invalidCharacter = std::any_of(
                        suggestedName.begin(), suggestedName.end(),
                        [](const unsigned char character) {
                            return character < 0x20 ||
                                std::string_view("<>:\"/\\|?*")
                                    .find(static_cast<char>(character)) !=
                                    std::string_view::npos;
                        });
                    if (!value || suggestedName.empty() ||
                        suggestedName.size() > 255 ||
                        !host.isValidUtf8(suggestedName) ||
                        invalidCharacter || suggestedName.back() == '.' ||
                        suggestedName.back() == ' ')
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem suggestedName is invalid");
                    }
                    arguments.emplace(
                        "suggestedName", std::move(suggestedName));
                }
                lua_pop(state, 1);
            }
        }
        else
        {
            std::string access = "read";
            if (hasArguments)
            {
                lua_getfield(state, 2, "access");
                if (!lua_isnil(state, -1))
                {
                    size_t length = 0;
                    const char* value = lua_type(state, -1) == LUA_TSTRING
                        ? lua_tolstring(state, -1, &length) : nullptr;
                    access.assign(value ? value : "", length);
                    if (!value || (access != "read" &&
                            access != "write" &&
                            access != "readWrite"))
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem folder access must be read, write, or readWrite");
                    }
                }
                lua_pop(state, 1);
            }
            arguments.emplace("access", std::move(access));
        }
    }
    else if (IsFilesystemHandleTask(taskName))
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: filesystem argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(
                state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            const bool allowed = key == "handle" ||
                (taskName == "filesystem.list" &&
                    (key == "offset" || key == "limit" || key == "grantHandles")) ||
                (taskName == "filesystem.image" &&
                    (key == "name" || key == "maxDimension")) ||
                (taskName == "filesystem.read" &&
                    (key == "encoding" || key == "maxBytes")) ||
                (taskName == "filesystem.write" &&
                    (key == "encoding" || key == "text" || key == "data" ||
                        key == "expectedRevision"));
            if (!allowed)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    taskName.c_str());
            }
            lua_pop(state, 1);
        }

        lua_getfield(state, 2, "handle");
        size_t handleLength = 0;
        const char* handleValue = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &handleLength) : nullptr;
        std::string handle(handleValue ? handleValue : "", handleLength);
        lua_pop(state, 1);
        if (!handleValue || !snowdesktop::widget_runtime::
                WidgetFilesystemHandleStore::IsOpaqueHandle(handle))
            return luaL_error(state,
                "task.start: filesystem handle is invalid");
        arguments.emplace("handle", std::move(handle));

        const auto readInteger = [&](const char* field,
            lua_Integer fallback, lua_Integer minimum,
            lua_Integer maximum, lua_Integer& output) {
            lua_getfield(state, 2, field);
            if (lua_isnil(state, -1)) output = fallback;
            else if (lua_isinteger(state, -1))
                output = lua_tointeger(state, -1);
            else
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pop(state, 1);
            return output >= minimum && output <= maximum;
        };
        if (taskName == "filesystem.image")
        {
            lua_Integer dimension = 2048;
            if (!readInteger("maxDimension", 2048, 1, 2048, dimension))
                return luaL_error(state, "task.start: maxDimension must be 1 through 2048");
            arguments.emplace("maxDimension", std::to_string(dimension));
            lua_getfield(state, 2, "name");
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = lua_type(state, -1) == LUA_TSTRING ?
                    lua_tolstring(state, -1, &length) : nullptr;
                if (!value || !snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::
                        IsDirectChildName(std::string_view(value, length)))
                    return luaL_error(state, "task.start: image name must be a direct child filename");
                arguments.emplace("name", std::string(value, length));
            }
            lua_pop(state, 1);
        }
        if (taskName == "filesystem.list")
        {
            lua_getfield(state, 2, "grantHandles");
            if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
                return luaL_error(state, "task.start: grantHandles must be boolean");
            arguments.emplace("grantHandles", lua_isnil(state, -1) ||
                lua_toboolean(state, -1) ? "true" : "false");
            lua_pop(state, 1);
            lua_Integer offset = 0;
            lua_Integer limit = 50;
            if (!readInteger("offset", 0, 0,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumListOffset,
                    offset) ||
                !readInteger("limit", 50, 1,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumListLimit,
                    limit))
                return luaL_error(state,
                    "task.start: filesystem.list pagination is out of range");
            arguments.emplace("offset", std::to_string(offset));
            arguments.emplace("limit", std::to_string(limit));
        }
        if (taskName == "filesystem.read" ||
            taskName == "filesystem.write")
        {
            std::string encoding = "utf8";
            lua_getfield(state, 2, "encoding");
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = lua_type(state, -1) == LUA_TSTRING
                    ? lua_tolstring(state, -1, &length) : nullptr;
                const std::string_view requested(
                    value ? value : "", length);
                if (!value ||
                    (requested != "utf8" && requested != "binary"))
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: filesystem encoding must be utf8 or binary");
                }
                encoding.assign(requested.data(), requested.size());
            }
            lua_pop(state, 1);
            arguments.emplace("encoding", encoding);
        }
        if (taskName == "filesystem.read")
        {
            lua_Integer maxBytes = 512 * 1024;
            if (!readInteger("maxBytes", 512 * 1024, 1,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumTextBytes,
                    maxBytes))
                return luaL_error(state,
                    "task.start: filesystem.read maxBytes is out of range");
            arguments.emplace("maxBytes", std::to_string(maxBytes));
        }
        else if (taskName == "filesystem.write")
        {
            const std::string& encoding = arguments.at("encoding");
            const char* payloadField = encoding == "binary" ? "data" : "text";
            const char* rejectedField = encoding == "binary" ? "text" : "data";
            lua_getfield(state, 2, rejectedField);
            const bool hasRejectedPayload = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (hasRejectedPayload)
                return luaL_error(state,
                    "task.start: filesystem.write payload does not match encoding");

            lua_getfield(state, 2, payloadField);
            size_t length = 0;
            const char* value = lua_type(state, -1) == LUA_TSTRING
                ? lua_tolstring(state, -1, &length) : nullptr;
            std::string text(value ? value : "", length);
            lua_pop(state, 1);
            if (!value || text.size() > snowdesktop::widget_runtime::
                    WidgetFilesystemTaskExecutor::MaximumTextBytes ||
                (encoding == "utf8" &&
                    (text.find('\0') != std::string::npos ||
                        (!text.empty() && !host.isValidUtf8(text)))))
                return luaL_error(state,
                    "task.start: filesystem.write payload is invalid or exceeds 1048576 bytes");
            arguments.emplace(payloadField, std::move(text));

            lua_getfield(state, 2, "expectedRevision");
            if (!lua_isnil(state, -1))
            {
                size_t revisionLength = 0;
                const char* revision = lua_type(state, -1) == LUA_TSTRING
                    ? lua_tolstring(state, -1, &revisionLength) : nullptr;
                if (!revision || revisionLength == 0 ||
                    revisionLength > 64 ||
                    std::string_view(revision, revisionLength)
                        .find('\0') != std::string_view::npos)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: filesystem.write expectedRevision is invalid");
                }
                arguments.emplace("expectedRevision",
                    std::string(revision, revisionLength));
            }
            lua_pop(state, 1);
        }
    }
    else if (taskName == "app.search" || taskName == "desktop.search" ||
        taskName == "everything.search")
    {
        const std::string& searchName = taskName;
        const lua_Integer maximumOffset =
            taskName == "app.search" ? 10000 : 100;
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                searchName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s argument keys must be strings",
                    searchName.c_str());
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyValue ? keyValue : "", keyLength);
            if (key != "query" && key != "limit" && key != "offset")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    searchName.c_str());
            }
            lua_pop(state, 1);
        }

        lua_getfield(state, 2, "query");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: %s query must be a string",
                searchName.c_str());
        }
        size_t queryLength = 0;
        const char* queryValue = lua_tolstring(state, -1, &queryLength);
        std::string query(queryValue ? queryValue : "", queryLength);
        lua_pop(state, 1);
        if (query.empty() || query.size() > 256 ||
            !host.isValidUtf8(query))
        {
            return luaL_error(state,
                "task.start: %s query must contain 1 to 256 bytes of valid UTF-8",
                searchName.c_str());
        }
        arguments.emplace("query", std::move(query));

        lua_Integer limit = 50;
        lua_getfield(state, 2, "limit");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: %s limit must be an integer",
                    searchName.c_str());
            }
            limit = lua_tointeger(state, -1);
        }
        lua_pop(state, 1);
        if (limit < 1 || limit > 100)
            return luaL_error(state,
                "task.start: %s limit must be between 1 and 100",
                searchName.c_str());
        arguments.emplace("limit", std::to_string(limit));

        lua_Integer offset = 0;
        lua_getfield(state, 2, "offset");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: %s offset must be an integer",
                    searchName.c_str());
            }
            offset = lua_tointeger(state, -1);
        }
        lua_pop(state, 1);
        if (offset < 0 || offset > maximumOffset)
            return luaL_error(state,
                "task.start: %s offset must be between 0 and %lld",
                searchName.c_str(),
                static_cast<long long>(maximumOffset));
        arguments.emplace("offset", std::to_string(offset));
    }
    else if (taskName == "app.launch" ||
        taskName == "shell.openItem" ||
        taskName == "shell.revealItem")
    {
        const std::string& actionName = taskName;
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                actionName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s argument keys must be strings",
                    actionName.c_str());
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            if (std::string_view(keyValue ? keyValue : "", keyLength) != "ref")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    actionName.c_str());
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "ref");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: %s ref must be a string",
                actionName.c_str());
        }
        size_t refLength = 0;
        const char* refValue = lua_tolstring(state, -1, &refLength);
        std::string reference(refValue ? refValue : "", refLength);
        lua_pop(state, 1);
        if (reference.empty() || reference.size() > 128)
            return luaL_error(state,
                "task.start: %s ref must contain 1 to 128 bytes",
                actionName.c_str());
        arguments.emplace("ref", std::move(reference));
    }
    else if (taskName == "notification.show" ||
        taskName == "notification.update" ||
        taskName == "notification.dismiss" ||
        taskName == "notification.schedule" ||
        taskName == "notification.cancel")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s argument keys must be strings",
                    taskName.c_str());
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyValue ? keyValue : "", keyLength);
            const bool textKey = key == "title" || key == "message";
            const bool idKey = key == "notificationId";
            const bool atKey = key == "atMs";
            const bool structuredKey = key == "image" ||
                key == "progress" || key == "actions";
            const bool allowed =
                ((taskName == "notification.show") &&
                    (textKey || structuredKey)) ||
                ((taskName == "notification.schedule") &&
                    (textKey || atKey || structuredKey)) ||
                ((taskName == "notification.update") &&
                    (textKey || idKey || structuredKey)) ||
                ((taskName == "notification.dismiss" ||
                    taskName == "notification.cancel") && idKey);
            if (!allowed)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    taskName.c_str());
            }
            lua_pop(state, 1);
        }

        const auto readText = [&](const char* field, std::size_t maximum,
            bool required, std::string& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_isnil(state, -1) && !required)
            {
                lua_pop(state, 1);
                return true;
            }
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return false;
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            output.assign(value ? value : "", length);
            lua_pop(state, 1);
            return !output.empty() && output.size() <= maximum &&
                output.find('\0') == std::string::npos &&
                host.isValidUtf8(output);
        };
        if (taskName == "notification.show" ||
            taskName == "notification.schedule" ||
            taskName == "notification.update")
        {
            const bool required = taskName != "notification.update";
            std::string title;
            std::string message;
            if (!readText("title", 256, required, title))
                return luaL_error(state,
                    "task.start: %s title must contain 1 to 256 bytes of valid UTF-8",
                    taskName.c_str());
            if (!readText("message", 2048, required, message))
                return luaL_error(state,
                    "task.start: %s message must contain 1 to 2048 bytes of valid UTF-8",
                    taskName.c_str());
            if (!title.empty()) arguments.emplace("title", std::move(title));
            if (!message.empty()) arguments.emplace("message", std::move(message));
        }
        if (taskName == "notification.show" ||
            taskName == "notification.schedule" ||
            taskName == "notification.update")
        {
            lua_getfield(state, 2, "image");
            if (!lua_isnil(state, -1))
            {
                if (taskName == "notification.update" &&
                    lua_isboolean(state, -1) &&
                    lua_toboolean(state, -1) == 0)
                {
                    arguments.emplace("imageResource", "");
                }
                else
                {
                    const auto* handle = TestResourceHandle(state, -1);
                    const bool resolved = handle &&
                        handle->type == LuaResourceType::Image &&
                        !IsWidgetRuntimeImageToken(handle->name) &&
                        host.hasPackageImage(state, -1);
                    if (!resolved)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: %s image must be a package resource.image handle%s",
                            taskName.c_str(),
                            taskName == "notification.update"
                                ? " or false" : "");
                    }
                    arguments.emplace("imageResource", handle->name);
                }
            }
            lua_pop(state, 1);

            lua_getfield(state, 2, "progress");
            if (!lua_isnil(state, -1))
            {
                if (taskName == "notification.update" &&
                    lua_isboolean(state, -1) &&
                    lua_toboolean(state, -1) == 0)
                {
                    arguments.emplace("progress", "");
                }
                else
                {
                    int isNumber = 0;
                    const double progress =
                        lua_tonumberx(state, -1, &isNumber);
                    if (!isNumber || !std::isfinite(progress) ||
                        progress < 0.0 || progress > 1.0)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: %s progress must be between 0 and 1%s",
                            taskName.c_str(),
                            taskName == "notification.update"
                                ? " or false" : "");
                    }
                    char buffer[64]{};
                    const auto encoded = std::to_chars(
                        buffer, buffer + sizeof(buffer), progress,
                        std::chars_format::general);
                    if (encoded.ec != std::errc{})
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: notification progress cannot be represented");
                    }
                    arguments.emplace("progress",
                        std::string(buffer, encoded.ptr));
                }
            }
            lua_pop(state, 1);

            lua_getfield(state, 2, "actions");
            if (!lua_isnil(state, -1))
            {
                if (lua_type(state, -1) != LUA_TTABLE ||
                    lua_getmetatable(state, -1) != 0)
                {
                    if (lua_istable(state, -1)) lua_pop(state, 1);
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: %s actions must be a plain array",
                        taskName.c_str());
                }
                const int actionsTable = lua_absindex(state, -1);
                const std::size_t actionCount = lua_rawlen(
                    state, actionsTable);
                if (actionCount > 2)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: notifications accept at most 2 actions");
                }
                std::unordered_set<std::string> actionIds;
                for (std::size_t actionIndex = 1;
                    actionIndex <= actionCount; ++actionIndex)
                {
                    lua_rawgeti(state, actionsTable,
                        static_cast<lua_Integer>(actionIndex));
                    if (lua_type(state, -1) != LUA_TTABLE ||
                        lua_getmetatable(state, -1) != 0)
                    {
                        if (lua_istable(state, -1)) lua_pop(state, 1);
                        lua_pop(state, 2);
                        return luaL_error(state,
                            "task.start: notification actions must be plain tables");
                    }
                    const int actionTable = lua_absindex(state, -1);
                    lua_pushnil(state);
                    while (lua_next(state, actionTable) != 0)
                    {
                        if (lua_type(state, -2) != LUA_TSTRING)
                        {
                            lua_pop(state, 4);
                            return luaL_error(state,
                                "task.start: notification action keys must be strings");
                        }
                        size_t actionKeyLength = 0;
                        const char* actionKeyRaw = lua_tolstring(
                            state, -2, &actionKeyLength);
                        const std::string_view actionKey(
                            actionKeyRaw ? actionKeyRaw : "",
                            actionKeyLength);
                        if (actionKey != "id" && actionKey != "label")
                        {
                            lua_pop(state, 4);
                            return luaL_error(state,
                                "task.start: notification action received an unknown field");
                        }
                        lua_pop(state, 1);
                    }
                    const auto readActionField = [&](const char* field,
                        std::string& output) {
                        lua_getfield(state, actionTable, field);
                        if (lua_type(state, -1) != LUA_TSTRING)
                        {
                            lua_pop(state, 1);
                            return false;
                        }
                        size_t length = 0;
                        const char* value = lua_tolstring(
                            state, -1, &length);
                        output.assign(value ? value : "", length);
                        lua_pop(state, 1);
                        return !output.empty() && output.size() <= 64 &&
                            output.find('\0') == std::string::npos &&
                            host.isValidUtf8(output);
                    };
                    std::string actionId;
                    std::string actionLabel;
                    if (!readActionField("id", actionId) ||
                        !readActionField("label", actionLabel) ||
                        !actionIds.insert(actionId).second)
                    {
                        lua_pop(state, 2);
                        return luaL_error(state,
                            "task.start: notification action id and label must be unique non-empty UTF-8 strings up to 64 bytes");
                    }
                    arguments.emplace(
                        "action" + std::to_string(actionIndex - 1) + "Id",
                        std::move(actionId));
                    arguments.emplace(
                        "action" + std::to_string(actionIndex - 1) + "Label",
                        std::move(actionLabel));
                    lua_pop(state, 1);
                }
                lua_pushnil(state);
                std::size_t enumerated = 0;
                while (lua_next(state, actionsTable) != 0)
                {
                    ++enumerated;
                    const bool validIndex = lua_isinteger(state, -2) &&
                        lua_tointeger(state, -2) >= 1 &&
                        static_cast<std::size_t>(
                            lua_tointeger(state, -2)) <= actionCount;
                    lua_pop(state, 1);
                    if (!validIndex)
                    {
                        lua_pop(state, 2);
                        return luaL_error(state,
                            "task.start: notification actions must be a contiguous array");
                    }
                }
                if (enumerated != actionCount)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: notification actions must be a contiguous array");
                }
                arguments.emplace("actionCount",
                    std::to_string(actionCount));
            }
            lua_pop(state, 1);
            if (taskName == "notification.update" && arguments.empty())
                return luaL_error(state,
                    "task.start: notification.update requires a content field");
        }
        if (taskName == "notification.update" ||
            taskName == "notification.dismiss" ||
            taskName == "notification.cancel")
        {
            std::string notificationId;
            if (!readText("notificationId", 128, true, notificationId))
                return luaL_error(state,
                    "task.start: %s notificationId must contain 1 to 128 bytes of valid UTF-8",
                    taskName.c_str());
            arguments.emplace("notificationId", std::move(notificationId));
        }
        if (taskName == "notification.schedule")
        {
            lua_getfield(state, 2, "atMs");
            int isInteger = 0;
            const lua_Integer atMs =
                lua_tointegerx(state, -1, &isInteger);
            lua_pop(state, 1);
            if (!isInteger || atMs <= 0)
                return luaL_error(state,
                    "task.start: notification.schedule atMs must be a positive epoch millisecond integer");
            arguments.emplace("atMs", std::to_string(atMs));
        }
    }
    else if (taskName == "network.request")
    {
        constexpr std::size_t MaximumHeaders = 32;
        constexpr std::size_t MaximumHeaderNameBytes = 128;
        constexpr std::size_t MaximumHeaderValueBytes = 16 * 1024;
        constexpr std::size_t MaximumHeaderBytes = 32 * 1024;
        constexpr std::size_t MaximumBodyBytes = 64 * 1024;
        if (!hasArguments)
            return luaL_error(state,
                "task.start: network.request requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: network.request argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            if (key != "url" && key != "method" &&
                key != "headers" && key != "body" &&
                key != "timeoutMs" && key != "cacheSeconds" &&
                key != "maxBytes" && key != "retryDirect")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: network.request received an unknown argument");
            }
            lua_pop(state, 1);
        }

        const auto readRequiredUtf8 = [&](const char* field,
            std::size_t maximum, std::string& output) {
            lua_getfield(state, 2, field);
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return false;
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            output.assign(value ? value : "", length);
            lua_pop(state, 1);
            return !output.empty() && output.size() <= maximum &&
                output.find('\0') == std::string::npos &&
                host.isValidUtf8(output);
        };
        std::string url;
        if (!readRequiredUtf8("url", 2048, url))
        {
            return luaL_error(state,
                "task.start: network.request url must contain 1 to 2048 bytes of valid UTF-8");
        }
        arguments.emplace("url", std::move(url));

        std::string method = "GET";
        lua_getfield(state, 2, "method");
        if (!lua_isnil(state, -1))
        {
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: network.request method must be a string");
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            method.assign(value ? value : "", length);
            std::transform(method.begin(), method.end(), method.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::toupper(ch));
                });
        }
        lua_pop(state, 1);
        if (method != "GET" && method != "HEAD" &&
            method != "POST" && method != "PUT" &&
            method != "PATCH" && method != "DELETE")
        {
            return luaL_error(state,
                "task.start: network.request method is not supported");
        }
        arguments.emplace("method", method);

        const auto validHeaderName = [](std::string_view name) {
            if (name.empty() || name.size() > MaximumHeaderNameBytes)
                return false;
            if (!std::all_of(name.begin(), name.end(),
                [](unsigned char ch) {
                    return std::isalnum(ch) || ch == '!' || ch == '#' ||
                        ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
                        ch == '*' || ch == '+' || ch == '-' || ch == '.' ||
                        ch == '^' || ch == '_' || ch == '`' || ch == '|' ||
                        ch == '~';
                }))
                return false;
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            static constexpr std::array<std::string_view, 7> Forbidden{
                "connection", "content-length", "cookie", "host",
                "proxy-authorization", "te", "transfer-encoding" };
            return std::find(Forbidden.begin(), Forbidden.end(), lower) ==
                Forbidden.end();
        };
        const auto encodeSecretDescriptor = [&](int tableIndex,
            const std::string& prefix, std::size_t maximumAffixBytes,
            bool header, std::string& descriptorError) {
            tableIndex = lua_absindex(state, tableIndex);
            if (lua_type(state, tableIndex) != LUA_TTABLE ||
                lua_getmetatable(state, tableIndex) != 0)
            {
                if (lua_istable(state, tableIndex)) lua_pop(state, 1);
                descriptorError = "secret descriptor must be a plain table";
                return false;
            }
            lua_pushnil(state);
            while (lua_next(state, tableIndex) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    descriptorError =
                        "secret descriptor keys must be strings";
                    return false;
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                if (key != "secretRef" && key != "prefix" &&
                    key != "suffix")
                {
                    lua_pop(state, 2);
                    descriptorError =
                        "secret descriptor received an unknown field";
                    return false;
                }
                lua_pop(state, 1);
            }
            const auto readDescriptorField = [&](const char* field,
                bool required, std::string& output) {
                lua_getfield(state, tableIndex, field);
                if (lua_isnil(state, -1) && !required)
                {
                    lua_pop(state, 1);
                    output.clear();
                    return true;
                }
                if (lua_type(state, -1) != LUA_TSTRING)
                {
                    lua_pop(state, 1);
                    return false;
                }
                size_t length = 0;
                const char* value = lua_tolstring(state, -1, &length);
                output.assign(value ? value : "", length);
                lua_pop(state, 1);
                return true;
            };
            std::string reference;
            std::string valuePrefix;
            std::string valueSuffix;
            if (!readDescriptorField("secretRef", true, reference) ||
                !snowdesktop::widget_runtime::WidgetSecretStore::
                    IsReference(reference) ||
                !readDescriptorField("prefix", false, valuePrefix) ||
                !readDescriptorField("suffix", false, valueSuffix) ||
                valuePrefix.size() > maximumAffixBytes ||
                valueSuffix.size() > maximumAffixBytes -
                    valuePrefix.size())
            {
                descriptorError =
                    "secret descriptor fields are invalid or too large";
                return false;
            }
            if (header &&
                (valuePrefix.find_first_of("\r\n\0", 0, 3) !=
                        std::string::npos ||
                    valueSuffix.find_first_of("\r\n\0", 0, 3) !=
                        std::string::npos ||
                    (!valuePrefix.empty() && !host.isValidUtf8(valuePrefix)) ||
                    (!valueSuffix.empty() && !host.isValidUtf8(valueSuffix))))
            {
                descriptorError =
                    "secret header affixes must be valid single-line UTF-8";
                return false;
            }
            arguments.emplace(prefix + "kind", "secret");
            arguments.emplace(prefix + "ref", std::move(reference));
            arguments.emplace(prefix + "prefix", std::move(valuePrefix));
            arguments.emplace(prefix + "suffix", std::move(valueSuffix));
            arguments.insert_or_assign("usesSecret", "1");
            return true;
        };

        std::size_t headerCount = 0;
        std::size_t headerBytes = 0;
        std::unordered_set<std::string> normalizedHeaderNames;
        lua_getfield(state, 2, "headers");
        if (!lua_isnil(state, -1))
        {
            if (lua_type(state, -1) != LUA_TTABLE ||
                lua_getmetatable(state, -1) != 0)
            {
                if (lua_istable(state, -1)) lua_pop(state, 1);
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: network.request headers must be a plain table");
            }
            const int headersTable = lua_absindex(state, -1);
            lua_pushnil(state);
            while (lua_next(state, headersTable) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING ||
                    headerCount >= MaximumHeaders)
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "task.start: network.request accepts at most 32 string-keyed headers");
                }
                size_t headerNameLength = 0;
                const char* nameValue = lua_tolstring(
                    state, -2, &headerNameLength);
                std::string headerName(
                    nameValue ? nameValue : "", headerNameLength);
                std::string normalizedHeaderName = headerName;
                std::transform(normalizedHeaderName.begin(),
                    normalizedHeaderName.end(),
                    normalizedHeaderName.begin(),
                    [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                if (!validHeaderName(headerName) ||
                    !normalizedHeaderNames.insert(
                        normalizedHeaderName).second)
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "task.start: network.request header name is invalid");
                }
                const std::string fieldPrefix = "header." +
                    std::to_string(headerCount) + ".";
                arguments.emplace(fieldPrefix + "name", headerName);
                headerBytes += headerName.size() + 4;
                if (lua_type(state, -1) == LUA_TSTRING)
                {
                    if (normalizedHeaderName == "authorization" ||
                        normalizedHeaderName == "api-key" ||
                        normalizedHeaderName == "x-api-key")
                    {
                        lua_pop(state, 3);
                        return luaL_error(state,
                            "task.start: credential headers require a secret descriptor");
                    }
                    size_t valueLength = 0;
                    const char* value = lua_tolstring(
                        state, -1, &valueLength);
                    std::string headerValue(
                        value ? value : "", valueLength);
                    if (headerValue.size() > MaximumHeaderValueBytes ||
                        headerValue.find_first_of("\r\n\0", 0, 3) !=
                            std::string::npos ||
                        !host.isValidUtf8(headerValue))
                    {
                        lua_pop(state, 3);
                        return luaL_error(state,
                            "task.start: network.request header values must be bounded single-line UTF-8 strings");
                    }
                    headerBytes += headerValue.size();
                    arguments.emplace(fieldPrefix + "kind", "raw");
                    arguments.emplace(fieldPrefix + "value",
                        std::move(headerValue));
                }
                else if (lua_type(state, -1) == LUA_TTABLE)
                {
                    std::string descriptorError;
                    if (!encodeSecretDescriptor(-1, fieldPrefix,
                            MaximumHeaderValueBytes, true,
                            descriptorError))
                    {
                        lua_pop(state, 3);
                        lua_pushfstring(state,
                            "task.start: network.request header secret descriptor is invalid: %s",
                            descriptorError.c_str());
                        return -1;
                    }
                    headerBytes += arguments[fieldPrefix + "prefix"].size() +
                        arguments[fieldPrefix + "suffix"].size();
                }
                else
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "task.start: network.request header values must be strings or secret descriptors");
                }
                if (headerBytes > MaximumHeaderBytes)
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "task.start: network.request headers exceed the 32 KiB limit");
                }
                ++headerCount;
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 1);
        arguments.emplace("headerCount", std::to_string(headerCount));

        lua_getfield(state, 2, "body");
        if (lua_isnil(state, -1))
        {
            arguments.emplace("body.kind", "raw");
            arguments.emplace("body.value", "");
        }
        else if (lua_type(state, -1) == LUA_TSTRING)
        {
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            if (length > MaximumBodyBytes)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: network.request body exceeds the 64 KiB limit");
            }
            arguments.emplace("body.kind", "raw");
            arguments.emplace("body.value",
                std::string(value ? value : "", length));
        }
        else if (lua_type(state, -1) == LUA_TTABLE)
        {
            std::string descriptorError;
            if (!encodeSecretDescriptor(-1, "body.",
                    MaximumBodyBytes, false, descriptorError))
            {
                lua_pop(state, 1);
                lua_pushfstring(state,
                    "task.start: network.request body secret descriptor is invalid: %s",
                    descriptorError.c_str());
                return -1;
            }
        }
        else
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: network.request body must be a string or secret descriptor");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "retryDirect");
        if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: network.request retryDirect must be a boolean");
        }
        const bool retryDirect = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        if (retryDirect && ((method != "GET" && method != "HEAD") ||
                arguments.at("body.kind") != "raw" ||
                !arguments.at("body.value").empty()))
        {
            return luaL_error(state,
                "task.start: network.request retryDirect requires a bodyless GET or HEAD");
        }
        arguments.emplace("retryDirect", retryDirect ? "true" : "false");

        const auto readBoundedInteger = [&](const char* field,
            lua_Integer fallback, lua_Integer minimum,
            lua_Integer maximum, lua_Integer& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_isnil(state, -1))
                output = fallback;
            else if (lua_isinteger(state, -1))
                output = lua_tointeger(state, -1);
            else
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pop(state, 1);
            return output >= minimum && output <= maximum;
        };
        lua_Integer timeoutMs = 15000;
        lua_Integer cacheSeconds = 0;
        lua_Integer maxBytes = 512 * 1024;
        if (!readBoundedInteger(
                "timeoutMs", 15000, 1000, 30000, timeoutMs) ||
            !readBoundedInteger(
                "cacheSeconds", 0, 0, 86400, cacheSeconds) ||
            !readBoundedInteger(
                "maxBytes", 512 * 1024, 4096,
                HttpRequestOptions::MaximumResponseBytes,
                maxBytes))
        {
            return luaL_error(state,
                "task.start: network.request numeric options are out of range");
        }
        arguments.emplace("timeoutMs", std::to_string(timeoutMs));
        arguments.emplace("cacheSeconds", std::to_string(cacheSeconds));
        arguments.emplace("maxBytes", std::to_string(maxBytes));
    }
    else if (taskName == "shell.openUri")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: shell.openUri requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING ||
                std::string_view(lua_tostring(state, -2)) != "url")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: shell.openUri accepts only url");
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "url");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: shell.openUri url must be a string");
        }
        size_t urlLength = 0;
        const char* urlValue = lua_tolstring(state, -1, &urlLength);
        std::string url(urlValue ? urlValue : "", urlLength);
        lua_pop(state, 1);
        if (url.empty() || url.size() > 2048 ||
            url.find('\0') != std::string::npos ||
            !host.isValidUtf8(url))
        {
            return luaL_error(state,
                "task.start: shell.openUri url must contain 1 to 2048 bytes of valid UTF-8");
        }
        arguments.emplace("url", std::move(url));
    }
    else if (taskName == "calendar.create" ||
        taskName == "calendar.update" ||
        taskName == "calendar.remove" ||
        taskName == "calendar.series.create" ||
        taskName == "calendar.series.update" ||
        taskName == "calendar.series.remove")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        const bool seriesTask = taskName.starts_with("calendar.series.");
        const bool update = taskName == "calendar.update" ||
            taskName == "calendar.series.update";
        const bool remove = taskName == "calendar.remove" ||
            taskName == "calendar.series.remove";
        const bool create = !update && !remove;
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: calendar argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            const bool known = key == "id" ||
                (seriesTask && remove && key == "expectedRevision") ||
                (seriesTask && (key == "kind" || key == "dates" ||
                    key == "startDate" || key == "endDate" ||
                    key == "interval" || key == "weekdays" ||
                    key == "monthDay")) ||
                (!remove && (key == "title" || key == "date" ||
                    key == "allDay" || key == "startMinutes" ||
                    key == "endMinutes" || key == "notes" ||
                    key == "reminderMinutes" ||
                    key == "expectedRevision"));
            const bool allowed = known &&
                (update || (seriesTask && remove) || key != "expectedRevision") &&
                (!create || key != "id") &&
                (!remove || key == "id" ||
                    (seriesTask && key == "expectedRevision"));
            if (!allowed)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    taskName.c_str());
            }
            lua_pop(state, 1);
        }

        const auto readText = [&](const char* field,
            std::size_t maximum, bool allowEmpty,
            std::string& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return false;
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            output.assign(value ? value : "", length);
            lua_pop(state, 1);
            return output.size() <= maximum &&
                (allowEmpty || !output.empty()) &&
                output.find('\0') == std::string::npos &&
                (output.empty() || host.isValidUtf8(output));
        };
        const auto readInteger = [&](const char* field,
            lua_Integer& output) -> bool {
            lua_getfield(state, 2, field);
            const bool valid = lua_isinteger(state, -1);
            if (valid) output = lua_tointeger(state, -1);
            lua_pop(state, 1);
            return valid;
        };

        if (update || remove)
        {
            std::string id;
            if (!readText("id", 128, false, id))
                return luaL_error(state,
                    "task.start: %s id must contain 1 to 128 bytes of valid UTF-8",
                    taskName.c_str());
            arguments.emplace("id", std::move(id));
        }
        if (seriesTask)
        {
            std::string kind;
            std::string startDate;
            std::string endDate;
            if (!remove)
            {
                if (!readText("kind", 16, false, kind) ||
                    (kind != "dates" && kind != "weekly" && kind != "monthly") ||
                    !readText("startDate", 10, false, startDate) ||
                    !snowdesktop::calendar::CalendarService::GetDateInfo(startDate) ||
                    !readText("endDate", 10, true, endDate) ||
                    (!endDate.empty() &&
                        !snowdesktop::calendar::CalendarService::GetDateInfo(endDate)))
                    return luaL_error(state, "task.start: calendar series rule is invalid");
                arguments.emplace("kind", kind);
                arguments.emplace("startDate", startDate);
                arguments.emplace("endDate", endDate);
                lua_Integer interval = 0;
                lua_Integer monthDay = 0;
                if (!readInteger("interval", interval) || interval < 1 || interval > 99 ||
                    !readInteger("monthDay", monthDay) || monthDay < 0 || monthDay > 31)
                    return luaL_error(state, "task.start: calendar series interval or monthDay is invalid");
                arguments.emplace("interval", std::to_string(interval));
                arguments.emplace("monthDay", std::to_string(monthDay));
                const auto readArray = [&](const char* field, bool dates,
                    std::size_t maximum) -> bool {
                    lua_getfield(state, 2, field);
                    if (!lua_istable(state, -1)) { lua_pop(state, 1); return false; }
                    const std::size_t count = lua_rawlen(state, -1);
                    if (count > maximum) { lua_pop(state, 1); return false; }
                    std::string encoded;
                    for (std::size_t index = 1; index <= count; ++index)
                    {
                        lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
                        if (dates)
                        {
                            size_t length = 0;
                            const char* raw = lua_tolstring(state, -1, &length);
                            const std::string value(raw ? raw : "", length);
                            if (lua_type(state, -1) != LUA_TSTRING ||
                                !snowdesktop::calendar::CalendarService::GetDateInfo(value))
                            { lua_pop(state, 2); return false; }
                            if (!encoded.empty()) encoded.push_back(',');
                            encoded += value;
                        }
                        else
                        {
                            if (!lua_isinteger(state, -1) ||
                                lua_tointeger(state, -1) < 1 ||
                                lua_tointeger(state, -1) > 7)
                            { lua_pop(state, 2); return false; }
                            if (!encoded.empty()) encoded.push_back(',');
                            encoded += std::to_string(lua_tointeger(state, -1));
                        }
                        lua_pop(state, 1);
                    }
                    lua_pop(state, 1);
                    arguments.emplace(field, std::move(encoded));
                    return true;
                };
                if (!readArray("dates", true, 366) ||
                    !readArray("weekdays", false, 7))
                    return luaL_error(state, "task.start: calendar series dates or weekdays is invalid");
            }
        }
        if (!remove)
        {
            std::string title;
            std::string date;
            std::string notes;
            if (!readText("title", 512, false, title))
                return luaL_error(state,
                    "task.start: calendar title must contain 1 to 512 bytes of valid UTF-8");
            if (!readText("date", 10, false, date) ||
                !snowdesktop::calendar::CalendarService::GetDateInfo(date))
                return luaL_error(state,
                    "task.start: calendar date must be a valid YYYY-MM-DD date");
            if (!readText("notes", 8192, true, notes))
                return luaL_error(state,
                    "task.start: calendar notes must contain at most 8192 bytes of valid UTF-8");

            lua_getfield(state, 2, "allDay");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: calendar allDay must be a boolean");
            }
            const bool allDay = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);

            lua_Integer startMinutes = 0;
            lua_Integer endMinutes = 0;
            lua_Integer reminderMinutes = -1;
            if (!readInteger("startMinutes", startMinutes) ||
                startMinutes < 0 || startMinutes > 1439 ||
                !readInteger("endMinutes", endMinutes) ||
                endMinutes < 0 || endMinutes > 1439 ||
                (!allDay && endMinutes < startMinutes))
            {
                return luaL_error(state,
                    "task.start: calendar time range is invalid");
            }
            if (!readInteger("reminderMinutes", reminderMinutes) ||
                (reminderMinutes != -1 && reminderMinutes != 0 &&
                    reminderMinutes != 5 && reminderMinutes != 15 &&
                    reminderMinutes != 30 && reminderMinutes != 60 &&
                    reminderMinutes != 1440))
            {
                return luaL_error(state,
                    "task.start: calendar reminderMinutes is unsupported");
            }

            arguments.emplace("title", std::move(title));
            arguments.emplace("date", std::move(date));
            arguments.emplace("notes", std::move(notes));
            arguments.emplace("allDay", allDay ? "1" : "0");
            arguments.emplace("startMinutes", std::to_string(startMinutes));
            arguments.emplace("endMinutes", std::to_string(endMinutes));
            arguments.emplace("reminderMinutes",
                std::to_string(reminderMinutes));

            if (update)
            {
                lua_Integer expectedRevision = 0;
                if (!readInteger("expectedRevision", expectedRevision) ||
                    expectedRevision <= 0 ||
                    expectedRevision > std::numeric_limits<int>::max())
                {
                    return luaL_error(state,
                        "task.start: calendar expectedRevision must be a positive integer");
                }
                arguments.emplace("expectedRevision",
                    std::to_string(expectedRevision));
            }
        }
        else if (seriesTask)
        {
            lua_Integer expectedRevision = 0;
            if (!readInteger("expectedRevision", expectedRevision) ||
                expectedRevision <= 0 ||
                expectedRevision > std::numeric_limits<int>::max())
                return luaL_error(state,
                    "task.start: calendar series expectedRevision must be a positive integer");
            arguments.emplace("expectedRevision", std::to_string(expectedRevision));
        }
    }
    else if (hasArguments)
    {
        lua_pushnil(state);
        if (lua_next(state, 2) != 0)
        {
            lua_pop(state, 2);
            return luaL_error(state,
                "task.start: this task does not accept arguments");
        }
    }

    auto result = host.startTask(state, taskName, std::move(arguments));
    if (!result)
        return luaL_error(state, "task.start: host is unavailable");
    if (!*result)
    {
        lua_pushnil(state);
        lua_pushlstring(state, result->error.data(), result->error.size());
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(result->id));
    return 1;
}

}

int StartTaskFromLua(lua_State* state, const LuaTaskStartHost& host)
{
    const int result = LuaTaskStartWithDeferredSecretError(state, host);
    // Lua uses longjmp in this build. Finish the secret descriptor parser's
    // C++ string/container lifetimes before raising its validation error.
    return result < 0 ? lua_error(state) : result;
}
}
