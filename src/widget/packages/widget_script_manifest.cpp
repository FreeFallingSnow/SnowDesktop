#include "widget_script_manifest.h"

#include "common/constants.h"
#include "common/json_value.h"
#include "widget/api/widget_logical_slot_manifest.h"
#include "widget/runtime/widget_l10n_lua.h"
#include "widget/settings/widget_setting_rules.h"
#include "widget/settings/widget_settings_declarations.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
std::wstring ManifestPathForScriptFile(const std::wstring& fullScriptPath,
    const std::filesystem::path& packageRoot)
{
    if (!packageRoot.empty()) return (packageRoot / L"widget.json").wstring();
    const std::filesystem::path packageManifest =
        std::filesystem::path(fullScriptPath).parent_path() / L"widget.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(packageManifest, error))
        return packageManifest.wstring();
    std::wstring path = fullScriptPath;
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (ext && _wcsicmp(ext, L".lua") == 0)
        path.resize(static_cast<size_t>(ext - path.c_str()));
    path += L".widget.json";
    return path;
}

std::string ReadTextFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}
}

std::string HashWidgetFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest(32);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    char buffer[8192];
    while (file)
    {
        file.read(buffer, sizeof(buffer));
        auto count = file.gcount();
        if (count > 0)
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(count), 0);
    }
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned char byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0F]);
    }
    return result;
}

LuaWidgetManifest ReadWidgetScriptManifest(const std::wstring& fullPath,
    const std::filesystem::path& packageRoot)
{
    LuaWidgetManifest manifest;
    if (fullPath.empty()) return manifest;
    using widget_l10n::TranslateManifest;

    std::wstring manifestPath = ManifestPathForScriptFile(fullPath, packageRoot);
    std::string text = ReadTextFile(manifestPath);
    if (text.empty())
        return manifest;

    JsonValue root;
    if (!ParseJson(text, root) || !root.IsObject())
        return manifest;
    manifest.hasManifest = true;

    auto readString = [](const JsonValue& object, const char* field,
        std::string& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsString())
            return false;
        output = value->string;
        return true;
    };
    auto valueString = [](const JsonValue& value) -> std::string {
        if (value.IsString()) return value.string;
        if (value.IsBoolean()) return value.boolean ? "1" : "0";
        if (value.IsNumber())
        {
            std::ostringstream stream;
            stream << value.number;
            return stream.str();
        }
        return {};
    };
    auto readStringArray = [](const JsonValue& object, const char* field) {
        std::vector<std::string> result;
        const JsonValue* values = object.Find(field);
        if (!values || !values->IsArray())
            return result;
        for (const JsonValue& value : values->array)
            if (value.IsString())
                result.push_back(value.string);
        return result;
    };
    auto readNumber = [](const JsonValue& object, const char* field,
        double& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsNumber())
            return false;
        output = value->number;
        return true;
    };
    auto readBool = [](const JsonValue& object, const char* field,
        bool& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsBoolean())
            return false;
        output = value->boolean;
        return true;
    };
    auto readSettingCondition = [&](const JsonValue& object,
        const char* field) ->
        std::optional<LuaWidgetManifest::SettingCondition> {
        const JsonValue* value = object.Find(field);
        if (!value) return std::nullopt;
        LuaWidgetManifest::SettingCondition condition;
        if (!value->IsObject())
        {
            condition.operation = "__invalid";
            return condition;
        }
        readString(*value, "key", condition.key);
        readString(*value, "operator", condition.operation);
        if (const JsonValue* expected = value->Find("value"))
        {
            if (expected->IsArray())
            {
                for (const auto& item : expected->array)
                {
                    if (!item.IsString() && !item.IsNumber() &&
                        !item.IsBoolean())
                        condition.operation = "__invalid";
                    condition.values.push_back(valueString(item));
                }
            }
            else
            {
                if (!expected->IsString() && !expected->IsNumber() &&
                    !expected->IsBoolean())
                    condition.operation = "__invalid";
                condition.values.push_back(valueString(*expected));
            }
        }
        return condition;
    };

    readString(root, "id", manifest.packageId);
    readString(root, "slug", manifest.slug);
    double packageNumber = 0;
    if (readNumber(root, "schemaVersion", packageNumber))
        manifest.schemaVersion = static_cast<int>(packageNumber);
    if (readNumber(root, "apiVersion", packageNumber))
        manifest.apiVersion = static_cast<int>(packageNumber);
    if (readNumber(root, "dataVersion", packageNumber))
        manifest.dataVersion = static_cast<int>(packageNumber);
    readBool(root, "confirmRemoval", manifest.confirmRemoval);
    readString(root, "name", manifest.name);
    readString(root, "nameKey", manifest.nameKey);
    readString(root, "version", manifest.version);
    readString(root, "description", manifest.description);
    readString(root, "descriptionKey", manifest.descriptionKey);
    manifest.titleKeys = readStringArray(root, "titleKeys");
    if (const JsonValue* locales = root.Find("locales");
        locales && locales->IsObject())
    {
        for (const auto& [language, values] : locales->object)
        {
            if (!values.IsObject())
                continue;
            std::unordered_map<std::string, std::string> catalog;
            bool valid = true;
            for (const auto& [key, value] : values.object)
            {
                if (!value.IsString())
                {
                    valid = false;
                    break;
                }
                catalog.emplace(key, value.string);
            }
            if (valid && !catalog.empty())
                manifest.locales.emplace(language, std::move(catalog));
        }
    }
    if (!manifest.nameKey.empty())
        manifest.name = TranslateManifest(manifest.locales, manifest.nameKey, manifest.name);
    if (!manifest.descriptionKey.empty())
        manifest.description =
            TranslateManifest(manifest.locales, manifest.descriptionKey, manifest.description);
    readString(root, "publisher", manifest.publisher);
    readString(root, "minHostVersion", manifest.minHostVersion);
    readString(root, "preview", manifest.preview);
    if (const JsonValue* previewData = root.Find("previewData");
        previewData && previewData->IsObject())
    {
        readString(*previewData, "introduction",
            manifest.previewIntroduction);
        readString(*previewData, "introductionKey",
            manifest.previewIntroductionKey);
        if (const JsonValue* storage = previewData->Find("storage");
            storage && storage->IsObject())
        {
            for (const auto& [key, value] : storage->object)
            {
                if (!key.empty() &&
                    (value.IsString() || value.IsNumber() || value.IsBoolean()))
                    manifest.previewStorage[key] = valueString(value);
            }
        }
        if (const JsonValue* storageKeys = previewData->Find("storageKeys");
            storageKeys && storageKeys->IsObject())
        {
            for (const auto& [storageKey, localizationKey] :
                storageKeys->object)
            {
                if (!storageKey.empty() && localizationKey.IsString() &&
                    !localizationKey.string.empty() &&
                    manifest.previewStorage.contains(storageKey))
                    manifest.previewStorageKeys[storageKey] =
                        localizationKey.string;
            }
        }
        if (const JsonValue* variants = previewData->Find("variants");
            variants && variants->IsArray())
        {
            for (const JsonValue& value : variants->array)
            {
                if (!value.IsObject() || manifest.previewVariants.size() >= 4)
                    continue;
                snowdesktop::widget::PreviewVariant variant;
                readString(value, "id", variant.id);
                readString(value, "title", variant.title);
                readString(value, "titleKey", variant.titleKey);
                readString(value, "description", variant.description);
                readString(value, "descriptionKey", variant.descriptionKey);
                if (const JsonValue* size = value.Find("size");
                    size && size->IsObject())
                {
                    double number = 0;
                    if (readNumber(*size, "columns", number) &&
                        std::isfinite(number))
                        variant.columns = std::clamp(
                            static_cast<int>(number), 1, 8);
                    if (readNumber(*size, "rows", number) &&
                        std::isfinite(number))
                        variant.rows = std::clamp(
                            static_cast<int>(number), 1, 8);
                    std::string sizeError;
                    (void)snowdesktop::widget::ReadAspectGridSize(size,
                        variant.columns, variant.rows, variant.aspectSize, sizeError);
                }
                if (const JsonValue* variantStorage = value.Find("storage");
                    variantStorage && variantStorage->IsObject())
                {
                    for (const auto& [key, stored] : variantStorage->object)
                    {
                        if (!key.empty() && (stored.IsString() ||
                            stored.IsNumber() || stored.IsBoolean()))
                            variant.storage[key] = valueString(stored);
                    }
                }
                if (const JsonValue* variantStorageKeys =
                        value.Find("storageKeys");
                    variantStorageKeys && variantStorageKeys->IsObject())
                {
                    for (const auto& [storageKey, localizationKey] :
                        variantStorageKeys->object)
                    {
                        if (!storageKey.empty() &&
                            localizationKey.IsString() &&
                            !localizationKey.string.empty() &&
                            variant.storage.contains(storageKey))
                            variant.storageKeys[storageKey] =
                                localizationKey.string;
                    }
                }
                manifest.previewVariants.push_back(std::move(variant));
            }
        }
    }
    if (const auto* presets = root.Find("creationPresets"); presets && presets->IsArray())
    {
        for (const auto& item : presets->array)
        {
            if (!item.IsObject() || manifest.creationPresets.size() >= 32) continue;
            snowdesktop::widget::CreationPreset preset;
            readString(item, "id", preset.id);
            readString(item, "title", preset.title);
            readString(item, "titleKey", preset.titleKey);
            if (const auto* size = item.Find("size"); size && size->IsObject())
            {
                snowdesktop::widget::WidgetSizeDefinition definition;
                if (const auto* c = size->Find("columns"); c && c->IsNumber() && std::isfinite(c->number))
                    definition.columns = static_cast<int>(std::clamp(c->number, 1.0, 8.0));
                if (const auto* r = size->Find("rows"); r && r->IsNumber() && std::isfinite(r->number))
                    definition.rows = static_cast<int>(std::clamp(r->number, 1.0, 8.0));
                std::string sizeError;
                (void)snowdesktop::widget::ReadAspectGridSize(size, definition.columns,
                    definition.rows, definition.aspect, sizeError);
                preset.size = definition;
            }
            if (const auto* values = item.Find("values"); values && values->IsObject())
                for (const auto& [key, value] : values->object)
                    if (value.IsString()) preset.values[key] = value.string;
            manifest.creationPresets.push_back(std::move(preset));
        }
    }
    readString(root, "entry", manifest.entry);
    readString(root, "signature", manifest.signature);
    manifest.permissions = readStringArray(root, "permissions");
    manifest.optionalPermissions =
        readStringArray(root, "optionalPermissions");
    manifest.networkDomains = readStringArray(root, "networkDomains");
    manifest.requiredFeatures = readStringArray(root, "requiredFeatures");
    manifest.optionalFeatures = readStringArray(root, "optionalFeatures");
    {
        std::vector<snowdesktop::widget_runtime::
            LogicalSlotManifestError> slotErrors;
        (void)snowdesktop::widget_runtime::ParseLogicalSlotDeclarations(
            root.Find("slots"), manifest.logicalSlots, slotErrors);
    }
    if (const JsonValue* resources = root.Find("resources");
        resources && resources->IsObject())
    {
        for (const auto& [name, value] : resources->object)
        {
            if (!value.IsObject() || name.empty()) continue;
            snowdesktop::widget::PackageResource resource;
            readString(value, "type", resource.type);
            readString(value, "path", resource.path);
            readString(value, "license", resource.license);
            if ((resource.type == "image" || resource.type == "font" || resource.type == "audio") &&
                !resource.path.empty())
                manifest.resources.emplace(name, std::move(resource));
        }
    }
    double refreshMs = 0;
    if (readNumber(root, "refreshIntervalMs", refreshMs) &&
        std::isfinite(refreshMs) && refreshMs > 0)
    {
        const double clamped = std::clamp(refreshMs,
            static_cast<double>(kWidgetRefreshMinIntervalMs),
            static_cast<double>(kWidgetRefreshMaxIntervalMs));
        manifest.refreshIntervalMs = static_cast<int>(clamped);
    }
    if (const JsonValue* settings = root.Find("settings");
        settings && settings->IsArray())
    {
        for (const JsonValue& object : settings->array)
        {
            if (!object.IsObject())
                continue;
            LuaWidgetManifest::Setting setting;
            readString(object, "key", setting.key);
            readString(object, "label", setting.label);
            readString(object, "description", setting.description);
            readString(object, "group", setting.group);
            readString(object, "validationMessage",
                setting.validationMessage);
            readString(object, "dependsOn", setting.dependsOn);
            readString(object, "type", setting.type);
            if (setting.type == "duration")
            {
                readString(object, "unit", setting.durationUnit);
                if (const auto unit = object.Find("unit"); unit && !unit->IsString()) setting.durationUnit = "__invalid";
                const int scale = snowdesktop::widget_runtime::DurationSettingUnitSeconds(setting.durationUnit);
                setting.maxValue = scale ? snowdesktop::widget_runtime::kMaximumSettingDurationSeconds / scale : 0;
                setting.stepValue = 1;
            }
            readString(object, "searchKey", setting.searchKey);
            readString(object, "binding", setting.binding);
            readString(object, "access", setting.access);
            readString(object, "emptyLabel", setting.emptyLabel);
            readString(object, "noResultsLabel",
                setting.noResultsLabel);
            if (const JsonValue* defaultValue = object.Find("default"))
            {
                if (defaultValue->IsArray())
                {
                    for (const JsonValue& value : defaultValue->array)
                    {
                        const std::string item = valueString(value);
                        setting.defaultValues.push_back(item);
                    }
                }
                else
                    setting.defaultValue = valueString(*defaultValue);
            }
            readNumber(object, "min", setting.minValue);
            readNumber(object, "max", setting.maxValue);
            readNumber(object, "step", setting.stepValue);
            if (setting.type == "duration" && setting.defaultValue.empty())
                setting.defaultValue = std::to_string(setting.minValue);
            if (setting.type == "duration")
                for (const char* property : {"min", "max", "step"})
                    if (const auto number = object.Find(property); number && !number->IsNumber())
                        setting.stepValue = std::numeric_limits<double>::quiet_NaN();
            if (const JsonValue* minimum = object.Find("minLength"))
            {
                setting.minLength = minimum->IsNumber() &&
                    std::isfinite(minimum->number) &&
                    std::floor(minimum->number) == minimum->number &&
                    minimum->number >= (std::numeric_limits<int>::min)() &&
                    minimum->number <= (std::numeric_limits<int>::max)()
                    ? static_cast<int>(minimum->number) : -2;
            }
            if (const JsonValue* maximum = object.Find("maxLength"))
            {
                setting.maxLength = maximum->IsNumber() &&
                    std::isfinite(maximum->number) &&
                    std::floor(maximum->number) == maximum->number &&
                    maximum->number >= (std::numeric_limits<int>::min)() &&
                    maximum->number <= (std::numeric_limits<int>::max)()
                    ? static_cast<int>(maximum->number) : -2;
            }
            if (const JsonValue* required = object.Find("required"))
            {
                if (required->IsBoolean()) setting.required = required->boolean;
                else setting.minLength = -2;
            }
            setting.showWhen = readSettingCondition(object, "showWhen");
            setting.enabledWhen =
                readSettingCondition(object, "enabledWhen");
            setting.options = readStringArray(object, "options");
            setting.optionLabels =
                readStringArray(object, "optionLabels");
            setting.extensions =
                readStringArray(object, "extensions");
            if (!setting.key.empty() && !setting.label.empty())
            {
                if (setting.type.empty()) setting.type = "text";
                if (setting.type == "password")
                {
                    setting.defaultValue.clear();
                    setting.defaultValues.clear();
                    setting.options.clear();
                    setting.optionLabels.clear();
                }
                else if (IsEntityReferenceSettingType(setting.type))
                {
                    setting.defaultValue.clear();
                    setting.defaultValues.clear();
                    setting.searchKey.clear();
                    setting.options.clear();
                    setting.optionLabels.clear();
                }
                else if (IsFilesystemHandleSettingType(setting.type))
                {
                    setting.defaultValue.clear();
                    setting.defaultValues.clear();
                    setting.searchKey.clear();
                    setting.binding.clear();
                    setting.options.clear();
                    setting.optionLabels.clear();
                }
                if (!IsHostStructureSettingKey(setting.key) &&
                    !IsHostAppearanceSettingKey(setting.key))
                    manifest.settings.push_back(std::move(setting));
            }
        }
    }
    if (const JsonValue* groups = root.Find("settingGroups");
        groups && groups->IsArray())
    {
        for (const JsonValue& object : groups->array)
        {
            if (!object.IsObject()) continue;
            LuaWidgetManifest::SettingGroup group;
            readString(object, "id", group.id);
            readString(object, "label", group.label);
            readString(object, "description", group.description);
            readBool(object, "collapsible", group.collapsible);
            readBool(object, "defaultExpanded", group.defaultExpanded);
            if (group.label.empty()) group.label = group.id;
            manifest.settingGroups.push_back(std::move(group));
        }
    }
    if (const JsonValue* presets = root.Find("presets");
        presets && presets->IsArray())
    {
        for (const JsonValue& object : presets->array)
        {
            if (!object.IsObject())
                continue;
            LuaWidgetManifest::SettingPreset preset;
            readString(object, "id", preset.id);
            readString(object, "label", preset.label);
            if (preset.label.empty())
                readString(object, "name", preset.label);
            readBool(object, "default", preset.isDefault);
            if (const JsonValue* values = object.Find("values");
                values && values->IsObject())
            {
                for (const auto& [key, value] : values->object)
                {
                    if (!IsHostStructureSettingKey(key))
                    {
                        if (value.IsArray())
                        {
                            auto& array = preset.arrayValues[key];
                            for (const JsonValue& item : value.array)
                                array.push_back(valueString(item));
                        }
                        else
                            preset.values[key] = valueString(value);
                    }
                }
            }
            if (!preset.id.empty() && !preset.label.empty() &&
                (!preset.values.empty() || !preset.arrayValues.empty()))
                manifest.presets.push_back(std::move(preset));
        }
    }
    auto readSize = [&](const char* field, int& columns, int& rows,
        int minimum, int maximum) {
        const JsonValue* size = root.Find(field);
        if (!size || !size->IsObject())
            return;
        auto normalize = [&](double value) {
            if (!std::isfinite(value))
                return minimum;
            const double upper = maximum > minimum
                ? static_cast<double>(maximum)
                : static_cast<double>((std::numeric_limits<int>::max)());
            return static_cast<int>(std::clamp(
                value, static_cast<double>(minimum), upper));
        };
        double value = 0;
        if (readNumber(*size, "columns", value))
            columns = normalize(value);
        if (readNumber(*size, "rows", value))
            rows = normalize(value);
    };
    readSize("defaultSize", manifest.defaultColumns, manifest.defaultRows, 1, 8);
    {
        std::string sizeError;
        (void)snowdesktop::widget::ReadAspectGridSize(root.Find("defaultSize"),
            manifest.defaultColumns, manifest.defaultRows, manifest.defaultAspectSize, sizeError);
    }
    readSize("minSize", manifest.minColumns, manifest.minRows, 1, 1);
    readSize("maxSize", manifest.maxColumns, manifest.maxRows, 0, 0);

    if (manifest.maxColumns > 0)
        manifest.maxColumns = std::max(manifest.maxColumns, manifest.minColumns);
    if (manifest.maxRows > 0)
        manifest.maxRows = std::max(manifest.maxRows, manifest.minRows);
    manifest.defaultColumns = std::max(manifest.defaultColumns, manifest.minColumns);
    manifest.defaultRows = std::max(manifest.defaultRows, manifest.minRows);
    if (manifest.maxColumns > 0)
        manifest.defaultColumns = std::min(manifest.defaultColumns, manifest.maxColumns);
    if (manifest.maxRows > 0)
        manifest.defaultRows = std::min(manifest.defaultRows, manifest.maxRows);
    if (!manifest.previewIntroductionKey.empty())
    {
        manifest.previewIntroduction = TranslateManifest(manifest.locales,
            manifest.previewIntroductionKey,
            manifest.previewIntroduction);
    }
    for (const auto& [storageKey, localizationKey] :
        manifest.previewStorageKeys)
    {
        if (auto value = manifest.previewStorage.find(storageKey);
            value != manifest.previewStorage.end())
            value->second = TranslateManifest(
                manifest.locales, localizationKey, value->second);
    }
    for (auto& variant : manifest.previewVariants)
    {
        if (variant.columns <= 0 && variant.rows <= 0 && !variant.aspectSize)
            variant.aspectSize = manifest.defaultAspectSize;
        if (variant.columns <= 0) variant.columns = manifest.defaultColumns;
        if (variant.rows <= 0) variant.rows = manifest.defaultRows;
        if (variant.aspectSize)
        {
            const auto span = snowdesktop::widget::ResolveWidgetSize(
                {variant.columns, variant.rows, variant.aspectSize},
                snowdesktop::widget::StandardWidgetSizeGridMetrics(), manifest.minColumns,
                manifest.minRows, manifest.maxColumns, manifest.maxRows);
            variant.columns = span.first; variant.rows = span.second;
        }
        if (!variant.titleKey.empty())
            variant.title = TranslateManifest(
                manifest.locales, variant.titleKey, variant.title);
        if (!variant.descriptionKey.empty())
            variant.description = TranslateManifest(
                manifest.locales, variant.descriptionKey, variant.description);
        for (const auto& [storageKey, localizationKey] : variant.storageKeys)
        {
            if (auto value = variant.storage.find(storageKey);
                value != variant.storage.end())
                value->second = TranslateManifest(
                    manifest.locales, localizationKey, value->second);
        }
    }

    for (auto& preset : manifest.creationPresets)
        if (!preset.titleKey.empty()) preset.title = TranslateManifest(
            manifest.locales, preset.titleKey, preset.title);

    if (manifest.permissions.empty())
        manifest.permissions = {};
    if (!manifest.signature.empty())
    {
        std::string expected = manifest.signature;
        if (expected.starts_with("sha256:")) expected.erase(0, 7);
        std::transform(expected.begin(), expected.end(), expected.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        manifest.signatureValid = expected == HashWidgetFile(fullPath);
    }
    return manifest;
}
}
