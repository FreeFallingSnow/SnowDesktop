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
};

class WidgetEngine
{
public:
    WidgetEngine() = default;
    ~WidgetEngine();

    bool Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory);
    void Shutdown();
    void ReloadAll();
    void RenderAll(ID2D1DeviceContext* context);
    void RenderWidget(const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds,
        float bgR, float bgG, float bgB, float alpha, float borderR, float borderG, float borderB, float gradientEndA);
    bool HasCustomStyle(const std::wstring& scriptPath) const;

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
