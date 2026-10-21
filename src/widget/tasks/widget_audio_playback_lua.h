#pragma once
#include "widget_audio_playback.h"
#include "widget_filesystem_task_executor.h"
#include "widget/data/widget_resource_lua.h"
#include <cmath>
#include <charconv>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
// Validation does not resolve paths or perform I/O on the Lua thread.
inline bool ReadAudioPlaybackArguments(lua_State* state, const std::string& action,
    std::unordered_map<std::string, std::string>& result)
{
    if (!lua_istable(state, 2)) return false;
    const bool play = action == "audio.play";
    lua_pushnil(state);
    while (lua_next(state, 2))
    {
        if (lua_type(state, -2) != LUA_TSTRING) { lua_pop(state, 2); return false; }
        size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        const std::string key(text, length);
        bool valid = false;
        if (play && key == "source")
        {
            const auto* resource = TestResourceHandle(state, -1);
            valid = resource && resource->type == LuaResourceType::Audio;
            if (valid) result["resource"] = resource->name;
        }
        else if ((play && (key == "handle" || key == "name")) || (!play && key == "playbackId"))
        {
            size_t size = 0;
            const char* value = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &size) : nullptr;
            valid = value && size > 0 && size <= 1024 && std::string_view(value, size).find('\0') == std::string_view::npos;
            if (valid) result[key] = std::string(value, size);
        }
        else if ((play || action == "audio.setVolume") && key == "volume")
        {
            const double value = lua_type(state, -1) == LUA_TNUMBER ? lua_tonumber(state, -1) : -1;
            valid = std::isfinite(value) && value >= 0 && value <= 1;
            if (valid) { char buffer[64]; const auto converted = std::to_chars(buffer, buffer + 64, value); result[key] = std::string(buffer, converted.ptr); }
        }
        else if ((play || action == "audio.setLoop") && key == "loop")
        {
            valid = lua_isboolean(state, -1);
            if (valid) result[key] = lua_toboolean(state, -1) ? "1" : "0";
        }
        else if (action == "audio.seek" && key == "positionMs")
        {
            const double value = lua_type(state, -1) == LUA_TNUMBER ? lua_tonumber(state, -1) : -1;
            valid = std::isfinite(value) && value >= 0 && value <= INT64_MAX / 10000 && std::floor(value) == value;
            if (valid) result[key] = std::to_string(static_cast<std::int64_t>(value));
        }
        lua_pop(state, 1);
        if (!valid) { lua_pop(state, 1); return false; }
    }
    if (play)
        return (result.contains("resource") != result.contains("handle")) &&
            (!result.contains("name") || (result.contains("handle") && WidgetFilesystemTaskExecutor::IsDirectChildName(result.at("name"))));
    return result.contains("playbackId") && result.at("playbackId").starts_with("audio-") && result.at("playbackId").size() <= 64 &&
        (action != "audio.seek" || result.contains("positionMs")) &&
        (action != "audio.setVolume" || result.contains("volume")) &&
        (action != "audio.setLoop" || result.contains("loop"));
}
}
