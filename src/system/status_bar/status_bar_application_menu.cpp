#include "status_bar_application_menu.h"
#include "ui/menu/menu_label.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace snowdesktop::status_bar_application_menu
{
namespace
{
constexpr int kHeadingLimit = 32, kLevelLimit = 160;
constexpr std::size_t kCommandLimit = 512, kDepthLimit = 8;
constexpr UINT kInitTimeoutMs = 75;

struct Entry
{
    HMENU parent = nullptr, submenu = nullptr;
    UINT position = 0, id = 0, type = 0, state = 0;
    std::wstring text;

    bool Enabled() const { return !(state & (MFS_DISABLED | MFS_GRAYED)); }
    bool Separator() const { return (type & MFT_SEPARATOR) != 0; }
    bool SameIdentity(const Entry& other) const
    {
        return parent == other.parent && position == other.position && submenu == other.submenu &&
            id == other.id && type == other.type && text == other.text;
    }
    std::wstring Title() const
    {
        return DecodeMenuLabel(std::wstring_view(text).substr(0, text.find(L'\t'))).text;
    }
};

std::optional<Entry> ReadEntry(HMENU menu, UINT position)
{
    if (!menu) return {};
    std::array<wchar_t, 1024> text{};
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
    info.dwTypeData = text.data();
    info.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoW(menu, position, TRUE, &info) || info.cch >= text.size() - 1 ||
        (info.fType & (MFT_OWNERDRAW | MFT_BITMAP))) return {};
    if (!(info.fType & MFT_SEPARATOR) && !info.cch) return {};
    return Entry{menu, info.hSubMenu, position, info.wID, info.fType, info.fState, {text.data(), info.cch}};
}

bool NotifyByPosition(HMENU root)
{
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_STYLE;
    return GetMenuInfo(root, &info) && (info.dwStyle & MNS_NOTIFYBYPOS);
}

bool CanPost(const Entry& entry, HMENU root)
{
    return NotifyByPosition(root) || entry.id <= (std::numeric_limits<WORD>::max)();
}
}

bool Source::Live() const
{
    DWORD currentProcess = 0;
    return window && root && thread && GetWindowThreadProcessId(window, &currentProcess) == thread &&
        currentProcess == process && GetMenu(window) == root;
}

bool Source::CanInteract() const
{
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    if (foreground && foreground != window) GetWindowThreadProcessId(foreground, &foregroundProcess);
    return (foreground == window || foregroundProcess == GetCurrentProcessId()) && Live();
}

Snapshot Read(HWND window)
{
    if (!window) return {};
    Source source;
    source.window = window;
    source.thread = GetWindowThreadProcessId(window, &source.process);
    if (!source.thread || source.process == GetCurrentProcessId()) return {};
    source.root = GetMenu(window);
    if (!source.root) return {};

    Snapshot result{source, {}};
    const int count = std::min(GetMenuItemCount(source.root), kHeadingLimit);
    for (int index = 0; index < count; ++index)
        if (const auto entry = ReadEntry(source.root, static_cast<UINT>(index)); entry && !entry->Separator())
        {
            auto title = entry->Title();
            if (!title.empty())
                result.headings.push_back({entry->position, entry->id, std::move(title), entry->submenu != nullptr,
                    entry->Enabled() && (entry->submenu || CanPost(*entry, source.root))});
        }
    if (result.headings.empty() || !source.Live()) return {};
    return result;
}

void MenuDeleter::operator()(HMENU menu) const noexcept { DestroyMenu(menu); }

struct Popup::Impl
{
    struct Route
    {
        std::vector<UINT> path;
        Entry entry;
    };

    explicit Impl(Target value) : target(std::move(value)) {}
    ~Impl()
    {
        if (!target.source.Live()) return;
        for (auto it = initialized.rbegin(); it != initialized.rend(); ++it)
            if (IsMenu(*it)) PostMessageW(target.source.window, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(*it), 0);
    }

    Target target;
    std::optional<Entry> heading;
    std::vector<HMENU> initialized;
    std::vector<Route> routes;

    bool Notify(UINT message, HMENU menu, LPARAM info) const
    {
        DWORD_PTR ignored = 0;
        return target.source.CanInteract() && SendMessageTimeoutW(target.source.window, message,
            reinterpret_cast<WPARAM>(menu), info, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            kInitTimeoutMs, &ignored) != 0;
    }

    bool Initialize(const Entry& entry)
    {
        if (std::find(initialized.begin(), initialized.end(), entry.submenu) != initialized.end()) return true;
        if (!entry.submenu || initialized.size() >= kHeadingLimit ||
            !Notify(WM_INITMENUPOPUP, entry.submenu, MAKELPARAM(entry.position, FALSE))) return false;
        initialized.push_back(entry.submenu);
        return true;
    }

    std::optional<Entry> Resolve(const std::vector<UINT>& path) const
    {
        if (path.empty() || !heading || !target.source.CanInteract()) return {};
        auto current = ReadEntry(target.source.root, path.front());
        if (!current || !current->Enabled() || !heading->SameIdentity(*current)) return {};
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            current = ReadEntry(current->submenu, path[i]);
            if (!current || !current->Enabled() || current->Separator()) return {};
        }
        return current;
    }

    OwnedMenu CopyLevel(HMENU menu, const std::vector<UINT>& parentPath)
    {
        OwnedMenu copy(CreatePopupMenu());
        if (!copy || !target.source.CanInteract()) return {};
        const int count = std::min(GetMenuItemCount(menu), kLevelLimit);
        bool populated = false;
        for (int index = 0; index < count && routes.size() < kCommandLimit; ++index)
        {
            auto entry = ReadEntry(menu, static_cast<UINT>(index));
            if (!entry) continue;
            if (entry->Separator())
            {
                if (populated) AppendMenuW(copy.get(), MF_SEPARATOR, 0, nullptr);
                continue;
            }
            OwnedMenu child;
            if (entry->submenu && parentPath.size() < kDepthLimit)
            {
                child.reset(CreatePopupMenu());
                if (!child || !AppendMenuW(child.get(), MF_STRING | MF_GRAYED, 0, L"")) continue;
            }
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_STATE | MIIM_ID | MIIM_STRING;
            info.fState = entry->state & (MFS_DISABLED | MFS_GRAYED | MFS_CHECKED);
            if (entry->submenu ? !child : !CanPost(*entry, target.source.root)) info.fState |= MFS_DISABLED;
            info.wID = static_cast<UINT>(routes.size() + 1);
            info.dwTypeData = entry->text.data();
            if (child)
            {
                info.fMask |= MIIM_SUBMENU;
                info.hSubMenu = child.get();
            }
            if (!InsertMenuItemW(copy.get(), static_cast<UINT>(GetMenuItemCount(copy.get())), TRUE, &info)) continue;
            (void)child.release();
            auto path = parentPath;
            path.push_back(entry->position);
            routes.push_back({std::move(path), std::move(*entry)});
            populated = true;
        }
        if (!populated) return {};
        return copy;
    }

    bool Post(const Entry& entry) const
    {
        if (entry.submenu || !entry.Enabled()) return false;
        const auto& source = target.source;
        if (NotifyByPosition(source.root))
            return PostMessageW(source.window, WM_MENUCOMMAND, entry.position, reinterpret_cast<LPARAM>(entry.parent)) != FALSE;
        return CanPost(entry, source.root) && PostMessageW(source.window, WM_COMMAND, MAKEWPARAM(entry.id, 0), 0) != FALSE;
    }
};

Popup::Popup(Target target) : impl_(std::make_unique<Impl>(std::move(target))) {}
Popup::~Popup() = default;

OwnedMenu Popup::Open()
{
    auto& self = *impl_;
    const auto& target = self.target;
    if (!target.heading.enabled || !self.Notify(WM_INITMENU, target.source.root, 0)) return {};
    auto heading = ReadEntry(target.source.root, target.heading.position);
    if (!heading || !heading->Enabled() || heading->id != target.heading.command ||
        (heading->submenu != nullptr) != target.heading.submenu || heading->Title() != target.heading.label) return {};
    self.heading = std::move(heading);
    if (!self.heading->submenu || !self.Initialize(*self.heading)) return {};
    const std::vector<UINT> path{target.heading.position};
    if (!self.Resolve(path)) return {};
    return self.CopyLevel(self.heading->submenu, path);
}

OwnedMenu Popup::Expand(UINT command)
{
    auto& self = *impl_;
    if (!command || command > self.routes.size()) return {};
    const auto route = self.routes[command - 1];
    const auto current = self.Resolve(route.path);
    if (!current || !route.entry.SameIdentity(*current) || !self.Initialize(*current) || !self.Resolve(route.path)) return {};
    return self.CopyLevel(current->submenu, route.path);
}

bool Popup::InvokeDirect() const
{
    const auto& self = *impl_;
    const auto current = self.Resolve({self.target.heading.position});
    return current && self.Post(*current);
}

bool Popup::Invoke(UINT command) const
{
    const auto& self = *impl_;
    if (!command || command > self.routes.size()) return false;
    const auto& route = self.routes[command - 1];
    const auto current = self.Resolve(route.path);
    return current && route.entry.SameIdentity(*current) && self.Post(*current);
}
}
