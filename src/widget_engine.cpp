#include "widget_engine.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <algorithm>
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

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
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
    std::string storagePrefix;
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
    float maxWidth = static_cast<float>(luaL_optnumber(L, 6, 0));
    bool bold = lua_toboolean(L, 7) != 0;

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
        // Use TextLayout for word wrapping
        ComPtr<IDWriteTextLayout> layout;
        s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format.Get(), maxWidth, 5000.0f, &layout);
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
    std::string prefix = WideToUtf8(widgetId) + ".";
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

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath)
{
    if (FindWidget(widgetId) >= 0) return true;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"widgets");
    std::wstring fullPath = exePath;
    fullPath += L"\\";
    fullPath += scriptPath;
    return LoadWidget(fullPath, widgetId);
}

bool WidgetEngine::LoadWidget(const std::wstring& path, const std::wstring& widgetId)
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
        const char* err = lua_tostring(L_, -1);
        // store error to global storage for this widget path
        std::string idUtf8 = WideToUtf8(widgetId);
        g_storage[idUtf8 + ".lastError"] = err ? err : "(load error)";
        SaveStorageFile();
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
            const char* err = lua_tostring(L_, -1);
            std::string idUtf8 = WideToUtf8(widgetId);
            g_storage[idUtf8 + ".lastError"] = err ? err : "(pcall error)";
            SaveStorageFile();
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

    // Set storage prefix to widget instance ID
    int len = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
    d2dState_->storagePrefix.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &d2dState_->storagePrefix[0], len, nullptr, nullptr);

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
            lua_pcall(L_, 0, 0, 0);
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
    d2dState_->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));

    // Set storage prefix from widget instance ID
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        d2dState_->storagePrefix.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &d2dState_->storagePrefix[0], len, nullptr, nullptr);
    }

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
            std::string idUtf8 = WideToUtf8(widgetId);
            g_storage[idUtf8 + ".lastError"] = err ? err : "(render error)";
            SaveStorageFile();
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
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, "onOpen");
    if (lua_isfunction(L_, -1))
        lua_pcall(L_, 0, 0, 0);
    else
        lua_pop(L_, 1);
    lua_pop(L_, 1);
}

void WidgetEngine::InvokeClick(const std::wstring& widgetId, int x, int y)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
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

std::wstring WidgetEngine::GetWidgetDisplayName(const std::wstring& filename)
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"widgets");
    std::wstring fullPath = exePath;
    fullPath += L"\\";
    fullPath += filename;

    std::ifstream file(fullPath, std::ios::binary);
    if (!file) return {};
    std::string line;
    for (int i = 0; i < 10 && std::getline(file, line); ++i)
    {
        size_t pos = line.find_first_not_of(" \t\r");
        if (pos == std::string::npos) continue;
        if (line[pos] == '-' && pos + 1 < line.size() && line[pos + 1] == '-') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(pos, eq - pos);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key != "name") continue;

        size_t q1 = line.find('"', eq + 1);
        size_t q2 = line.find('\'', eq + 1);
        char quote = 0;
        if (q1 != std::string::npos && (q2 == std::string::npos || q1 < q2)) { quote = '"'; pos = q1; }
        else if (q2 != std::string::npos) { quote = '\''; pos = q2; }
        else continue;

        size_t qEnd = line.find(quote, pos + 1);
        if (qEnd == std::string::npos) continue;

        std::string utf8Name = line.substr(pos + 1, qEnd - pos - 1);
        // Convert UTF-8 to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), (int)utf8Name.size(), nullptr, 0);
        std::wstring wname(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), (int)utf8Name.size(), wname.data(), wlen);
        return wname;
    }
    // Fallback: filename without extension
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
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : g_storage)
    {
        lua_pushstring(L, kv.first.c_str());
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
    lua_pushcfunction(L, lua_DrawLine);  lua_setfield(L, -2, "line");
    lua_pushcfunction(L, lua_DrawCircle);lua_setfield(L, -2, "circle");
    lua_setglobal(L, "draw");

    lua_newtable(L_);
    lua_setglobal(L_, "widget");

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
    lua_setglobal(L, "imgui");
}
