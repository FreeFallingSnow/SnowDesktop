#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

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
    std::string name;
    std::wstring filePath;
    int ref = LUA_NOREF;
    bool valid = false;
    bool customStyle = false;
    FILETIME lastModified = {};
};

class WidgetEngine
{
public:
    WidgetEngine() = default;
    ~WidgetEngine();

    bool Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory);
    void Shutdown();
    void ReloadAll();
    bool ReloadWidget(const std::wstring& scriptPath);
    void RenderAll(ID2D1DeviceContext* context);
    void RenderWidget(const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds,
        float bgR, float bgG, float bgB, float alpha, float borderR, float borderG, float borderB, float gradientEndA);
    bool HasCustomStyle(const std::wstring& scriptPath) const;
    void InvokeOpen(const std::wstring& scriptPath);
    std::string InvokeGetEditText(const std::wstring& scriptPath) const;
    bool HasEditSupport(const std::wstring& scriptPath) const;
    void InvokeEditCommit(const std::wstring& scriptPath, const std::string& text) const;
    void InvokeClick(const std::wstring& scriptPath, int x, int y);
    bool HandleKeyDown(const std::wstring& scriptPath, int vk);
    bool HandleChar(const std::wstring& scriptPath, wchar_t ch);
    void BlurActiveInput();
    bool ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const;

    const std::vector<LuaWidget>& GetWidgets() const { return widgets_; }
    static std::vector<std::wstring> ListAvailable();

private:
    bool LoadWidget(const std::wstring& path);
    void RegisterDrawAPI(lua_State* L);

    lua_State* L_ = nullptr;
    D2DState* d2dState_ = nullptr;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    std::vector<LuaWidget> widgets_;
};
