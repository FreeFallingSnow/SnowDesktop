#pragma once

#include "lua_runtime.h"
#include "widget_layout_context.h"
#include "widget_logical_slot.h"
#include "widget_lua_lifecycle.h"
#include "widget_runtime_health.h"
#include "widget_runtime_scheduler.h"
#include "widget/packages/widget_script_manifest.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/storage/widget_storage_write_budget.h"
#include "widget/view/widget_interaction_region.h"
#include "widget/view/widget_native_marquee.h"
#include "widget/view/widget_surface_context.h"
#include "widget/view/widget_surface_theme.h"
#include "widget/view/widget_text_input_rules.h"
#include "widget/view/widget_view_tree.h"

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

extern "C" {
#include <lauxlib.h>
}

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        std::string title;
        std::string source;
        std::string type;
        bool persistent = false;
        bool available = true;
    };

    struct ItemReference
    {
        std::string target;
        std::string sourceTask;
        std::uint64_t revision = 0;
        std::string title;
        std::string source;
        std::string type;
        std::string referenceKind = "filesystem.reference";
        bool persistent = false;
        bool available = true;
    };

    struct HostControl
    {
        enum class Type { Button, Toggle, Input, Scroll };
        Type type = Type::Button;
        std::string id;
        std::string surface = "desktop";
        std::string storageKey;
        RECT rect{};
        std::optional<RECT> clipRect;
        bool value = false;
        bool enabled = true;
        bool focusable = true;
        bool readOnly = false;
        bool controlled = false;
        bool numeric = false;
        bool selectAll = true;
        std::optional<snowdesktop::widget_runtime::ViewTextSelection>
            selection;
        bool liveUpdate = false;
        bool multiline = false;
        std::string controlledText;
        std::string placeholder;
        snowdesktop::widget_runtime::InteractionAction changeAction;
        snowdesktop::widget_runtime::InteractionAction
            selectionChangeAction;
        snowdesktop::widget_runtime::InteractionAction focusAction;
        snowdesktop::widget_runtime::InteractionAction blurAction;
        snowdesktop::widget_runtime::InteractionAction submitAction;
        float minimum = 0.0f;
        float maximum = 1.0f;
        float step = 0.01f;
        float fontSize = 15.0f;
        snowdesktop::widget_runtime::ViewEdgeInsets padding{ 8.0f };
        int contentHeight = 0;
        int viewportHeight = 0;
        int contentWidth = 0;
        int viewportWidth = 0;
        bool horizontal = false;
        bool selectPopup = false;
        int initialScrollOffset = 0;
        std::size_t maximumUtf8Bytes = 0;
    };

    struct VariableVirtualMeasurement
    {
        float extent = 0.0f;
        std::uint64_t lastUsed = 0;
        std::string itemKey;
    };

    struct VariableVirtualState
    {
        std::size_t itemCount = 0;
        float estimatedItemSize = 0.0f;
        float mainGap = 0.0f;
        bool horizontal = false;
        std::uint64_t layoutRevision = 0;
        std::uint64_t sequence = 0;
        std::unordered_map<std::size_t, VariableVirtualMeasurement>
            measurements;
    };

    struct LogicalSlotPointerDrag
    {
        std::string sourceSlotId;
        std::string targetSlotId;
        std::string itemId;
        std::size_t sourceIndex = 0;
        std::size_t targetIndex = 0;
        POINT start{};
        RECT sourceBounds{};
        RECT indicatorBounds{};
        bool moved = false;
        std::uint64_t targetRevision = 0;
    };

    struct LogicalSlotFocus
    {
        std::string slotId;
        std::string itemId;
    };

    using NativeMarqueeText = snowdesktop::widget_runtime::NativeMarqueeText;

    struct NativeMarqueeSurface
    {
        Microsoft::WRL::ComPtr<ID2D1Device> commandDevice;
        Microsoft::WRL::ComPtr<ID2D1CommandList> staticCommands;
        std::vector<NativeMarqueeText> marquees;
        std::vector<NativeMarqueeText> pendingMarquees;
        std::chrono::steady_clock::time_point lastAdvance{};
        bool collecting = false;
        bool framePending = false;
        bool requiresLuaRender = true;
        bool compositionManaged = false;
    };
    std::wstring widgetId;               ///< 小部件实例唯一 ID
    std::string packageId;                ///< 组件包 UUID
    std::filesystem::path packageRoot;    ///< 已校验组件包根目录
    lua_State* state = nullptr;           ///< Per-instance Lua VM
    std::unique_ptr<LuaRuntimeQuota> quota; ///< VM memory/instruction accounting
    std::unique_ptr<snowdesktop::widget_runtime::WidgetStorageWriteBudget>
        storageWriteBudget;                ///< Persistent commit rate accounting
    std::string name;                    ///< 小部件名称
    std::wstring filePath;               ///< Lua 脚本文件的完整路径
    LuaWidgetManifest manifest;          ///< 从清单文件解析的元数据
    std::unordered_map<std::string, std::string>
        packageImageContentKeys;         ///< Resource name to decoded image content digest
    std::unordered_set<std::string> permissions; ///< 已授予的权限集合
    int ref = LUA_NOREF;                 ///< Lua 注册表引用，LUA_NOREF 表示无效
    bool valid = false;                  ///< 是否已成功加载且可执行
    bool customStyle = false;            ///< 是否启用了自定义主题样式
    bool followPersonalizationDefault = false; ///< 尚未保存外观状态时是否默认跟随全局
    bool hasBackgroundLayer = false;     ///< 是否声明桌面装饰背景层
    LuaWidgetTheme theme;                ///< 自定义主题配置（当 customStyle 为 true 时生效）
    LuaWidgetPreviewDataState previewDataState =
        LuaWidgetPreviewDataState::Ready; ///< 作者预览的确定性数据状态
    LuaWidgetSurfaceContext surfaceContext; ///< 当前显示器、工作区与 DPI 摘要
    std::vector<LuaWidgetManifest::Setting> scriptSettings; ///< Lua 顶层声明式设置
    std::vector<LuaWidgetManifest::SettingGroup> scriptSettingGroups; ///< Lua 顶层设置分组
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets; ///< Lua 顶层声明式预设
    FILETIME lastModified = {};          ///< 脚本文件最后修改时间，用于变更检测
    RECT lastBounds{};                   ///< 最后一次渲染时的边界矩形
    RECT panelBounds{};                  ///< 最后一次面板渲染时的边界矩形
    int lastColumns = 1;
    int lastRows = 1;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics;
    bool desktopVisible = false;
    bool keepRuntimeActiveForHiddenPage = false;
    bool desktopOccluded = false;
    bool desktopOcclusionResumePending = false;
    bool hostVisible = false;
    bool usesSystemSnapshot = false;
    bool usesMediaSnapshot = false;
    double lastCallbackMs = 0.0;
    snowdesktop::widget_runtime::RuntimeHealth health;
    std::chrono::steady_clock::time_point notificationWindow{};
    std::uint32_t notificationsInWindow = 0;
    UINT_PTR refreshTimerId = 0;        ///< 宿主统一截止时间队列分配的周期令牌（0 = 未开）
    UINT_PTR namedTimerId = 0;          ///< v2 schedule 命名计划共用的下一次唤醒令牌
    UINT_PTR animationTimerId = 0;      ///< animation.requestFrame 共用的单次下一帧令牌
    snowdesktop::widget_runtime::NamedTimerSchedule namedTimers;
    snowdesktop::widget_runtime::AnimationFrameRequests animationFrames;
    std::vector<HostControl> hostControls;
    snowdesktop::widget_runtime::DeferredHostInputFocus
        deferredHostInputFocus;
    std::unordered_map<std::string, int> scrollOffsets;
    std::unordered_map<std::string, int> panelScrollOffsets;
    std::unordered_map<std::string, VariableVirtualState>
        variableVirtualStates;
    std::unordered_map<std::string, VariableVirtualState>
        panelVariableVirtualStates;
    std::unordered_map<std::uint64_t, std::string> dataSubscriptions;
    std::unordered_set<std::uint64_t> taskIds;
    std::unordered_map<std::string, ApplicationReference>
        applicationReferences;
    std::unordered_map<std::string, ItemReference> itemReferences;
    snowdesktop::widget_runtime::LogicalSlotModel logicalSlots;
    snowdesktop::widget_runtime::LogicalSlotHistory logicalSlotHistory;
    std::optional<LogicalSlotPointerDrag> logicalSlotPointerDrag;
    std::optional<LogicalSlotFocus> logicalSlotFocus;
    std::string viewKeyboardFocusKey;
    bool viewFocusCueVisible = false;
    snowdesktop::widget_runtime::WidgetInteractionRegions interactionRegions;
    std::optional<snowdesktop::widget_runtime::ViewNode> viewTree;
    std::uint64_t desktopPaintRevision = 0;
    snowdesktop::widget_runtime::ViewLayoutCache viewLayoutCache;
    snowdesktop::widget_runtime::ViewTransitionRuntime viewTransitions;
    bool viewTransitionFramePending = false;
    bool viewIndeterminateProgressActive = false;
    NativeMarqueeSurface desktopMarquee;
    std::string panelViewKeyboardFocusKey;
    bool panelViewFocusCueVisible = false;
    snowdesktop::widget_runtime::WidgetInteractionRegions
        panelInteractionRegions;
    std::optional<snowdesktop::widget_runtime::ViewNode> panelViewTree;
    snowdesktop::widget_runtime::ViewLayoutCache panelViewLayoutCache;
    snowdesktop::widget_runtime::ViewTransitionRuntime panelViewTransitions;
    bool panelViewTransitionFramePending = false;
    bool panelIndeterminateProgressActive = false;
    NativeMarqueeSurface panelMarquee;
    std::string panelSurface = "panel";
    bool panelActive = false;
    bool panelInitialKeyboardFocusPending = false;
    bool panelFrameOpen = false;
    std::string backgroundLayerError;
    std::uint64_t runtimeToken = 0;
    bool preview = false;
    std::unordered_map<std::string, std::string> previewStorage;
    snowdesktop::widget_runtime::WidgetLuaLifecycle lifecycle;
};
