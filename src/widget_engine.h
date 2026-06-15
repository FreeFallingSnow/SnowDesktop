/**
 * @file widget_engine.h
 * @brief 桌面小部件引擎 —— 负责 Lua 小部件的生命周期管理、沙箱执行与渲染调度
 *
 * WidgetEngine 是 SnowDesktop 中小部件子系统的核心模块。它维护一组 Lua 小部件实例，
 * 每个实例运行在独立的沙箱环境中，通过受限的 Lua API 与宿主交互。引擎提供以下核心能力：
 *
 * - 小部件加载/卸载/重载（基于文件监视的按需加载）
 * - Lua 沙箱隔离（受限环境、权限检查、安全 API 注册）
 * - 跨小部件渲染调度（基于 Direct2D 的合成与绘制）
 * - 桌面项目快照、文件打开/揭示、右键菜单等宿主回调桥接
 * - 运行时日志、错误收集与诊断
 * - 内联文本编辑请求的管理
 * - 小部件主题色自定义
 *
 * 设计要点：
 * - 所有 Lua 执行均阻留在引擎线程，回调通过函数指针桥接到外部
 * - 沙箱采用独立 lua_State，预注册只读/受限 API，防止越权
 * - 渲染通过 D2DState 结构管理资源，支持逐小部件独立绘制
 */
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

/**
 * @struct LuaWidgetManifest
 * @brief 小部件清单元数据，解析自小部件目录下的清单文件
 *
 * 清单文件定义了小部件的名称、版本、描述、默认栅格尺寸及其声明的权限列表。
 * 引擎在加载小部件时读取该结构，用于权限校验和 UI 布局。
 */
struct LuaWidgetManifest
{
    bool hasManifest = false;          ///< 是否存在清单文件
    std::string name;                  ///< 小部件显示名称
    std::string version;               ///< 版本号字符串
    std::string description;           ///< 功能描述文本
    int defaultColumns = 1;            ///< 默认占据列数（桌面栅格）
    int defaultRows = 1;               ///< 默认占据行数（桌面栅格）
    int minColumns = 1;                ///< 最少占据列数
    int minRows = 1;                   ///< 最少占据行数
    int maxColumns = 0;                ///< 最多占据列数，0 表示不限制
    int maxRows = 0;                   ///< 最多占据行数，0 表示不限制
    std::vector<std::string> permissions; ///< 声明的权限列表，如 "filesystem", "exec"
};

/**
 * @struct LuaDesktopItemInfo
 * @brief 桌面项目基本信息，由宿主提供给 Lua 沙箱的快照条目
 *
 * 当 Lua 脚本调用桌面查询 API 时，宿主通过回调返回该项目列表，
 * 用于支持小部件内的文件浏览、快速启动等功能。
 */
struct LuaDesktopItemInfo
{
    std::string id;         ///< 项目唯一标识符
    std::string title;      ///< 项目显示标题
    std::string path;       ///< 项目完整路径
    std::string source;     ///< 来源标识（如 "desktop", "startmenu", "custom"）
    std::string type;       ///< 项目类型（如 "file", "folder", "shortcut"）
    bool selected = false;  ///< 当前是否处于选中状态
};

/**
 * @struct LuaWidgetMenuItem
 * @brief 右键菜单项定义，由 Lua 脚本动态生成
 *
 * Lua 小部件可通过回调返回该结构数组，向宿主注册自定义的上下文菜单项。
 */
struct LuaWidgetMenuItem
{
    int id = 0;                ///< 菜单项标识符，回调时回传
    std::string label;         ///< 菜单项显示文本
    std::string icon;          ///< 可选 Font Awesome 图标字符
    bool enabled = true;       ///< 是否可用（灰显）
    bool separator = false;    ///< 是否为分隔线（为 true 时忽略其他字段）
};

/**
 * @struct WidgetLogEntry
 * @brief 日志条目，记录小部件运行时的单条日志信息
 *
 * 用于诊断面板日志展示，包含日志键名、级别和消息内容。
 */
struct WidgetLogEntry
{
    std::string key;      ///< 日志键名
    std::string level;    ///< 日志级别（"info", "warn", "error", "debug"）
    std::string message;  ///< 日志消息内容
};

/**
 * @struct WidgetDiagnosticEntry
 * @brief 诊断条目，描述单个小部件的完整诊断快照
 *
 * 用于调试面板展示，汇总小部件的标识、路径、权限、错误状态及日志。
 * 由 GetWidgetDiagnostics() 收集返回。
 */
struct WidgetDiagnosticEntry
{
    std::wstring widgetId;              ///< 小部件实例 ID
    std::string name;                   ///< 小部件名称
    std::wstring scriptPath;            ///< 脚本文件路径
    bool valid = false;                 ///< 是否成功加载并处于有效状态
    bool hasManifest = false;           ///< 是否包含清单文件
    std::vector<std::string> permissions; ///< 已授予的权限列表
    std::string lastError;              ///< 最近一次错误信息
    std::vector<WidgetLogEntry> logs;   ///< 运行时日志列表
};

/**
 * @struct LuaInlineTextEditRequest
 * @brief 内联文本编辑请求，由 Lua 脚本发起，宿主编排文本输入
 *
 * 当 Lua 小部件需要就地编辑文本时（如重命名文件），引擎通过回调将此结构
 * 传递给宿主，由宿主显示内联输入框并将结果写回存储。
 */
struct LuaInlineTextEditRequest
{
    std::wstring widgetId;   ///< 发起请求的小部件 ID
    std::string storageKey;  ///< 存储键名，宿主完成编辑后通过该键写回
    std::string text;        ///< 初始文本内容
    RECT localRect{};        ///< 输入框在小部件本地坐标系中的位置和尺寸
    bool multiline = false;  ///< 是否支持多行输入
    bool selectAll = true;   ///< 是否自动全选文本
    int textColor = 0x000000; ///< 文本颜色（ARGB 格式，默认黑色）
};

/**
 * @struct LuaWidgetTheme
 * @brief 小部件主题色定义，控制背景、边框和渐变透明度
 *
 * 当小部件启用自定义样式时，引擎使用此结构中的颜色值替代默认渲染。
 *
 * @note 颜色字段采用 ARGB 格式（0xAARRGGBB），Alpha 通道默认不透明。
 */
struct LuaWidgetTheme
{
    int bg = 0x151A21;          ///< 背景色（ARGB 格式，默认深灰蓝）
    int border = 0xFFFFFF;      ///< 边框色（ARGB 格式，默认白色）
    float alpha = 0.36f;        ///< 整体透明度（0~1，默认 0.36）
    float gradientEndA = 0.65f; ///< 渐变末端透明度（0~1，默认 0.65）
};

/**
 * @struct LuaWidget
 * @brief 运行时小部件实例的完整状态描述
 *
 * 引擎内部维护一组 LuaWidget 实例，每个实例对应一个已加载的 Lua 小部件脚本。
 * 该结构包含了小部件的标识信息、清单元数据、已授予的权限集、Lua 引用、
 * 有效性标志、主题配置、文件时间戳及最后一次渲染的边界矩形。
 *
 * @note ref 字段存储 Lua 注册表引用（LUA_NOREF 表示无效），
 *       用于在沙箱 lua_State 中快速定位小部件的主环境表。
 */
struct LuaWidget
{
    std::wstring widgetId;               ///< 小部件实例唯一 ID
    std::string name;                    ///< 小部件名称
    std::wstring filePath;               ///< Lua 脚本文件的完整路径
    LuaWidgetManifest manifest;          ///< 从清单文件解析的元数据
    std::unordered_set<std::string> permissions; ///< 已授予的权限集合
    int ref = LUA_NOREF;                 ///< Lua 注册表引用，LUA_NOREF 表示无效
    bool valid = false;                  ///< 是否已成功加载且可执行
    bool customStyle = false;            ///< 是否启用了自定义主题样式
    LuaWidgetTheme theme;                ///< 自定义主题配置（当 customStyle 为 true 时生效）
    FILETIME lastModified = {};          ///< 脚本文件最后修改时间，用于变更检测
    RECT lastBounds{};                   ///< 最后一次渲染时的边界矩形
};

/**
 * @struct WidgetErrorEntry
 * @brief 错误条目，记录引擎或小部件运行时产生的单条错误
 *
 * 用于 GetWidgetErrors() 返回，供外部诊断 UI 展示。
 */
struct WidgetErrorEntry
{
    std::string key;      ///< 错误键名，用于去重或归类
    std::string message;  ///< 错误描述信息
};

class WidgetEngine
{
public:
    /**
     * @brief 默认构造函数
     */
    WidgetEngine() = default;

    /**
     * @brief 析构函数，释放所有小部件实例和 Lua 状态
     */
    ~WidgetEngine();

    /**
     * @brief 初始化引擎
     * @param d2dContext Direct2D 设备上下文指针
     * @param dwriteFactory DirectWrite 工厂接口指针
     * @return 初始化成功返回 true，否则返回 false
     */
    bool Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory);

    /**
     * @brief 关闭引擎，释放所有资源，卸载所有已加载的小部件
     */
    void Shutdown();

    using DesktopSnapshotProvider = std::function<std::vector<LuaDesktopItemInfo>()>;
    using WidgetTitleCallback = std::function<void(const std::wstring&, const std::wstring&)>;
    using InvalidateCallback = std::function<void()>;
    using DesktopPathAction = std::function<bool(const std::wstring&)>;
    using DesktopRefreshCallback = std::function<void()>;
    using InlineTextEditCallback = std::function<void(const LuaInlineTextEditRequest&)>;
    using NotifyCallback = std::function<void(const std::wstring&, const std::wstring&)>;

    /** @brief 设置桌面快照提供者回调 */
    void SetDesktopSnapshotProvider(DesktopSnapshotProvider provider) { desktopSnapshotProvider_ = std::move(provider); }
    /** @brief 设置选中项提供者回调 */
    void SetSelectionProvider(DesktopSnapshotProvider provider) { selectionProvider_ = std::move(provider); }
    /** @brief 设置小部件标题变更回调 */
    void SetWidgetTitleCallback(WidgetTitleCallback callback) { setWidgetTitleCallback_ = std::move(callback); }
    /** @brief 设置失效回调（请求宿主重绘） */
    void SetInvalidateCallback(InvalidateCallback callback) { invalidateCallback_ = std::move(callback); }
    /** @brief 设置桌面文件打开回调 */
    void SetDesktopOpenCallback(DesktopPathAction callback) { desktopOpenCallback_ = std::move(callback); }
    /** @brief 设置桌面文件揭示回调（在资源管理器中定位） */
    void SetDesktopRevealCallback(DesktopPathAction callback) { desktopRevealCallback_ = std::move(callback); }
    /** @brief 设置桌面刷新回调 */
    void SetDesktopRefreshCallback(DesktopRefreshCallback callback) { desktopRefreshCallback_ = std::move(callback); }
    /** @brief 设置内联文本编辑回调 */
    void SetInlineTextEditCallback(InlineTextEditCallback callback) { inlineTextEditCallback_ = std::move(callback); }
    /** @brief 设置系统通知回调 */
    void SetNotifyCallback(NotifyCallback callback) { notifyCallback_ = std::move(callback); }

    /**
     * @brief 确保小部件已加载到沙箱中
     * @param widgetId 小部件实例 ID
     * @param scriptPath Lua 脚本文件路径
     * @return 加载成功或已存在返回 true，否则返回 false
     */
    bool EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath);

    /**
     * @brief 卸载指定小部件实例
     * @param widgetId 要卸载的小部件 ID
     */
    void UnloadWidget(const std::wstring& widgetId);

    /**
     * @brief 重新加载指定小部件实例
     * @param widgetId 要重新加载的小部件 ID
     * @return 重载成功返回 true，否则返回 false
     */
    bool ReloadWidget(const std::wstring& widgetId);

    /**
     * @brief 渲染所有已加载的小部件
     * @param context Direct2D 设备上下文
     */
    void RenderAll(ID2D1DeviceContext* context);

    /**
     * @brief 渲染指定小部件实例
     * @param widgetId 小部件实例 ID
     * @param scriptPath Lua 脚本路径
     * @param context Direct2D 设备上下文
     * @param bounds 小部件在桌面栅格中的边界矩形
     */
    void RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath, ID2D1DeviceContext* context, RECT bounds);

    /**
     * @brief 查询指定小部件是否启用了自定义主题样式
     * @param widgetId 小部件实例 ID
     * @return 启用了自定义主题返回 true，否则返回 false
     */
    bool HasCustomStyle(const std::wstring& widgetId) const;

    /**
     * @brief 触发小部件的打开回调
     * @param widgetId 小部件实例 ID
     */
    void InvokeOpen(const std::wstring& widgetId);

    /**
     * @brief 触发小部件的点击回调
     * @param widgetId 小部件实例 ID
     * @param x 点击位置的 x 坐标
     * @param y 点击位置的 y 坐标
     */
    void InvokeClick(const std::wstring& widgetId, int x, int y);

    /**
     * @brief 触发小部件的鼠标事件回调
     * @param widgetId 小部件实例 ID
     * @param callbackName 回调函数名称
     * @param x 鼠标 x 坐标
     * @param y 鼠标 y 坐标
     * @param button 鼠标按钮编号（0=左键, 1=右键, 2=中键）
     * @param delta 滚轮滚动量
     */
    void InvokeMouseEvent(const std::wstring& widgetId, const char* callbackName, int x, int y,
        int button = 0, int delta = 0);

    /**
     * @brief 获取指定小部件的右键菜单项列表
     * @param widgetId 小部件实例 ID
     * @return 菜单项数组
     */
    std::vector<LuaWidgetMenuItem> GetContextMenu(const std::wstring& widgetId);

    /**
     * @brief 触发小部件的菜单项点击回调
     * @param widgetId 小部件实例 ID
     * @param menuId 菜单项标识符
     */
    void InvokeMenu(const std::wstring& widgetId, int menuId);

    /**
     * @brief 通知引擎桌面内容已变更
     * @param reason 变更原因描述字符串
     */
    void NotifyDesktopChanged(const std::string& reason);

    /**
     * @brief 读取 Lua 脚本中的布尔标志值
     * @param scriptPath Lua 脚本文件路径
     * @param flag 标志名称
     * @param defaultVal 默认值（脚本中无定义时使用）
     * @return 布尔标志值
     */
    bool ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const;

    /**
     * @brief 读取小部件的自定义颜色值
     * @param widgetId 小部件实例 ID
     * @param bgR 输出：背景色红色分量
     * @param bgG 输出：背景色绿色分量
     * @param bgB 输出：背景色蓝色分量
     * @param alpha 输出：透明度
     * @param borderR 输出：边框色红色分量
     * @param borderG 输出：边框色绿色分量
     * @param borderB 输出：边框色蓝色分量
     * @param gradientEndA 输出：渐变末端透明度
     * @return 成功读取返回 true
     */
    bool ReadCustomColors(const std::wstring& widgetId,
        float& bgR, float& bgG, float& bgB, float& alpha,
        float& borderR, float& borderG, float& borderB, float& gradientEndA) const;

    /**
     * @brief 获取所有小部件运行时的错误条目列表
     * @return 错误条目数组
     */
    std::vector<WidgetErrorEntry> GetWidgetErrors() const;

    /**
     * @brief 获取所有小部件的完整诊断信息列表
     * @return 诊断条目数组
     */
    std::vector<WidgetDiagnosticEntry> GetWidgetDiagnostics() const;

    /**
     * @brief 清除所有运行时错误记录
     */
    void ClearWidgetErrors();

    /** @brief 获取所有已加载的小部件列表（只读引用） */
    const std::vector<LuaWidget>& GetWidgets() const { return widgets_; }

    /**
     * @brief 枚举所有可用的小部件
     * @return 可用小部件文件名列表
     */
    static std::vector<std::wstring> ListAvailable();

    /**
     * @brief 获取小部件的显示名称
     * @param filename 小部件文件名
     * @return 显示名称字符串
     */
    static std::wstring GetWidgetDisplayName(const std::wstring& filename);

    /**
     * @brief 获取小部件的清单元数据
     * @param filename 小部件文件名
     * @return 解析后的清单元数据
     */
    static LuaWidgetManifest GetWidgetManifest(const std::wstring& filename);

    /**
     * @brief 获取小部件在桌面栅格中的默认跨列/跨行数
     * @param filename 小部件文件名
     * @param columns 输出：默认列数
     * @param rows 输出：默认行数
     * @return 成功读取返回 true
     */
    static bool GetWidgetDefaultSpan(const std::wstring& filename, int& columns, int& rows);

    /**
     * @brief 渲染指定小部件的 ImGui 编辑器界面
     * @param widgetId 小部件实例 ID
     * @param widgetName 小部件名称
     * @return 渲染成功返回 true
     */
    bool RenderWidgetEditor(const std::wstring& widgetId, const std::wstring& widgetName);

    /**
     * @brief 检查小部件是否拥有指定运行时权限
     * @param widgetId 小部件实例 ID
     * @param permission 权限名称
     * @return 拥有权限返回 true，否则返回 false
     */
    bool RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const;

    /**
     * @brief 记录运行时错误
     * @param widgetId 小部件实例 ID
     * @param message 错误消息
     */
    void RuntimeRecordError(const std::wstring& widgetId, const std::string& message);

    /**
     * @brief 添加一条运行时日志
     * @param widgetId 小部件实例 ID
     * @param level 日志级别
     * @param message 日志消息
     */
    void RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message);

    /**
     * @brief 获取当前桌面项目快照
     * @return 桌面项目信息列表
     */
    std::vector<LuaDesktopItemInfo> RuntimeDesktopItems() const;

    /**
     * @brief 获取当前桌面选中项快照
     * @return 选中项信息列表
     */
    std::vector<LuaDesktopItemInfo> RuntimeDesktopSelection() const;

    /**
     * @brief 通过宿主打开指定路径
     * @param path 要打开的路径
     * @return 操作成功返回 true
     */
    bool RuntimeOpenDesktopPath(const std::wstring& path);

    /**
     * @brief 通过宿主在文件管理器中定位指定路径
     * @param path 要揭示的路径
     * @return 操作成功返回 true
     */
    bool RuntimeRevealDesktopPath(const std::wstring& path);

    /**
     * @brief 请求宿主刷新桌面内容
     */
    void RuntimeRefreshDesktop();

    /**
     * @brief 设置小部件标题
     * @param widgetId 小部件实例 ID
     * @param title 新标题
     */
    void RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title);

    /**
     * @brief 请求宿主重绘画布
     */
    void RuntimeInvalidateHost();

    /**
     * @brief 获取小部件的持久化存储值
     * @param widgetId 小部件实例 ID
     * @param key 存储键名
     * @return 存储的字符串值，键不存在返回空字符串
     */
    std::string RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const;

    /**
     * @brief 设置小部件的持久化存储值
     * @param widgetId 小部件实例 ID
     * @param key 存储键名
     * @param value 要存储的值
     */
    void RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value);

    /**
     * @brief 发起内联文本编辑请求
     * @param request 编辑请求参数
     */
    void RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request);

    /**
     * @brief 获取小部件的当前主题配置
     * @param widgetId 小部件实例 ID
     * @return 主题配置
     */
    LuaWidgetTheme RuntimeGetWidgetTheme(const std::wstring& widgetId) const;

    /**
     * @brief 设置小部件的自定义主题
     * @param widgetId 小部件实例 ID
     * @param theme 主题配置
     */
    void SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme);

    /**
     * @brief 发送系统通知
     * @param title 通知标题
     * @param message 通知内容
     */
    void RuntimeNotify(const std::wstring& title, const std::wstring& message);

private:
    /**
     * @brief 内部加载小部件脚本到沙箱
     * @param path Lua 脚本文件路径
     * @param widgetId 小部件实例 ID
     * @return 加载成功返回 true
     */
    bool LoadWidget(const std::wstring& path, const std::wstring& widgetId);

    /**
     * @brief 向 Lua 状态机注册绘制 API
     * @param L Lua 状态机指针
     */
    void RegisterDrawAPI(lua_State* L);

    /**
     * @brief 推入一个安全的沙箱环境表
     * @param L Lua 状态机指针
     * @param widget 小部件引用信息
     */
    void PushSafeEnvironment(lua_State* L, const LuaWidget& widget);

    /**
     * @brief 按 ID 查找小部件在内部数组中的索引
     * @param widgetId 小部件实例 ID
     * @return 找到返回索引，否则返回 -1
     */
    int FindWidget(const std::wstring& widgetId) const;

    lua_State* L_ = nullptr;                           ///< 全局 Lua 状态机指针
    D2DState* d2dState_ = nullptr;                     ///< Direct2D 渲染状态管理对象指针
    ComPtr<ID2D1DeviceContext> d2dContext_;            ///< Direct2D 设备上下文
    ComPtr<IDWriteFactory> dwriteFactory_;             ///< DirectWrite 工厂接口
    std::vector<LuaWidget> widgets_;                   ///< 已加载的小部件实例列表
    DesktopSnapshotProvider desktopSnapshotProvider_;  ///< 桌面快照提供者回调
    DesktopSnapshotProvider selectionProvider_;        ///< 当前选中项提供者回调
    WidgetTitleCallback setWidgetTitleCallback_;       ///< 设置小部件标题的回调
    InvalidateCallback invalidateCallback_;            ///< 请求宿主重绘的回调
    DesktopPathAction desktopOpenCallback_;            ///< 打开桌面路径的回调
    DesktopPathAction desktopRevealCallback_;          ///< 在资源管理器中定位路径的回调
    DesktopRefreshCallback desktopRefreshCallback_;    ///< 刷新桌面的回调
    InlineTextEditCallback inlineTextEditCallback_;    ///< 内联文本编辑请求的回调
    NotifyCallback notifyCallback_;                     ///< 系统通知回调
};
