#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <string>
#include <vector>

class Container;
class Item;

enum class HitRegion
{
    None,
    Empty,       // slot is empty → "移动到此空位"
    SortBefore,  // insert before this slot (upper/left half)
    SortAfter,   // insert after this slot (lower/right half)
    Handoff,     // mouse on icon rect → "交给xx处理"
};

class Slot
{
public:
    Slot(Container* parent, RECT bounds, size_t index);

    Container* GetParent() const;
    RECT GetBounds() const;
    Item* GetItem() const;
    void SetItem(Item* item); // null = clear
    size_t GetIndex() const;
    bool IsEmpty() const;
    RECT GetIconRect() const;

    // Drop target (replaces DropZone hierarchy)
    HitRegion HitTest(POINT pt) const;
    std::wstring GetDropHint(HitRegion region, const std::vector<Item*>& sourceItems) const;
    void ExecuteDrop(HitRegion region, const std::vector<Item*>& sourceItems, Container* origin, int mods);
    void DrawDropIndicator(ID2D1DeviceContext* ctx, HitRegion region) const;

private:
    Container* parent_;
    RECT bounds_;
    size_t index_;
    Item* item_ = nullptr;
};
