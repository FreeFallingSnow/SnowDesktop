#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <string>
#include <vector>
#include <memory>

class Item;
class Slot;
enum class HitRegion;

enum class BarStyle { VBar, HBar };

class Container
{
public:
    virtual ~Container() = default;
    virtual std::wstring GetTitle() const = 0;

    // Build and cache slots. Returns stable references valid until InvalidateSlots.
    const std::vector<std::unique_ptr<Slot>>& GetSlots();
    void InvalidateSlots() { slotsValid_ = false; }
    virtual std::vector<std::unique_ptr<Slot>> BuildSlots() = 0;

    virtual void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) = 0;
    virtual void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) {}
    virtual void DrawContents(ID2D1DeviceContext* context);
    virtual RECT GetBounds() const = 0;
    virtual BarStyle GetInsertionStyle() const = 0;

    // ── Drag source ──────────────────────────────────────
    // Returns items currently selected within this container.
    // Pointers may reference temporaries owned by the container; valid until next call.
    virtual std::vector<Item*> GetSelectedItems() const { return {}; }

    // ── Hit testing ──────────────────────────────────────
    // Tests pt for a drop target within this container. Returns the hit region
    // and sets outSlot to the target slot (may be null for empty/trailing areas).
    virtual HitRegion HitTestDrag(POINT pt, Slot*& outSlot) = 0;

    // ── Drag hint ────────────────────────────────────────
    virtual std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const
    { (void)slot; (void)region; (void)sourceItems; (void)origin; (void)mods; return L""; }

    // ── Drop preview ─────────────────────────────────────
    virtual void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot,
        HitRegion region) { (void)ctx; (void)slot; (void)region; }

    // ── Post-drop cleanup hint ──────────────────────────
    // Returns true if the app should reload shell items after a drop on this container.
    virtual bool NeedsShellReloadAfterDrop() const { return false; }

protected:
    std::vector<std::unique_ptr<Slot>> cachedSlots_;
    bool slotsValid_ = false;
};

// ── GridContainer: 2D grid layout (only DesktopGrid uses this) ─
// Accepts any Item including Widgets.
class GridContainer : public Container
{
public:
    BarStyle GetInsertionStyle() const override { return BarStyle::VBar; }
};

// ── ListContainer: 1D list layout (all Widgets use this) ─
// Accepts regular Items only — does NOT accept Widgets.
// Three render modes chosen at runtime: Fixed / ScrollingIcon / ScrollingRow
class ListContainer : public Container
{
public:
    BarStyle GetInsertionStyle() const override { return BarStyle::HBar; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;

    enum class RenderMode { Fixed, ScrollingIcon, ScrollingRow };
    virtual RenderMode GetRenderMode() const { return RenderMode::Fixed; }

    // Subclasses override these to control layout
    virtual size_t GetSlotCount() const = 0;
    virtual int  GetItemHeight() const { return 32; }
    virtual int  GetItemWidth()  const { return 92; }
    virtual bool SingleColumn() const { return false; }
    virtual bool IncludeTrailingEmptySlot() const { return false; }
    virtual Item* GetSlotItem(size_t idx) const { return nullptr; }
};
