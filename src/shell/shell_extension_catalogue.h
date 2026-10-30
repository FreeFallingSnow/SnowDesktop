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
    // Live command and asset proof excludes decoded icon pixels. Persisted
    // catalogues store this separately; the settings IPC contract is unchanged.
    std::uint64_t menuRevision = 0;
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
// Current package manifest commands only; no file-class scan or COM activation.
std::vector<Registration> ReadPackagedMenuCommands(HKEY classes = HKEY_CLASSES_ROOT);
void LoadMenuCommandIcon(Entry& entry, const std::wstring& location);
// Background refresh uses fewer workers and expands to the normal limit when
// a foreground query raises demand. The ordinary reader keeps its full budget.
class CatalogueForegroundDemand;
Catalogue ReadCatalogueWithForegroundDemand(HKEY classes, bool packages,
                                            CatalogueForegroundDemand &foreground);
// Recheck only exact, already known Verb/Handler registration paths and the
// extension associations named by those seeds. No class/package enumeration or
// extension activation. Missing/unsupported registrations are omitted.
// Types and contexts prove only the seed's scope; revision is domain-separated
// limited proof, never a complete inventory or folder/background baseline.
// Command validity uses each returned row's menuRevision, not revision (which
// also reflects display metadata such as decoded icon pixels).
Catalogue ReadCatalogueSources(const std::vector<Registration> &sources,
                               CatalogueForegroundDemand *foreground = nullptr,
                               HKEY classes = HKEY_CLASSES_ROOT);
// Same complete folder registration inputs as the full scan, without
// enumerating unrelated file classes. Metadata only; no handler activation.
// Optional per-scope demand supports cooperative cancellation; a cancelled
// read throws instead of returning a usable complete proof.
std::uint64_t ReadFolderCatalogueRevision(HKEY classes = HKEY_CLASSES_ROOT, bool packages = true,
                                         CatalogueForegroundDemand *foreground = nullptr);
// Background scope includes shared Directory\Background registrations and,
// for Desktop, DesktopBackground. Uses the same live inputs as ReadCatalogue.
std::uint64_t ReadBackgroundCatalogueRevision(Context scope, HKEY classes = HKEY_CLASSES_ROOT, bool packages = true,
                                             CatalogueForegroundDemand *foreground = nullptr);
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
