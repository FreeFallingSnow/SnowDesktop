#include "widget_engine.h"

#include <shlwapi.h>
#include <fstream>
#include <sstream>

static std::string ReadFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ── Drawing API ──────────────────────────────────────────────────
struct D2DState
{
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
};

static D2DState* GetD2D(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__d2d_ptr");
    auto* s = static_cast<D2DState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

static int lua_DrawText(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    const char* text = luaL_checkstring(L, 3);
    float size = static_cast<float>(luaL_optnumber(L, 4, 14));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));

    auto* s = GetD2D(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    ComPtr<IDWriteTextFormat> format;
    s->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
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

    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + 800, y + s->widgetRect.top + 200 };
    s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
        format.Get(), &rect, brush.Get());
    return 0;
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
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, st.wYear);   lua_setfield(L, -2, "year");
    lua_pushinteger(L, st.wMonth);  lua_setfield(L, -2, "month");
    lua_pushinteger(L, st.wDay);    lua_setfield(L, -2, "day");
    lua_pushinteger(L, st.wHour);   lua_setfield(L, -2, "hour");
    lua_pushinteger(L, st.wMinute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, st.wSecond); lua_setfield(L, -2, "sec");
    return 1;
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

    luaL_openlibs(L_);
    RegisterDrawAPI(L_);

    // Allocate D2D state
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    lua_pushlightuserdata(L_, d2dState_);
    lua_setfield(L_, LUA_REGISTRYINDEX, "__d2d_ptr");

    ReloadAll();
    return true;
}

void WidgetEngine::Shutdown()
{
    widgets_.clear();
    if (L_) { lua_close(L_); L_ = nullptr; }
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::ReloadAll()
{
    widgets_.clear();

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"widgets");
    CreateDirectoryW(exePath, nullptr);

    std::wstring search = exePath;
    search += L"\\*.lua";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path = exePath;
        path += L"\\";
        path += fd.cFileName;
        LoadWidget(path);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

bool WidgetEngine::LoadWidget(const std::wstring& path)
{
    std::string source = ReadFile(path);
    if (source.empty()) return false;

    // Create a sandbox table
    lua_newtable(L_);                        // sandbox
    lua_newtable(L_);                        // sandbox meta
    lua_getglobal(L_, "_G");                // sandbox_meta, _G
    lua_setfield(L_, -2, "__index");         // sandbox_meta.__index = _G (reads fall through to globals)
    lua_setmetatable(L_, -2);               // setmetatable(sandbox, sandbox_meta)
    // sandbox is at top of stack

    // Load the chunk
    if (luaL_loadstring(L_, source.c_str()) != LUA_OK)
    {
        lua_pop(L_, 2);
        return false;
    }

    // Try to set the chunk's first upvalue (_ENV) to sandbox
    const char* envName = lua_setupvalue(L_, -2, 1);
    if (envName == nullptr)
    {
        // Chunk has no _ENV upvalue - run directly in sandbox via alternative method
        // Pop the chunk, reload with explicit environment
        lua_pop(L_, 1);  // pop chunk, keep sandbox
        // Wrap the source to use the sandbox explicitly
        std::string wrapped = "local _ENV = ...;\n" + source;
        if (luaL_loadstring(L_, wrapped.c_str()) != LUA_OK)
        {
            lua_pop(L_, 2);
            return false;
        }
        lua_pushvalue(L_, -2);  // push sandbox as argument
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
        {
            lua_pop(L_, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
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
    w.name = name;
    w.filePath = path;
    w.ref = ref;
    w.valid = true;
    w.customStyle = customStyle;
    widgets_.push_back(w);
    return true;
}

void WidgetEngine::RenderAll(ID2D1DeviceContext* context)
{
    d2dState_->ctx = context;
    // For global-scope widgets (no grid bounds), render at origin
    // Grid-bound widgets use RenderWidget instead
}

// ── Render a specific widget within given bounds ─────────────────
void WidgetEngine::RenderWidget(const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds, float bgR, float bgG, float bgB, float alpha, float borderR, float borderG, float borderB, float gradientEndA)
{
    // Find the loaded widget matching this script path
    LuaWidget* found = nullptr;
    for (auto& w : widgets_)
    {
        if (w.valid && w.filePath.size() >= scriptPath.size() &&
            w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        {
            found = &w;
            break;
        }
    }
    if (!found) return;

    d2dState_->ctx = context;
    d2dState_->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));

    lua_rawgeti(L_, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    // Inject style if widget uses custom styling
    if (found->customStyle)
    {
        lua_getfield(L_, -1, "style");
        if (lua_isnil(L_, -1))
        {
            lua_pop(L_, 1);
            lua_newtable(L_);
            lua_pushnumber(L_, bgR);    lua_setfield(L_, -2, "bgR");
            lua_pushnumber(L_, bgG);    lua_setfield(L_, -2, "bgG");
            lua_pushnumber(L_, bgB);    lua_setfield(L_, -2, "bgB");
            lua_pushnumber(L_, alpha);  lua_setfield(L_, -2, "alpha");
            lua_pushnumber(L_, borderR);lua_setfield(L_, -2, "borderR");
            lua_pushnumber(L_, borderG);lua_setfield(L_, -2, "borderG");
            lua_pushnumber(L_, borderB);lua_setfield(L_, -2, "borderB");
            lua_pushnumber(L_, gradientEndA); lua_setfield(L_, -2, "gradientEndA");
            lua_setfield(L_, -2, "style");
        }
        else
        {
            lua_pop(L_, 1);
        }
    }

    lua_getfield(L_, -1, "render");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
            lua_pop(L_, 1);
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& scriptPath) const
{
    for (const auto& w : widgets_)
    {
        if (w.valid && w.filePath.size() >= scriptPath.size() &&
            w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        {
            return w.customStyle;
        }
    }
    return false;
}

// ── List available widget scripts ────────────────────────────────
std::vector<std::wstring> WidgetEngine::ListAvailable()
{
    std::vector<std::wstring> result;
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"widgets");

    std::wstring search = exePath;
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

// ── Layout API ────────────────────────────────────────────────────
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
    lua_pushcfunction(L, lua_DrawRect);  lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, lua_DrawLine);  lua_setfield(L, -2, "line");
    lua_pushcfunction(L, lua_DrawCircle);lua_setfield(L, -2, "circle");
    lua_setglobal(L, "draw");

    lua_newtable(L);
    lua_pushcfunction(L, lua_GetTime);   lua_setfield(L, -2, "getTime");
    lua_setglobal(L, "sys");

    lua_newtable(L);
    lua_pushcfunction(L, lua_LayoutWidth);  lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_LayoutHeight); lua_setfield(L, -2, "height");
    lua_setglobal(L, "layout");
}
