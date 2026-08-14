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
#include "system_snapshot.h"
#include "http_runtime.h"
#include "calendar_service.h"
#include "widget_package.h"
#include "steam_workshop_sync.h"
#include "lua_runtime.h"
#include "widget_layout_context.h"
#include "widget_runtime_diagnostics.h"
#include "widget_runtime_health.h"
#include "widget_host_state.h"
#include "widget_runtime_scheduler.h"
#include "widget_lua_lifecycle.h"
#include "widget_data_broker.h"
#include "widget_task_broker.h"
#include "widget_media_task_executor.h"
#include "widget_app_task_executor.h"
#include "widget_trusted_gesture.h"
#include "widget_system_data_provider.h"
#include "widget_audio_analysis_provider.h"
#include "widget_interaction_region.h"

struct ImGuiContext;
struct PersonalizationSettings;

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
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
    struct Setting
    {
        std::string key;
        std::string label;
        std::string type;
        std::string defaultValue;
        double minValue = 0.0;
        double maxValue = 100.0;
        std::vector<std::string> options;
        std::vector<std::string> optionLabels;
    };
    struct SettingPreset
    {
        std::string id;
        std::string label;
        std::unordered_map<std::string, std::string> values;
        bool isDefault = false;
    };
    bool hasManifest = false;          ///< 是否存在清单文件
    int schemaVersion = 0;             ///< 组件包清单版本
    std::string packageId;             ///< 不可变组件包 UUID
    std::string slug;                  ///< 人类可读短名称
    int apiVersion = 0;                ///< Lua 宿主 API 契约版本
    int dataVersion = 1;               ///< 实例存储结构版本
    std::string name;                  ///< 小部件显示名称
    std::string nameKey;               ///< 小部件名称翻译键
    std::string version;               ///< 版本号字符串
    std::string description;           ///< 功能描述文本
    std::string descriptionKey;        ///< 小部件描述翻译键
    std::vector<std::string> titleKeys; ///< 脚本可管理的本地化标题键
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> locales; ///< 组件自带的多语言文本
    int defaultColumns = 1;            ///< 默认占据列数（桌面栅格）
    int defaultRows = 1;               ///< 默认占据行数（桌面栅格）
    int minColumns = 1;                ///< 最少占据列数
    int minRows = 1;                   ///< 最少占据行数
    int maxColumns = 0;                ///< 最多占据列数，0 表示不限制
    int maxRows = 0;                   ///< 最多占据行数，0 表示不限制
    int refreshIntervalMs = 0;          ///< manifest 声明的自动刷新间隔（ms），0 = 不自动刷新
    std::vector<std::string> networkDomains; ///< 兼容保留的网络域名元数据
    std::vector<std::string> requiredFeatures; ///< 激活前必须满足的 v2 宿主特性
    std::vector<std::string> optionalFeatures; ///< 可由脚本探测并降级的 v2 宿主特性
    std::unordered_map<std::string, snowdesktop::widget::PackageResource>
        resources; ///< v2 清单声明的包内图片和私有字体
    std::vector<Setting> settings;        ///< 宿主生成的声明式设置
    std::vector<SettingPreset> presets;   ///< 宿主生成的声明式预设
    std::string publisher;
    std::string minHostVersion;
    std::string preview;
    std::string previewIntroduction;
    std::string previewIntroductionKey;
    /// Preview-only storage values supplied by the component author.
    /// They are visible through storage.get() but never persisted.
    std::unordered_map<std::string, std::string> previewStorage;
    /// Maps preview storage names to keys in the manifest locale catalogs.
    std::unordered_map<std::string, std::string> previewStorageKeys;
    std::vector<snowdesktop::widget::PreviewVariant> previewVariants;
    std::string entry;
    std::string signature;
    bool signatureValid = true;
    std::vector<std::string> permissions; ///< 必需权限列表
    std::vector<std::string> optionalPermissions; ///< 可拒绝并降级的权限列表
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
    std::string icon;          ///< 可选图标字符
    std::string iconFont = "fa"; ///< "fa"（兼容默认）或 "fluent"
    bool enabled = true;       ///< 是否可用（灰显）
    bool checked = false;      ///< 是否显示选中标记
    bool separator = false;    ///< 是否为分隔线（为 true 时忽略其他字段）
    bool v2Action = false;
    std::string actionId;
    std::string targetKey;
    snowdesktop::widget_runtime::InteractionValue contextValue;
    std::uint64_t interactionGeneration = 0;
};

/**
 * @struct WidgetLogEntry
 * @brief 日志条目，记录小部件运行时的单条日志信息
 *
 * 用于诊断面板日志展示，包含日志键名、级别和消息内容。
 */
using WidgetLogEntry = snowdesktop::widget_runtime::LogEntry;

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
    std::string packageId;
    std::string packageVersion;
    bool valid = false;                 ///< 是否成功加载并处于有效状态
    bool hasManifest = false;           ///< 是否包含清单文件
    std::vector<std::string> permissions; ///< 已授予的权限列表
    std::string lastError;              ///< 最近一次错误信息
    std::vector<WidgetLogEntry> logs;   ///< 运行时日志列表
    std::size_t memoryBytes = 0;
    std::size_t memoryLimit = 0;
    double lastCallbackMs = 0.0;
    bool executionQuotaExceeded = false;
    bool memoryQuotaExceeded = false;
    bool circuitOpen = false;
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
    bool liveUpdate = false; ///< 输入过程中是否实时写回存储
    int textColor = 0x000000; ///< 文本颜色（ARGB 格式，默认黑色）
    int backgroundColor = 0xFFFFFF; ///< 编辑框背景颜色（RGB，默认白色）
    float fontSize = 15.0f;  ///< 编辑框字号（像素，默认 15）
};

struct LuaWidgetPanelRequest
{
    std::wstring widgetId;
    std::wstring title;
    int width = 520;
    int height = 620;
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
    float alpha = 0.36f;        ///< 背景透明度（0~1，默认 0.36）
    float borderAlpha = 0.40f;  ///< 边框透明度（0~1，默认 0.40）
    float gradientEndA = 0.65f; ///< 渐变末端透明度（0~1，默认 0.65）
    float cornerRadius = 12.0f; ///< 圆角半径（cu）
    int contentTheme = 0;       ///< 文字颜色主题 (0=浅色/白字, 1=深色/黑字)
};

struct LuaWidgetSurfaceContext
{
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    RECT monitorBounds{};
    RECT workArea{};
    bool monitorAvailable = false;
    bool primaryMonitor = false;
};

struct LuaWidgetContextState
{
    LuaWidgetSurfaceContext surface;
    bool visible = false;
    bool preview = false;
    bool focused = false;
    bool selected = false;
};

struct LuaWidgetDataSnapshot
{
    std::string topic;
    bool available = false;
    bool stale = true;
    bool warmingUp = false;
    std::int64_t timestampMs = 0;
    std::string error;
    snowdesktop::widget_runtime::WidgetCpuDataSnapshot cpu;
    snowdesktop::widget_runtime::WidgetMemoryDataSnapshot memory;
    snowdesktop::widget_runtime::WidgetPowerDataSnapshot power;
    snowdesktop::widget_runtime::WidgetNetworkStatusDataSnapshot
        networkStatus;
    snowdesktop::widget_runtime::WidgetNetworkTrafficDataSnapshot
        networkTraffic;
    snowdesktop::widget_runtime::WidgetGpuDataSnapshot gpu;
    snowdesktop::widget_runtime::WidgetStorageVolumesDataSnapshot
        storageVolumes;
    snowdesktop::widget_runtime::WidgetStorageIoDataSnapshot storageIo;
    snowdesktop::widget_runtime::WidgetDisplayTopologyDataSnapshot
        displayTopology;
    snowdesktop::widget_runtime::WidgetDisplayDataSnapshot displayCurrent;
    snowdesktop::widget_runtime::WidgetAudioOutputDefaultDataSnapshot
        audioOutputDefault;
    snowdesktop::widget_runtime::WidgetAudioOutputVolumeDataSnapshot
        audioOutputVolume;
    snowdesktop::widget_runtime::WidgetAudioAnalysisDataSnapshot
        audioAnalysis;
    snowdesktop::widget_runtime::WidgetMediaSessionsDataSnapshot
        mediaSessions;
    snowdesktop::widget_runtime::WidgetMediaCurrentDataSnapshot
        mediaCurrent;
    snowdesktop::widget_runtime::WidgetMediaTimelineDataSnapshot
        mediaTimeline;
    std::vector<LuaDesktopItemInfo> desktopItems;
    std::uint64_t desktopRevision = 0;
    std::string desktopChangeReason;
    std::vector<snowdesktop::calendar::CalendarEvent> calendarEvents;
    std::string calendarSelectedDate;
    std::string calendarRangeStart;
    std::string calendarRangeEnd;
    std::uint64_t calendarRevision = 0;
    bool calendarTruncated = false;
    std::string appIndexState;
    std::uint64_t appIndexRevision = 0;
};

struct LuaApplicationCatalogSnapshot
{
    std::string state = "unavailable";
    std::vector<snowdesktop::widget_runtime::WidgetAppCatalogEntry> entries;
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
    struct ApplicationReference
    {
        std::string catalogId;
        std::string launchTarget;
        std::uint64_t catalogRevision = 0;
    };

    struct HostControl
    {
        enum class Type { Button, Toggle, Input, Scroll };
        Type type = Type::Button;
        std::string id;
        std::string storageKey;
        RECT rect{};
        bool value = false;
        bool selectAll = true;
        bool liveUpdate = false;
        bool multiline = false;
        float fontSize = 15.0f;
        float padding = 8.0f;
        int contentHeight = 0;
        int viewportHeight = 0;
    };
    std::wstring widgetId;               ///< 小部件实例唯一 ID
    std::string packageId;                ///< 组件包 UUID
    std::filesystem::path packageRoot;    ///< 已校验组件包根目录
    lua_State* state = nullptr;           ///< Per-instance Lua VM
    std::unique_ptr<LuaRuntimeQuota> quota; ///< VM memory/instruction accounting
    std::string name;                    ///< 小部件名称
    std::wstring filePath;               ///< Lua 脚本文件的完整路径
    LuaWidgetManifest manifest;          ///< 从清单文件解析的元数据
    std::unordered_set<std::string> permissions; ///< 已授予的权限集合
    int ref = LUA_NOREF;                 ///< Lua 注册表引用，LUA_NOREF 表示无效
    bool valid = false;                  ///< 是否已成功加载且可执行
    bool customStyle = false;            ///< 是否启用了自定义主题样式
    bool followPersonalizationDefault = false; ///< 尚未保存外观状态时是否默认跟随全局
    LuaWidgetTheme theme;                ///< 自定义主题配置（当 customStyle 为 true 时生效）
    LuaWidgetSurfaceContext surfaceContext; ///< 当前显示器、工作区与 DPI 摘要
    std::vector<LuaWidgetManifest::Setting> scriptSettings; ///< Lua 顶层声明式设置
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets; ///< Lua 顶层声明式预设
    FILETIME lastModified = {};          ///< 脚本文件最后修改时间，用于变更检测
    RECT lastBounds{};                   ///< 最后一次渲染时的边界矩形
    int lastColumns = 1;
    int lastRows = 1;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics;
    bool hostVisible = false;
    bool usesSystemSnapshot = false;
    bool usesMediaSnapshot = false;
    double lastCallbackMs = 0.0;
    snowdesktop::widget_runtime::RuntimeHealth health;
    std::chrono::steady_clock::time_point notificationWindow{};
    std::uint32_t notificationsInWindow = 0;
    std::chrono::steady_clock::time_point lastRenderTime{};
    UINT_PTR refreshTimerId = 0;        ///< 宿主统一截止时间队列分配的周期令牌（0 = 未开）
    UINT_PTR namedTimerId = 0;          ///< widget.setTimer 命名定时器共用的下一次唤醒令牌
    snowdesktop::widget_runtime::NamedTimerSchedule namedTimers;
    std::vector<HostControl> hostControls;
    std::unordered_map<std::string, int> scrollOffsets;
    std::unordered_map<std::uint64_t, std::string> dataSubscriptions;
    std::unordered_set<std::uint64_t> taskIds;
    std::unordered_map<std::string, ApplicationReference>
        applicationReferences;
    snowdesktop::widget_runtime::WidgetInteractionRegions interactionRegions;
    std::uint64_t runtimeToken = 0;
    bool preview = false;
    std::unordered_map<std::string, std::string> previewStorage;
    snowdesktop::widget_runtime::WidgetLuaLifecycle lifecycle;
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
    /** Initialize a render-only engine without live services or disk state. */
    bool InitPreview(ID2D1DeviceContext* d2dContext,
        IDWriteFactory* dwriteFactory);
    bool IsPreviewOnly() const { return previewOnly_; }

    /**
     * @brief 关闭引擎，释放所有资源，卸载所有已加载的小部件
     */
    void Shutdown();

    using DesktopSnapshotProvider = std::function<std::vector<LuaDesktopItemInfo>()>;
    using ApplicationSearchProvider = std::function<std::vector<LuaDesktopItemInfo>(const std::string&, int)>;
    using ApplicationCatalogProvider =
        std::function<LuaApplicationCatalogSnapshot()>;
    using ApplicationIndexStatusProvider = std::function<std::string()>;
    using EverythingSearchProvider = std::function<std::vector<LuaDesktopItemInfo>(const std::string&, int)>;
    using WidgetSelectedProvider = std::function<bool(const std::wstring&)>;
    using SelectedWidgetPackageProvider = std::function<std::wstring()>;
    using WidgetTitleCallback = std::function<void(const std::wstring&, const std::wstring&)>;
    using InvalidateCallback = std::function<void(const std::wstring&)>;
    using DesktopPathAction = std::function<bool(const std::wstring&)>;
    using DesktopRefreshCallback = std::function<void()>;
    using InlineTextEditCallback = std::function<void(const LuaInlineTextEditRequest&)>;
    using WidgetPanelOpenCallback = std::function<void(const LuaWidgetPanelRequest&)>;
    using WidgetPanelCloseCallback = std::function<void(const std::wstring&)>;
    using HostInputFocusCallback = std::function<void()>;
    using NotifyCallback = std::function<void(const std::wstring&, const std::wstring&)>;
    using WidgetTimerRequestCallback = std::function<UINT_PTR(const std::wstring& widgetId, UINT intervalMs)>;
    using WidgetTimerKillCallback = std::function<void(UINT_PTR timerId)>;

    /** @brief 设置桌面快照提供者回调 */
    void SetDesktopSnapshotProvider(DesktopSnapshotProvider provider) { desktopSnapshotProvider_ = std::move(provider); }
    /** @brief 设置选中项提供者回调 */
    void SetSelectionProvider(DesktopSnapshotProvider provider) { selectionProvider_ = std::move(provider); }
    /** @brief 设置组件选中状态提供者回调 */
    void SetWidgetSelectedProvider(WidgetSelectedProvider provider) { widgetSelectedProvider_ = std::move(provider); }
    /** @brief 设置当前唯一选中组件的包 UUID 提供者 */
    void SetSelectedWidgetPackageProvider(
        SelectedWidgetPackageProvider provider)
    {
        selectedWidgetPackageProvider_ = std::move(provider);
    }
    void SetApplicationSearchProvider(ApplicationSearchProvider provider) { applicationSearchProvider_ = std::move(provider); }
    void SetApplicationCatalogProvider(ApplicationCatalogProvider provider)
    {
        applicationCatalogProvider_ = std::move(provider);
    }
    void SetApplicationIndexStatusProvider(
        ApplicationIndexStatusProvider provider)
    {
        applicationIndexStatusProvider_ = std::move(provider);
    }
    void SetApplicationLaunchCallback(DesktopPathAction callback)
    {
        applicationLaunchCallback_ = std::move(callback);
    }
    void SetEverythingSearchProvider(EverythingSearchProvider provider) { everythingSearchProvider_ = std::move(provider); }
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
    /** @brief 设置宿主绘制输入框取得键盘焦点时的回调 */
    void SetHostInputFocusCallback(HostInputFocusCallback callback) { hostInputFocusCallback_ = std::move(callback); }
    /** @brief 设置打开组件设置面板回调 */
    void SetOpenWidgetSettingsCallback(WidgetTitleCallback callback) { openWidgetSettingsCallback_ = std::move(callback); }
    void SetOpenWidgetPanelCallback(WidgetPanelOpenCallback callback) { openWidgetPanelCallback_ = std::move(callback); }
    void SetCloseWidgetPanelCallback(WidgetPanelCloseCallback callback) { closeWidgetPanelCallback_ = std::move(callback); }
    /** @brief 设置系统通知回调 */
    void SetNotifyCallback(NotifyCallback callback) { notifyCallback_ = std::move(callback); }
    /** @brief 设置组件刷新截止时间请求回调（宿主返回统一调度令牌） */
    void SetWidgetTimerRequestCallback(WidgetTimerRequestCallback callback) { widgetTimerRequestCallback_ = std::move(callback); }
    /** @brief 设置组件独立刷新定时器关闭回调 */
    void SetWidgetTimerKillCallback(WidgetTimerKillCallback callback) { widgetTimerKillCallback_ = std::move(callback); }
    /** @brief 主宿主窗口重建后，将组件刷新与命名定时器重新绑定到新 HWND。 */
    void RebindHostTimers();

    /**
     * @brief 确保小部件已加载到沙箱中
     * @param widgetId 小部件实例 ID
     * @param scriptPath Lua 脚本文件路径
     * @return 加载成功或已存在返回 true，否则返回 false
     */
    bool EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath);
    /** Load an isolated, side-effect-free instance for the component picker. */
    bool EnsureWidgetPreviewLoaded(const std::wstring& widgetId,
        const std::wstring& packageId,
        const std::unordered_map<std::string, std::string>&
            storageOverrides = {});

    /**
     * @brief 卸载指定小部件实例
     * @param widgetId 要卸载的小部件 ID
     */
    void UnloadWidget(const std::wstring& widgetId);
    void DeleteWidgetInstance(const std::wstring& widgetId);

    /**
     * @brief 重新加载指定小部件实例
     * @param widgetId 要重新加载的小部件 ID
     * @return 重载成功返回 true，否则返回 false
     */
    bool ReloadWidget(const std::wstring& widgetId);
    bool RetryWidget(const std::wstring& widgetId,
        const std::wstring& packageId);
    snowdesktop::widget_runtime::WidgetHostState GetWidgetHostState(
        const std::wstring& widgetId,
        const std::wstring& packageId) const;
    void NotifyLanguageChanged(const std::wstring& widgetId);

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
    void RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath,
        ID2D1DeviceContext* context, RECT bounds, int columns = 1, int rows = 1);
    bool RenderWidgetPanel(const std::wstring& widgetId,
        ID2D1DeviceContext* context, RECT bounds);
    void TickRuntime();
    /**
     * @brief 处理宿主转发的组件调度截止时间到期
     * @param widgetId 触发刷新的小部件实例 ID
     * @param timerId 宿主触发的调度令牌
     *
     * 同时处理 manifest.refreshIntervalMs 的周期刷新和 widget.setTimer 命名定时器。
     * 命名定时器只为最近一次到期时间申请单次宿主唤醒，避免全局轮询全部组件。
     */
    void OnWidgetTimer(const std::wstring& widgetId, UINT_PTR timerId);

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
    void InvokeOpen(const std::wstring& widgetId,
        bool trustedGesture = false);

    /**
     * @brief 触发小部件被选中的回调
     * @param widgetId 小部件实例 ID
     */
    void InvokeSelected(const std::wstring& widgetId);

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
    std::vector<LuaWidgetMenuItem> GetContextMenu(
        const std::wstring& widgetId, int x = -1, int y = -1);

    /**
     * @brief 触发小部件的菜单项点击回调
     * @param widgetId 小部件实例 ID
     * @param menuId 菜单项标识符
     */
    void InvokeMenu(const std::wstring& widgetId, int menuId);
    void InvokeMenu(const std::wstring& widgetId,
        const LuaWidgetMenuItem& menuItem);

    /**
     * @brief 通知引擎桌面内容已变更
     * @param reason 变更原因描述字符串
     */
    void NotifyDesktopChanged(const std::string& reason);
    void NotifyCalendarChanged(const std::string& reason);

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
     * @param borderAlpha 输出：边框透明度
     * @param gradientEndA 输出：渐变末端透明度
     * @param glassEnabled 输出：毛玻璃背景开关
     * @param acrylicEnabled 输出：亚克力颗粒开关
     * @return 成功读取返回 true
     */
    bool ReadCustomColors(const std::wstring& widgetId,
        float& bgR, float& bgG, float& bgB, float& alpha,
        float& borderR, float& borderG, float& borderB, float& borderAlpha,
        float& gradientEndA, bool& glassEnabled, bool& acrylicEnabled) const;

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
    std::string GetSystemSnapshotError() const;

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
    /** @brief 判断标题是否为清单中任一语言的默认组件名。 */
    static bool IsWidgetDefaultName(const std::wstring& filename,
        const std::wstring& title);

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
    static bool InstallWidgetPackage(const std::wstring& manifestPath,
        std::wstring& error, bool allowSourceChange = false,
        bool allowPermissionExpansion = false,
        snowdesktop::widget::InstalledPackage* installed = nullptr);
    bool InstallAndVerifyWidgetPackage(const std::wstring& path,
        std::wstring& error, bool allowSourceChange = false,
        bool allowPermissionExpansion = false);
    static snowdesktop::widget::ProviderStatus GetStaticWidgetCatalogStatus(
        const std::filesystem::path& catalogPath);
    static void RegisterWidgetPackageSource(
        std::shared_ptr<snowdesktop::widget::IWidgetPackageSource> source);
    static bool ConfigureStaticWidgetCatalog(
        const std::filesystem::path& catalogPath, std::string& error);
    static std::vector<snowdesktop::widget::PackageSourceInfo>
        ListWidgetPackageSources();
    static std::vector<snowdesktop::widget::PackageDetails>
        QueryWidgetPackageSource(const std::string& providerId,
            const snowdesktop::widget::PackageQuery& query,
            std::string& error);
    static bool IsSteamWorkshopBridgeAvailable();
    static bool UnsubscribeSteamWorkshopItem(
        const std::string& externalItemId, std::string& error);
    static snowdesktop::widget::SteamWorkshopSubscriptionSnapshot
        QuerySteamWorkshopSubscriptions(const std::string& locale);
    static snowdesktop::widget::SteamWorkshopSubscriptionHistory
        GetSteamWorkshopSubscriptionHistory();
    static void PrepareSteamWorkshopSubscriptionArtifacts(
        snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
        const std::vector<snowdesktop::widget::InstalledPackage>& installed,
        const std::filesystem::path& stagingRoot);
    static std::unordered_map<std::string, std::string>
        CachedSteamWorkshopPackageAssociations();
    static std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
        CachedSteamWorkshopInstallFailures();
    snowdesktop::widget::SteamWorkshopSyncResult
        ApplySteamWorkshopSubscriptions(
            const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot&
                snapshot);
    bool InstallAndVerifyWidgetPackageFromSource(
        const std::string& providerId, const std::string& externalItemId,
        const std::string& version, std::wstring& error,
        bool allowSourceChange = false,
        bool allowPermissionExpansion = false);
    int ApplySafeWidgetPackageUpdates(const std::string& providerId,
        std::string& report);
    static std::vector<snowdesktop::widget::PackageDetails>
        QueryStaticWidgetCatalog(const std::filesystem::path& catalogPath,
            const snowdesktop::widget::PackageQuery& query,
            std::string& error);
    bool InstallAndVerifyStaticWidgetPackage(
        const std::filesystem::path& catalogPath,
        const std::string& externalItemId, const std::string& version,
        std::wstring& error, bool allowSourceChange = false,
        bool allowPermissionExpansion = false);
    static snowdesktop::widget::PackagePaths GetWidgetPackagePaths();
    static std::vector<snowdesktop::widget::InstalledPackage>
        ListWidgetPackages();
    static std::vector<snowdesktop::widget::InvalidPackage>
        ListInvalidWidgetPackages();
    static std::optional<snowdesktop::widget::InstalledPackage>
        GetWidgetPackage(const std::wstring& packageId);
    static bool SetWidgetPermissionDecision(
        const std::wstring& packageId,
        snowdesktop::widget::PermissionDecisionState state,
        const std::vector<std::string>& grantedPermissions,
        const std::vector<std::string>& grantedNetworkDomains,
        std::string& error);
    bool ApplyWidgetPermissionDecision(
        const std::wstring& packageId,
        snowdesktop::widget::PermissionDecisionState state,
        const std::vector<std::string>& grantedPermissions,
        const std::vector<std::string>& grantedNetworkDomains,
        std::string& error);
    static std::optional<snowdesktop::widget::PackageSourceRef>
        GetWidgetPackageSource(const std::wstring& packageId);
    static bool IsWidgetPackageAvailable(const std::wstring& packageId);
    static bool IsWidgetPackageInstalled(const std::wstring& packageId);
    static std::vector<snowdesktop::widget::LegacyPackage>
        ListLegacyWidgetPackages();
    static std::optional<std::wstring> ResolveLegacyWidgetPackage(
        const std::wstring& legacyName);
    static snowdesktop::widget::LegacyMigrationResult MigrateLegacyWidgetPackage(
        const snowdesktop::widget::LegacyPackage& legacy);
    static bool SetWidgetPackageEnabled(const std::string& packageId,
        bool enabled, std::string& error);
    static bool CreateWidgetDevelopmentProject(const std::string& packageId,
        std::filesystem::path& projectRoot, std::string& error);
    static bool SetWidgetDevelopmentOverride(const std::string& packageId,
        bool active, std::string& error);
    static bool RollbackWidgetPackage(const std::string& packageId,
        const std::string& version, std::string& error);
    static bool UninstallWidgetPackage(const std::string& packageId,
        std::string& error);

    /**
     * @brief 渲染指定小部件的 ImGui 编辑器界面
     * @param widgetId 小部件实例 ID
     * @param widgetName 小部件名称
     * @param mainPersonalization 当前宿主个性化设置；原生毛玻璃控件直接修改其中参数
     * @param sharedGlassSettingsChanged 输出：共享模糊半径已变化
     * @param sharedGlassSettingsSaveRequested 输出：模糊半径需要持久化
     * @return 渲染成功返回 true
     */
    bool RenderWidgetEditor(const std::wstring& widgetId, const std::wstring& widgetName,
        PersonalizationSettings& mainPersonalization,
        bool& sharedGlassSettingsChanged,
        bool& sharedGlassSettingsSaveRequested);

    /**
     * @brief 检查小部件是否拥有指定运行时权限
     * @param widgetId 小部件实例 ID
     * @param permission 权限名称
     * @return 拥有权限返回 true，否则返回 false
     */
    bool RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const;
    void ActivateWidgetState(const std::wstring& widgetId);

    /**
     * @brief 记录运行时错误
     * @param widgetId 小部件实例 ID
     * @param message 错误消息
     */
    void RuntimeRecordError(const std::wstring& widgetId, const std::string& message);

    snowdesktop::widget_runtime::DataSubscriptionResult
        RuntimeSubscribeData(
            const std::wstring& widgetId, std::string topic,
            std::chrono::milliseconds maxAge,
            snowdesktop::widget_runtime::DataHiddenPolicy whenHidden,
            std::string rangeStart = {}, std::string rangeEnd = {});
    bool RuntimeUnsubscribeData(std::uint64_t subscriptionId);
    std::optional<LuaWidgetDataSnapshot> RuntimeGetDataSnapshot(
        std::uint64_t subscriptionId) const;

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
    std::vector<LuaDesktopItemInfo> RuntimeApplicationSearch(const std::string& query, int maxResults) const;
    std::vector<LuaDesktopItemInfo> RuntimeEverythingSearch(const std::string& query, int maxResults) const;
    std::string RuntimeCalendarSelectedDate() const;
    bool RuntimeCalendarSetSelectedDate(
        const std::string& date);
    std::optional<snowdesktop::calendar::DateInfo>
        RuntimeCalendarDateInfo(
            const std::string& date) const;
    std::optional<std::string> RuntimeCalendarAddDays(
        const std::string& date, int offset) const;
    std::vector<snowdesktop::calendar::CalendarEvent>
        RuntimeCalendarEvents(
            const std::string& fromDate,
            const std::string& toDate) const;
    snowdesktop::calendar::MutationResult
        RuntimeCalendarCreate(
            snowdesktop::calendar::CalendarEvent event);
    snowdesktop::calendar::MutationResult
        RuntimeCalendarUpdate(
            const std::string& id,
            int expectedRevision,
            snowdesktop::calendar::CalendarEvent event);
    snowdesktop::calendar::MutationResult
        RuntimeCalendarRemove(const std::string& id);

    /**
     * @brief 通过宿主打开指定路径
     * @param path 要打开的路径
     * @return 宿主接受打开请求时返回 true
     */
    bool RuntimeOpenDesktopPath(const std::wstring& path);

    /**
     * @brief 通过宿主在文件管理器中定位指定路径
     * @param path 要揭示的路径
     * @return 操作成功返回 true
     */
    bool RuntimeRevealDesktopPath(const std::wstring& path);
    std::optional<std::wstring> RuntimeResolvePackageAsset(
        const std::wstring& widgetId, const std::wstring& relativePath) const;

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
    void RuntimeInvalidateHost(const std::wstring& widgetId = {});
    bool RuntimeSubmitInteractionRegion(const std::wstring& widgetId,
        snowdesktop::widget_runtime::InteractionRegion region,
        std::string& error);
    bool RuntimeInteractionHovered(const std::wstring& widgetId,
        std::string_view key) const;
    bool RuntimeInteractionPressed(const std::wstring& widgetId,
        std::string_view key) const;
    void UpdateInteractionHover(const std::wstring& widgetId, int x, int y);
    void ClearInteractionHover();
    std::string InteractionCursorAt(const std::wstring& widgetId,
        int x, int y) const;

    void ReloadStorage();

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

    /** Store host layout metrics for one widget without mutating an active
     * callback belonging to another widget. The metrics are applied when the
     * target widget's execution context is entered.
     */
    void SetWidgetLayoutMetrics(const std::wstring& widgetId,
        int cellWidth, int cellHeight, int gapY, int barHeight,
        DWRITE_FONT_WEIGHT fontWeight);
    void SetWidgetSurfaceContext(const std::wstring& widgetId,
        const LuaWidgetSurfaceContext& context);
    LuaWidgetContextState RuntimeGetWidgetContextState(
        const std::wstring& widgetId) const;
    void RuntimeOpenWidgetSettings(const std::wstring& widgetId);
    void RuntimeOpenWidgetPanel(const std::wstring& widgetId,
        std::wstring title, int width, int height);
    void RuntimeCloseWidgetPanel(const std::wstring& widgetId);

    /**
     * @brief 发送系统通知
     * @param title 通知标题
     * @param message 通知内容
     */
    void RuntimeNotify(const std::wstring& widgetId,
        const std::wstring& title, const std::wstring& message);
    std::string RuntimePostNotification(const std::wstring& widgetId,
        const std::wstring& title, const std::wstring& message);
    CpuSnapshot RuntimeGetCpuSnapshot(const std::wstring& widgetId);
    MemorySnapshot RuntimeGetMemorySnapshot(const std::wstring& widgetId);
    BatterySnapshot RuntimeGetBatterySnapshot(const std::wstring& widgetId);
    NetworkSnapshot RuntimeGetNetworkSnapshot(const std::wstring& widgetId);
    GpuSnapshot RuntimeGetGpuSnapshot(const std::wstring& widgetId);
    MediaSnapshot RuntimeGetMediaSnapshot(const std::wstring& widgetId);
    bool RuntimeMediaPlayPause();
    bool RuntimeMediaNext();
    bool RuntimeMediaPrevious();
    snowdesktop::widget_runtime::TaskStartResult RuntimeStartTask(
        const std::wstring& widgetId, std::uint64_t ownerToken,
        std::string name,
        std::unordered_map<std::string, std::string> arguments = {});
    bool RuntimeCancelTask(
        const std::wstring& widgetId, std::uint64_t ownerToken,
        std::uint64_t taskId);
    bool RuntimeSetTimer(const std::wstring& widgetId,
        const std::string& name, int intervalMs, bool repeat,
        snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy =
            snowdesktop::widget_runtime::ScheduleHiddenPolicy::Continue);
    bool RuntimeCancelTimer(const std::wstring& widgetId, const std::string& name);
    int RuntimeHttpRequest(const std::wstring& widgetId, HttpRequestOptions options);
    bool RuntimeHttpCancel(const std::wstring& widgetId, int requestId);
    void RuntimeRegisterHostControl(const std::wstring& widgetId, LuaWidget::HostControl control);
    bool RuntimeFocusHostInput(const std::wstring& widgetId, const std::string& id);
    bool RuntimeGetFocusedHostInput(const std::wstring& widgetId, const std::string& id,
        std::wstring& text, size_t& cursor, size_t& selectionAnchor,
        std::wstring& compositionText, size_t& compositionCursor) const;
    bool RuntimeIsWidgetSelected(const std::wstring& widgetId) const;
    std::wstring RuntimeSelectedWidgetPackageId() const;
    bool HandleHostInputKey(WPARAM key);
    bool HandleHostInputChar(wchar_t ch);
    bool SetHostInputComposition(
        const std::wstring& text, size_t cursor);
    bool CommitHostInputComposition(const std::wstring& text);
    void ClearHostInputComposition();
    bool HasFocusedHostInput() const;
    bool GetFocusedHostInputCaretRect(RECT& rect) const;
    bool IsHostInputAt(const std::wstring& widgetId, int x, int y) const;
    bool IsFocusedHostInputAt(const std::wstring& widgetId, int x, int y) const;
    bool HandleHostInputPointerMove(const std::wstring& widgetId, int x, int y);
    bool HandleHostInputPointerUp(const std::wstring& widgetId, int x, int y);
    void BlurHostInput(bool cancel = false);
    int RuntimeGetScrollOffset(const std::wstring& widgetId, const std::string& id) const;
    void RuntimeSetScrollOffset(const std::wstring& widgetId,
        const std::string& id, int offset);
    bool HandleHostUiPointer(const std::wstring& widgetId, int x, int y, int delta, bool wheel);
    std::vector<LuaWidget::HostControl> GetScrollControls(const std::wstring& widgetId) const;

private:
    bool VerifyInstalledWidgetPackage(const std::string& packageId,
        const std::optional<std::string>& previousVersion,
        std::wstring& error);
    size_t HitTestHostInputPosition(const LuaWidget::HostControl& control,
        const std::wstring& widgetId, int x, int y) const;

    /**
     * @brief 内部加载小部件脚本到沙箱
     * @param path Lua 脚本文件路径
     * @param widgetId 小部件实例 ID
     * @return 加载成功返回 true
     */
    bool LoadWidget(const std::wstring& path, const std::wstring& widgetId,
        bool preview = false,
        const std::unordered_map<std::string, std::string>*
            previewStorageOverrides = nullptr);
    bool IsPreviewWidget(const std::wstring& widgetId) const;

    /**
     * @brief 向 Lua 状态机注册绘制 API
     * @param L Lua 状态机指针
     */
    void RegisterDrawAPI(lua_State* L, int apiVersion);

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
    void RecordWidgetHostFailure(const std::wstring& widgetId,
        const std::string& message, bool quotaExceeded = false,
        bool circuitOpen = false);
    void InvokeSimpleCallback(LuaWidget& widget, const char* callbackName);
    bool InitializeWidgetLifecycle(LuaWidget& widget);
    bool InvokeLifecycleEvent(LuaWidget& widget, const char* kind,
        const std::function<void(lua_State*)>& pushFields);
    void DispatchInteractionAction(LuaWidget& widget,
        const std::string& targetKey, const char* eventName,
        int x, int y, int button, int delta, int clickCount = 0,
        bool includeRetired = false);
    void DispatchInteractionTransition(LuaWidget& widget,
        const snowdesktop::widget_runtime::InteractionHoverTransition& transition,
        int x, int y);
    void DisposeWidgetLifecycle(LuaWidget& widget, const char* reason);
    void InitializeWidgetDataBroker();
    void ApplyWidgetDataBrokerActions();
    void ReleaseWidgetDataSubscriptions(LuaWidget& widget);
    void InitializeWidgetTaskBroker();
    void ApplyWidgetTaskBrokerActions();
    void ReleaseWidgetTasks(LuaWidget& widget,
        snowdesktop::widget_runtime::TaskBrokerCancelReason reason);
    void EnsureSystemSnapshotServiceStarted();
    void RescheduleNamedTimer(LuaWidget& widget);

    D2DState* d2dState_ = nullptr;                     ///< Direct2D 渲染状态管理对象指针
    ComPtr<ID2D1DeviceContext> d2dContext_;            ///< Direct2D 设备上下文
    ComPtr<IDWriteFactory> dwriteFactory_;             ///< DirectWrite 工厂接口
    std::vector<LuaWidget> widgets_;                   ///< 已加载的小部件实例列表
    std::unordered_map<std::wstring,
        snowdesktop::widget_runtime::WidgetHostState>
        widgetHostFailures_;
    DesktopSnapshotProvider desktopSnapshotProvider_;  ///< 桌面快照提供者回调
    DesktopSnapshotProvider selectionProvider_;        ///< 当前选中项提供者回调
    WidgetSelectedProvider widgetSelectedProvider_;    ///< 当前组件选中状态提供者回调
    SelectedWidgetPackageProvider
        selectedWidgetPackageProvider_; ///< 当前唯一选中组件包 UUID
    ApplicationSearchProvider applicationSearchProvider_; ///< Windows 应用搜索提供者回调
    ApplicationCatalogProvider applicationCatalogProvider_;
    ApplicationIndexStatusProvider applicationIndexStatusProvider_;
    EverythingSearchProvider everythingSearchProvider_; ///< Everything 搜索提供者回调
    WidgetTitleCallback setWidgetTitleCallback_;       ///< 设置小部件标题的回调
    WidgetTitleCallback openWidgetSettingsCallback_;   ///< 打开小部件设置面板的回调
    WidgetPanelOpenCallback openWidgetPanelCallback_;
    WidgetPanelCloseCallback closeWidgetPanelCallback_;
    InvalidateCallback invalidateCallback_;            ///< 请求宿主重绘的回调
    DesktopPathAction desktopOpenCallback_;            ///< 打开桌面路径的回调
    DesktopPathAction applicationLaunchCallback_;      ///< 启动已解析应用引用的回调
    DesktopPathAction desktopRevealCallback_;          ///< 在资源管理器中定位路径的回调
    DesktopRefreshCallback desktopRefreshCallback_;    ///< 刷新桌面的回调
    InlineTextEditCallback inlineTextEditCallback_;    ///< 内联文本编辑请求的回调
    HostInputFocusCallback hostInputFocusCallback_;    ///< 让隐藏桌面输入窗口取得键盘焦点的回调
    NotifyCallback notifyCallback_;                     ///< 系统通知回调
    WidgetTimerRequestCallback widgetTimerRequestCallback_; ///< 请求宿主为 widget 开独立 timer
    WidgetTimerKillCallback widgetTimerKillCallback_;   ///< 请求宿主关闭 widget 独立 timer
    std::unique_ptr<SystemSnapshotService> systemSnapshotService_;
    std::unique_ptr<snowdesktop::widget_runtime::WidgetDataBroker>
        dataBroker_;
    std::unique_ptr<snowdesktop::widget_runtime::WidgetTaskBroker>
        taskBroker_;
    std::unique_ptr<
        snowdesktop::widget_runtime::WidgetMediaTaskExecutor>
        mediaTaskExecutor_;
    std::unique_ptr<
        snowdesktop::widget_runtime::WidgetAppTaskExecutor>
        appTaskExecutor_;
    std::unordered_map<std::uint64_t,
        snowdesktop::widget_runtime::WidgetAppSearchCompletion>
        appSearchCompletions_;
    snowdesktop::widget_runtime::WidgetTrustedGestureState
        trustedGestureState_;
    std::uint64_t nextWidgetRuntimeToken_ = 0;
    std::unique_ptr<snowdesktop::widget_runtime::WidgetSystemDataProvider>
        widgetSystemDataProvider_;
    std::unique_ptr<snowdesktop::widget_runtime::WidgetAudioAnalysisProvider>
        widgetAudioAnalysisProvider_;
    std::uint64_t desktopDataRevision_ = 0;
    std::int64_t desktopDataTimestampMs_ = 0;
    std::string desktopDataChangeReason_ = "initial";
    std::uint64_t calendarEventsRevision_ = 0;
    std::uint64_t calendarSelectionRevision_ = 0;
    std::uint64_t appIndexRevision_ = 0;
    std::unique_ptr<
        snowdesktop::calendar::CalendarService>
        calendarService_;
    bool pendingCalendarSelectionChange_ = false;
    bool pendingCalendarEventsChange_ = false;
    bool systemSnapshotServiceStarted_ = false;
    std::atomic<bool> systemSnapshotChanged_{ false };
    std::atomic<bool> mediaSnapshotChanged_{ false };
    std::unique_ptr<AsyncHttpService> httpService_;
    bool previewOnly_ = false;
    struct FocusedHostInput
    {
        bool active = false;
        std::wstring widgetId;
        std::string id;
        std::string storageKey;
        std::wstring text;
        std::wstring originalText;
        size_t cursor = 0;
        size_t selectionAnchor = 0;
        std::wstring compositionText;
        size_t compositionCursor = 0;
        bool pointerSelecting = false;
        bool liveUpdate = true;
        bool multiline = false;
    };
    FocusedHostInput focusedHostInput_;
};
