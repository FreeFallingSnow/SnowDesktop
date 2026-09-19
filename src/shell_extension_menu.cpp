#include "shell_extension_menu.h"
#include "settings_process.h"
#include "shell_context_menu_invoke.h"
#include "shell_context_menu_site.h"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <shlwapi.h>
#include <shobjidl.h>
#include <thread>
#include <wrl/client.h>

namespace snowdesktop::shell_extensions
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr size_t kMaximumEntries = 2048;
std::atomic<unsigned> sessions{0};
std::string Utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::wstring Wide(const std::string &s)
{
    if (s.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(size, 0);
    if (size)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), size);
    return result;
}
std::wstring Lower(std::wstring text)
{
    for (auto &c : text)
        c = towlower(c);
    return text;
}
std::wstring Display(const std::wstring &text)
{
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'&')
        {
            if (i + 1 < text.size() && text[i + 1] == L'&')
                ++i;
            else
                continue;
        }
        result += text[i];
    }
    return result;
}
bool OwnedVerb(std::wstring verb)
{
    verb = Lower(std::move(verb));
    return verb == L"open" || verb == L"explore" || verb == L"opennewwindow" || verb == L"cut" || verb == L"copy" ||
           verb == L"paste" || verb == L"pastelink" || verb == L"delete" || verb == L"rename" ||
           verb == L"properties" || verb == L"runas" || verb == L"copyaspath" || verb == L"opencontaining" ||
           verb == L"view" || verb == L"arrange" || verb == L"refresh" || verb == L"new" || verb == L"newfolder";
}
void PrepareSample(Request &request, std::filesystem::path &ownedDirectory)
{
    if (!request.catalogueOnly || !request.paths.empty())
        return;
    if (request.context == Context::Desktop)
    {
        PWSTR desktop = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
            throw settings_ipc::ProtocolError("desktop unavailable");
        request.paths = {desktop};
        CoTaskMemFree(desktop);
        request.background = true;
        return;
    }
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        throw settings_ipc::ProtocolError("sample identifier unavailable");
    wchar_t guid[40]{};
    StringFromGUID2(id, guid, 40);
    auto directory = std::filesystem::temp_directory_path() / (std::wstring(L"SnowDesktop-MenuPreview-") + guid);
    if (!std::filesystem::create_directory(directory))
        throw settings_ipc::ProtocolError("sample directory unavailable");
    ownedDirectory = directory; // Only this newly-created directory is owned.
    request.background = request.context == Context::FolderBackground;
    if (request.context == Context::File || request.context == Context::Automatic)
    {
        const auto file = directory / L"Sample.txt";
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw settings_ipc::ProtocolError("sample file unavailable");
        CloseHandle(handle);
        request.paths = {file.wstring()};
    }
    else
        request.paths = {directory.wstring()};
}
void IdentifyEntries(std::vector<Entry> &entries)
{
    std::map<std::string, unsigned> counts;
    for (const auto &entry : entries)
        if (!entry.key.empty())
            ++counts[Utf8(Lower(Wide(entry.key)))];
    for (auto &entry : entries)
    {
        if (entry.separator)
            continue;
        const auto verb = Utf8(Lower(Wide(entry.key)));
        if (!verb.empty() && counts[verb] == 1)
            entry.provider = "verb:" + verb;
        else
        {
            // Prefer canonical verbs; unidentified commands use their visible
            // label and submenu verb signature, scoped to the selected context.
            std::set<std::string> verbs;
            std::function<void(const std::vector<Entry> &)> collect = [&](const auto &items) {
                for (const auto &item : items)
                {
                    if (!item.key.empty())
                        verbs.insert(Utf8(Lower(Wide(item.key))));
                    collect(item.children);
                }
            };
            collect(entry.children);
            std::uint64_t signature = 14695981039346656037ull;
            for (const auto &key : verbs)
            {
                for (unsigned char c : key)
                {
                    signature ^= c;
                    signature *= 1099511628211ull;
                }
                signature *= 1099511628211ull;
            }
            entry.provider = "menu:" + Utf8(Lower(entry.label)) + ":" + std::to_string(signature);
        }
    }
}
struct Pidl
{
    PIDLIST_ABSOLUTE value = nullptr;
    ~Pidl()
    {
        CoTaskMemFree(value);
    }
};
struct Native
{
    ComPtr<IContextMenu> context;
    ComPtr<IShellFolder> folder;
    ShellContextMenuSite site;
    HMENU menu = CreatePopupMenu();
    std::wstring directory;
    ~Native()
    {
        if (menu)
            DestroyMenu(menu);
    }
};
struct Command
{
    Native *source = nullptr;
    UINT offset = 0;
    bool native = false;
    HMENU nativeMenu = nullptr;
};
struct Host
{
    HWND window = nullptr;
    std::vector<std::unique_ptr<Native>> menus;
    std::map<UINT, Command> commands;
    UINT next = 1;
    Native *tracking = nullptr;
    bool invoked = false;
    size_t count = 0;
    std::function<void(const std::string &)> progress = [](const auto &) {};
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto *self = reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && self->tracking &&
            (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR))
        {
            ComPtr<IContextMenu3> c3;
            LRESULT result = 0;
            if (SUCCEEDED(self->tracking->context.As(&c3)) && SUCCEEDED(c3->HandleMenuMsg2(msg, wp, lp, &result)))
            {
                if (msg == WM_INITMENUPOPUP)
                    self->DisableOwned(*self->tracking, reinterpret_cast<HMENU>(wp));
                return result;
            }
            ComPtr<IContextMenu2> c2;
            if (SUCCEEDED(self->tracking->context.As(&c2)) && SUCCEEDED(c2->HandleMenuMsg(msg, wp, lp)))
            {
                if (msg == WM_INITMENUPOPUP)
                    self->DisableOwned(*self->tracking, reinterpret_cast<HMENU>(wp));
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    Host()
    {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"SnowDesktopShellExtensionOwner";
        RegisterClassW(&cls);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"SnowDesktop", WS_POPUP, 0, 0, 0, 0, nullptr,
                                 nullptr, cls.hInstance, this);
    }
    ~Host()
    {
        menus.clear();
        if (window)
            DestroyWindow(window);
    }
    std::wstring Verb(Native &source, UINT id)
    {
        if (id < 1 || id > 0x7fff)
            return {};
        wchar_t verb[1024]{};
        if (SUCCEEDED(source.context->GetCommandString(id - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb),
                                                       std::size(verb))))
            return verb;
        char ansi[1024]{};
        if (FAILED(source.context->GetCommandString(id - 1, GCS_VERBA, nullptr, ansi, std::size(ansi))))
            return {};
        MultiByteToWideChar(CP_ACP, 0, ansi, -1, verb, std::size(verb));
        return verb;
    }
    void Bitmap(Entry &entry, HBITMAP bitmap)
    {
        if (!bitmap || reinterpret_cast<INT_PTR>(bitmap) <= 16)
            return;
        BITMAP info{};
        if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0 || info.bmWidth > 128 ||
            info.bmHeight > 128)
            return;
        BITMAPINFO dib{};
        dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dib.bmiHeader.biWidth = info.bmWidth;
        dib.bmiHeader.biHeight = -info.bmHeight;
        dib.bmiHeader.biPlanes = 1;
        dib.bmiHeader.biBitCount = 32;
        dib.bmiHeader.biCompression = BI_RGB;
        entry.pixels.resize(info.bmWidth * info.bmHeight * 4);
        HDC dc = GetDC(nullptr);
        const bool ok = GetDIBits(dc, bitmap, 0, info.bmHeight, entry.pixels.data(), &dib, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, dc);
        if (!ok)
        {
            entry.pixels.clear();
            return;
        }
        entry.width = info.bmWidth;
        entry.height = info.bmHeight;
        bool alpha = false;
        for (size_t i = 3; i < entry.pixels.size(); i += 4)
            alpha |= entry.pixels[i] != 0;
        if (!alpha)
            for (size_t i = 3; i < entry.pixels.size(); i += 4)
                entry.pixels[i] = 255;
    }
    bool MenuMessage(Native &source, UINT message, WPARAM wp, LPARAM lp)
    {
        ComPtr<IContextMenu3> c3;
        LRESULT result = 0;
        if (SUCCEEDED(source.context.As(&c3)))
            return SUCCEEDED(c3->HandleMenuMsg2(message, wp, lp, &result));
        ComPtr<IContextMenu2> c2;
        return SUCCEEDED(source.context.As(&c2)) && SUCCEEDED(c2->HandleMenuMsg(message, wp, lp));
    }
    void CallbackBitmap(Entry &entry, Native &source, HMENU menu, const MENUITEMINFOW &item)
    {
        if (item.hbmpItem != HBMMENU_CALLBACK && item.hbmpUnchecked != HBMMENU_CALLBACK &&
            item.hbmpChecked != HBMMENU_CALLBACK)
            return;
        MEASUREITEMSTRUCT measure{};
        measure.CtlType = ODT_MENU;
        measure.itemID = item.wID;
        measure.itemData = item.dwItemData;
        measure.itemWidth = GetSystemMetrics(SM_CXSMICON);
        measure.itemHeight = GetSystemMetrics(SM_CYSMICON);
        MenuMessage(source, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure));
        const int width = static_cast<int>(measure.itemWidth), height = static_cast<int>(measure.itemHeight);
        if (width <= 0 || height <= 0 || width > 128 || height > 128)
            return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!bitmap || !dc)
        {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            return;
        }
        const auto previous = SelectObject(dc, bitmap);
        const size_t size = static_cast<size_t>(width) * height * 4;
        std::vector<unsigned char> black(size);
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_MENU;
        draw.itemID = item.wID;
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState = entry.enabled ? 0 : ODS_DISABLED;
        draw.hwndItem = reinterpret_cast<HWND>(menu);
        draw.hDC = dc;
        draw.rcItem = {0, 0, width, height};
        draw.itemData = item.dwItemData;
        // GDI callbacks may omit alpha. Rendering onto black and white recovers
        // premultiplied alpha without adding a black rectangle to light menus.
        memset(bits, 0, size);
        bool ok = MenuMessage(source, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
        GdiFlush();
        memcpy(black.data(), bits, size);
        memset(bits, 255, size);
        ok &= MenuMessage(source, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
        GdiFlush();
        bool visible = false;
        const auto white = static_cast<const unsigned char *>(bits);
        if (ok)
            for (size_t i = 0; i < size; i += 4)
            {
                int difference = 0;
                for (size_t c = 0; c < 3; ++c)
                    difference = std::max(difference, static_cast<int>(white[i + c]) - black[i + c]);
                black[i + 3] = static_cast<unsigned char>(255 - difference);
                visible |= black[i + 3] != 0;
            }
        SelectObject(dc, previous);
        DeleteDC(dc);
        DeleteObject(bitmap);
        if (ok && visible)
        {
            entry.width = width;
            entry.height = height;
            entry.pixels = std::move(black);
        }
    }
    std::vector<Entry> Read(Native &source, HMENU menu, const std::string &provider, int depth = 0,
                            const std::wstring &title = L"root")
    {
        std::vector<Entry> entries;
        if (depth > 8)
            return entries;
        if (depth == 0)
        {
            progress("initialize root menu");
            MenuMessage(source, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
        }
        // Read only materialized entries. Initializing every Shell cascade here
        // can synchronously enumerate network/cloud providers before the user
        // ever opens them. Deferred popups retain their native menu session.
        progress("read popup: " + Utf8(title));
        for (int i = 0; i < GetMenuItemCount(menu) && count < kMaximumEntries; ++i)
        {
            ++count;
            wchar_t label[2048]{};
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_BITMAP | MIIM_CHECKMARKS | MIIM_DATA;
            item.dwTypeData = label;
            item.cch = std::size(label);
            if (!GetMenuItemInfoW(menu, i, TRUE, &item))
                continue;
            Entry entry;
            entry.provider = provider;
            entry.label = Display(label);
            entry.separator = (item.fType & MFT_SEPARATOR) != 0;
            entry.enabled = (item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
            entry.checked = (item.fState & MFS_CHECKED) != 0;
            const auto verb = Verb(source, item.wID);
            entry.key = Utf8(verb);
            if (OwnedVerb(verb))
                continue;
            if (item.fType & MFT_OWNERDRAW || (!entry.separator && entry.label.empty()))
            {
                entry.native = true;
                entry.label = entry.label.empty() ? L"…" : entry.label;
                entry.token = next++;
                commands[entry.token] = {&source, 0, true, item.hSubMenu ? item.hSubMenu : menu};
            }
            else if (item.hSubMenu)
            {
                entry.children = Read(source, item.hSubMenu, provider, depth + 1, entry.label);
                const bool placeholder = entry.children.empty() ||
                    std::all_of(entry.children.begin(), entry.children.end(), [](const auto &child) {
                        return child.separator || (!child.token && child.children.empty());
                    });
                if (placeholder)
                {
                    entry.children.clear();
                    entry.native = true;
                    entry.token = next++;
                    commands[entry.token] = {&source, 0, true, item.hSubMenu};
                }
            }
            else if (item.wID >= 1 && item.wID <= 0x7fff)
            {
                entry.token = next++;
                commands[entry.token] = {&source, item.wID - 1, false};
            }
            Bitmap(entry, item.hbmpItem);
            if (entry.pixels.empty())
                Bitmap(entry, entry.checked && item.hbmpChecked ? item.hbmpChecked : item.hbmpUnchecked);
            if (entry.pixels.empty() && !(item.fType & MFT_OWNERDRAW))
                CallbackBitmap(entry, source, menu, item);
            entries.push_back(std::move(entry));
        }
        // Duplicate or absent verbs cannot be persisted as individual commands.
        std::map<std::string, int> keys;
        std::function<void(const std::vector<Entry> &)> tally = [&](const auto &list) {
            for (const auto &e : list)
            {
                if (!e.key.empty())
                    ++keys[e.key];
                tally(e.children);
            }
        };
        tally(entries);
        std::function<void(std::vector<Entry> &)> clear = [&](auto &list) {
            for (auto &e : list)
            {
                if (keys[e.key] > 1)
                    e.key.clear();
                clear(e.children);
            }
        };
        clear(entries);
        return entries;
    }
    Reply Query(const Request &request)
    {
        progress("validate paths");
        if (request.paths.empty() || request.paths.size() > 256)
            return {};
        for (const auto &path : request.paths)
            if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos ||
                GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                return {};
        if (request.background && request.paths.size() != 1)
            return {};
        std::vector<std::unique_ptr<Pidl>> pidls;
        std::vector<PCIDLIST_ABSOLUTE> raw;
        for (const auto &path : request.paths)
        {
            progress("parse selection");
            auto id = std::make_unique<Pidl>();
            if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &id->value, 0, nullptr)))
                return {};
            raw.push_back(id->value);
            pidls.push_back(std::move(id));
        }
        const std::wstring directory = request.background
                                           ? request.paths.front()
                                           : std::filesystem::path(request.paths.front()).parent_path().wstring();
        Pidl folderId;
        ComPtr<IShellItem> folderItem;
        ComPtr<IShellFolder> folder;
        progress("bind folder");
        if (FAILED(SHParseDisplayName(directory.c_str(), nullptr, &folderId.value, 0, nullptr)) ||
            FAILED(SHCreateItemFromIDList(folderId.value, IID_PPV_ARGS(&folderItem))) ||
            FAILED(folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))))
            return {};
        auto native = std::make_unique<Native>();
        native->directory = directory;
        // The real Shell aggregate decides what exists and applies system
        // filtering. Never instantiate registrations to bypass that decision.
        if (request.background)
        {
            progress("bind background menu");
            if (ResolveContext(request) == Context::Desktop)
            {
                PWSTR desktopPath = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPath)))
                {
                    if (Lower(directory) == Lower(desktopPath))
                        SHGetDesktopFolder(folder.ReleaseAndGetAddressOf());
                    CoTaskMemFree(desktopPath);
                }
            }
            if (!folder || FAILED(folder->CreateViewObject(window, IID_PPV_ARGS(&native->context))))
                return {};
        }
        else
        {
            progress("bind selection menu");
            const bool sameFolder = std::all_of(request.paths.begin(), request.paths.end(), [&](const auto &path) {
                return Lower(std::filesystem::path(path).parent_path().wstring()) == Lower(directory);
            });
            if (sameFolder)
            {
                std::vector<PCUITEMID_CHILD> children;
                for (auto id : raw)
                    children.push_back(ILFindLastID(id));
                if (FAILED(folder->GetUIObjectOf(window, static_cast<UINT>(children.size()), children.data(),
                                                 IID_IContextMenu, nullptr,
                                                 reinterpret_cast<void **>(native->context.GetAddressOf()))))
                    return {};
            }
            else
            {
                ComPtr<IShellItemArray> selection;
                if (FAILED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(raw.size()), raw.data(), &selection)) ||
                    FAILED(selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&native->context))))
                    return {};
            }
        }
        // A Shell view is needed for invoking view-dependent verbs, not for
        // enumerating the aggregate. Creating it before loading extensions can
        // make Shell window hooks re-enter item binding from a handler's DllMain.
        native->folder = folder;
        progress("query context menu");
        if (FAILED(native->context->QueryContextMenu(native->menu, 0, 1, 0x7fff,
                                                     CMF_NORMAL | (request.background ? 0 : CMF_ITEMMENU) |
                                                         (request.extended ? CMF_EXTENDEDVERBS : 0))))
            return {};
        Reply reply;
        reply.entries = Read(*native, native->menu, "");
        if (count >= kMaximumEntries)
            return {};
        IdentifyEntries(reply.entries);
        reply.ok = true;
        menus.push_back(std::move(native));
        return reply;
    }
    void DisableOwned(Native &source, HMENU menu, int depth = 0)
    {
        if (depth > 16)
            return;
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
        {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_ID | MIIM_SUBMENU;
            if (!GetMenuItemInfoW(menu, i, TRUE, &item))
                continue;
            if (OwnedVerb(Verb(source, item.wID)))
                EnableMenuItem(menu, i, MF_BYPOSITION | MF_GRAYED);
            if (item.hSubMenu)
                DisableOwned(source, item.hSubMenu, depth + 1);
        }
    }
    void Invoke(UINT token, POINT point)
    {
        auto found = commands.find(token);
        if (found == commands.end() || invoked)
            return;
        invoked = true;
        auto command = found->second;
        auto &source = *command.source;
        source.site.Initialize(source.folder.Get(), window);
        source.site.Attach(source.context.Get());
        if (command.native)
        {
            const auto popup = command.nativeMenu ? command.nativeMenu : source.menu;
            DisableOwned(source, popup);
            tracking = &source;
            SetForegroundWindow(window);
            const UINT chosen =
                TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, window, nullptr);
            tracking = nullptr;
            if (!chosen)
                return;
            command.offset = chosen - 1;
        }
        // Keep application-owned file operations out of the native fallback.
        // Their desktop/Dock semantics are handled by the existing host menu.
        if (OwnedVerb(Verb(source, command.offset + 1)))
            return;
        std::function<bool(HMENU, int)> enabled = [&](HMENU menu, int depth) {
            if (depth > 16)
                return false;
            for (int i = 0; i < GetMenuItemCount(menu); ++i)
            {
                MENUITEMINFOW state{sizeof(state)};
                state.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(menu, i, TRUE, &state))
                    continue;
                if (state.wID == command.offset + 1)
                    return (state.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
                if (state.hSubMenu && enabled(state.hSubMenu, depth + 1))
                    return true;
            }
            return false;
        };
        if (!enabled(source.menu, 0))
            return;
        source.site.SetInvocationOwner(window);
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_NOASYNC;
        invoke.hwnd = window;
        invoke.lpVerb = MAKEINTRESOURCEA(command.offset);
        invoke.lpVerbW = MAKEINTRESOURCEW(command.offset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = point;
        std::string ansi;
        SetShellInvocationDirectory(invoke, source.directory, ansi);
        source.context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&invoke));
    }
};
} // namespace

struct Session::Impl
{
    settings_ipc::Channel channel;
    std::shared_ptr<settings_ipc::SettingsProcess> process = std::make_shared<settings_ipc::SettingsProcess>();
    std::optional<Reply> reply;
    std::string stage = "start helper";
    std::filesystem::path sampleDirectory;
    ULONGLONG started = GetTickCount64();
    DWORD timeout = 8000;
    bool delivered = false, detached = false;
    ~Impl()
    {
        channel.Close();
        if (!detached)
            process->Stop();
        if (!sampleDirectory.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(sampleDirectory, ignored);
        }
        sessions.fetch_sub(1);
    }
};
Session::Session(const Request &request, DWORD queryTimeoutMs)
{
    if (sessions.fetch_add(1) >= 8)
    {
        sessions.fetch_sub(1);
        throw settings_ipc::ProtocolError("too many Shell menus");
    }
    try
    {
        impl_ = std::make_unique<Impl>();
    }
    catch (...)
    {
        sessions.fetch_sub(1);
        throw;
    }
    impl_->timeout = std::clamp<DWORD>(queryTimeoutMs, 100, 8000);
    impl_->channel.Bind<void, std::string>("menu.progress", [p = impl_.get()](std::string stage) {
        p->stage = std::move(stage);
    });
    impl_->channel.Bind<void, Reply>("menu.reply", [p = impl_.get()](Reply result) {
        if (!result.ok && result.error.empty())
            result.error = "query failed at " + p->stage;
        p->reply = std::move(result);
    });
    auto query = request;
    PrepareSample(query, impl_->sampleDirectory);
    impl_->process->Start(impl_->channel, L"--shell-menu-helper");
    impl_->channel.Notify("menu.query", query);
}
Session::~Session() = default;
std::optional<Reply> Session::Poll()
{
    if (impl_->delivered)
        return {};
    if (!impl_->reply && (!impl_->process->Running() || GetTickCount64() - impl_->started > impl_->timeout))
    {
        impl_->process->Stop();
        impl_->reply = Reply{false, {}, "helper stopped or timed out at " + impl_->stage};
    }
    if (!impl_->reply)
        return {};
    impl_->delivered = true;
    return std::move(impl_->reply);
}
void Session::Invoke(UINT token, POINT position)
{
    if (!impl_->delivered || !impl_->process->Running())
        return;
    AllowSetForegroundWindow(impl_->process->ProcessId());
    // Request acknowledgement only queues the invocation; arbitrary extension
    // code is dispatched afterwards, so this does not wait for a dialog.
    impl_->channel.CallWithTimeout<void>(1000, "menu.invoke", token, position.x, position.y);
    auto process = impl_->process;
    impl_->detached = true;
    std::thread([process] {
        for (unsigned i = 0; i < 1200 && process->Running(); ++i)
            Sleep(100);
        process->Stop();
    }).detach();
}
std::optional<int> TryRunHelper(QueryExecutor query)
{
    if (!settings_ipc::IsSettingsProcessCommand(L"--shell-menu-helper"))
        return {};
    if (FAILED(OleInitialize(nullptr)))
        return ERROR_DLL_INIT_FAILED;
    int result = 0;
    try
    {
        settings_ipc::Channel channel;
        settings_ipc::OpenInheritedSettingsChannel(channel, L"--shell-menu-helper");
        Host host;
        host.progress = [&](const auto &stage) { channel.Notify("menu.progress", stage); };
        bool queried = false, invocationQueued = false;
        ULONGLONG invokedAt = 0, dispatchedAt = 0;
        channel.SetDisconnected([&] {
            if (!invocationQueued)
                PostQuitMessage(0);
        });
        channel.Bind<void, Request>("menu.query", [&](Request request) {
            if (queried)
                return;
            queried = true;
            channel.Notify("menu.reply", query ? query(request) : host.Query(request));
        });
        channel.Bind<void, UINT, LONG, LONG>("menu.invoke", [&](UINT token, LONG x, LONG y) {
            if (invocationQueued)
                return;
            invocationQueued = true;
            dispatchedAt = GetTickCount64();
            channel.Post([&, token, x, y] {
                host.Invoke(token, {x, y});
                invokedAt = GetTickCount64();
            });
        });
        MSG msg{};
        bool running = true;
        while (running && (!dispatchedAt || GetTickCount64() - dispatchedAt < 120000))
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            // Keep modeless extension dialogs alive; only exit after all visible
            // process windows close, with a short asynchronous handoff grace.
            if (invokedAt && GetTickCount64() - invokedAt > 3000)
            {
                bool visible = false;
                EnumWindows(
                    [](HWND w, LPARAM p) -> BOOL {
                        DWORD process = 0;
                        GetWindowThreadProcessId(w, &process);
                        if (process == GetCurrentProcessId() && IsWindowVisible(w))
                            *reinterpret_cast<bool *>(p) = true;
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&visible));
                if (!visible)
                    break;
            }
        }
    }
    catch (...)
    {
        result = ERROR_INVALID_DATA;
    }
    OleUninitialize();
    return result;
}
Context ResolveContext(const Request &request)
{
    if (request.context >= Context::File && request.context <= Context::Desktop)
        return request.context;
    if (request.background)
        return Context::FolderBackground;
    bool allFolders = !request.paths.empty();
    for (const auto &path : request.paths)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        allFolders &= attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return allFolders ? Context::Folder : Context::File;
}
std::vector<Entry> VisibleEntries(const Preferences &prefs, const std::vector<Entry> &entries, const Request &request)
{
    const auto context = ResolveContext(request);
    bool mixed = false;
    if (context == Context::File && request.context == Context::Automatic)
        for (const auto &path : request.paths)
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            mixed |= attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
    std::vector<Entry> result;
    for (const auto &entry : entries)
    {
        if (entry.separator)
        {
            if (!result.empty() && !result.back().separator)
                result.push_back(entry);
        }
        else if (!IsHidden(prefs, entry.provider, context) &&
                 !(mixed && IsHidden(prefs, entry.provider, Context::Folder)))
            result.push_back(entry);
    }
    while (!result.empty() && result.back().separator)
        result.pop_back();
    return result;
}
} // namespace snowdesktop::shell_extensions
