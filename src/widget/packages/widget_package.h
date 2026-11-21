/**
 * @file widget_package.h
 * @brief SnowDesktop Lua component package manifest and its value types.
 */

#pragma once

#include "widget_size_types.h"
#include "widget/api/widget_logical_slot_declarations.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget
{
inline constexpr int kPackageSchemaVersion = 2;
inline constexpr int kHostApiVersion = 2;

struct LocalizedMetadata
{
    std::string title;
    std::string description;
    std::unordered_map<std::string, std::string> strings;
};

struct PackageResource
{
    std::string type;
    std::string path;
    std::string license;

    bool operator==(const PackageResource&) const = default;
};

struct PreviewVariant
{
    std::string id;
    std::string title;
    std::string titleKey;
    std::string description;
    std::string descriptionKey;
    int columns = 0;
    int rows = 0;
    std::optional<AspectGridSize> aspectSize;
    std::unordered_map<std::string, std::string> storage;
    std::unordered_map<std::string, std::string> storageKeys;
};

/** Ordinary initial settings selected in the component picker, never a theme. */
struct CreationPreset
{
    std::string id;
    std::string title;
    std::string titleKey;
    std::unordered_map<std::string, std::string> values;
    std::optional<WidgetSizeDefinition> size;
};

inline WidgetSizeDefinition SelectWidgetCreationSize(const WidgetSizeDefinition& fallback,
    const PreviewVariant* variant, const CreationPreset* preset)
{
    if (preset && preset->size) return *preset->size;
    if (variant) return {variant->columns, variant->rows, variant->aspectSize};
    return fallback;
}

struct NativeHelper
{
    std::string id, path, sha256;
    std::string purposeKey, operationsKey, reasonKey, deniedKey;
    std::string purpose, operations, reason, denied;
};

struct PackageManifest
{
    int schemaVersion = 0;
    std::string id;
    std::string slug;
    std::string version;
    int apiVersion = 0;
    int dataVersion = 1;
    bool confirmRemoval = false;
    std::string entry = "main.lua";
    std::string minHostVersion;
    std::string name;
    std::string nameKey;
    std::string description;
    std::string descriptionKey;
    std::string author;
    std::string license;
    std::string preview;
    // Absent preserves the existing Steam gallery; an empty array clears our images.
    std::optional<std::vector<std::string>> previewGallery;
    std::string previewIntroduction;
    std::string previewIntroductionKey;
    std::unordered_map<std::string, std::string> previewStorage;
    std::unordered_map<std::string, std::string> previewStorageKeys;
    std::vector<PreviewVariant> previewVariants;
    std::vector<CreationPreset> creationPresets;
    int defaultColumns = 1;
    int defaultRows = 1;
    std::optional<AspectGridSize> defaultAspectSize;
    int minColumns = 1;
    int minRows = 1;
    int maxColumns = 0;
    int maxRows = 0;
    int refreshIntervalMs = 0;
    std::vector<std::string> permissions;
    std::vector<std::string> optionalPermissions;
    std::vector<std::string> networkDomains;
    std::vector<std::string> requiredFeatures;
    std::vector<std::string> optionalFeatures;
    std::unordered_map<std::string, PackageResource> resources;
    snowdesktop::widget_runtime::LogicalSlotDeclarations logicalSlots;
    std::unordered_map<std::string, LocalizedMetadata> locales;
    std::vector<NativeHelper> nativeHelpers;
    // Derived by directory validation; never accepted from the manifest.
    std::string nativeContentFingerprint;
};

std::string PackagePermissionScopeFingerprint(const PackageManifest& manifest);

PackageManifest LocalizePackageManifest(PackageManifest manifest,
    const std::string& requestedLocale);
bool IsExecutablePackageContract(int schemaVersion,
    int apiVersion) noexcept;
bool IsExecutablePackageContract(const PackageManifest& manifest) noexcept;
bool MeetsWidgetMinimumHostVersion(std::string_view host, std::string_view minimum);
std::vector<std::string> DeclaredPermissions(
    const PackageManifest& manifest);
}
