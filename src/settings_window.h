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

#include "personalization.h"
#include "navigation_settings.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ImFont;

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
     */
    void Show();

    /**
     * @brief 检查窗口当前是否可见
     * @return 窗口已创建且可见时返回 true
     */
    bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

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
     * @brief 设置失效回调（通知主窗口使缓存失效）
     * @param callback 无参回调，在需要刷新缓存时触发
     */
    void SetInvalidateCallback(std::function<void()> callback) { invalidateCallback_ = std::move(callback); }

    /**
     * @brief 设置导航设置变更回调
     * @param callback 无参回调，在导航设置被修改后触发
     */
    void SetNavigationSettingsChangedCallback(std::function<void()> callback) { navigationSettingsChangedCallback_ = std::move(callback); }

    /** @} */
    /** @name 公共功能
     *  @{ */

    /**
     * @brief 显示退出确认弹窗（模态对话框）
     */
    void ShowExitConfirm();

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

    /** @} */

    /** @name 窗口布局与页面状态
     *  @{ */

    /// 窗口宽度（像素），初始值 800
    int windowWidth_ = 800;

    /// 窗口高度（像素），初始值 560
    int windowHeight_ = 560;

    /// 系统 DPI 缩放比例，用于字体和界面缩放适配
    float dpiScale_ = 1.0f;

    /// 当前活动页面索引（0 = 通用, 1 = 个性化, 2 = 备份, 3 = 小组件编辑器, 4 = 调试, 5 = 关于）
    int activePage_ = 0;

    /// 备份名称输入缓冲区
    char backupNameBuf_[128] = {};

    /// 是否正在显示退出确认弹窗
    bool showExitConfirm_ = false;

    /// 是否请求关闭窗口（延迟关闭标记）
    bool pendingClose_ = false;

    /// 是否已解锁调试页面（通过版本号点击彩蛋激活）
    bool debugUnlocked_ = false;

    /// 版本号点击计数（用于激活调试页面的彩蛋逻辑）
    int versionClickCount_ = 0;

    /// 更新检查状态：空字符串=空闲，"checking"=检查中，其余为结果信息
    std::string updateCheckStatus_;
    /// 更新检查返回的最新版本号
    std::string latestVersion_;
    /// 更新检查返回的下载页面 URL
    std::string downloadUrl_;
    /// 是否有可用更新
    bool updateAvailable_ = false;

    /// 调试页使用的 Font Awesome 字体
    ImFont* faDebugFont_ = nullptr;

    /// 内嵌 Font Awesome 字体中实际存在的私有区字符
    std::vector<unsigned int> faDebugCodepoints_;

    /** @} */

    /** @name 回调函数
     *  @{ */

    /// 布局重载回调（备份恢复后触发）
    std::function<void()> reloadCallback_;

    /// 退出应用回调
    std::function<void()> exitCallback_;

    /// 缓存失效回调（设置变更后通知主窗口）
    std::function<void()> invalidateCallback_;

    /// 导航设置变更回调
    std::function<void()> navigationSettingsChangedCallback_;

    /** @} */

    /** @name 设置数据
     *  @{ */

    /// 当前个性化设置（背景、字体等）
    PersonalizationSettings personalization_;

    /// 个性化设置是否已修改（需要保存）
    bool personalizationDirty_ = false;

    /// 当前导航设置
    NavigationSettings navigationSettings_;

    /// 导航设置是否已修改（需要保存）
    bool navigationSettingsDirty_ = false;

    /** @} */

    /** @name 小组件编辑器状态
     *  @{ */

    /// 小组件引擎指针（非拥有，由外部注入）
    class WidgetEngine* widgetEngine_ = nullptr;

    /// 正在编辑的小组件在引擎中的索引
    size_t editingWidgetIndex_ = static_cast<size_t>(-1);

    /// 正在编辑的小组件唯一标识符
    std::wstring editingWidgetId_;

    /// 正在编辑的小组件显示名称
    std::wstring editingWidgetName_;

    /// 正在编辑的小组件脚本文件路径
    std::wstring editingScriptPath_;

    /** @} */
};

extern SettingsWindow* g_settingsWindow;
