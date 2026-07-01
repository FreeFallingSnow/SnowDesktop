/**
 * @file item.h
 * @brief 可拖拽/可渲染项的抽象多态体系
 *
 * 本文件定义了桌面系统中所有可拖拽、可渲染项的抽象基类 Item，
 * 以及三个具体实现：
 *   - DesktopIcon    ：包装 DesktopItem（非拥有指针），用于桌面图标
 *   - FolderEntryIcon：包装 FolderEntry，用于文件夹条目图标
 *   - ExternalFileItem：包装外部拖拽进来的文件路径，轻量传输对象
 *
 * 整个体系围绕 Item 提供的纯虚接口构建，Container 通过基类指针
 * 统一管理渲染、选中、拖拽等行为。
 */

#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

class Container;
class DesktopApp;

/**
 * @class Item
 * @brief 所有可拖拽/可渲染项的抽象多态基类
 *
 * Item 定义了桌面系统中一个 "项" 的最小公共接口。
 * 任何能在桌面上被渲染、被选中、被拖拽的实体都通过此基类操作。
 * Container 持有 Item* 的集合，不关心具体派生类型。
 *
 * 子类必须实现以下行为：
 *   - 标题与路径（GetTitle / GetPath）
 *   - 图标位图（GetIconBitmap）
 *   - 边界矩形（GetBounds / SetBounds）
 *   - 选中状态（IsSelected / SetSelected）
 *   - 容器归属（GetContainer）
 *   - 自绘制（Draw）
 *   - 拖拽数据（CreateDataObject）
 */
class Item
{
public:
    virtual ~Item() = default;

    /** @brief 获取项的显示标题 */
    virtual std::wstring GetTitle() const = 0;

    /** @brief 获取项的文件系统路径（可用于拖拽或打开） */
    virtual std::wstring GetPath() const = 0;

    /** @brief 获取项的图标位图句柄 */
    virtual HBITMAP GetIconBitmap() const = 0;

    /** @brief 获取项的边界矩形（像素坐标，相对于桌面） */
    virtual RECT GetBounds() const = 0;

    /** @brief 设置项的边界矩形 */
    virtual void SetBounds(RECT bounds) = 0;

    /** @brief 判断项当前是否处于选中状态 */
    virtual bool IsSelected() const = 0;

    /** @brief 设置项的选中状态 */
    virtual void SetSelected(bool selected) = 0;

    /** @brief 获取项所属的容器指针，无容器时返回 nullptr */
    virtual Container* GetContainer() const = 0;

    /**
     * @brief 绘制项到指定设备上下文
     * @param context Direct2D 设备上下文
     * @param rect    绘制区域（像素坐标）
     * @param state   绘制状态：0=正常, 1=悬停, 2=选中, 3=拖拽中
     */
    virtual void Draw(ID2D1DeviceContext* context, RECT rect, int state) = 0;

    /** @brief 创建用于拖拽操作的数据传输对象（OLE IDataObject） */
    virtual ComPtr<IDataObject> CreateDataObject() = 0;
};

/** @struct DesktopItem
 *  @brief 桌面项数据模型（完整定义见 types.h） */
struct DesktopItem;

/** @struct FolderEntry
 *  @brief 文件夹条目数据模型（完整定义见 types.h） */
struct FolderEntry;

/**
 * @class DesktopIcon
 * @brief 包装 DesktopItem 的 Item 适配器（非拥有指针）
 *
 * DesktopIcon 不拥有所指向的 DesktopItem 的生命周期。
 * 它只是为已有桌面数据模型提供 Item 多态接口，使得 Container
 * 可以统一管理桌面图标。
 *
 * 除 Item 接口外，额外提供：
 *   - GetDesktopItem() —— 访问原始 DesktopItem 指针
 *   - GetApp()         —— 访问 DesktopApp 实例
 */
class DesktopIcon : public Item
{
public:
    explicit DesktopIcon(DesktopItem* item, Container* container = nullptr, DesktopApp* app = nullptr);
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    void Draw(ID2D1RenderTarget* context, RECT rect, int state, bool lightTheme = false,
        bool drawText = true, bool quickNavLayout = false);
    ComPtr<IDataObject> CreateDataObject() override;
    DesktopItem* GetDesktopItem() const { return item_; }
    DesktopApp* GetApp() const { return app_; }

private:
    DesktopItem* item_;              /**< 指向底层 DesktopItem 的非拥有指针 */
    Container* container_;           /**< 项所属的容器 */
    DesktopApp* app_;                /**< 桌面应用实例指针 */
    RECT boundsOverride_{};          /**< 覆盖的边界矩形（当 hasBoundsOverride_=true 时生效） */
    bool hasBoundsOverride_ = false; /**< 是否启用边界矩形覆盖 */
};

/**
 * @class FolderEntryIcon
 * @brief 包装 FolderEntry 的 Item 适配器
 *
 * 用于在桌面或容器中展示文件夹内的子条目。
 * 与 DesktopIcon 类似，FolderEntryIcon 不拥有所指向的 FolderEntry
 * 的生命周期。
 *
 * 额外提供：
 *   - GetFolderEntry() —— 访问原始 FolderEntry 指针
 *   - GetApp()         —— 访问 DesktopApp 实例
 */
class FolderEntryIcon : public Item
{
public:
    explicit FolderEntryIcon(FolderEntry* entry, Container* container = nullptr, DesktopApp* app = nullptr);
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    void Draw(ID2D1RenderTarget* context, RECT rect, int state, bool lightTheme = false,
        bool drawText = true, bool quickNavLayout = false);
    ComPtr<IDataObject> CreateDataObject() override;
    FolderEntry* GetFolderEntry() const { return entry_; }
    DesktopApp* GetApp() const { return app_; }

private:
    FolderEntry* entry_; /**< 指向底层 FolderEntry 的非拥有指针 */
    Container* container_; /**< 项所属的容器 */
    DesktopApp* app_;      /**< 桌面应用实例指针 */
    RECT bounds_{};        /**< 边界矩形（像素坐标） */
};

/**
 * @class ExternalFileItem
 * @brief 包装外部拖拽文件路径的轻量 Item
 *
 * 当用户从桌面外部（如资源管理器）拖拽文件到应用窗口时，
 * OnItemsDropped 会创建 ExternalFileItem 作为拖拽源。
 * 该类型只携带文件路径和标题，不关联任何持久数据模型。
 *
 * 仅有 GetPath() / GetTitle() 有实际意义，其余接口返回默认值。
 * 生命周期由拖拽事件的发起方管理，用后即销毁。
 */
class ExternalFileItem : public Item
{
public:
    explicit ExternalFileItem(const std::wstring& filePath);
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT) override;
    bool IsSelected() const override;
    void SetSelected(bool) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext*, RECT, int) override;
    ComPtr<IDataObject> CreateDataObject() override;

private:
    std::wstring path_;  /**< 文件完整路径 */
    std::wstring title_; /**< 文件显示标题（不含路径的文件名） */
};
