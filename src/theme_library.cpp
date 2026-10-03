#include "theme_library.h"

#include <objbase.h>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace snowdesktop::themes
{
namespace
{
bool Fail(std::string& error, const char* reason) { error = reason; return false; }
std::string Quote(std::string_view value)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
bool Utf8(std::string_view text)
{
    return !text.empty() && text.size() <= kMaximumPackageBytes &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0) > 0;
}
bool Text(std::string_view text, std::size_t maximum)
{
    if (text.empty() || text.size() > maximum || !Utf8(text)) return false;
    for (unsigned char c : text) if (c < 32) return false;
    return true;
}
bool Id(std::string_view id)
{
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '-')) return false;
    return true;
}
bool Keys(const JsonValue& object, std::initializer_list<std::string_view> keys)
{
    if (!object.IsObject()) return false;
    for (const auto& [key, value] : object.object)
    {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) return false;
    }
    return true;
}
bool String(const JsonValue& object, std::string_view key, std::string& out)
{
    const auto* value = object.Find(key);
    if (!value || !value->IsString()) return false;
    out = value->string;
    return true;
}
bool Document(std::string_view text, JsonValue& out, std::string& error)
{
    if (!Utf8(text)) return Fail(error, "invalidUtf8");
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text)
    {
        if (escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') quoted = !quoted;
        if (quoted) continue;
        if ((c == '{' || c == '[') && ++depth > 32) return Fail(error, "invalidPackage");
        if (c == '}' || c == ']') --depth;
    }
    if (!ParseJson(text, out, &error) || !out.IsObject()) return Fail(error, "invalidPackage");
    return true;
}
template<class V> void LayoutFields(V visit)
{
#define FIELD(name) visit(#name, &QuickNavigationLayout::name)
    FIELD(expandedWidth); FIELD(collapsedWidth); FIELD(maximumHeight); FIELD(visibleRows);
    FIELD(padding); FIELD(searchHeight); FIELD(iconSize); FIELD(gridGap); FIELD(rowGap);
    FIELD(fontSize); FIELD(secondaryFontSize); FIELD(searchFontSize); FIELD(resultRowHeight);
    FIELD(labelLines); FIELD(cornerRadius); FIELD(searchRadius); FIELD(tabRadius); FIELD(itemRadius);
#undef FIELD
}
bool Colors(const std::map<std::string, std::string>& colors)
{
    // The whitelist matches the renderer's detailed palette, excluding data.
    static const std::set<std::string> allowed = {
        "searchBg", "searchBorder", "searchText", "searchPlaceholder", "searchFocus", "typeFill", "typeText",
        "tabDefaultFill", "tabDefaultStroke", "tabText", "tabHoverFill", "tabHoverStroke", "tabActiveFill",
        "tabActiveStroke", "tabActiveText", "tabHoverText", "iconPlateFill", "iconPlateBorder", "headerText",
        "headerSeparator", "resultFill", "resultBorder", "appNameText", "appTypeText", "appRowHoverFill",
        "appRowHoverStroke", "scrollTrack", "scrollThumbDefault", "scrollThumbHover", "selectedFill",
        "selectedBorder", "selectedText", "itemText", "itemHoverFill", "itemHoverStroke", "emptyText"};
    for (const auto& [key, value] : colors)
    {
        if (!allowed.contains(key) || value.size() != 7 || value[0] != '#') return false;
        for (std::size_t i = 1; i < value.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}
std::string EncodeTheme(const Theme& theme)
{
    if (Builtin(theme.id)) return "{\"id\":" + Quote(theme.id) + '}';
    const auto appearance = EncodePanelAppearance(theme.appearance, true);
    if (appearance.empty()) return {};
    std::string out = "{\"id\":" + Quote(theme.id) + ",\"name\":" + Quote(theme.name) +
        ",\"appearance\":" + appearance;
    if (theme.kind == Kind::Global)
        out += ",\"scopes\":" + std::to_string(theme.scopes) + ",\"quickPanel\":" + Quote(theme.quickPanel) +
            ",\"popup\":" + Quote(theme.popup);
    else if (theme.kind == Kind::QuickPanel)
    {
        out += ",\"layout\":{";
        bool first = true;
        LayoutFields([&](auto key, auto member) {
            if (!first) out += ','; first = false;
            out += Quote(key) + ':' + std::to_string(theme.layout.*member);
        });
        out += "},\"colors\":{"; first = true;
        for (const auto& [key, value] : theme.colors)
        {
            if (!first) out += ','; first = false;
            out += Quote(key) + ':' + Quote(value);
        }
        out += '}';
    }
    return out + '}';
}
bool DecodeTheme(const JsonValue& json, Kind kind, Theme& theme)
{
    Theme out; out.kind = kind;
    if (!String(json, "id", out.id) || !Id(out.id)) return false;
    if (out.id.starts_with("builtin/"))
    {
        auto builtin = Builtin(out.id);
        if (!builtin || builtin->kind != kind || !Keys(json, {"id"})) return false;
        theme = *builtin;
        return true;
    }
    if (!String(json, "name", out.name) || !Text(out.name, 256)) return false;
    if (kind == Kind::Global && !Keys(json, {"id", "name", "appearance", "scopes", "quickPanel", "popup"})) return false;
    if (kind == Kind::QuickPanel && !Keys(json, {"id", "name", "appearance", "layout", "colors"})) return false;
    if (kind == Kind::Popup && !Keys(json, {"id", "name", "appearance"})) return false;
    const auto* appearance = json.Find("appearance");
    if (!appearance || !Keys(*appearance, {"backgroundR", "backgroundG", "backgroundB", "opacity", "borderR",
        "borderG", "borderB", "borderOpacity", "borderWidth", "highlightWidth", "highlightStrength", "blurRadius",
        "cornerRadius", "glass", "acrylic", "highlight", "edgeLight", "contentTheme", "gradient", "gradientEndOpacity"}) ||
        !DecodePanelAppearance(*appearance, out.appearance, true)) return false;
    if (kind == Kind::Global)
    {
        const auto* scopes = json.Find("scopes");
        if (!scopes || !scopes->IsNumber() || scopes->number < 1 || scopes->number > static_cast<double>(All) ||
            std::floor(scopes->number) != scopes->number || !String(json, "quickPanel", out.quickPanel) ||
            !String(json, "popup", out.popup)) return false;
        out.scopes = static_cast<unsigned>(scopes->number);
    }
    else if (kind == Kind::QuickPanel)
    {
        const auto* layout = json.Find("layout");
        const auto* colors = json.Find("colors");
        if (!layout || !layout->IsObject() || !colors || !colors->IsObject()) return false;
        bool valid = true;
        std::set<std::string> names;
        LayoutFields([&](auto key, auto member) {
            names.insert(key);
            const auto* value = layout->Find(key);
            if (!value || !value->IsNumber() || value->number < 0 || value->number > 1800 ||
                std::floor(value->number) != value->number) valid = false;
            else out.layout.*member = static_cast<int>(value->number);
        });
        for (const auto& [key, value] : layout->object) { (void)value; if (!names.contains(key)) valid = false; }
        for (const auto& [key, value] : colors->object)
        { if (!value.IsString()) return false; out.colors.emplace(key, value.string); }
        NavigationSettings navigation; navigation.layout = out.layout;
        NormalizeNavigationSettings(navigation);
        if (!valid || navigation.layout != out.layout || !Colors(out.colors)) return false;
    }
    theme = std::move(out);
    return true;
}
std::string Sections(const Package& package)
{
    std::string out;
    for (const auto kind : {Kind::Global, Kind::QuickPanel, Kind::Popup})
    {
        out += "," + Quote(KindName(kind)) + ":[";
        bool first = true;
        for (const auto& [id, theme] : package)
        {
            (void)id;
            if (theme.kind != kind) continue;
            if (!first) out += ','; first = false;
            out += EncodeTheme(theme);
        }
        out += ']';
    }
    return out;
}
bool ReadSections(const JsonValue& document, Package& package, std::string& error, bool allowEmpty)
{
    Package out;
    for (const auto kind : {Kind::Global, Kind::QuickPanel, Kind::Popup})
    {
        const auto* section = document.Find(KindName(kind));
        if (!section || !section->IsArray() || section->array.size() > 512) return Fail(error, "invalidPackage");
        for (const auto& entry : section->array)
        {
            Theme theme;
            if (!DecodeTheme(entry, kind, theme) || !out.emplace(theme.id, theme).second) return Fail(error, "invalidPackage");
        }
    }
    if (!allowEmpty && out.empty()) return Fail(error, "emptyPackage");
    if (!Validate(out, error)) return false;
    package = std::move(out);
    return true;
}
bool ReadFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaximumPackageBytes) return Fail(error, "readFailed");
    std::ifstream file(path, std::ios::binary);
    if (!file) return Fail(error, "readFailed");
    text.resize(static_cast<std::size_t>(size));
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file || file.peek() != std::char_traits<char>::eof()) return Fail(error, "readFailed");
    return true;
}
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool AtomicWrite(const std::filesystem::path& path, std::string_view text, std::string& error)
{
    if (text.empty() || text.size() > kMaximumPackageBytes) return Fail(error, "writeFailed");
    auto temporary = path;
    const auto suffix = CreateId();
    if (suffix.empty()) return Fail(error, "writeFailed");
    temporary += L".pending-" + std::wstring(suffix.begin() + 6, suffix.end());
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return Fail(error, "writeFailed");
    DWORD written = 0;
    const bool stored = WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file.value);
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    if (!stored || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { DeleteFileW(temporary.c_str()); return Fail(error, "writeFailed"); }
    return true;
}
}

std::string CreateId()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t buffer[40]{};
    if (StringFromGUID2(guid, buffer, 40) == 0) return {};
    std::string id = "theme/";
    for (int i = 1; i < 37; ++i) id += static_cast<char>(std::tolower(static_cast<unsigned char>(buffer[i])));
    return id;
}
std::string KindName(Kind kind)
{
    switch (kind) { case Kind::Global: return "global"; case Kind::QuickPanel: return "quickPanel"; default: return "popup"; }
}
std::string ErrorLocalizationKey(std::string_view error)
{
    constexpr std::pair<std::string_view, std::string_view> keys[] = {
        {"invalidUtf8", "themeLibrary.error.invalidUtf8"},
        {"invalidPackage", "themeLibrary.error.invalidPackage"},
        {"missingDependency", "themeLibrary.error.missingDependency"},
        {"unsupportedVersion", "themeLibrary.error.unsupportedVersion"},
        {"emptyPackage", "themeLibrary.error.emptyPackage"},
        {"themeNotFound", "themeLibrary.error.themeNotFound"},
        {"invalidSelection", "themeLibrary.error.invalidSelection"},
        {"idConflict", "themeLibrary.error.idConflict"},
        {"themeInUse", "themeLibrary.error.themeInUse"},
        {"readFailed", "themeLibrary.error.readFailed"},
        {"writeFailed", "themeLibrary.error.writeFailed"},
        {"libraryBusy", "themeLibrary.error.libraryBusy"},
        {"unsupportedExtension", "themeLibrary.error.unsupportedExtension"}};
    for (const auto& [reason, key] : keys) if (reason == error) return std::string(key);
    return "themeLibrary.error.invalidPackage";
}
std::optional<Theme> Builtin(std::string_view id)
{
    constexpr std::pair<std::string_view, int> presets[] = {{"dark", 0}, {"light", 1}, {"glass-dark", 6},
        {"glass-light", 7}, {"glass-transparent", 13}, {"acrylic-dark", 10}, {"acrylic-light", 11}};
    for (const auto kind : {Kind::Global, Kind::QuickPanel, Kind::Popup})
        for (const auto& [name, preset] : presets)
        {
            if (kind != Kind::Global && preset != 0 && preset != 1 && preset != 10 && preset != 11) continue;
            const auto expected = "builtin/" + KindName(kind) + '/' + std::string(name);
            // IDs are lower-case even though the section name is quickPanel.
            auto stable = expected; std::transform(stable.begin(), stable.end(), stable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (id != stable) continue;
            Theme theme; theme.id = stable; theme.name = std::string(name); theme.kind = kind;
            theme.appearance = kind == Kind::Global ? MakeAppearancePreset(preset) : kind == Kind::QuickPanel ?
                MakeQuickNavigationAppearancePreset(preset) : MakeCollectionPopupAppearancePreset(preset);
            theme.quickPanel = "builtin/quickpanel/" + std::string(preset == 1 || preset == 7 || preset == 11 ? "light" : "dark");
            theme.popup = "builtin/popup/" + std::string(preset == 1 || preset == 7 || preset == 11 ? "light" : "dark");
            if (preset >= 6) { theme.quickPanel.insert(19, "acrylic-"); theme.popup.insert(14, "acrylic-"); }
            return theme;
        }
    return {};
}
const Theme* Find(const Package& package, std::string_view id)
{
    const auto found = package.find(std::string(id));
    return found == package.end() ? nullptr : &found->second;
}
std::optional<Theme> Resolve(const Package& package, std::string_view id)
{
    if (auto builtin = Builtin(id)) return builtin;
    if (const auto* found = Find(package, id)) return *found;
    return {};
}
std::vector<Theme> Choices(const Library& library, Kind kind, unsigned scope)
{
    std::vector<Theme> out;
    for (const auto name : {"dark", "light", "glass-dark", "glass-light", "glass-transparent", "acrylic-dark", "acrylic-light"})
    {
        auto section = KindName(kind); std::transform(section.begin(), section.end(), section.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (auto theme = Builtin("builtin/" + section + '/' + name)) out.push_back(*theme);
    }
    for (const auto& [id, theme] : library.themes)
    { (void)id; if (theme.kind == kind && (kind != Kind::Global || (theme.scopes & scope) == scope)) out.push_back(theme); }
    return out;
}
bool Validate(const Package& package, std::string& error)
{
    error.clear();
    if (package.size() > 1536) return Fail(error, "invalidPackage");
    for (const auto& [id, theme] : package)
    {
        if ((theme.kind != Kind::Global && theme.kind != Kind::QuickPanel && theme.kind != Kind::Popup) ||
            id != theme.id || !Id(id) || (!Builtin(id) && (id.starts_with("builtin/") || !Text(theme.name, 256))) ||
            EncodeTheme(theme).empty()) return Fail(error, "invalidPackage");
        if (theme.kind == Kind::Global)
        {
            auto quick = Resolve(package, theme.quickPanel), popup = Resolve(package, theme.popup);
            if (theme.scopes < 1 || theme.scopes > All || !quick || quick->kind != Kind::QuickPanel ||
                !popup || popup->kind != Kind::Popup) return Fail(error, "missingDependency");
        }
        if (theme.kind == Kind::QuickPanel)
        {
            NavigationSettings navigation; navigation.layout = theme.layout;
            NormalizeNavigationSettings(navigation);
            if (navigation.layout != theme.layout || !Colors(theme.colors)) return Fail(error, "invalidPackage");
        }
    }
    return true;
}
std::string EncodePackage(const Package& package, std::string& error)
{
    if (package.empty() || !Validate(package, error)) { if (error.empty()) error = "emptyPackage"; return {}; }
    return "{\"format\":\"snowdesktop.theme\",\"version\":1" + Sections(package) + '}';
}
bool DecodePackage(std::string_view text, Package& package, std::string& error)
{
    JsonValue json;
    if (!Document(text, json, error) || !Keys(json, {"format", "version", "global", "quickPanel", "popup"})) return Fail(error, "invalidPackage");
    std::string format; const auto* version = json.Find("version");
    if (!String(json, "format", format) || format != "snowdesktop.theme") return Fail(error, "invalidPackage");
    if (!version || !version->IsNumber() || version->number != kPackageVersion) return Fail(error, "unsupportedVersion");
    return ReadSections(json, package, error, false);
}
bool Export(const Library& library, std::string_view id, Package& package, std::string& error)
{
    const auto theme = Resolve(library.themes, id);
    if (!theme) return Fail(error, "themeNotFound");
    Package out; out.emplace(theme->id, *theme);
    if (theme->kind == Kind::Global)
        for (const auto& dependency : {theme->quickPanel, theme->popup})
        {
            const auto resolved = Resolve(library.themes, dependency);
            if (!resolved) return Fail(error, "missingDependency");
            if (!Builtin(dependency)) out.emplace(dependency, *resolved);
        }
    if (!Validate(out, error)) return false;
    package = std::move(out);
    return true;
}
bool Import(Library& library, const Package& package, std::map<std::string, std::string>& mapping,
    std::string& error, const NewId& newId)
{
    if (package.empty() || !Validate(package, error)) return false;
    Library next = library;
    std::map<std::string, std::string> remap;
    // Children first: a reused global must also reference the remapped children.
    for (const auto kind : {Kind::QuickPanel, Kind::Popup, Kind::Global})
        for (const auto& [id, input] : package)
        {
            if (input.kind != kind) continue;
            Theme theme = input;
            if (kind == Kind::Global)
            {
                if (remap.contains(theme.quickPanel)) theme.quickPanel = remap.at(theme.quickPanel);
                if (remap.contains(theme.popup)) theme.popup = remap.at(theme.popup);
            }
            if (Builtin(id)) { remap[id] = id; continue; }
            const auto found = Find(next.themes, id);
            if (found && EncodeTheme(*found) != EncodeTheme(theme))
            {
                theme.id = newId();
                if (!Id(theme.id) || theme.id.starts_with("builtin/") || next.themes.contains(theme.id) || package.contains(theme.id)) return Fail(error, "idConflict");
            }
            remap[id] = theme.id;
            next.themes[theme.id] = std::move(theme);
        }
    if (!Validate(next.themes, error)) return false;
    library = std::move(next); mapping = std::move(remap);
    return true;
}
bool Save(Library& library, Theme theme, const Package& dependencies, bool update,
    std::string& savedId, std::string& error, const NewId& newId, bool preserveObjects)
{
    Library next = library;
    if (update)
    {
        const auto* old = Find(next.themes, theme.id);
        if (!old || Builtin(theme.id) || old->kind != theme.kind) return Fail(error, "themeNotFound");
        for (auto& [target, ref] : next.references)
        {
            (void)target;
            if (ref.id == theme.id && theme.kind == Kind::Global && (theme.scopes & ref.scope) != ref.scope)
            { if (!preserveObjects) return Fail(error, "themeInUse"); ref.id.clear(); }
        }
    }
    else
    {
        theme.id = newId();
        if (!Id(theme.id) || theme.id.starts_with("builtin/") || next.themes.contains(theme.id) || dependencies.contains(theme.id)) return Fail(error, "idConflict");
    }
    for (const auto& [id, dependency] : dependencies)
    {
        if (Builtin(id) || dependency.kind == Kind::Global || next.themes.contains(id)) return Fail(error, "idConflict");
        next.themes.emplace(id, dependency);
    }
    next.themes[theme.id] = theme;
    if (!Validate(next.themes, error)) return false;
    // Updating a theme intentionally leaves existing object snapshots intact.
    // The host applies a new selection only after its settings commit succeeds.
    savedId = theme.id; library = std::move(next);
    return true;
}
bool Select(Library& library, std::string target, std::string_view id, Kind kind, unsigned scope, std::string& error)
{
    const auto theme = Resolve(library.themes, id);
    if (!theme || theme->kind != kind || !Text(target, 512) || scope < 1 || scope > All ||
        (kind == Kind::Global && (theme->scopes & scope) != scope)) return Fail(error, "invalidSelection");
    Package snapshot;
    if (!Export(library, id, snapshot, error)) return false;
    library.references[std::move(target)] = {std::string(id), kind, scope, std::move(snapshot)};
    return true;
}
void Detach(Library& library, std::string_view target)
{
    if (auto found = library.references.find(std::string(target)); found != library.references.end()) found->second.id.clear();
}
std::vector<std::string> References(const Library& library, std::string_view id)
{
    std::vector<std::string> out;
    for (const auto& [key, theme] : library.themes)
        if (theme.kind == Kind::Global && (theme.quickPanel == id || theme.popup == id)) out.push_back(key);
    for (const auto& [target, ref] : library.references) if (ref.id == id) out.push_back(target);
    return out;
}
bool Remove(Library& library, std::string_view id, std::string_view replacement, bool preserveObjects, std::string& error)
{
    const auto* old = Find(library.themes, id);
    if (!old || Builtin(id)) return Fail(error, "themeNotFound");
    const auto substitute = replacement.empty() ? std::optional<Theme>{} : Resolve(library.themes, replacement);
    if (!replacement.empty() && (!substitute || substitute->kind != old->kind || replacement == id)) return Fail(error, "invalidSelection");
    Library next = library;
    for (auto& [key, theme] : next.themes)
    {
        (void)key;
        for (auto* binding : {&theme.quickPanel, &theme.popup})
            if (*binding == id)
            { if (!substitute) return Fail(error, "themeInUse"); *binding = substitute->id; }
    }
    for (auto& [target, ref] : next.references)
    {
        if (ref.id != id) continue;
        if (preserveObjects) ref.id.clear();
        else if (substitute)
        {
            if (!Select(next, target, substitute->id, ref.kind, ref.scope, error)) return false;
        }
        else return Fail(error, "themeInUse");
    }
    next.themes.erase(std::string(id));
    if (!Validate(next.themes, error)) return false;
    library = std::move(next);
    return true;
}
void ApplyAppearance(PersonalizationSettings& target, const PersonalizationSettings& appearance)
{
    VisitPanelAppearanceFields([&](auto, auto member, double, double) { target.*member = appearance.*member; });
    VisitPanelAppearanceFlags([&](auto, auto member) { target.*member = appearance.*member; });
    target.gradientEndA = appearance.gradientEndA;
    target.panelGradient = appearance.panelGradient; target.edgeLight = appearance.edgeLight;
    target.contentTheme = appearance.contentTheme; target.backgroundPreset = kAppearancePresetCustom;
}
void ApplyQuickPanel(NavigationSettings& target, const Theme& theme)
{
    if (theme.kind == Kind::QuickPanel) { target.layout = theme.layout; target.colors = theme.colors; }
}
Theme Capture(Kind kind, const PersonalizationSettings& appearance, const NavigationSettings& navigation)
{
    Theme theme; theme.kind = kind; ApplyAppearance(theme.appearance, appearance);
    if (kind == Kind::QuickPanel) { theme.layout = navigation.layout; theme.colors = navigation.colors; }
    return theme;
}
std::string EncodeLibrary(const Library& library, std::string& error)
{
    if (!Validate(library.themes, error)) return {};
    std::string out = "{\"format\":\"snowdesktop.theme-library\",\"version\":1" + Sections(library.themes) + ",\"references\":{";
    bool first = true;
    for (const auto& [target, ref] : library.references)
    {
        auto snapshot = EncodePackage(ref.snapshot, error);
        if (!Text(target, 512) || snapshot.empty() || (!ref.id.empty() && !Resolve(library.themes, ref.id))) return {};
        if (!first) out += ','; first = false;
        out += Quote(target) + ":{\"id\":" + Quote(ref.id) + ",\"kind\":" + Quote(KindName(ref.kind)) +
            ",\"scope\":" + std::to_string(ref.scope) + ",\"snapshot\":" + snapshot + '}';
    }
    return out + "}}";
}
bool DecodeLibrary(std::string_view text, Library& library, std::string& error)
{
    JsonValue json;
    if (!Document(text, json, error) || !Keys(json, {"format", "version", "global", "quickPanel", "popup", "references"})) return Fail(error, "invalidPackage");
    std::string format; const auto* version = json.Find("version");
    if (!String(json, "format", format) || format != "snowdesktop.theme-library" ||
        !version || !version->IsNumber() || version->number != 1) return Fail(error, "unsupportedVersion");
    Library out;
    if (!ReadSections(json, out.themes, error, true)) return false;
    const auto* refs = json.Find("references");
    if (!refs || !refs->IsObject() || refs->object.size() > 4096) return Fail(error, "invalidPackage");
    for (const auto& [target, value] : refs->object)
    {
        Reference ref; std::string kind;
        const auto* scope = value.Find("scope"); const auto* snapshot = value.Find("snapshot");
        if (!Text(target, 512) || !Keys(value, {"id", "kind", "scope", "snapshot"}) ||
            !String(value, "id", ref.id) || !String(value, "kind", kind) || !scope || !scope->IsNumber() ||
            scope->number < 1 || scope->number > static_cast<double>(All) || std::floor(scope->number) != scope->number || !snapshot ||
            !ReadSections(*snapshot, ref.snapshot, error, false)) return Fail(error, "invalidPackage");
        if (kind == "global") ref.kind = Kind::Global;
        else if (kind == "quickPanel") ref.kind = Kind::QuickPanel;
        else if (kind == "popup") ref.kind = Kind::Popup;
        else return Fail(error, "invalidPackage");
        std::string snapshotFormat;
        const auto* snapshotVersion = snapshot->Find("version");
        if (!Keys(*snapshot, {"format", "version", "global", "quickPanel", "popup"}) ||
            !String(*snapshot, "format", snapshotFormat) || snapshotFormat != "snowdesktop.theme" ||
            !snapshotVersion || !snapshotVersion->IsNumber() || snapshotVersion->number != kPackageVersion)
            return Fail(error, "unsupportedVersion");
        unsigned roots = 0;
        for (const auto& [id, theme] : ref.snapshot)
            if (theme.kind == ref.kind) { ++roots; if (!ref.id.empty() && id != ref.id) return Fail(error, "invalidSelection"); }
        if (roots != 1) return Fail(error, "invalidSelection");
        ref.scope = static_cast<unsigned>(scope->number);
        if (!ref.id.empty())
        {
            const auto resolved = Resolve(out.themes, ref.id);
            if (!resolved || resolved->kind != ref.kind || (ref.kind == Kind::Global &&
                (resolved->scopes & ref.scope) != ref.scope)) return Fail(error, "invalidSelection");
        }
        out.references.emplace(target, std::move(ref));
    }
    library = std::move(out);
    return true;
}
PackageType Classify(const std::filesystem::path& path)
{
    const auto extension = path.extension().wstring();
    if (_wcsicmp(extension.c_str(), L".snowwidget") == 0) return PackageType::Widget;
    if (_wcsicmp(extension.c_str(), L".snowtheme") == 0) return PackageType::Theme;
    return PackageType::Unsupported;
}
bool ReadPackage(const std::filesystem::path& path, Package& package, std::string& error)
{
    if (Classify(path) != PackageType::Theme) return Fail(error, "unsupportedExtension");
    std::string text;
    return ReadFile(path, text, error) && DecodePackage(text, package, error);
}
bool WritePackage(const std::filesystem::path& path, const Package& package, std::string& error)
{
    if (Classify(path) != PackageType::Theme) return Fail(error, "unsupportedExtension");
    const auto text = EncodePackage(package, error);
    return !text.empty() && AtomicWrite(path, text, error);
}
bool Load(const std::filesystem::path& path, Library& library, std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    { if (ec) return Fail(error, "readFailed"); library = {}; return true; }
    std::string text;
    return ReadFile(path, text, error) && DecodeLibrary(text, library, error);
}
bool Transact(const std::filesystem::path& path,
    const std::function<bool(Library&, std::string&)>& edit, Library& output, std::string& error)
{
    auto lockPath = path; lockPath += L".lock";
    Handle lock{CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (lock.value == INVALID_HANDLE_VALUE) return Fail(error, "libraryBusy");
    Library next;
    if (!Load(path, next, error) || !edit(next, error)) return false;
    const auto text = EncodeLibrary(next, error);
    Library validated;
    if (text.empty() || !DecodeLibrary(text, validated, error) || !AtomicWrite(path, text, error)) return false;
    output = std::move(validated);
    return true;
}
}
