#pragma once
#include "shell_extension_menu.h"
#include <filesystem>
#include <array>

namespace snowdesktop::shell_extensions
{
enum class RegistrationKind : int { Verb, Handler, Packaged, Observed };
struct Application
{
    std::string id;
    std::wstring name;
    friend bool operator==(const Application &, const Application &) = default;
};
struct Registration
{
    std::string id;
    RegistrationKind kind = RegistrationKind::Verb;
    Entry display;
    std::vector<std::wstring> sources, types;
    std::vector<std::string> verbs;
    unsigned contexts = 0;
    bool systemEnabled = true, linked = false;
    std::uint64_t revision = 0;
    // Exact execution identity for grouping equivalent static registrations in
    // settings. Original IDs remain the persisted visibility/command authority.
    std::string commandIdentity;
    Application application;
};
struct Association
{
    std::string provider, registration;
    Context context = Context::File;
};
struct Catalogue
{
    std::vector<Registration> rows;
    std::vector<Association> associations;
    std::uint64_t revision = 0;
    // Transient proof from this process; persisted/IPC catalogues omit it and
    // use full verification until a live scan establishes the baseline.
    std::uint64_t folderRevision = 0;
    std::array<std::uint64_t, 2> backgroundRevisions{};
};
constexpr unsigned ContextBit(Context context) { return 1u << static_cast<unsigned>(context); }
// Read-only, intended for the metadata worker. No Shell extension is activated.
Catalogue ReadCatalogue(HKEY classes = HKEY_CLASSES_ROOT, bool packages = true);
// Same complete folder registration inputs as the full scan, without
// enumerating unrelated file classes. Metadata only; no handler activation.
std::uint64_t ReadFolderCatalogueRevision(HKEY classes = HKEY_CLASSES_ROOT, bool packages = true);
// Background scope includes shared Directory\Background registrations and,
// for Desktop, DesktopBackground. Uses the same live inputs as ReadCatalogue.
std::uint64_t ReadBackgroundCatalogueRevision(Context scope, HKEY classes = HKEY_CLASSES_ROOT, bool packages = true);
// Internal menu compatibility path uses the same Blocked/Approved policy as
// discovery. Alternate roots are for isolated registry policy tests only.
bool HandlerEnabled(const std::wstring &clsid, HKEY user = HKEY_CURRENT_USER, HKEY machine = HKEY_LOCAL_MACHINE);
void Associate(Catalogue &catalogue, const Request &request, Reply &reply);
void MigrateAssociations(Preferences &preferences, const Catalogue &catalogue);
bool Applies(const Registration &row, const Request &request);
}
namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::Application>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.id, v.name); }
};
template <> struct Fields<shell_extensions::Registration>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.id, v.kind, v.display, v.sources, v.types, v.verbs, v.contexts, v.systemEnabled, v.linked, v.revision, v.commandIdentity, v.application); }
};
template <> struct Fields<shell_extensions::Association>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.provider, v.registration, v.context); }
};
template <> struct Fields<shell_extensions::Catalogue>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.rows, v.associations, v.revision); }
};
}
