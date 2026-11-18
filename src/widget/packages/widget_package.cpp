#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "common/language_fallback.h"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <utility>

namespace snowdesktop::widget
{
namespace
{
using detail::Lower;
using detail::Timestamp;
using detail::Utf8ToWide;

PackageManifest LocalizedManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    if (requestedLocale.empty() || manifest.locales.empty()) return manifest;
    const LocalizedMetadata* selected = nullptr;
    std::vector<std::string> available;
    available.reserve(manifest.locales.size());
    for (const auto& [locale, metadata] : manifest.locales)
    {
        (void)metadata;
        available.push_back(locale);
    }
    const std::string selectedLocale =
        snowdesktop::localization::ResolveBestLanguage(
            available, requestedLocale);
    if (!selectedLocale.empty())
        selected = &manifest.locales.at(selectedLocale);
    if (selected)
    {
        if (!selected->title.empty()) manifest.name = selected->title;
        if (!selected->description.empty())
            manifest.description = selected->description;
        auto translate = [&](const std::string& key,
            std::string& value) {
            if (key.empty()) return;
            if (const auto found = selected->strings.find(key);
                found != selected->strings.end())
                value = found->second;
        };
        translate(manifest.previewIntroductionKey,
            manifest.previewIntroduction);
        for (auto& preset : manifest.creationPresets)
            translate(preset.titleKey, preset.title);
        for (auto& helper : manifest.nativeHelpers)
        {
            translate(helper.purposeKey, helper.purpose);
            translate(helper.operationsKey, helper.operations);
            translate(helper.reasonKey, helper.reason);
            translate(helper.deniedKey, helper.denied);
        }
        for (const auto& [storageKey, localizationKey] :
            manifest.previewStorageKeys)
        {
            if (const auto value = manifest.previewStorage.find(storageKey);
                value != manifest.previewStorage.end())
                translate(localizationKey, value->second);
        }
        for (auto& variant : manifest.previewVariants)
        {
            translate(variant.titleKey, variant.title);
            translate(variant.descriptionKey, variant.description);
            for (const auto& [storageKey, localizationKey] :
                variant.storageKeys)
            {
                if (const auto value = variant.storage.find(storageKey);
                    value != variant.storage.end())
                    translate(localizationKey, value->second);
            }
        }
    }
    return manifest;
}
}

PackageManifest LocalizePackageManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    return LocalizedManifest(std::move(manifest), requestedLocale);
}

bool MeetsWidgetMinimumHostVersion(std::string_view host, std::string_view minimum)
{
    std::istringstream actual{std::string(host)};
    std::istringstream required{std::string(minimum)};
    std::string actualPart, requiredPart;
    for (int part = 0; part < 4; ++part)
    {
        const int a = std::getline(actual, actualPart, '.') ? std::atoi(actualPart.c_str()) : 0;
        const int b = std::getline(required, requiredPart, '.') ? std::atoi(requiredPart.c_str()) : 0;
        if (a != b) return a > b;
    }
    return true;
}

bool IsExecutablePackageContract(int schemaVersion, int apiVersion) noexcept
{
    return schemaVersion == kPackageSchemaVersion &&
        apiVersion == kHostApiVersion;
}

bool IsExecutablePackageContract(const PackageManifest& manifest) noexcept
{
    return IsExecutablePackageContract(
        manifest.schemaVersion, manifest.apiVersion);
}

std::filesystem::path WidgetPackageManager::CreateStagingPath(
    const char* purpose) const
{
    return paths_.staging /
        (Utf8ToWide(std::string(purpose) + "-" + Timestamp() + "-" +
            GenerateUuid()));
}

bool WidgetPackageManager::CopyPackageTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error) const
{
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec)
    {
        error = "cannot create package destination: " + ec.message();
        return false;
    }
    std::filesystem::copy(source, destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "cannot copy package: " + ec.message();
        return false;
    }
    return true;
}

std::string WidgetPackageManager::Sha256File(
    const std::filesystem::path& path)
{
    return detail::HashPackageFile(path, false);
}

std::string WidgetPackageManager::GenerateUuid()
{
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) return {};
    char buffer[37]{};
    std::snprintf(buffer, sizeof(buffer),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
        guid.Data4[6], guid.Data4[7]);
    return Lower(buffer);
}
}
