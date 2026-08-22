#include "widgets/lua_logical_slot.h"
#include "logical_slot_picker_rules.h"
#include "logical_slot_pointer_rules.h"
#include "logical_slot_keyboard_rules.h"

#include "core/item.h"
#include "core/drag_session.h"

#include <array>
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
    surface.revision = 1;
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

void TestLuaDragFeedbackUsesCommittedSurfaceIdentity()
{
    auto surface = CollectionSurface();
    LuaLogicalSlotContainer container(
        L"widget-1", "favorites",
        [&surface]() -> std::optional<LogicalSlotHostSurface> {
            return surface;
        }, {});
    DragSession session;
    session.Begin(nullptr, {}, {}, POINT{}, POINT{});

    Slot* first = nullptr;
    const HitRegion firstRegion =
        container.HitTestDrag({150, 165}, first);
    Check(first && session.UpdateTarget(
            &container, first, firstRegion),
        "the first Lua logical target must publish feedback");
    const std::uint64_t firstPresentation =
        session.PresentationRevision();
    const std::uint64_t firstGeneration =
        container.GetSlotGeneration();

    Slot* equivalent = nullptr;
    const HitRegion equivalentRegion =
        container.HitTestDrag({150, 165}, equivalent);
    Check(container.GetSlotGeneration() != firstGeneration,
        "Lua hit testing currently rebuilds its cached Slot generation");
    Check(!session.UpdateTarget(
            &container, equivalent, equivalentRegion) &&
            session.PresentationRevision() == firstPresentation &&
            session.TargetSlot() == equivalent,
        "an unchanged committed Lua surface must keep feedback stable while rebinding the latest Slot");

    ++surface.revision;
    Slot* revised = nullptr;
    const HitRegion revisedRegion =
        container.HitTestDrag({150, 165}, revised);
    Check(session.UpdateTarget(
            &container, revised, revisedRegion),
        "a committed Lua surface revision must refresh feedback");

    surface.items[1].bounds.left += 5;
    Slot* moved = nullptr;
    const HitRegion movedRegion =
        container.HitTestDrag({150, 165}, moved);
    Check(session.UpdateTarget(
            &container, moved, movedRegion),
        "a Lua target geometry change must refresh feedback without pointer identity");

    surface.bounds.right += 10;
    Slot* resizedSurface = nullptr;
    const HitRegion resizedRegion =
        container.HitTestDrag({150, 165}, resizedSurface);
    Check(session.UpdateTarget(
            &container, resizedSurface, resizedRegion),
        "a Lua slotSurface bounds change must refresh feedback even when the target item is unchanged");
}

void TestHostPickerCandidatePolicy()
{
    namespace picker = snowdesktop::logical_slot_picker_rules;
    const std::vector<std::string> all{
        "desktop.item", "app.reference", "filesystem.reference" };
    Check(picker::DesktopCandidateKind(all, true, true) ==
            "app.reference",
        "an application shortcut must prefer the declared app reference");
    Check(picker::DesktopCandidateKind(all, false, true) ==
            "desktop.item",
        "an ordinary desktop object must prefer the declared desktop reference");
    Check(picker::DesktopCandidateKind(
            { "filesystem.reference" }, false, true) ==
            "filesystem.reference",
        "a path-backed desktop object may fall back to a filesystem reference");
    Check(picker::DesktopCandidateKind(
            { "app.reference" }, false, true).empty(),
        "a non-application must be filtered from an app-only host picker");
    Check(picker::DesktopCandidateKind(
            { "filesystem.reference" }, false, false).empty(),
        "a namespace-only object must not become a filesystem reference");
    Check(picker::MatchesType({}, "file") &&
            picker::MatchesType("file", "file") &&
            !picker::MatchesType("folder", "file"),
        "settings reference pickers must enforce optional file/folder filters");
    Check(picker::NormalizeApplicationLaunchTarget(
            L"Contoso.Player_123!App") ==
            L"shell:AppsFolder\\Contoso.Player_123!App" &&
            picker::NormalizeApplicationLaunchTarget(
                L"SHELL:AppsFolder\\Contoso.Player_123!App") ==
                L"SHELL:AppsFolder\\Contoso.Player_123!App" &&
            picker::NormalizeApplicationLaunchTarget(
                L"C:\\Apps\\Player.exe") == L"C:\\Apps\\Player.exe" &&
            picker::NormalizeApplicationLaunchTarget(
                L"\\\\server\\Apps\\Player.exe") ==
                L"\\\\server\\Apps\\Player.exe",
        "application references must share one launch target normalization policy");
    Check(picker::IsFilesystemApplicationLaunchTarget(
            L"C:\\Users\\Example\\Desktop\\Player.lnk") &&
            picker::IsFilesystemApplicationLaunchTarget(
                L"\\\\server\\Apps\\Player.lnk") &&
            !picker::IsFilesystemApplicationLaunchTarget(
                L"shell:AppsFolder\\Contoso.Player_123!App") &&
            !picker::IsFilesystemApplicationLaunchTarget(
                L"Contoso.Player_123!App"),
        "filesystem-backed app references must bypass catalog availability checks");
}

void TestPointerReorderTargets()
{
    using snowdesktop::widget_runtime::ResolveLogicalSlotInsertionTarget;
    using snowdesktop::widget_runtime::ResolveLogicalSlotPointerTarget;
    const RECT surface{ 100, 100, 300, 300 };
    const std::vector<RECT> vertical{
        { 110, 110, 290, 150 },
        { 110, 160, 290, 200 },
        { 110, 210, 290, 250 },
    };
    const auto moveDown = ResolveLogicalSlotPointerTarget(
        vertical, 0, POINT{ 150, 190 }, surface);
    Check(moveDown && moveDown->insertionIndex == 2 &&
            moveDown->targetIndex == 1 && !moveDown->horizontal &&
            moveDown->indicator.top == 208,
        "dragging below another vertical item must account for source removal");
    const auto moveUp = ResolveLogicalSlotPointerTarget(
        vertical, 2, POINT{ 150, 115 }, surface);
    Check(moveUp && moveUp->insertionIndex == 0 &&
            moveUp->targetIndex == 0 && moveUp->indicator.top == 108,
        "dragging above the first vertical item must target index zero");

    const std::vector<RECT> horizontal{
        { 110, 110, 150, 190 },
        { 160, 110, 200, 190 },
        { 210, 110, 250, 190 },
    };
    const auto moveRight = ResolveLogicalSlotPointerTarget(
        horizontal, 0, POINT{ 245, 150 }, surface);
    Check(moveRight && moveRight->horizontal &&
            moveRight->insertionIndex == 3 &&
            moveRight->targetIndex == 2 &&
            moveRight->indicator.left == 248,
        "horizontal pointer reorder must expose a trailing vertical indicator");
    Check(!ResolveLogicalSlotPointerTarget(
            std::span<const RECT>(horizontal.data(), 1),
            0, POINT{ 120, 120 }, surface),
        "a one-item collection must not enter pointer reorder mode");

    const auto emptyTarget = ResolveLogicalSlotInsertionTarget(
        {}, POINT{ 150, 150 }, surface);
    Check(emptyTarget && emptyTarget->insertionIndex == 0 &&
            emptyTarget->targetIndex == 0 &&
            emptyTarget->indicator.top >= surface.top,
        "an empty collection must expose a bounded cross-slot insertion target");
    const std::array<RECT, 1> single{{ { 110, 110, 290, 150 } }};
    const auto appendTarget = ResolveLogicalSlotInsertionTarget(
        single, POINT{ 150, 190 }, surface);
    Check(appendTarget && appendTarget->insertionIndex == 1 &&
            appendTarget->targetIndex == 1 &&
            appendTarget->indicator.top == 148,
        "a cross-slot drop below one item must append after it");
    Check(!ResolveLogicalSlotInsertionTarget(
            single, POINT{ 50, 50 }, surface),
        "a cross-slot insertion target must stay inside its surface");
}

void TestKeyboardFocusRules()
{
    using namespace snowdesktop::widget_runtime;
    Check(EnterWidgetKeyboardFocus(
                3, std::nullopt, false, false, false) == 0 &&
            !EnterWidgetKeyboardFocus(
                0, std::nullopt, false, false, false) &&
            !EnterWidgetKeyboardFocus(3, 1, false, false, false) &&
            !EnterWidgetKeyboardFocus(
                3, std::nullopt, true, false, false) &&
            !EnterWidgetKeyboardFocus(
                3, std::nullopt, false, true, false) &&
            !EnterWidgetKeyboardFocus(
                3, std::nullopt, false, false, true),
        "plain non-repeated Enter must enter an unfocused widget at its first element");
    Check(BeginAuxiliarySurfaceKeyboardFocus(3, true, false) == 0 &&
            !BeginAuxiliarySurfaceKeyboardFocus(0, true, false) &&
            !BeginAuxiliarySurfaceKeyboardFocus(3, false, false) &&
            !BeginAuxiliarySurfaceKeyboardFocus(3, true, true),
        "an opened auxiliary surface must focus its first element exactly once");
    Check(CycleLogicalSlotFocus(3, std::nullopt, false) == 0 &&
            CycleLogicalSlotFocus(3, std::nullopt, true) == 2 &&
            CycleLogicalSlotFocus(3, 2, false) == 0 &&
            CycleLogicalSlotFocus(3, 0, true) == 2 &&
            !CycleLogicalSlotFocus(0, std::nullopt, false),
        "Tab focus must initialize, wrap, and handle an empty slot surface");
    Check(MoveLogicalSlotItemTarget(3, 1, -1) == 0 &&
            MoveLogicalSlotItemTarget(3, 1, 1) == 2 &&
            !MoveLogicalSlotItemTarget(3, 0, -1) &&
            !MoveLogicalSlotItemTarget(3, 2, 1),
        "keyboard reorder must stop at collection boundaries");

    const std::vector<LogicalSlotFocusRect> grid{
        { 0, 0, 80, 40 },
        { 100, 0, 180, 40 },
        { 0, 60, 80, 100 },
        { 100, 60, 180, 100 },
    };
    Check(FindLogicalSlotSpatialFocus(
            grid, 0, LogicalSlotFocusDirection::Right) == 1 &&
            FindLogicalSlotSpatialFocus(
                grid, 0, LogicalSlotFocusDirection::Down) == 2 &&
            FindLogicalSlotSpatialFocus(
                grid, 3, LogicalSlotFocusDirection::Left) == 2 &&
            FindLogicalSlotSpatialFocus(
                grid, 3, LogicalSlotFocusDirection::Up) == 1 &&
            !FindLogicalSlotSpatialFocus(
                grid, 0, LogicalSlotFocusDirection::Left),
        "arrow focus must follow spatial rows and columns without wrapping");
}
}

int main()
{
    TestCollectionHitAndCommitBoundary();
    TestCapacityAndBindingPolicy();
    TestLuaDragFeedbackUsesCommittedSurfaceIdentity();
    TestHostPickerCandidatePolicy();
    TestPointerReorderTargets();
    TestKeyboardFocusRules();
    std::cout << "Lua logical slot container tests passed\n";
    return 0;
}
