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
#include <functional>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

struct D2DState;

struct LuaWidgetManifest
{
    bool hasManifest = false;
    std::string name;
    std::string version;
    std::string description;
    int defaultColumns = 1;
    int defaultRows = 1;
    std::vector<std::string> permissions;
};

struct LuaDesktopItemInfo
{
    std::string id;
    std::string title;
    std::string path;
    std::string source;
    std::string type;
    bool selected = false;
};

struct LuaWidgetMenuItem
{
    int id = 0;
    std::string label;
    bool enabled = true;
    bool separator = false;
};

struct WidgetLogEntry
{
    std::string key;
    std::string level;
    std::string message;
};

struct WidgetDiagnosticEntry
{
    std::wstring widgetId;
    std::string name;
    std::wstring scriptPath;
    bool valid = false;
    bool hasManifest = false;
    std::vector<std::string> permissions;
    std::string lastError;
    std::vector<WidgetLogEntry> logs;
};

struct LuaInlineTextEditRequest
{
    std::wstring widgetId;
    std::string storageKey;
    std::string text;
    RECT localRect{};
    bool multiline = false;
    bool selectAll = true;
    int textColor = 0x000000;
};

struct LuaWidgetTheme
{
    int bg = 0x151A21;
    int border = 0xFFFFFF;
    float alpha = 0.36f;
    float gradientEndA = 0.65f;
};

struct LuaWidget
{
    std::wstring widgetId;
    std::string name;
    std::wstring filePath;
    LuaWidgetManifest manifest;
    std::unordered_set<std::string> permissions;
    int ref = LUA_NOREF;
    bool valid = false;
    bool customStyle = false;
    LuaWidgetTheme theme;
    FILETIME lastModified = {};
    RECT lastBounds{};
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

    using DesktopSnapshotProvider = std::function<std::vector<LuaDesktopItemInfo>()>;
    using WidgetTitleCallback = std::function<void(const std::wstring&, const std::wstring&)>;
    using InvalidateCallback = std::function<void()>;
    using DesktopPathAction = std::function<bool(const std::wstring&)>;
    using DesktopRefreshCallback = std::function<void()>;
    using InlineTextEditCallback = std::function<void(const LuaInlineTextEditRequest&)>;

    void SetDesktopSnapshotProvider(DesktopSnapshotProvider provider) { desktopSnapshotProvider_ = std::move(provider); }
    void SetSelectionProvider(DesktopSnapshotProvider provider) { selectionProvider_ = std::move(provider); }
    void SetWidgetTitleCallback(WidgetTitleCallback callback) { setWidgetTitleCallback_ = std::move(callback); }
    void SetInvalidateCallback(InvalidateCallback callback) { invalidateCallback_ = std::move(callback); }
    void SetDesktopOpenCallback(DesktopPathAction callback) { desktopOpenCallback_ = std::move(callback); }
    void SetDesktopRevealCallback(DesktopPathAction callback) { desktopRevealCallback_ = std::move(callback); }
    void SetDesktopRefreshCallback(DesktopRefreshCallback callback) { desktopRefreshCallback_ = std::move(callback); }
    void SetInlineTextEditCallback(InlineTextEditCallback callback) { inlineTextEditCallback_ = std::move(callback); }

    // Load a widget script into a sandbox for a specific widget instance
    bool EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath);
    void UnloadWidget(const std::wstring& widgetId);
    bool ReloadWidget(const std::wstring& widgetId);

    void RenderAll(ID2D1DeviceContext* context);
    void RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds);
    bool HasCustomStyle(const std::wstring& widgetId) const;
    void InvokeOpen(const std::wstring& widgetId);
    void InvokeClick(const std::wstring& widgetId, int x, int y);
    void InvokeMouseEvent(const std::wstring& widgetId, const char* callbackName, int x, int y,
        int button = 0, int delta = 0);
    std::vector<LuaWidgetMenuItem> GetContextMenu(const std::wstring& widgetId);
    void InvokeMenu(const std::wstring& widgetId, int menuId);
    void NotifyDesktopChanged(const std::string& reason);
    bool ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const;
    bool ReadCustomColors(const std::wstring& widgetId,
        float& bgR, float& bgG, float& bgB, float& alpha,
        float& borderR, float& borderG, float& borderB, float& gradientEndA) const;
    std::vector<WidgetErrorEntry> GetWidgetErrors() const;
    std::vector<WidgetDiagnosticEntry> GetWidgetDiagnostics() const;
    void ClearWidgetErrors();

    const std::vector<LuaWidget>& GetWidgets() const { return widgets_; }
    static std::vector<std::wstring> ListAvailable();
    static std::wstring GetWidgetDisplayName(const std::wstring& filename);
    static LuaWidgetManifest GetWidgetManifest(const std::wstring& filename);
    static bool GetWidgetDefaultSpan(const std::wstring& filename, int& columns, int& rows);

    // Render the ImGui editor for a specific widget instance
    bool RenderWidgetEditor(const std::wstring& widgetId, const std::wstring& widgetName);

    bool RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const;
    void RuntimeRecordError(const std::wstring& widgetId, const std::string& message);
    void RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message);
    std::vector<LuaDesktopItemInfo> RuntimeDesktopItems() const;
    std::vector<LuaDesktopItemInfo> RuntimeDesktopSelection() const;
    bool RuntimeOpenDesktopPath(const std::wstring& path);
    bool RuntimeRevealDesktopPath(const std::wstring& path);
    void RuntimeRefreshDesktop();
    void RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title);
    void RuntimeInvalidateHost();
    std::string RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const;
    void RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value);
    void RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request);
    LuaWidgetTheme RuntimeGetWidgetTheme(const std::wstring& widgetId) const;
    void SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme);

private:
    bool LoadWidget(const std::wstring& path, const std::wstring& widgetId);
    void RegisterDrawAPI(lua_State* L);
    void PushSafeEnvironment(lua_State* L, const LuaWidget& widget);
    int FindWidget(const std::wstring& widgetId) const;

    lua_State* L_ = nullptr;
    D2DState* d2dState_ = nullptr;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    std::vector<LuaWidget> widgets_;
    DesktopSnapshotProvider desktopSnapshotProvider_;
    DesktopSnapshotProvider selectionProvider_;
    WidgetTitleCallback setWidgetTitleCallback_;
    InvalidateCallback invalidateCallback_;
    DesktopPathAction desktopOpenCallback_;
    DesktopPathAction desktopRevealCallback_;
    DesktopRefreshCallback desktopRefreshCallback_;
    InlineTextEditCallback inlineTextEditCallback_;
};
