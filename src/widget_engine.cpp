#include "widget_engine.h"
#include "utils.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "windowscodecs.lib")

static std::string ReadTextFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::string WidgetWideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

static std::wstring Utf8ToWideLocal(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}

static bool JsonReadString(const std::string& text, const char* field, std::string& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    p = text.find('"', p + 1);
    if (p == std::string::npos) return false;
    std::string result;
    for (++p; p < text.size(); ++p)
    {
        char ch = text[p];
        if (ch == '"') { out = result; return true; }
        if (ch == '\\' && p + 1 < text.size())
        {
            char esc = text[++p];
            switch (esc)
            {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            default: result.push_back(esc); break;
            }
        }
        else
        {
            result.push_back(ch);
        }
    }
    return false;
}

static bool JsonReadInt(const std::string& text, const char* field, int& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    size_t start = p;
    if (p < text.size() && (text[p] == '-' || text[p] == '+')) ++p;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
    if (p == start) return false;
    out = std::atoi(text.substr(start, p - start).c_str());
    return true;
}

static std::vector<std::string> JsonReadStringArray(const std::string& text, const char* field)
{
    std::vector<std::string> result;
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return result;
    p = text.find('[', p + marker.size());
    if (p == std::string::npos) return result;
    size_t end = text.find(']', p + 1);
    if (end == std::string::npos) return result;
    for (++p; p < end; ++p)
    {
        if (text[p] != '"') continue;
        std::string value;
        for (++p; p < end; ++p)
        {
            if (text[p] == '"') break;
            if (text[p] == '\\' && p + 1 < end)
                value.push_back(text[++p]);
            else
                value.push_back(text[p]);
        }
        if (!value.empty()) result.push_back(value);
    }
    return result;
}

static std::wstring GetExeWidgetsDir()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"widgets");
    return exePath;
}

static std::wstring ResolveWidgetPath(const std::wstring& scriptPath)
{
    std::wstring fullPath = GetExeWidgetsDir();
    fullPath += L"\\";
    fullPath += scriptPath;
    return fullPath;
}

static std::wstring ManifestPathForScriptFile(const std::wstring& fullScriptPath)
{
    std::wstring path = fullScriptPath;
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (ext && _wcsicmp(ext, L".lua") == 0)
        path.resize(static_cast<size_t>(ext - path.c_str()));
    path += L".widget.json";
    return path;
}

// ── Storage (shared localStorage for Lua widgets) ────────────────
static std::unordered_map<std::string, std::string> g_storage;
static std::wstring g_storagePath;
static std::deque<WidgetLogEntry> g_widgetLogs;

static void LoadStorageFile()
{
    g_storage.clear();
    if (g_storagePath.empty()) return;
    std::ifstream file(g_storagePath, std::ios::binary);
    if (!file) return;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    size_t pos = text.find('{');
    if (pos == std::string::npos) return;
    while (true)
    {
        pos = text.find('"', pos);
        if (pos == std::string::npos) break;
        size_t keyEnd = text.find('"', pos + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = text.substr(pos + 1, keyEnd - pos - 1);
        pos = text.find(':', keyEnd + 1);
        if (pos == std::string::npos) break;
        pos = text.find('"', pos + 1);
        if (pos == std::string::npos) break;
        size_t valEnd = text.find('"', pos + 1);
        if (valEnd == std::string::npos) break;
        g_storage[key] = text.substr(pos + 1, valEnd - pos - 1);
        pos = valEnd + 1;
    }
}

static void SaveStorageFile()
{
    if (g_storagePath.empty()) return;
    std::ofstream file(g_storagePath, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "{";
    bool first = true;
    for (const auto& kv : g_storage)
    {
        if (!first) file << ",";
        file << "\n  \"" << kv.first << "\": \"" << kv.second << "\"";
        first = false;
    }
    if (!g_storage.empty()) file << "\n";
    file << "}\n";
}

static bool EndsWithLastError(const std::string& key)
{
    constexpr const char* suffix = ".lastError";
    constexpr size_t suffixLen = 10;
    return key.size() >= suffixLen && key.compare(key.size() - suffixLen, suffixLen, suffix) == 0;
}

// ── Drawing API ──────────────────────────────────────────────────
struct D2DState
{
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
    WidgetEngine* engine = nullptr;
    std::string storagePrefix;
    std::wstring currentWidgetId;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> imageCache;
};

static D2DState* GetD2D(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__d2d_ptr");
    auto* s = static_cast<D2DState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

static void SetWidgetExecutionContext(D2DState* state, const std::wstring& widgetId)
{
    if (!state) return;
    state->currentWidgetId = widgetId;
    state->storagePrefix = WidgetWideToUtf8(widgetId);
}

static void SetWidgetRectContext(D2DState* state, RECT bounds)
{
    if (!state || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    state->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
}

static int lua_DrawText(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    const char* text = luaL_checkstring(L, 3);
    float size = static_cast<float>(luaL_optnumber(L, 4, 14));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 6, 0));
    bool bold = lua_toboolean(L, 7) != 0;
    bool singleLine = lua_toboolean(L, 8) != 0;

    auto* s = GetD2D(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    ComPtr<IDWriteTextFormat> format;
    s->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    if (!format) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;

    ComPtr<ID2D1SolidColorBrush> brush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, b), &brush);
    if (!brush) return 0;

    float bx = x + s->widgetRect.left;
    float by = y + s->widgetRect.top;

    if (maxWidth > 0)
    {
        format->SetWordWrapping(singleLine ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);

        ComPtr<IDWriteTextLayout> layout;
        const float maxHeight = singleLine ? std::max(size * 1.35f, size + 4.0f) : 5000.0f;
        s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format.Get(), maxWidth, maxHeight, &layout);
        if (layout && singleLine)
        {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(s->dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)) && ellipsis)
            {
                DWRITE_TRIMMING trimming{};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        if (layout)
            s->ctx->DrawTextLayout(D2D1::Point2F(bx, by), layout.Get(), brush.Get());
    }
    else
    {
        D2D1_RECT_F rect = { bx, by, bx + 800, by + 200 };
        s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format.Get(), &rect, brush.Get());
    }
    return 0;
}

static int lua_MeasureText(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    float size = static_cast<float>(luaL_optnumber(L, 2, 14));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 3, 0));
    bool bold = lua_toboolean(L, 4) != 0;

    auto pushSize = [&](float width, float height) {
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, width);
        lua_setfield(L, -2, "width");
        lua_pushnumber(L, height);
        lua_setfield(L, -2, "height");
        return 1;
    };

    auto* s = GetD2D(L);
    if (!s || !s->dwrite)
        return pushSize(0.0f, 0.0f);

    ComPtr<IDWriteTextFormat> format;
    s->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    if (!format)
        return pushSize(0.0f, 0.0f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    ComPtr<IDWriteTextLayout> layout;
    const float layoutWidth = maxWidth > 0.0f ? maxWidth : 4096.0f;
    if (FAILED(s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
        format.Get(), layoutWidth, 4096.0f, &layout)) || !layout)
        return pushSize(0.0f, size);

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return pushSize(metrics.widthIncludingTrailingWhitespace, metrics.height);
}

static int lua_DrawRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float alpha = static_cast<float>(luaL_optnumber(L, 7, 1.0));

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;

    ComPtr<ID2D1SolidColorBrush> brush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, alpha), &brush);
    if (!brush) return 0;

    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
    {
        D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        s->ctx->FillRoundedRectangle(rounded, brush.Get());
    }
    else
    {
        s->ctx->FillRectangle(rect, brush.Get());
    }
    return 0;
}

static int lua_GetTime(lua_State* L)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, st.wYear);   lua_setfield(L, -2, "year");
    lua_pushinteger(L, st.wMonth);  lua_setfield(L, -2, "month");
    lua_pushinteger(L, st.wDay);    lua_setfield(L, -2, "day");
    lua_pushinteger(L, st.wDayOfWeek + 1); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, st.wHour);   lua_setfield(L, -2, "hour");
    lua_pushinteger(L, st.wMinute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, st.wSecond); lua_setfield(L, -2, "sec");
    return 1;
}

static bool RequirePermission(lua_State* L, const char* permission)
{
    auto* s = GetD2D(L);
    if (!s || !s->engine) return false;
    if (s->engine->RuntimeHasPermission(s->currentWidgetId, permission))
        return true;
    std::string msg = std::string("Permission denied: ") + permission;
    s->engine->RuntimeRecordError(s->currentWidgetId, msg);
    luaL_error(L, "%s", msg.c_str());
    return false;
}

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
        std::wstring result = Utf8ToWideLocal(path ? path : "");
        lua_pop(L, 1);
        return result;
    }
    const char* path = luaL_checkstring(L, index);
    return Utf8ToWideLocal(path ? path : "");
}

static int lua_WidgetInfo(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_createtable(L, 0, 5);
    if (!s)
        return 1;
    lua_pushstring(L, WidgetWideToUtf8(s->currentWidgetId).c_str()); lua_setfield(L, -2, "id");
    lua_pushnumber(L, s->widgetRect.right - s->widgetRect.left); lua_setfield(L, -2, "width");
    lua_pushnumber(L, s->widgetRect.bottom - s->widgetRect.top); lua_setfield(L, -2, "height");
    return 1;
}

static int lua_WidgetSetTitle(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeSetWidgetTitle(s->currentWidgetId, Utf8ToWideLocal(title ? title : ""));
    return 0;
}

static int lua_WidgetInvalidate(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeInvalidateHost();
    return 0;
}

static int lua_WidgetLog(lua_State* L)
{
    const char* level = luaL_optstring(L, 1, "info");
    const char* message = luaL_optstring(L, 2, "");
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeAddLog(s->currentWidgetId, level ? level : "info", message ? message : "");
    return 0;
}

static int lua_WidgetTheme(lua_State* L)
{
    auto* s = GetD2D(L);
    LuaWidgetTheme theme;
    if (s && s->engine)
        theme = s->engine->RuntimeGetWidgetTheme(s->currentWidgetId);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, theme.bg); lua_setfield(L, -2, "bg");
    lua_pushinteger(L, theme.border); lua_setfield(L, -2, "border");
    lua_pushnumber(L, theme.alpha); lua_setfield(L, -2, "alpha");
    lua_pushnumber(L, theme.gradientEndA); lua_setfield(L, -2, "gradientEndA");
    return 1;
}

static int lua_WidgetEditText(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    int x = static_cast<int>(luaL_checknumber(L, 2));
    int y = static_cast<int>(luaL_checknumber(L, 3));
    int w = static_cast<int>(luaL_checknumber(L, 4));
    int h = static_cast<int>(luaL_checknumber(L, 5));
    bool multiline = lua_toboolean(L, 6) != 0;
    auto* s = GetD2D(L);
    if (!s || !s->engine || !key || !*key)
        return 0;

    std::string initial;
    if (lua_isstring(L, 7))
        initial = lua_tostring(L, 7);
    else
        initial = s->engine->RuntimeGetStorageValue(s->currentWidgetId, key);

    LuaInlineTextEditRequest request;
    request.widgetId = s->currentWidgetId;
    request.storageKey = key;
    request.text = initial;
    request.localRect = { x, y, x + std::max(1, w), y + std::max(1, h) };
    request.multiline = multiline;
    request.selectAll = lua_isnil(L, 8) ? true : (lua_toboolean(L, 8) != 0);
    request.textColor = static_cast<int>(luaL_optinteger(L, 9, 0x000000));
    s->engine->RuntimeBeginInlineTextEdit(request);
    return 0;
}

static int lua_DesktopItems(lua_State* L)
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

static int lua_DesktopSelection(lua_State* L)
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

static int lua_DesktopFind(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    std::string query = queryRaw ? queryRaw : "";
    std::transform(query.begin(), query.end(), query.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    lua_newtable(L);
    int i = 1;
    for (const auto& item : items)
    {
        std::string hay = item.title + " " + item.path;
        std::transform(hay.begin(), hay.end(), hay.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (query.empty() || hay.find(query) != std::string::npos)
        {
            PushDesktopItem(L, item);
            lua_rawseti(L, -2, i++);
        }
    }
    return 1;
}

static int lua_DesktopOpen(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeOpenDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

static int lua_DesktopReveal(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeRevealDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

static int lua_DesktopRefresh(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeRefreshDesktop();
    return 0;
}

static int lua_DrawStrokeRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float thickness = static_cast<float>(luaL_optnumber(L, 7, 1.0));
    float alpha = static_cast<float>(luaL_optnumber(L, 8, 1.0));
    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    ComPtr<ID2D1SolidColorBrush> brush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, alpha), &brush);
    if (!brush) return 0;
    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
        s->ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), thickness);
    else
        s->ctx->DrawRectangle(rect, brush.Get(), thickness);
    return 0;
}

static ID2D1Bitmap1* LoadImageBitmap(D2DState* s, const std::wstring& path)
{
    if (!s || !s->ctx || path.empty()) return nullptr;
    auto it = s->imageCache.find(path);
    if (it != s->imageCache.end()) return it->second.Get();

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) || !factory)
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter)
        return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(s->ctx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    s->imageCache[path] = bitmap;
    return result;
}

static int lua_DrawImage(lua_State* L)
{
    const char* pathRaw = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float w = static_cast<float>(luaL_checknumber(L, 4));
    float h = static_cast<float>(luaL_checknumber(L, 5));
    float alpha = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    auto* s = GetD2D(L);
    std::wstring path = Utf8ToWideLocal(pathRaw ? pathRaw : "");
    if (path.empty() || !PathIsRelativeW(path.c_str()))
        return 0;
    std::wstring fullPath = GetExeWidgetsDir();
    fullPath += L"\\";
    fullPath += path;
    ID2D1Bitmap1* bmp = LoadImageBitmap(s, fullPath);
    if (!s || !s->ctx || !bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h);
    s->ctx->DrawBitmap(bmp, dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

static ComPtr<ID2D1Bitmap1> BitmapFromHBitmap(ID2D1DeviceContext* ctx, HBITMAP hbm)
{
    ComPtr<ID2D1Bitmap1> result;
    if (!ctx || !hbm) return result;
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || !bm.bmBits) return result;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(ctx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(bm.bmWidth), static_cast<UINT32>(bm.bmHeight)),
        bm.bmBits, static_cast<UINT32>(bm.bmWidthBytes), &props, &result)))
        result.Reset();
    return result;
}

static int lua_DrawIcon(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    std::wstring path = ReadLuaPathArg(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 32));
    float alpha = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    auto* s = GetD2D(L);
    if (!s || !s->ctx || path.empty()) return 0;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return 0;
    SIZE bitmapSize{};
    HBITMAP hbm = GetHighResolutionShellIconBitmap(pidl, 0, bitmapSize);
    CoTaskMemFree(pidl);
    if (!hbm) return 0;
    ComPtr<ID2D1Bitmap1> bmp = BitmapFromHBitmap(s->ctx, hbm);
    DeleteObject(hbm);
    if (!bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + size, y + s->widgetRect.top + size);
    s->ctx->DrawBitmap(bmp.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

// ── WidgetEngine ──────────────────────────────────────────────────
WidgetEngine::~WidgetEngine()
{
    Shutdown();
}

bool WidgetEngine::Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;

    L_ = luaL_newstate();
    if (!L_) return false;

    luaL_requiref(L_, "_G", luaopen_base, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L_, 1);
    RegisterDrawAPI(L_);

    // Allocate D2D state
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    lua_pushlightuserdata(L_, d2dState_);
    lua_setfield(L_, LUA_REGISTRYINDEX, "__d2d_ptr");

    // Init storage path
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"SnowDesktop.storage.json");
    g_storagePath = exePath;
    LoadStorageFile();
    return true;
}

void WidgetEngine::Shutdown()
{
    widgets_.clear();
    if (L_) { lua_close(L_); L_ = nullptr; }
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::UnloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    luaL_unref(L_, LUA_REGISTRYINDEX, widgets_[idx].ref);
    widgets_.erase(widgets_.begin() + idx);

    // Remove storage data for this widget
    std::string prefix = WidgetWideToUtf8(widgetId) + ".";
    auto it = g_storage.begin();
    while (it != g_storage.end())
    {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = g_storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

int WidgetEngine::FindWidget(const std::wstring& widgetId) const
{
    for (int i = 0; i < (int)widgets_.size(); ++i)
    {
        if (widgets_[i].valid && widgets_[i].widgetId == widgetId)
            return i;
    }
    return -1;
}

void WidgetEngine::PushSafeEnvironment(lua_State* L, const LuaWidget& widget)
{
    static const char* funcs[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
        "tonumber", "tostring", "type", "xpcall"
    };

    lua_newtable(L);
    for (const char* name : funcs)
    {
        lua_getglobal(L, name);
        lua_setfield(L, -2, name);
    }
    lua_getglobal(L, "string"); lua_setfield(L, -2, "string");
    lua_getglobal(L, "table");  lua_setfield(L, -2, "table");
    lua_getglobal(L, "math");   lua_setfield(L, -2, "math");
    lua_getglobal(L, "utf8");   lua_setfield(L, -2, "utf8");

    lua_getglobal(L, "draw");    lua_setfield(L, -2, "draw");
    lua_getglobal(L, "sys");     lua_setfield(L, -2, "sys");
    lua_getglobal(L, "layout");  lua_setfield(L, -2, "layout");
    lua_getglobal(L, "storage"); lua_setfield(L, -2, "storage");
    lua_getglobal(L, "widget");  lua_setfield(L, -2, "widget");
    lua_getglobal(L, "desktop"); lua_setfield(L, -2, "desktop");

    if (widget.permissions.contains("ui.input"))
    {
        lua_getglobal(L, "imgui");
        lua_setfield(L, -2, "imgui");
    }

    lua_pushstring(L, WidgetWideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath)
{
    if (FindWidget(widgetId) >= 0) return true;
    return LoadWidget(ResolveWidgetPath(scriptPath), widgetId);
}

bool WidgetEngine::LoadWidget(const std::wstring& path, const std::wstring& widgetId)
{
    std::string source = ReadTextFile(path);
    if (source.empty()) return false;

    LuaWidget pending;
    pending.widgetId = widgetId;
    pending.filePath = path;
    pending.manifest = GetWidgetManifest(PathFindFileNameW(path.c_str()));
    for (const auto& permission : pending.manifest.permissions)
        pending.permissions.insert(permission);
    SetWidgetExecutionContext(d2dState_, widgetId);

    // Create a sandbox table with only the registered safe API surface.
    PushSafeEnvironment(L_, pending);

    // Load the chunk
    if (luaL_loadstring(L_, source.c_str()) != LUA_OK)
    {
        const char* err = lua_tostring(L_, -1);
        RuntimeRecordError(widgetId, err ? err : "(load error)");
        lua_pop(L_, 2);
        return false;
    }

    // Try to set the chunk's first upvalue (_ENV) to sandbox.
    lua_pushvalue(L_, -2);
    const char* envName = lua_setupvalue(L_, -2, 1);
    if (envName == nullptr)
    {
        // Chunk has no _ENV upvalue - run directly in sandbox via alternative method
        // Pop the chunk, reload with explicit environment
        lua_pop(L_, 2);  // pop sandbox copy and chunk, keep sandbox
        // Wrap the source to use the sandbox explicitly
        std::string wrapped = "local _ENV = ...;\n" + source;
        if (luaL_loadstring(L_, wrapped.c_str()) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(load error)");
            lua_pop(L_, 2);
            return false;
        }
        lua_pushvalue(L_, -2);  // push sandbox as argument
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(L_, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(L_, 2);
            return false;
        }
    }

    // sandbox now contains the script's globals (name, render, etc.)
    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);

    std::string name = "Unnamed";
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_getfield(L_, -1, "name");
    if (lua_isstring(L_, -1))
        name = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    // Read customStyle flag
    bool customStyle = false;
    lua_getfield(L_, -1, "useCustomStyle");
    if (!lua_isnil(L_, -1))
        customStyle = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);

    lua_pop(L_, 1);  // pop table

    LuaWidget w;
    w.widgetId = widgetId;
    w.name = name;
    w.filePath = path;
    w.manifest = pending.manifest;
    w.permissions = pending.permissions;
    w.ref = ref;
    w.valid = true;
    w.customStyle = customStyle;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(w);
    return true;
}

void WidgetEngine::RenderAll(ID2D1DeviceContext* context)
{
    d2dState_->ctx = context;
}

bool WidgetEngine::RenderWidgetEditor(const std::wstring& widgetId, const std::wstring& widgetName)
{
    (void)widgetName;

    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    int ref = (idx >= 0) ? widgets_[idx].ref : LUA_NOREF;
    if (ref == LUA_NOREF) return true;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_istable(L_, -1))
    {
        // Inject widgetId global
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(L_, widUtf8.c_str());
        lua_setfield(L_, -2, "widgetId");

        lua_getfield(L_, -1, "imguiRender");
        if (lua_isfunction(L_, -1))
        {
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(L_, -1);
                RuntimeRecordError(widgetId, err ? err : "(imguiRender error)");
                lua_pop(L_, 1);
            }
        }
        else
            lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
}

void WidgetEngine::RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds)
{
    (void)scriptPath;

    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    LuaWidget* found = &widgets_[idx];

    // Hot-reload: check if file changed or deleted
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        bool exists = GetFileAttributesExW(found->filePath.c_str(), GetFileExInfoStandard, &attr) != 0;
        if (!exists) { found->valid = false; return; }
        if (CompareFileTime(&attr.ftLastWriteTime, &found->lastModified) != 0)
        {
            luaL_unref(L_, LUA_REGISTRYINDEX, found->ref);
            found->valid = false;
            found->ref = LUA_NOREF;
            LoadWidget(found->filePath, widgetId);
            idx = FindWidget(widgetId);
            if (idx < 0) return;
            found = &widgets_[idx];
        }
    }

    d2dState_->ctx = context;
    SetWidgetExecutionContext(d2dState_, widgetId);
    found->lastBounds = bounds;
    SetWidgetRectContext(d2dState_, bounds);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    // Inject widgetId global
    {
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(L_, widUtf8.c_str());
        lua_setfield(L_, -2, "widgetId");
    }

    lua_getfield(L_, -1, "render");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(render error)");
            lua_pop(L_, 1);

            // draw conspicuous placeholder
            if (d2dState_ && d2dState_->ctx)
            {
                ComPtr<ID2D1SolidColorBrush> brush;
                d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.15f, 1.0f), &brush);
                d2dState_->ctx->FillRectangle(d2dState_->widgetRect, brush.Get());

                // draw white text
                if (d2dState_->dwrite)
                {
                    ComPtr<IDWriteTextFormat> format;
                    d2dState_->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        14.0f, L"", &format);
                    if (format)
                    {
                        std::wstring wmsg = L"WIDGET ERROR: ";
                        std::string serr = err ? err : "(unknown)";
                        // append part of error
                        size_t maxlen = 128;
                        if (serr.size() > maxlen) serr = serr.substr(0, maxlen) + "...";
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, nullptr, 0);
                        std::wstring werr(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, &werr[0], wlen);
                        wmsg += werr;

                        D2D1_RECT_F r = d2dState_->widgetRect;
                        r.left += 8; r.top += 8; r.right -= 8; r.bottom -= 8;
                        ComPtr<ID2D1SolidColorBrush> textBrush;
                        d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &textBrush);
                        d2dState_->ctx->DrawTextW(wmsg.c_str(), (UINT32)wmsg.size() - 1, format.Get(), &r, textBrush.Get());
                    }
                }
            }
            // mark invalid to avoid repeated attempts
            found->valid = false;
            lua_pop(L_, 1);
            lua_pop(L_, 1);
            return;
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 && widgets_[idx].customStyle;
}

void WidgetEngine::InvokeOpen(const std::wstring& widgetId)
{
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, "onOpen");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(onOpen error)");
            lua_pop(L_, 1);
        }
    }
    else
        lua_pop(L_, 1);
    lua_pop(L_, 1);
}

void WidgetEngine::InvokeClick(const std::wstring& widgetId, int x, int y)
{
    InvokeMouseEvent(widgetId, "onClick", x, y, 1, 0);
}

void WidgetEngine::InvokeMouseEvent(const std::wstring& widgetId, const char* callbackName, int x, int y,
    int button, int delta)
{
    if (!callbackName || !*callbackName) return;
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, callbackName);
    if (lua_isfunction(L_, -1))
    {
        lua_pushinteger(L_, x);
        lua_pushinteger(L_, y);
        lua_pushinteger(L_, button);
        lua_pushinteger(L_, delta);
        if (lua_pcall(L_, 4, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(mouse callback error)");
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(const std::wstring& widgetId)
{
    std::vector<LuaWidgetMenuItem> result;
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return result;
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return result; }
    lua_getfield(L_, -1, "getContextMenu");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 1, 0) == LUA_OK && lua_istable(L_, -1))
        {
            int count = static_cast<int>(lua_rawlen(L_, -1));
            for (int i = 1; i <= count; ++i)
            {
                lua_rawgeti(L_, -1, i);
                if (lua_istable(L_, -1))
                {
                    LuaWidgetMenuItem item;
                    lua_getfield(L_, -1, "id");
                    item.id = lua_isinteger(L_, -1) ? static_cast<int>(lua_tointeger(L_, -1)) : i;
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "label");
                    item.label = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "enabled");
                    item.enabled = lua_isnil(L_, -1) ? true : (lua_toboolean(L_, -1) != 0);
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "separator");
                    item.separator = lua_toboolean(L_, -1) != 0;
                    lua_pop(L_, 1);
                    if (item.separator || !item.label.empty())
                        result.push_back(std::move(item));
                }
                lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
        else
        {
            const char* err = lua_tostring(L_, -1);
            if (err)
                RuntimeRecordError(widgetId, err);
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return result;
}

void WidgetEngine::InvokeMenu(const std::wstring& widgetId, int menuId)
{
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return;
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, "onMenu");
    if (lua_isfunction(L_, -1))
    {
        lua_pushinteger(L_, menuId);
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(onMenu error)");
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void WidgetEngine::NotifyDesktopChanged(const std::string& reason)
{
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || !RuntimeHasPermission(widget.widgetId, "desktop.read"))
            continue;
        SetWidgetExecutionContext(d2dState_, widget.widgetId);
        SetWidgetRectContext(d2dState_, widget.lastBounds);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }
        lua_getfield(L_, -1, "onDesktopChanged");
        if (lua_isfunction(L_, -1))
        {
            lua_pushstring(L_, reason.c_str());
            if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(L_, -1);
                RuntimeRecordError(widget.widgetId, err ? err : "(onDesktopChanged error)");
                lua_pop(L_, 1);
            }
        }
        else
        {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }
}

bool WidgetEngine::ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const
{
    for (const auto& w : widgets_)
    {
        if (w.valid && w.filePath.size() >= scriptPath.size() &&
            w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
            if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return defaultVal; }
            lua_getfield(L_, -1, flag);
            bool result = lua_isnil(L_, -1) ? defaultVal : (lua_toboolean(L_, -1) != 0);
            lua_pop(L_, 2);
            return result;
        }
    }
    return defaultVal;
}

bool WidgetEngine::ReadCustomColors(const std::wstring& widgetId,
    float& bgR, float& bgG, float& bgB, float& alpha,
    float& borderR, float& borderG, float& borderB, float& gradientEndA) const
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& w = widgets_[idx];

    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }

            auto readHex = [&](const char* key, float& r, float& g, float& b, int def) {
                lua_getfield(L_, -1, key);
                int val = lua_isinteger(L_, -1) ? (int)lua_tointeger(L_, -1) : def;
                lua_pop(L_, 1);
                r = ((val >> 16) & 0xFF) / 255.0f;
                g = ((val >> 8) & 0xFF) / 255.0f;
                b = (val & 0xFF) / 255.0f;
            };

            auto readFloat = [&](const char* key, float& out, float def) {
                lua_getfield(L_, -1, key);
                out = lua_isnumber(L_, -1) ? (float)lua_tonumber(L_, -1) : def;
                lua_pop(L_, 1);
            };

            readHex("bg", bgR, bgG, bgB, 0x151A21);
            readHex("border", borderR, borderG, borderB, 0xFFFFFF);
            readFloat("alpha", alpha, 0.36f);
            readFloat("gradientEndA", gradientEndA, 0.0f);

            lua_pop(L_, 1);
            return true;
}

std::vector<WidgetErrorEntry> WidgetEngine::GetWidgetErrors() const
{
    std::vector<WidgetErrorEntry> result;
    for (const auto& kv : g_storage)
    {
        if (!EndsWithLastError(kv.first))
            continue;
        result.push_back({ kv.first, kv.second });
    }
    std::sort(result.begin(), result.end(), [](const WidgetErrorEntry& a, const WidgetErrorEntry& b) {
        return a.key < b.key;
    });
    return result;
}

void WidgetEngine::ClearWidgetErrors()
{
    for (auto it = g_storage.begin(); it != g_storage.end(); )
    {
        if (EndsWithLastError(it->first))
            it = g_storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

bool WidgetEngine::ReloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    std::wstring path = widgets_[idx].filePath;
    luaL_unref(L_, LUA_REGISTRYINDEX, widgets_[idx].ref);
    widgets_.erase(widgets_.begin() + idx);
    return LoadWidget(path, widgetId);
}

bool WidgetEngine::RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const
{
    if (!permission || !*permission) return true;
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& perms = widgets_[idx].permissions;
    return perms.contains(permission);
}

void WidgetEngine::RuntimeRecordError(const std::wstring& widgetId, const std::string& message)
{
    std::string idUtf8 = WidgetWideToUtf8(widgetId);
    g_storage[idUtf8 + ".lastError"] = message;
    SaveStorageFile();
    RuntimeAddLog(widgetId, "error", message);
}

void WidgetEngine::RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message)
{
    WidgetLogEntry entry;
    entry.key = WidgetWideToUtf8(widgetId);
    entry.level = level.empty() ? "info" : level;
    entry.message = message;
    g_widgetLogs.push_back(std::move(entry));
    while (g_widgetLogs.size() > 200)
        g_widgetLogs.pop_front();
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopItems() const
{
    return desktopSnapshotProvider_ ? desktopSnapshotProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopSelection() const
{
    return selectionProvider_ ? selectionProvider_() : std::vector<LuaDesktopItemInfo>{};
}

bool WidgetEngine::RuntimeOpenDesktopPath(const std::wstring& path)
{
    if (path.empty()) return false;
    return desktopOpenCallback_ ? desktopOpenCallback_(path) : false;
}

bool WidgetEngine::RuntimeRevealDesktopPath(const std::wstring& path)
{
    if (path.empty()) return false;
    return desktopRevealCallback_ ? desktopRevealCallback_(path) : false;
}

void WidgetEngine::RuntimeRefreshDesktop()
{
    if (desktopRefreshCallback_)
        desktopRefreshCallback_();
}

void WidgetEngine::RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (setWidgetTitleCallback_)
        setWidgetTitleCallback_(widgetId, title);
}

void WidgetEngine::RuntimeInvalidateHost()
{
    if (invalidateCallback_)
        invalidateCallback_();
}

std::string WidgetEngine::RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const
{
    std::string fullKey = WidgetWideToUtf8(widgetId) + "." + key;
    auto it = g_storage.find(fullKey);
    return it != g_storage.end() ? it->second : std::string{};
}

void WidgetEngine::RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value)
{
    if (key.empty()) return;
    g_storage[WidgetWideToUtf8(widgetId) + "." + key] = value;
    SaveStorageFile();
}

void WidgetEngine::RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (inlineTextEditCallback_)
        inlineTextEditCallback_(request);
}

LuaWidgetTheme WidgetEngine::RuntimeGetWidgetTheme(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 ? widgets_[idx].theme : LuaWidgetTheme{};
}

void WidgetEngine::SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme)
{
    int idx = FindWidget(widgetId);
    if (idx >= 0)
        widgets_[idx].theme = theme;
}

std::vector<WidgetDiagnosticEntry> WidgetEngine::GetWidgetDiagnostics() const
{
    std::vector<WidgetDiagnosticEntry> result;
    result.reserve(widgets_.size());
    for (const auto& widget : widgets_)
    {
        WidgetDiagnosticEntry entry;
        entry.widgetId = widget.widgetId;
        entry.name = widget.name;
        entry.scriptPath = widget.filePath;
        entry.valid = widget.valid;
        entry.hasManifest = widget.manifest.hasManifest;
        entry.permissions = widget.manifest.permissions;
        std::string errorKey = WidgetWideToUtf8(widget.widgetId) + ".lastError";
        auto errIt = g_storage.find(errorKey);
        if (errIt != g_storage.end())
            entry.lastError = errIt->second;
        std::string logKey = WidgetWideToUtf8(widget.widgetId);
        for (const auto& log : g_widgetLogs)
        {
            if (log.key == logKey)
                entry.logs.push_back(log);
        }
        result.push_back(std::move(entry));
    }
    return result;
}

// ── List available widget scripts ────────────────────────────────
std::vector<std::wstring> WidgetEngine::ListAvailable()
{
    std::vector<std::wstring> result;
    std::wstring search = GetExeWidgetsDir();
    search += L"\\*.lua";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        result.push_back(fd.cFileName);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return result;
}

LuaWidgetManifest WidgetEngine::GetWidgetManifest(const std::wstring& filename)
{
    LuaWidgetManifest manifest;
    std::wstring fullPath = filename;
    if (PathIsRelativeW(fullPath.c_str()))
        fullPath = ResolveWidgetPath(filename);

    std::wstring manifestPath = ManifestPathForScriptFile(fullPath);
    std::string text = ReadTextFile(manifestPath);
    if (text.empty())
        return manifest;

    manifest.hasManifest = true;
    JsonReadString(text, "name", manifest.name);
    JsonReadString(text, "version", manifest.version);
    JsonReadString(text, "description", manifest.description);
    manifest.permissions = JsonReadStringArray(text, "permissions");
    std::string sizeText = text;
    size_t sizeName = text.find("\"defaultSize\"");
    if (sizeName != std::string::npos)
    {
        size_t open = text.find('{', sizeName);
        size_t close = text.find('}', open == std::string::npos ? sizeName : open + 1);
        if (open != std::string::npos && close != std::string::npos && close > open)
            sizeText = text.substr(open, close - open + 1);
    }

    int columns = manifest.defaultColumns;
    int rows = manifest.defaultRows;
    if (JsonReadInt(sizeText, "columns", columns))
        manifest.defaultColumns = std::clamp(columns, 1, 8);
    if (JsonReadInt(sizeText, "rows", rows))
        manifest.defaultRows = std::clamp(rows, 1, 8);

    if (manifest.permissions.empty())
        manifest.permissions = {};
    return manifest;
}

bool WidgetEngine::GetWidgetDefaultSpan(const std::wstring& filename, int& columns, int& rows)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    columns = std::max(1, manifest.defaultColumns);
    rows = std::max(1, manifest.defaultRows);
    return manifest.hasManifest;
}

std::wstring WidgetEngine::GetWidgetDisplayName(const std::wstring& filename)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    if (!manifest.name.empty())
        return Utf8ToWideLocal(manifest.name);

    std::wstring fallback = filename;
    if (fallback.size() > 4 && fallback.substr(fallback.size() - 4) == L".lua")
        fallback = fallback.substr(0, fallback.size() - 4);
    return fallback;
}

static int lua_DrawLine(lua_State* L)
{
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    float thick = (float)luaL_optnumber(L, 5, 1);
    int color = (int)luaL_optinteger(L, 6, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 7, 1.0);

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;

    ComPtr<ID2D1SolidColorBrush> brush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, alpha), &brush);
    if (!brush) return 0;

    s->ctx->DrawLine(
        D2D1::Point2F(x1 + s->widgetRect.left, y1 + s->widgetRect.top),
        D2D1::Point2F(x2 + s->widgetRect.left, y2 + s->widgetRect.top),
        brush.Get(), thick);
    return 0;
}

static int lua_DrawCircle(lua_State* L)
{
    float cx = (float)luaL_checknumber(L, 1);
    float cy = (float)luaL_checknumber(L, 2);
    float r = (float)luaL_checknumber(L, 3);
    int color = (int)luaL_optinteger(L, 4, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 5, 1.0);

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    float colR = ((color >> 16) & 0xFF) / 255.0f;
    float colG = ((color >> 8) & 0xFF) / 255.0f;
    float colB = (color & 0xFF) / 255.0f;

    ComPtr<ID2D1SolidColorBrush> brush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(colR, colG, colB, alpha), &brush);
    if (!brush) return 0;

    s->ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx + s->widgetRect.left, cy + s->widgetRect.top), r, r),
        brush.Get());
    return 0;
}

static int lua_StorageGet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) { lua_pushnil(L); return 1; }
    std::string fullKey = s->storagePrefix + "." + key;
    auto it = g_storage.find(fullKey);
    if (it != g_storage.end())
        lua_pushstring(L, it->second.c_str());
    else
        lua_pushnil(L);
    return 1;
}

static int lua_StorageSet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (!s) return 0;
    g_storage[s->storagePrefix + "." + key] = value;
    SaveStorageFile();
    return 0;
}

// ── ImGui Lua API ──────────────────────────────────────────────────
static int lua_ImGuiText(lua_State* L) { ImGui::Text("%s", luaL_checkstring(L, 1)); return 0; }
static int lua_ImGuiTextWrapped(lua_State* L) { ImGui::TextWrapped("%s", luaL_checkstring(L, 1)); return 0; }

static int lua_ImGuiSeparator(lua_State* L)
{
    (void)L;
    ImGui::Separator();
    return 0;
}

static int lua_ImGuiSameLine(lua_State* L)
{
    float offset = (float)luaL_optnumber(L, 1, 0.0);
    float spacing = (float)luaL_optnumber(L, 2, -1.0);
    ImGui::SameLine(offset, spacing);
    return 0;
}

static int lua_ImGuiSpacing(lua_State* L)
{
    (void)L;
    ImGui::Spacing();
    return 0;
}

static int lua_ImGuiCollapsingHeader(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool open = ImGui::CollapsingHeader(label);
    lua_pushboolean(L, open);
    return 1;
}

static int lua_ImGuiTreeNode(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool open = ImGui::TreeNode(label);
    lua_pushboolean(L, open);
    return 1;
}

static int lua_ImGuiTreePop(lua_State* L)
{
    (void)L;
    ImGui::TreePop();
    return 0;
}

static int lua_ImGuiButton(lua_State* L)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(luaL_checkstring(L, 1));
    ImGui::PopStyleColor();
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiColorEdit3(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int hex = (int)luaL_checkinteger(L, 2);
    float col[3] = {
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f
    };
    if (ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_NoInputs))
    {
        int r = (int)(col[0] * 255);
        int g = (int)(col[1] * 255);
        int b = (int)(col[2] * 255);
        lua_pushinteger(L, (r << 16) | (g << 8) | b);
    }
    else
        lua_pushinteger(L, hex);
    return 1;
}

static int lua_ImGuiSliderFloat(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    float min = (float)luaL_checknumber(L, 3);
    float max = (float)luaL_checknumber(L, 4);
    if (ImGui::SliderFloat(label, &v, min, max))
        lua_pushnumber(L, v);
    else
        lua_pushnumber(L, (double)luaL_checknumber(L, 2));
    return 1;
}

static int lua_ImGuiInputText(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    char buf[4096]{};
    strncpy_s(buf, sizeof(buf), text, _TRUNCATE);
    if (ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(-1, 120)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, text);
    return 1;
}

static int lua_ImGuiInputTextSingle(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    char buf[4096]{};
    strncpy_s(buf, sizeof(buf), text, _TRUNCATE);
    if (ImGui::InputText(label, buf, sizeof(buf)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, text);
    return 1;
}

static int lua_ImGuiCheckbox(lua_State* L)
{
    bool v = lua_toboolean(L, 2) != 0;
    if (ImGui::Checkbox(luaL_checkstring(L, 1), &v))
        lua_pushboolean(L, v);
    else
        lua_pushboolean(L, lua_toboolean(L, 2) != 0);
    return 1;
}

static int lua_ImGuiSliderInt(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int v = (int)luaL_checkinteger(L, 2);
    int min = (int)luaL_checkinteger(L, 3);
    int max = (int)luaL_checkinteger(L, 4);
    if (ImGui::SliderInt(label, &v, min, max))
        lua_pushinteger(L, v);
    else
        lua_pushinteger(L, (int)luaL_checkinteger(L, 2));
    return 1;
}

static int lua_ImGuiCombo(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int current = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    const int count = (int)lua_rawlen(L, 3);
    items.reserve(count);
    for (int i = 1; i <= count; ++i)
    {
        lua_rawgeti(L, 3, i);
        items.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    std::string preview = (current >= 1 && current <= count) ? items[(size_t)current - 1] : "";
    if (ImGui::BeginCombo(label, preview.c_str()))
    {
        for (int i = 0; i < count; ++i)
        {
            bool selected = (current == i + 1);
            if (ImGui::Selectable(items[(size_t)i].c_str(), selected))
                current = i + 1;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    lua_pushinteger(L, current);
    return 1;
}

static int lua_ImGuiSelectable(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool selected = lua_toboolean(L, 2) != 0;
    bool clicked = ImGui::Selectable(label, selected);
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiRadio(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool active = lua_toboolean(L, 2) != 0;
    bool clicked = ImGui::RadioButton(label, active);
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiBeginDisabled(lua_State* L)
{
    bool disabled = lua_toboolean(L, 1) != 0;
    ImGui::BeginDisabled(disabled);
    return 0;
}

static int lua_ImGuiEndDisabled(lua_State* L)
{
    (void)L;
    ImGui::EndDisabled();
    return 0;
}

static int lua_StorageRemove(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) return 0;
    g_storage.erase(s->storagePrefix + "." + key);
    SaveStorageFile();
    return 0;
}

static int lua_StorageKeys(lua_State* L)
{
    auto* s = GetD2D(L);
    std::string prefix = s ? s->storagePrefix + "." : "";
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : g_storage)
    {
        if (!prefix.empty() && kv.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string key = prefix.empty() ? kv.first : kv.first.substr(prefix.size());
        lua_pushstring(L, key.c_str());
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int lua_LayoutWidth(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushnumber(L, s ? s->widgetRect.right - s->widgetRect.left : 400);
    return 1;
}

static int lua_LayoutHeight(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushnumber(L, s ? s->widgetRect.bottom - s->widgetRect.top : 300);
    return 1;
}

void WidgetEngine::RegisterDrawAPI(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_DrawText);  lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_MeasureText); lua_setfield(L, -2, "measureText");
    lua_pushcfunction(L, lua_DrawRect);  lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, lua_DrawStrokeRect); lua_setfield(L, -2, "strokeRect");
    lua_pushcfunction(L, lua_DrawLine);  lua_setfield(L, -2, "line");
    lua_pushcfunction(L, lua_DrawCircle);lua_setfield(L, -2, "circle");
    lua_pushcfunction(L, lua_DrawImage); lua_setfield(L, -2, "image");
    lua_pushcfunction(L, lua_DrawIcon);  lua_setfield(L, -2, "icon");
    lua_setglobal(L, "draw");

    lua_newtable(L);
    lua_pushcfunction(L, lua_WidgetInfo); lua_setfield(L, -2, "info");
    lua_pushcfunction(L, lua_WidgetSetTitle); lua_setfield(L, -2, "setTitle");
    lua_pushcfunction(L, lua_WidgetInvalidate); lua_setfield(L, -2, "invalidate");
    lua_pushcfunction(L, lua_WidgetLog); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, lua_WidgetTheme); lua_setfield(L, -2, "theme");
    lua_pushcfunction(L, lua_WidgetEditText); lua_setfield(L, -2, "editText");
    lua_setglobal(L, "widget");

    lua_newtable(L);
    lua_pushcfunction(L, lua_GetTime);   lua_setfield(L, -2, "getTime");
    lua_setglobal(L, "sys");

    lua_newtable(L);
    lua_pushcfunction(L, lua_DesktopItems); lua_setfield(L, -2, "items");
    lua_pushcfunction(L, lua_DesktopSelection); lua_setfield(L, -2, "selection");
    lua_pushcfunction(L, lua_DesktopFind); lua_setfield(L, -2, "find");
    lua_pushcfunction(L, lua_DesktopOpen); lua_setfield(L, -2, "open");
    lua_pushcfunction(L, lua_DesktopReveal); lua_setfield(L, -2, "reveal");
    lua_pushcfunction(L, lua_DesktopRefresh); lua_setfield(L, -2, "refresh");
    lua_setglobal(L, "desktop");

    lua_newtable(L);
    lua_pushcfunction(L, lua_LayoutWidth);  lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_LayoutHeight); lua_setfield(L, -2, "height");
    lua_setglobal(L, "layout");

    lua_newtable(L);
    lua_pushcfunction(L, lua_StorageGet);   lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_StorageSet);   lua_setfield(L, -2, "set");
    lua_pushcfunction(L, lua_StorageRemove);lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_StorageKeys);  lua_setfield(L, -2, "keys");
    lua_setglobal(L, "storage");

    lua_newtable(L);
    lua_pushcfunction(L, lua_ImGuiText);     lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_ImGuiTextWrapped); lua_setfield(L, -2, "textWrapped");
    lua_pushcfunction(L, lua_ImGuiSeparator); lua_setfield(L, -2, "separator");
    lua_pushcfunction(L, lua_ImGuiSameLine);  lua_setfield(L, -2, "sameLine");
    lua_pushcfunction(L, lua_ImGuiSpacing);   lua_setfield(L, -2, "spacing");
    lua_pushcfunction(L, lua_ImGuiCollapsingHeader); lua_setfield(L, -2, "collapsingHeader");
    lua_pushcfunction(L, lua_ImGuiTreeNode);  lua_setfield(L, -2, "treeNode");
    lua_pushcfunction(L, lua_ImGuiTreePop);   lua_setfield(L, -2, "treePop");
    lua_pushcfunction(L, lua_ImGuiButton);   lua_setfield(L, -2, "button");
    lua_pushcfunction(L, lua_ImGuiInputText);lua_setfield(L, -2, "input");
    lua_pushcfunction(L, lua_ImGuiInputTextSingle); lua_setfield(L, -2, "inputText");
    lua_pushcfunction(L, lua_ImGuiCheckbox); lua_setfield(L, -2, "checkbox");
    lua_pushcfunction(L, lua_ImGuiColorEdit3); lua_setfield(L, -2, "colorEdit3");
    lua_pushcfunction(L, lua_ImGuiSliderFloat); lua_setfield(L, -2, "sliderFloat");
    lua_pushcfunction(L, lua_ImGuiSliderInt); lua_setfield(L, -2, "sliderInt");
    lua_pushcfunction(L, lua_ImGuiCombo); lua_setfield(L, -2, "combo");
    lua_pushcfunction(L, lua_ImGuiSelectable); lua_setfield(L, -2, "selectable");
    lua_pushcfunction(L, lua_ImGuiRadio); lua_setfield(L, -2, "radio");
    lua_pushcfunction(L, lua_ImGuiBeginDisabled); lua_setfield(L, -2, "beginDisabled");
    lua_pushcfunction(L, lua_ImGuiEndDisabled); lua_setfield(L, -2, "endDisabled");
    lua_setglobal(L, "imgui");
}
