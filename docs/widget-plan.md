# Widget 组件 OO 实现计划

## 架构原则

**app 层只做三件事**：
1. 把鼠标坐标传给正确的 Container
2. 把选中 items 打包传给目标 Container
3. 触发重绘

**所有具体逻辑由 Container 子类覆写虚方法实现**。app 层零 `dynamic_cast`、零 `switch(type)`、零 `if (onDesktop) else`。

---

## 关键接口扩展

### Container 新增虚方法

```cpp
class Container {
public:
    // ── 拖拽源 ──────────────────────────────────
    // 返回此容器内当前选中的 items (用于发起拖拽)
    virtual std::vector<Item*> GetSelectedItems() const { return {}; }

    // ── 命中检测 ────────────────────────────────
    // 拖拽命中检测：pt 离哪个 slot 最近、是什么区域
    virtual HitRegion HitTestDrag(POINT pt, Slot*& outSlot) = 0;

    // ── 拖拽提示 ────────────────────────────────
    virtual std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const = 0;

    // ── 拖拽预览 ────────────────────────────────
    virtual void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot,
        HitRegion region) {}

    // ── OnItemsDropped (已有) ────────────────────
    // 每个子类自行实现全部场景
};

class WidgetContainer : public Widget, public ListContainer {
public:
    // ── 获取成员 Item ───────────────────────────
    // 每个成员 slot 对应的 Item*(用于 hit/draw)
    virtual Item* GetMemberItem(size_t memberIndex) const { return nullptr; }
    // 选中的成员索引
    virtual std::vector<size_t> GetSelectedMemberIndices() const { return {}; }

    // ── 成员重排序 ──────────────────────────────
    virtual void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) {}
};
```

### Slot 新增：绑定来源信息

```cpp
class Slot {
public:
    // 这个 slot 对应的 Item (null = 空)
    Item* GetItem() const;
    void SetItem(Item*);
    // 在所属 Container 中的逻辑索引 (对应 memberIndex)
    size_t GetIndex() const;
};
```

Slot 已经存了 index 和 item 指针——这就是命中检测的全部信息。

---

## 三个 Container 的角色

### DesktopGrid

- `GetSelectedItems()` → 遍历 items_[] ，找 `selected=true` 的 DesktopItem → 创建临时 DesktopIcon 包装
- `HitTestDrag(pt, outSlot)` → `HitTestAtPoint` + `HitTestItem` 覆写 Handoff
- `GetDragHint()` → 和现在 `MakeDragHint` 逻辑相同，但移到类内
- `DrawDropPreview()` → 蓝色/红色圆角框
- `OnItemsDropped()` → Handoff → ShellExecute; Sort/Empty → CellFromPoint → Move/Copy/Link (按 mods); ExternalFileItem → delegate shell

### Collection (WidgetContainer)

- `GetSelectedItems()` → `GetSelectedMemberIndices()` → 找对应的 DesktopItem → 创建 DesktopIcon 临时包装
- `GetMemberItem(size_t idx)` → `itemKeys[idx]` → `FindItemIndexByKey` → 包装为 DesktopIcon
- `HitTestDrag(pt, outSlot)` → 遍历 `GetSlots()` 找 hit slot
- `OnItemsDropped(sourceItems, origin, targetSlot, region, mods)`:
  - origin == this → 重排序 key
  - origin == another Widget → 从 origin 移除 key, 插入自己
  - origin == DesktopGrid → 收集 sourceItems 的 layoutKey → 插入 itemKeys
  - mods 无影响 (纯内存 key 操作)

### FileCategories (WidgetContainer)

- 同 Collection 但 key 插入后按文件类型自动分类
- `GetSelectedItems()` → 按 `activeCategoryId` 筛选
- `OnItemsDropped` → 同 Collection + 分类逻辑

### FolderMapping (WidgetContainer)

- `GetSelectedItems()` → `GetSelectedMemberIndices()` → 对应的 FolderEntry → 创建 ExternalFileItem
- `GetMemberItem()` → `folderEntries[idx]` → 创建 ExternalFileItem
- `OnItemsDropped()`:
  - origin == this → `ReorderMembers(indices, insertBefore)` (重排 folderEntries)
  - origin != this → `CopyFilesToFolder` (+ mods 决定 move/copy/link) → `RefreshFolderMapping`

---

## app 层简化为胶水代码

### 拖拽发起 (OnMouseMove 首次超过阈值)

```cpp
// mouseDownHit_ 指向命中的 DesktopIcon* 或 Widget* 或 Widget member slot
// 找到其所属 Container
dragSource_ = mouseDownHit_->GetContainer();  // DesktopGrid or WidgetContainer
dragSourceItems_ = dragSource_->GetSelectedItems();
```

### 命中检测 (OnMouseMove 拖拽中)

```cpp
// 遍历所有 Container (Widget 优先, DesktopGrid 兜底)
for (auto& c : containers_) {
    Slot* slot;
    HitRegion region = c->HitTestDrag(pt, slot);
    if (region != None) {
        dragTargetContainer_ = c;
        dragTargetSlot_ = slot;
        dragTargetRegion_ = region;
        break;
    }
}
// hint
if (dragTargetContainer_)
    dragHint_ = dragTargetContainer_->GetDragHint(
        dragTargetSlot_, dragTargetRegion_, dragSourceItems_, dragSource_, mods);
```

### 落点执行 (OnLeftButtonUp)

```cpp
if (dragTargetContainer_) {
    // Handoff region → Shell OLE (仍在 app 层, 因为涉及 COM)
    if (dragTargetRegion_ == Handoff) {
        ShellHandoff(dragCurrentPoint_);
    } else {
        dragTargetContainer_->OnItemsDropped(
            dragSourceItems_, dragSource_, dragTargetSlot_, dragTargetRegion_, mods);
    }
}
```

### 渲染 (RenderFrame)

```cpp
for (auto& c : containers_)
    c->DrawChrome(ctx, mousePt);
// 拖拽偏移绘制
for (auto* item : dragSourceItems_)
    item->Draw(ctx, OffsetRect(item->GetBounds(), delta), state=3);
// 预览
if (dragTargetContainer_)
    dragTargetContainer_->DrawDropPreview(ctx, dragTargetSlot_, dragTargetRegion_);
```

---

## 修饰键规则

所有 `OnItemsDropped` 接收 `int mods` (MK_CONTROL|MK_ALT|MK_SHIFT)：

| 场景 | mods 影响 |
|------|----------|
| DesktopGrid 接收 DesktopIcon | Ctrl=复制 Alt=快捷方式 否则=移动 |
| Collection/FC 接收 DesktopIcon | **无影响** (纯内存 key) |
| FolderMapping 接收文件 | Ctrl=复制 Alt=快捷方式 否则=移动 |
| DesktopGrid 接收 FolderMapping 条目 | 同上 |

---

## 实施步骤

### Step 1: 完善 Container 虚方法
- `Container::GetSelectedItems()`, `HitTestDrag()`, `GetDragHint()`, `DrawDropPreview()`
- `WidgetContainer::GetMemberItem()`, `GetSelectedMemberIndices()`, `ReorderMembers()`

### Step 2: 重构 app 拖拽胶水
- `dragSource_` / `dragSourceItems_` 替代分散的 `mouseDownHit_` / 旧 drag 变量
- `OnMouseMove` 统一遍历 Container 做 hit
- `OnLeftButtonUp` 统一调用 `dragTargetContainer_->OnItemsDropped()`

### Step 3: 逐个实现 Container 逻辑
- DesktopGrid (已有大部分, 补充 hint/preview/drop)
- Collection (itemKeys 操作)
- FileCategories (itemKeys + 分类)
- FolderMapping (folderEntries + 文件操作)

### Step 4: Widget chrome + 内容渲染
- 标题、悬浮底栏、Collection 1×1/2×2+、弹窗、FileCategories Tab、FolderMapping 按钮

### Step 5: 右键菜单 + 重命名 + 删除
