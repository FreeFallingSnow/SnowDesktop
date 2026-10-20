#include "widget/runtime/lua_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

namespace
{
void Require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}

// Load the shipped entry and call its real setup/event/render functions.
// Only external host services and outcomes are substituted; expectations and
// assertions stay in C++, independent of the component's parsing algorithm.
class RssVm
{
public:
    explicit RssVm(const std::filesystem::path& repository,
        bool largeResponse = true, bool headers = true, bool deny = false)
    {
        state_ = lua_newstate(Allocate, this);
        Require(state_ != nullptr, "RSS Lua state is available");
        luaL_openlibs(state_);
        lua_pushlightuserdata(state_, &quota_);
        lua_setfield(state_, LUA_REGISTRYINDEX, "__quota_ptr");
        for (const auto& [name, enabled] : std::vector<std::pair<const char*, bool>>{
                {"_large", largeResponse}, {"_headers", headers}, {"_deny", deny}})
        {
            lua_pushboolean(state_, enabled);
            lua_setglobal(state_, name);
        }
        Run(R"lua(
_drawn = {}; _requestCount = 0
widget = {define=function(d) return d end, setTitle=function() end,
    hasFeature=function(name)
        if name == "task.network.largeResponse" then return _large end
        if name == "task.network.headers" then return _headers end
        return false
    end}
l10n = {tr=function(key, value)
    return value and (key .. "|" .. tostring(value)) or key
end}
storage = {get=function() return nil end}
schedule = {every=function() end}
task = {start=function(_, args)
    _args = args
    if _deny then return nil, "permissionDenied" end
    _requestCount = _requestCount + 1
    return _requestCount
end}
time = {monotonic=function() return 0 end}
ui = {metrics=function() return {layoutRowHeight=28} end}
layout = {contentWidth=function() return 420 end, contentHeight=function() return 280 end}
interaction = {setScrollOffset=function() end, region=function() end,
    scroll=function() return {offset=0} end, isHovered=function() return false end}
draw = {measureText=function(_, size) return {width=80, height=size} end,
    text=function(_, _, value) _drawn[#_drawn + 1] = value end,
    line=function() end, pushClip=function() end, popClip=function() end}
)lua");
        const auto path = repository / "widgets/rss-reader/main.lua";
        Require(luaL_loadfile(state_, path.string().c_str()) == LUA_OK,
            "production RSS entry loads");
        Call(0, 1);
        lua_setglobal(state_, "descriptor");
        Run("model = descriptor.setup({preview=false})");
    }
    ~RssVm() { if (state_) lua_close(state_); }
    RssVm(const RssVm&) = delete;
    RssVm& operator=(const RssVm&) = delete;

    void Complete(std::string_view body, const char* error = nullptr, int status = 200)
    {
        lua_getglobal(state_, "descriptor");
        lua_getfield(state_, -1, "event");
        lua_remove(state_, -2);
        lua_newtable(state_);
        lua_getglobal(state_, "model");
        lua_newtable(state_);
        SetText("kind", "task.complete");
        lua_pushinteger(state_, static_cast<lua_Integer>(Number("requestId")));
        lua_setfield(state_, -2, "taskId");
        lua_pushboolean(state_, error == nullptr);
        lua_setfield(state_, -2, "ok");
        if (error)
        {
            SetText("error", error);
            lua_pushinteger(state_, status);
            lua_setfield(state_, -2, "status");
        }
        else
        {
            lua_newtable(state_);
            SetText("body", body);
            lua_pushinteger(state_, status);
            lua_setfield(state_, -2, "status");
            lua_setfield(state_, -2, "value");
        }
        Call(3, 0);
    }
    void Refresh() { Run("descriptor.event({}, model, {kind='action', id='rss.refresh'})"); }
    std::string Text(const char* key)
    {
        lua_getglobal(state_, "model");
        lua_getfield(state_, -1, key);
        const char* value = lua_tostring(state_, -1);
        std::string result = value ? value : "";
        lua_pop(state_, 2);
        return result;
    }
    double Number(const char* key)
    {
        lua_getglobal(state_, "model");
        lua_getfield(state_, -1, key);
        const double result = lua_tonumber(state_, -1);
        lua_pop(state_, 2);
        return result;
    }
    std::string Article(const char* key)
    {
        lua_getglobal(state_, "model");
        lua_getfield(state_, -1, "articles");
        lua_rawgeti(state_, -1, 1);
        lua_getfield(state_, -1, key);
        const char* value = lua_tostring(state_, -1);
        std::string result = value ? value : "";
        lua_pop(state_, 4);
        return result;
    }
    std::size_t Count()
    {
        lua_getglobal(state_, "model");
        lua_getfield(state_, -1, "articles");
        const auto count = lua_rawlen(state_, -1);
        lua_pop(state_, 2);
        return count;
    }
    bool Renders(std::string_view text)
    {
        Run("_drawn = {}; descriptor.render({}, model)");
        lua_getglobal(state_, "_drawn");
        bool found = false;
        for (std::size_t index = 1; index <= lua_rawlen(state_, -1); ++index)
        {
            lua_rawgeti(state_, -1, static_cast<lua_Integer>(index));
            const char* value = lua_tostring(state_, -1);
            found = found || (value && value == text);
            lua_pop(state_, 1);
        }
        lua_pop(state_, 1);
        return found;
    }
    std::size_t RequestedBytes()
    {
        lua_getglobal(state_, "_args");
        lua_getfield(state_, -1, "maxBytes");
        const auto result = static_cast<std::size_t>(lua_tointeger(state_, -1));
        lua_pop(state_, 2);
        return result;
    }
    bool HasHeaders()
    {
        lua_getglobal(state_, "_args");
        lua_getfield(state_, -1, "headers");
        const bool present = lua_istable(state_, -1);
        lua_pop(state_, 2);
        return present;
    }
    std::string RequestedHeader(const char* key)
    {
        lua_getglobal(state_, "_args");
        lua_getfield(state_, -1, "headers");
        lua_getfield(state_, -1, key);
        const char* value = lua_tostring(state_, -1);
        std::string result = value ? value : "";
        lua_pop(state_, 3);
        return result;
    }
    std::size_t PeakMemory() const { return peakMemory_; }

private:
    static void* Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize)
    {
        auto* owner = static_cast<RssVm*>(context);
        auto* quota = &owner->quota_;
        const auto retained = quota->memoryBytes - (pointer ? oldSize : 0);
        if (newSize == 0)
        {
            std::free(pointer);
            quota->memoryBytes = retained;
            return nullptr;
        }
        if (newSize > quota->memoryLimit || retained > quota->memoryLimit - newSize)
        {
            quota->memoryExceeded = true;
            return nullptr;
        }
        void* result = std::realloc(pointer, newSize);
        if (result)
        {
            quota->memoryBytes = retained + newSize;
            owner->peakMemory_ = std::max(owner->peakMemory_, quota->memoryBytes);
        }
        return result;
    }
    void SetText(const char* key, std::string_view value)
    {
        lua_pushlstring(state_, value.data(), value.size());
        lua_setfield(state_, -2, key);
    }
    void Run(const char* code)
    {
        Require(luaL_loadstring(state_, code) == LUA_OK, "RSS host fixture compiles");
        Call(0, 0);
    }
    void Call(int arguments, int results)
    {
        const int code = snowdesktop::lua_runtime::ProtectedCall(state_, arguments, results);
        peakMemory_ = std::max(peakMemory_, quota_.memoryBytes);
        const char* error = code == LUA_OK ? nullptr : lua_tostring(state_, -1);
        Require(code == LUA_OK, std::string("RSS production callback failed: ") + (error ? error : "unknown"));
        Require(!quota_.memoryExceeded && !quota_.executionExceeded,
            "RSS callbacks stay within the production Lua quotas");
    }
    LuaRuntimeQuota quota_;
    lua_State* state_ = nullptr;
    std::size_t peakMemory_ = 0;
};
}

void TestRssReaderBehavior(const std::filesystem::path& repository)
{
    RssVm reader(repository);
    reader.Complete(R"xml(<rss><channel xmlns:content="urn:content"><title>News &#8211; &amp; Updates</title>
<item><title type="text"><![CDATA[Rock &amp; Roll &#x1F642; &apos;]]></title>
<link>https://example.com/article?a=1&amp;b=2</link><pubDate>Today</pubDate></item></channel></rss>)xml");
    Require(reader.Text("feedTitle") == "News – & Updates",
        "channel attributes and decimal entities retain the feed title");
    Require(reader.Count() == 1 && reader.Article("title") == "Rock & Roll 🙂 '",
        "RSS title attributes, CDATA, hexadecimal entities and apostrophes decode");
    Require(reader.Article("link") == "https://example.com/article?a=1&b=2",
        "article URL entities preserve the original query");
    reader.Refresh();
    reader.Complete("", "httpStatus", 403);
    Require(reader.Number("requestId") != 0 && reader.Text("error").empty() &&
            reader.RequestedHeader("User-Agent").find("SnowDesktopRSS/1.0") != std::string::npos,
        "a refusal starts one compatible-UA read retry");
    reader.Complete("", "httpStatus", 403);
    Require(reader.Number("errorStatus") == 403 && reader.Count() == 1 &&
            reader.Renders("lua_widget.rss_reader.http_error|403"),
        "HTTP refusal is visible with its status while preserving previous articles");
    reader.Refresh();
    reader.Complete("<html>Unavailable</html>");
    Require(reader.Count() == 1 && reader.Text("error") == "parseFailed" &&
            reader.Renders("lua_widget.rss_reader.no_articles_parsed"),
        "a non-feed refresh cannot discard previously downloaded articles");
    reader.Refresh();
    reader.Complete("<feed><title>Atom</title><entry><title type='text'>Entry</title>"
        "<link href='https://example.com/atom'/><updated>2026-10-14</updated></entry></feed>");
    Require(reader.Text("error").empty() && reader.Number("errorStatus") == 0 &&
            reader.Text("feedTitle") == "Atom" && reader.Article("title") == "Entry",
        "a successful Atom refresh clears the prior failure and replaces content");

    for (const auto& [error, message] : std::vector<std::pair<const char*, const char*>>{
            {"httpStatus", "http_error|503"}, {"responseTooLarge", "response_too_large|4"},
            {"permissionDenied", "network_permission_required"},
            {"permissionRevoked", "network_permission_required"},
            {"redirectRejected", "redirect_rejected"}, {"networkError", "request_failed"}})
    {
        RssVm failure(repository);
        failure.Complete("", error, 503);
        Require(failure.Renders(std::string("lua_widget.rss_reader.") + message),
            std::string("RSS renders the specific failure: ") + error);
    }
    RssVm empty(repository);
    empty.Complete("");
    Require(empty.Renders("lua_widget.rss_reader.empty_response"), "empty responses have their own message");
    RssVm denied(repository, true, true, true);
    Require(denied.Renders("lua_widget.rss_reader.network_permission_required"),
        "a rejected initial task displays the missing network permission");

    Require(reader.RequestedBytes() == 4 * 1024 * 1024 && reader.HasHeaders(),
        "new hosts request a bounded large feed with compatible request headers");
    Require(reader.RequestedHeader("User-Agent").empty(),
        "ordinary refreshes retain the host User-Agent");
    RssVm limited(repository);
    limited.Complete("", "httpStatus", 429);
    Require(limited.Number("requestId") == 0 && limited.Renders("lua_widget.rss_reader.http_error|429"),
        "rate-limited feeds do not trigger a compatibility retry");
    RssVm oldHost(repository, false, false);
    Require(oldHost.RequestedBytes() == 1024 * 1024 && !oldHost.HasHeaders(),
        "older hosts receive only supported request arguments and byte ceilings");
    oldHost.Complete("", "responseTooLarge");
    Require(oldHost.Renders("lua_widget.rss_reader.response_too_large|1"),
        "the size error describes the ceiling actually requested on the old host");

    RssVm entities(repository);
    entities.Complete("<rss><channel><title>Entities</title><item>"
        "<title>&amp;#8211; &#0; &#xD800; &#x110000; &#xZZ; &#999999999999999999999999;</title>"
        "<link>https://example.com</link></item></channel></rss>");
    Require(entities.Article("title") ==
        "&#8211; &#0; &#xD800; &#x110000; &#xZZ; &#999999999999999999999999;",
        "entities decode once; invalid Unicode cannot crash the callback");

    // Match the observed two-MiB full-text feed without retaining article bodies.
    std::string fullText = "<rss><channel><title>Large feed</title>";
    for (int index = 0; index < 30; ++index)
        fullText += "<item><title>Headline</title><link>https://example.com/article</link>"
            "<description>" + std::string(72 * 1024, 'x') + "</description></item>";
    fullText += "</channel></rss>";
    RssVm large(repository);
    large.Complete(fullText);
    Require(large.Count() == 30 && large.Text("error").empty() &&
            large.Article("description").empty() && large.PeakMemory() < 16 * 1024 * 1024,
        "two-MiB feeds parse within the host quota without retaining unused full text");
    std::cout << "RSS entry lifecycle, failure rendering, compatibility and large-feed quota checks passed\n";
}
