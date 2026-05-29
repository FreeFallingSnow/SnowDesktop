#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <string>
#include <vector>
#include <memory>

class Item;
class Slot;
enum class HitRegion;

class Container
{
public:
    virtual ~Container() = default;
    virtual std::wstring GetTitle() const = 0;
    virtual std::vector<std::unique_ptr<Slot>> BuildSlots() = 0;
    virtual void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) = 0;
    virtual void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) {}
    virtual void DrawContents(ID2D1DeviceContext* context);
    virtual RECT GetBounds() const = 0;
};

// ── GridContainer: 2D grid layout (only DesktopGrid uses this) ─
// Accepts any Item including Widgets.
class GridContainer : public Container
{
public:
    // Default insertion style: VBar
};

// ── ListContainer: 1D list layout (all Widgets use this) ─
// Accepts regular Items only — does NOT accept Widgets.
// Three render modes chosen at runtime: Fixed / ScrollingIcon / ScrollingRow
class ListContainer : public Container
{
public:
    // Default insertion style: HBar

    enum class RenderMode { Fixed, ScrollingIcon, ScrollingRow };
    virtual RenderMode GetRenderMode() const { return RenderMode::Fixed; }
};
