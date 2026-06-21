/**
 * @file item.cpp
 * @brief 桌面图标项绘制、数据对象创建与属性访问的实现
 *
 * 该文件实现了 DesktopIcon、FolderEntryIcon 和 ExternalFileItem 三个 Item 子类，
 * 分别对应桌面原生图标、文件夹内条目图标和外部文件图标。
 * 核心功能包括：图标与文本的 Direct2D 绘制、拖拽状态与选中状态的视觉反馈、
 * 以及标题、路径、边界等属性访问。
 */

#include "item.h"
#include "types.h"
#include "app.h"

// ── DesktopIcon ──────────────────────────────────────────────

/**
 * @brief 构造函数
 * @param item      关联的桌面原生数据项，不能为空
 * @param container 所属的容器对象
 * @param app       DesktopApp 实例指针，用于访问绘制工具与缓存
 */
DesktopIcon::DesktopIcon(DesktopItem* item, Container* container, DesktopApp* app)
    : item_(item), container_(container), app_(app) {}

/**
 * @brief  获取图标标题
 * @return 桌面项的 name 字段；若 item_ 为空则返回空字符串
 */
std::wstring DesktopIcon::GetTitle() const { return item_ ? item_->name : L""; }

/**
 * @brief  获取图标对应路径
 * @return 桌面项的 parsingName（COM 解析名）；item_ 为空时返回空字符串
 */
std::wstring DesktopIcon::GetPath() const { return item_ ? item_->parsingName : L""; }

/**
 * @brief  获取图标的 HBITMAP 句柄
 * @return 桌面项的 iconBitmap；可能为 nullptr
 */
HBITMAP DesktopIcon::GetIconBitmap() const { return item_ ? item_->iconBitmap : nullptr; }

/**
 * @brief  获取图标绘制的边界矩形
 * @return 若已通过 SetBounds 设置了覆盖值则返回覆盖值；
 *         否则返回桌面项的原始 bounds；item_ 为空时返回空 RECT
 */
RECT DesktopIcon::GetBounds() const
{
    if (hasBoundsOverride_) return boundsOverride_;
    return item_ ? item_->bounds : RECT{};
}

/**
 * @brief 覆盖设置图标边界矩形
 * @param bounds 新的边界矩形
 *
 * 设置后 GetBounds 将返回此覆盖值而非 item_->bounds，
 * 适用于拖拽过程中临时改变位置。
 */
void DesktopIcon::SetBounds(RECT bounds)
{
    boundsOverride_ = bounds;
    hasBoundsOverride_ = true;
}

/**
 * @brief  判断图标是否处于选中状态
 * @return 桌面项的 selected 标志；item_ 为空时返回 false
 */
bool DesktopIcon::IsSelected() const { return item_ && item_->selected; }

/**
 * @brief 设置图标的选中状态
 * @param selected 是否选中
 */
void DesktopIcon::SetSelected(bool selected) { if (item_) item_->selected = selected; }

/**
 * @brief  获取图标所属的容器指针
 * @return 构造函数传入的 container_
 */
Container* DesktopIcon::GetContainer() const { return container_; }

/**
 * @brief 使用 Direct2D 绘制桌面图标
 * @param context D2D 设备上下文
 * @param rect    图标所在矩形区域
 * @param state   视觉状态：0=默认，1=悬停，2=选中，3=拖拽中
 *
 * 绘制顺序：
 *   1. 悬停背景（半透明白色圆角矩形）
 *   2. 选中背景（灰色圆角矩形）
 *   3. 图标位图（支持剪切透明度与拖拽透明度叠加）
 *   4. 文字标签（拖拽状态下不绘制）
 */
void DesktopIcon::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || !item_) return;
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;

    const bool hovered = (state == 1);
    const bool selected = (state == 2 || state == 3);
    const bool dragged = (state == 3);
    const float cutOpacity = item_->isCut ? 0.4f : 1.0f;
    const float dragOpacity = dragged ? 0.6f : 1.0f;
    const float alpha = dragOpacity * cutOpacity;

    if (hovered && !selected)
    {
        app_->DrawD2DRoundedRectangle(context, rect,
            6.0f * app_->GetItemLayoutScale(rect),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * alpha),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * alpha));
    }

    RECT iconRect = app_->GetItemIconRect(rect);

    if (selected && !dragged)
    {
        RECT sel = app_->GetItemSelectionRect(rect, true);
        app_->DrawD2DRoundedRectangle(context, sel,
            6.0f * app_->GetItemLayoutScale(rect),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f * alpha),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f * alpha));
    }

    if (item_->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(item_->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    if (!dragged)
        app_->DrawItemText(context, rect, item_->name, selected, alpha);
}

/**
 * @brief 创建用于拖放操作的数据对象
 * @return ComPtr 包装的 IDataObject；当前实现返回 nullptr
 *
 * @note 桌面原生图标暂未实现拖放数据对象的创建，后续可扩展
 *       以支持从桌面拖出文件的场景。
 */
ComPtr<IDataObject> DesktopIcon::CreateDataObject()
{
    return nullptr;
}

// ── FolderEntryIcon ─────────────────────────────────────────

/**
 * @brief 构造函数
 * @param entry     文件夹条目对象指针
 * @param container 所属容器
 * @param app       DesktopApp 实例
 */
FolderEntryIcon::FolderEntryIcon(FolderEntry* entry, Container* container, DesktopApp* app)
    : entry_(entry), container_(container), app_(app) {}

/**
 * @brief  获取文件夹条目的标题
 * @return entry_ 的 name 字段；entry_ 为空时返回空字符串
 */
std::wstring FolderEntryIcon::GetTitle() const { return entry_ ? entry_->name : L""; }

/**
 * @brief  获取文件夹条目的完整路径
 * @return entry_ 的 fullPath；entry_ 为空时返回空字符串
 */
std::wstring FolderEntryIcon::GetPath() const { return entry_ ? entry_->fullPath : L""; }

/**
 * @brief  获取文件夹条目的图标位图句柄
 * @return entry_ 的 iconBitmap；可能为 nullptr
 */
HBITMAP FolderEntryIcon::GetIconBitmap() const { return entry_ ? entry_->iconBitmap : nullptr; }

/**
 * @brief  获取图标边界矩形（直接返回本地缓存的 bounds_）
 * @return 当前 bounds_ 值
 */
RECT FolderEntryIcon::GetBounds() const { return bounds_; }

/**
 * @brief 设置图标边界矩形
 * @param bounds 新的边界矩形
 */
void FolderEntryIcon::SetBounds(RECT bounds) { bounds_ = bounds; }

/**
 * @brief  判断当前条目是否被选中
 * @return entry_ 的 selected 标志；entry_ 为空时返回 false
 */
bool FolderEntryIcon::IsSelected() const { return entry_ && entry_->selected; }

/**
 * @brief 设置当前条目的选中状态
 * @param selected 是否选中
 */
void FolderEntryIcon::SetSelected(bool selected) { if (entry_) entry_->selected = selected; }

/**
 * @brief  获取所属容器指针
 * @return 构造函数传入的 container_
 */
Container* FolderEntryIcon::GetContainer() const { return container_; }

/**
 * @brief 使用 Direct2D 绘制文件夹条目图标
 * @param context D2D 设备上下文
 * @param rect    图标所在矩形区域
 * @param state   视觉状态：0=默认，1=悬停，2=选中，3=拖拽中
 *
 * 与 DesktopIcon::Draw 的区别：
 *   - 选中背景使用 DrawD2DFilledRectangle（实心矩形）而非圆角矩形
 *   - 透明度计算不区分剪切与拖拽，统一取两者中的生效值
 */
void FolderEntryIcon::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || !entry_) return;
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;

    const bool hovered = (state == 1);
    const bool selected = (state == 2 || state == 3);
    const bool dragged = (state == 3);
    const float opacity = dragged ? 0.6f : (entry_->isCut ? 0.4f : 1.0f);

    if (hovered && !selected)
    {
        app_->DrawD2DRoundedRectangle(context, rect,
            6.0f * app_->GetItemLayoutScale(rect),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * opacity),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * opacity));
    }

    RECT iconRect = app_->GetItemIconRect(rect);

    if (selected && !dragged)
    {
        app_->DrawD2DFilledRectangle(context,
            app_->GetItemSelectionRect(rect, true),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f * opacity),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f * opacity));
    }

    if (entry_->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(entry_->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    if (!dragged)
        app_->DrawItemText(context, rect, entry_->name, selected, opacity);
}

/**
 * @brief 创建文件夹条目的拖放数据对象
 * @return ComPtr 包装的 IDataObject；当前实现返回 nullptr
 *
 * @note 文件夹内部条目的拖放尚未实现，留待后续扩展。
 */
ComPtr<IDataObject> FolderEntryIcon::CreateDataObject()
{
    return nullptr;
}

// ── ExternalFileItem ─────────────────────────────────────────

/**
 * @brief 构造函数，从文件路径解析标题
 * @param filePath 外部文件的绝对路径
 *
 * 自动从路径末尾提取文件名作为标题。
 */
ExternalFileItem::ExternalFileItem(const std::wstring& filePath)
    : path_(filePath)
{
    size_t pos = path_.find_last_of(L"\\/");
    title_ = (pos != std::wstring::npos) ? path_.substr(pos + 1) : path_;
}

/**
 * @brief  获取文件标题（不含路径的文件名）
 * @return 从路径末尾提取的文件名
 */
std::wstring ExternalFileItem::GetTitle() const { return title_; }

/**
 * @brief  获取文件的完整路径
 * @return 构造函数传入的原始路径
 */
std::wstring ExternalFileItem::GetPath() const { return path_; }

/**
 * @brief  获取图标位图句柄（未实现）
 * @return 始终返回 nullptr
 */
HBITMAP ExternalFileItem::GetIconBitmap() const { return nullptr; }

/**
 * @brief  获取边界矩形（未实现）
 * @return 始终返回空 RECT
 */
RECT ExternalFileItem::GetBounds() const { return {}; }

/**
 * @brief 设置边界矩形（空操作）
 * @param 未使用的 RECT 参数
 *
 * @note ExternalFileItem 不参与布局，此方法无实际效果。
 */
void ExternalFileItem::SetBounds(RECT) {}

/**
 * @brief  判断是否被选中（未实现）
 * @return 始终返回 false
 */
bool ExternalFileItem::IsSelected() const { return false; }

/**
 * @brief 设置选中状态（空操作）
 * @param 未使用的布尔参数
 */
void ExternalFileItem::SetSelected(bool) {}

/**
 * @brief  获取容器指针（未实现）
 * @return 始终返回 nullptr
 */
Container* ExternalFileItem::GetContainer() const { return nullptr; }

/**
 * @brief 绘制图标（未实现）
 *
 * @note ExternalFileItem 仅为文件路径的轻量包装，不参与实际绘制。
 */
void ExternalFileItem::Draw(ID2D1DeviceContext*, RECT, int) {}

/**
 * @brief 创建拖放数据对象（未实现）
 * @return 始终返回 nullptr
 */
ComPtr<IDataObject> ExternalFileItem::CreateDataObject() { return nullptr; }
