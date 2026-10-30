#include "shell_extension_menu.h"
#include "shell_menu_read_projection.h"
#include "shell_extension_source.h"
#include "shell_extension_diagnostics.h"
#include "shell_menu_registry_watch.h"
#include "shell_extension_menu_items.h"
#include "shell_extension_menu_cache.h"
#include "shell_extension_nvidia_compat.h"
#include "ui/menu/menu_label.h"
#include "ui/menu/native_menu_theme.h"
#include "settings/settings_process.h"
#include "shell_context_menu_invoke.h"
#include "shell_context_menu_site.h"
#include "shell_start_pin.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
constexpr std::uint64_t kMenuSignatureSeed = 14695981039346656037ull;
std::atomic<unsigned> sessions{0};
std::atomic<unsigned> startPinSessions{0};
constexpr ULONGLONG kWorkerLifetimeMs = 300000;
constexpr unsigned kMaximumWarmQueries = 8;
Request ShellMenuRequest(const Request& source)
{
    auto request = source;
    // Win32 file APIs accept '/', but Shell parsing names require Windows
    // separators. Preserve the original objects and shortcut targets.
    for (auto& path : request.paths) std::replace(path.begin(), path.end(), L'/', L'\\');
    return request;
}
using RegistryWatch = detail::MenuRegistryMonitor;
struct RegistryNotifications
{
    std::mutex mutex;
    std::uint64_t revision = 1;
    std::vector<std::unique_ptr<RegistryWatch>> watches;
    std::vector<HANDLE> subscribers;
    RegistryNotifications()
    {
        for (auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
            for (auto path : {L"Software\\Classes",
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\nvcplui.exe",
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions",
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer"})
                watches.push_back(std::make_unique<RegistryWatch>(root, path, static_cast<unsigned>(watches.size()),
                    lstrcmpiW(path, L"Software\\Classes") == 0));
        watches.push_back(std::make_unique<RegistryWatch>(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 8));
        watches.push_back(std::make_unique<RegistryWatch>(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts", 9));
    }
    // Caller holds mutex. Keep existing consumers' edges while establishing
    // a current baseline for a new consumer; older events are not its changes.
    std::uint64_t Poll()
    {
        bool signalled = false;
        for (auto &watch : watches) signalled |= watch->Changed();
        if (signalled)
        {
            ++revision;
            for (auto subscriber : subscribers) SetEvent(subscriber);
        }
        return revision;
    }
};
std::shared_ptr<RegistryNotifications> SharedRegistryNotifications()
{
    // Subscribe once per process, rather than opening every class root for
    // each STA/service. TLS caches retain independent invalidation generations.
    static auto notifications = std::make_shared<RegistryNotifications>();
    return notifications;
}
struct CacheState
{
    std::uint64_t generation = 1, seen = 0;
    bool dirty = false;
    std::wstring epoch = SharedMenuCache().Epoch();
    std::shared_ptr<RegistryNotifications> notifications = SharedRegistryNotifications();
    HANDLE changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CacheState()
    {
        std::lock_guard lock(notifications->mutex);
        seen = notifications->Poll();
        if (changed) notifications->subscribers.push_back(changed);
    }
    ~CacheState()
    {
        std::lock_guard lock(notifications->mutex);
        std::erase(notifications->subscribers, changed);
        if (changed) CloseHandle(changed);
    }
    std::uint64_t Poll()
    {
        std::lock_guard lock(notifications->mutex);
        // The per-subscriber event preserves changes consumed by a sibling
        // between this Poll and its following wait.
        dirty |= seen != notifications->Poll();
        seen = notifications->revision;
        if (changed) ResetEvent(changed);
        return generation;
    }
};
CacheState &Caches()
{
    thread_local CacheState state;
    return state;
}
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
        else if (entry.provider.empty())
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
            std::uint64_t signature = kMenuSignatureSeed;
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
std::wstring RegistryString(const std::wstring &key, const wchar_t *name = nullptr, HKEY root = HKEY_CLASSES_ROOT)
{
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     &type, value, &bytes) != ERROR_SUCCESS)
        return {};
    if (type == REG_EXPAND_SZ)
    {
        wchar_t expanded[32768]{};
        const auto count = ExpandEnvironmentStringsW(value, expanded, static_cast<DWORD>(std::size(expanded)));
        if (!count || count > std::size(expanded)) return {};
        return expanded;
    }
    return value;
}
std::set<std::wstring> MenuProviderClasses(const Request& request)
{
    if (request.startPinOnly) return {};
    if (!request.sourceClsid.empty())
        return HandlerEnabled(request.sourceClsid) && (request.sourceKey.empty() || SourceRegistered(request.sourceClsid, request.sourceKey))
            ? std::set<std::wstring>{request.sourceClsid} : std::set<std::wstring>{};
    std::set<std::wstring> roots, classes;
    if (request.background || request.context == Context::Desktop)
    {
        roots.insert(L"Directory\\Background");
        if (request.context == Context::Desktop) roots.insert(L"DesktopBackground");
    }
    else for (const auto& path : request.paths)
    {
        roots.insert(L"*"); roots.insert(L"AllFilesystemObjects");
        const auto attributes = GetFileAttributesW(path.c_str());
        const std::wstring extension = Lower(PathFindExtensionW(path.c_str()));
        // Shell delegates folder shortcuts to their target's folder handlers.
        if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) || extension == L".lnk")
        { roots.insert(L"Directory"); roots.insert(L"Folder"); }
        if (extension.empty()) continue;
        roots.insert(extension); roots.insert(L"SystemFileAssociations\\" + extension);
        const auto addType = [&](const std::wstring& type) {
            if (type.empty()) return;
            roots.insert(type);
            if (const auto current = RegistryString(type + L"\\CurVer"); !current.empty()) roots.insert(current);
        };
        addType(RegistryString(extension));
        addType(RegistryString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
            extension + L"\\UserChoice", L"ProgId", HKEY_CURRENT_USER));
        if (const auto perceived = RegistryString(extension, L"PerceivedType"); !perceived.empty())
            roots.insert(L"SystemFileAssociations\\" + perceived);
    }
    for (const auto& root : roots)
    {
        const auto keyPath = root + L"\\shellex\\ContextMenuHandlers";
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD index = 0; index < 512; ++index)
        {
            wchar_t name[256]{}; DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            auto clsid = RegistryString(keyPath + L"\\" + name);
            if (clsid.empty()) clsid = name;
            CLSID parsed{};
            if (SUCCEEDED(CLSIDFromString(clsid.c_str(), &parsed)) && HandlerEnabled(clsid)) classes.insert(Lower(clsid));
        }
        RegCloseKey(key);
    }
    return classes;
}
// Load class factories without the query STA's Shell window hooks. Some vendor
// DllMain implementations create windows; first loading them inside QCM can
// re-enter Shell under loader lock. This never queries a menu or a dummy item.
// Retain factories on their own apartment until the bounded helper exits.
class MenuProviderLoader
{
    HANDLE work_ = nullptr, finished_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::set<std::wstring> requested_;
    bool stopping_ = false;
    void Run()
    {
        const HRESULT initialized = OleInitialize(nullptr);
        {
            std::map<std::wstring, ComPtr<IClassFactory>> factories;
            for (;;)
            {
                const auto ready = MsgWaitForMultipleObjectsEx(1, &work_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (ready == WAIT_OBJECT_0)
                {
                    std::set<std::wstring> requested;
                    { std::lock_guard lock(mutex_); if (stopping_) break; requested.swap(requested_); }
                    for (const auto& name : requested) if (!factories.contains(name))
                    {
                        ComPtr<IClassFactory> factory;
                        CLSID clsid{};
                        HRESULT result = FAILED(initialized) ? initialized : CLSIDFromString(name.c_str(), &clsid);
                        if (SUCCEEDED(result)) result = CoGetClassObject(clsid, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory));
                        factories.emplace(name, std::move(factory));
                        MenuTrace("provider_load", SUCCEEDED(result) ? "loaded" : "unavailable");
                    }
                    SetEvent(finished_);
                }
                else if (ready == WAIT_OBJECT_0 + 1)
                {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    { TranslateMessage(&message); DispatchMessageW(&message); }
                }
                else break;
            }
        }
        if (SUCCEEDED(initialized)) OleUninitialize();
    }
public:
    ~MenuProviderLoader()
    {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        if (work_) SetEvent(work_);
        if (thread_.joinable()) thread_.join();
        if (work_) CloseHandle(work_);
        if (finished_) CloseHandle(finished_);
    }
    void Load(const Request& request, const std::vector<Registration>& packages = {})
    {
        auto classes = MenuProviderClasses(request);
        for (const auto& registration : packages)
            if (registration.systemEnabled && !registration.verbs.empty() && Applies(registration, request))
                classes.insert(Wide(registration.verbs.front()));
        if (classes.empty()) return;
        MenuTiming timing("provider_load.wait");
        if (!thread_.joinable())
        {
            work_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            finished_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!work_ || !finished_) throw settings_ipc::ProtocolError("provider loader unavailable");
            thread_ = std::thread([this] { Run(); });
        }
        ResetEvent(finished_);
        { std::lock_guard lock(mutex_); requested_ = std::move(classes); }
        SetEvent(work_);
        // All waits remain inside the supervised child query deadline.
        if (WaitForSingleObject(finished_, INFINITE) != WAIT_OBJECT_0)
            throw settings_ipc::ProtocolError("provider loader stopped");
        timing.Record("complete");
    }
};
struct Native
{
    std::vector<std::unique_ptr<Pidl>> selection;
    ComPtr<IContextMenu> context;
    ComPtr<IShellFolder> folder;
    ShellContextMenuSite site;
    HMENU menu = CreatePopupMenu();
    std::wstring directory;
    std::vector<std::wstring> paths;
    bool metadataOnly = false;
    bool shortcutObjects = false;
    ComPtr<IShellItem> nvidiaApplication;
    ComPtr<IExplorerCommand> explorer;
    std::wstring explorerClass;
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
    ComPtr<IExplorerCommand> explorer;
    ComPtr<IShellItemArray> selection;
};
struct Host
{
    HWND window = nullptr;
    MenuProviderLoader providers;
    std::vector<std::unique_ptr<Native>> menus;
    std::vector<std::unique_ptr<Native>> retiredMenus;
    unsigned ordinaryQueries = 0;
    std::map<UINT, Command> commands;
    UINT next = 1;
    Native *tracking = nullptr;
    bool nativePopup = false;
    bool nativeLightTheme = true;
    bool invoked = false;
    size_t count = 0;
    StartPinExecutor startPin;
    struct RegisteredIcon { std::wstring name, friendlyName, module; };
    std::map<Context, std::vector<RegisteredIcon>> registeredIcons;
    struct ResourceIcon
    {
        WIN32_FILE_ATTRIBUTE_DATA stamp{};
        Entry image;
    };
    std::map<std::wstring, ResourceIcon> resourceIcons;
    std::function<void(const std::string &)> progress = [](const auto &) {};
    std::function<void(Reply)> rootPreview;
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
                if (msg == WM_INITMENUPOPUP && !self->nativePopup)
                    self->DisableOwned(*self->tracking, reinterpret_cast<HMENU>(wp));
                return result;
            }
            ComPtr<IContextMenu2> c2;
            if (SUCCEEDED(self->tracking->context.As(&c2)) && SUCCEEDED(c2->HandleMenuMsg(msg, wp, lp)))
            {
                if (msg == WM_INITMENUPOPUP && !self->nativePopup)
                    self->DisableOwned(*self->tracking, reinterpret_cast<HMENU>(wp));
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    explicit Host(StartPinExecutor executor) : startPin(std::move(executor))
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
        retiredMenus.clear();
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
        // GDI handles can be sign-extended on 64-bit Windows. Only the
        // documented menu bitmap sentinels are special, not negative handles.
        if (!bitmap || bitmap == HBMMENU_CALLBACK || reinterpret_cast<UINT_PTR>(bitmap) <= 16)
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
    const std::vector<RegisteredIcon> &RegisteredIcons(Context scope)
    {
        if (const auto found = registeredIcons.find(scope); found != registeredIcons.end()) return found->second;
        std::vector<RegisteredIcon> icons;
        std::vector<std::wstring> roots;
        if (scope == Context::File) roots = {L"*", L"AllFilesystemObjects"};
        else if (scope == Context::Folder) roots = {L"Directory", L"Folder", L"AllFilesystemObjects"};
        else if (scope == Context::FolderBackground) roots = {L"Directory\\Background"};
        else roots = {L"DesktopBackground", L"Directory\\Background"};
        for (const auto &root : roots)
        {
            const auto handlers = root + L"\\shellex\\ContextMenuHandlers";
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, handlers.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
            for (DWORD index = 0; index < 512; ++index)
            {
                wchar_t name[256]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                auto clsid = RegistryString(handlers + L"\\" + name);
                if (clsid.empty()) clsid = name;
                const auto classKey = L"CLSID\\" + clsid;
                auto module = RegistryString(classKey + L"\\InprocServer32");
                if (module.size() >= 2 && module.front() == L'"' && module.back() == L'"')
                    module = module.substr(1, module.size() - 2);
                if (!module.empty()) icons.push_back({Lower(name), Lower(RegistryString(classKey)), std::move(module)});
            }
            RegCloseKey(key);
        }
        return registeredIcons.emplace(scope, std::move(icons)).first->second;
    }
    void RegisteredBitmap(Entry &entry, const Request &request)
    {
        if (!entry.pixels.empty() || entry.separator || entry.label.empty()) return;
        const auto label = Lower(entry.label);
        for (const auto &icon : RegisteredIcons(ResolveContext(request)))
        {
            // Metadata only, matched to an item the actual Shell already returned.
            if (label != icon.name && label != icon.friendlyName) continue;
            WIN32_FILE_ATTRIBUTE_DATA stamp{};
            if (!GetFileAttributesExW(icon.module.c_str(), GetFileExInfoStandard, &stamp)) continue;
            auto found = resourceIcons.find(icon.module);
            if (found == resourceIcons.end() ||
                CompareFileTime(&stamp.ftLastWriteTime, &found->second.stamp.ftLastWriteTime) != 0 ||
                stamp.nFileSizeHigh != found->second.stamp.nFileSizeHigh ||
                stamp.nFileSizeLow != found->second.stamp.nFileSizeLow)
            {
                ResourceIcon resource;
                resource.stamp = stamp;
                HMODULE module = LoadLibraryExW(icon.module.c_str(), nullptr,
                    LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
                if (!module) continue;
                struct Search { Host *host; Entry *entry; } search{this, &resource.image};
                EnumResourceNamesW(module, RT_BITMAP,
                    [](HMODULE source, LPCWSTR, LPWSTR name, LONG_PTR context) -> BOOL {
                        auto &search = *reinterpret_cast<Search *>(context);
                        HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(source, name, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
                        BITMAP info{};
                        if (bitmap && GetObjectW(bitmap, sizeof(info), &info) && info.bmWidth == info.bmHeight &&
                            info.bmWidth >= 8 && info.bmWidth <= 64)
                            search.host->Bitmap(*search.entry, bitmap);
                        if (bitmap) DeleteObject(bitmap);
                        return search.entry->pixels.empty();
                    }, reinterpret_cast<LONG_PTR>(&search));
                FreeLibrary(module);
                if (resourceIcons.size() >= 64) resourceIcons.clear();
                found = resourceIcons.insert_or_assign(icon.module, std::move(resource)).first;
            }
            entry.width = found->second.image.width;
            entry.height = found->second.image.height;
            entry.pixels = found->second.image.pixels;
            if (!entry.pixels.empty()) return;
        }
    }
    std::vector<Entry> ReadUnexpandedIdentity(Native &source, HMENU menu, int depth)
    {
        std::vector<Entry> entries;
        if (depth > 8 || InspectPopup(menu) == PopupContents::Deferred) return entries;
        for (int i = 0; i < GetMenuItemCount(menu) && count < kMaximumEntries; ++i)
        {
            MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_ID | MIIM_SUBMENU;
            if (!GetMenuItemInfoW(menu, i, TRUE, &item)) continue;
            ++count;
            Entry entry; entry.key = Utf8(Verb(source, item.wID));
            if (item.hSubMenu) entry.children = ReadUnexpandedIdentity(source, item.hSubMenu, depth + 1);
            entries.push_back(std::move(entry));
        }
        return entries;
    }
    std::vector<Entry> Read(Native &source, HMENU menu, const std::string &provider, int depth = 0,
                            const std::wstring &title = L"root", const Request *request = nullptr)
    {
        std::vector<Entry> entries;
        std::vector<bool> omitted;
        struct Cascade { size_t entry; HMENU menu; UINT index; std::wstring verb; };
        std::vector<Cascade> cascades;
        const bool previewRoots = rootPreview && request && CanPreviewPopupRoots(*request, depth);
        const auto readCascade = [&](Entry &entry, HMENU child, UINT index, const std::wstring &verb) {
            if (depth < 8 && InspectPopup(child) == PopupContents::Deferred)
            {
                progress("initialize deferred popup: " + Utf8(entry.label));
                auto *previous = tracking;
                tracking = &source;
                TryMaterializePopup(child, window, index, [&] {
                    if (source.shortcutObjects || source.site.HasView()) return false;
                    progress("prepare deferred popup view: " + Utf8(entry.label));
                    if (!source.site.Initialize(source.folder.Get(), window)) return false;
                    source.site.Attach(source.context.Get());
                    return true;
                }, PopupNeedsInitialView(verb));
                tracking = previous;
            }
            if (!RequiresNativePopup(child)) entry.children = Read(source, child, provider, depth + 1, entry.label);
            const bool placeholder = entry.children.empty() ||
                std::all_of(entry.children.begin(), entry.children.end(), [](const auto &e) {
                    return e.separator || (!e.token && e.children.empty());
                });
            if (placeholder)
            {
                entry.children.clear(); entry.native = true; entry.token = next++;
                commands[entry.token] = {&source, 0, true, child};
            }
        };
        if (depth > 8)
            return entries;
        if (depth == 0)
        {
            progress("initialize root menu");
            MenuMessage(source, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
        }
        // Already materialized cascades need no initialization. Placeholder
        // popups are prepared in this supervised helper's STA menu state;
        // unsupported owner-drawn popups retain their native menu session.
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
            auto text = DecodeMenuLabel(label);
            entry.label = std::move(text.text);
            entry.accessKey = text.accessKey;
            entry.separator = (item.fType & MFT_SEPARATOR) != 0;
            entry.enabled = (item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
            entry.checked = (item.fState & MFS_CHECKED) != 0;
            const auto verb = Verb(source, item.wID);
            entry.key = Utf8(verb);
            if (OwnedVerb(verb) && IsRootMenuCommand(source.menu, item.wID))
                continue;
            const bool deferred = item.hSubMenu && InspectPopup(item.hSubMenu) == PopupContents::Deferred;
            if (depth == 0 && deferred)
                entry.provider = "menu:" + Utf8(Lower(entry.label)) + ":" + std::to_string(kMenuSignatureSeed);
            const bool skip = depth == 0 && request && !entry.separator &&
                !(item.fType & MFT_OWNERDRAW) && !entry.label.empty() &&
                !ReadMenuRootDetails(*request, entry.key.empty() ? "" : "verb:" + Utf8(Lower(Wide(entry.key))),
                                    "menu:" + Utf8(Lower(entry.label)) + ":");
            if (skip)
            {
                // Keep root/sibling identity and already-readable descendant
                // verbs for duplicate normalization, without invoking hidden
                // cascades, reading bitmap callbacks or loading icon resources.
                if (item.hSubMenu) entry.children = ReadUnexpandedIdentity(source, item.hSubMenu, depth + 1);
                omitted.push_back(true); entries.push_back(std::move(entry));
                MenuTrace("read_projection", "root_omitted");
                continue;
            }
            if (item.fType & MFT_OWNERDRAW || (!entry.separator && entry.label.empty()))
            {
                entry.native = true;
                entry.label = entry.label.empty() ? L"…" : entry.label;
                entry.token = next++;
                commands[entry.token] = {&source, 0, true, item.hSubMenu ? item.hSubMenu : menu};
            }
            else if (item.hSubMenu)
            {
                if (previewRoots) cascades.push_back({entries.size(), item.hSubMenu, static_cast<UINT>(i), verb});
                else readCascade(entry, item.hSubMenu, static_cast<UINT>(i), verb);
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
            omitted.push_back(false); entries.push_back(std::move(entry));
        }
        if (!cascades.empty())
        {
            auto roots = entries;
            for (auto &entry : roots) { entry.token = 0; if (!entry.separator) entry.enabled = false; }
            for (const auto &cascade : cascades)
            {
                auto &entry = roots[cascade.entry];
                entry.enabled = entries[cascade.entry].enabled;
                Entry loading; loading.label = L"…"; loading.enabled = false;
                entry.children = {std::move(loading)};
            }
            IdentifyEntries(roots);
            for (size_t i = roots.size(); i-- > 0;) if (omitted[i]) roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(i));
            progress("root headers ready; prefetch all cascades");
            rootPreview(Reply{true, std::move(roots), {}});
            // Continue immediately on this same STA/session. Hovering never
            // starts Shell work and cannot be required for completion.
            for (const auto &cascade : cascades)
                readCascade(entries[cascade.entry], cascade.menu, cascade.index, cascade.verb);
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
        if (depth == 0 && request && request->visibleMenuProviders)
        {
            // Normalize against all root headers before removing hidden rows;
            // filtering must not turn duplicate root verbs into unique IDs.
            IdentifyEntries(entries);
            for (size_t i = entries.size(); i-- > 0;)
                if (omitted[i]) entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return entries;
    }
    void ReleaseMenu()
    {
        progress("release previous menu");
        commands.clear();
        menus.clear();
        next = 1;
        count = 0;
        invoked = false;
    }
    void AppendPackagedCommands(const Request& request, IShellFolder* folder,
        std::vector<PCIDLIST_ABSOLUTE>& ids, const std::vector<Registration>& packages, Reply& reply)
    {
        if (request.startPinOnly || request.originalShortcutOnly || !request.sourceClsid.empty()) return;
        MenuTiming timing("packaged_commands");
        ComPtr<IShellItemArray> selection;
        if (!request.background && FAILED(SHCreateShellItemArrayFromIDLists(
                static_cast<UINT>(ids.size()), ids.data(), &selection))) return;
        std::vector<PCUITEMID_CHILD> children;
        const bool sameFolder = std::all_of(request.paths.begin(), request.paths.end(), [&](const auto& path) {
            return Lower(std::filesystem::path(path).parent_path().wstring()) ==
                Lower(std::filesystem::path(request.paths.front()).parent_path().wstring());
        });
        if (!request.background) for (const auto id : ids)
            children.push_back(sameFolder ? ILFindLastID(id) : reinterpret_cast<PCUITEMID_CHILD>(id));
        for (const auto& registration : packages)
        {
            if (!registration.systemEnabled || registration.verbs.empty() || !Applies(registration, request)) continue;
            const auto provider = "verb:" + registration.verbs.front();
            if (request.visibleMenuProviders && std::none_of(request.visibleMenuProviders->begin(), request.visibleMenuProviders->end(),
                    [&](const auto& id) { return id == provider || id == registration.id; })) continue;
            CLSID clsid{};
            if (FAILED(CLSIDFromString(Wide(registration.verbs.front()).c_str(), &clsid))) continue;
            ComPtr<IExplorerCommand> command;
            if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&command))))
            { MenuTrace("packaged_commands", "activation_failed"); continue; }
            auto source = std::make_unique<Native>();
            source->explorer = command;
            source->explorerClass = Wide(registration.verbs.front());
            source->paths = request.paths; source->folder = folder;
            source->directory = request.background ? request.paths.front() :
                std::filesystem::path(request.paths.front()).parent_path().wstring();
            if (FAILED(source->site.InitializeSelection(folder, window, static_cast<UINT>(children.size()), children.data()))) continue;
            std::function<std::optional<Entry>(IExplorerCommand*, int)> read = [&](IExplorerCommand* current, int depth) -> std::optional<Entry> {
                if (depth > 8 || count >= kMaximumEntries) return {};
                source->site.Attach(current);
                EXPCMDSTATE state = ECS_HIDDEN;
                HRESULT hr = current->GetState(selection.Get(), FALSE, &state);
                if (hr == E_PENDING) hr = current->GetState(selection.Get(), TRUE, &state);
                if (FAILED(hr) || (state & ECS_HIDDEN)) return {};
                ++count;
                Entry entry; entry.enabled = !(state & ECS_DISABLED); entry.checked = (state & ECS_CHECKED) != 0;
                EXPCMDFLAGS flags = ECF_DEFAULT; current->GetFlags(&flags);
                if (flags & ECF_ISSEPARATOR) { entry.separator = true; return entry; }
                PWSTR title = nullptr;
                if (FAILED(current->GetTitle(selection.Get(), &title)) || !title) return {};
                const auto decoded = DecodeMenuLabel(title); CoTaskMemFree(title);
                entry.label = decoded.text; entry.accessKey = decoded.accessKey;
                GUID canonical{};
                if (SUCCEEDED(current->GetCanonicalName(&canonical)) && !IsEqualGUID(canonical, GUID_NULL))
                { wchar_t text[40]{}; StringFromGUID2(canonical, text, 40); entry.key = Utf8(text); }
                PWSTR icon = nullptr;
                if (SUCCEEDED(current->GetIcon(selection.Get(), &icon)) && icon)
                { LoadMenuCommandIcon(entry, icon); CoTaskMemFree(icon); }
                if (flags & ECF_HASSUBCOMMANDS)
                {
                    ComPtr<IEnumExplorerCommand> enumeration;
                    if (SUCCEEDED(current->EnumSubCommands(&enumeration)) && enumeration)
                        for (unsigned index = 0; index < kMaximumEntries && count < kMaximumEntries; ++index)
                        {
                            ComPtr<IExplorerCommand> child; ULONG fetched = 0;
                            if (enumeration->Next(1, &child, &fetched) != S_OK || fetched != 1 || !child) break;
                            if (auto value = read(child.Get(), depth + 1)) entry.children.push_back(std::move(*value));
                        }
                    if (entry.children.empty()) entry.enabled = false;
                }
                else
                {
                    entry.token = next++;
                    Command action; action.source = source.get(); action.explorer = current; action.selection = selection;
                    commands.emplace(entry.token, std::move(action));
                }
                return entry;
            };
            if (auto entry = read(command.Get(), 0))
            {
                // A legacy aggregate may occasionally expose this modern verb.
                // Use its direct current state once, independent of that race.
                const auto key = Lower(Wide(entry->key));
                std::erase_if(reply.entries, [&](const auto& item) { return !key.empty() && Lower(Wide(item.key)) == key; });
                entry->registration = registration.id;
                reply.entries.push_back(std::move(*entry));
                menus.push_back(std::move(source));
            }
        }
        timing.Record("complete");
    }
    void NvidiaCompatibility(const Request &request, Reply &reply)
    {
        if (request.omitNvidiaCompatibility || !request.background || ResolveContext(request) != Context::Desktop || !request.sourceClsid.empty() ||
            !NvidiaControlPanelRegistered() || !HandlerEnabled(NvidiaControlPanelClsid)) return;
        progress("activate NVIDIA compatibility factory");
        CLSID clsid{};
        if (FAILED(CLSIDFromString(NvidiaControlPanelClsid, &clsid))) return;
        ComPtr<IContextMenu> extension;
        const auto factory = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&extension));
        if (!NvidiaCompatibilityRequired(request, true, true, factory)) return;
        progress("bind NVIDIA compatibility application");
        auto item = NvidiaControlPanelApplication();
        if (!item) return;
        auto source = std::make_unique<Native>();
        source->nvidiaApplication = item;
        PWSTR filePath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) && filePath)
        {
            source->directory = std::filesystem::path(filePath).parent_path().wstring();
            CoTaskMemFree(filePath);
        }
        // Display metadata does not need an application context-menu query.
        // A click resolves and validates its default open command separately.
        progress("read NVIDIA compatibility name");
        PWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || !name) return;
        Entry entry;
        entry.label = name; CoTaskMemFree(name);
        if (entry.label.empty()) return;
        entry.key = "{3d1975af-48c6-4f8e-a182-be0e08fa86a9}";
        entry.registration = NvidiaControlPanelRegistration;
        progress("read NVIDIA compatibility image");
        ComPtr<IShellItemImageFactory> image;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(item.As(&image)) && SUCCEEDED(image->GetImage({20, 20}, SIIGBF_ICONONLY, &bitmap)))
        {
            Bitmap(entry, bitmap);
            DeleteObject(bitmap);
        }
        entry.token = next++;
        commands[entry.token] = {source.get(), 0, false};
        reply.entries.push_back(std::move(entry));
        ++count;
        menus.push_back(std::move(source));
        MenuTrace("nvidia_compatibility", "installed_application");
    }
    Reply QueryShortcutObjects(const Request &request, Native &delegated,
        IShellFolder *selectionFolder, PCIDLIST_ABSOLUTE folderId,
        std::vector<PCIDLIST_ABSOLUTE> raw)
    {
        // ShellLink contributes a target menu to the usual aggregate. Build
        // file operations from the original selection instead. Do not load
        // lnkfile's ShellLink context handler back into this aggregate.
        auto actual = std::make_unique<Native>();
        actual->directory = delegated.directory; actual->paths = request.paths;
        actual->shortcutObjects = true;
        actual->folder = selectionFolder;
        ComPtr<IShellFolder> desktop;
        Pidl desktopId;
        const bool sameFolder = std::all_of(request.paths.begin(), request.paths.end(), [&](const auto &path) {
            return Lower(std::filesystem::path(path).parent_path().wstring()) == Lower(delegated.directory);
        });
        if (!sameFolder)
        {
            if (FAILED(SHGetDesktopFolder(&desktop)) ||
                FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktopId.value))) return {};
            selectionFolder = desktop.Get(); folderId = desktopId.value;
            actual->folder = desktop;
        }
        std::vector<PCUITEMID_CHILD> children;
        for (auto id : raw) children.push_back(sameFolder ? ILFindLastID(id) : reinterpret_cast<PCUITEMID_CHILD>(id));
        ASSOCIATIONELEMENT classes[] = {
            {ASSOCCLASS_STAR, nullptr, nullptr},
            {ASSOCCLASS_PROGID_STR, nullptr, L"AllFilesystemObjects"},
        };
        ComPtr<IQueryAssociations> associations;
        if (FAILED(AssocCreateForClasses(classes, static_cast<ULONG>(std::size(classes)), IID_PPV_ARGS(&associations)))) return {};
        DEFCONTEXTMENU description{};
        description.hwnd = window; description.psf = selectionFolder; description.pidlFolder = folderId;
        description.cidl = static_cast<UINT>(children.size()); description.apidl = children.data();
        description.punkAssociationInfo = associations.Get();
        if (FAILED(SHCreateDefaultContextMenu(&description, IID_PPV_ARGS(&actual->context))) ||
            FAILED(actual->context->QueryContextMenu(actual->menu, 0, 1, 0x7fff,
                CMF_ITEMMENU | (request.extended ? CMF_EXTENDEDVERBS : 0)))) return {};
        Reply reply; reply.entries = Read(*actual, actual->menu, "", 0, L"root", &request);
        IdentifyEntries(reply.entries);
        menus.push_back(std::move(actual));

        for (auto& entry : reply.entries) RegisteredBitmap(entry, request);
        if (count >= kMaximumEntries) return {};
        IdentifyEntries(reply.entries); reply.ok = true;
        return reply;
    }
    Reply Query(const Request &request)
    {
        if (!request.startPinOnly)
        {
            if (++ordinaryQueries > kMaximumWarmQueries)
                return Reply{false, {}, "menu worker reached its query lifetime"};
            // Vendor timers/window hooks may still refer to earlier COM menus.
            // Keep their objects/HMENUs alive until this bounded worker exits;
            // ReleaseMenu below removes their executable command mappings.
            for (auto &menu : menus) retiredMenus.push_back(std::move(menu));
            menus.clear();
        }
        ReleaseMenu();
        progress("validate paths");
        if (request.paths.empty() || request.paths.size() > 256)
            return {};
        for (const auto &path : request.paths)
            if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos ||
                GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                return {};
        if (request.background && request.paths.size() != 1)
            return {};
        if (request.startPinOnly && (request.background || request.catalogueOnly || request.paths.size() != 1 ||
            !request.sourceClsid.empty() || !request.sourceKey.empty())) return {};
        if (request.originalShortcutOnly && !CanQueryOriginalShortcutObjects(request)) return {};
        progress("load registered menu providers");
        const auto packages = !request.startPinOnly && !request.originalShortcutOnly && request.sourceClsid.empty()
            ? ReadPackagedMenuCommands() : std::vector<Registration>{};
        providers.Load(request, packages);
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
        // Desktop menus bind the virtual root below, but their view site must
        // retain the requested filesystem directory rather than enumerate the
        // desktop namespace and its network/application children.
        const auto viewFolder = folder;
        auto native = std::make_unique<Native>();
        native->directory = directory;
        native->paths = request.paths;
        if (request.originalShortcutOnly)
        {
            progress("query original shortcut objects");
            return QueryShortcutObjects(request, *native, folder.Get(), folderId.value, raw);
        }
        // Source queries supply metadata without retaining executable authority.
        // Popup clicks are always re-resolved through a fresh real aggregate.
        if (request.startPinOnly || !request.sourceClsid.empty())
        {
            native->metadataOnly = !request.startPinOnly;
            constexpr auto startHandler = L"{470C0EBD-5D73-4D58-9CED-E91E22E23282}";
            if (request.startPinOnly && !HandlerEnabled(startHandler)) return Reply{true, {}, {}};
            if (!request.sourceClsid.empty() && !HandlerEnabled(request.sourceClsid)) return Reply{true, {}, {}};
            if (!request.sourceClsid.empty() && !request.sourceKey.empty() &&
                !SourceRegistered(request.sourceClsid, request.sourceKey)) return Reply{true, {}, {}};
            CLSID clsid{};
            if (FAILED(CLSIDFromString(request.startPinOnly ? startHandler : request.sourceClsid.c_str(), &clsid))) return {};
            if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native->context))))
            {
                if (request.startPinOnly) return {};
                ComPtr<IExplorerCommand> command;
                if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&command)))) return {};
                ComPtr<IShellItemArray> selection;
                if (FAILED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(raw.size()), raw.data(), &selection))) return {};
                GUID canonical{}; PWSTR title = nullptr;
                if (FAILED(command->GetCanonicalName(&canonical))) return {};
                if (FAILED(command->GetTitle(selection.Get(), &title)) || !title) return {};
                Entry entry; entry.label = DecodeMenuLabel(title).text; CoTaskMemFree(title);
                wchar_t guid[40]{}; StringFromGUID2(canonical, guid, 40); entry.key = Utf8(guid);
                if (!request.sourceKey.empty() &&
                    (!HandlerEnabled(request.sourceClsid) || !SourceRegistered(request.sourceClsid, request.sourceKey)))
                    return Reply{true, {}, {}};
                Reply reply; reply.ok = true; reply.entries.push_back(std::move(entry)); return reply;
            }
            ComPtr<IDataObject> data;
            if (!request.background)
            {
                ComPtr<IShellItemArray> selection;
                if (FAILED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(raw.size()), raw.data(), &selection)) ||
                    FAILED(selection->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return {};
            }
            HKEY type = nullptr;
            if (!request.sourceKey.empty()) RegOpenKeyExW(HKEY_CLASSES_ROOT, request.sourceKey.c_str(), 0, KEY_READ, &type);
            ComPtr<IShellExtInit> initialize;
            const HRESULT initialized = SUCCEEDED(native->context.As(&initialize))
                ? initialize->Initialize(request.background || request.startPinOnly ? folderId.value : nullptr, data.Get(), type) : E_NOINTERFACE;
            if (type) RegCloseKey(type);
            if (FAILED(initialized)) return {};
        }
        else if (request.background)
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
                        IID_IContextMenu, nullptr, reinterpret_cast<void**>(native->context.GetAddressOf()))))
                    return {};
            }
            else
            {
                // BHID_SFUIObject only supports a flat, same-folder array.
                // Desktop-relative PIDLs represent the original objects from
                // every selected directory without substituting link targets.
                ComPtr<IShellFolder> desktop;
                if (FAILED(SHGetDesktopFolder(&desktop))) return {};
                std::vector<PCUITEMID_CHILD> children;
                for (auto id : raw) children.push_back(reinterpret_cast<PCUITEMID_CHILD>(id));
                if (FAILED(desktop->GetUIObjectOf(window, static_cast<UINT>(children.size()), children.data(),
                    IID_IContextMenu, nullptr, reinterpret_cast<void **>(native->context.GetAddressOf()))))
                    return {};
                folder = std::move(desktop);
            }
        }
        native->folder = request.background ? viewFolder : folder;
        progress("query context menu");
        const UINT flags = CMF_NORMAL | (request.background ? 0 : CMF_ITEMMENU) |
            (request.extended ? CMF_EXTENDEDVERBS : 0);
        if (!request.startPinOnly)
        {
            // Background ExplorerCommands have no selected items: Terminal
            // resolves its location from the site during GetState, and New+
            // retains that site for its first submenu enumeration. Item menus
            // use the same builder, which attaches their site after cold QCM.
            auto bound = std::move(native->context);
            const auto bind = [&](HWND, IContextMenu** context) { return bound.CopyTo(context); };
            HRESULT built;
            if (request.background)
                built = native->site.BuildMenu(native->folder.Get(), window, native->menu,
                    1, 0x7fff, flags, bind, native->context.GetAddressOf());
            else
            {
                const bool sameFolder = std::all_of(request.paths.begin(), request.paths.end(), [&](const auto& path) {
                    return Lower(std::filesystem::path(path).parent_path().wstring()) == Lower(directory);
                });
                ComPtr<IShellFolder> siteFolder = native->folder;
                if (!sameFolder && FAILED(SHGetDesktopFolder(&siteFolder))) return {};
                std::vector<PCUITEMID_CHILD> children;
                for (const auto id : raw) children.push_back(sameFolder ? ILFindLastID(id) : reinterpret_cast<PCUITEMID_CHILD>(id));
                built = native->site.BuildSelectionMenu(siteFolder.Get(), window, native->menu,
                    1, 0x7fff, flags, bind, static_cast<UINT>(children.size()), children.data(), native->context.GetAddressOf());
            }
            if (FAILED(built)) return {};
        }
        else if (FAILED(native->context->QueryContextMenu(native->menu, 0, 1, 0x7fff, flags))) return {};
        Reply reply;
        reply.entries = Read(*native, native->menu, "", 0, L"root", &request);
        if (request.startPinOnly)
            std::erase_if(reply.entries, [](const auto &entry) {
                const auto *pair = StatePairForVerb(entry.key);
                return !pair || pair->id != "state:start-pin";
            });
        if (!request.startPinOnly)
            for (auto &entry : reply.entries) RegisteredBitmap(entry, request);
        NvidiaCompatibility(request, reply);
        AppendPackagedCommands(request, native->folder.Get(), raw, packages, reply);
        if (count >= kMaximumEntries)
            return {};
        IdentifyEntries(reply.entries);
        if (!request.sourceClsid.empty() && !request.sourceKey.empty() &&
            (!HandlerEnabled(request.sourceClsid) || !SourceRegistered(request.sourceClsid, request.sourceKey)))
            return Reply{true, {}, {}};
        reply.ok = true;
        menus.push_back(std::move(native));
        return reply;
    }
    bool PrepareNative(const Request& request, const NativeMenuSelection& snapshot, POINT point)
    {
        ReleaseMenu();
        providers.Load(request, ReadPackagedMenuCommands());
        nativePopup = true;
        nativeLightTheme = snapshot.lightTheme;
        // Providers and Windows menu metrics obtain DPI from this HWND.
        // Move it before binding/querying, rather than scaling a primary-screen
        // menu bitmap or virtualizing coordinates after the menu was built.
        SetWindowPos(window, nullptr, point.x, point.y, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE);
        MenuTrace("native_session.dpi", "owner_on_target_monitor", 0, GetDpiForWindow(window));
        auto source = std::make_unique<Native>();
        source->paths = request.paths;
        source->directory = request.paths.empty() ? std::wstring{} :
            ShellInvocationDirectoryForItem(request.paths.front());
        std::vector<PCUITEMID_CHILD> children;
        const auto decode = [](const std::vector<unsigned char>& bytes) -> std::unique_ptr<Pidl> {
            if (bytes.size() < sizeof(USHORT) || bytes.size() > 65536) return {};
            size_t position = 0;
            while (position + sizeof(USHORT) <= bytes.size())
            {
                USHORT length = 0; memcpy(&length, bytes.data() + position, sizeof(length));
                if (!length)
                {
                    if (position + sizeof(USHORT) != bytes.size()) return {};
                    auto id = std::make_unique<Pidl>();
                    id->value = static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(bytes.size()));
                    if (!id->value) return {};
                    memcpy(id->value, bytes.data(), bytes.size()); return id;
                }
                if (length < sizeof(USHORT) || length > bytes.size() - position) return {};
                position += length;
            }
            return {};
        };
        if (!snapshot.children.empty())
        {
            if (request.background || snapshot.children.size() > 256) return false;
            size_t selectionSize = snapshot.parent.size();
            for (const auto& bytes : snapshot.children)
            {
                if (bytes.size() > 65536 || selectionSize > 2 * 1024 * 1024 - bytes.size()) return false;
                selectionSize += bytes.size();
            }
            auto parent = decode(snapshot.parent);
            if (!parent) return false;
            HRESULT bound = ILIsEmpty(parent->value) ? SHGetDesktopFolder(&source->folder) :
                SHBindToObject(nullptr, parent->value, nullptr, IID_PPV_ARGS(&source->folder));
            if (FAILED(bound)) return false;
            source->selection.push_back(std::move(parent));
            for (const auto& bytes : snapshot.children)
            {
                auto child = decode(bytes);
                if (!child || ILIsEmpty(child->value)) return false;
                children.push_back(child->value);
                source->selection.push_back(std::move(child));
            }
        }
        else
        {
            if (request.paths.empty() || request.paths.size() > 256 || (request.background && request.paths.size() != 1)) return false;
            std::unique_ptr<Pidl> firstParent;
            for (const auto& path : request.paths)
            {
                auto id = std::make_unique<Pidl>();
                if (path.empty() || FAILED(SHParseDisplayName(path.c_str(), nullptr, &id->value, 0, nullptr))) return false;
                if (request.background)
                {
                    if (request.context == Context::Desktop)
                    { if (FAILED(SHGetDesktopFolder(&source->folder))) return false; }
                    else if (FAILED(SHBindToObject(nullptr, id->value, nullptr, IID_PPV_ARGS(&source->folder)))) return false;
                }
                else
                {
                    auto parent = std::make_unique<Pidl>(); parent->value = ILCloneFull(id->value);
                    if (!parent->value || !ILRemoveLastID(parent->value)) return false;
                    if (!firstParent)
                    {
                        PCUITEMID_CHILD child = nullptr;
                        if (FAILED(SHBindToParent(id->value, IID_PPV_ARGS(&source->folder), &child))) return false;
                        firstParent = std::move(parent);
                    }
                    else if (!ILIsEqual(firstParent->value, parent->value)) return false;
                    children.push_back(ILFindLastID(id->value));
                }
                source->selection.push_back(std::move(id));
            }
        }
        auto* target = source.get();
        const auto bind = [&](HWND owner, IContextMenu** menu) {
            return request.background
                ? target->folder->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(menu))
                : target->folder->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(),
                    IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu));
        };
        const UINT flags = CMF_NORMAL | CMF_SYNCCASCADEMENU | (request.background ? CMF_EXPLORE : (CMF_ITEMMENU | CMF_CANRENAME)) |
            (request.extended ? CMF_EXTENDEDVERBS : 0);
        // Every target uses this same supervised single-query builder.
        // A partial/failed first result never selects a primer or UI fallback.
        ComPtr<IShellFolder> viewFolder = source->folder;
        if (request.background && request.context == Context::Desktop &&
            FAILED(SHBindToObject(nullptr, source->selection.front()->value, nullptr, IID_PPV_ARGS(&viewFolder)))) return false;
        const HRESULT built = request.background
            ? source->site.BuildMenu(viewFolder.Get(), window, source->menu, 1, 0x7fff,
                flags, bind, source->context.GetAddressOf())
            : source->site.BuildSelectionMenu(source->folder.Get(), window, source->menu, 1, 0x7fff,
                flags, bind, static_cast<UINT>(children.size()), children.data(), source->context.GetAddressOf());
        if (FAILED(built)) return false;
        menus.push_back(std::move(source));
        return true;
    }
    NativeMenuResult ShowNative(POINT point, UINT& invokeToken)
    {
        if (menus.empty()) return {false, false, {}, "native menu query failed"};
        auto& source = *menus.back();
        tracking = &source;
        native_menu_theme::ScopedTheme theme(nativeLightTheme);
        theme.ApplyToWindow(window);
        SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        SetWindowPos(window, HWND_TOPMOST, point.x, point.y, 1, 1, SWP_NOACTIVATE);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SetForegroundWindow(window);
        const UINT chosen = TrackPopupMenuEx(source.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            point.x, point.y, window, nullptr);
        tracking = nullptr;
        if (!chosen) { ShowWindow(window, SW_HIDE); return {true, false, {}, {}}; }
        auto verb = Lower(Verb(source, chosen));
        if (IsRootMenuCommand(source.menu, chosen))
        {
            if (verb == L"rename")
                return {true, false, "rename", {}};
            if (verb == L"delete")
                return {true, false, "delete", {}};
        }
        invokeToken = next++;
        commands[invokeToken] = {&source, chosen - 1, false, nullptr};
        return {true, true, Utf8(verb), {}};
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
            if (OwnedVerb(Verb(source, item.wID)) && IsRootMenuCommand(source.menu, item.wID))
                EnableMenuItem(menu, i, MF_BYPOSITION | MF_GRAYED);
            if (item.hSubMenu)
                DisableOwned(source, item.hSubMenu, depth + 1);
        }
    }
    bool Stateful(UINT token)
    {
        const auto found = commands.find(token);
        return found != commands.end() && !found->second.native &&
            !found->second.explorer &&
            StatePairForVerb(Utf8(Verb(*found->second.source, found->second.offset + 1)));
    }
    HRESULT Invoke(UINT token, POINT point, ShellInvocationOwner invocationOwner = {})
    {
        auto found = commands.find(token);
        if (found == commands.end() || invoked)
            return E_INVALIDARG;
        invoked = true;
        auto command = found->second;
        auto &source = *command.source;
        if (source.metadataOnly) return E_ACCESSDENIED;
        if (command.explorer)
        {
            if (!HandlerEnabled(source.explorerClass)) return E_ACCESSDENIED;
            EXPCMDSTATE state = ECS_HIDDEN;
            const HRESULT checked = command.explorer->GetState(command.selection.Get(), TRUE, &state);
            if (FAILED(checked) || (state & (ECS_HIDDEN | ECS_DISABLED))) return E_ACCESSDENIED;
            const HWND owner = invocationOwner.Resolve();
            source.site.SetInvocationOwner(owner ? owner : window);
            CoAllowSetForegroundWindow(command.explorer.Get(), nullptr);
            return command.explorer->Invoke(command.selection.Get(), nullptr);
        }
        if (source.nvidiaApplication)
        {
            // Recheck registration/security and the installed target at click.
            // A stale menu cannot launch an unregistered or blocked provider,
            // a replacement application, or a newly disabled default command.
            if (!NvidiaControlPanelRegistered() || !HandlerEnabled(NvidiaControlPanelClsid)) return E_ACCESSDENIED;
            auto current = NvidiaControlPanelApplication();
            int order = 1;
            if (!current || FAILED(source.nvidiaApplication->Compare(current.Get(), SICHINT_CANONICAL, &order)) || order) return E_INVALIDARG;
            source.context.Reset();
            DestroyMenu(source.menu); source.menu = CreatePopupMenu();
            if (!source.menu || FAILED(current->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&source.context))) ||
                FAILED(source.context->QueryContextMenu(source.menu, 0, 1, 0x7fff, CMF_DEFAULTONLY))) return E_FAIL;
            const auto refreshed = DefaultApplicationOpen(source.context.Get(), source.menu);
            if (!refreshed) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            command.offset = *refreshed;
        }
        else if (!source.shortcutObjects && (command.native || !shell_start_pin::CommandAction(source.context.Get(), command.offset)))
        {
            // Deferred children already belong to this live view/menu session.
            // Do not detach their site and recreate it between read and click.
            if (!source.site.HasView())
            {
                source.site.Initialize(source.folder.Get(), window);
                source.site.Attach(source.context.Get());
            }
            else if (!nativePopup)
                source.site.Attach(source.context.Get());
        }
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
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            command.offset = chosen - 1;
        }
        // Keep application-owned file operations out of the native fallback.
        // Their desktop/Dock semantics are handled by the existing host menu.
        if (!nativePopup && !source.nvidiaApplication && OwnedVerb(Verb(source, command.offset + 1)) &&
            IsRootMenuCommand(source.menu, command.offset + 1))
            return E_ACCESSDENIED;
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
            return E_ACCESSDENIED;
        // Exposed commands run in this supervised helper rather than in the
        // classic host menu. Start pinning still requires Explorer's thread,
        // and must use this live session's original shortcut, never its target.
        if (const auto result = shell_start_pin::Route(source.context.Get(), command.offset, source.paths,
                [&](shell_start_pin::Action action, const std::wstring &path) {
                    return startPin ? startPin(action, path, window, point) : E_NOTIMPL;
                }))
        {
            return *result; // A rejected/failed Start request must never invoke locally.
        }
        const HWND owner = invocationOwner.Resolve();
        const HWND commandOwner = owner ? owner : window;
        if (owner)
        {
            DWORD foregroundProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            // A slow fresh query must not steal focus after the user switches
            // apps. With the originating app still active, hand off to the
            // persistent input owner and consent broker immediately at click.
            if (foregroundProcess == invocationOwner.process || foregroundProcess == GetCurrentProcessId())
            {
                SetForegroundWindow(owner);
                AllowSetForegroundWindow(ASFW_ANY);
            }
        }
        source.site.SetInvocationOwner(commandOwner);
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_NOASYNC;
        invoke.hwnd = commandOwner;
        invoke.lpVerb = MAKEINTRESOURCEA(command.offset);
        invoke.lpVerbW = MAKEINTRESOURCEW(command.offset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = point;
        std::string ansi;
        SetShellInvocationDirectory(invoke, source.directory, ansi);
        return source.context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&invoke));
    }
};
} // namespace

std::uint64_t MenuCacheGeneration() { return Caches().generation; }
bool TakeMenuRegistryChanges() { auto &state = Caches(); state.Poll(); return std::exchange(state.dirty, false); }
std::vector<HANDLE> MenuRegistryWaitHandles(bool &complete)
{
    std::vector<HANDLE> handles;
    auto &cache = Caches();
    std::lock_guard lock(cache.notifications->mutex);
    complete = cache.changed != nullptr;
    if (cache.changed) handles.push_back(cache.changed);
    for (const auto &watch : cache.notifications->watches)
        watch->AppendWaitHandles(handles, complete);
    return handles;
}
void InvalidateMenuCache()
{
    SharedMenuCache().Invalidate();
    auto &cache = Caches();
    cache.epoch = SharedMenuCache().Epoch();
    ++cache.generation;
}

struct Session::Impl
{
    // Bounded ordinary workers and a separate system Start worker per STA.
    static thread_local std::vector<std::unique_ptr<Impl>> idle;
    settings_ipc::Channel channel;
    std::shared_ptr<settings_ipc::SettingsProcess> process = std::make_shared<settings_ipc::SettingsProcess>();
    std::optional<Reply> reply;
    std::optional<Reply> roots;
    unsigned selectionContexts = 0;
    std::set<UINT> statefulTokens;
    std::string stage;
    std::filesystem::path sampleDirectory;
    std::vector<std::filesystem::path> retiredSamples;
    ULONGLONG born = GetTickCount64(), started = 0;
    std::uint64_t generation = 0;
    DWORD timeout = 8000;
    bool delivered = false, detached = false, succeeded = false, startPinOnly = false;
    bool reusableQuery = false;
    unsigned queryUses = 0;
    bool ownsSessionSlot = false, preparationOnly = false, preparationReady = false, preparationFailed = false;
    void RemoveRetiredSamples()
    {
        for (const auto &directory : retiredSamples)
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
        retiredSamples.clear();
    }
    void RemoveSample()
    {
        if (!sampleDirectory.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(sampleDirectory, ignored);
            sampleDirectory.clear();
        }
    }
    ~Impl()
    {
        channel.Close();
        if (!detached) process->Stop();
        RemoveSample();
        RemoveRetiredSamples();
        if (ownsSessionSlot) (startPinOnly ? startPinSessions : sessions).fetch_sub(1);
    }
};
thread_local std::vector<std::unique_ptr<Session::Impl>> Session::Impl::idle;
Session::PreparationState Session::PollPreparedMenuWorker()
{
    const auto generation = MenuCacheGeneration();
    const auto now = GetTickCount64();
    std::erase_if(Impl::idle, [&](const auto &worker) {
        return worker->preparationOnly && (worker->preparationFailed || worker->generation != generation ||
            !worker->process->Running() || now - worker->born >= kWorkerLifetimeMs ||
            (!worker->preparationReady && now - worker->started >= worker->timeout));
    });
    for (const auto &worker : Impl::idle) if (worker->preparationOnly)
        return worker->preparationReady ? PreparationState::Ready : PreparationState::Pending;
    return PreparationState::Absent;
}
bool Session::PrepareFirstMenuWorker(DWORD timeoutMs)
{
    if (PollPreparedMenuWorker() != PreparationState::Absent) return true;
    auto worker = std::make_unique<Impl>();
    if (sessions.fetch_add(1) >= 2) { sessions.fetch_sub(1); return false; }
    worker->ownsSessionSlot = worker->preparationOnly = true;
    worker->generation = MenuCacheGeneration();
    worker->started = GetTickCount64();
    worker->timeout = std::clamp<DWORD>(timeoutMs, 100, 8000);
    worker->channel.Bind<void, std::string>("menu.progress", [p = worker.get()](std::string stage) {
        p->stage = std::move(stage);
    });
    worker->channel.Bind<void, bool>("menu.prepared", [p = worker.get()](bool ready) {
        p->preparationReady = ready; p->preparationFailed = !ready;
        MenuTrace("preparation", ready ? "ready" : "failed", double(GetTickCount64() - p->started));
    });
    try
    {
        worker->process->Start(worker->channel, L"--shell-menu-helper");
        worker->channel.Notify("menu.prepare");
        Impl::idle.push_back(std::move(worker));
        MenuTrace("preparation", "started");
        return true;
    }
    catch (...) { return false; }
}
void Session::ReleasePreparedMenuWorker()
{
    const auto count = std::count_if(Impl::idle.begin(), Impl::idle.end(), [](const auto &worker) { return worker->preparationOnly; });
    std::erase_if(Impl::idle, [](const auto &worker) { return worker->preparationOnly; });
    if (count) MenuTrace("preparation", "released", 0, static_cast<unsigned>(count));
}
Session::Session(const Request &request, DWORD queryTimeoutMs, bool usePreparedWorker)
{
    MenuTiming timing("helper_start");
    auto &counter = request.startPinOnly ? startPinSessions : sessions;
    bool acquiredSlot = false;
    try
    {
        PollPreparedMenuWorker();
        const auto generation = MenuCacheGeneration();
        auto &idle = Impl::idle;
        std::erase_if(idle, [=](const auto &worker) {
            return worker->generation != generation || !worker->process->Running() ||
                GetTickCount64() - worker->born >= kWorkerLifetimeMs ||
                (!worker->startPinOnly && worker->queryUses >= kMaximumWarmQueries) ||
                (!request.startPinOnly && !usePreparedWorker && !worker->startPinOnly && !worker->preparationOnly);
        });
        const auto available = std::find_if(idle.begin(), idle.end(), [&](const auto &worker) {
            if (request.startPinOnly) return worker->startPinOnly;
            if (!usePreparedWorker || worker->startPinOnly) return false;
            if (!worker->preparationOnly) return worker->reusableQuery;
            return !request.background && request.context != Context::Desktop && !request.catalogueOnly &&
                request.sourceClsid.empty() && request.sourceKey.empty() && !request.originalShortcutOnly;
        });
        bool reused = available != idle.end();
        if (reused) { impl_ = std::move(*available); idle.erase(available); }
        if (!impl_ || !impl_->ownsSessionSlot)
        {
            if (counter.fetch_add(1) >= (request.startPinOnly ? 1u : 2u))
            {
                counter.fetch_sub(1);
                if (!usePreparedWorker) throw settings_ipc::ProtocolError("too many Shell menus");
                // Preparation is optional. Yield its reserved slot to real
                // source/background work before refusing an interactive query.
                if (!request.startPinOnly) ReleasePreparedMenuWorker();
                if (request.startPinOnly || counter.fetch_add(1) >= 2u)
                {
                    if (!request.startPinOnly) counter.fetch_sub(1);
                    throw settings_ipc::ProtocolError("too many Shell menus");
                }
            }
            acquiredSlot = true;
        }
        if (!impl_) impl_ = std::make_unique<Impl>();
        impl_->startPinOnly = request.startPinOnly;
        impl_->reusableQuery = usePreparedWorker && !request.startPinOnly;
        ++impl_->queryUses;
        impl_->ownsSessionSlot = true; acquiredSlot = false;
        if (impl_->preparationOnly)
        {
            // The immediate popup never waits for preparation. Backgrounds keep
            // their original host so the exact selection/scope stays unchanged.
            if (!impl_->preparationReady || impl_->preparationFailed || request.background ||
                request.context == Context::Desktop || request.catalogueOnly ||
                !request.sourceClsid.empty() || !request.sourceKey.empty())
            {
                impl_->process->Stop(); impl_->channel.Close(); reused = false;
                impl_->born = GetTickCount64();
            }
            impl_->preparationOnly = impl_->preparationReady = impl_->preparationFailed = false;
        }
        impl_->generation = generation;
        impl_->started = GetTickCount64();
        impl_->delivered = impl_->succeeded = false;
        impl_->statefulTokens.clear();
        impl_->reply.reset();
        impl_->roots.reset();
        impl_->selectionContexts = 0;
        impl_->stage = "start helper";
        impl_->timeout = std::clamp<DWORD>(queryTimeoutMs, 100, 8000);
        impl_->channel.Bind<void, std::string>("menu.progress", [p = impl_.get()](std::string stage) {
            p->stage = std::move(stage);
        });
        impl_->channel.Bind<void, unsigned>("menu.selection_contexts", [p = impl_.get()](unsigned contexts) {
            p->selectionContexts = contexts;
        });
        impl_->channel.Bind<void, Reply>("menu.reply", [p = impl_.get()](Reply result) {
            if (!result.ok && result.error.empty()) result.error = "query failed at " + p->stage;
            p->reply = std::move(result);
        });
        impl_->channel.Bind<void, Reply>("menu.roots", [p = impl_.get()](Reply result) { p->roots = std::move(result); });
        impl_->channel.Bind<void>("menu.released", [p = impl_.get()] { p->RemoveRetiredSamples(); });
        auto query = ShellMenuRequest(request);
        PrepareSample(query, impl_->sampleDirectory);
        if (!impl_->process->Running()) impl_->process->Start(impl_->channel, L"--shell-menu-helper");
        impl_->channel.Notify("menu.query", query);
        timing.Record(reused ? "reused" : "cold");
    }
    catch (...)
    {
        impl_.reset();
        if (acquiredSlot) counter.fetch_sub(1);
        throw;
    }
}
Session::~Session()
{
    const bool startPinOnly = impl_->startPinOnly;
    // Reuse only a successful, non-invoked worker explicitly opted into by the
    // service. Its owning STA and loaded handlers stay warm, while each new
    // query gets a fresh menu/selection. Host retains old callback targets.
    if ((startPinOnly || (impl_->reusableQuery && impl_->queryUses < kMaximumWarmQueries &&
            GetTickCount64() - impl_->born < kWorkerLifetimeMs)) &&
        impl_->delivered && impl_->succeeded && !impl_->detached && impl_->process->Running() &&
        impl_->generation == Caches().generation)
    {
        try
        {
            impl_->reply.reset();
            if (!impl_->sampleDirectory.empty())
            {
                impl_->retiredSamples.push_back(std::move(impl_->sampleDirectory));
                impl_->sampleDirectory.clear();
            }
            if (startPinOnly)
            {
                // Start's dedicated system handler has the existing explicit
                // release protocol. Ordinary vendor callbacks retain targets.
                impl_->channel.Notify("menu.release");
                impl_->born = GetTickCount64();
            }
            const auto pooled = std::count_if(Impl::idle.begin(), Impl::idle.end(), [=](const auto &worker) {
                return worker->startPinOnly == startPinOnly;
            });
            if (pooled < (startPinOnly ? 1 : 2))
            {
                if (startPinOnly)
                {
                    impl_->ownsSessionSlot = false;
                    startPinSessions.fetch_sub(1);
                }
                Impl::idle.push_back(std::move(impl_));
            }
        }
        catch (...) { /* A disconnected worker is destroyed instead of pooled. */ }
    }
}
void Session::ReleaseIdleWorker() { Impl::idle.clear(); }
unsigned Session::SelectionContexts() const noexcept { return impl_->selectionContexts; }
DWORD Session::ProcessId() const noexcept { return impl_->process->ProcessId(); }
std::optional<Reply> Session::PollRoots() { return std::exchange(impl_->roots, {}); }
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
    impl_->succeeded = impl_->reply->ok;
    std::function<void(const std::vector<Entry> &)> identify = [&](const auto &entries) {
        for (const auto &entry : entries)
        { if (entry.token && !entry.native && StatePairForVerb(entry.key)) impl_->statefulTokens.insert(entry.token);
          identify(entry.children); }
    };
    identify(impl_->reply->entries);
    MenuTrace("query", impl_->succeeded ? "success" : "failure", double(GetTickCount64() - impl_->started), static_cast<unsigned>(impl_->reply->entries.size()));
    return std::move(impl_->reply);
}
void Session::Invoke(UINT token, POINT position, HWND owner)
{
    if (!impl_->delivered || !impl_->process->Running())
        throw settings_ipc::ProtocolError("Shell command session is no longer available");
    MenuTiming timing("invoke_ack");
    AllowSetForegroundWindow(impl_->process->ProcessId());
    const auto invocationOwner = ShellInvocationOwner::Capture(owner);
    impl_->succeeded = false; // An invoked or failed session can never return to the query pool.
    if (impl_->statefulTokens.contains(token))
    {
        // This STA is the service worker, never the desktop UI thread. Observe
        // completion of these bounded state changes before retiring the cache.
        const auto result = impl_->channel.CallWithTimeout<HRESULT>(12000, "menu.invoke-stateful",
            token, position.x, position.y, invocationOwner.window, invocationOwner.process, invocationOwner.thread);
        timing.Record("completed");
        if (FAILED(result)) throw settings_ipc::ProtocolError("Shell state command failed: " +
            std::to_string(static_cast<unsigned long>(result)));
    }
    // Request acknowledgement only queues arbitrary extension invocation;
    // code is dispatched afterwards, so this does not wait for a dialog.
    else
    {
        impl_->channel.CallWithTimeout<void>(1000, "menu.invoke", token, position.x, position.y,
            invocationOwner.window, invocationOwner.process, invocationOwner.thread);
        timing.Record("acknowledged");
    }
    auto process = impl_->process;
    impl_->detached = true;
    std::thread([process] {
        for (unsigned i = 0; i < 1200 && process->Running(); ++i)
            Sleep(100);
        process->Stop();
    }).detach();
}
struct NativeMenuSession::Impl
{
    settings_ipc::Channel channel;
    std::shared_ptr<settings_ipc::SettingsProcess> process = std::make_shared<settings_ipc::SettingsProcess>();
    std::optional<NativeMenuResult> result;
    ULONGLONG started = GetTickCount64();
    DWORD timeout = 8000;
    bool showing = false, invoked = false;
    ~Impl()
    {
        channel.Close();
        if (!invoked) process->Stop();
        else std::thread([child = process] {
            for (unsigned i = 0; i < 1200 && child->Running(); ++i) Sleep(100);
            child->Stop();
        }).detach();
    }
};
NativeMenuSelection NativeMenuSelection::Capture(IShellFolder* folder, UINT count, PCUITEMID_CHILD_ARRAY children)
{
    if (!folder || !children || !count || count > 256) throw settings_ipc::ProtocolError("invalid native selection");
    Pidl parent;
    if (FAILED(SHGetIDListFromObject(folder, &parent.value)) || !parent.value)
        throw settings_ipc::ProtocolError("cannot retain native selection parent");
    size_t total = 0;
    const auto copy = [&](PCUIDLIST_RELATIVE id) {
        const auto length = id ? ILGetSize(id) : 0;
        if (length < sizeof(USHORT) || length > 65536 || (total += length) > 2 * 1024 * 1024)
            throw settings_ipc::ProtocolError("invalid native selection size");
        const auto* bytes = reinterpret_cast<const unsigned char*>(id);
        return std::vector<unsigned char>(bytes, bytes + length);
    };
    NativeMenuSelection snapshot;
    snapshot.parent = copy(parent.value);
    for (UINT i = 0; i < count; ++i) snapshot.children.push_back(copy(children[i]));
    return snapshot;
}
NativeMenuSession::NativeMenuSession(const Request& request, POINT point, HWND owner,
    NativeMenuSelection selection, DWORD timeoutMs)
    : impl_(std::make_unique<Impl>())
{
    MenuTiming launch("native_session.launch");
    impl_->timeout = std::clamp<DWORD>(timeoutMs, 100, 8000);
    impl_->channel.Bind<void>("menu.native-showing", [p = impl_.get()] {
        p->showing = true;
        MenuTrace("native_session.showing", "ready", double(GetTickCount64() - p->started));
    });
    impl_->channel.Bind<void, NativeMenuResult>("menu.native-result", [p = impl_.get()](NativeMenuResult result) {
        p->invoked = result.invoked; p->result = std::move(result);
    });
    impl_->process->Start(impl_->channel, L"--shell-menu-helper");
    AllowSetForegroundWindow(impl_->process->ProcessId());
    const auto origin = ShellInvocationOwner::Capture(owner);
    impl_->channel.Notify("menu.native", ShellMenuRequest(request), selection, point.x, point.y, origin.window, origin.process, origin.thread,
        reinterpret_cast<std::uint64_t>(GetForegroundWindow()));
    launch.Record("scheduled");
}
NativeMenuSession::~NativeMenuSession() = default;
bool NativeMenuSession::Preparing() const { return !impl_->showing && !impl_->result; }
std::optional<NativeMenuResult> NativeMenuSession::Poll()
{
    if (!impl_->result && (!impl_->process->Running() ||
        (!impl_->showing && GetTickCount64() - impl_->started >= impl_->timeout)))
        impl_->result = NativeMenuResult{false, false, {}, "native menu helper stopped or timed out"};
    return std::exchange(impl_->result, {});
}

std::optional<int> TryRunHelper(QueryExecutor query, InvokeExecutor invoke, StartPinExecutor startPin,
    NativePopupExecutor nativePopup)
{
    if (!settings_ipc::IsSettingsProcessCommand(L"--shell-menu-helper"))
        return {};
    // The host supplies physical screen coordinates. Helpers bypass the
    // desktop's later DPI initialization and must establish the same context
    // before creating any menu owner or Shell view window.
    struct DpiScope
    {
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
    } dpiScope;
    if (FAILED(OleInitialize(nullptr)))
        return ERROR_DLL_INIT_FAILED;
    int result = 0;
    try
    {
        settings_ipc::Channel channel;
        settings_ipc::OpenInheritedSettingsChannel(channel, L"--shell-menu-helper");
        Host host(std::move(startPin));
        host.progress = [&](const auto &stage) { MenuTrace("helper.stage", stage); channel.Notify("menu.progress", stage); };
        host.rootPreview = [&](Reply roots) { channel.Notify("menu.roots", std::move(roots)); };
        bool preparationReceived = false;
        bool actualQueryReceived = false;
        bool invocationQueued = false;
        ULONGLONG lastQuery = GetTickCount64();
        ULONGLONG invokedAt = 0, dispatchedAt = 0;
        channel.SetDisconnected([&] {
            if (!invocationQueued)
                PostQuitMessage(0);
        });
        channel.Bind<void, Request, NativeMenuSelection, LONG, LONG, std::uint64_t, DWORD, DWORD, std::uint64_t>("menu.native",
            [&](Request request, NativeMenuSelection selection, LONG x, LONG y, std::uint64_t owner, DWORD ownerProcess, DWORD ownerThread, std::uint64_t foreground) {
            if (actualQueryReceived || invocationQueued) return;
            actualQueryReceived = invocationQueued = true;
            dispatchedAt = GetTickCount64();
            channel.Post([&, request = std::move(request), selection = std::move(selection), x, y, owner, ownerProcess, ownerThread, foreground] {
                UINT token = 0;
                NativeMenuResult result;
                const ShellInvocationOwner origin{owner, ownerProcess, ownerThread};
                if (const HWND sourceOwner = origin.Resolve())
                {
                    // Ownership survives the process boundary: source panels
                    // and their edit controls recognize this activation as an
                    // internal menu, while unrelated windows still deactivate.
                    SetWindowLongPtrW(host.window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(sourceOwner));
                    if (GetWindow(host.window, GW_OWNER) != sourceOwner)
                    {
                        channel.Notify("menu.native-result", NativeMenuResult{false, false, {}, "cannot attach native menu owner"});
                        invokedAt = GetTickCount64();
                        return;
                    }
                }
                if (nativePopup)
                    result = nativePopup(request, {x, y});
                else
                {
                    // Notify only after the expensive query, so preparation
                    // retains its deadline while a user's open menu does not.
                    host.progress = [&](const auto& stage) { MenuTrace("native_helper.stage", stage); };
                    host.rootPreview = {};
                    host.nativePopup = true;
                    if (!host.PrepareNative(request, selection, {x, y})) result = {false, false, {}, "native menu query failed"};
                    else if (request.catalogueOnly)
                    {
                        auto& source = *host.menus.back();
                        host.MenuMessage(source, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(source.menu), 0);
                        result = {true, false, "prepared:" + std::to_string(GetMenuItemCount(source.menu)), {}};
                    }
                    else
                    {
                        const HWND active = GetForegroundWindow();
                        DWORD activeProcess = 0; GetWindowThreadProcessId(active, &activeProcess);
                        if (active && reinterpret_cast<std::uint64_t>(active) != foreground &&
                            reinterpret_cast<std::uint64_t>(active) != owner && activeProcess != GetCurrentProcessId())
                            result = {true, false, {}, {}};
                        else
                        {
                            channel.Notify("menu.native-showing");
                            result = host.ShowNative({x, y}, token);
                        }
                    }
                }
                if (result.ok && (result.verb == "rename" || result.verb == "delete"))
                {
                    if (const HWND target = origin.Resolve())
                    {
                        AllowSetForegroundWindow(ownerProcess);
                        SetForegroundWindow(target);
                    }
                }
                channel.Notify("menu.native-result", result);
                if (token) host.Invoke(token, {x, y}, {owner, ownerProcess, ownerThread});
                ShowWindow(host.window, SW_HIDE);
                invokedAt = GetTickCount64();
            });
        });
        channel.Bind<void>("menu.prepare", [&] {
            if (actualQueryReceived || invocationQueued || preparationReceived) return;
            preparationReceived = true;
            // Prepare only the supervised STA, channel and owner HWND. No
            // temporary TXT or target menu is queried before a real request.
            channel.Notify("menu.prepared", true);
        });
        channel.Bind<void, Request>("menu.query", [&](Request request) {
            if (invocationQueued) return;
            actualQueryReceived = true;
            lastQuery = GetTickCount64();
            auto reply = query ? query(request) : host.Query(request);
            if (reply.ok)
            {
                unsigned contexts = 0;
                if (request.background || request.context == Context::Desktop)
                    contexts = ContextBit(ResolveContext(request));
                else for (const auto &path : request.paths)
                {
                    const auto attributes = GetFileAttributesW(path.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES) { contexts = 0; break; }
                    contexts |= ContextBit(attributes & FILE_ATTRIBUTE_DIRECTORY ? Context::Folder : Context::File);
                }
                // This separate notification leaves persisted Reply data intact.
                // Network/reconnect waits stay within the helper's query deadline.
                channel.Notify("menu.selection_contexts", contexts);
            }
            channel.Notify("menu.reply", std::move(reply));
        });
        channel.Bind<void>("menu.release", [&] {
            if (invocationQueued) return;
            host.ReleaseMenu();
            channel.Notify("menu.released");
        });
        channel.Bind<void, UINT, LONG, LONG, std::uint64_t, DWORD, DWORD>("menu.invoke",
            [&](UINT token, LONG x, LONG y, std::uint64_t ownerWindow, DWORD ownerProcess, DWORD ownerThread) {
            if (invocationQueued)
                return;
            invocationQueued = true;
            dispatchedAt = GetTickCount64();
            channel.Post([&, token, x, y, ownerWindow, ownerProcess, ownerThread] {
                if (invoke) invoke(token, {x, y});
                else host.Invoke(token, {x, y}, {ownerWindow, ownerProcess, ownerThread});
                invokedAt = GetTickCount64();
            });
        });
        channel.Bind<HRESULT, UINT, LONG, LONG, std::uint64_t, DWORD, DWORD>("menu.invoke-stateful",
            [&](UINT token, LONG x, LONG y, std::uint64_t ownerWindow, DWORD ownerProcess, DWORD ownerThread) {
            if (invocationQueued || (!invoke && !host.Stateful(token))) return E_INVALIDARG;
            invocationQueued = true;
            dispatchedAt = GetTickCount64();
            HRESULT execution = S_OK;
            if (invoke) invoke(token, {x, y});
            else execution = host.Invoke(token, {x, y}, {ownerWindow, ownerProcess, ownerThread});
            invokedAt = GetTickCount64();
            return execution;
        });
        MSG msg{};
        bool running = true;
        while (running && (dispatchedAt ? GetTickCount64() - dispatchedAt < 120000
                                       : GetTickCount64() - lastQuery < kWorkerLifetimeMs))
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
        if (host.nativePopup)
        {
            // A native copy/cut provider can publish a delayed OLE IDataObject.
            // Materialize our own clipboard data before retiring its STA; never
            // flush a clipboard subsequently owned by another application.
            DWORD clipboardProcess = 0;
            const HWND clipboardOwner = GetClipboardOwner();
            if (clipboardOwner && GetWindowThreadProcessId(clipboardOwner, &clipboardProcess) &&
                clipboardProcess == GetCurrentProcessId())
            {
                const HRESULT flushed = OleFlushClipboard();
                MenuTrace("native_session.clipboard", SUCCEEDED(flushed) ? "retained" : "flush_failed");
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
bool CanQueryOriginalShortcutObjects(const Request &request)
{
    return !request.background && !request.catalogueOnly && !request.startPinOnly &&
        request.context != Context::Desktop && request.sourceClsid.empty() && request.sourceKey.empty() &&
        std::any_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
            return lstrcmpiW(PathFindExtensionW(path.c_str()), L".lnk") == 0;
        });
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
