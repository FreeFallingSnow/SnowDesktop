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
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
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
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                            result.data(), size);
    return result;
}
bool Blocked(const wchar_t *clsid)
{
    for (auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked", 0,
                          KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        DWORD size = 0;
        const auto status = RegQueryValueExW(key, clsid, nullptr, nullptr, nullptr, &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS)
            return true;
    }
    return false;
}
std::wstring Lower(std::wstring s)
{
    for (auto &c : s)
        c = towlower(c);
    return s;
}
struct Reg
{
    HKEY key = nullptr;
    explicit Reg(const std::wstring &path)
    {
        RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key);
    }
    ~Reg()
    {
        if (key)
            RegCloseKey(key);
    }
    Reg(const Reg &) = delete;
};
std::wstring Value(HKEY key, const wchar_t *name = nullptr)
{
    wchar_t text[32768]{};
    DWORD bytes = sizeof(text), type = 0;
    if (!key ||
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(text), &bytes) !=
            ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > sizeof(text) || bytes % sizeof(wchar_t))
        return {};
    text[std::size(text) - 1] = 0;
    return text;
}
bool Exists(HKEY key, const wchar_t *value)
{
    DWORD bytes = 0;
    return key && RegQueryValueExW(key, value, nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}
std::vector<std::wstring> Keys(const std::wstring &path)
{
    Reg key(path);
    std::vector<std::wstring> result;
    if (!key.key)
        return result;
    for (DWORD i = 0; i < 4096; ++i)
    {
        wchar_t name[512]{};
        DWORD n = std::size(name);
        if (RegEnumKeyExW(key.key, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        result.emplace_back(name, n);
    }
    return result;
}
std::wstring Display(std::wstring text)
{
    if (text.starts_with(L"@"))
    {
        wchar_t result[1024]{};
        if (SUCCEEDED(SHLoadIndirectString(text.c_str(), result, std::size(result), nullptr)))
            text = result;
    }
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'&')
        {
            if (i + 1 < text.size() && text[i + 1] == L'&')
                ++i;
            else
                continue;
        }
        out += text[i];
    }
    return out;
}
bool OwnedVerb(std::wstring name)
{
    name = Lower(std::move(name));
    return name == L"open" || name == L"explore" || name == L"opennewwindow" || name == L"cut" ||
           name == L"copy" || name == L"paste" || name == L"pastelink" || name == L"delete" ||
           name == L"rename" || name == L"properties";
}
struct Registration
{
    std::string id;
    std::wstring root, name, verb, clsid;
};
std::vector<Registration> Registrations(const Request &request)
{
    std::set<std::wstring> roots;
    if (request.background)
    {
        roots.insert(L"Directory\\Background");
        PWSTR desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        {
            if (Lower(desktop) == Lower(request.paths.front()))
                roots.insert(L"DesktopBackground");
            CoTaskMemFree(desktop);
        }
    }
    else
    {
        roots.insert(L"AllFilesystemObjects");
        bool allFolders = true, allFiles = true;
        std::wstring extension;
        for (const auto &path : request.paths)
        {
            const bool folder = (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
            allFolders &= folder;
            allFiles &= !folder;
            auto ext = Lower(std::filesystem::path(path).extension().wstring());
            if (extension.empty())
                extension = ext;
            else if (extension != ext)
                extension = L"!mixed";
        }
        if (allFolders)
        {
            roots.insert(L"Folder");
            roots.insert(L"Directory");
        }
        if (allFiles)
            roots.insert(L"*");
        if (allFiles && !extension.empty() && extension != L"!mixed")
        {
            roots.insert(extension);
            roots.insert(L"SystemFileAssociations\\" + extension);
            Reg ext(extension);
            auto prog = Value(ext.key);
            if (!prog.empty())
                roots.insert(prog);
            auto perceived = Value(ext.key, L"PerceivedType");
            if (!perceived.empty())
                roots.insert(L"SystemFileAssociations\\" + perceived);
        }
    }
    std::vector<Registration> result;
    std::set<std::string> seen;
    for (const auto &root : roots)
    {
        for (const auto &name : Keys(root + L"\\shell"))
        {
            if (OwnedVerb(name))
                continue;
            Reg key(root + L"\\shell\\" + name);
            if (Exists(key.key, L"LegacyDisable") || Exists(key.key, L"ProgrammaticAccessOnly") ||
                (!request.extended && Exists(key.key, L"Extended")))
                continue;
            auto label = Value(key.key, L"MUIVerb");
            if (label.empty())
                label = Value(key.key);
            if (label.empty())
                label = name;
            auto id = "verb:" + Utf8(Lower(root + L"\\shell\\" + name));
            result.push_back({id, root, Display(label), name, {}});
        }
        for (const auto &name : Keys(root + L"\\shellex\\ContextMenuHandlers"))
        {
            Reg key(root + L"\\shellex\\ContextMenuHandlers\\" + name);
            auto clsid = Value(key.key);
            if (clsid.empty())
                clsid = name;
            CLSID parsed{};
            if (FAILED(CLSIDFromString(clsid.c_str(), &parsed)))
                continue;
            wchar_t canonical[40]{};
            StringFromGUID2(parsed, canonical, 40);
            if (Blocked(canonical))
                continue;
            auto id = "handler:" + Utf8(Lower(canonical));
            if (!seen.insert(id).second)
                continue;
            // Canonical CLSID identifies a handler across applicable registry roots.
            Reg description(L"CLSID\\" + clsid);
            auto label = Value(description.key);
            if (label.empty())
                label = name;
            result.push_back({id, root, Display(label), {}, clsid});
        }
    }
    if (result.size() > 512)
        result.resize(512);
    return result;
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
            if (SUCCEEDED(self->tracking->context.As(&c3)) &&
                SUCCEEDED(c3->HandleMenuMsg2(msg, wp, lp, &result)))
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
        window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"SnowDesktop", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, cls.hInstance, this);
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
        if (SUCCEEDED(source.context->GetCommandString(id - 1, GCS_VERBW, nullptr,
                                                       reinterpret_cast<LPSTR>(verb), std::size(verb))))
            return verb;
        char ansi[1024]{};
        if (FAILED(source.context->GetCommandString(id - 1, GCS_VERBA, nullptr, ansi, std::size(ansi))))
            return {};
        MultiByteToWideChar(CP_ACP, 0, ansi, -1, verb, std::size(verb));
        return verb;
    }
    void InitPopup(Native &source, HMENU menu)
    {
        ComPtr<IContextMenu3> c3;
        LRESULT ignored = 0;
        if (SUCCEEDED(source.context.As(&c3)))
            c3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0, &ignored);
        else
        {
            ComPtr<IContextMenu2> c2;
            if (SUCCEEDED(source.context.As(&c2)))
                c2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
        }
    }
    void Bitmap(Entry &entry, HBITMAP bitmap)
    {
        if (!bitmap || reinterpret_cast<INT_PTR>(bitmap) <= 16)
            return;
        BITMAP info{};
        if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0 ||
            info.bmWidth > 128 || info.bmHeight > 128)
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
        const bool ok =
            GetDIBits(dc, bitmap, 0, info.bmHeight, entry.pixels.data(), &dib, DIB_RGB_COLORS) != 0;
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
    std::vector<Entry> Read(Native &source, HMENU menu, const std::string &provider, int depth = 0)
    {
        std::vector<Entry> entries;
        if (depth > 8)
            return entries;
        InitPopup(source, menu);
        for (int i = 0; i < GetMenuItemCount(menu) && count < kMaximumEntries; ++i)
        {
            ++count;
            wchar_t label[2048]{};
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_BITMAP;
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
                commands[entry.token] = {&source, 0, true};
            }
            else if (item.hSubMenu)
                entry.children = Read(source, item.hSubMenu, provider, depth + 1);
            else if (item.wID >= 1 && item.wID <= 0x7fff)
            {
                entry.token = next++;
                commands[entry.token] = {&source, item.wID - 1, false};
            }
            Bitmap(entry, item.hbmpItem);
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
        if (request.paths.empty() || request.paths.size() > 256 || request.providers.size() > 512)
            return {};
        for (const auto &p : request.paths)
            if (p.empty() || p.size() > 32767 || p.find(L'\0') != std::wstring::npos ||
                GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                return {};
        if (request.background && request.paths.size() != 1)
            return {};
        const auto registry = Registrations(request);
        Reply reply;
        reply.ok = true;
        if (request.catalogueOnly)
        {
            for (const auto &r : registry)
            {
                Entry e;
                e.provider = r.id;
                e.label = r.name;
                reply.entries.push_back(std::move(e));
            }
            return reply;
        }
        std::vector<std::unique_ptr<Pidl>> pidls;
        std::vector<PCIDLIST_ABSOLUTE> raw;
        for (const auto &p : request.paths)
        {
            auto id = std::make_unique<Pidl>();
            if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &id->value, 0, nullptr)))
                return {};
            raw.push_back(id->value);
            pidls.push_back(std::move(id));
        }
        ComPtr<IShellItemArray> selection;
        ComPtr<IDataObject> data;
        if (!request.background)
        {
            if (FAILED(
                    SHCreateShellItemArrayFromIDLists(static_cast<UINT>(raw.size()), raw.data(), &selection)))
                return {};
            selection->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        }
        ComPtr<IShellFolder> folder;
        Pidl directory;
        const std::wstring dir = request.background
                                     ? request.paths.front()
                                     : std::filesystem::path(request.paths.front()).parent_path().wstring();
        if (FAILED(SHParseDisplayName(dir.c_str(), nullptr, &directory.value, 0, nullptr)))
            return {};
        ComPtr<IShellItem> folderItem;
        if (FAILED(SHCreateItemFromIDList(directory.value, IID_PPV_ARGS(&folderItem))) ||
            FAILED(folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))))
            return {};
        for (const auto &r : registry)
        {
            if (std::find(request.providers.begin(), request.providers.end(), r.id) ==
                request.providers.end())
                continue;
            auto native = std::make_unique<Native>();
            native->directory = dir;
            native->site.Initialize(folder.Get(), window);
            if (!r.clsid.empty())
            {
                CLSID id{};
                CLSIDFromString(r.clsid.c_str(), &id);
                if (FAILED(
                        CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native->context))))
                    continue;
                ComPtr<IShellExtInit> init;
                if (FAILED(native->context.As(&init)))
                    continue;
                Reg classKey(r.root);
                if (FAILED(init->Initialize(directory.value, request.background ? nullptr : data.Get(),
                                            classKey.key)))
                    continue;
            }
            else
            {
                ComPtr<IContextMenu> combined;
                if (!combined)
                {
                    if (request.background)
                        folder->CreateViewObject(window, IID_PPV_ARGS(&combined));
                    else
                        selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&combined));
                }
                native->context = combined;
                if (!native->context)
                    continue;
            }
            native->site.Attach(native->context.Get());
            if (FAILED(native->context->QueryContextMenu(native->menu, 0, 1, 0x7fff,
                                                         CMF_NORMAL | CMF_SYNCCASCADEMENU |
                                                             (request.extended ? CMF_EXTENDEDVERBS : 0))))
                continue;
            auto entries = Read(*native, native->menu, r.id);
            if (count >= kMaximumEntries)
                return {};
            if (!r.verb.empty())
            {
                std::vector<Entry> matching;
                std::function<void(const std::vector<Entry> &)> find = [&](const auto &list) {
                    for (const auto &e : list)
                    {
                        if (Lower(Wide(e.key)) == Lower(r.verb))
                            matching.push_back(e);
                        find(e.children);
                    }
                };
                find(entries);
                if (matching.size() != 1)
                {
                    for (auto it = commands.begin(); it != commands.end();)
                        if (it->second.source == native.get())
                            it = commands.erase(it);
                        else
                            ++it;
                    continue;
                }
                entries = std::move(matching);
            }
            Entry group;
            group.provider = r.id;
            group.label = r.name;
            group.children = std::move(entries);
            group.token = next++;
            group.native = true;
            commands[group.token] = {native.get(), 0, true};
            // Empty handlers remain available through their native menu.
            reply.entries.push_back(std::move(group));
            menus.push_back(std::move(native));
        }
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
        if (command.native)
        {
            DisableOwned(source, source.menu);
            tracking = &source;
            SetForegroundWindow(window);
            const UINT chosen = TrackPopupMenuEx(source.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x,
                                                 point.y, window, nullptr);
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
    std::shared_ptr<settings_ipc::SettingsProcess> process =
        std::make_shared<settings_ipc::SettingsProcess>();
    std::optional<Reply> reply;
    ULONGLONG started = GetTickCount64();
    DWORD timeout = 8000;
    bool delivered = false, detached = false;
    ~Impl()
    {
        channel.Close();
        if (!detached)
            process->Stop();
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
    impl_->channel.Bind<void, Reply>("menu.reply",
                                     [p = impl_.get()](Reply result) { p->reply = std::move(result); });
    impl_->process->Start(impl_->channel, L"--shell-menu-helper");
    impl_->channel.Notify("menu.query", request);
}
Session::~Session() = default;
std::optional<Reply> Session::Poll()
{
    if (impl_->delivered)
        return {};
    if (!impl_->reply && (!impl_->process->Running() || GetTickCount64() - impl_->started > impl_->timeout))
    {
        impl_->process->Stop();
        impl_->reply = Reply{};
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
Placement ResolvePlacement(const Preferences &prefs, const Entry &entry)
{
    if (!prefs.enabled)
        return Placement::Hidden;
    for (const auto &s : prefs.selections)
        if (s.provider == entry.provider && !entry.key.empty() && s.command == entry.key)
            return s.placement;
    for (const auto &s : prefs.selections)
        if (s.provider == entry.provider && s.command.empty())
            return s.placement;
    return Placement::Hidden;
}
std::vector<Entry> SelectEntries(const Preferences &prefs, const std::vector<Entry> &entries, Placement place)
{
    std::vector<Entry> result;
    for (const auto &e : entries)
    {
        if (e.separator)
            continue;
        auto children = SelectEntries(prefs, e.children, place);
        if (!children.empty())
        {
            if (place == Placement::Root && ResolvePlacement(prefs, e) != Placement::Root)
                result.insert(result.end(), children.begin(), children.end());
            else
            {
                auto copy = e;
                copy.children = std::move(children);
                result.push_back(std::move(copy));
            }
        }
        else if (e.children.empty() && ResolvePlacement(prefs, e) == place)
            result.push_back(e);
    }
    const auto rank = [&](const Entry &entry) {
        for (size_t i = 0; i < prefs.selections.size(); ++i)
            if (prefs.selections[i].provider == entry.provider && prefs.selections[i].command == entry.key)
                return i;
        for (size_t i = 0; i < prefs.selections.size(); ++i)
            if (prefs.selections[i].provider == entry.provider && prefs.selections[i].command.empty())
                return i;
        return prefs.selections.size();
    };
    std::stable_sort(result.begin(), result.end(),
                     [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
    return result;
}
} // namespace snowdesktop::shell_extensions
