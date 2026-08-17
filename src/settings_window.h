/**
 * @file settings_window.h
 * @brief 设置窗口模块
 *
 * 基于 ImGui 和 Direct3D 11 实现的设置 UI 窗口。
 * 提供多页面设置界面，包含通用设置、个性化、小组件编辑、调试和关于页面。
 * 管理布局备份（layout backup）的保存、恢复、删除和列举。
 * 拥有独立的 DXGI SwapChain，用于在单独的 HWND 中渲染 ImGui 界面。
 */

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "general_settings.h"
#include "personalization.h"
#include "dock_settings.h"
#include "navigation_settings.h"
#include "category_settings.h"
#include "full_data_backup.h"
#include "widget_package.h"
#include "authoring_toolchain.h"
#include "../layout_spacing_rules.h"
#include "icon_beautify.h"
#include "constants.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ImFont;
struct ImGuiContext;
class AsyncHttpService;

/**
 * @brief 布局备份条目结构体
 *
 * 描述一个已保存的桌面布局备份文件。
 * 每个备份包含一个唯一的文件名、用户可读的显示名称以及创建时间戳。
 */
struct LayoutBackup
{
    std::wstring filename;
    std::wstring displayName;
    FILETIME timestamp;
};

/** @brief 设置页中可配置的全局快捷键用途。 */
enum class HotkeySettingTarget
{
    None,
    QuickNavigation,
    DesktopPassthrough,
    FloatingDock,
};

/**
 * @brief 基于 ImGui 的设置窗口类
 *
 * 管理一个独立的 Win32 窗口 (HWND)，通过 DXGI SwapChain 将 ImGui 渲染到该窗口。
 * 主要功能：
 *   - 多页面设置界面：通用、个性化、小组件编辑、调试、关于
 *   - 布局备份管理：将当前桌面布局保存为备份文件，支持列举、恢复和删除
 *   - 开机自启管理
 *   - 个性化设置（PersonalizationSettings）与导航设置（NavigationSettings）的编辑
 *   - 小组件脚本编辑器入口
 *
 * 该窗口与主渲染线程共享 ID3D11Device，但拥有独立的 SwapChain 和 RenderTargetView。
 */
class SettingsWindow
{
public:
    /// @brief 默认构造函数
    SettingsWindow() = default;

    /// @brief 析构函数，清理窗口、SwapChain 及 ImGui 资源
    ~SettingsWindow();

    // 禁用拷贝和赋值
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /** @name 初始化与关闭
     *  @{ */

    /**
     * @brief 初始化设置窗口
     * @param instance 应用程序实例句柄 (HINSTANCE)，用于注册窗口类
     * @param device   共享的 D3D11 设备指针，用于创建 SwapChain
     * @return 初始化成功返回 true，失败返回 false
     *
     * 创建隐藏的 Win32 窗口、DXGI SwapChain、ImGui 上下文并设置字体。
     */
    bool Init(HINSTANCE instance, ID3D11Device* device);

    /**
     * @brief 关闭设置窗口，释放所有资源
     *
     * 销毁 ImGui 上下文、RenderTargetView、SwapChain 以及窗口句柄。
     */
    void Shutdown();

    /** @} */
    /** @name 窗口生命周期
     *  @{ */

    /**
     * @brief 显示设置窗口（将隐藏窗口设为可见并置前）
     * @return 窗口已经完成初始化并变为可见时返回 true
     */
    bool Show();
    void ApplyLanguageChange();

    /** @brief 显示设置窗口并直接切换到 Dock 页面。 */
    bool ShowDockSettings();
    /** @brief 显示设置窗口并直接切换到外观页面。 */
    bool ShowAppearanceSettings();
    bool ShowWidgetMigration();

    /**
     * @brief 检查窗口当前是否可见
     * @return 窗口已创建且可见时返回 true
     */
    bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

    /** @brief 是否正在等待用户按下新的快捷键组合。 */
    bool IsHotkeyCaptureActive() const
    { return hotkeyCaptureTarget_ != HotkeySettingTarget::None; }
    /**
     * @brief 将已由 RegisterHotKey 截获的组合交给当前录制框。
     */
    void CaptureRegisteredHotkey(
        UINT modifiers, UINT virtualKey);

    /** @brief 设置窗口是否有尚未绘制的界面变化。 */
    bool NeedsRender() const { return renderRequested_; }

    /**
     * @brief 渲染一帧 ImGui 界面
     *
     * 绘制侧边栏导航和当前活动页面，完成后 Present SwapChain。
     */
    void Render();

    /** @} */

    /** @name 回调注册
     *  @{ */

    /**
     * @brief 设置布局重载回调
     * @param callback 无参回调，在备份恢复或布局刷新时触发
     */
    void SetReloadCallback(std::function<void()> callback) { reloadCallback_ = std::move(callback); }

    /**
     * @brief 设置退出应用回调
     * @param callback 无参回调，在用户点击退出时触发
     */
    void SetExitCallback(std::function<void()> callback) { exitCallback_ = std::move(callback); }

    /**
     * @brief 设置重启应用回调
     * @param callback 无参回调，在完整数据迁移后触发
     */
    void SetRestartCallback(std::function<void()> callback)
    { restartCallback_ = std::move(callback); }

    /**
     * @brief 设置失效回调（通知主窗口使缓存失效）
     * @param callback 无参回调，在需要刷新缓存时触发
     */
    void SetInvalidateCallback(std::function<void()> callback) { invalidateCallback_ = std::move(callback); }
    /**
     * @brief 设置导航设置变更回调
     * @param callback 无参回调，在导航设置被修改后触发
     */
    void SetNavigationSettingsChangedCallback(std::function<void()> callback) { navigationSettingsChangedCallback_ = std::move(callback); }

    void SetGeneralSettingsChangedCallback(std::function<void()> callback) { generalSettingsChangedCallback_ = std::move(callback); }

    void SetLanguageChangedCallback(std::function<void()> callback) { languageChangedCallback_ = std::move(callback); }

    void SetDockEnabledChangedCallback(std::function<void(bool)> callback)
    { dockEnabledChangedCallback_ = std::move(callback); }

    void SetDockSettingsChangedCallback(std::function<void()> callback)
    { dockSettingsChangedCallback_ = std::move(callback); }

    /**
     * @brief 设置系统级快捷键可用性探测器。
     *
     * 回调应在组合可注册时返回 true；若被 Windows、其他程序或当前
     * SnowDesktop 运行实例占用则返回 false。
     */
    void SetHotkeyAvailabilityCallback(
        std::function<bool(HotkeySettingTarget, UINT, UINT)> callback)
    {
        hotkeyAvailabilityCallback_ = std::move(callback);
    }

    void SetDockSettingsPreviewChangedCallback(
        std::function<void(const DockSettings&)> callback)
    {
        dockSettingsPreviewChangedCallback_ =
            std::move(callback);
    }

    void SetPersonalizationChangedCallback(std::function<void()> callback)
    { personalizationChangedCallback_ = std::move(callback); }

    void SyncDockEnabled(bool enabled) { dockEnabled_ = enabled; }
    void SyncSoftwareDesktopEnabled(bool enabled)
    { generalSettings_.softwareDesktopEnabled = enabled; }
    void SyncDockSettings(const DockSettings& settings)
    {
        dockSettings_ = settings;
        NormalizeDockSettings(dockSettings_);
    }
    void SyncNavigationSettings(const NavigationSettings& settings)
    { navigationSettings_ = settings; }

    void SetDisplaySettingsChangedCallback(std::function<void()> callback) { displaySettingsChangedCallback_ = std::move(callback); }
    void SetLayoutSpacingChangedCallback(
        std::function<void(float, bool)> callback)
    {
        layoutSpacingChangedCallback_ = std::move(callback);
    }
    void SetIconBeautifySettingsChangedCallback(
        std::function<void(snowdesktop::IconBeautifyUpdateKind)> callback)
    { iconBeautifySettingsChangedCallback_ = std::move(callback); }

    void SetCategorySettingsChangedCallback(std::function<void()> callback) { categorySettingsChangedCallback_ = std::move(callback); }

    /** @brief 设置从组件库向桌面添加 Lua 组件的回调。 */
    void SetAddWidgetToDesktopCallback(
        std::function<bool(const std::wstring&)> callback)
    {
        addWidgetToDesktopCallback_ = std::move(callback);
    }

    /** @brief 设置原生毛玻璃状态文本提供者（设置界面只读状态行）。 */
    void SetGlassStatusProvider(std::function<std::wstring()> provider) { glassStatusProvider_ = std::move(provider); }

    /** @brief 设置动画性能诊断文本提供者。 */
    void SetAnimationDiagnosticsProvider(
        std::function<std::wstring()> provider)
    { animationDiagnosticsProvider_ = std::move(provider); }

    /** @brief 设置仅本次运行有效的动画诊断开关。 */
    void SetAnimationDiagnosticsToggleCallback(
        std::function<void(bool)> callback)
    { animationDiagnosticsToggleCallback_ = std::move(callback); }

    void SyncDisplaySettings(float spacingScale,
        float fontSizeCu, float listFontSizeCu, float fontWeight,
        int shortcutArrowMode,
        const snowdesktop::IconBeautifySettings& iconBeautifySettings)
    {
        iconSpacingScale_ = snowdesktop::layout_spacing_rules::
            ClampScale(spacingScale);
        itemFontSizeCu_ = fontSizeCu;
        listItemFontSizeCu_ = listFontSizeCu;
        itemFontWeight_ = fontWeight;
        shortcutArrowMode_ = std::clamp(shortcutArrowMode, 0, 2);
        iconBeautifySettings_ = snowdesktop::icon_beautify::Normalize(
            iconBeautifySettings);
        iconBeautifySettings_.preset = snowdesktop::icon_beautify::IdentifyPreset(
            iconBeautifySettings_);
        displaySpacingPct_ = static_cast<int>(std::round(
            iconSpacingScale_ * 100.0f));
    }

    /** @} */
    /** @name 公共功能
     *  @{ */

    /**
     * @brief 显示退出确认弹窗（模态对话框）
     */
    bool ShowExitConfirm();

    /**
     * @brief 设置小组件引擎指针
     * @param engine WidgetEngine 对象指针
     */
    void SetWidgetEngine(class WidgetEngine* engine) { widgetEngine_ = engine; }

    /**
     * @brief 打开小组件编辑器页面并填充当前编辑的小组件信息
     * @param widgetIndex 小组件在引擎中的索引
     * @param widgetId    小组件唯一标识符
     * @param widgetName  小组件显示名称
     * @param scriptPath  小组件脚本路径
     */
    void ShowWidgetEditor(size_t widgetIndex, const wchar_t* widgetId,
        const wchar_t* widgetName, const wchar_t* scriptPath);

    /**
     * @brief 获取当前个性化设置（只读引用）
     * @return 指向 PersonalizationSettings 的常引用
     */
    const PersonalizationSettings& GetPersonalization() const { return personalization_; }
    const DockSettings& GetDockSettings() const { return dockSettings_; }
    PersonalizationSettings GetSystemTaskbarAppearance() const
    {
        return dockSettings_.systemTaskbarFollowPersonalization
            ? personalization_ : dockSettings_.systemTaskbarAppearance;
    }
    const CategorySettings& GetCategorySettings() const { return categorySettings_; }

    float GetIconSpacingScale() const { return iconSpacingScale_; }
    float GetItemFontSizeCu() const { return itemFontSizeCu_; }
    float GetListItemFontSizeCu() const { return listItemFontSizeCu_; }
    float GetItemFontWeightD() const { return itemFontWeight_; }
    int GetShortcutArrowMode() const { return shortcutArrowMode_; }
    const snowdesktop::IconBeautifySettings& GetIconBeautifySettings() const
    { return iconBeautifySettings_; }

    /** @} */

    /**
     * @brief 窗口过程函数（静态）
     * @param hwnd   窗口句柄
     * @param msg    窗口消息 ID
     * @param wParam 消息参数（平台相关）
     * @param lParam 消息参数（平台相关）
     * @return 消息处理结果，由 DefWindowProc 或 ImGui 决定
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    /** @name SwapChain 管理
     *  @{ */

    /**
     * @brief 创建 DXGI SwapChain 和 RenderTargetView
     * @return 创建成功返回 true
     */
    bool CreateSwapChain();

    /**
     * @brief 销毁 SwapChain 和 RenderTargetView 资源
     */
    void CleanupSwapChain();

    /** @} */

    /**
     * @brief 设置 ImGui 字体（根据 DPI 缩放加载默认字体）
     */
    void SetupFonts();

    /**
     * @brief 向窗口发送关闭消息，触发关闭流程
     */
    void RequestClose();
    /** @name 页面绘制
     *  @{ */

    /**
     * @brief 绘制左侧导航侧边栏（页面切换按钮列表）
     */
    void DrawSidebar();

    /**
     * @brief 绘制布局备份管理页面（列举、保存、恢复、删除备份）
     */
    void DrawBackupPage();

    /**
     * @brief 绘制通用设置页面（开机自启、重载、退出等选项）
     */
    void DrawGeneralPage();

    /**
     * @brief 绘制个性化设置页面（背景、字体等外观选项）
     */
    void DrawPersonalizationPage();

    void DrawDockPage();

    /** @brief 绘制共享的点击录制式快捷键控件。 */
    void DrawHotkeyRecorder(
        HotkeySettingTarget target,
        const char* label,
        const char* id,
        bool enabled,
        UINT modifiers,
        UINT virtualKey,
        UINT defaultModifiers,
        UINT defaultVirtualKey);
    /** @brief 开始捕获物理键盘组合。 */
    void StartHotkeyCapture(HotkeySettingTarget target);
    /** @brief 轮询当前物理键状态并完成快捷键录入。 */
    void UpdateHotkeyCapture();
    /** @brief 使用设置窗口收到的键盘消息补充快速按键捕获。 */
    void HandleHotkeyCaptureKeyMessage(
        UINT message, WPARAM virtualKey);
    /** @brief 取消当前快捷键录入。 */
    void CancelHotkeyCapture();
    /** @brief 将录制结果写入对应设置并请求保存。 */
    void CommitHotkeyCapture(
        HotkeySettingTarget target,
        UINT modifiers,
        UINT virtualKey);
    /** @brief 查找与当前组合冲突的另一个已启用功能。 */
    HotkeySettingTarget FindInternalHotkeyConflict(
        HotkeySettingTarget target,
        UINT modifiers,
        UINT virtualKey) const;

    void DrawSystemTaskbarPage();

    void DrawDisplayPage();

    void DrawCategorySettingsPage();
    void DrawWidgetPackagesPage();
    void DrawWidgetDeveloperTools();
    void RefreshAgentSkillStatuses();

    /**
     * @brief 绘制小组件编辑器页面（脚本编辑与保存）
     */
    void DrawWidgetEditorPage();

    /**
     * @brief 绘制调试页面（内部状态查看与诊断信息）
     */
    void DrawDebugPage();

    /**
     * @brief 绘制关于页面（版本信息、构建信息等）
     */
    void DrawAboutPage();

    /**
     * @brief 执行在线更新检查（调用 GitHub API）
     */
    void PerformUpdateCheck();

    /**
     * @brief 轮询并应用后台更新检查结果
     */
    void PollUpdateCheck();

    /** @} */

    /** @name 布局备份辅助方法
     *  @{ */

    /**
     * @brief 获取备份文件存储目录路径
     * @return 备份目录的完整路径（宽字符串）
     */
    std::wstring GetBackupDir() const;

    /**
     * @brief 列举所有已保存的布局备份
     * @return LayoutBackup 列表，按时间戳降序排列
     */
    std::vector<LayoutBackup> ListBackups() const;

    /**
     * @brief 将当前布局保存为备份
     * @param name 备份名称（用户输入的可读名称）
     * @return 保存成功返回 true
     */
    bool SaveBackup(const std::wstring& name);

    /**
     * @brief 从备份文件恢复布局
     * @param filename 备份文件名（不含路径）
     * @return 恢复成功返回 true
     */
    bool RestoreBackup(const std::wstring& filename);

    /**
     * @brief 删除指定的布局备份文件
     * @param filename 要删除的备份文件名（不含路径）
     * @return 删除成功返回 true
     */
    bool DeleteBackup(const std::wstring& filename);

    /**
     * @brief 选择其他 SnowDesktop 数据目录并迁入当前版本
     */
    void MigrateAllData();

    void CreateFullDataBackup();
    void ImportFullDataBackup();
    void ExportFullDataBackup(
        const snowdesktop::backup::BackupInfo& backup);
    void RestoreFullDataBackup(
        const snowdesktop::backup::BackupInfo& backup);
    void DeleteFullDataBackup(
        const snowdesktop::backup::BackupInfo& backup);
    void RestartAfterDataReplacement(const char* successMessageKey);

    /**
     * @brief 基于当前时间生成唯一的备份文件名
     * @return 格式为 "Layout_YYYYMMDD_HHMMSS" 的字符串
     */
    std::wstring MakeBackupTimestampName() const;

    /** @} */

    /** @name 系统功能
     *  @{ */

    /**
     * @brief 检查当前用户是否已启用开机自启
     * @return 已启用返回 true
     */
    bool IsAutoStartEnabled() const;

    /**
     * @brief 设置或取消开机自启
     * @param enable true 启用开机自启，false 禁用
     */
    void SetAutoStart(bool enable) const;

    void SyncCategoryRuleBuffersFromSettings();
    void NormalizeCategoryRuleBuffers();

    /** @} */

    /** @name 窗口与 D3D11 资源
     *  @{ */

    /// 应用程序实例句柄，用于窗口注册和创建
    HINSTANCE instance_ = nullptr;

    /// 设置窗口的 Win32 窗口句柄
    HWND hwnd_ = nullptr;

    /// 共享的 D3D11 设备指针（与主渲染线程共用）
    ComPtr<ID3D11Device> device_;

    /// D3D11 设备上下文（从 device_ 获取）
    ComPtr<ID3D11DeviceContext> context_;

    /// DXGI SwapChain，用于将 ImGui 渲染到独立的设置窗口
    ComPtr<IDXGISwapChain1> swapChain_;

    /// SwapChain 的后缓冲区 RenderTargetView
    ComPtr<ID3D11RenderTargetView> rtv_;

    /// SettingsWindow 独占的 ImGui 上下文及后端初始化阶段。
    ImGuiContext* imguiContext_ = nullptr;
    bool imguiWin32Initialized_ = false;
    bool imguiDx11Initialized_ = false;

    /** @} */

    /** @name 窗口布局与页面状态
     *  @{ */

    /// 窗口宽度（像素），初始值 800
    int windowWidth_ = 800;

    /// 窗口高度（像素），初始值 560
    int windowHeight_ = 560;

    /// 系统 DPI 缩放比例，用于字体和界面缩放适配
    float dpiScale_ = 1.0f;

    /// 当前活动页面索引（8 = 组件管理，9 = 组件创作）
    int activePage_ = 0;
    std::string widgetPackageStatus_;
    std::string pendingWidgetPackageUninstall_;
    std::string pendingWidgetPackageUninstallWorkshopItem_;
    int widgetPackageFilter_ = 0;
    char widgetPackageSearch_[160] = {};
    std::string widgetPermissionEditorPackageId_;
    std::string widgetPermissionEditorPackageVersion_;
    snowdesktop::widget::PackageSourceRef widgetPermissionEditorSource_;
    std::string widgetPermissionEditorScopeFingerprint_;
    std::vector<std::string> widgetPermissionEditorGrants_;
    std::vector<snowdesktop::steam_bridge::SkillInstallStatus>
        agentSkillStatuses_;
    std::string agentSkillStatusError_;
    std::string agentSkillActionStatus_;
    enum class PendingWidgetInstallKind
    {
        None,
        Local,
    };
    PendingWidgetInstallKind pendingWidgetInstallKind_ =
        PendingWidgetInstallKind::None;
    std::wstring pendingWidgetInstallPath_;
    std::wstring pendingWidgetInstallReason_;
    std::string pendingWidgetInstallDevelopmentId_;

    /// 备份名称输入缓冲区
    char backupNameBuf_[128] = {};

    /// 是否正在显示退出确认弹窗
    bool showExitConfirm_ = false;

    /// 是否请求关闭窗口（延迟关闭标记）
    bool pendingClose_ = false;

    /// 设置窗口脏帧标记；避免桌面消息触发无关的 ImGui Present。
    bool renderRequested_ = false;

    /// 防止尺寸变化消息在当前帧内嵌套进入 ImGui/DX11 渲染。
    bool renderInProgress_ = false;

    /// 当前正在录制快捷键的功能及已观察到的组合。
    HotkeySettingTarget hotkeyCaptureTarget_ =
        HotkeySettingTarget::None;
    UINT hotkeyCaptureModifiers_ = 0;
    UINT hotkeyCapturePressedModifiers_ = 0;
    UINT hotkeyCaptureVirtualKey_ = 0;
    bool hotkeyCapturePrimarySeen_ = false;
    bool hotkeyCapturePrimaryDown_ = false;
    bool hotkeyCaptureClearPending_ = false;

    /// 是否已解锁调试页面（通过版本号点击彩蛋激活）
    bool debugUnlocked_ = false;
    bool animationDiagnosticsEnabled_ = false;

    /// 版本号点击计数（用于激活调试页面的彩蛋逻辑）
    int versionClickCount_ = 0;

    /// 更新检查状态：空字符串=空闲，"checking"=检查中，其余为结果信息
    std::string updateCheckStatus_;
    /// 更新检查状态对应的翻译键；语言切换时据此重建缓存文案
    std::string updateCheckStatusKey_;
    /// 更新检查状态的可选格式化参数（当前用于最新版本号）
    std::string updateCheckStatusArgument_;
    /// 更新检查返回的最新版本号
    std::string latestVersion_;
    /// 更新检查返回的下载页面 URL
    std::string downloadUrl_;
    /// 是否有可用更新
    bool updateAvailable_ = false;
    /// 携带版更新检查使用的异步 HTTP 服务
    std::unique_ptr<AsyncHttpService> updateHttpService_;
    /// 当前更新检查请求 ID；0 表示没有进行中的请求
    int updateCheckRequestId_ = 0;

    /// MSIX StartupTask 状态是否已完成首次查询
    mutable bool packagedAutoStartStateKnown_ = false;

    /// MSIX StartupTask 最近一次查询或设置后的实际状态
    mutable bool packagedAutoStartEnabled_ = false;

    /// 调试页使用的 Font Awesome 字体
    ImFont* faDebugFont_ = nullptr;

    /// 内嵌 Font Awesome 字体中实际存在的私有区字符
    std::vector<unsigned int> faDebugCodepoints_;

    /// 调试页使用的 Fluent System Icons Regular 字体
    ImFont* fluentDebugFont_ = nullptr;

    /// 内嵌 Fluent Regular 字体中实际存在的私有区字符
    std::vector<unsigned int> fluentDebugCodepoints_;

    /** @} */

    /** @name 回调函数
     *  @{ */

    /// 布局重载回调（备份恢复后触发）
    std::function<void()> reloadCallback_;

    /// 退出应用回调
    std::function<void()> exitCallback_;

    /// 完整数据迁移后的重启回调
    std::function<void()> restartCallback_;

    /// 缓存失效回调（设置变更后通知主窗口）
    std::function<void()> invalidateCallback_;
    std::function<std::wstring()> animationDiagnosticsProvider_;
    std::function<void(bool)> animationDiagnosticsToggleCallback_;

    /// 导航设置变更回调
    std::function<void()> navigationSettingsChangedCallback_;

    /// 通用设置变更回调
    std::function<void()> generalSettingsChangedCallback_;

    std::function<void()> languageChangedCallback_;

    std::function<void(bool)> dockEnabledChangedCallback_;

    std::function<void()> dockSettingsChangedCallback_;

    /// 使用实际 RegisterHotKey 状态探测系统级占用。
    std::function<bool(HotkeySettingTarget, UINT, UINT)>
        hotkeyAvailabilityCallback_;

    std::function<void(const DockSettings&)>
        dockSettingsPreviewChangedCallback_;

    std::function<void()> personalizationChangedCallback_;

    /// 显示设置变更回调
    std::function<void()> displaySettingsChangedCallback_;
    std::function<void(float, bool)> layoutSpacingChangedCallback_;
    /// 图标美化预览/提交回调；false 仅刷新，true 持久化。
    std::function<void(snowdesktop::IconBeautifyUpdateKind)>
        iconBeautifySettingsChangedCallback_;

    /// 分类设置变更回调
    std::function<void()> categorySettingsChangedCallback_;

    /// 从组件库卡片把指定包添加到桌面。
    std::function<bool(const std::wstring&)> addWidgetToDesktopCallback_;

    /// 原生毛玻璃状态文本提供者
    std::function<std::wstring()> glassStatusProvider_;

    /** @} */

    /** @name 设置数据
     *  @{ */

    /// 当前个性化设置（背景、字体等）
    PersonalizationSettings personalization_;

    /// 个性化设置是否包含尚未持久化的修改
    bool personalizationDirty_ = false;

    /// 当前帧是否需要将个性化修改实时预览到桌面
    bool personalizationPreviewDirty_ = false;

    /// 是否应在当前帧持久化个性化设置（连续拖动结束后置位）
    bool personalizationSaveRequested_ = false;

    /// 当前导航设置
    NavigationSettings navigationSettings_;

    /// 导航设置是否已修改（需要保存）
    bool navigationSettingsDirty_ = false;

    /// 当前通用设置
    GeneralSettings generalSettings_;

    bool dockEnabled_ = false;

    DockSettings dockSettings_;

    bool dockSettingsDirty_ = false;
    bool dockSettingsPreviewDirty_ = false;
    bool dockSettingsSaveRequested_ = false;

    /// 通用设置是否已修改（需要保存）
    bool generalSettingsDirty_ = false;

    /// 当前分类设置
    CategorySettings categorySettings_ = CategorySettings::Defaults();

    /// 分类设置是否已修改（需要保存）
    bool categorySettingsDirty_ = false;

    /// 是否应在当前帧持久化分类设置
    bool categorySettingsSaveRequested_ = false;

    /// 当前图标间距缩放
    float iconSpacingScale_ = 1.0f;

    /// 当前桌面项目字号（cu）
    float itemFontSizeCu_ = kDefaultItemFontSizeCu;

    /// 当前内置列表项目字号（cu）
    float listItemFontSizeCu_ = kDefaultItemFontSizeCu;

    /// 当前桌面项目字体粗细 (DWRITE_FONT_WEIGHT)
    float itemFontWeight_ = 600.0f;

    int shortcutArrowMode_ = 0;

    /// 全局图标美化设置。
    snowdesktop::IconBeautifySettings iconBeautifySettings_{};
    snowdesktop::icon_beautify::ContinuousPreviewState
        iconBeautifyContinuousState_{};

    int displaySpacingPct_ = 100;

    struct CategoryRuleEditBuffer
    {
        std::wstring id;
        char label[128] = {};
        char extensions[1024] = {};
        bool usesDefaultLabel = false;
    };

    std::vector<CategoryRuleEditBuffer> categoryRuleBuffers_;
    char newCategoryLabelBuf_[128] = {};
    char newCategoryExtensionsBuf_[1024] = {};
    DWORD categorySettingsSavedTick_ = 0;

    /** @} */

    /** @name 小组件编辑器状态
     *  @{ */

    /// 小组件引擎指针（非拥有，由外部注入）
    class WidgetEngine* widgetEngine_ = nullptr;

    /// 正在编辑的小组件在引擎中的索引
    size_t editingWidgetIndex_ = static_cast<size_t>(-1);

    /// 返回主页面请求延迟到当前 ImGui frame 收尾后执行
    bool widgetEditorBackPending_ = false;

    /// 正在编辑的小组件唯一标识符
    std::wstring editingWidgetId_;

    /// 正在编辑的小组件显示名称
    std::wstring editingWidgetName_;

    /// 正在编辑的小组件脚本文件路径
    std::wstring editingScriptPath_;

    /** @} */
};

extern SettingsWindow* g_settingsWindow;
