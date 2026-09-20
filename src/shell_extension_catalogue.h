#pragma once
#include "shell_extension_menu.h"
#include <filesystem>

namespace snowdesktop::shell_extensions
{
enum class RegistrationKind : int { Verb, Handler, Packaged, Observed };
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
};
constexpr unsigned ContextBit(Context context) { return 1u << static_cast<unsigned>(context); }
// Read-only, intended for the metadata worker. No Shell extension is activated.
Catalogue ReadCatalogue(HKEY classes = HKEY_CLASSES_ROOT, bool packages = true);
void Associate(Catalogue &catalogue, const Request &request, Reply &reply);
void MigrateAssociations(Preferences &preferences, const Catalogue &catalogue);
bool Applies(const Registration &row, const Request &request);
}
namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::Registration>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.id, v.kind, v.display, v.sources, v.types, v.verbs, v.contexts, v.systemEnabled, v.linked, v.revision, v.commandIdentity); }
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
