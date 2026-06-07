/**
 * @file drop_model.h
 * @brief 拖放操作 (Drag-and-Drop) 的完整类型系统
 *
 * 本文件定义了桌面拖放操作所需的所有数据结构与枚举类型。
 * 整个拖放流程分为三个阶段：
 *   - 拖出阶段：DropPayload 对即将拖拽的条目进行分类；DragSourceList / DragSourceEntry
 *     追踪每个拖拽条目的来源信息（来源组件、原始位置等）。
 *   - 悬停阶段：DropPreviewList / DropLanding 规划条目将要落地的位置与方式。
 *   - 落地阶段：PendingLandingCache / PendingLandingEntry 缓存尚未执行的落地操作，
 *     用于延后执行（如跨组件移动需要先完成源侧删除再执行目标侧插入）。
 *
 * DropAction 枚举控制拖放行为的语义（移动 / 复制 / 链接），
 * DropSourceKind / DropTargetKind / DropLandingKind 分别描述源类型、目标类型和落地类型。
 */

#pragma once

#include "item.h"
#include "widget.h"
#include "types.h"
#include "utils.h"

#include <string>
#include <unordered_set>
#include <vector>

/**
 * @enum DropAction
 * @brief 拖放操作的语义类型
 *
 * 决定拖拽条目到达目标后的处理方式：
 *   - Move：  移动操作，条目从源移除并添加到目标（默认行为）。
 *   - Copy：  复制操作，条目在源保留并在目标创建副本。
 *   - Link：  链接操作，目标创建指向源的引用/快捷方式。
 *
 * 用户可通过键盘修饰键切换默认行为：
 *   Shift = Move，Ctrl = Copy，Alt = Link。
 */
enum class DropAction
{
    Move,
    Copy,
    Link,
};

/**
 * @brief 根据键盘修饰键推导拖放操作类型
 * @param mods            键盘修饰键标志位（Windows MK_* 常量组合）
 * @param defaultAction   未匹配任何修饰键时的默认操作（默认为 Move）
 * @return 匹配的 DropAction 枚举值
 *
 * 优先级：Alt > Ctrl > Shift。
 * 例如用户按住 Ctrl 拖拽时返回 DropAction::Copy。
 */
inline DropAction DropActionFromMods(int mods, DropAction defaultAction = DropAction::Move)
{
    if (mods & MK_ALT) return DropAction::Link;
    if (mods & MK_CONTROL) return DropAction::Copy;
    if (mods & MK_SHIFT) return DropAction::Move;
    return defaultAction;
}

/**
 * @brief 将拖放操作类型转换为对应的键盘修饰键标志
 * @param action 拖放操作类型
 * @return Windows MK_* 常量（MK_CONTROL / MK_ALT），Move 返回 0
 *
 * 用于在菜单或配置界面中展示应按下哪个修饰键以触发对应操作。
 */
inline int DropActionToMods(DropAction action)
{
    switch (action)
    {
    case DropAction::Copy: return MK_CONTROL;
    case DropAction::Link: return MK_ALT;
    case DropAction::Move:
    default:
        return 0;
    }
}

/**
 * @struct DropPayload
 * @brief 对即将拖拽的条目进行分类与信息提取
 *
 * 将拖拽源条目（Item* 列表）按类型归类，提取各类型特定的元数据，
 * 供后续的拖放预览与落地决策使用。
 *
 * 分类维度：
 *   - hasWidgets：       是否包含 Widget（小组件）。
 *   - hasDesktopIcons：  是否包含桌面图标（DesktopIcon）。
 *   - hasFolderEntries： 是否包含文件夹条目（FolderEntryIcon）。
 *   - hasExternalFiles： 是否包含外部文件（ExternalFileItem）。
 *
 * 辅助数据：
 *   - filePaths：    收集所有可提取的文件系统路径。
 *   - desktopKeys：  收集桌面图标的布局键（用于快速查重）。
 */
struct DropPayload
{
    std::vector<Item*> items;
    std::vector<std::wstring> filePaths;
    std::vector<std::wstring> desktopKeys;
    bool hasWidgets = false;
    bool hasDesktopIcons = false;
    bool hasFolderEntries = false;
    bool hasExternalFiles = false;

    /**
     * @brief 从源条目列表构造 DropPayload
     * @param sourceItems 拖拽的源条目列表
     * @return 填充完成的 DropPayload
     *
     * 遍历每个条目并依据其运行时类型进行分流处理：
     *   - Widget：仅标记 hasWidgets，不收集路径。
     *   - DesktopIcon：提取布局键（layoutKey）和真实文件路径，
     *     跳过 Clsid 类型的虚拟图标。
     *   - FolderEntryIcon / ExternalFileItem：分别标记对应标志，
     *     并收集文件路径。
     */
    static DropPayload From(const std::vector<Item*>& sourceItems)
    {
        DropPayload payload;
        payload.items = sourceItems;
        for (auto* src : sourceItems)
        {
            if (!src) continue;

            if (dynamic_cast<Widget*>(src))
            {
                payload.hasWidgets = true;
                continue;
            }

            if (auto* icon = dynamic_cast<DesktopIcon*>(src))
            {
                payload.hasDesktopIcons = true;
                DesktopItem* item = icon->GetDesktopItem();
                if (item && !item->layoutKey.empty())
                    payload.desktopKeys.push_back(ToUpperInvariant(item->layoutKey));

                std::wstring path = icon->GetPath();
                if (item && item->desktopIconClsid.empty() && !path.empty())
                    payload.filePaths.push_back(path);
                continue;
            }

            if (dynamic_cast<FolderEntryIcon*>(src))
                payload.hasFolderEntries = true;
            else if (dynamic_cast<ExternalFileItem*>(src))
                payload.hasExternalFiles = true;

            std::wstring path = src->GetPath();
            if (!path.empty())
                payload.filePaths.push_back(path);
        }
        return payload;
    }
};

/**
 * @enum DropSourceKind
 * @brief 拖拽条目的来源类型
 *
 * 用于 DragSourceEntry 中标记每个被拖拽项来自何处。
 * 不同来源在落地时可能需要不同的处理逻辑（例如桌面图标移到文件夹
 * 等价于文件移动，而 Widget 拖拽不涉及文件系统操作）。
 */
enum class DropSourceKind
{
    DesktopIcon,    ///< 来自桌面的图标（DesktopIcon）
    FolderEntry,    ///< 来自文件夹视图内部的条目
    ExternalFile,   ///< 来自桌面外部（如资源管理器拖入）的文件
    Widget,         ///< 来自 Widget 组件自身
    Unknown,        ///< 来源未知
};

/**
 * @enum DropTargetKind
 * @brief 拖放目标的类型
 *
 * 描述当前鼠标悬停的目标区域属于哪种容器。
 * 决定落地策略（例如拖到 KeyedWidget 按插入索引放置，
 * 拖到 FolderMapping 按文件路径处理）。
 */
enum class DropTargetKind
{
    Desktop,        ///< 桌面主区域（GridCell 定位）
    KeyedWidget,    ///< 带有布局键的 Widget 内部
    FolderMapping,  ///< 映射文件夹
    External,       ///< 桌面外部（如资源管理器窗口）
    Handoff,        ///< 移交给其他组件处理
    Unknown,        ///< 目标未知
};

/**
 * @enum DropLandingKind
 * @brief 条目落地方式
 *
 * DropLanding 使用此枚举描述每个被拖拽条目最终应如何放置。
 * 与 DropTargetKind 不同，LandingKind 关注的是"放置动作本身"，
 * 而非目标区域的类型。
 */
enum class DropLandingKind
{
    None,           ///< 未指定（不执行落地）
    DesktopCell,    ///< 放置到桌面的某个网格单元格
    WidgetIndex,    ///< 插入到 Widget 内部的指定索引位置
    Folder,         ///< 放置到某个文件夹中
    External,       ///< 放置到桌面外部（通常是跨进程拖放）
};

/**
 * @struct DragSourceEntry
 * @brief 单个拖拽条目的来源信息
 *
 * 记录被拖拽条目的来源类型、原始 Item 指针、在源容器中的索引、
 * 原始网格位置、显示名称等。拖放操作完成前，这些信息用于：
 *   - 在目标侧决定插入位置
 *   - 在源侧执行删除/移除操作时定位原始条目
 *   - 生成落地的回退方案（如原始位置恢复）
 */
struct DragSourceEntry
{
    DropSourceKind kind = DropSourceKind::Unknown;
    Item* item = nullptr;
    size_t sourceIndex = 0;
    size_t desktopIndex = static_cast<size_t>(-1);
    size_t memberIndex = static_cast<size_t>(-1);
    std::wstring desktopKey;
    std::wstring filePath;
    std::wstring displayName;
    GridCell originalCell;
    GridSpan originalSpan;
    bool protectedDesktopIcon = false;
};

/**
 * @struct DragSourceList
 * @brief 本次拖拽操作的全部来源信息
 *
 * 是 DragSourceEntry 的集合容器，同时记录源容器（Container* origin）的
 * 整体属性（是否来自 Widget、Widget 类型、包含哪些类型的条目等）。
 *
 * 提供两个便捷方法：
 *   - FilePaths()：  提取所有条目的文件路径列表。
 *   - DesktopKeys()：提取所有桌面图标的布局键列表。
 *
 * Empty() 可用于判断当前是否为空拖拽（无有效条目）。
 */
struct DragSourceList
{
    std::vector<DragSourceEntry> entries;
    Container* origin = nullptr;
    bool hasOriginWidget = false;
    std::wstring originWidgetId;
    DesktopWidgetType originWidgetType = DesktopWidgetType::Collection;
    bool hasDesktopIcons = false;
    bool hasFolderEntries = false;
    bool hasExternalFiles = false;
    bool hasWidgets = false;

    bool Empty() const { return entries.empty(); }

    /**
     * @brief 获取所有条目的文件路径
     * @return 非空文件路径列表
     */
    std::vector<std::wstring> FilePaths() const
    {
        std::vector<std::wstring> paths;
        for (const auto& entry : entries)
            if (!entry.filePath.empty())
                paths.push_back(entry.filePath);
        return paths;
    }

    /**
     * @brief 获取所有桌面图标的布局键
     * @return 非空布局键列表
     */
    std::vector<std::wstring> DesktopKeys() const
    {
        std::vector<std::wstring> keys;
        for (const auto& entry : entries)
            if (!entry.desktopKey.empty())
                keys.push_back(entry.desktopKey);
        return keys;
    }
};

/**
 * @struct DropLanding
 * @brief 单个条目的落地规划
 *
 * 记录某个被拖拽条目应落在哪里、以何种方式放置。
 * 由拖放预览阶段（如鼠标悬停时的实时布局计算）填充，
 * 最终传递给 PendingLandingCache 执行实际落地。
 */
struct DropLanding
{
    DropLandingKind kind = DropLandingKind::None;
    size_t sourceIndex = 0;
    GridCell cell;
    size_t insertIndex = 0;
    DesktopWidget* widget = nullptr;
    std::wstring widgetId;
};

/**
 * @struct DropPreviewList
 * @brief 一次拖放操作的全部落地规划
 *
 * 包含目标容器（targetContainer）、目标 Widget、锚点单元格等全局信息，
 * 以及每个条目的 DropLanding 列表。
 *
 * 系统在拖拽过程中实时更新此结构（随鼠标移动重新计算布局），
 * 并在拖放完成时将 landings 转换为 PendingLandingCache 执行落地。
 */
struct DropPreviewList
{
    DropTargetKind targetKind = DropTargetKind::Unknown;
    DropAction action = DropAction::Move;
    Container* targetContainer = nullptr;
    DesktopWidget* targetWidget = nullptr;
    GridCell anchorCell;
    size_t insertIndex = 0;
    bool fileBacked = false;
    std::vector<DropLanding> landings;

    bool Empty() const { return landings.empty(); }
};

/**
 * @struct PendingLandingEntry
 * @brief 待执行的单个落地操作
 *
 * 存储将某个条目实际放置到目标所需的所有参数。
 * 与 DropLanding 不同，此结构关注执行层面的细节：
 *   - sourcePath / sourceName：源文件路径和名称，用于文件级操作。
 *   - createdPath：落地后创建的文件路径（如文件复制结果）。
 *   - action：最终落地时采用的拖放操作类型（可能已变更）。
 *
 * 落地执行时使用这些参数在目标容器中创建对应的 Icon 或条目。
 */
struct PendingLandingEntry
{
    size_t sourceIndex = 0;
    DropAction action = DropAction::Move;
    DropLandingKind kind = DropLandingKind::None;
    std::wstring sourcePath;
    std::wstring sourceName;
    std::wstring createdPath;
    GridCell cell;
    size_t insertIndex = 0;
    DesktopWidget* widget = nullptr;
    std::wstring widgetId;
};

/**
 * @struct PendingLandingCache
 * @brief 挂起的落地操作缓存
 *
 * 用于在拖放操作的"提交"阶段之前暂存所有待执行的落地操作。
 * 典型使用场景：
 *   1. 用户从 Widget A 向 Widget B 拖拽条目。
 *   2. 系统先将落地操作写入 PendingLandingCache（此时不执行）。
 *   3. 源侧先执行条目移除（从 Widget A 删除）。
 *   4. 目标侧再读取缓存执行插入（添加到 Widget B）。
 *
 * 这种"先删除、后插入"的两阶段策略避免了跨组件移动时的状态不一致。
 *
 * existingDesktopKeys 用于在落地时快速判断目标桌面键是否已存在，
 * 避免创建重复的桌面图标。
 */
struct PendingLandingCache
{
    std::vector<PendingLandingEntry> entries;
    std::unordered_set<std::wstring> existingDesktopKeys;
    bool active = false;
    DWORD tick = 0;

    /**
     * @brief 清空缓存，重置到未激活状态
     */
    void Clear()
    {
        entries.clear();
        existingDesktopKeys.clear();
        active = false;
        tick = 0;
    }
};
