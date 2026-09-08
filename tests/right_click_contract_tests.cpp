#include "namespace_menu_actions.h"
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

            Check(protectedMenu == expected,
                std::string(descriptor.name) +
                ": protected namespace icons expose the host menu without changing item capabilities");
        }
    }
}


void TestNamespaceCommonActions()
{
    class Context final : public IContextMenu
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR offset, UINT type, UINT*, LPSTR text, UINT count) override
        {
            if (type != GCS_VERBW || offset > 1) return E_INVALIDARG;
            return wcscpy_s(reinterpret_cast<wchar_t*>(text), count, offset == 0 ? L"Manage" : L"empty") == 0 ? S_OK : E_FAIL;
        }
    } context;
    HMENU menu = CreatePopupMenu(), child = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Manage command");
    AppendMenuW(child, MF_STRING | MF_GRAYED, 2, L"Empty bin");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"Actions");
    const auto manage = snowdesktop::namespace_menu_actions::Find(&context, menu, L"manage");
    const auto empty = snowdesktop::namespace_menu_actions::Find(&context, menu, L"empty");
    Check(manage && manage->offset == 0 && manage->enabled && manage->label == L"Manage command",
        "namespace quick actions match canonical verbs case-insensitively and preserve the system label");
    Check(empty && empty->offset == 1 && !empty->enabled && empty->label == L"Empty bin",
        "namespace actions preserve disabled states even inside nested Shell menus");
    Check(!snowdesktop::namespace_menu_actions::Find(&context, menu, L"properties"),
        "unsupported system commands are not invented");
    DestroyMenu(menu);
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
    Check(
        !contract::ShouldOfferComponentPanelShortcut(Scope::Widget) &&
            contract::ShouldOfferComponentPanelShortcut(Scope::Element),
        "only element menus expose the component-panel escape hatch");
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
    Check(
        actions::ResolveRemovalAction(1, 0, 1, false) ==
            actions::RemovalAction::HideDesktopNamespace,
        "a single third-party desktop namespace must expose an explicit hide action");
    Check(
        actions::ResolveRemovalAction(2, 1, 1, false) ==
            actions::RemovalAction::Disabled,
        "mixed file and namespace selections must not partially delete their file items");
    Check(
        actions::ResolveRemovalAction(2, 2, 0, false) ==
            actions::RemovalAction::DeleteFiles,
        "a path-backed file selection must retain its delete action");
    Check(
        actions::ResolveRemovalAction(1, 0, 1, true) ==
            actions::RemovalAction::RemoveDockMapping,
        "removing a Dock mapping must take precedence over hiding its source namespace");
}

} // namespace

int main()
{
    TestContainerMenuMatrix();
    TestSlotItemMenuMatrix();
    TestSelectionContract();
    TestNamespaceCommonActions();
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
