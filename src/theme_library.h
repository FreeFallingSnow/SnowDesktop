#pragma once

#include "surface_theme.h"
#include "navigation_settings.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace snowdesktop::themes
{
// The package version is independent of the widget API and Steam bridge CLI.
inline constexpr int kPackageVersion = 1;
inline constexpr std::size_t kMaximumPackageBytes = 4 * 1024 * 1024;
enum class Kind { Global, QuickPanel, Popup };
enum Scope : unsigned { Components = 1, Dock = 2, StatusBar = 4, Taskbar = 8, All = 15 };

struct Theme
{
    std::string id, name;
    Kind kind = Kind::Global;
    unsigned scopes = All;
    PersonalizationSettings appearance;
    QuickNavigationLayout layout;
    std::map<std::string, std::string> colors;
    std::string quickPanel, popup;
};

// Both package sections and local snapshots contain the same immutable values.
// This is also the seam for a future preview made from the exact export snapshot.
using Package = std::map<std::string, Theme>;
struct Reference
{
    std::string id;
    Kind kind = Kind::Global;
    unsigned scope = Components;
    Package snapshot;
};
struct Library
{
    Package themes;
    std::map<std::string, Reference> references;
    struct WorkshopOrigin
    {
        std::string owner, sha256;
        std::set<std::string> accounts, ids;
    };
    std::map<std::string, WorkshopOrigin> workshop;
    std::map<std::string, std::set<std::string>> subscriptionAccounts;
};
using NewId = std::function<std::string()>;

std::string CreateId();
std::string KindName(Kind kind);
std::string ErrorLocalizationKey(std::string_view error);
std::optional<Theme> Builtin(std::string_view id);
std::vector<Theme> Choices(const Library& library, Kind kind, unsigned scope = All);
const Theme* Find(const Package& package, std::string_view id);
std::optional<Theme> Resolve(const Package& package, std::string_view id);
bool Validate(const Package& package, std::string& error);
std::string EncodePackage(const Package& package, std::string& error);
bool DecodePackage(std::string_view text, Package& package, std::string& error);
bool Export(const Library& library, std::string_view id, Package& package, std::string& error);
bool Import(Library& library, const Package& package, std::map<std::string, std::string>& mapping,
    std::string& error, const NewId& newId = CreateId);

// Save-as generates a new ID. Update is always explicit and keeps that ID.
// Dependencies and the parent are validated together before changing the library.
bool Save(Library& library, Theme theme, const Package& dependencies, bool update,
    std::string& savedId, std::string& error, const NewId& newId = CreateId, bool preserveObjects = false);
bool Select(Library& library, std::string target, std::string_view id, Kind kind,
    unsigned scope, std::string& error);
void Detach(Library& library, std::string_view target);
std::vector<std::string> References(const Library& library, std::string_view id);
// In-use bindings require a replacement. Object references may instead become
// custom while retaining the complete last successful appearance snapshot.
bool Remove(Library& library, std::string_view id, std::string_view replacement,
    bool preserveObjects, std::string& error);

// Applies only material fields, never object layout, enablement, or author data.
void ApplyAppearance(PersonalizationSettings& target, const PersonalizationSettings& appearance);
void ApplyQuickPanel(NavigationSettings& target, const Theme& theme);
Theme Capture(Kind kind, const PersonalizationSettings& appearance,
    const NavigationSettings& navigation = {});

std::string EncodeLibrary(const Library& library, std::string& error);
bool DecodeLibrary(std::string_view text, Library& library, std::string& error);
bool ReadPackage(const std::filesystem::path& path, Package& package, std::string& error);
bool WritePackage(const std::filesystem::path& path, const Package& package, std::string& error);
// Serializes writers, rereads the latest file, and replaces one complete file.
// A failed parse, validation, callback, or write leaves the last file untouched.
bool Load(const std::filesystem::path& path, Library& library, std::string& error);
bool Transact(const std::filesystem::path& path,
    const std::function<bool(Library&, std::string&)>& edit, Library& output, std::string& error);
enum class PackageType { Unsupported, Widget, Theme };
PackageType Classify(const std::filesystem::path& path);
}
