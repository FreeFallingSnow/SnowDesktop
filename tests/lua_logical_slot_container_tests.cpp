#include "widgets/lua_logical_slot.h"

#include "core/item.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TestItem final : public Item
{
public:
    std::wstring GetTitle() const override { return L"source"; }
    std::wstring GetPath() const override { return L"C:\\source.txt"; }
    HBITMAP GetIconBitmap() const override { return nullptr; }
    RECT GetBounds() const override { return {}; }
    void SetBounds(RECT) override {}
    bool IsSelected() const override { return false; }
    void SetSelected(bool) override {}
    Container* GetContainer() const override { return nullptr; }
    void Draw(ID2D1DeviceContext*, RECT, int) override {}
    Microsoft::WRL::ComPtr<IDataObject> CreateDataObject() override
    {
        return {};
    }
};

LogicalSlotHostSurface CollectionSurface(bool full = false)
{
    LogicalSlotHostSurface surface;
    surface.widgetId = L"widget-1";
    surface.slotId = "favorites";
    surface.kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    surface.capacity = full ? 2 : 3;
    surface.itemCount = 2;
    surface.accepts = { "desktop.item", "filesystem.reference" };
    surface.bounds = { 100, 100, 300, 300 };
    surface.items = {
        { "first", { 110, 110, 290, 150 } },
        { "second", { 110, 160, 290, 200 } },
    };
    return surface;
}

void TestCollectionHitAndCommitBoundary()
{
    auto surface = CollectionSurface();
    std::size_t committedIndex = 99;
    int commitCount = 0;
    LuaLogicalSlotContainer container(
        L"widget-1", "favorites",
        [&surface]() -> std::optional<LogicalSlotHostSurface> {
            return surface;
        },
        [&](const std::vector<Item*>& items, std::size_t index) {
            ++commitCount;
            committedIndex = index;
            return items.size() == 1;
        });

    Check(container.AcceptsDragPayload(
            snowdesktop::slot_contract::DragPayloadKind::DesktopItem, 1),
        "a collection must accept one declared desktop reference");
    Check(!container.AcceptsDragPayload(
            snowdesktop::slot_contract::DragPayloadKind::DesktopItem, 2),
        "native logical slots must reject ambiguous multi-item ingress");

    Slot* slot = nullptr;
    Check(container.HitTestDrag({ 150, 165 }, slot) ==
            HitRegion::SortBefore && slot && slot->GetIndex() == 1,
        "the leading half of a slotItem must expose its exact insert boundary");
    TestItem item;
    Check(container.CommitItems({ &item }, slot, HitRegion::SortBefore) &&
            commitCount == 1 && committedIndex == 1,
        "committing a slotItem boundary must preserve the host insert index");

    slot = nullptr;
    Check(container.HitTestDrag({ 150, 260 }, slot) == HitRegion::Empty &&
            slot && slot->GetIndex() == 2,
        "uncovered collection space must append through a trailing host slot");
    Check(container.HitTestDrag({ 20, 20 }, slot) == HitRegion::None,
        "logical slot geometry must not leak outside the committed surface");

    const auto firstHit = container.ItemAtPoint({ 150, 130 });
    Check(firstHit && firstHit->itemId == "first" &&
            firstHit->index == 0 && firstHit->itemCount == 2 &&
            firstHit->kind ==
                snowdesktop::widget_runtime::LogicalSlotKind::Collection &&
            firstHit->canRemove,
        "a committed slotItem must expose an exact host context-menu hit");
    Check(!container.ItemAtPoint({ 150, 260 }),
        "empty slotSurface space must not expose an item context menu");
}

void TestCapacityAndBindingPolicy()
{
    auto full = CollectionSurface(true);
    LuaLogicalSlotContainer fullContainer(
        L"widget-1", "favorites", [&full]() {
            return std::optional<LogicalSlotHostSurface>(full);
        }, {});
    Check(!fullContainer.AcceptsDragPayload(
            snowdesktop::slot_contract::DragPayloadKind::ExternalFile, 1),
        "a full collection must be rejected before native hit testing");

    LogicalSlotHostSurface binding;
    binding.widgetId = L"widget-1";
    binding.slotId = "primary";
    binding.kind = snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    binding.capacity = 1;
    binding.itemCount = 1;
    binding.allowClear = false;
    binding.replacePolicy = "reject";
    binding.accepts = { "app.reference" };
    binding.bounds = { 0, 0, 100, 100 };
    binding.items = { { "primary-item", binding.bounds } };
    LuaLogicalSlotContainer bindingContainer(
        L"widget-1", "primary", [&binding]() {
            return std::optional<LogicalSlotHostSurface>(binding);
        }, {});
    Check(!bindingContainer.AcceptsDragPayload(
            snowdesktop::slot_contract::DragPayloadKind::DesktopItem, 1),
        "a populated reject-policy binding must not advertise replacement");
    const auto bindingHit = bindingContainer.ItemAtPoint({ 50, 50 });
    Check(bindingHit && !bindingHit->canRemove,
        "a binding with allowClear=false must disable host removal");
}
}

int main()
{
    TestCollectionHitAndCommitBoundary();
    TestCapacityAndBindingPolicy();
    std::cout << "Lua logical slot container tests passed\n";
    return 0;
}
