#include "shell_extension_catalogue.h"
#include "shell_extension_diagnostics.h"
#include "menu_label.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <shlwapi.h>
#include <shlobj.h>
#include <msxml6.h>
#include <wrl/client.h>

namespace snowdesktop::shell_extensions
{
namespace
{
using Microsoft::WRL::ComPtr;
std::wstring Lower(std::wstring value) { for (auto &c : value) c = towlower(c); return value; }
std::string Utf8(const std::wstring &s)
{
    const auto n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string value(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), value.data(), n, nullptr, nullptr);
    return value;
}
struct Key
{
    HKEY value = nullptr;
    Key(HKEY root, const std::wstring &path) { RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &value); }
    ~Key() { if (value) RegCloseKey(value); }
    Key(const Key &) = delete;
};
std::vector<std::wstring> Children(HKEY root, const std::wstring &path)
{
    Key key(root, path);
    std::vector<std::wstring> result;
    if (!key.value) return result;
    for (DWORD i = 0; i < 65536; ++i)
    {
        wchar_t name[1024]{}; DWORD count = 1024;
        if (RegEnumKeyExW(key.value, i, name, &count, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        result.emplace_back(name, count);
    }
    return result;
}
std::wstring Read(HKEY root, const std::wstring &path, const wchar_t *name = nullptr)
{
    wchar_t text[32768]{}; DWORD bytes = sizeof(text);
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, text, &bytes)) return {};
    return text;
}
bool Has(HKEY root, const std::wstring &path, const wchar_t *name)
{
    DWORD bytes = 0;
    return RegGetValueW(root, path.c_str(), name, RRF_RT_ANY, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}
bool Blocked(const std::wstring &clsid)
{
    if (clsid.empty()) return false;
    for (auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        if (Has(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked", clsid.c_str())) return true;
    return false;
}
std::wstring Localized(std::wstring text)
{
    wchar_t buffer[4096]{};
    if (!text.empty() && text.front() == L'@' && SUCCEEDED(SHLoadIndirectString(text.c_str(), buffer, 4096, nullptr))) return buffer;
    return text;
}
std::uint64_t Hash(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) { hash ^= std::to_integer<unsigned char>(byte); hash *= 1099511628211ull; }
    return hash;
}
void LoadIcon(Entry &entry, std::wstring location)
{
    if (location.empty()) return;
    wchar_t path[32768]{};
    wcsncpy_s(path, location.c_str(), _TRUNCATE);
    const auto index = PathParseIconLocationW(path);
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(path, index, 0, nullptr, &icon, MAKELONG(20, 20))) || !icon) return;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 20;
    info.bmiHeader.biHeight = -20; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (dc && bitmap && pixels)
    {
        const auto old = SelectObject(dc, bitmap);
        memset(pixels, 0, 1600);
        if (DrawIconEx(dc, 0, 0, icon, 20, 20, 0, nullptr, DI_NORMAL))
        {
            entry.width = entry.height = 20;
            entry.pixels.assign(static_cast<unsigned char *>(pixels), static_cast<unsigned char *>(pixels) + 1600);
        }
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    DestroyIcon(icon);
}
template<class T> void Unique(std::vector<T> &items) { std::sort(items.begin(), items.end()); items.erase(std::unique(items.begin(), items.end()), items.end()); }
struct Scanner
{
    HKEY classes;
    Catalogue result;
    std::map<std::string, size_t> ids;
    void Add(Registration row, const std::wstring &icon)
    {
        if (row.types.empty()) row.types.push_back(L"*");
        if (row.id.empty() || result.rows.size() >= 16384) return;
        row.display.provider = row.id;
        if (const auto it = ids.find(row.id); it != ids.end())
        {
            auto &existing = result.rows[it->second];
            existing.contexts |= row.contexts;
            existing.sources.insert(existing.sources.end(), row.sources.begin(), row.sources.end());
            existing.types.insert(existing.types.end(), row.types.begin(), row.types.end());
            existing.verbs.insert(existing.verbs.end(), row.verbs.begin(), row.verbs.end());
            existing.systemEnabled |= row.systemEnabled;
            return;
        }
        LoadIcon(row.display, icon);
        ids[row.id] = result.rows.size();
        result.rows.push_back(std::move(row));
    }
    void Root(const std::wstring &root, unsigned contexts, const std::vector<std::wstring> &types = {})
    {
        for (const auto &verb : Children(classes, root + L"\\shell"))
        {
            const auto path = root + L"\\shell\\" + verb;
            const auto handler = Read(classes, path, L"ExplorerCommandHandler");
            Registration row;
            row.kind = RegistrationKind::Verb;
            row.id = "reg:" + Utf8(Lower(path));
            row.contexts = contexts; row.types = types; row.sources = {path};
            row.verbs = {Utf8(Lower(verb))};
            if (!handler.empty()) row.verbs.push_back(Utf8(Lower(handler)));
            row.systemEnabled = !Has(classes, path, L"LegacyDisable") && !Has(classes, path, L"ProgrammaticAccessOnly") && !Blocked(handler);
            auto label = Read(classes, path, L"MUIVerb");
            if (label.empty()) label = Read(classes, path);
            if (label.empty()) label = verb;
            row.display.label = DecodeMenuLabel(Localized(label)).text;
            auto icon = Read(classes, path, L"Icon");
            if (icon.empty() && !handler.empty()) icon = Read(classes, L"CLSID\\" + handler + L"\\InprocServer32");
            if (icon.empty())
            {
                auto command = Read(classes, path + L"\\command");
                if (!command.empty()) { wchar_t exe[32768]{}; wcsncpy_s(exe, command.c_str(), _TRUNCATE); PathRemoveArgsW(exe); PathUnquoteSpacesW(exe); icon = exe; }
            }
            // Include values that affect applicability, not volatile registry write times.
            row.revision = Hash(settings_ipc::Pack(Read(classes, path + L"\\command"), handler, Read(classes, path, L"AppliesTo"), Read(classes, path, L"MultiSelectModel"), Has(classes, path, L"Extended"), Read(classes, path, L"SubCommands")));
            Add(std::move(row), icon);
        }
        for (const auto &name : Children(classes, root + L"\\shellex\\ContextMenuHandlers"))
        {
            const auto path = root + L"\\shellex\\ContextMenuHandlers\\" + name;
            auto clsid = Read(classes, path);
            if (clsid.empty() && !name.empty() && name.front() == L'{') clsid = name;
            GUID guid{};
            if (FAILED(CLSIDFromString(clsid.c_str(), &guid))) continue;
            Registration row;
            row.kind = RegistrationKind::Handler; row.id = "clsid:" + Utf8(Lower(clsid));
            row.contexts = contexts; row.types = types; row.sources = {path}; row.verbs = {Utf8(Lower(clsid))};
            row.systemEnabled = !Blocked(clsid);
            auto label = Read(classes, L"CLSID\\" + clsid);
            row.display.label = Localized(label.empty() ? name : label);
            const auto module = Read(classes, L"CLSID\\" + clsid + L"\\InprocServer32");
            row.revision = Hash(settings_ipc::Pack(module));
            auto icon = Read(classes, L"CLSID\\" + clsid + L"\\DefaultIcon");
            Add(std::move(row), icon.empty() ? module : icon);
        }
    }
    static std::wstring Attribute(IXMLDOMNode *node, const wchar_t *name)
    {
        ComPtr<IXMLDOMNamedNodeMap> attributes; ComPtr<IXMLDOMNode> value;
        BSTR key = SysAllocString(name), text = nullptr;
        if (node && SUCCEEDED(node->get_attributes(&attributes)) && attributes) attributes->getNamedItem(key, &value);
        if (value) value->get_text(&text);
        std::wstring result = text ? text : L"";
        SysFreeString(key); SysFreeString(text); return result;
    }
    void Packages()
    {
        const std::wstring root = L"Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
        for (const auto &package : Children(classes, root))
        {
            const auto directory = Read(classes, root + L"\\" + package, L"PackageRootFolder");
            if (directory.empty()) continue;
            ComPtr<IXMLDOMDocument2> document;
            if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document)))) return;
            document->put_async(VARIANT_FALSE); document->put_validateOnParse(VARIANT_FALSE); document->put_resolveExternals(VARIANT_FALSE);
            VARIANT filename{}; VariantInit(&filename); filename.vt = VT_BSTR;
            filename.bstrVal = SysAllocString((directory + L"\\AppxManifest.xml").c_str());
            VARIANT_BOOL loaded = VARIANT_FALSE; document->load(filename, &loaded); VariantClear(&filename);
            if (!loaded) continue;
            BSTR query = SysAllocString(L"//*[local-name()='Extension' and @Category='windows.fileExplorerContextMenus']//*[local-name()='Verb']");
            ComPtr<IXMLDOMNodeList> nodes; document->selectNodes(query, &nodes); SysFreeString(query);
            if (!nodes) continue;
            long count = 0; nodes->get_length(&count);
            for (long i = 0; i < count; ++i)
            {
                ComPtr<IXMLDOMNode> verb, parent; nodes->get_item(i, &verb); verb->get_parentNode(&parent);
                const auto clsid = Attribute(verb.Get(), L"Clsid"), id = Attribute(verb.Get(), L"Id");
                const auto type = Attribute(parent.Get(), L"Type");
                if (clsid.empty() || type.empty()) continue;
                const auto first = package.find(L'_'), last = package.rfind(L'_');
                const auto family = first != std::wstring::npos && last > first ? package.substr(0, first) + package.substr(last) : package;
                Registration row; row.kind = RegistrationKind::Packaged;
                row.id = "package:" + Utf8(Lower(family + L":" + clsid));
                row.sources = {L"package:" + package + L":" + id}; row.verbs = {Utf8(Lower(clsid))};
                row.contexts = type == L"Directory" ? ContextBit(Context::Folder) : type == L"Directory\\Background" ? ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop) : ContextBit(Context::File);
                if (type != L"*" && type != L"Directory" && type != L"Directory\\Background") row.types = {Lower(type)};
                row.systemEnabled = !Blocked(clsid); row.display.label = family.substr(0, family.find(L'_'));
                row.revision = Hash(settings_ipc::Pack(package, id, clsid));
                Add(std::move(row), L"");
            }
        }
    }
};
}
Catalogue ReadCatalogue(HKEY classes, bool packages)
{
    MenuTiming timing("catalogue");
    Scanner scanner{classes};
    const unsigned files = ContextBit(Context::File), folders = ContextBit(Context::Folder);
    scanner.Root(L"*", files); scanner.Root(L"AllFilesystemObjects", files | folders);
    scanner.Root(L"Directory", folders); scanner.Root(L"Folder", folders); scanner.Root(L"Drive", folders);
    scanner.Root(L"Directory\\Background", ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop));
    scanner.Root(L"DesktopBackground", ContextBit(Context::Desktop));
    const auto roots = Children(classes, L"");
    std::map<std::wstring, std::vector<std::wstring>> types;
    for (const auto &name : roots)
        if (!name.empty() && name.front() == L'.')
        {
            types[Lower(name)].push_back(Lower(name));
            const auto prog = Read(classes, name); if (!prog.empty()) types[Lower(prog)].push_back(Lower(name));
            const auto perceived = Read(classes, name, L"PerceivedType");
            if (!perceived.empty()) types[L"systemfileassociations\\" + Lower(perceived)].push_back(Lower(name));
            types[L"systemfileassociations\\" + Lower(name)].push_back(Lower(name));
            for (const auto &progId : Children(classes, name + L"\\OpenWithProgids")) types[Lower(progId)].push_back(Lower(name));
            if (classes == HKEY_CLASSES_ROOT)
            {
                const auto choice = Read(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + name + L"\\UserChoice", L"ProgId");
                if (!choice.empty()) types[Lower(choice)].push_back(Lower(name));
            }
        }
    const std::set<std::wstring> special{L"*", L"allfilesystemobjects", L"directory", L"folder", L"drive", L"desktopbackground", L"clsid", L"interface", L"typelib"};
    for (const auto &name : roots)
        if (!special.contains(Lower(name)))
        {
            auto &extensions = types[Lower(name)];
            if (extensions.empty()) extensions.push_back(L"progid:" + Lower(name));
            scanner.Root(name, files, extensions);
        }
    for (const auto &name : Children(classes, L"SystemFileAssociations"))
        scanner.Root(L"SystemFileAssociations\\" + name, files, types[L"systemfileassociations\\" + Lower(name)]);
    if (packages) scanner.Packages();
    for (auto &row : scanner.result.rows)
    {
        Unique(row.sources); Unique(row.types); Unique(row.verbs);
        row.revision = Hash(settings_ipc::Pack(row.revision, row.sources, row.types, row.verbs, row.contexts, row.systemEnabled, row.display.label, row.display.pixels));
    }
    std::sort(scanner.result.rows.begin(), scanner.result.rows.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    scanner.result.revision = Hash(settings_ipc::Pack(scanner.result.rows));
    timing.Record("complete", static_cast<unsigned>(scanner.result.rows.size()));
    return std::move(scanner.result);
}
bool Applies(const Registration &row, const Request &request)
{
    if (!(row.contexts & ContextBit(ResolveContext(request)))) return false;
    if (row.types.empty() || std::find(row.types.begin(), row.types.end(), L"*") != row.types.end() || request.paths.empty()) return true;
    return std::all_of(request.paths.begin(), request.paths.end(), [&](const auto &p) {
        return std::find(row.types.begin(), row.types.end(), Lower(std::filesystem::path(p).extension().wstring())) != row.types.end();
    });
}
void Associate(Catalogue &catalogue, const Request &request, Reply &reply)
{
    if (!reply.ok) return;
    for (auto &entry : reply.entries)
    {
        if (entry.separator) continue;
        Registration *match = nullptr;
        auto key = entry.key; std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        for (auto &row : catalogue.rows)
            if (row.systemEnabled && Applies(row, request) && !key.empty() && std::find(row.verbs.begin(), row.verbs.end(), key) != row.verbs.end())
            {
                if (match) { match = nullptr; break; }
                match = &row;
            }
        if (!match) continue; // Labels and submenu contents never establish ownership.
        match->linked = true;
        const Association association{entry.provider, match->id, ResolveContext(request)};
        if (std::none_of(catalogue.associations.begin(), catalogue.associations.end(), [&](const auto &a) { return a.provider == association.provider && a.registration == association.registration && a.context == association.context; })) catalogue.associations.push_back(association);
        entry.registration = match->id;
    }
}
void MigrateAssociations(Preferences &prefs, const Catalogue &catalogue)
{
    for (const auto &a : catalogue.associations)
    {
        if (std::any_of(catalogue.associations.begin(), catalogue.associations.end(), [&](const auto &b) { return b.provider == a.provider && b.context == a.context && b.registration != a.registration; })) continue;
        const bool configured = std::any_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto &r) { return r.id == a.registration && r.category == CategoryOf(a.context); }) ||
            std::any_of(prefs.overrides.begin(), prefs.overrides.end(), [&](const auto &r) { return r.id == a.registration && r.context == a.context; });
        if (!configured && !IsHidden(prefs, a.provider, a.context)) SetOverride(prefs, a.registration, a.context, Visibility::Show);
    }
}
}
