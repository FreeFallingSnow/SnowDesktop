#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

struct ImGuiContext;

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct D2DState;

struct LuaWidget
{
    std::wstring widgetId;
    std::string name;
    std::wstring filePath;
    int ref = LUA_NOREF;
    bool valid = false;
    bool customStyle = false;
    FILETIME lastModified = {};
};

struct WidgetErrorEntry
{
    std::string key;
    std::string message;
};

class WidgetEngine
{
public:
    WidgetEngine() = default;
    ~WidgetEngine();

    bool Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory);
    void Shutdown();

    // Load a widget script into a sandbox for a specific widget instance
    bool EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath);
    void UnloadWidget(const std::wstring& widgetId);

    void RenderAll(ID2D1DeviceContext* context);
    void RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds);
    bool HasCustomStyle(const std::wstring& widgetId) const;
    void InvokeOpen(const std::wstring& widgetId);
    void InvokeClick(const std::wstring& widgetId, int x, int y);
    bool ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const;
    bool ReadCustomColors(const std::wstring& widgetId,
        float& bgR, float& bgG, float& bgB, float& alpha,
        float& borderR, float& borderG, float& borderB, float& gradientEndA) const;
    std::vector<WidgetErrorEntry> GetWidgetErrors() const;
    void ClearWidgetErrors();

    const std::vector<LuaWidget>& GetWidgets() const { return widgets_; }
    static std::vector<std::wstring> ListAvailable();
    static std::wstring GetWidgetDisplayName(const std::wstring& filename);

    // Render the ImGui editor for a specific widget instance
    bool RenderWidgetEditor(const std::wstring& widgetId, const std::wstring& widgetName);

private:
    bool LoadWidget(const std::wstring& path, const std::wstring& widgetId);
    void RegisterDrawAPI(lua_State* L);
    int FindWidget(const std::wstring& widgetId) const;

    lua_State* L_ = nullptr;
    D2DState* d2dState_ = nullptr;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    std::vector<LuaWidget> widgets_;
};
