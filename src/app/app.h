/**
 * @file app.h
 * @brief SnowDesktop 主应用程序类的声明头文件。
 *
 * 该文件定义了桌面应用程序的核心结构，包括：
 * - 拖拽渲染缓存 DragRenderCache，用于高效缓存拖拽过程中的静态场景
 * - 主应用类 DesktopApp，封装了窗口管理、图形渲染、数据模型、
 *   拖放交互、托盘图标、上下文菜单、快速导航、部件系统等核心功能
 * - 快速导航条目 QuickNavigationEntry，用于在快速导航面板中展示
 *   桌面项、文件夹条目等快捷入口
 *
 * DesktopApp 实现了 COM 接口 IDropTarget 和 IDropSource，支持
 * OLE 拖放协议，可接受来自外部程序的文件拖入以及发起内部拖拽。
 *
 * @note 本文件仅包含声明，内联实现分散在 app_run.h、app_gfx.h、
 *       app_interact.h、app_menu.h、app_grid.h 等子头文件中。
 */
#pragma once
#include "item.h"
#include "slot.h"
#include "container.h"
#include "desktop.h"
#include "widget.h"
#include "drop_model.h"
#include "drag_session.h"
#include "settings_window.h"
#include "navigation_settings.h"
#include "general_settings.h"
#include "utils.h"
#include "widget_engine.h"
#include "types.h"
#include "constants.h"
#include "resource.h"

#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

enum class IconLoadPhase { Phase1, Phase2 };

struct IconLoadTask {
    uint64_t serial = 0;
    std::wstring requestKey;
    std::wstring layoutKey;
    std::wstring widgetId;
    Pidl absolutePidl;
    int sysIconIndex = -1;
    std::wstring parsingName;
    bool isDesktopItem = true;
    std::wstring folderPath;
    IconLoadPhase phase = IconLoadPhase::Phase1;
};

struct IconLoadResult {
    uint64_t serial = 0;
    std::wstring requestKey;
    std::wstring layoutKey;
    std::wstring widgetId;
    HBITMAP bitmap = nullptr;
    SIZE bitmapSize{};
    bool shortcutArrow = false;
    IconLoadPhase phase = IconLoadPhase::Phase1;
    bool isDesktopItem = true;
    std::wstring folderPath;
};

/**
 * @brief 拖拽渲染缓存，用于缓存拖拽操作期间的静态场景位图。
 *
 * 在拖拽过程中，桌面背景的静态部分只需绘制一次并缓存为位图，
 * 后续帧只需要将动态内容（如拖拽图标、放置预览）叠加绘制到
 * 缓存的静态位图上，从而大幅减少每帧的绘制开销。
 */
class DragRenderCache
{
public:
    /** @brief 重置缓存，释放所有 GPU 资源。在窗口尺寸变化或设备丢失时调用。 */
    void Reset();

    /**
     * @brief 确保缓存已创建且为最新版本。
     * @param device     D2D 设备对象
     * @param pixelSize  所需的像素尺寸
     * @param revision   场景修订号，修订号未变时跳过重绘以提高性能
     * @param drawStatic 用于绘制静态场景的回调函数
     * @return true 表示缓存已就绪；false 表示创建失败
     */
    bool Ensure(ID2D1Device* device, D2D1_SIZE_U pixelSize, std::uint64_t revision,
        const std::function<void(ID2D1DeviceContext*)>& drawStatic);

    /** @brief 将缓存的静态场景绘制到指定设备上下文中。 @param ctx D2D 设备上下文 */
    void Draw(ID2D1DeviceContext* ctx) const;

private:
    ComPtr<ID2D1Bitmap1> bitmap_;      /**< 缓存的静态场景位图 */
    ComPtr<ID2D1DeviceContext> context_; /**< 离屏渲染设备上下文 */
    UINT width_ = 0;                   /**< 缓存位图宽度 */
    UINT height_ = 0;                  /**< 缓存位图高度 */
    std::uint64_t revision_ = 0;       /**< 当前缓存的修订号，用于判断是否需要重绘 */
};

/**
 * @brief SnowDesktop 主应用程序类。
 *
 * 桌面应用的核心类，负责以下功能：
 * - 创建和管理桌面覆盖窗口，与 Explorer 桌面宿主交互
 * - 基于 Direct2D / Direct3D / DirectComposition 的图形渲染管线
 * - 桌面图标（DesktopItem）和网格页面（GridPage）的数据管理
 * - 部件系统（Widget）：集合、文件分类、文件夹映射、Lua 脚本等
 * - OLE 拖放协议支持（IDropTarget / IDropSource），处理内部和外部拖拽
 * - 托盘图标与系统托盘菜单
 * - 上下文菜单（外壳扩展、新建菜单等）
 * - 快速导航面板，用于快速定位桌面项和部件条目
 * - 布局持久化：网格尺寸、页面状态保存与恢复
 *
 * 实现 split 模式：核心声明在此文件中，内联实现按功能域分散在
 * app_run.h、app_gfx.h、app_interact.h、app_menu.h、app_grid.h 中。
 */
class DesktopApp : public IDropTarget, public IDropSource
{
public:
    /** @brief 默认构造函数，refCount_ 初始化为 1。 */
    DesktopApp() = default;

    /** @brief 析构函数，释放所有 COM 资源、GPU 资源和窗口资源。 */
    ~DesktopApp();

    // ── IUnknown ────────────────────────────────────────────
    /**
     * @brief 查询 COM 接口。
     * @param riid   请求的接口 ID
     * @param object 输出指针，接收接口指针
     * @return S_OK 成功，E_NOINTERFACE 不支持该接口
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    /** @brief 递增 COM 引用计数。 @return 新的引用计数值 */
    ULONG STDMETHODCALLTYPE AddRef() override;
    /** @brief 递减 COM 引用计数，减至零时销毁对象。 @return 新的引用计数值 */
    ULONG STDMETHODCALLTYPE Release() override;

    // ── IDropTarget ────────────────────────────────────────
    /**
     * @brief 拖拽对象进入窗口时调用，判断是否接受拖入。
     * @param dataObject 拖拽数据对象
     * @param keyState   键盘修饰键状态
     * @param point      鼠标屏幕坐标
     * @param effect     输出允许的拖放效果（DROPEFFECT_*）
     */
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;
    /**
     * @brief 拖拽对象在窗口内移动时调用，更新拖放效果。
     * @param keyState 键盘修饰键状态
     * @param point    鼠标屏幕坐标
     * @param effect   输出当前的拖放效果
     */
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override;
    /** @brief 拖拽离开窗口时调用，清理拖拽状态。 */
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    /**
     * @brief 拖拽对象在窗口内释放（放置）时调用，执行放置操作。
     * @param dataObject 拖拽数据对象
     * @param keyState   键盘修饰键状态
     * @param point      鼠标屏幕坐标
     * @param effect     输出最终执行的拖放效果
     */
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;

    // ── IDropSource ────────────────────────────────────────
    /**
     * @brief 查询是否继续拖拽或取消/执行放置。
     * @param escapePressed Escape 键是否被按下
     * @param keyState      键盘修饰键状态
     * @return S_OK 继续拖拽，DRAGDROP_S_CANCEL 取消，DRAGDROP_S_DROP 执行放置
     */
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
    /**
     * @brief 提供拖拽过程中的视觉反馈（光标样式等）。
     * @param effect 当前的拖放效果
     * @return DRAGDROP_S_USEDEFAULT 使用系统默认光标
     */
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

    /**
     * @brief 启动应用程序的主消息循环。
     * @param instance    应用程序实例句柄
     * @param showCommand 窗口显示命令（来自 WinMain）
     * @return 退出码
     */
    int Run(HINSTANCE instance, int showCommand);

    // ── Friends（面向对象渲染分发）──────────────────────────
    friend class DesktopIcon;
    friend class FolderEntryIcon;
    friend class DesktopGrid;
    friend class Widget;
    friend class WidgetContainer;
    friend class Collection;
    friend class FileCategories;
    friend class FolderMapping;
    friend class ScrollingItemWidget;
    friend class LuaScript;
    friend class GuideWidget;

    /**
     * @brief 快速导航面板中的条目结构体。
     *
     * 表示快速导航面板中的一个可操作项，可以是桌面图标项（DesktopItem）
     * 或文件夹部件中的条目（FolderEntry），包含图标、名称、路径等信息，
     * 供导航面板渲染和交互使用。
     */
    struct QuickNavigationEntry
    {
        /**
         * @brief 条目来源类型。
         * @var DesktopItem  来自桌面图标
         * @var FolderEntry  来自文件夹映射部件的条目
         */
        enum class Kind
        {
            DesktopItem,
            FolderEntry,
        };

Kind kind = Kind::DesktopItem;            /**< 条目来源类型 */
        size_t itemIndex = static_cast<size_t>(-1);   /**< 在桌面项列表中的索引（DesktopItem 类型时有效） */
        size_t widgetIndex = static_cast<size_t>(-1); /**< 所在部件的索引 */
        size_t folderEntryIndex = static_cast<size_t>(-1); /**< 在文件夹条目列表中的索引（FolderEntry 类型时有效） */
        std::wstring name;           /**< 显示名称 */
        std::wstring path;           /**< 完整路径 */
std::wstring source;         /**< 来源标识 */
        HBITMAP iconBitmap = nullptr; /**< 图标位图句柄 */
    };

    // ── OO 系统访问器（Object-Oriented System Accessors）────
    /** @brief 获取所有容器的引用（网格、部件等）。 @return 容器指针的 vector 引用 */
    std::vector<std::unique_ptr<Container>>& GetContainers() { return containers_; }
    /** @brief 获取所有面向对象项的常量引用。 @return Item 唯一指针 vector 的 const 引用 */
    const std::vector<std::unique_ptr<Item>>& GetItemsOO() const { return items_oo_; }
    /** @brief 获取所有面向对象项的可变引用。 @return Item 唯一指针 vector 的引用 */
    std::vector<std::unique_ptr<Item>>& GetItemsOO() { return items_oo_; }
    /** @brief 获取桌面项的常量引用（旧式结构体数组）。 @return DesktopItem vector 的 const 引用 */
    const std::vector<DesktopItem>& GetDesktopItems() const { return items_; }
    /** @brief 获取桌面项的可变引用。 @return DesktopItem vector 的引用 */
    std::vector<DesktopItem>& GetDesktopItems() { return items_; }
    /** @brief 获取桌面网格容器指针。 @return DesktopGrid 指针，尚未初始化时返回 nullptr */
    DesktopGrid* GetDesktopGrid() { return static_cast<DesktopGrid*>(containers_.empty() ? nullptr : containers_[0].get()); }
    /** @brief 获取所有部件的可变引用。 @return DesktopWidget vector 的引用 */
    std::vector<DesktopWidget>& GetWidgets() { return widgets_; }
    /** @brief 获取所有部件的常量引用。 @return DesktopWidget vector 的 const 引用 */
    const std::vector<DesktopWidget>& GetWidgets() const { return widgets_; }
    /** @brief 使整个桌面窗口失效并触发重绘。 */
    void InvalidateDesktop() { ::InvalidateRect(hwnd_, nullptr, TRUE); }

private:
    // ── Window ──────────────────────────────────────────────
    /**
     * @brief 主窗口过程。
     * @param hwnd 窗口句柄
     * @param msg  消息 ID
     * @param wp   消息的 WPARAM
     * @param lp   消息的 LPARAM
     * @return 消息处理结果
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    /**
     * @brief 快速导航窗口的窗口过程。
     * @param hwnd 窗口句柄
     * @param msg  消息 ID
     * @param wp   消息的 WPARAM
     * @param lp   消息的 LPARAM
     * @return 消息处理结果
     */
    static LRESULT CALLBACK QuickNavigationWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    /**
     * @brief 主窗口消息分发处理。
     * @param hwnd 窗口句柄
     * @param msg  消息 ID
     * @param wp   消息的 WPARAM
     * @param lp   消息的 LPARAM
     * @return 消息处理结果
     */
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    /**
     * @brief 快速导航窗口消息分发处理。
     * @param hwnd 窗口句柄
     * @param msg  消息 ID
     * @param wp   消息的 WPARAM
     * @param lp   消息的 LPARAM
     * @return 消息处理结果
     */
    LRESULT HandleQuickNavigationMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    /** @brief 创建桌面覆盖窗口，挂载到 Explorer 桌面上层。 @return 成功返回 true */
    bool CreateDesktopOverlayWindow();
    /** @brief 重置桌面窗口相关的 D2D/DComp 资源（窗口尺寸变化或设备丢失时调用）。 */
    void ResetDesktopWindowResources();
    /** @brief 将覆盖窗口附加到指定的桌面宿主窗口。 @param host 桌面宿主窗口句柄 */
    void AttachWindowToDesktopHost(HWND host);
    /** @brief 创建独立键盘输入窗口。 @param host 桌面宿主窗口句柄 @return 成功返回 true */
    bool CreateDesktopInputWindow(HWND host);
    /** @brief 将键盘输入窗口附加到桌面宿主并放置到不可见区域。 @param host 桌面宿主窗口句柄 */
    void AttachInputWindowToDesktopHost(HWND host);
    /** @brief 将键盘焦点交给独立输入窗口。 */
    void FocusDesktopInputWindow();
    /** @brief 请求退出应用程序，在下次消息循环中执行清理。 */
    void RequestExit();
    /** @brief 隐藏 Explorer 原生桌面图标。 */
    void HideExplorerIcons();
    /** @brief 恢复 Explorer 原生桌面图标。 */
    void RestoreExplorerIcons();
    /** @brief 向 OLE 注册窗口为拖放目标。 */
    void RegisterOleDropTarget();
    /**
     * @brief 在 Explorer 重启后恢复桌面宿主连接。
     *
     * Explorer 崩溃或重启时会创建新的桌面宿主窗口，
     * 此方法重新查找并附加到新的宿主窗口。
     */
    void RecoverDesktopHostAfterExplorerRestart();
    /** @brief 监听桌面宿主窗口状态，检测 Explorer 重启事件。 */
    void WatchDesktopHost();
    /** @brief 捕获当前活动显示器拓扑的稳定签名。 */
    std::wstring CaptureDisplayTopologySignature() const;
    /** @brief 防抖调度一次显示器拓扑复查。 */
    void ScheduleDisplayTopologyRefresh();
    /** @brief 仅在显示器拓扑确实变化时调整覆盖窗口并重建布局。 */
    void RefreshDisplayTopologyIfChanged();

    // ── Graphics ────────────────────────────────────────────
    /** @brief 初始化 Direct2D、Direct3D 和 DirectComposition 图形管线。 @return 成功返回 true */
    bool InitGraphics();
    /** @brief 重建图标标题文本格式（字号变更时调用）。 */
    void RecreateItemTextFormat();
    /** @brief 创建或调整 DirectComposition 表面的大小。 @return S_OK 成功，否则为 HRESULT 错误码 */
    HRESULT CreateOrResizeCompositionSurface();
    /** @brief WM_PAINT 响应，触发完整帧渲染。 */
    void OnPaint();
    /** @brief 渲染一帧画面到指定的 D2D 上下文。 @param ctx D2D 设备上下文 */
    void RenderFrame(ID2D1DeviceContext* ctx);
    /** @brief 绘制静态背景（桌面项图标、文本、网格等）。 @param ctx D2D 设备上下文 */
    void DrawStaticBackground(ID2D1DeviceContext* ctx);
    /** @brief 绘制动态叠加层（拖拽图标、放置预览、选择框等）。 @param ctx D2D 设备上下文 */
    void DrawDynamicOverlays(ID2D1DeviceContext* ctx);
    /** @brief 绘制翻页导航按钮（左右箭头）。 @param ctx D2D 设备上下文 */
    void DrawPageNavButtons(ID2D1DeviceContext* ctx);
    /** @brief 绘制换页通知覆盖层（左上角角标，类似电视台换台）。 @param ctx D2D 设备上下文 */
    void DrawPageNotify(ID2D1DeviceContext* ctx);
    /** @brief 绘制隐藏状态提示（双击取消隐藏）。 */
    void DrawHiddenHintOverlay(ID2D1DeviceContext* ctx);
    /** @brief 绘制添加组件操作提示。 */
    void DrawWidgetAddedHintOverlay(ID2D1DeviceContext* ctx);
    /** @brief 触发换页通知（记录文本与时间戳，启动定时器）。 @param text 通知文本 */
    void ShowPageNotify(const std::wstring& text);
    /** @brief 获取左右翻页导航按钮的矩形区域。 @param[out] outPrev 上一页按钮矩形 @param[out] outNext 下一页按钮矩形 */
    void GetNavButtonRects(RECT& outPrev, RECT& outNext) const;
    /** @brief 使拖拽静态场景缓存失效，下次拖拽时重建缓存。 */
    void InvalidateDragStaticScene();
    /** @brief 结束当前拖拽会话，清理拖拽状态。 */
    void EndDragSession();
    /** @brief 在控件重建后重新绑定拖拽源。 */
    void RebindDragSourceAfterRebuild();
    /**
     * @brief 更新拖拽期间的翻页按钮悬停状态，并在停留超时后翻页。
     * @param clientPoint 当前鼠标客户端坐标
     * @return 拖拽会话仍可继续时返回 true
     */
    bool UpdateDragPageNavigation(POINT clientPoint);

    // ── Data ────────────────────────────────────────────────
    /** @brief 从 Explorer 加载桌面项数据（IShellFolder 枚举）。 */
    void LoadDesktopItems();
    /** @brief 重新加载所有项目并可选从磁盘恢复布局。 @param reloadLayoutFromDisk 是否重新从磁盘加载布局 */
    void ReloadItems(bool reloadLayoutFromDisk = true);
    /** @brief 根据可用显示器信息更新布局工作区域。 */
    void UpdateLayoutWorkArea();
    /** @brief 用当前设置（行列数）配置指定网格页面。 @param page 网格页面引用 */
    void ConfigureGridPage(GridPage& page) const;
    /** @brief 将用户保存的网格尺寸应用到各页面上。 */
    void ApplySavedGridDimensions();
    /** @brief 根据图标间距比例重新计算页面单元格与间距。 @param page 网格页面引用 */
    void ApplyIconSpacingToPage(GridPage& page);
    /** @brief 执行网格布局，为每个桌面项计算槽位位置。 */
    void LayoutItems();
    /** @brief 重建容器（网格、部件）和面向对象项列表。 */
    void RebuildContainersAndItems();
    /** @brief 显示设置窗口。 */
    void ShowSettingsWindow();
    /** @brief 加载导航设置并应用（注册热键等）。 */
    void LoadNavigationSettingsAndApply();
    /** @brief 加载通用设置。 */
    void LoadGeneralSettingsAndApply();
    /** @brief 切换桌面图标可见性（双击空白处隐藏/恢复）。 */
    void ToggleDesktopIconsVisibility();
    /** @brief 显示隐藏状态提示文字。 */
    void ShowHiddenHint();
    /** @brief 清除隐藏状态提示。 */
    void ClearHiddenHint();
    /** @brief 显示添加组件操作提示。 */
    void ShowWidgetAddedHint();
    /** @brief 清除添加组件操作提示。 */
    void ClearWidgetAddedHint();
    /** @brief 注册快速导航热键。 */
    void ApplyNavigationHotkey();
    /** @brief 注销快速导航热键。 */
    void UnregisterNavigationHotkey();
    /** @brief 切换快速导航面板的打开/关闭状态。 */
    void ToggleQuickNavigation();
    /** @brief 打开快速导航面板。 */
    void OpenQuickNavigation();
    /** @brief 关闭快速导航面板。 */
    void CloseQuickNavigation();
    /** @brief 创建快速导航窗口。 @return 成功返回 true */
    bool CreateQuickNavigationWindow();
    /** @brief 销毁快速导航窗口。 */
    void DestroyQuickNavigationWindow();
    /** @brief 计算并设置快速导航窗口的位置。 */
    void PositionQuickNavigationWindow();
    /** @brief 使快速导航窗口失效并触发重绘。 */
    void InvalidateQuickNavigationWindow();
    /** @brief 绘制快速导航窗口内容。 @param hwnd 窗口句柄 */
    void PaintQuickNavigationWindow(HWND hwnd);
    /** @brief 确保快速导航搜索编辑框已创建。 */
    void EnsureQuickNavigationSearchEdit();
    /** @brief 更新快速导航搜索编辑框的位置和大小。 */
    void UpdateQuickNavigationSearchEditRect();
    /** @brief 刷新快速导航搜索文本内容。 */
    void RefreshQuickNavigationSearchText();
    /** @brief 计算快捷导航标签页的宽度。 */
    int GetQuickNavigationTabWidth() const;
    /** @brief 确保 navTabOrder_ 包含所有集合组件 ID。 */
    void EnsureNavTabOrder();

    // ── Layout persistence ──────────────────────────────────
    /** @brief 获取布局文件的完整路径。 @return 布局文件路径（JSON 格式） */
    std::wstring GetLayoutPath() const;
    /** @brief 从磁盘文件加载布局信息（槽位记录、页面配置等）。 */
    void LoadLayoutSlots();
    /** @brief 将当前布局信息保存到磁盘文件。 */
    void SaveLayoutSlots();
    /** @brief 从 JSON 字符串中恢复已保存的页面配置。 @param text 包含页面配置的 JSON 字符串 */
    void LoadSavedPagesFromJson(const std::string& text);
    /** @brief 记住已保存的页面 ID，用于维护页面顺序。 @param pageId 页面标识 */
    void RememberSavedPageId(const std::wstring& pageId);

    // ── Interaction ─────────────────────────────────────────
    /**
     * @brief 对坐标点进行命中测试，返回桌面项的索引。
     * @param pt 客户端坐标点
     * @return 命中项的索引，未命中返回 -1
     */
    int HitTestItem(POINT pt) const;
    /**
     * @brief 对坐标点进行命中测试，返回桌面图标对象指针。
     * @param pt 客户端坐标点
     * @return 命中项的 DesktopIcon 指针，未命中返回 nullptr
     */
    DesktopIcon* HitTestIcon(POINT pt) const;
    /** @brief 处理鼠标移动消息。 @param wp WPARAM @param lp LPARAM */
    void OnMouseMove(WPARAM wp, LPARAM lp);
    /** @brief 处理鼠标左键按下消息。 @param wp WPARAM @param lp LPARAM */
    void OnLeftButtonDown(WPARAM wp, LPARAM lp);
    /** @brief 处理鼠标左键释放消息。 @param wp WPARAM @param lp LPARAM */
    void OnLeftButtonUp(WPARAM wp, LPARAM lp);
    /** @brief 处理鼠标右键释放消息（弹出上下文菜单）。 @param lp LPARAM */
    void OnRightButtonUp(LPARAM lp);
    /** @brief 处理键盘按键消息。 @param key 按键虚拟键码 */
    void OnKeyDown(WPARAM key);
    /** @brief 根据键盘修饰键状态刷新拖拽提示文本。 */
    void RefreshDragHintFromKeyboard();
    /**
     * @brief 对选中项调用指定的外壳扩展谓词（如"删除"、"属性"等）。
     * @param verb 谓词名称字符串
     */
    void InvokeSelectedShellVerb(const char* verb);
    /**
     * @brief 获取所有选中的文件夹条目路径。
     * @param[out] firstWidgetIndex 可选，返回第一个条目所在部件的索引
     * @return 选中的条目路径列表
     */
    std::vector<std::wstring> GetSelectedFolderEntryPaths(size_t* firstWidgetIndex = nullptr) const;
    /** @brief 查找文件夹映射中选中的快捷方式的目标索引。 @return 目标索引 */
    size_t FindFolderMappingShortcutTarget() const;
    /**
     * @brief 复制或剪切选中的文件夹条目。
     * @param cut true 为剪切，false 为复制
     * @return 操作是否成功
     */
    bool CopyCutSelectedFolderEntries(bool cut);
    /** @brief 删除选中的文件夹条目。 @return 操作是否成功 */
    bool DeleteSelectedFolderEntries();
    /**
     * @brief 将剪贴板中的内容粘贴到指定的文件夹映射部件中。
     * @param widgetIndex 目标部件索引
     * @return 操作是否成功
     */
    bool PasteClipboardToFolderMapping(size_t widgetIndex);
    /** @brief 根据方向键在桌面网格上进行 2D 空间导航。 @param arrowKey 方向键的虚拟键码 */
    void NavigateDesktopGrid(WPARAM arrowKey);
    /** @brief 在组件内部导航成员项。 @param arrowKey 方向键的虚拟键码 */
    void NavigateWidgetMembers(WPARAM arrowKey);
    /** @brief 进入当前选中的组件以进行内部导航。 */
    void EnterWidget();
    /** @brief 退出组件内部导航，返回桌面网格。 */
    void ExitWidget();
    /** @brief 打开当前选中的桌面项（模拟双击）。 */
    void OpenSelectedDesktopItem();
    /** @brief 打开组件内指定索引的成员项。 @param widgetIndex 组件索引 @param memberIndex 成员索引 */
    void OpenWidgetMember(size_t widgetIndex, int memberIndex);
    /** @brief 根据当前选中状态同步键盘导航上下文（桌面/组件内模式）。 */
    void SyncKeyboardNavFromSelection();
    /** @brief 滚动组件确保指定成员项在视口内可见。 @param widgetIndex 组件索引 @param memberIndex 成员索引 */
    void ScrollWidgetToMember(size_t widgetIndex, int memberIndex);
    /** @brief 处理定时器事件。 @param timerId 定时器标识 */
    void OnTimer(WPARAM timerId);
    /** @brief 更新集合弹出面板的悬停停留计时。 @param point 当前鼠标位置 */
    void UpdateCollectionPopupDwell(POINT point);
    /**
     * @brief 尝试打开悬停停留后的集合弹出面板。
     * @param now 当前时间（毫秒）
     * @return 是否已打开弹出面板
     */
    bool TryOpenDwellCollectionPopup(DWORD now);
    /**
     * @brief 拖拽时优先检查集合弹窗命中（弹窗遮挡的容器不应被穿透命中）。
     * @param client 客户端坐标
     * @param[out] targetContainer 命中的容器
     * @param[out] targetSlot 命中的槽位
     * @param[out] targetRegion 命中的区域
     * @return 命中弹窗返回 true，否则 false
     */
    bool HitTestPopupForDrag(POINT client, Container*& targetContainer, Slot*& targetSlot, HitRegion& targetRegion);
    /** @brief 清除所有选中项。 */
    void ClearSelection();
    /**
     * @brief 判断指定的桌面项是否存在于任一部件中。
     * @param item 桌面项
     * @return 存在返回 true
     */
    bool IsItemInAnyWidget(const DesktopItem& item) const;
    /**
     * @brief 判断坐标点是否落在某个部件的标题栏或边框区域上。
     * @param pt 客户端坐标点
     * @return 是则返回 true
     */
    bool IsPointOverWidgetChrome(POINT pt) const;
    /** @brief 清除指定部件外部的选中状态。 @param widgetIndex 部件索引 */
    void ClearSelectionOutsideWidget(size_t widgetIndex);
    /** @brief 清除桌面区域的选择状态（仅保留部件内选择）。 */
    void ClearSelectionOutsideDesktop();
    /** @brief 仅选择指定索引的桌面项。 @param index 桌面项索引 */
    void SelectOnly(int index);
    /** @brief 仅选中指定索引的部件。 @param index 部件索引 */
    void SelectWidgetOnly(size_t index);
    /** @brief 切换指定桌面项的选择状态。 @param index 桌面项索引 */
    void ToggleSelection(int index);
    /** @brief 获取当前框选目标使用的滚动偏移。 */
    int GetMarqueeScrollOffset() const;
    /** @brief 获取当前框选目标的内容视口。 */
    RECT GetMarqueeViewportRect() const;
    /** @brief 按当前鼠标位置更新框选矩形及命中状态。 */
    void UpdateMarqueeSelection(POINT current);
    /**
     * @brief 处理翻页导航按钮的点击事件。
     * @param point 点击坐标
     * @return 是否已处理
     */
    bool HandlePageNavClick(POINT point);
    /** @brief 按名称对桌面图标排序。 @param ascending 是否升序 */
    void SortIconsByName(bool ascending = true);
    /** @brief 按类型对桌面图标排序。 @param ascending 是否升序 */
    void SortIconsByType(bool ascending = true);
    /**
     * @brief 对指定部件的内容进行排序。
     * @param widgetIndex 部件索引
     * @param mode 排序模式
     * @param ascending 是否升序
     */
    void SortWidgetContents(size_t widgetIndex, int mode, bool ascending = true);
    /** @brief 更新剪切状态高亮显示。 */
    void UpdateCutState();

    // ── Tray ────────────────────────────────────────────────
    /** @brief 添加托盘图标。 @param force 是否强制重新添加 */
    void AddTrayIcon(bool force = false);
    /** @brief 移除托盘图标。 */
    void RemoveTrayIcon();
    /** @brief 显示托盘气泡通知。 @param title 标题 @param message 内容 */
    void ShowBalloonNotification(const std::wstring& title, const std::wstring& message);
    /** @brief 在托盘图标上显示上下文菜单。 @param screenPoint 屏幕坐标 */
    void ShowTrayMenu(POINT screenPoint);
    /** @brief 处理托盘图标回调事件。 @param lParam 消息的 LPARAM */
    void OnTrayCallback(LPARAM lParam);

    // ── Context menus ───────────────────────────────────────
    /** @brief 显示桌面背景上下文菜单。 @param screenPoint 屏幕坐标 */
    void ShowBackgroundContextMenu(POINT screenPoint);
    /** @brief 连续显示行列调整菜单，直到用户取消。 */
    void ShowGridAdjustmentMenu(POINT screenPoint, UINT initialCommand);
    /** @brief 显示指定部件的上下文菜单。 @param screenPoint 屏幕坐标 @param widgetIndex 部件索引 */
    void ShowWidgetContextMenu(POINT screenPoint, size_t widgetIndex);
    /** @brief 显示文件夹条目上下文菜单。 @param screenPoint 屏幕坐标 @param widgetIndex 部件索引 @param memberIndex 成员索引 */
    void ShowFolderEntryContextMenu(POINT screenPoint, size_t widgetIndex, size_t memberIndex);
    /** @brief 显示桌面项上下文菜单。 @param screenPoint 屏幕坐标 @param itemIndex 桌面项索引 */
    void ShowItemContextMenu(POINT screenPoint, int itemIndex);
    /** @brief 显示外壳扩展上下文菜单。 @param screenPoint 屏幕坐标 @param itemIndex 桌面项索引（可选，-1 表示背景） */
    void ShowShellContextMenu(POINT screenPoint, int itemIndex = -1);
    /** @brief 显示"新建"菜单并执行选择的命令。 @param screenPoint 屏幕坐标 @param targetDir 目标目录 */
    void ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir);
    /** @brief 显示桌面背景的专用上下文菜单（含新建、显示设置等）。 @param screenPoint 屏幕坐标 */
    void ShowDesktopBackgroundContextMenu(POINT screenPoint);
    /** @brief 为指定路径显示外壳扩展上下文菜单。 @param folderPath 文件夹路径 @param screenPoint 屏幕坐标 */
    void ShowShellContextMenuForPath(const std::wstring& folderPath, POINT screenPoint);
    /**
     * @brief 创建用于菜单图标的位图（包含文本渲染）。
     * @param text 图标文字
     * @return 位图句柄
     */
    HBITMAP CreateMenuIconBitmap(const wchar_t* text);
    /** @brief 为菜单项设置自定义图标。 @param menu 菜单句柄 @param command 命令 ID @param text 图标文字 */
    void SetMenuItemIcon(HMENU menu, UINT_PTR command, const wchar_t* text);
    /** @brief 清除所有菜单图标，释放位图资源。 */
    void ClearMenuIcons();
    /** @brief 恢复桌面窗口层叠顺序。 */
    void RestoreDesktopWindowLayer();
    /**
     * @brief 判断桌面项是否为受保护的系统图标（如回收站）。
     * @param item 桌面项
     * @return 受保护返回 true
     */
    bool IsProtectedDesktopIcon(const DesktopItem& item) const;
    /**
     * @brief 判断上下文菜单中的命令是否为外壳重命名命令。
     * @param contextMenu 上下文菜单接口指针
     * @param commandOffset 命令偏移量
     * @return 是重命名命令返回 true
     */
    bool IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const;

    // ── Grid helpers ────────────────────────────────────────
    /**
     * @brief 从坐标点查找所在的网格页面。
     * @param point 客户端坐标
     * @return 网格页面指针，未找到返回 nullptr
     */
    const GridPage* GridPageFromPoint(POINT point) const;
    /** @brief 调整网格行数。 @param delta 行数增量 */
    void AdjustGridRows(int delta);
    /** @brief 调整网格列数。 @param delta 列数增量 */
    void AdjustGridColumns(int delta);
    /** @brief 将右键所在页面设置为指定行列预设。 */
    void SetGridDimensions(int columns, int rows);
    /** @brief 根据显示器宽高比和对角线英寸计算舒适的推荐网格。 */
    GridSpan CalculateRecommendedGridDimensions(
        int aspectWidth, int aspectHeight, float diagonalInches) const;
    /** @brief 从坐标点所在显示器切换首屏锁定（持久化、与末屏锁互斥）。 @param screenPoint 屏幕坐标 */
    void ToggleFirstPagePin(POINT screenPoint);
    /** @brief 从坐标点所在显示器切换末屏锁定（持久化、与首屏锁互斥）。 @param screenPoint 屏幕坐标 */
    void ToggleLastPagePin(POINT screenPoint);
    /** @brief 设置图标间距比例。 @param value 间距倍率 */
    void SetIconSpacing(float value);
    /** @brief 调整图标间距比例。 @param delta 间距增量 */
    void AdjustIconSpacing(float delta);
    /** @brief 设置图标标题字号（12/14/16）。 @param value 字号 */
    void SetItemFontSize(float value);
    /** @brief 设置图标标题字体粗细（粗/中/细）。 @param weight DWRITE_FONT_WEIGHT */
    void SetItemFontWeight(DWRITE_FONT_WEIGHT weight);
    /** @brief 应用页面到显示器的映射关系（编排清理/补齐/重排/映射）。 */
    void ApplyPageMapping();
    /** @brief 清理溢出区空页（保留前 N-1 槽位页与末屏当前显示的空页）。 */
    void PruneEmptyOverflowPages();
    /** @brief 按显示器数量补齐前 N-1 个槽位页。 */
    void PadPagesToMonitorCount();
    /** @brief 将页面 ID 重排为连续的 __page:1,2,3...（封装 NormalizePageIds 的有变动才执行）。 */
    void CompactPageIds();
    /** @brief 将 savedPageIds_ 映射到各显示器（末屏翻页 + pageOffset 钳制）。 */
    void MapPagesToMonitors();
    /** @brief 获取最大页面偏移量。 @return 最大偏移页数 */
    int MaxPageOffset() const;
    /**
     * @brief 判断页面是否有内容。
     * @param pageId 页面标识
     * @return 有内容返回 true
     */
    bool PageHasContent(const std::wstring& pageId) const;
    /**
     * @brief 从起始偏移沿指定方向查找下一个非空页面。
     * @param fromOffset 起始偏移
     * @param direction 方向（1 或 -1）
     * @return 找到的偏移量
     */
    int NextNonEmptyOffset(int fromOffset, int direction) const;
    /** @brief 按 delta 翻页，自动对齐到有内容的页面。 @param delta 方向（1 或 -1） */
    void NavigatePageOffset(int delta);
    /** @brief 跳转到指定的页面偏移量。 @param targetOffset 目标偏移 */
    void JumpToPageOffset(int targetOffset);
    /** @brief 新增空白分页，放置指南组件并自动跳转。 */
    void AddNewPage();
    /** @brief 在指定页面上放置分页指南组件。 @param pageId 页面标识 */
    void PlaceGuideWidgetOnPage(const std::wstring& pageId);
    /** @brief 生成唯一的 __page:N 格式页面 ID。 */
    std::wstring GeneratePageId() const;
    /** @brief 每次加载布局时将页面 ID 重新规整为 __page:1, __page:2... 顺序。 */
    void NormalizePageIds();
    /** @brief 获取保存页面的显示名称（首屏/分页1/分页2...）。 @param index 在 savedPageIds_ 中的索引 */
    std::wstring GetPageDisplayName(int index) const;
    size_t FirstMonitorOrderIndex() const;
    /** @brief 构建显示器渲染顺序列表。 @return 显示器索引列表 */
    std::vector<size_t> BuildMonitorRenderOrder() const;
    /**
     * @brief 尝试在网格中查找空闲单元格。
     * @param span 网格跨度
     * @param usedSlots 已占用槽位集合
     * @param[out] result 找到的空闲单元格
     * @param preferredPageId 首选页面 ID
     * @param preferredStartSlot 首选起始槽位
     * @return 是否找到空闲单元格
     */
    bool TryFindFreeCell(GridSpan span, std::unordered_set<std::wstring>& usedSlots, GridCell& result,
        const std::wstring& preferredPageId = L"", int preferredStartSlot = 0) const;
    /** @brief 在已占用集合中标记指定网格区域。 @param usedSlots 已占用集合 @param cell 网格单元格 @param span 跨度 */
    static void MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span);
    /** @brief 检查指定网格区域是否已被标记。 @param usedSlots 已占用集合 @param cell 网格单元格 @param span 跨度 @return 全部标记返回 true */
    static bool AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span);
    /** @brief 检查网格区域是否有效（不越界）。 @param cell 网格单元格 @param span 跨度 @return 有效返回 true */
    static bool IsGridAreaValid(const GridCell& cell, GridSpan span);
    /** @brief 重新布局被移位的桌面项。 */
    void RelayoutDisplacedItems();

    // ── Drag helpers ───────────────────────────────────────
    /**
     * @brief 从坐标点计算对应的网格单元格。
     * @param point 客户端坐标
     * @return 网格单元格
     */
    GridCell CellFromPoint(POINT point) const;
    /**
     * @brief 从坐标点计算对应的网格单元格（拖拽放置用，按左上角边界命中而非中心距离）。
     * @param point 客户端坐标
     * @return 网格单元格
     */
    GridCell CellFromPointForDrag(POINT point) const;
    /**
     * @brief 根据坐标和轴向获取网格轴索引。
     * @param page 网格页面
     * @param coordinate 坐标值
     * @param horizontal true 为水平轴，false 为垂直轴
     * @return 轴索引
     */
    static int GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal);
    /**
     * @brief 根据目标单元格查找最佳的放置位置。
     * @param targetCell 目标单元格
     * @return 最佳放置单元格
     */
    GridCell FindBestDropCell(GridCell targetCell) const;
    /** @brief 待处理的网格移动项（索引 + 目标单元格）。 */
    struct PendingGridMove { size_t index; GridCell cell; };
    /**
     * @brief 构建选中项的移动计划。
     * @param targetCell 目标单元格
     * @return 待处理的移动列表
     */
    std::vector<PendingGridMove> BuildSelectedMove(GridCell targetCell) const;
    /**
     * @brief 构建桌面区域的放置预览列表。
     * @param sourceList 拖拽源列表
     * @param targetCell 目标单元格
     * @param internalMove 是否为内部移动
     * @return 放置预览列表
     */
    std::vector<DropLanding> BuildDesktopLandings(const DragSourceList& sourceList,
        GridCell targetCell, bool internalMove) const;
    /**
     * @brief 判断网格区域是否被非选中项占用。
     * @param cell 网格单元格
     * @param span 跨度
     * @return 被占用返回 true
     */
    bool IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const;
    /** @brief 移动选中项到指定的目标单元格。 @param targetCell 目标单元格 */
    void MoveSelectedItemsToCell(GridCell targetCell);
    /** @brief 更新拖拽组的原点位置。 */
    void UpdateDragGroupOrigin();
    /** @brief 将选中项迁移到最后一个监视器的页面。 */
    void MigrateSelectedItemsToLastMonitorPage();
    /**
     * @brief 获取拖拽的目标点（考虑吸附效果）。
     * @param current 当前鼠标坐标
     * @return 调整后的目标点
     */
    POINT GetDragTargetPoint(POINT current) const;
    /** @brief 为选中的桌面项创建拖拽数据对象。 @return COM 数据对象 */
    ComPtr<IDataObject> CreateSelectedDataObject() const;
    /**
     * @brief 为指定路径列表创建文件拖拽数据对象。
     * @param paths 文件路径列表
     * @return COM 数据对象
     */
    static ComPtr<IDataObject> CreateFileDropDataObject(const std::vector<std::wstring>& paths);
    /**
     * @brief 为指定项列表创建拖拽数据对象。
     * @param sourceItems 源项指针列表
     * @return COM 数据对象
     */
    ComPtr<IDataObject> CreateDataObjectForItems(const std::vector<Item*>& sourceItems) const;
    /** @brief 将选中项放置到目标桌面项上。 @param targetIndex 目标桌面项索引 */
    void DropSelectedItemsOnTarget(int targetIndex);
    /**
     * @brief 根据键值查找桌面项的索引。
     * @param key 桌面项键值
     * @return 索引，未找到返回 -1
     */
    size_t FindItemIndexByKey(const std::wstring& key) const;
    /** @brief 重建桌面项目布局键索引。 */
    void RefreshDesktopItemIndexCache();
    /** @brief 重建所有桌面型组件已收纳键缓存。 */
    void RefreshCollectedKeysCache();
    /**
     * @brief 从所有部件中移除指定的桌面键值。
     * @param keys 需要移除的键值列表
     */
    void RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys);
    /** @brief 快照当前桌面键值集合。 @return 桌面键值集合 */
    std::unordered_set<std::wstring> SnapshotDesktopKeys() const;
    /**
     * @brief 计算自快照以来新增的桌面键值。
     * @param existingKeys 之前的键值快照
     * @return 新增的键值列表
     */
    std::vector<std::wstring> NewDesktopKeysSince(const std::unordered_set<std::wstring>& existingKeys) const;
    /**
     * @brief 构建拖拽源列表。
     * @param sourceItems 源项指针列表
     * @param origin 来源容器
     * @return 拖拽源列表
     */
    DragSourceList BuildDragSourceList(const std::vector<Item*>& sourceItems, Container* origin) const;
    /**
     * @brief 构建放置预览列表。
     * @param sourceList 拖拽源列表
     * @param target 目标容器
     * @param targetSlot 目标槽位
     * @param region 命中区域
     * @param mods 修饰键
     * @param dropPoint 放置坐标
     * @return 放置预览列表
     */
    DropPreviewList BuildDropPreviewList(const DragSourceList& sourceList, Container* target,
        Slot* targetSlot, HitRegion region, int mods, POINT dropPoint) const;
    /**
     * @brief 构建外部拖拽到桌面的放置预览。
     * @param targetCell 目标单元格
     * @param count 项目数量
     * @return 放置预览列表
     */
    DropPreviewList BuildExternalDesktopPreviewList(GridCell targetCell, size_t count) const;
    /**
     * @brief 执行完整的放置流水线（包括内部移动和外部文件放置）。
     * @param sourceList 拖拽源列表
     * @param preview 放置预览
     * @return 操作是否成功
     */
    bool ExecuteDropPipeline(const DragSourceList& sourceList, const DropPreviewList& preview);
    /**
     * @brief 执行内部拖拽放置计划。
     * @param sourceList 拖拽源列表
     * @param preview 放置预览
     * @return 操作是否成功
     */
    bool ExecuteInternalDropPlan(const DragSourceList& sourceList, const DropPreviewList& preview);
    /**
     * @brief 执行基于文件的拖拽放置计划（来自外部或内部文件）。
     * @param sourceList 拖拽源列表
     * @param preview 放置预览
     * @return 操作是否成功
     */
    bool ExecuteFileBackedDropPlan(const DragSourceList& sourceList, const DropPreviewList& preview);
    /**
     * @brief 将文件实际写入桌面（从源列表物化）。
     * @param sourceList 拖拽源列表
     * @param action 放置动作
     * @param duplicateDesktopCopyNames 是否去重同名文件
     * @param[out] createdPathsBySource 可选，源索引到创建路径的映射
     * @return 操作是否成功
     */
    bool MaterializeFilesToDesktop(const DragSourceList& sourceList, DropAction action,
        bool duplicateDesktopCopyNames,
        std::unordered_map<size_t, std::wstring>* createdPathsBySource = nullptr);
    /**
     * @brief 将文件写入指定的目标文件夹。
     * @param sourceList 拖拽源列表
     * @param folder 目标文件夹路径
     * @param action 放置动作
     * @return 操作是否成功
     */
    bool MaterializeFilesToFolder(const DragSourceList& sourceList, const std::wstring& folder,
        DropAction action) const;
    /**
     * @brief 缓存待处理的放置信息（用于外壳刷新后恢复）。
     * @param sourceList 拖拽源列表
     * @param preview 放置预览
     * @param existingKeys 现有的桌面键快照
     * @param createdPathsBySource 可选，已创建路径的映射
     */
    void StorePendingLandingCache(const DragSourceList& sourceList, const DropPreviewList& preview,
        const std::unordered_set<std::wstring>& existingKeys,
        const std::unordered_map<size_t, std::wstring>* createdPathsBySource = nullptr);
    /**
     * @brief 判断拖拽是否为基于文件的放置。
     * @param sourceList 拖拽源列表
     * @param targetKind 目标类型
     * @param action 放置动作
     * @return 基于文件返回 true
     */
    bool IsDropFileBacked(const DragSourceList& sourceList, DropTargetKind targetKind,
        DropAction action) const;
    /** @brief 判断拖拽是否来自开启自动收集的桌面文件组件。 */
    bool IsAutoCollectFileCategorySource(const DragSourceList& sourceList) const;
    /** @brief 在 D2D 上下文中绘制桌面放置预览列表。 @param ctx D2D 上下文 @param preview 放置预览列表 */
    void DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx, const DropPreviewList& preview);
    /** @brief 获取或重建缓存的桌面放置预览（位置/动作不变时复用缓存，避免每帧重建）。 */
    const DropPreviewList& GetCachedDesktopDropPreview(bool hasItemDrag, const DragSourceList& sourceList,
        Container* target, Slot* slot, HitRegion region, int mods, POINT dragPoint);

    // ── Rendering helpers ───────────────────────────────────
    /** @brief 从项边界矩形计算图标区域。 @param bounds 项边界 @return 图标矩形 */
    RECT GetItemIconRect(RECT bounds) const;
    /** @brief 从项边界矩形计算文本区域。 @param bounds 项边界 @param expanded 是否展开 @return 文本矩形 */
    RECT GetItemTextRect(RECT bounds, bool expanded) const;
    /** @brief 获取项目所在网格单元相对于 92x116 基准尺寸的布局缩放比例。 */
    float GetItemLayoutScale(RECT bounds) const;
    /** @brief 从项边界矩形计算选中框区域。 @param bounds 项边界 @param expanded 是否展开 @return 选中框矩形 */
    RECT GetItemSelectionRect(RECT bounds, bool expanded) const;
    /**
     * @brief 绘制圆角矩形。
     * @param ctx D2D 上下文
     * @param rect 矩形区域
     * @param radius 圆角半径
     * @param fill 填充色
     * @param stroke 边框色
     * @param strokeWidth 边框宽度
     */
    void DrawD2DRoundedRectangle(ID2D1DeviceContext* ctx, RECT rect, float radius,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth = 1.0f);
    /**
     * @brief 绘制填充矩形。
     * @param ctx D2D 上下文
     * @param rect 矩形区域
     * @param fill 填充色
     * @param stroke 边框色
     */
    void DrawD2DFilledRectangle(ID2D1DeviceContext* ctx, RECT rect,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke);
    /**
     * @brief 绘制项的文字标签。
     * @param ctx D2D 上下文
     * @param bounds 边界矩形
     * @param text 文字内容
     * @param selected 是否选中
     * @param opacity 透明度
     */
    void DrawItemText(ID2D1DeviceContext* ctx, RECT bounds,
        const std::wstring& text, bool selected, float opacity = 1.0f);
    /**
     * @brief 使用桌面图标标题样式绘制已排版的文字。
     * @param ctx D2D 上下文
     * @param layout DirectWrite 文本布局
     * @param shadowKey 阴影缓存键
     * @param origin 文本左上角
     * @param layoutSize 文本布局尺寸
     * @param layoutScale 布局缩放
     * @param opacity 整体透明度
     */
    void DrawStyledItemTextLayout(ID2D1DeviceContext* ctx,
        IDWriteTextLayout* layout, const std::wstring& shadowKey,
        D2D1_POINT_2F origin, D2D1_SIZE_F layoutSize,
        float layoutScale, float opacity = 1.0f);
    /**
     * @brief 使用指定格式绘制 D2D 文字。
     * @param ctx D2D 上下文
     * @param text 文字内容
     * @param rect 绘制矩形
     * @param format 文字格式
     * @param color 文字颜色
     */
    void DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text,
        RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color);
    /** @brief 绘制集合弹出面板内容。 @param ctx D2D 上下文 */
    void DrawCollectionPopup(ID2D1DeviceContext* ctx);
    /** @brief 绘制快速导航叠加层。 @param ctx D2D 上下文 */
    void DrawQuickNavigationOverlay(ID2D1DeviceContext* ctx);
    /** @brief 将 RECT 转换为 D2D1_RECT_F。 @param r 输入矩形 @return D2D 矩形 */
    static D2D1_RECT_F ToD2DRect(const RECT& r);

    // D2D bitmap cache — public for Item::Draw
    /**
     * @brief 获取或创建与 HBITMAP 关联的 D2D 位图。
     * @param hbm GDI 位图句柄
     * @return D2D 位图指针，失败返回 nullptr
     */
    ID2D1Bitmap1* GetOrCreateD2DBitmap(HBITMAP hbm);

    /**
     * @brief 在指定矩形上绘制快捷方式箭头叠加层。
     * @param ctx D2D 上下文
     * @param iconRect 图标区域矩形（逻辑像素）
     * @param alpha 整体透明度
     */
    void DrawShortcutArrowOverlay(ID2D1DeviceContext* ctx, RECT iconRect, float alpha);

    // ── Async Icon Loading ──────────────────────────────────
    void StartIconLoader();
    void StopIconLoader();
    void BeginIconLoadGeneration();
    void EnqueueIconLoad(IconLoadTask task);
    void OnIconLoaded(WPARAM wParam, LPARAM lParam);
    void CacheSystemImageListSmall();
    void DrawPlaceholderIcon(ID2D1DeviceContext* ctx, int sysIconIndex, RECT iconRect, float alpha);

    // ── Filtering ───────────────────────────────────────────
    /**
     * @brief 获取桌面项的稳定布局键值，用于跨外壳刷新保持定位。
     * @param pidl 项的 PIDL
     * @param parsingName 解析名称
     * @param desktopIconClsid 可选的桌面图标 CLSID
     * @return 稳定的布局键值
     */
    std::wstring GetStableLayoutKey(PCIDLIST_ABSOLUTE pidl,
        const std::wstring& parsingName, const std::wstring& desktopIconClsid = {});
    /** @brief 将快捷方式箭头叠加到位图上。 @param bitmap 位图句柄 @param bitmapSize 位图尺寸 */
    static void ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize);
    /** @brief 注册外壳变更通知，监听文件系统变化。 */
    void RegisterShellChangeNotifications();

    // ── JSON helpers ────────────────────────────────────────
    /**
     * @brief 从 JSON 对象字符串中读取字符串字段。
     * @param objectText JSON 对象文本
     * @param fieldName 字段名
     * @param[out] value 读取的值
     * @return 成功返回 true
     */
    bool ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const;
    /**
     * @brief 从 JSON 对象字符串中读取整数字段。
     * @param objectText JSON 对象文本
     * @param fieldName 字段名
     * @param[out] value 读取的值
     * @return 成功返回 true
     */
    bool ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const;
    /**
     * @brief 从 JSON 对象字符串中读取布尔字段。
     * @param objectText JSON 对象文本
     * @param fieldName 字段名
     * @param[out] value 读取的值
     * @return 成功返回 true
     */
    bool ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const;
    /**
     * @brief 从 JSON 对象字符串中读取浮点字段。
     * @param objectText JSON 对象文本
     * @param fieldName 字段名
     * @param[out] value 读取的值
     * @return 成功返回 true
     */
    bool ReadJsonFloatField(const std::string& objectText, const char* fieldName, float& value) const;
    /**
     * @brief 从 JSON 对象字符串中读取字符串数组字段。
     * @param objectText JSON 对象文本
     * @param fieldName 字段名
     * @param[out] values 读取的值列表
     * @return 成功返回 true
     */
    bool ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const;
    /**
     * @brief 从 JSON 文本中查找对象的结束位置。
     * @param text JSON 文本
     * @param start 起始位置
     * @return 结束位置
     */
    size_t FindJsonObjectEnd(const std::string& text, size_t start) const;
    /**
     * @brief 从 JSON 文本中查找数组的结束位置。
     * @param text JSON 文本
     * @param start 起始位置
     * @return 结束位置
     */
    size_t FindJsonArrayEnd(const std::string& text, size_t start) const;
    /**
     * @brief 从 JSON 文本中查找成对容器（花括号/方括号）的结束位置。
     * @param text JSON 文本
     * @param start 起始位置
     * @param open 开括号字符
     * @param close 闭括号字符
     * @return 结束位置
     */
    size_t FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const;
    /**
     * @brief 从 JSON 字符串解析部件类型。
     * @param type JSON 中的类型字符串
     * @return 对应的 DesktopWidgetType 枚举值
     */
    DesktopWidgetType WidgetTypeFromJson(const std::wstring& type) const;
    /**
     * @brief 将部件类型序列化为 JSON 字符串。
     * @param type 部件类型枚举值
     * @return JSON 类型字符串
     */
    std::wstring WidgetTypeToJson(DesktopWidgetType type) const;

    // ── Widget helpers ──────────────────────────────────────
    /** @brief 生成新的唯一部件 ID。 @return 部件 ID 字符串 */
    std::wstring MakeNewWidgetId() const;
    /** @brief 判断组件是否允许通过宿主界面重命名。 */
    bool CanRenameWidget(const DesktopWidget& widget) const;
    /** @brief 根据组件类型或 Lua 清单初始化网格尺寸限制。 */
    void ConfigureWidgetGridLimits(DesktopWidget& widget) const;
    /** @brief 将组件跨度限制在组件声明和当前页面允许的范围内。 */
    GridSpan ClampWidgetGridSpan(const DesktopWidget& widget, GridSpan span,
        int availableColumns, int availableRows) const;
    /**
     * @brief 将部件添加到网格中。
     * @param widget 部件对象（移动语义）
     * @param span 网格跨度
     */
    void AddWidgetToGrid(DesktopWidget&& widget, GridSpan span);
    /** @brief 在指定屏幕位置创建集合部件。 @param screenPoint 屏幕坐标 */
    void AddCollectionWidgetAt(POINT screenPoint);
    /** @brief 在指定屏幕位置创建文件分类部件。 @param screenPoint 屏幕坐标 */
    void AddFileCategoryWidgetAt(POINT screenPoint);
    /** @brief 在指定屏幕位置创建文件夹映射部件。 @param screenPoint 屏幕坐标 */
    void AddFolderMappingWidgetAt(POINT screenPoint);
    /** @brief 在指定屏幕位置创建 Lua 脚本部件。 @param screenPoint 屏幕坐标 @param scriptFilename 脚本文件名 */
    void AddLuaWidgetAt(POINT screenPoint, const std::wstring& scriptFilename);
    /**
     * @brief 放置部件并位移冲突项。
     * @param widgetIndex 部件索引
     * @param targetCell 目标单元格
     * @param targetSpan 目标跨度
     * @param isMove 是否为移动操作
     */
    void PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan, bool isMove = false);
    /** @brief 枚举文件夹映射部件中的条目。 @param widget 部件引用 */
    void EnumerateFolderMappingEntries(DesktopWidget& widget);
    /** @brief 刷新文件夹映射部件的内容。 @param widgetIndex 部件索引 */
    void RefreshFolderMappingWidget(size_t widgetIndex);
    /**
     * @brief 收集文件分类部件中的项。
     * @param widgetIndex 部件索引
     * @param persist 是否持久化
     * @return 操作是否成功
     */
    bool CollectFileCategoryWidget(size_t widgetIndex, bool persist);
    /** @brief 对所有标记为自动收集的文件分类部件执行收集。 */
    void ApplyAutoCollectFileCategoryWidgets();
    /** @brief 确保只有一个自动收集的文件分类部件处于激活状态。 @param activeWidgetIndex 当前激活的部件索引 */
    void EnforceSingleAutoCollectFileCategory(size_t activeWidgetIndex);
    /**
     * @brief 在指定位置打开集合弹出面板。
     * @param widgetIndex 部件索引
     * @param anchorPoint 锚点坐标
     * @param categoryId 可选的分类 ID
     */
    void OpenCollectionPopupAt(size_t widgetIndex, POINT anchorPoint, const std::wstring& categoryId = L"");
    /** @brief 关闭集合弹出面板。 */
    void CloseCollectionPopup();
    /**
     * @brief 判断坐标点是否位于当前打开的弹出面板内。
     * @param point 客户端坐标
     * @return 在面板内返回 true
     */
    bool IsPointInsideOpenPopup(POINT point) const;
    /**
     * @brief 获取指定部件中弹出面板要显示的项键列表。
     * @param widget 部件引用
     * @return 项键值列表
     */
    std::vector<std::wstring> GetPopupItemKeys(const DesktopWidget& widget) const;
    /** @brief 获取快速导航面板中的集合部件索引列表。 @return 部件索引列表 */
    std::vector<size_t> GetQuickNavigationCollectionIndices() const;
    /** @brief 获取快速导航面板中的桌面项键值列表。 @return 项键值列表 */
    std::vector<std::wstring> GetQuickNavigationItemKeys() const;
    /** @brief 获取快速导航面板的所有条目。 @return 条目列表 */
    std::vector<QuickNavigationEntry> GetQuickNavigationEntries() const;
    /** @brief 获取快速导航面板的外层矩形。 @return 外层矩形 */
    RECT GetQuickNavigationRect() const;
    /** @brief 获取快速导航面板中搜索框的矩形。 @param overlay 面板矩形 @return 搜索框矩形 */
    RECT GetQuickNavigationSearchRect(const RECT& overlay) const;
    /** @brief 获取快速导航面板中标签页的矩形。 @param overlay 面板矩形 @return 标签页矩形 */
    RECT GetQuickNavigationTabsRect(const RECT& overlay) const;
    /** @brief 获取快速导航面板中内容区域的矩形。 @param overlay 面板矩形 @return 内容矩形 */
    RECT GetQuickNavigationContentRect(const RECT& overlay) const;
    /** @brief 获取快速导航面板中标签栏内容的宽度。 @param overlay 面板矩形 @return 内容宽度 */
    int GetQuickNavigationTabStripContentWidth(const RECT& overlay) const;
    /** @brief 获取快速导航面板中标签页的最大滚动偏移。 @param overlay 面板矩形 @return 最大滚动偏移 */
    int GetQuickNavigationMaxTabScrollOffset(const RECT& overlay) const;
    /** @brief 获取快速导航面板中指定标签页的矩形。 @param overlay 面板矩形 @param tabIndex 标签索引 @return 标签矩形 */
    RECT GetQuickNavigationTabRect(const RECT& overlay, size_t tabIndex) const;
    /** @brief 获取快速导航面板中指定项的矩形。 @param overlay 面板矩形 @param linearIndex 项索引 @return 项矩形 */
    RECT GetQuickNavigationItemRect(const RECT& overlay, size_t linearIndex) const;
    /** @brief 获取快速导航面板中的列数。 @param overlay 面板矩形 @return 列数 */
    int GetQuickNavigationColumnCount(const RECT& overlay) const;
    /** @brief 获取快速导航面板中项之间的间距。 @param overlay 面板矩形 @return 间距 */
    int GetQuickNavigationGap(const RECT& overlay) const;
    /** @brief 获取快速导航面板中内容的最大滚动偏移。 @param overlay 面板矩形 @return 最大滚动偏移 */
    int GetQuickNavigationMaxScrollOffset(const RECT& overlay) const;
    /** @brief 处理快速导航面板的点击事件。 @param point 点击坐标 @return 是否已处理 */
    bool HandleQuickNavigationClick(POINT point);
    /** @brief 获取集合弹出面板的矩形。 @param widget 部件引用 @return 面板矩形 */
    RECT GetCollectionPopupRect(const DesktopWidget& widget) const;
    /** @brief 获取集合弹出面板中内容区域的矩形。 @param popup 面板矩形 @return 内容矩形 */
    RECT GetCollectionPopupContentRect(const RECT& popup) const;
    /** @brief 获取集合弹出面板中的列数。 @param popup 面板矩形 @return 列数 */
    int GetCollectionPopupColumnCount(const RECT& popup) const;
    /** @brief 获取集合弹出面板中的行数。 @param widget 部件引用 @param popup 面板矩形 @return 行数 */
    int GetCollectionPopupRowCount(const DesktopWidget& widget, const RECT& popup) const;
    /** @brief 获取集合弹出面板中内容的最大滚动偏移。 @param widget 部件引用 @param popup 面板矩形 @return 最大滚动偏移 */
    int GetCollectionPopupMaxScrollOffset(const DesktopWidget& widget, const RECT& popup) const;
    /** @brief 获取集合弹出面板中指定项的矩形。 @param popup 面板矩形 @param linearIndex 项索引 @return 项矩形 */
    RECT GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const;
    /** @brief 处理鼠标滚轮消息。 @param wp WPARAM @param lp LPARAM */
    void OnMouseWheel(WPARAM wp, LPARAM lp);
    /**
     * @brief 在鼠标坐标处刷新拖拽目标状态。
     * @param clientPoint 客户端坐标
     * @param mods 修饰键状态
     */
    void RefreshDragTargetAt(POINT clientPoint, int mods);
    /**
     * @brief 获取独立模式部件的框架矩形。
     * @param widget 部件引用
     * @return 框架矩形
    */
    RECT GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const;
    float GetWidgetCellScale(const DesktopWidget& widget) const;
    /**
     * @brief 获取独立模式部件的移动手柄矩形。
     * @param widget 部件引用
     * @return 移动手柄矩形
     */
    RECT GetStandaloneWidgetMoveHandleRect(const DesktopWidget& widget) const;
    /**
     * @brief 获取独立模式部件的缩放手柄矩形。
     * @param widget 部件引用
     * @return 缩放手柄矩形
     */
    RECT GetStandaloneWidgetResizeHandleRect(const DesktopWidget& widget) const;
    /**
     * @brief 对独立模式部件进行命中测试。
     * @param widgetIndex 部件索引
     * @param pt 坐标点
     * @return 命中区域类型
     */
    WidgetHit HitTestStandaloneWidget(size_t widgetIndex, POINT pt) const;
    /**
     * @brief 对独立模式部件进行命中测试，返回部件索引。
     * @param pt 坐标点
     * @return 部件索引，未命中返回 -1
     */
    size_t HitTestStandaloneWidgetIndex(POINT pt) const;
    /** @brief 显示部件编辑器宿主窗口。 @param widgetIndex 部件索引 */
    void ShowWidgetEditorHost(size_t widgetIndex);
    /**
     * @brief 构建 Lua 桌面快照信息列表。
     * @param selectedOnly 是否仅包含选中项
     * @return Lua 桌面项信息列表
     */
    std::vector<LuaDesktopItemInfo> BuildLuaDesktopSnapshot(bool selectedOnly) const;
    /** @brief 通过 Lua 脚本打开指定路径。 @param path 要打开的路径 @return 操作是否成功 */
    bool LuaOpenPath(const std::wstring& path);
    /** @brief 通过 Lua 脚本在资源管理器中显示指定路径。 @param path 路径 @return 操作是否成功 */
    bool LuaRevealPath(const std::wstring& path);
    /** @brief 通过 Lua 脚本设置部件标题。 @param widgetId 部件 ID @param title 标题文字 */
    void LuaSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title);
    /** @brief 开始 Lua 内联文本编辑。 @param request 编辑请求参数 */
    void BeginLuaInlineTextEdit(const LuaInlineTextEditRequest& request);
    /** @brief 提交或取消 Lua 内联文本编辑。 @param cancel true 取消，false 提交 */
    void CommitLuaInlineTextEdit(bool cancel);
    /**
     * @brief 获取可见的集合项边界矩形。
     * @param itemIndex 项索引
     * @return 边界矩形
     */
    RECT GetVisibleCollectionItemBounds(size_t itemIndex) const;
    /**
     * @brief 查找当前选中的单个文件夹条目。
     * @param[out] widgetIndex 部件索引
     * @param[out] memberIndex 成员索引
     * @return 是否找到
     */
    bool FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const;
    /**
     * @brief 获取文件夹条目重命名编辑框的矩形。
     * @param widgetIndex 部件索引
     * @param memberIndex 成员索引
     * @return 编辑框矩形
     */
    RECT GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const;
    /** @brief 开始重命名文件夹条目。 @param widgetIndex 部件索引 @param memberIndex 成员索引 */
    void BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex);
    /** @brief 提交或取消文件夹条目重命名。 @param newName 新名称 @param cancel true 取消 */
    void CommitFolderEntryRename(const std::wstring& newName, bool cancel);

    // ── Member variables ────────────────────────────────────
    /** @brief 应用程序实例句柄 */
    HINSTANCE instance_ = nullptr;
    /** @brief 桌面覆盖窗口句柄 */
    HWND hwnd_ = nullptr;
    /** @brief 虚拟桌面区域（左、上、宽、高） */
    int virtualLeft_ = 0, virtualTop_ = 0, virtualWidth_ = 0, virtualHeight_ = 0;

    /** @name D3D / D2D / DComp 图形资源 */
    /** @{ */
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ID2D1Factory1* GetD2DFactory() const { return d2dFactory_.Get(); }
    IDWriteFactory* GetDWriteFactory() const { return dwriteFactory_.Get(); }
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    /** @brief 用于录制标题阴影蒙版的独立 D2D 上下文。 */
    ComPtr<ID2D1DeviceContext> itemTextEffectContext_;
    /** @brief 画笔缓存：颜色值到画刷的映射，按 ctx 失效，跨帧复用 */
    std::unordered_map<std::uint64_t, ComPtr<ID2D1SolidColorBrush>> brushCache_;
    ID2D1DeviceContext* brushCacheContext_ = nullptr;
    ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual2> dcompVisual_;
    ComPtr<IDCompositionSurface> dcompSurface_;
    UINT compositionWidth_ = 0, compositionHeight_ = 0;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> itemTextFormat_;
    ComPtr<IDWriteTextFormat> listItemTextFormat_;
    ComPtr<IDWriteTextFormat> navTabTextFormat_;
    ComPtr<IDWriteTextFormat> fileCategoryTabTextFormat_;
    ComPtr<IDWriteTextFormat> faTextFormat_;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> itemTextLayoutCache_;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> itemTextShadowCache_;
    HANDLE faFontHandle_ = nullptr;
    HFONT faMenuFont_ = nullptr;
    std::vector<HBITMAP> menuIconPool_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<WidgetEngine> widgetEngine_;
    NavigationSettings navigationSettings_;
    GeneralSettings generalSettings_;
    bool desktopIconsHidden_ = false;
    bool showHiddenHint_ = false;
    DWORD hiddenHintStartTick_ = 0;
    bool showWidgetAddedHint_ = false;
    DWORD widgetAddedHintStartTick_ = 0;
    bool navigationHotkeyRegistered_ = false;
    /** @} */

    /** @name Shell 外壳相关 */
    /** @{ */
    ComPtr<IShellFolder> desktopFolder_;
    Pidl desktopPidl_;
    Pidl recycleBinPidl_;
    DesktopWindows desktopWindows_{};
    ULONG shellChangeRegId_ = 0;
    bool reloading_ = false;
    /** @} */

    /** @name 数据模型 */
    /** @{ */
    std::vector<DesktopItem> items_;
    std::unordered_map<std::wstring, size_t> itemIndexByKeyCache_;
    std::vector<GridPage> gridPages_;
    std::vector<DesktopWidget> widgets_;
    std::unordered_map<std::wstring, LayoutRecord> layoutRecords_;
    std::unordered_map<std::wstring, bool> settingsIconVisibility_;
    std::unordered_map<std::wstring, int> savedPageColumns_;
    std::unordered_map<std::wstring, int> savedPageRows_;
    std::vector<std::wstring> savedPageIds_;
    RECT layoutWorkArea_{};
    float iconSpacingScale_ = 1.0f;
    float itemFontSize_ = kItemFontSize;
    DWRITE_FONT_WEIGHT itemFontWeight_ = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    std::wstring primaryMonitorId_;
    std::wstring firstPageMonitorId_;   // 持久化：锁定显示首屏的显示器
    std::wstring lastPageMonitorId_;    // 持久化：锁定显示末屏/翻页区的显示器
    std::wstring lastMonitorPageId_;    // 运行时：末屏当前显示的 pageId（savedPageIds_[N-1+pageOffset_]）
    int pageOffset_ = 0;
    // ── 键盘导航状态 ──
    bool keyboardNavInsideWidget_ = false;
    size_t keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    int keyboardNavMemberIndex_ = -1;
    int navHoverSide_ = 0;
    DWORD navAutoFlipTick_ = 0;
    int navAutoFlipDir_ = 0;
    // ── 换页通知覆盖层（电视台换台式角标） ──
    std::wstring pageNotifyText_;
    DWORD pageNotifyStartTick_ = 0;
    bool pageNotifyActive_ = false;
    POINT lastContextMenuScreenPoint_{};
    POINT gridAdjustmentMenuAnchor_{};
    HMENU gridAdjustmentParentMenu_ = nullptr;
    bool gridAdjustmentMenuAnchorValid_ = false;
    /** @} */

    /** @name 控制窗口（托盘图标所有权 + 桌面宿主监听） */
    /** @{ */
    HWND controlHwnd_ = nullptr;
    HWND inputHwnd_ = nullptr;
    HWND quickNavigationHwnd_ = nullptr;
    HWND quickNavigationSearchEdit_ = nullptr;
    HFONT quickNavigationSearchFont_ = nullptr;
    static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleInputMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND navigationHotkeyHwnd_ = nullptr;
    UINT taskbarRestartMsg_ = 0;
    bool exitRequested_ = false;
    bool customDesktopVisible_ = true;
    bool updatingDisplayTopology_ = false;
    std::wstring displayTopologySignature_;
    /** @} */

    /** @name 托盘图标 */
    /** @{ */
    HICON trayIcon_ = nullptr;
    HWND trayIconOwnerHwnd_ = nullptr;
    bool trayIconAdded_ = false;
    /** @} */

    /** @name 鼠标/交互状态 */
    /** @{ */
    POINT lastMousePoint_{};
    // per-widget 独立刷新定时器：timerId -> widgetId（manifest refreshIntervalMs 驱动）
    std::unordered_map<UINT_PTR, std::wstring> widgetTimerIds_;
    UINT_PTR nextWidgetTimerId_ = kWidgetTimerIdBase;
    bool mouseDown_ = false;
    POINT mouseDownPoint_{};
    Item* mouseDownHit_ = nullptr;
    bool marqueeActive_ = false;
    RECT marqueeRect_{};
    size_t marqueeWidgetIndex_ = static_cast<size_t>(-1);
    POINT marqueeAnchorPoint_{};
    int marqueeInitialScrollOffset_ = 0;
    size_t pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    size_t pendingCtrlToggleWidgetIndex_ = static_cast<size_t>(-1);
    Item*  pendingCtrlToggleWidgetItem_ = nullptr;
    /** @} */

    /** @name 拖拽状态 */
    /** @{ */
    DragSession dragSession_;
    DragRenderCache dragRenderCache_;
    int dragGroupOriginX_ = 0;
    int dragGroupOriginY_ = 0;
    // ── 拖放预览缓存（避免每帧重建 BuildDropPreviewList） ──
    DropPreviewList cachedDropPreview_;
    POINT cachedDropPreviewPoint_{ -1, -1 };
    int cachedDropPreviewMods_ = -1;
    Container* cachedDropPreviewTarget_ = nullptr;
    Slot* cachedDropPreviewSlot_ = nullptr;
    HitRegion cachedDropPreviewRegion_ = HitRegion::None;
    bool cachedDropPreviewHasItems_ = false;
    size_t cachedDropPreviewSourceCount_ = static_cast<size_t>(-1);
    /** @} */

    /** @name 部件拖拽/缩放状态 */
    /** @{ */
    size_t mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    bool draggingWidget_ = false;
    bool resizingWidget_ = false;
    enum class WidgetAction { None, PendingMove, PendingResize, Move, Resize };
    WidgetAction widgetAction_ = WidgetAction::None;
    GridCell widgetDragOriginalCell_{};
    GridSpan widgetDragOriginalSpan_{};
    GridCell widgetPreviewCell_{};
    GridSpan widgetPreviewSpan_{};
    bool widgetPreviewOccupied_ = false;
    /** @} */

    /** @name OLE 拖拽状态 */
    /** @{ */
    LONG refCount_ = 1;
    bool selfDragActive_ = false;
    bool selfDragReturned_ = false;
    std::vector<std::wstring> selfDragOutKeys_;
    bool externalDragActive_ = false;
    int externalDropFileCount_ = 0;
    bool externalDropHasShortcut_ = false;
    bool dropTargetRegistered_ = false;
    /** @} */

    /** @name 待处理放置缓存（源列表 -> 预览在外壳刷新后仍存活） */
    /** @{ */
    PendingLandingCache pendingLandingCache_;
    /** @} */

    /** @brief 将屏幕坐标转换为客户端坐标。 @param screen 屏幕坐标 @return 客户端坐标 */
    POINT ScreenPointToClient(POINTL screen) const;
    /** @brief 判断指定客户端坐标是否位于外部拖拽源窗口之上。 @param clientPoint 客户端坐标 @return 是则返回 true */
    bool IsExternalDropWindowAt(POINT clientPoint) const;
    /** @brief 判断指定窗口是否为已知的桌面表面窗口。 @param window 窗口句柄 @return 是则返回 true */
    bool IsKnownDesktopSurfaceWindow(HWND window) const;
    /** @brief 判断两个窗口是否位于同一窗口树中。 @param parent 父窗口 @param window 子窗口 @return 是则返回 true */
    static bool IsSameWindowTree(HWND parent, HWND window);
    /**
     * @brief 根据键盘状态和允许的效果选择拖放效果。
     * @param keyState 键盘修饰键状态
     * @param allowed 允许的拖放效果
     * @return 选择的拖放效果
     */
    DWORD ChooseDropEffect(DWORD keyState, DWORD allowed) const;

    /** @brief 回收站项计数（用于轮询检测回收站状态变化） */
    int64_t lastRecycleBinItemCount_ = -1;

    /** @brief 剪贴板剪切追踪的路径集合 */
    std::unordered_set<std::wstring> cutPaths_;

    /** @name 重命名编辑控件 */
    /** @{ */
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    size_t renameIndex_ = static_cast<size_t>(-1);
    bool renamingWidget_ = false;
    bool renamingFolderEntry_ = false;
    size_t renameFolderWidgetIndex_ = static_cast<size_t>(-1);
    size_t renameFolderEntryIndex_ = static_cast<size_t>(-1);
    /** @brief 开始重命名选中的项。 */
    void BeginRenameSelected();
    /** @brief 提交或取消重命名操作。 @param cancel true 取消，false 提交 */
    void CommitRename(bool cancel);
    /** @brief 重命名编辑框的子类化窗口过程。 @param hwnd 窗口句柄 @param message 消息 @param wParam WPARAM @param lParam LPARAM @param subclassId 子类化 ID @param refData 引用数据 @return 消息处理结果 */
    static LRESULT CALLBACK RenameEditSubclassProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    /** @brief 快速导航搜索框的子类化窗口过程。 @param hwnd 窗口句柄 @param message 消息 @param wParam WPARAM @param lParam LPARAM @param subclassId 子类化 ID @param refData 引用数据 @return 消息处理结果 */
    static LRESULT CALLBACK QuickNavigationSearchSubclassProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    HWND luaInlineEdit_ = nullptr;
    HFONT luaInlineEditFont_ = nullptr;
    std::wstring luaInlineEditWidgetId_;
    std::string luaInlineEditStorageKey_;
    bool luaInlineEditMultiline_ = false;
    COLORREF luaInlineEditTextColor_ = RGB(0, 0, 0);
    /** @brief Lua 内联编辑框的子类化窗口过程。 @param hwnd 窗口句柄 @param message 消息 @param wParam WPARAM @param lParam LPARAM @param subclassId 子类化 ID @param refData 引用数据 @return 消息处理结果 */
    static LRESULT CALLBACK LuaInlineEditSubclassProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    /** @} */

    /** @name 拖拽提示窗口 */
    /** @{ */
    std::wstring dragHint_;
    HWND hintHwnd_ = nullptr;
    std::wstring hintTextCache_;
    /** @brief 确保拖拽提示窗口已创建。 @return 成功返回 true */
    bool EnsureDragHintWindow();
    /** @brief 在客户端坐标位置显示拖拽提示。 @param clientPoint 客户端坐标 @param text 提示文本 */
    void ShowDragHintWindow(POINT clientPoint, const std::wstring& text);
    /** @brief 在屏幕坐标位置显示拖拽提示。 @param screenPoint 屏幕坐标 @param text 提示文本 */
    void ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text);
    /** @brief 隐藏拖拽提示窗口。 */
    void HideDragHintWindow();
    /** @brief 销毁拖拽提示窗口。 */
    void DestroyDragHintWindow();
    /** @brief 从拖拽数据对象中提取文件路径列表。 @param dataObject 数据对象 @return 路径列表 */
    static std::vector<std::wstring> GetDropPaths(IDataObject* dataObject);
    static std::vector<std::wstring> TryGetNonFileDropPaths(IDataObject* dataObject);
    static std::vector<std::wstring> TryExtractUrlFromDataObject(IDataObject* dataObject);
    static std::vector<std::wstring> TryExtractImageFromDataObject(IDataObject* dataObject);
    static std::vector<std::wstring> TryExtractTextFromDataObject(IDataObject* dataObject);
    static bool IsFileDownloadUrl(const std::wstring& url, std::wstring& fileName);
    static std::wstring HandleUrlContent(const std::wstring& url);
    /** @brief 从完整路径中提取文件名。 @param path 完整路径 @return 文件名 */
    static std::wstring FileNameFromPath(const std::wstring& path);
    /** @brief 判断项名与源文件名是否匹配（用于放置后定位）。 @param itemName 项名称 @param srcFileName 源文件名 @return 匹配返回 true */
    static bool MatchPendingName(const std::wstring& itemName, const std::wstring& srcFileName);
    /** @brief 应用待处理的放置操作（外壳刷新后执行）。 */
    void ApplyPendingPlacement();
    /** @} */

    /** @name 集合弹出面板 */
    /** @{ */
    size_t popupWidgetIndex_ = static_cast<size_t>(-1);
    RECT popupRect_{};
    int popupScrollOffset_ = 0;
    bool popupHasAnchor_ = false;
    POINT popupAnchorPoint_{};
    std::wstring popupPageId_;
    std::wstring popupCategoryId_;
    std::unique_ptr<DesktopIcon> popupMouseDownItem_;
    /** @brief 悬停打开：拖拽中悬停在集合"全部"按钮上 */
    size_t popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    DWORD popupDwellTick_ = 0;
    std::unique_ptr<Slot> popupDragTargetSlot_;
    /** @} */

    /** @name 快速导航 */
    /** @{ */
    bool quickNavigationOpen_ = false;
    size_t quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    int quickNavigationScrollOffset_ = 0;
    int quickNavigationTabScrollOffset_ = 0;
    POINT quickNavigationOpenPoint_{};
    RECT quickNavigationRect_{};
    std::wstring quickNavigationSearchText_;
    float quickNavDpiScale_ = 1.0f;
    std::vector<std::wstring> navTabOrder_;
    size_t quickNavTabDragIndex_ = static_cast<size_t>(-1);
    int quickNavTabDragDeltaX_ = 0;
    POINT quickNavTabDragStartPoint_{};
    bool quickNavTabDragging_ = false;

    int QuickNavScale(int px) const { return static_cast<int>(px * quickNavDpiScale_); }
    /** @} */

    /** @name 面向对象系统 */
    /** @{ */
    std::vector<std::unique_ptr<Container>> containers_;
    std::vector<std::unique_ptr<Item>> items_oo_;
    /** @brief 快速部件键值查找缓存（与容器同时重建） */
    std::unordered_set<std::wstring> collectedKeysCache_;
    /** @} */

    /** @brief D2D 位图缓存 */
    std::unordered_map<std::uintptr_t, ComPtr<ID2D1Bitmap1>> d2dIconCache_;

    /** @brief 快捷方式箭头图标的 D2D 位图缓存（惰性初始化） */
    ComPtr<ID2D1Bitmap1> shortcutArrowBitmap_;
    SIZE shortcutArrowBitmapSize_{};

    /** @name 异步图标加载 */
    /** @{ */
    std::thread iconLoaderThread_;
    std::mutex iconLoaderMutex_;
    std::condition_variable iconLoaderCv_;
    std::deque<IconLoadTask> iconLoaderQueue_;
    std::unordered_set<std::wstring> iconLoaderPendingKeys_;
    std::atomic<bool> iconLoaderRunning_{false};
    uint64_t iconLoadSerial_ = 0;

    HIMAGELIST systemImageListSmall_ = nullptr;
    ComPtr<ID2D1Bitmap1> systemIconStripBitmap_;
    int systemIconStripCount_ = 0;
    SIZE systemIconStripIconSize_{};
    /** @} */

    /** @name 新建菜单 COM 上下文 */
    /** @{ */
    ComPtr<IContextMenu2> newMenuContextMenu_;
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
    /** @} */
};

// ── Inline implementations (split into sub-headers) ─────────
#include "app_run.h"
#include "app_gfx.h"
#include "app_interact.h"
#include "app_menu.h"
#include "app_grid.h"
