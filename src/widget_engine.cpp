#include "widget_engine.h"

#include <shlwapi.h>
#include <fstream>
#include <sstream>
#include <unordered_map>

static std::string ReadFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ── Storage (shared localStorage for Lua widgets) ────────────────
static std::unordered_map<std::string, std::string> g_storage;
static std::wstring g_storagePath;

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

// ── Drawing API ──────────────────────────────────────────────────
struct D2DState
{
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
    std::string storagePrefix;

    // Active input
    std::string activeInputId;
    std::string inputText;
    int cursorPos = 0;
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

    // Init storage path
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"SnowDesktop.storage.json");
    g_storagePath = exePath;
    LoadStorageFile();

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
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
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

    // Hot-reload: check if file changed or deleted
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        bool exists = GetFileAttributesExW(found->filePath.c_str(), GetFileExInfoStandard, &attr) != 0;
        if (!exists) { found->valid = false; return; }
        if (CompareFileTime(&attr.ftLastWriteTime, &found->lastModified) != 0)
        {
            luaL_unref(L_, LUA_REGISTRYINDEX, found->ref);
            std::wstring path = found->filePath;
            found->valid = false;
            found->ref = LUA_NOREF;
            LoadWidget(path);
            found = &widgets_.back();
        }
    }

    d2dState_->ctx = context;
    d2dState_->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));

    // Set storage prefix from script path
    {
        std::wstring name = scriptPath;
        auto slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        if (name.size() > 4 && name.substr(name.size() - 4) == L".lua")
            name = name.substr(0, name.size() - 4);
        int len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0, nullptr, nullptr);
        d2dState_->storagePrefix.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), &d2dState_->storagePrefix[0], len, nullptr, nullptr);
    }

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

void WidgetEngine::InvokeOpen(const std::wstring& scriptPath)
{
    for (auto& w : widgets_)
    {
        if (w.valid && w.filePath.size() >= scriptPath.size() &&
            w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
            if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
            lua_getfield(L_, -1, "onOpen");
            if (lua_isfunction(L_, -1))
                lua_pcall(L_, 0, 0, 0);
            else
                lua_pop(L_, 1);
            lua_pop(L_, 1);
            return;
        }
    }
}

std::string WidgetEngine::InvokeGetEditText(const std::wstring& scriptPath) const
{
    for (const auto& w : widgets_)
    {
        if (!w.valid || w.filePath.size() < scriptPath.size()) continue;
        if (w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) != 0) continue;

        lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return ""; }
        lua_getfield(L_, -1, "getEditText");
        if (lua_isfunction(L_, -1))
        {
            lua_pcall(L_, 0, 1, 0);
            const char* result = lua_tostring(L_, -1);
            std::string r = result ? result : "";
            lua_pop(L_, 2);
            return r;
        }
        lua_pop(L_, 2);
    }
    return "";
}

bool WidgetEngine::HasEditSupport(const std::wstring& scriptPath) const
{
    for (const auto& w : widgets_)
    {
        if (!w.valid || w.filePath.size() < scriptPath.size()) continue;
        if (w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) != 0) continue;

        lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
        lua_getfield(L_, -1, "getEditText");
        bool has = lua_isfunction(L_, -1) != 0;
        lua_pop(L_, 2);
        return has;
    }
    return false;
}

void WidgetEngine::InvokeEditCommit(const std::wstring& scriptPath, const std::string& text) const
{
    for (auto& w : widgets_)
    {
        if (!w.valid || w.filePath.size() < scriptPath.size()) continue;
        if (w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) != 0) continue;

        lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
        lua_getfield(L_, -1, "onEditCommit");
        if (lua_isfunction(L_, -1))
        {
            lua_pushstring(L_, text.c_str());
            lua_pcall(L_, 1, 0, 0);
        }
        else
        {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
        return;
    }
}

void WidgetEngine::BlurActiveInput()
{
    d2dState_->activeInputId.clear();
    d2dState_->inputText.clear();
    d2dState_->cursorPos = 0;
    focusedScriptPath_.clear();
}

bool WidgetEngine::HandleKeyDown(const std::wstring& scriptPath, int vk)
{
    if (d2dState_->activeInputId.empty()) return false;

    // Find matching widget to verify this is the focused one
    bool match = false;
    for (auto& w : widgets_)
    {
        if (!w.valid || w.filePath.size() < scriptPath.size()) continue;
        if (w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        { match = true; break; }
    }
    if (!match || focusedScriptPath_ != scriptPath) return false;

    auto& text = d2dState_->inputText;
    auto& pos = d2dState_->cursorPos;

    switch (vk)
    {
    case VK_LEFT:
        if (pos > 0) --pos;
        return true;
    case VK_RIGHT:
        if (pos < (int)text.size()) ++pos;
        return true;
    case VK_HOME:
        pos = 0;
        return true;
    case VK_END:
        pos = (int)text.size();
        return true;
    case VK_BACK:
        if (pos > 0) { text.erase(pos - 1, 1); --pos; }
        return true;
    case VK_DELETE:
        if (pos < (int)text.size()) text.erase(pos, 1);
        return true;
    case VK_RETURN:
    case VK_ESCAPE:
        // Commit: call onEditCommit
        InvokeEditCommit(scriptPath, text);
        BlurActiveInput();
        return true;
    }
    return false;
}

bool WidgetEngine::HandleChar(const std::wstring& scriptPath, wchar_t ch)
{
    if (d2dState_->activeInputId.empty()) return false;
    if (focusedScriptPath_ != scriptPath) return false;
    if (ch < 32) return false;  // control chars

    auto& text = d2dState_->inputText;
    auto& pos = d2dState_->cursorPos;

    // Insert char at cursor
    char utf8[8]{};
    int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, (int)sizeof(utf8), nullptr, nullptr);
    if (len > 0)
    {
        text.insert(pos, utf8, len);
        pos += len;
    }
    return true;
}

void WidgetEngine::InvokeClick(const std::wstring& scriptPath, int x, int y)
{
    focusedScriptPath_ = scriptPath;
    for (auto& w : widgets_)
    {
        if (!w.valid || w.filePath.size() < scriptPath.size()) continue;
        if (w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) != 0) continue;

        lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
        lua_getfield(L_, -1, "onClick");
        if (lua_isfunction(L_, -1))
        {
            lua_pushinteger(L_, x);
            lua_pushinteger(L_, y);
            lua_pcall(L_, 2, 0, 0);
        }
        else
        {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
        return;
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

static int lua_StorageRemove(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) return 0;
    g_storage.erase(s->storagePrefix + "." + key);
    SaveStorageFile();
    return 0;
}

static int lua_WidgetInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4);
    float h = (float)luaL_optnumber(L, 5, 24);
    const char* defaultText = luaL_optstring(L, 6, "");

    auto* s = GetD2D(L);
    if (!s || !s->ctx) { lua_pushstring(L, defaultText); return 1; }

    float rx = s->widgetRect.left + x;
    float ry = s->widgetRect.top + y;

    // Draw input background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    s->ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &bgBrush);
    if (bgBrush)
    {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(rx, ry, rx + w, ry + h), 4, 4);
        s->ctx->FillRoundedRectangle(rr, bgBrush.Get());
    }

    // Determine if this input is active
    bool isActive = (s->activeInputId == id);
    std::string text = isActive ? s->inputText : defaultText;

    // Draw text
    if (!text.empty())
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring wtext(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), wlen);

        ComPtr<IDWriteTextFormat> fmt;
        s->dwrite->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14, L"", &fmt);
        if (fmt)
        {
            ComPtr<ID2D1SolidColorBrush> textBrush;
            s->ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &textBrush);
            if (textBrush)
                s->ctx->DrawTextW(wtext.c_str(), (UINT32)(wtext.size() - 1), fmt.Get(),
                    D2D1::RectF(rx + 6, ry + 3, rx + w - 6, ry + h - 3), textBrush.Get());
        }
    }

    // Draw cursor
    if (isActive)
    {
        // Measure text up to cursorPos
        std::string before = text.substr(0, s->cursorPos);
        int wlen2 = MultiByteToWideChar(CP_UTF8, 0, before.c_str(), -1, nullptr, 0);
        std::wstring wbefore(wlen2, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, before.c_str(), -1, wbefore.data(), wlen2);

        ComPtr<IDWriteTextFormat> fmt;
        s->dwrite->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14, L"", &fmt);
        if (fmt)
        {
            ComPtr<IDWriteTextLayout> layout;
            s->dwrite->CreateTextLayout(wbefore.c_str(), (UINT32)(wbefore.size() - 1),
                fmt.Get(), w, h, &layout);
            if (layout)
            {
                DWRITE_TEXT_METRICS metrics;
                layout->GetMetrics(&metrics);
                float cx = rx + 6 + metrics.widthIncludingTrailingWhitespace;
                ComPtr<ID2D1SolidColorBrush> cursorBrush;
                s->ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.6f), &cursorBrush);
                if (cursorBrush)
                    s->ctx->DrawLine(D2D1::Point2F(cx, ry + 4), D2D1::Point2F(cx, ry + h - 4),
                        cursorBrush.Get(), 1.5f);
            }
        }
    }

    // Handle click to focus
    POINT mp;
    GetCursorPos(&mp);
    if (s->ctx)
    {
        // We can't get client coords easily from Lua. Use hit test from ImGui or mouse pos.
    }

    lua_pushstring(L, text.c_str());
    return 1;
}

static int lua_WidgetFocus(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) return 0;
    s->activeInputId = id;
    if (!id || !id[0])
    {
        s->inputText.clear();
        s->cursorPos = 0;
    }
    // Get engine via global... simpler: set focused path on engine
    // We reach engine through the d2dState ptr. Let's add focusedScriptPath tracking.
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
    lua_pushcfunction(L, lua_WidgetInput);lua_setfield(L, -2, "input");
    lua_pushcfunction(L, lua_WidgetFocus);lua_setfield(L, -2, "focus");
    lua_setglobal(L, "widget");

    lua_newtable(L);
    lua_pushcfunction(L, lua_GetTime);   lua_setfield(L, -2, "getTime");
    lua_setglobal(L, "sys");

    lua_newtable(L);
    lua_pushcfunction(L, lua_LayoutWidth);  lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_LayoutHeight); lua_setfield(L, -2, "height");
    lua_setglobal(L, "layout");

    lua_newtable(L);
    lua_pushcfunction(L, lua_StorageGet);   lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_StorageSet);   lua_setfield(L, -2, "set");
    lua_pushcfunction(L, lua_StorageRemove);lua_setfield(L, -2, "remove");
    lua_setglobal(L, "storage");
}
