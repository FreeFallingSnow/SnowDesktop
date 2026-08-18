#include "right_click_contract.h"
#include "app/shell_item_action_rules.h"

#include <iostream>
#include <string>
#include <vector>

namespace contract = snowdesktop::right_click_contract;
namespace slot = snowdesktop::slot_contract;

namespace
{
int failures = 0;

void Check(bool condition, const std::string& message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestContainerMenuMatrix()
{
    using Surface = slot::SlotSurfaceKind;
    using Menu = contract::ContextMenuKind;

    for (const auto& descriptor :
        slot::kSurfaceDescriptors)
    {
        const Menu menu = contract::ResolveContainerMenu(
            descriptor.kind);
        if (descriptor.kind == Surface::External)
        {
            Check(menu == Menu::None,
                "external ingress has no container to right-click");
            continue;
        }

        Check(menu != Menu::None,
            std::string(descriptor.name) +
            ": every interactive container must expose a container menu");

        Menu expected = Menu::None;
        switch (descriptor.kind)
        {
        case Surface::Desktop:
            expected = Menu::Background;
            break;
        case Surface::Dock:
            expected = Menu::Dock;
            break;
        case Surface::Collection:
        case Surface::FileCategories:
        case Surface::FolderMapping:
        case Surface::CollectionGroup:
        case Surface::FileGroup:
        case Surface::LuaLogicalSlot:
        case Surface::Guide:
            expected = Menu::Widget;
            break;
        case Surface::External:
            expected = Menu::None;
            break;
        default:
            break;
        }
        Check(menu == expected,
            std::string(descriptor.name) +
            ": container right-click must route to the expected menu");
    }
}

void TestSlotItemMenuMatrix()
{
    using Surface = slot::SlotSurfaceKind;
    using Item = contract::SlotItemKind;
    using Menu = contract::ContextMenuKind;

    for (const auto& descriptor :
        slot::kSurfaceDescriptors)
    {
        for (std::size_t itemIndex = 0;
            itemIndex < static_cast<std::size_t>(Item::Count);
            ++itemIndex)
        {
            const Item item =
                static_cast<Item>(itemIndex);
            const Menu menu = contract::ResolveSlotItemMenu(
                descriptor.kind, item, false);
            const Menu protectedMenu =
                contract::ResolveSlotItemMenu(
                    descriptor.kind, item, true);

            if (item == Item::None)
            {
                Check(menu == Menu::None &&
                        protectedMenu == Menu::None,
                    "no item must never open a slot item menu");
                continue;
            }

            Menu expected = Menu::None;
            switch (descriptor.kind)
            {
            case Surface::Desktop:
                if (item == Item::DesktopItem)
                    expected = Menu::DesktopItem;
                else if (item == Item::Widget)
                    expected = Menu::Widget;
                break;
            case Surface::Dock:
                if (item == Item::DesktopItem)
                    expected = Menu::DesktopItem;
                else if (item == Item::FolderEntry)
                    expected = Menu::FolderEntry;
                else if (item == Item::Widget)
                    expected = Menu::Widget;
                break;
            case Surface::Collection:
            case Surface::FileCategories:
                if (item == Item::DesktopItem)
                    expected = Menu::DesktopItem;
                break;
            case Surface::FolderMapping:
                if (item == Item::FolderEntry)
                    expected = Menu::FolderEntry;
                break;
            case Surface::CollectionGroup:
                if (item == Item::DesktopItem)
                    expected = Menu::DesktopItem;
                else if (item == Item::CollectionGroupLabel)
                    expected = Menu::CollectionGroupTab;
                else if (item == Item::Widget)
                    expected = Menu::Widget;
                break;
            case Surface::FileGroup:
                if (item == Item::DesktopItem)
                    expected = Menu::DesktopItem;
                else if (item == Item::FolderEntry)
                    expected = Menu::FolderEntry;
                else if (item == Item::FileGroupLabel)
                    expected = Menu::FileGroupSourceTab;
                else if (item == Item::Widget)
                    expected = Menu::Widget;
                break;
            case Surface::LuaLogicalSlot:
                if (item == Item::LogicalSlotItem)
                    expected = Menu::LogicalSlotItem;
                break;
            default:
                break;
            }

            Check(menu == expected,
                std::string(descriptor.name) +
                ": item right-click must route to the expected menu");

            const Menu protectedExpected =
                item == Item::DesktopItem &&
                        expected == Menu::DesktopItem
                    ? Menu::ShellDesktopItem
                    : expected;
            Check(protectedMenu == protectedExpected,
                std::string(descriptor.name) +
                ": protected desktop icons must route to Shell");
        }
    }
}

void TestSelectionContract()
{
    Check(
        !contract::ShouldPreserveSelectionOnRightClick(false),
        "right-clicking an unselected item must clear stale selection");
    Check(
        contract::ShouldPreserveSelectionOnRightClick(true),
        "right-clicking a selected item must preserve multi-selection");
}

void TestLuaWidgetMenuScope()
{
    struct MenuItem
    {
        std::string actionId;
        bool elementContext = false;
        bool separator = false;
        std::vector<MenuItem> children;
    };

    using Scope = contract::LuaWidgetMenuScope;
    Check(
        contract::ResolveLuaWidgetMenuScope(false) == Scope::Widget,
        "component actions must remain attached to the widget menu");
    Check(
        contract::ResolveLuaWidgetMenuScope(true) == Scope::Element,
        "element actions must replace the widget menu at that target");
    const std::vector<MenuItem> nestedComponentMenu = {
        MenuItem{ {}, false, false,
            { MenuItem{ "component-action", false, false, {} } } }
    };
    const std::vector<MenuItem> nestedElementMenu = {
        MenuItem{ {}, false, false,
            { MenuItem{ "element-action", true, false, {} } } }
    };
    Check(
        !contract::HasLuaElementMenuAction(nestedComponentMenu) &&
            contract::HasLuaElementMenuAction(nestedElementMenu),
        "nested Lua menu leaves must participate in element scope routing");
}

void TestMenuFocusRestoreContract()
{
    Check(
        contract::ShouldRestoreInteractionFocusAfterMenu(
            false, false),
        "a completed desktop menu must restore interaction focus");
    Check(
        !contract::ShouldRestoreInteractionFocusAfterMenu(
            true, false),
        "an open interaction surface must retain its own focus");
    Check(
        !contract::ShouldRestoreInteractionFocusAfterMenu(
            false, true),
        "a newly started inline editor must retain focus");
}

void TestShellItemActionContract()
{
    namespace actions =
        snowdesktop::shell_item_action_rules;
    Check(
        actions::IsAdministratorRunnableExtension(L".exe") &&
        actions::IsAdministratorRunnableExtension(L".lnk") &&
        actions::IsAdministratorRunnableExtension(L".cmd"),
        "executable Shell items must expose the administrator action");
    Check(
        !actions::IsAdministratorRunnableExtension(L".txt") &&
        !actions::IsAdministratorRunnableExtension(L""),
        "ordinary documents must not expose the administrator action");
}

} // namespace

int main()
{
    TestContainerMenuMatrix();
    TestSlotItemMenuMatrix();
    TestSelectionContract();
    TestLuaWidgetMenuScope();
    TestMenuFocusRestoreContract();
    TestShellItemActionContract();
    if (failures != 0)
    {
        std::cerr << failures
            << " right-click contract test(s) failed\n";
        return 1;
    }
    std::cout
        << "All right-click contract tests passed\n";
    return 0;
}
