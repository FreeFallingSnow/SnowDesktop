#include "full_data_backup.h"
#include "layout_storage.h"
#include "widget_package.h"
#include "portable_data_migration.h"
#include "single_instance.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace snowdesktop::widget;

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool CorruptArchivePayload(const std::filesystem::path& path,
    const std::string& payload)
{
    std::fstream file(path,
        std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        return false;
    std::vector<char> bytes;
    bytes.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    const auto found = std::search(
        bytes.begin(), bytes.end(), payload.begin(), payload.end());
    if (found == bytes.end())
        return false;
    const auto position =
        static_cast<std::streamoff>(found - bytes.begin());
    file.clear();
    file.seekp(position);
    const char replacement =
        payload.front() == 'X' ? 'Y' : 'X';
    file.write(&replacement, 1);
    return static_cast<bool>(file);
}

void Write16(std::ofstream& output, std::uint16_t value)
{
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void Write32(std::ofstream& output, std::uint32_t value)
{
    Write16(output, static_cast<std::uint16_t>(value & 0xffff));
    Write16(output, static_cast<std::uint16_t>(value >> 16));
}

void MakeUnsafeArchive(const std::filesystem::path& path,
    const std::vector<std::string>& names)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    for (const auto& name : names)
    {
        Write32(output, 0x04034b50);
        Write16(output, 20);
        Write16(output, 0x0800);
        Write16(output, 0);
        Write16(output, 0);
        Write16(output, 0);
        Write32(output, 0);
        Write32(output, 0);
        Write32(output, 0);
        Write16(output, static_cast<std::uint16_t>(name.size()));
        Write16(output, 0);
        output.write(name.data(), static_cast<std::streamsize>(name.size()));
    }
}

void MakePackage(const std::filesystem::path& root, std::string version,
    std::string id = "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
    std::string permissions = "\"ui.input\"",
    std::string networkDomains = "",
    std::string entry = "main.lua",
    std::string optionalPermissions = "",
    int schemaVersion = 1,
    int apiVersion = 1,
    std::string requiredFeatures = "",
    std::string optionalFeatures = "",
    std::string resources = "",
    std::string slots = "")
{
    Write(root / std::filesystem::path(entry), "function render() end\n");
    Write(root / L"assets" / L"label.txt", "asset");
    Write(root / L"widget.json",
        "{\n"
        "  \"schemaVersion\": " + std::to_string(schemaVersion) + ",\n"
        "  \"id\": \"" + id + "\",\n"
        "  \"slug\": \"package-test\",\n"
        "  \"version\": \"" + version + "\",\n"
        "  \"apiVersion\": " + std::to_string(apiVersion) + ",\n"
        "  \"dataVersion\": 1,\n"
        "  \"entry\": \"" + entry + "\",\n"
        "  \"minHostVersion\": \"1.0.1.0\",\n"
        "  \"name\": \"Package Test\",\n"
        "  \"description\": \"English fallback\",\n"
        "  \"locales\": {\"zh-CN\": {\"preview.intro\": \"多尺寸介绍\", \"preview.message\": \"预览消息\", \"preview.compact\": \"紧凑模式\", \"preview.compact_hint\": \"紧凑说明\", \"preview.compact_mode\": \"紧凑数据\"}},\n"
        "  \"author\": \"SnowDesktop\",\n"
        "  \"license\": \"GPL-3.0-only\",\n"
        "  \"resources\": {" + resources + "},\n"
        "  \"slots\": {" + slots + "},\n"
        "  \"previewData\": {\"introduction\": \"Multiple sizes\", \"introductionKey\": \"preview.intro\", \"storage\": {\"message\": \"Preview\", \"count\": 3}, \"storageKeys\": {\"message\": \"preview.message\"}, \"variants\": [{\"id\": \"compact\", \"title\": \"Compact\", \"titleKey\": \"preview.compact\", \"description\": \"Compact hint\", \"descriptionKey\": \"preview.compact_hint\", \"size\": {\"columns\": 1, \"rows\": 1}, \"storage\": {\"mode\": \"compact\"}, \"storageKeys\": {\"mode\": \"preview.compact_mode\"}}, {\"id\": \"wide\", \"title\": \"Wide\", \"description\": \"Wide hint\", \"size\": {\"columns\": 2, \"rows\": 1}}]},\n"
        "  \"permissions\": [" + permissions + "],\n"
        "  \"optionalPermissions\": [" + optionalPermissions + "],\n"
        "  \"networkDomains\": [" + networkDomains + "],\n"
        "  \"requiredFeatures\": [" + requiredFeatures + "],\n"
        "  \"optionalFeatures\": [" + optionalFeatures + "]\n"
        "}\n");
}

class DeclaredArtifactSource final : public IWidgetPackageSource
{
public:
    DeclaredArtifactSource(
        PackageDetails details, std::filesystem::path artifact)
        : details_(std::move(details)), artifact_(std::move(artifact)) {}

    std::string ProviderId() const override { return "declared-artifact"; }
    ProviderCapabilities Capabilities() const override
    {
        return { false, true, true, false, false, false };
    }
    ProviderStatus Status() override { return { true, "available" }; }
    std::vector<PackageDetails> Query(
        const PackageQuery&, std::string& error) override
    {
        error.clear();
        return { details_ };
    }
    std::optional<PackageDetails> GetDetails(
        const std::string& externalItemId, std::string& error) override
    {
        if (externalItemId == details_.source.externalItemId)
            return details_;
        error = "not found";
        return std::nullopt;
    }
    std::optional<PackageArtifact> Materialize(
        const std::string& externalItemId, const std::string& version,
        const std::filesystem::path& destination,
        std::string& error) override
    {
        if (externalItemId != details_.source.externalItemId ||
            version != details_.manifest.version)
        {
            error = "not found";
            return std::nullopt;
        }
        std::error_code ec;
        std::filesystem::copy_file(artifact_, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = ec.message();
            return std::nullopt;
        }
        return PackageArtifact{
            destination, details_.manifest.id, version, {} };
    }
    std::vector<PackageUpdate> CheckUpdates(
        const std::vector<PackageVersionRef>&,
        std::string& error) override
    {
        error.clear();
        return {};
    }

private:
    PackageDetails details_;
    std::filesystem::path artifact_;
};

PackagePaths TestPaths(const std::filesystem::path& root)
{
    PackagePaths paths;
    paths.builtin = root / L"builtin";
    paths.installed = root / L"data" / L"widgets" / L"installed";
    paths.development = root / L"data" / L"widgets" / L"dev";
    paths.staging = root / L"data" / L"widgets" / L"staging";
    paths.quarantine = root / L"data" / L"widgets" / L"quarantine";
    paths.migrations = root / L"data" / L"widgets" / L"migrations";
    paths.registry = root / L"data" / L"widgets" / L"packages.json";
    return paths;
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopApplicationDataLifecycleTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);

    const auto layoutPath =
        root / L"layout-storage" / L"SnowDesktop.layout.json";
    const std::string firstLayout =
        "{\"revision\":1,\"metadata\":{\"key\":\"not-an-item\"},"
        "\"pages\":[{\"rows\":3,\"id\":\"page-a\",\"columns\":4}],"
        "\"items\":[{\"y\":2,\"key\":\"item-a\",\"x\":1,"
        "\"page\":\"page-a\",\"w\":2,\"h\":1}],"
        "\"widgets\":[{\"y\":0,\"id\":\"widget-a\",\"x\":0,"
        "\"page\":\"page-a\",\"type\":\"collection\","
        "\"items\":[\"item-b\"]}],"
        "\"dockEntries\":[{\"ref\":\"item-a\",\"type\":\"item\"}],"
        "\"navTabOrder\":[\"widget-a\"]}";
    const std::string secondLayout =
        "{\"layoutSchemaVersion\":1,\"pages\":[],\"items\":[],\"widgets\":[],"
        "\"dockEntries\":[],\"navTabOrder\":[],\"revision\":2}";
    const std::string thirdLayout =
        "{\"layoutSchemaVersion\":1,\"pages\":[],\"items\":[],\"widgets\":[],"
        "\"dockEntries\":[],\"navTabOrder\":[],\"revision\":3}";
    std::string layoutError;
    Expect(!snowdesktop::layout_storage::ValidateDocument(
            "{\"pages\":[}", &layoutError) && !layoutError.empty(),
        "truncated layout JSON is rejected before application state changes");
    Expect(!snowdesktop::layout_storage::ValidateDocument(
            "{\"pages\":{}}", &layoutError),
        "layout collection fields must preserve their schema types");
    Expect(!snowdesktop::layout_storage::ValidateDocument(
            "{\"layoutSchemaVersion\":2}", &layoutError),
        "unsupported future layout schemas are rejected without partial loading");
    struct InvalidLayoutCase
    {
        const char* name;
        const char* document;
        const char* errorPath;
    };
    const InvalidLayoutCase invalidLayoutCases[] = {
        { "root scalar type", "[]", "layout root" },
        { "top-level boolean type", "{\"dockEnabled\":1}",
            "dockEnabled" },
        { "top-level number type", "{\"itemFontSize\":\"16\"}",
            "itemFontSize" },
        { "list font size type", "{\"listItemFontSize\":\"15\"}",
            "listItemFontSize" },
        { "cu title font size type", "{\"itemFontSizeCu\":\"16\"}",
            "itemFontSizeCu" },
        { "cu list font size type", "{\"listItemFontSizeCu\":\"15\"}",
            "listItemFontSizeCu" },
        { "component spacing type", "{\"componentSpacing\":\"1.5\"}",
            "componentSpacing" },
        { "integer exactness", "{\"shortcutArrowMode\":1.5}",
            "shortcutArrowMode" },
        { "page required field", "{\"pages\":[{\"columns\":4}]}",
            "pages[0].id" },
        { "item coordinate group",
            "{\"items\":[{\"key\":\"a\",\"page\":\"p\",\"x\":1}]}",
            "items[0]" },
        { "widget required field",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0}]}",
            "widgets[0].y" },
        { "widget boolean type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"showTitle\":\"yes\"}]}",
            "widgets[0].showTitle" },
        { "widget details type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"showDetails\":\"yes\"}]}",
            "widgets[0].showDetails" },
        { "widget detail column type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"detailShowModified\":\"yes\"}]}",
            "widgets[0].detailShowModified" },
        { "widget detail width type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"detailModifiedWidth\":\"160\"}]}",
            "widgets[0].detailModifiedWidth" },
        { "widget detail position type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"detailModifiedPosition\":\"0.25\"}]}",
            "widgets[0].detailModifiedPosition" },
        { "widget content sort type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"contentSortColumn\":2}]}",
            "widgets[0].contentSortColumn" },
        { "widget package source URL type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"packageSourceUrl\":7}]}",
            "widgets[0].packageSourceUrl" },
        { "nested string-array type",
            "{\"widgets\":[{\"id\":\"w\",\"page\":\"p\",\"x\":0,"
            "\"y\":0,\"items\":[\"a\",2]}]}",
            "widgets[0].items[1]" },
        { "dock required field",
            "{\"dockEntries\":[{\"type\":\"item\"}]}",
            "dockEntries[0].ref" },
        { "navigation array type", "{\"navTabOrder\":[\"w\",false]}",
            "navTabOrder[1]" },
        { "icon beautify shape type",
            "{\"iconBeautifyShape\":\"circle\"}",
            "iconBeautifyShape" },
        { "icon beautify preset type",
            "{\"iconBeautifyPreset\":\"classic\"}",
            "iconBeautifyPreset" },
        { "icon beautify finish type",
            "{\"iconBeautifyFinish\":true}",
            "iconBeautifyFinish" },
        { "icon beautify texture type",
            "{\"iconBeautifyTextureHighlightStrength\":\"strong\"}",
            "iconBeautifyTextureHighlightStrength" },
        { "icon beautify filter enabled type",
            "{\"iconBeautifyFilterEnabled\":1}",
            "iconBeautifyFilterEnabled" },
        { "icon beautify filter strength type",
            "{\"iconBeautifyFilterStrength\":\"full\"}",
            "iconBeautifyFilterStrength" },
        { "icon beautify content scale type",
            "{\"iconBeautifyContentScale\":\"large\"}",
            "iconBeautifyContentScale" },
        { "icon beautify outline enabled type",
            "{\"iconBeautifyOutlineEnabled\":1}",
            "iconBeautifyOutlineEnabled" },
        { "icon beautify outline color type",
            "{\"iconBeautifyOutlineR\":[]}",
            "iconBeautifyOutlineR" },
    };
    for (const auto& invalid : invalidLayoutCases)
    {
        layoutError.clear();
        Expect(!snowdesktop::layout_storage::ValidateDocument(
                invalid.document, &layoutError) &&
                layoutError.find(invalid.errorPath) != std::string::npos,
            (std::string("typed layout matrix rejects ") +
                invalid.name).c_str());
    }
    snowdesktop::layout_storage::Document typedLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            firstLayout, typedLayout, &layoutError) &&
            typedLayout.sourceSchemaVersion == 0 &&
            typedLayout.schemaVersion ==
                snowdesktop::layout_storage::kCurrentSchemaVersion &&
            typedLayout.pages.size() == 1 &&
            typedLayout.pages[0].id == "page-a" &&
            typedLayout.items.size() == 1 &&
            typedLayout.items[0].key == "item-a" &&
            typedLayout.widgets.size() == 1 &&
            typedLayout.dockEntries.size() == 1 &&
            !typedLayout.componentSpacing.has_value(),
        "legacy schema and reordered fields decode into one typed document");
    snowdesktop::layout_storage::Document spacingLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"componentSpacing\":1.5}", spacingLayout, &layoutError) &&
            spacingLayout.componentSpacing.has_value() &&
            *spacingLayout.componentSpacing == 1.5f,
        "component spacing is decoded as an optional layout setting");
    snowdesktop::layout_storage::Document legacyFontLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"itemFontSize\":18,\"listItemFontSize\":16}",
            legacyFontLayout, &layoutError) &&
            legacyFontLayout.itemFontSize.value_or(0.0f) == 18.0f &&
            legacyFontLayout.listItemFontSize.value_or(0.0f) == 16.0f &&
            !legacyFontLayout.itemFontSizeCu.has_value() &&
            !legacyFontLayout.listItemFontSizeCu.has_value(),
        "legacy point font fields remain available as migration inputs");
    const std::string detailsLayoutText =
        "{\"layoutSchemaVersion\":1,"
        "\"widgetContentOptionsSchemaVersion\":4,"
        "\"itemFontSizeCu\":18,\"listItemFontSizeCu\":16,"
        "\"widgets\":[{\"id\":\"details-widget\",\"page\":\"page-a\","
        "\"x\":0,\"y\":0,\"type\":\"folderMapping\","
        "\"showDetails\":true,\"detailShowModified\":true,"
        "\"detailShowType\":false,\"detailShowSize\":true,"
        "\"detailModifiedPosition\":0.22,"
        "\"detailTypePosition\":0.61,"
        "\"detailSizePosition\":0.84,\"contentSortColumn\":\"size\","
        "\"contentSortAscending\":false}]}";
    snowdesktop::layout_storage::Document detailsLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            detailsLayoutText, detailsLayout, &layoutError) &&
            detailsLayout.widgetContentOptionsSchemaVersion.value_or(0) == 4 &&
            detailsLayout.itemFontSizeCu.value_or(0.0f) == 18.0f &&
            detailsLayout.listItemFontSizeCu.value_or(0.0f) == 16.0f &&
            detailsLayout.widgets.size() == 1 &&
            detailsLayout.widgets[0].showDetails &&
            detailsLayout.widgets[0].detailShowModified &&
            !detailsLayout.widgets[0].detailShowType &&
            detailsLayout.widgets[0].detailShowSize &&
            detailsLayout.widgets[0].detailModifiedPosition.value_or(0.0f) ==
                0.22f &&
            detailsLayout.widgets[0].detailTypePosition.value_or(0.0f) ==
                0.61f &&
            detailsLayout.widgets[0].detailSizePosition.value_or(0.0f) ==
                0.84f &&
            detailsLayout.widgets[0].contentSortColumn == "size" &&
            !detailsLayout.widgets[0].contentSortAscending,
        "list font and detail view state decode into the typed layout model");
    const auto detailsLayoutPath =
        root / L"layout-storage" / L"details.layout.json";
    Expect(snowdesktop::layout_storage::SaveDocument(
            detailsLayoutPath, detailsLayoutText, &layoutError),
        "detail view layout fields save through the validated storage path");
    snowdesktop::layout_storage::Document loadedDetailsLayout;
    const auto detailsLayoutLoad =
        snowdesktop::layout_storage::LoadDocument(
            detailsLayoutPath, loadedDetailsLayout);
    Expect(detailsLayoutLoad.status ==
            snowdesktop::layout_storage::LoadStatus::LoadedPrimary &&
            loadedDetailsLayout.itemFontSizeCu.value_or(0.0f) == 18.0f &&
            loadedDetailsLayout.listItemFontSizeCu.value_or(0.0f) == 16.0f &&
            loadedDetailsLayout.widgets.size() == 1 &&
            loadedDetailsLayout.widgets[0].showDetails &&
            loadedDetailsLayout.widgets[0].detailShowModified &&
            !loadedDetailsLayout.widgets[0].detailShowType &&
            loadedDetailsLayout.widgets[0].detailShowSize &&
            loadedDetailsLayout.widgets[0].detailModifiedPosition.value_or(0.0f) ==
                0.22f &&
            loadedDetailsLayout.widgets[0].detailSizePosition.value_or(0.0f) ==
                0.84f &&
            loadedDetailsLayout.widgets[0].contentSortColumn == "size",
        "list font and detail view fields round-trip through layout storage");
    snowdesktop::layout_storage::Document legacyBeautifyLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"iconBeautifyEnabled\":true,\"iconBeautifyMode\":0}",
            legacyBeautifyLayout, &layoutError) &&
            legacyBeautifyLayout.iconBeautifyEnabled.value_or(false) &&
            !legacyBeautifyLayout.iconBeautifyPreset.has_value() &&
            !legacyBeautifyLayout.iconBeautifyShape.has_value() &&
            !legacyBeautifyLayout.iconBeautifyFinish.has_value() &&
            !legacyBeautifyLayout.iconBeautifyTextureHighlightStrength.has_value() &&
            !legacyBeautifyLayout.iconBeautifyFilterEnabled.has_value() &&
            !legacyBeautifyLayout.iconBeautifyFilterStrength.has_value() &&
            !legacyBeautifyLayout.iconBeautifyContentScale.has_value(),
        "legacy icon beautify layouts keep new fields optional");
    snowdesktop::layout_storage::Document iconBeautifyLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"iconBeautifyPreset\":5,"
            "\"iconBeautifyShape\":10,"
            "\"iconBeautifyContentScale\":0.73,"
            "\"iconBeautifyFinish\":2,"
            "\"iconBeautifyTextureHighlightStrength\":0.4,"
            "\"iconBeautifyTextureHighlightSize\":0.6,"
            "\"iconBeautifyTextureHighlightAngle\":-0.3,"
            "\"iconBeautifyTextureShadeStrength\":0.2,"
            "\"iconBeautifyTextureEdgeHighlight\":0.5,"
            "\"iconBeautifyFilterEnabled\":true,"
            "\"iconBeautifyFilterStrength\":0.85,"
            "\"iconBeautifyFilterTintR\":0.7,"
            "\"iconBeautifyFilterTintG\":0.8,"
            "\"iconBeautifyFilterTintB\":0.9,"
            "\"iconBeautifyOutlineEnabled\":true,"
            "\"iconBeautifyOutlineWidth\":2.5,"
            "\"iconBeautifyOutlineOpacity\":0.6,"
            "\"iconBeautifyOutlineR\":0.1,"
            "\"iconBeautifyOutlineG\":0.2,"
            "\"iconBeautifyOutlineB\":0.3,"
            "\"iconBeautifyShadowStrength\":0.42}",
            iconBeautifyLayout, &layoutError) &&
            iconBeautifyLayout.iconBeautifyPreset.value_or(-1) == 5 &&
            iconBeautifyLayout.iconBeautifyShape.value_or(-1) == 10 &&
            iconBeautifyLayout.iconBeautifyContentScale.value_or(0.0f) == 0.73f &&
            iconBeautifyLayout.iconBeautifyFinish.value_or(-1) == 2 &&
            iconBeautifyLayout.iconBeautifyTextureHighlightStrength.value_or(0.0f) == 0.4f &&
            iconBeautifyLayout.iconBeautifyTextureHighlightSize.value_or(0.0f) == 0.6f &&
            iconBeautifyLayout.iconBeautifyTextureHighlightAngle.value_or(0.0f) == -0.3f &&
            iconBeautifyLayout.iconBeautifyTextureShadeStrength.value_or(0.0f) == 0.2f &&
            iconBeautifyLayout.iconBeautifyTextureEdgeHighlight.value_or(0.0f) == 0.5f &&
            iconBeautifyLayout.iconBeautifyFilterEnabled.value_or(false) &&
            iconBeautifyLayout.iconBeautifyFilterStrength.value_or(0.0f) == 0.85f &&
            iconBeautifyLayout.iconBeautifyFilterTintR.value_or(0.0f) == 0.7f &&
            iconBeautifyLayout.iconBeautifyFilterTintG.value_or(0.0f) == 0.8f &&
            iconBeautifyLayout.iconBeautifyFilterTintB.value_or(0.0f) == 0.9f &&
            iconBeautifyLayout.iconBeautifyOutlineEnabled.value_or(false) &&
            !iconBeautifyLayout.iconBeautifyOutlineMode.has_value() &&
            iconBeautifyLayout.iconBeautifyOutlineWidth.value_or(0.0f) == 2.5f &&
            iconBeautifyLayout.iconBeautifyOutlineOpacity.value_or(0.0f) == 0.6f &&
            iconBeautifyLayout.iconBeautifyOutlineR.value_or(0.0f) == 0.1f &&
            iconBeautifyLayout.iconBeautifyOutlineG.value_or(0.0f) == 0.2f &&
            iconBeautifyLayout.iconBeautifyOutlineB.value_or(0.0f) == 0.3f &&
            iconBeautifyLayout.iconBeautifyShadowStrength.value_or(0.0f) == 0.42f,
        "all icon beautify fields decode into the typed layout model");
    snowdesktop::layout_storage::Document legacyOutlineLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"iconBeautifyOutlineMode\":2}",
            legacyOutlineLayout, &layoutError) &&
            !legacyOutlineLayout.iconBeautifyOutlineEnabled.has_value() &&
            legacyOutlineLayout.iconBeautifyOutlineMode.value_or(-1) == 2,
        "legacy outline mode remains available for bool migration");
    snowdesktop::layout_storage::Document packageSourceLayout;
    Expect(snowdesktop::layout_storage::ParseDocument(
            "{\"widgets\":[{\"id\":\"workshop-widget\","
            "\"page\":\"page-a\",\"x\":1,\"y\":2,"
            "\"type\":\"lua\",\"packageId\":\"package-a\","
            "\"packageSourceProvider\":\"steam-workshop\","
            "\"packageSourceExternalItemId\":\"3780926790@42\","
            "\"packageSourceUrl\":\"https://steamcommunity.com/"
            "sharedfiles/filedetails/?id=3780926790\"}]}",
            packageSourceLayout, &layoutError) &&
            packageSourceLayout.widgets.size() == 1 &&
            packageSourceLayout.widgets[0].packageSourceProvider ==
                "steam-workshop" &&
            packageSourceLayout.widgets[0].packageSourceExternalItemId ==
                "3780926790@42" &&
            packageSourceLayout.widgets[0].packageSourceUrl ==
                "https://steamcommunity.com/sharedfiles/filedetails/"
                "?id=3780926790",
        "layout preserves a Workshop source address for missing widgets");
    Expect(snowdesktop::layout_storage::SaveDocument(
            layoutPath, firstLayout, &layoutError),
        "first layout document is written atomically");
    snowdesktop::layout_storage::Document loadedLayout;
    auto layoutLoad = snowdesktop::layout_storage::LoadDocument(
        layoutPath, loadedLayout);
    Expect(layoutLoad.status ==
            snowdesktop::layout_storage::LoadStatus::LoadedPrimary &&
            loadedLayout.pages.size() == 1 &&
            loadedLayout.items.size() == 1,
        "valid primary layout document loads without fallback");
    Expect(snowdesktop::layout_storage::SaveDocument(
            layoutPath, secondLayout, &layoutError) &&
            Read(snowdesktop::layout_storage::BackupPath(layoutPath)) ==
                firstLayout,
        "replacing a valid layout preserves the previous last-good document");
    Write(layoutPath, "{\"pages\":[}");
    layoutLoad = snowdesktop::layout_storage::LoadDocument(
        layoutPath, loadedLayout);
    Expect(layoutLoad.status ==
            snowdesktop::layout_storage::LoadStatus::RecoveredBackup &&
            loadedLayout.pages.size() == 1 &&
            loadedLayout.pages[0].id == "page-a",
        "corrupt primary layout recovers from the last-good document");
    const std::string corruptPrimary = Read(layoutPath);
    Expect(!snowdesktop::layout_storage::SaveDocument(
            layoutPath, "{\"items\":[}", &layoutError) &&
            Read(layoutPath) == corruptPrimary,
        "invalid replacement layout never touches the active file");
    Expect(snowdesktop::layout_storage::SaveDocument(
            layoutPath, thirdLayout, &layoutError) &&
            Read(snowdesktop::layout_storage::BackupPath(layoutPath)) ==
                firstLayout,
        "saving after recovery does not overwrite the last-good backup with corruption");

    Expect(snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" --wait-for-pid=4321") ==
            4321,
        "restart predecessor PID is parsed from the internal command line");
    Expect(snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" --wait-for-pid=0") == 0 &&
        snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" x--wait-for-pid=42") == 0,
        "invalid or embedded restart predecessor arguments are rejected");
    Expect(snowdesktop::single_instance::VersionsMatch(
            L"1.0.1", L"1.0.1.0") &&
        !snowdesktop::single_instance::VersionsMatch(
            L"1.0.1.0", L"1.0.2.0") &&
        !snowdesktop::single_instance::VersionsMatch(
            L"1.0.1.", L"1.0.1.0"),
        "instance versions compare normalized numeric parts");
    Expect(snowdesktop::single_instance::DataDirectoriesMatch(
            L"C:\\SnowDesktop\\data\\",
            L"c:/snowdesktop/data") &&
        !snowdesktop::single_instance::DataDirectoriesMatch(
            L"C:\\SnowDesktop\\data",
            L"D:\\SnowDesktop\\data"),
        "instance data directories compare canonical path spelling");
    const std::wstring testMutexName =
        L"Local\\SnowDesktop.SingleInstance.Test." +
        std::to_wstring(GetCurrentProcessId());
    {
        snowdesktop::single_instance::Guard firstInstance;
        snowdesktop::single_instance::Guard secondInstance;
        Expect(firstInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Primary,
            "the first executable location owns the shared instance lock");
        Expect(secondInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Existing,
            "a second executable location sees the existing instance");
    }
    {
        snowdesktop::single_instance::Guard replacementInstance;
        Expect(replacementInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Primary,
            "the shared instance lock is released when the owner exits");
    }

    const auto sourceV1 = root / L"source-v1";
    MakePackage(sourceV1, "1.0.0",
        "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
        "\"ui.input\", \"network.http\"", "\"api.example.com\"");
    WidgetPackageValidator validator;
    PackageManifest manifest;
    auto report = validator.ValidateDirectory(sourceV1, &manifest);
    Expect(report.Ok(), "valid folder package is accepted");
    Expect(!IsExecutablePackageContract(manifest),
        "schema/API v1 is migration input and cannot enter the runtime");
    Expect(std::any_of(report.issues.begin(), report.issues.end(),
            [](const ValidationIssue& issue) {
                return issue.severity == ValidationSeverity::Warning &&
                    issue.code == "migration.apiV1";
            }),
        "legacy package validation emits an explicit API v2 migration diagnostic");
    Expect(manifest.entry == "main.lua", "entry is parsed");
    Expect(manifest.previewStorage["message"] == "Preview" &&
            manifest.previewStorage["count"] == "3" &&
            manifest.previewStorageKeys["message"] == "preview.message",
        "preview storage and its localization keys are validated and parsed");
    Expect(manifest.previewIntroduction == "Multiple sizes" &&
            manifest.previewVariants.size() == 2 &&
            manifest.previewVariants[0].columns == 1 &&
            manifest.previewVariants[0].storage["mode"] == "compact" &&
            manifest.previewVariants[0].storageKeys["mode"] ==
                "preview.compact_mode" &&
            manifest.previewVariants[1].columns == 2,
        "multi-size preview variants and per-variant storage are parsed");
    const auto localizedPreview =
        LocalizePackageManifest(manifest, "zh-CN");
    Expect(localizedPreview.previewIntroduction == "多尺寸介绍" &&
            localizedPreview.previewStorage.at("message") == "预览消息" &&
            localizedPreview.previewVariants[0].title == "紧凑模式" &&
            localizedPreview.previewVariants[0].description == "紧凑说明" &&
            localizedPreview.previewVariants[0].storage.at("mode") ==
                "紧凑数据",
        "preview text and storage examples use manifest localization keys");
    Expect(WidgetPackageValidator::IsUuid(manifest.id), "UUID is valid");
    Expect(WidgetPackageValidator::IsSemVer("1.2.3-beta.1+build.7"),
        "SemVer prerelease is valid");
    Expect(WidgetPackageValidator::IsNewerSemVer(
            "1.0.0-beta", "1.0.0-1") &&
        !WidgetPackageValidator::IsNewerSemVer(
            "1.0.0-1", "1.0.0-beta"),
        "SemVer non-numeric prerelease identifiers outrank numeric ones");
    Expect(!WidgetPackageValidator::IsSafeRelativePath(L"../escape.lua"),
        "parent traversal is rejected");
    manifest.locales["en-US"] = { "English title", "English description" };
    manifest.locales["zh-CN"] = { "中文标题", "中文介绍" };
    Expect(LocalizePackageManifest(manifest, "zh-CN").name == "中文标题",
        "package metadata uses the exact requested locale");
    Expect(LocalizePackageManifest(manifest, "ZH-cn").description ==
            "中文介绍",
        "package locale matching is case insensitive");
    Expect(LocalizePackageManifest(manifest, "zh-Hans").name == "中文标题",
        "package metadata falls back to the requested language family");
    Expect(LocalizePackageManifest(manifest, "fr-FR").name == manifest.name,
        "unknown package locale keeps the English manifest fallback");

    const auto contractV2Package = root / L"contract-v2";
    MakePackage(contractV2Package, "2.0.0",
        "ecffdc71-2600-44c2-b0f5-9941a583dc81", "", "", "main.lua",
        "", 2, 2,
        "\"draw.immediate\", \"interaction.contextMenu\"",
        "\"data.app.indexStatus\", \"view.tree\"");
    PackageManifest manifestV2;
    report = validator.ValidateDirectory(contractV2Package, &manifestV2);
    Expect(report.Ok() && manifestV2.schemaVersion == 2 &&
            manifestV2.apiVersion == 2 &&
            IsExecutablePackageContract(manifestV2) &&
            manifestV2.requiredFeatures ==
                std::vector<std::string>{ "draw.immediate",
                    "interaction.contextMenu" } &&
            manifestV2.optionalFeatures ==
                std::vector<std::string>{ "data.app.indexStatus",
                    "view.tree" },
        "schema/API v2 lower-camel feature segments are parsed and accepted");
    PackageManifest mixedContract = manifestV2;
    mixedContract.apiVersion = 1;
    Expect(!IsExecutablePackageContract(mixedContract),
        "mixed schema/API contracts cannot enter the runtime");

    const auto logicalSlotPackage = root / L"logical-slot-package";
    MakePackage(logicalSlotPackage, "2.0.0",
        "689ce096-bb57-4ccc-8cf5-7d75ba1987b8", "", "", "main.lua",
        "", 2, 2, "\"slots.model\", \"view.logicalSlots\"", "",
        "",
        "\"primaryApp\": {\"kind\": \"binding\", "
        "\"accepts\": [\"app.reference\"], "
        "\"operation\": \"reference\", \"replacePolicy\": \"allow\", "
        "\"allowClear\": true}, "
        "\"favorites\": {\"kind\": \"collection\", "
        "\"accepts\": [\"desktop.item\", \"filesystem.reference\"], "
        "\"operation\": \"reference\", \"capacity\": 32}");
    PackageManifest logicalSlotManifest;
    report = validator.ValidateDirectory(logicalSlotPackage,
        &logicalSlotManifest);
    Expect(report.Ok() && logicalSlotManifest.logicalSlots.size() == 2 &&
            logicalSlotManifest.logicalSlots.at("primaryApp").kind ==
                snowdesktop::widget_runtime::LogicalSlotKind::Binding &&
            logicalSlotManifest.logicalSlots.at("favorites").capacity == 32,
        "v2 package binding and collection logical slots are validated and parsed");

    const auto invalidLogicalSlotPackage = root / L"logical-slot-invalid";
    MakePackage(invalidLogicalSlotPackage, "2.0.0",
        "7ef66c66-f87c-4976-923b-073e07901196", "", "", "main.lua",
        "", 2, 2, "", "", "",
        "\"favorites\": {\"kind\": \"collection\", "
        "\"accepts\": [\"widget\"], \"operation\": \"move\", "
        "\"capacity\": 65}");
    Expect(!validator.ValidateDirectory(invalidLogicalSlotPackage).Ok(),
        "logical slots reject unknown payload kinds, destructive operations, and excessive capacity");

    const auto legacyLogicalSlotPackage = root / L"logical-slot-legacy";
    MakePackage(legacyLogicalSlotPackage, "1.0.0",
        "243f1469-cba6-43b3-a471-d18d9162dcb0", "", "", "main.lua",
        "", 1, 1, "", "", "",
        "\"primaryApp\": {\"kind\": \"binding\", "
        "\"accepts\": [\"app.reference\"]}");
    Expect(!validator.ValidateDirectory(legacyLogicalSlotPackage).Ok(),
        "schema/API v1 packages cannot declare v2 logical slots");

    const auto resourcePackage = root / L"resource-package";
    MakePackage(resourcePackage, "2.0.0",
        "1d1bfbc3-e777-4b59-8124-6e53f188ae5b", "", "", "main.lua",
        "", 2, 2, "\"resource.package\"", "",
        "\"logo\": {\"type\": \"image\", \"path\": "
        "\"assets/logo.png\"}, "
        "\"body\": {\"type\": \"font\", \"path\": "
        "\"assets/body.ttf\", \"license\": \"OFL-1.1\"}");
    std::string pngHeader(24, '\0');
    const std::string pngSignature("\x89PNG\r\n\x1a\n", 8);
    pngHeader.replace(0, pngSignature.size(), pngSignature);
    pngHeader.replace(12, 4, "IHDR");
    pngHeader[19] = 1;
    pngHeader[23] = 1;
    Write(resourcePackage / L"assets" / L"logo.png", pngHeader);
    Write(resourcePackage / L"assets" / L"body.ttf",
        std::string("\0\1\0\0", 4));
    PackageManifest resourceManifest;
    report = validator.ValidateDirectory(resourcePackage, &resourceManifest);
    Expect(report.Ok() && resourceManifest.resources.size() == 2 &&
            resourceManifest.resources.at("logo").type == "image" &&
            resourceManifest.resources.at("body").license == "OFL-1.1",
        "v2 package image and private font resources are validated and parsed");

    const auto traversalResourcePackage = root / L"resource-traversal";
    MakePackage(traversalResourcePackage, "2.0.0",
        "0c237b9b-7e14-44c8-9891-cd378f7b6ce6", "", "", "main.lua",
        "", 2, 2, "", "",
        "\"logo\": {\"type\": \"image\", \"path\": "
        "\"../logo.png\"}");
    Expect(!validator.ValidateDirectory(traversalResourcePackage).Ok(),
        "v2 package resource paths cannot escape the package root");

    const auto badResourceContentPackage = root / L"resource-content";
    MakePackage(badResourceContentPackage, "2.0.0",
        "4d399123-9ed4-4288-bbd0-4ea67dbb7aec", "", "", "main.lua",
        "", 2, 2, "", "",
        "\"logo\": {\"type\": \"image\", \"path\": "
        "\"assets/logo.png\"}");
    Write(badResourceContentPackage / L"assets" / L"logo.png",
        "not a png");
    Expect(!validator.ValidateDirectory(badResourceContentPackage).Ok(),
        "declared image resources must match their signature and dimensions");

    const auto unlicensedFontPackage = root / L"resource-font-license";
    MakePackage(unlicensedFontPackage, "2.0.0",
        "35bb0546-cd57-428d-be84-74682600d08f", "", "", "main.lua",
        "", 2, 2, "", "",
        "\"body\": {\"type\": \"font\", \"path\": "
        "\"assets/body.ttf\"}");
    Write(unlicensedFontPackage / L"assets" / L"body.ttf",
        std::string("\0\1\0\0", 4));
    Expect(!validator.ValidateDirectory(unlicensedFontPackage).Ok(),
        "private font resources must declare their package license");

    const auto legacyResourcePackage = root / L"legacy-resource";
    MakePackage(legacyResourcePackage, "1.0.0",
        "68a2ddbb-44dd-461d-9619-7114636338de", "", "", "main.lua",
        "", 1, 1, "", "",
        "\"logo\": {\"type\": \"image\", \"path\": "
        "\"assets/logo.png\"}");
    Write(legacyResourcePackage / L"assets" / L"logo.png", pngHeader);
    Expect(!validator.ValidateDirectory(legacyResourcePackage).Ok(),
        "legacy API packages cannot declare v2 resource handles");

    const auto mismatchedContract = root / L"mismatched-contract";
    MakePackage(mismatchedContract, "2.0.0",
        "24dc465a-33e7-4189-bc1e-2cb9005de950", "", "", "main.lua",
        "", 2, 1);
    Expect(!validator.ValidateDirectory(mismatchedContract).Ok(),
        "schema and API versions must move to v2 together");

    const auto invalidFeature = root / L"invalid-feature";
    MakePackage(invalidFeature, "2.0.0",
        "f9312831-8944-41c3-a6fb-d3f6a0918fc2", "", "", "main.lua",
        "", 2, 2, "\"View.Tree\"");
    Expect(!validator.ValidateDirectory(invalidFeature).Ok(),
        "feature identifier segments must start with a lowercase letter");

    const auto badSource = root / L"bad";
    MakePackage(badSource, "1.0.0",
        "not-a-uuid");
    Write(badSource / L"escape.exe", "MZ");
    report = validator.ValidateDirectory(badSource);
    Expect(!report.Ok(), "invalid UUID and executable payload are rejected");
    const auto badNetwork = root / L"bad-network";
    MakePackage(badNetwork, "1.0.0",
        "cb0e23fb-346f-4495-8622-ecad61865167",
        "\"network.http\"", "\"*.example.com\"");
    Expect(!validator.ValidateDirectory(badNetwork).Ok(),
        "wildcard network domains are rejected");
    const auto v2Network = root / L"v2-network";
    MakePackage(v2Network, "1.0.0",
        "6c22cbe5-055b-42db-8198-915297034d5e",
        "\"network.internet\"", "\"feeds.example.net\"",
        "main.lua", "", 2, 2);
    Write(v2Network / L"main.lua",
        "return widget.define({render=function() end})\n");
    Expect(validator.ValidateDirectory(v2Network).Ok(),
        "v2 public network permission accepts explicit domains");
    const auto calendarPackage = root / L"calendar-package";
    MakePackage(calendarPackage, "1.0.0",
        "fd084e05-bb0f-43d7-977d-426ae39c1ab9",
        "\"calendar.read\", \"calendar.write\"");
    Expect(validator.ValidateDirectory(calendarPackage).Ok(),
        "calendar permissions are accepted");
    const auto splitSystemPackage = root / L"split-system-permissions";
    MakePackage(splitSystemPackage, "1.0.0",
        "aed3b5fb-c90d-49d0-9f05-f59a5cdab519",
        "\"system.performance.read\", \"system.power.read\", \"system.network.read\"");
    Expect(validator.ValidateDirectory(splitSystemPackage).Ok(),
        "fine-grained system snapshot permissions are accepted");
    const auto v2PermissionVocabularyPackage =
        root / L"v2-permission-vocabulary";
    MakePackage(v2PermissionVocabularyPackage, "1.0.0",
        "403e9f91-33dd-4c20-9b11-c476074e3a3a",
        "\"system.storage.read\", \"system.display.read\", "
        "\"audio.output.read\", \"audio.output.analyze\", "
        "\"audio.output.control\", \"app.discovery\", \"app.launch\", "
        "\"shell.launch\", \"network.internet\", \"network.local\", "
        "\"notification.post\", \"clipboard.read\", "
        "\"clipboard.write\", \"process.summary.read\", "
        "\"filesystem.userSelected.read\", "
        "\"filesystem.userSelected.write\", "
        "\"filesystem.userSelected.watch\"");
    Expect(validator.ValidateDirectory(v2PermissionVocabularyPackage).Ok(),
        "the complete M2 permission vocabulary is accepted by package validation");
    const auto wildcardSystemPackage = root / L"wildcard-system-permission";
    MakePackage(wildcardSystemPackage, "1.0.0",
        "68be1e3c-c07f-4ad6-a787-91de0d60725d",
        "\"system.read\"");
    Expect(!validator.ValidateDirectory(wildcardSystemPackage).Ok(),
        "the legacy system.read wildcard is rejected");
    const auto invalidScanPaths = TestPaths(root / L"invalid-package-scan");
    const std::string invalidScanId =
        "68be1e3c-c07f-4ad6-a787-91de0d60725d";
    MakePackage(invalidScanPaths.installed /
            std::filesystem::path(invalidScanId) / L"1.0.0", "1.0.0",
        invalidScanId, "\"system.read\"");
    MakePackage(invalidScanPaths.development / L"invalid-development",
        "1.0.0", invalidScanId, "\"system.performance.read\"");
    MakePackage(invalidScanPaths.development /
            L"invalid-development-only", "1.0.0",
        "df67af31-e68e-4c8c-a079-1d65451f93f0", "\"system.read\"");
    Write(invalidScanPaths.registry,
        "{\n  \"schemaVersion\": 1,\n  \"packages\": [\n"
        "    {\"packageId\":\"" + invalidScanId +
        "\",\"activeVersion\":\"1.0.0\","
        "\"providerId\":\"steam-workshop\","
        "\"externalItemId\":\"invalid-test\","
        "\"permissionState\":\"granted\",\"enabled\":true,"
        "\"grantedPermissions\":[\"system.read\"],"
        "\"grantedNetworkDomains\":[]}\n  ]\n}\n");
    WidgetPackageManager invalidScanManager(invalidScanPaths);
    std::string invalidScanError;
    Expect(invalidScanManager.Initialize(invalidScanError),
        "package manager initializes while invalid packages are present");
    const auto invalidScanned = invalidScanManager.ListInvalidPackages();
    const auto validScanned = invalidScanManager.ListPackages();
    Expect(invalidScanned.size() == 2 &&
            validScanned.size() == 1,
        "invalid packages remain visible while a valid development copy of the same component remains loadable");
    const auto invalidInstalled = std::find_if(invalidScanned.begin(),
        invalidScanned.end(), [](const auto& package)
        {
            return !package.builtin && !package.development;
        });
    Expect(invalidInstalled != invalidScanned.end() &&
            invalidInstalled->manifest.id == invalidScanId &&
            invalidInstalled->packageId == invalidScanId &&
            invalidInstalled->selected &&
            invalidInstalled->source.providerId == "steam-workshop" &&
            !invalidInstalled->report.Ok(),
        "an invalid active installed package retains its identity, source, and validation report");
    Expect(!validScanned.empty() && invalidInstalled != invalidScanned.end() &&
            validScanned.front().development &&
            validScanned.front().manifest.id == invalidInstalled->packageId,
        "valid and invalid source copies expose the same package identity for one management-list entry");
    const auto optionalPermissionPackage =
        root / L"optional-permission-package";
    MakePackage(optionalPermissionPackage, "1.0.0",
        "266ad7e7-cb68-4a17-ae2d-90c5bed9ccdb",
        "\"desktop.read\"", "", "main.lua",
        "\"calendar.read\", \"ui.input\"");
    PackageManifest optionalPermissionManifest;
    Expect(validator.ValidateDirectory(optionalPermissionPackage,
            &optionalPermissionManifest).Ok() &&
            optionalPermissionManifest.permissions ==
                std::vector<std::string>{ "desktop.read" } &&
            optionalPermissionManifest.optionalPermissions ==
                std::vector<std::string>({
                    "calendar.read", "ui.input" }),
        "required and optional permissions are parsed separately");
    const auto duplicateOptionalPackage =
        root / L"duplicate-optional-permission";
    MakePackage(duplicateOptionalPackage, "1.0.0",
        "d93d3424-452e-455c-bd86-ec7df5e99bbd",
        "\"desktop.read\"", "", "main.lua",
        "\"desktop.read\"");
    Expect(!validator.ValidateDirectory(duplicateOptionalPackage).Ok(),
        "a permission cannot be both required and optional");
    const auto unknownOptionalPackage =
        root / L"unknown-optional-permission";
    MakePackage(unknownOptionalPackage, "1.0.0",
        "d70b2447-8f2d-4e87-93ec-6a7f86fac70f",
        "\"ui.input\"", "", "main.lua",
        "\"future.unregistered\"");
    Expect(!validator.ValidateDirectory(unknownOptionalPackage).Ok(),
        "unknown optional permissions are rejected");

    std::string error;
    const auto nestedSource = root / L"nested-entry";
    MakePackage(nestedSource, "1.0.0",
        "8d5535c5-1a53-45bc-bdf3-9034c929ea23",
        "\"ui.input\"", "", "scripts/main.lua");
    PackageManifest nestedManifest;
    Expect(validator.ValidateDirectory(
            nestedSource, &nestedManifest).Ok(),
        "a package-relative nested Lua entry validates");
    const auto nestedManagerPaths =
        TestPaths(root / L"nested-manager");
    WidgetPackageManager nestedManager(nestedManagerPaths);
    Expect(nestedManager.Initialize(error),
        "nested-entry package manager initializes");
    InstalledPackage nestedInstalled;
    Expect(nestedManager.InstallDirectory(nestedSource,
            { "local", "nested-entry" }, false,
            nestedInstalled, report, error),
        "nested-entry package installs");
    Expect(nestedInstalled.permissionState ==
            PermissionDecisionState::Granted,
        "new package records an explicit granted permission state");
    const std::string explicitEmptyRegistry =
        "{\n  \"schemaVersion\": 1,\n  \"packages\": [\n"
        "    {\"packageId\":\"" + nestedManifest.id +
        "\",\"activeVersion\":\"" + nestedManifest.version +
        "\",\"providerId\":\"local\","
        "\"externalItemId\":\"nested-entry\","
        "\"permissionState\":\"granted\",\"enabled\":true,"
        "\"grantedPermissions\":[],"
        "\"grantedNetworkDomains\":[]}\n  ]\n}\n";
    Write(nestedManagerPaths.registry, explicitEmptyRegistry);
    WidgetPackageManager explicitEmptyManager(nestedManagerPaths);
    Expect(explicitEmptyManager.Initialize(error),
        "package manager reloads an explicit empty permission grant");
    const auto explicitEmpty =
        explicitEmptyManager.Resolve(nestedManifest.id);
    Expect(explicitEmpty &&
            explicitEmpty->permissionState ==
                PermissionDecisionState::Granted &&
            explicitEmpty->grantedPermissions.empty(),
        "explicit empty grants do not fall back to manifest permissions");

    const std::string legacyEmptyRegistry =
        "{\n  \"schemaVersion\": 1,\n  \"packages\": [\n"
        "    {\"packageId\":\"" + nestedManifest.id +
        "\",\"activeVersion\":\"" + nestedManifest.version +
        "\",\"providerId\":\"local\","
        "\"externalItemId\":\"nested-entry\",\"enabled\":true,"
        "\"grantedPermissions\":[],"
        "\"grantedNetworkDomains\":[]}\n  ]\n}\n";
    Write(nestedManagerPaths.registry, legacyEmptyRegistry);
    WidgetPackageManager legacyEmptyManager(nestedManagerPaths);
    Expect(legacyEmptyManager.Initialize(error),
        "package manager reloads a legacy permission record");
    const auto legacyEmpty = legacyEmptyManager.Resolve(nestedManifest.id);
    Expect(legacyEmpty &&
            legacyEmpty->permissionState ==
                PermissionDecisionState::LegacyImplicit &&
            legacyEmpty->grantedPermissions ==
                nestedManifest.permissions,
        "legacy empty grants retain compatibility until migration");
    const auto nestedEntry =
        nestedManager.ResolveEntry(nestedManifest.id);
    const auto nestedResolved = nestedEntry
        ? nestedManager.ResolveEntryPath(*nestedEntry)
        : std::nullopt;
    Expect(nestedResolved &&
        std::filesystem::equivalent(
            nestedResolved->root, nestedInstalled.root),
        "nested entry resolves back to the package root");

    const auto managerPaths = TestPaths(root / L"manager");
    std::filesystem::create_directories(managerPaths.builtin);
    std::filesystem::copy(sourceV1, managerPaths.builtin / L"package-test",
        std::filesystem::copy_options::recursive, ec);
    std::filesystem::create_directories(managerPaths.development);
    std::filesystem::copy(sourceV1,
        managerPaths.development / L"package-test",
        std::filesystem::copy_options::recursive, ec);
    WidgetPackageManager manager(managerPaths);
    Expect(manager.Initialize(error), "package manager initializes");
    Expect(manager.Resolve(manifest.id)->builtin &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::LegacyImplicit,
        "a discovered development package is inactive by default");
    Expect(manager.SetPermissionDecision(manifest.id,
            PermissionDecisionState::Granted, manifest.permissions,
            manifest.networkDomains, error) &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Granted,
        "an explicit built-in permission grant applies before activation");
    WidgetPackageManager builtInGrantReloaded(managerPaths);
    Expect(builtInGrantReloaded.Initialize(error) &&
            builtInGrantReloaded.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Granted,
        "a source-bound built-in grant survives a manager restart");
    const auto initialPackages = manager.ListPackages();
    const auto initialDevelopment = std::find_if(
        initialPackages.begin(), initialPackages.end(),
        [&](const auto& package)
        {
            return package.development && package.manifest.id == manifest.id;
        });
    Expect(initialDevelopment != initialPackages.end() &&
        !initialDevelopment->active,
        "inactive development candidates remain visible to management UI");
    Expect(manager.SetDevelopmentOverride(manifest.id, true, error) &&
            manager.Resolve(manifest.id)->development &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::LegacyImplicit,
        "development source activation does not inherit a built-in grant");
    Expect(manager.SetDevelopmentOverride(manifest.id, false, error) &&
            manager.Resolve(manifest.id)->builtin &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Granted,
        "deactivating development restores the source-bound built-in grant");
    InstalledPackage installed;
    report = {};
    Expect(manager.InstallDirectory(sourceV1, { "local", "package-test" },
        false, installed, report, error), "folder package installs");
    Expect(manager.Resolve(manifest.id).has_value() &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Pending &&
            manager.Resolve(manifest.id)->grantedPermissions.empty(),
        "new sensitive package installs without receiving runtime permission");
    Expect(manager.SetDevelopmentOverride(manifest.id, true, error) &&
        manager.Resolve(manifest.id)->development,
        "an explicitly activated development package overrides an install");
    const auto shadowedPackages = manager.ListPackages();
    const auto shadowedInstalled = std::find_if(
        shadowedPackages.begin(), shadowedPackages.end(),
        [&](const auto& package)
        {
            return package.manifest.id == manifest.id &&
                !package.builtin && !package.development && package.selected;
        });
    Expect(shadowedInstalled != shadowedPackages.end() &&
        !shadowedInstalled->active,
        "an active development override keeps the selected install manageable");
    WidgetPackageManager reloadedManager(managerPaths);
    Expect(reloadedManager.Initialize(error) &&
        reloadedManager.Resolve(manifest.id)->development,
        "explicit development activation persists across manager restarts");
    Expect(reloadedManager.SetDevelopmentOverride(
            manifest.id, false, error) &&
        !reloadedManager.Resolve(manifest.id)->development,
        "deactivating development restores the installed version");
    Expect(manager.SetDevelopmentOverride(manifest.id, false, error) &&
        !manager.Resolve(manifest.id)->development,
        "the primary manager continues with the installed source");
    Expect(manager.SetPermissionDecision(manifest.id,
            PermissionDecisionState::Granted, manifest.permissions,
            manifest.networkDomains, error) &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Granted,
        "an installed package becomes runnable only after explicit consent");
    WidgetPackageManager installedGrantReloaded(managerPaths);
    Expect(installedGrantReloaded.Initialize(error) &&
            installedGrantReloaded.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Granted,
        "an installed package grant survives a manager restart");

    const auto optionalManagerPaths =
        TestPaths(root / L"optional-permission-manager");
    WidgetPackageManager optionalManager(optionalManagerPaths);
    error.clear();
    Expect(optionalManager.Initialize(error),
        "optional-permission package manager initializes");
    InstalledPackage optionalInstalled;
    report = {};
    Expect(optionalManager.InstallDirectory(optionalPermissionPackage,
            { "local", "optional-permission-package" }, false,
            optionalInstalled, report, error) &&
            optionalInstalled.permissionState ==
                PermissionDecisionState::Pending,
        "a package with sensitive optional access starts pending consent");
    const std::vector<std::string> requiredOnlyGrant = {
        "desktop.read", "ui.input"
    };
    Expect(optionalManager.SetPermissionDecision(
            optionalPermissionManifest.id,
            PermissionDecisionState::Granted, requiredOnlyGrant, {},
            error),
        "required access can be granted while sensitive optional access is omitted");
    WidgetPackageManager optionalGrantReloaded(optionalManagerPaths);
    const auto optionalReloaded = optionalGrantReloaded.Initialize(error)
        ? optionalGrantReloaded.Resolve(optionalPermissionManifest.id)
        : std::nullopt;
    Expect(optionalReloaded &&
            optionalReloaded->permissionState ==
                PermissionDecisionState::Granted &&
            optionalReloaded->manifest.optionalPermissions ==
                optionalPermissionManifest.optionalPermissions &&
            optionalReloaded->grantedPermissions == requiredOnlyGrant,
        "required-only grants and optional declarations survive a manager restart");
    Expect(Read(optionalManagerPaths.registry).find(
            "\"requestedOptionalPermissions\"") != std::string::npos,
        "source-bound permission decisions persist optional scope metadata");

    const auto changedBuiltinPaths =
        TestPaths(root / L"changed-builtin-scope-manager");
    const auto changedBuiltinRoot =
        changedBuiltinPaths.builtin / L"changed-scope";
    const std::string changedBuiltinId =
        "65440c4d-d6e9-42f2-92e7-bacdd6390069";
    MakePackage(changedBuiltinRoot, "1.0.0", changedBuiltinId,
        "\"desktop.read\"");
    WidgetPackageManager changedBuiltinManager(changedBuiltinPaths);
    error.clear();
    Expect(changedBuiltinManager.Initialize(error) &&
            changedBuiltinManager.SetPermissionDecision(changedBuiltinId,
                PermissionDecisionState::Granted,
                { "desktop.read" }, {}, error),
        "a source-bound built-in decision is stored before a scope change");
    MakePackage(changedBuiltinRoot, "1.0.0", changedBuiltinId,
        "\"desktop.read\", \"calendar.read\"");
    WidgetPackageManager changedBuiltinReloaded(changedBuiltinPaths);
    const auto changedBuiltin = changedBuiltinReloaded.Initialize(error)
        ? changedBuiltinReloaded.Resolve(changedBuiltinId)
        : std::nullopt;
    Expect(changedBuiltin && changedBuiltin->permissionState ==
            PermissionDecisionState::Pending &&
            changedBuiltin->grantedPermissions.empty(),
        "an in-place built-in scope change cannot fall back to implicit permission");
    MakePackage(changedBuiltinRoot, "1.0.0", changedBuiltinId, "");
    WidgetPackageManager permissionFreeBuiltinReloaded(changedBuiltinPaths);
    const auto permissionFreeBuiltin =
        permissionFreeBuiltinReloaded.Initialize(error)
        ? permissionFreeBuiltinReloaded.Resolve(changedBuiltinId)
        : std::nullopt;
    Expect(permissionFreeBuiltin && permissionFreeBuiltin->permissionState ==
            PermissionDecisionState::Granted &&
            permissionFreeBuiltin->grantedPermissions.empty(),
        "removing every permission clears a stale built-in consent block");
    Expect(manager.ResolveEntry(manifest.id).value_or(L"").filename() == L"main.lua",
        "entry resolves inside the package");
    Expect(manager.SetEnabled(manifest.id, false, error),
        "installed package can be disabled");
    Expect(!manager.Resolve(manifest.id).has_value(),
        "disabled package does not silently fall back to a built-in source");
    Expect(manager.ContainsPackage(manifest.id),
        "a disabled package remains physically installed for recovery UI");
    error.clear();
    Expect(manager.SetPermissionDecision(manifest.id,
            PermissionDecisionState::Denied, {}, {}, error),
        "permissions remain manageable while an installed package is disabled");
    Expect(manager.UpdateSteamSubscriptionHistory(
            "111", { "100", "200" }, error),
        "Steam subscription history is persisted per account");
    const auto subscriptionHistory = manager.SteamSubscriptionHistory();
    Expect(subscriptionHistory.contains("111") &&
        subscriptionHistory.at("111") ==
            std::vector<std::string>({ "100", "200" }),
        "Steam subscription history is normalized for reconciliation");
    WidgetPackageManager historyReloadedManager(managerPaths);
    error.clear();
    Expect(historyReloadedManager.Initialize(error) &&
        historyReloadedManager.SteamSubscriptionHistory() ==
            subscriptionHistory,
        "Steam subscription history survives a manager restart");
    Expect(manager.SetEnabled(manifest.id, true, error) &&
            manager.Resolve(manifest.id)->permissionState ==
                PermissionDecisionState::Denied,
        "a disabled package can be re-enabled without losing its permission decision");

    const auto sourceV2 = root / L"source-v2";
    MakePackage(sourceV2, "1.1.0",
        "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
        "\"ui.input\", \"network.http\"", "\"feeds.example.net\"");
    error.clear();
    Expect(manager.InstallDirectory(sourceV2, { "other-provider", "remote-42" },
        false, installed, report, error) == false,
        "silent cross-provider update is rejected");
    error.clear();
    Expect(!manager.InstallDirectory(sourceV2, { "local", "package-test" },
        false, installed, report, error),
        "network origin expansion requires confirmation");
    error.clear();
    Expect(manager.InstallDirectory(sourceV2, { "local", "package-test" },
        false, installed, report, error, true),
        "confirmed network origin expansion installs pending consent");
    Expect(manager.Resolve(manifest.id)->manifest.version == "1.1.0",
        "new version becomes active");
    Expect(manager.Resolve(manifest.id)->permissionState ==
            PermissionDecisionState::Pending &&
            manager.Resolve(manifest.id)->grantedPermissions.empty(),
        "a changed permission scope invalidates the previous grant");

    const auto sourceV3 = root / L"source-v3";
    MakePackage(sourceV3, "1.2.0",
        "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
        "\"ui.input\", \"ui.notify\"");
    LocalDirectorySource localSource(root);
    PackageQuery page;
    page.offset = 1;
    page.limit = 1;
    Expect(localSource.Query(page, error).size() == 1,
        "local source applies pagination to matched packages");
    error.clear();
    Expect(!manager.InstallDirectory(sourceV3, { "local", "package-test" },
        false, installed, report, error),
        "permission expansion requires confirmation");
    error.clear();
    Expect(manager.InstallDirectory(sourceV3, { "local", "package-test" },
        false, installed, report, error, true),
        "confirmed permission expansion installs");
    Expect(installed.permissionState == PermissionDecisionState::Pending &&
            installed.grantedPermissions.empty(),
        "install confirmation does not substitute for runtime consent");
    error.clear();
    Expect(manager.Rollback(manifest.id, "1.0.0", error),
        "known-good version can be restored");
    Expect(manager.Resolve(manifest.id)->manifest.version == "1.0.0",
        "rollback updates active version");

    const auto archive = root / L"exports" / L"package-test.snowwidget";
    PackageArtifact artifact;
    error.clear();
    Expect(manager.ExportArchive(manifest.id, archive, artifact, report, error),
        "folder package exports to .snowwidget");
    Expect(!artifact.sha256.empty(), "export records SHA-256");
    PackageManifest archiveManifest;
    Expect(manager.ValidateArchive(archive, &archiveManifest).Ok(),
        "archive validation securely extracts and validates the package");
    Expect(archiveManifest.id == manifest.id &&
        archiveManifest.version == "1.0.0",
        "full archive validation returns the package identity and version");
    const auto corruptArchive = root / L"exports" / L"corrupt.snowwidget";
    std::filesystem::copy_file(archive, corruptArchive,
        std::filesystem::copy_options::overwrite_existing, ec);
    {
        std::fstream corrupt(corruptArchive,
            std::ios::binary | std::ios::in | std::ios::out);
        std::vector<char> bytes((std::istreambuf_iterator<char>(corrupt)),
            std::istreambuf_iterator<char>());
        const std::string needle = "function render";
        const auto found = std::search(bytes.begin(), bytes.end(),
            needle.begin(), needle.end());
        if (found != bytes.end())
        {
            const auto position = std::distance(bytes.begin(), found);
            bytes[static_cast<std::size_t>(position)] ^= 0x01;
            corrupt.clear();
            corrupt.seekp(0);
            corrupt.write(bytes.data(),
                static_cast<std::streamsize>(bytes.size()));
        }
    }
    Expect(!manager.ValidateArchive(corruptArchive).Ok(),
        "archive CRC corruption is rejected");
    const auto traversalArchive =
        root / L"exports" / L"traversal.snowwidget";
    MakeUnsafeArchive(traversalArchive, { "../escape.lua" });
    Expect(!manager.ValidateArchive(traversalArchive).Ok(),
        "ZIP path traversal is rejected before extraction");
    const auto collisionArchive =
        root / L"exports" / L"case-collision.snowwidget";
    MakeUnsafeArchive(collisionArchive, { "Assets/icon.png", "assets/icon.png" });
    Expect(!manager.ValidateArchive(collisionArchive).Ok(),
        "case-insensitive ZIP path collisions are rejected");

    WidgetPackageManager importedManager(TestPaths(root / L"imported"));
    error.clear();
    Expect(importedManager.Initialize(error), "second manager initializes");
    InstalledPackage imported;
    Expect(importedManager.InstallArchive(archive,
        { "static-catalog", "remote-42" }, false, imported, report, error),
        "exported archive installs through staging");
    Expect(imported.manifest.id == manifest.id, "archive identity is preserved");

    const auto developmentCopyPaths = TestPaths(root / L"development-copy");
    WidgetPackageManager developmentCopyManager(developmentCopyPaths);
    error.clear();
    Expect(developmentCopyManager.Initialize(error),
        "development-copy manager initializes");
    InstalledPackage workshopInstalled;
    Expect(developmentCopyManager.InstallDirectory(sourceV1,
            { "steam-workshop", "5080330:123456789" }, false,
            workshopInstalled, report, error),
        "Workshop component installs before creating a development version");
    std::filesystem::path developmentProject;
    error.clear();
    Expect(developmentCopyManager.CreateDevelopmentProject(
            manifest.id, developmentProject, error),
        "an installed Workshop component creates a development project");
    Expect(developmentProject.parent_path() ==
            developmentCopyPaths.development &&
            std::filesystem::is_regular_file(
                developmentProject / L"widget.json") &&
            std::filesystem::is_regular_file(
                developmentProject / L"main.lua"),
        "the development project is a complete editable package directory");
    const auto developmentPackages = developmentCopyManager.ListPackages();
    const auto copiedDevelopment = std::find_if(
        developmentPackages.begin(), developmentPackages.end(),
        [&](const auto& package)
        {
            return package.development && package.manifest.id == manifest.id;
        });
    Expect(copiedDevelopment != developmentPackages.end() &&
            !copiedDevelopment->active &&
            developmentCopyManager.Resolve(manifest.id) &&
            !developmentCopyManager.Resolve(manifest.id)->development,
        "creating a development project keeps the installed version active");
    std::filesystem::path duplicateDevelopmentProject;
    error.clear();
    Expect(!developmentCopyManager.CreateDevelopmentProject(
            manifest.id, duplicateDevelopmentProject, error) &&
            duplicateDevelopmentProject.empty(),
        "a second development project cannot silently replace the first");

    WidgetPackageManager sourceTrustManager(
        TestPaths(root / L"source-trust"));
    error.clear();
    Expect(sourceTrustManager.Initialize(error),
        "source trust test manager initializes");
    PackageDetails spoofedIdentity{
        manifest,
        { "declared-artifact", "spoofed-identity" },
        { manifest.version },
        false };
    spoofedIdentity.manifest.id =
        "5213e963-a643-4e50-bc7a-82f761e0e29f";
    DeclaredArtifactSource identitySource(spoofedIdentity, archive);
    InstalledPackage rejectedInstall;
    error.clear();
    Expect(!sourceTrustManager.InstallFromSource(identitySource,
            spoofedIdentity.source.externalItemId,
            spoofedIdentity.manifest.version, false,
            rejectedInstall, report, error) &&
        !sourceTrustManager.Resolve(
            spoofedIdentity.manifest.id).has_value(),
        "source artifacts cannot spoof the catalog package identity");

    PackageDetails understatedPermissions{
        manifest,
        { "declared-artifact", "understated-permissions" },
        { manifest.version },
        false };
    understatedPermissions.manifest.permissions.clear();
    DeclaredArtifactSource permissionSource(
        understatedPermissions, archive);
    error.clear();
    Expect(!sourceTrustManager.InstallFromSource(permissionSource,
            understatedPermissions.source.externalItemId,
            understatedPermissions.manifest.version, false,
            rejectedInstall, report, error) &&
        !sourceTrustManager.Resolve(manifest.id).has_value(),
        "source artifacts cannot request undeclared permissions");

    LocalCatalogPublisher publisher(root / L"catalog");
    PublishRequest request;
    request.artifact = artifact;
    request.title = "Package Test";
    request.description = "Published locally";
    int progressCalls = 0;
    request.progress = [&](std::uint64_t, std::uint64_t)
    {
        ++progressCalls;
        return true;
    };
    const auto publishResult = publisher.Publish(request);
    Expect(publishResult.ok, "local catalog publisher creates an index");
    Expect(progressCalls >= 2, "local publisher reports upload progress");

    error.clear();
    Expect(manager.Rollback(manifest.id, "1.2.0", error),
        "newer version can be selected for a second publication");
    const auto archiveV2 = root / L"exports" / L"package-test-v2.snowwidget";
    PackageArtifact artifactV2;
    Expect(manager.ExportArchive(manifest.id, archiveV2, artifactV2,
        report, error), "second package version exports");
    request.artifact = artifactV2;
    request.externalItemId = publishResult.externalItemId;
    request.changeNotes = "Second test version";
    const auto publishResultV2 = publisher.Publish(request);
    Expect(publishResultV2.ok, "local catalog keeps multiple versions");

    StaticCatalogSource catalog(root / L"catalog" / L"catalog.json");
    Expect(catalog.Status().available, "static catalog reports source status");
    auto entries = catalog.Query({}, error);
    Expect(entries.size() == 1 && entries.front().versions.size() == 2,
        "static catalog groups multiple versions into one item");
    Expect(entries.front().manifest.version == "1.2.0",
        "static catalog selects the newest SemVer");
    const auto updates = catalog.CheckUpdates(
        { { manifest.id, "1.0.0" } }, error);
    Expect(updates.size() == 1 &&
        updates.front().available.manifest.version == "1.2.0",
        "static catalog checks installed versions for updates");
    const auto materialized = root / L"catalog-copy.snowwidget";
    Expect(catalog.Materialize(publishResult.externalItemId,
        artifact.version, materialized, error).has_value(),
        "static catalog materializes the requested older artifact");
    const auto materializedV2 = root / L"catalog-copy-v2.snowwidget";
    Expect(catalog.Materialize(publishResult.externalItemId,
        artifactV2.version, materializedV2, error).has_value(),
        "static catalog materializes the requested newer artifact");
    WidgetPackageManager catalogInstallManager(
        TestPaths(root / L"catalog-installed"));
    Expect(catalogInstallManager.Initialize(error),
        "catalog install manager initializes");
    InstalledPackage catalogInstalled;
    Expect(catalogInstallManager.InstallFromSource(catalog,
        publishResult.externalItemId, artifactV2.version, false,
        catalogInstalled, report, error),
        "package manager installs through the source contract");
    Expect(catalogInstalled.manifest.version == "1.2.0",
        "source installation activates the requested version");

    const auto portableWidgets = root / L"portable-widget-import" / L"widgets";
    Write(portableWidgets / L"my_legacy.lua",
        "function render() end\n");
    Write(portableWidgets / L"my_legacy.widget.json",
        "{ \"name\": \"My Legacy Widget\", \"version\": \"1.0.0\" }\n");
    Write(portableWidgets / L"orphan.lua",
        "function render() end\n");
    MakePackage(portableWidgets / L"folder-package", "1.0.0");
    Write(portableWidgets / L"snowdesktop-lua-widget" / L"SKILL.md",
        "# Authoring tool\n");
    Write(portableWidgets / L"README.txt", "not component data\n");
    const auto importedPortableWidgets =
        root / L"portable-widget-import" / L"staging-data" / L"widgets";
    const auto portableImport = ImportLegacyLooseWidgetPairs(
        portableWidgets, importedPortableWidgets);
    Expect(portableImport.ok && portableImport.copiedPairs == 1,
        "portable migration imports only complete legacy loose pairs");
    Expect(std::filesystem::is_regular_file(
        importedPortableWidgets / L"my_legacy.lua") &&
        std::filesystem::is_regular_file(
            importedPortableWidgets / L"my_legacy.widget.json"),
        "portable migration preserves the user-authored legacy pair");
    Expect(!std::filesystem::exists(
        importedPortableWidgets / L"orphan.lua"),
        "portable migration ignores orphaned Lua files");
    Expect(!std::filesystem::exists(
        importedPortableWidgets / L"folder-package") &&
        !std::filesystem::exists(
            importedPortableWidgets / L"snowdesktop-lua-widget") &&
        !std::filesystem::exists(
            importedPortableWidgets / L"README.txt"),
        "portable migration does not copy folder packages or authoring files");
    const auto missingPortableImport = ImportLegacyLooseWidgetPairs(
        root / L"portable-widget-import" / L"missing",
        importedPortableWidgets);
    Expect(missingPortableImport.ok &&
        missingPortableImport.copiedPairs == 0,
        "portable migration accepts a missing legacy widgets directory");

    const auto longPathSource =
        root / L"portable-long-path-source";
    std::filesystem::path longRelative;
    for (int i = 0; i < 5; ++i)
    {
        longRelative /=
            std::wstring(24, static_cast<wchar_t>(L'a' + i));
    }
    Write(longPathSource / longRelative /
            L"SnowDesktop.layout.json",
        "{ \"source\": \"portable-long-path\" }\n");
    const auto longPathDestination =
        root / L"portable-long-path-destination" /
        std::wstring(72, L'd');
    const auto longCopy = snowdesktop::migration::CopyDataTree(
        longPathSource, longPathDestination);
    const auto longCopiedFile = std::filesystem::absolute(
        longPathDestination / longRelative /
            L"SnowDesktop.layout.json");
    const std::wstring extendedLongCopiedFile =
        std::wstring(LR"(\\?\)") + longCopiedFile.wstring();
    Expect(longCopiedFile.wstring().size() > MAX_PATH,
        "portable migration long-path test exceeds legacy MAX_PATH");
    Expect(longCopy.ok && longCopy.files == 1,
        "portable migration copies data through extended-length paths");
    Expect(GetFileAttributesW(extendedLongCopiedFile.c_str()) !=
            INVALID_FILE_ATTRIBUTES,
        "portable migration creates the long destination file");

    const auto portableState =
        root / L"restart-safe-portable-migration";
    Write(portableState / L"data" / L"SnowDesktop.layout.json",
        "{ \"source\": \"installed\" }\n");
    const std::wstring portableToken = L"20260730-120000-42-100";
    const auto portableStage = portableState / L"TempState" /
        L"PortableMigration" / (L"staging-" + portableToken);
    Write(portableStage / L"SnowDesktop.layout.json",
        "{ \"source\": \"portable\" }\n");
    Write(portableStage / L"widgets" / L"packages.json",
        "{ \"schemaVersion\": 1, \"packages\": [] }\n");
    error.clear();
    Expect(snowdesktop::migration::Queue(
        portableState, portableToken, error),
        "portable data replacement is queued for the next startup");
    const auto pendingApply =
        snowdesktop::migration::ApplyPending(portableState);
    Expect(pendingApply.ok && pendingApply.pending &&
        pendingApply.applied,
        "queued portable data is applied before runtime initialization");
    Expect(std::filesystem::is_regular_file(
        portableState / L"data" / L"widgets" / L"packages.json"),
        "portable data becomes the active data directory");
    Expect(std::filesystem::is_regular_file(
        pendingApply.backup / L"SnowDesktop.layout.json"),
        "the previous installed data is retained as a complete backup");
    Expect(!std::filesystem::exists(
        portableState / L"TempState" / L"PortableMigration" /
            L"pending.txt"),
        "the pending marker is retired after a successful exchange");
    const auto noPendingApply =
        snowdesktop::migration::ApplyPending(portableState);
    Expect(noPendingApply.ok && !noPendingApply.pending &&
        !noPendingApply.applied,
        "completed portable migration is not repeated");

    const auto invalidPortableState =
        root / L"invalid-restart-safe-portable-migration";
    const std::wstring invalidToken = L"20260730-120100-42-200";
    std::filesystem::create_directories(invalidPortableState / L"data");
    std::filesystem::create_directories(invalidPortableState /
        L"TempState" / L"PortableMigration" /
        (L"staging-" + invalidToken));
    error.clear();
    Expect(!snowdesktop::migration::Queue(
        invalidPortableState, invalidToken, error),
        "invalid staged data is rejected before active data is touched");
    Expect(std::filesystem::is_directory(
        invalidPortableState / L"data"),
        "failed queue validation preserves current installed data");

    const auto fullBackupState =
        root / L"complete-data-backup";
    const auto fullBackupData =
        fullBackupState / L"data";
    const std::string originalLayout =
        "{ \"source\": \"complete-backup-original\" }\n";
    const std::string modifiedLayout =
        "{ \"source\": \"complete-backup-modified\" }\n";
    const std::string originalCalendar =
        "{ \"schemaVersion\": 1, \"events\": [] }\n";
    const std::string originalNotificationSchedules =
        "{ \"schemaVersion\": 1, \"entries\": [] }\n";
    Write(fullBackupData / L"SnowDesktop.layout.json",
        originalLayout);
    Write(fullBackupData / L"SnowDesktop.general.json",
        "{ \"language\": \"zh-CN\" }\n");
    Write(fullBackupData / L"SnowDesktop.calendar.json",
        originalCalendar);
    Write(fullBackupData / L"SnowDesktop.widget-notifications.json",
        originalNotificationSchedules);
    Write(fullBackupData / L"widgets" / L"installed" /
        L"package-id" / L"1.0.0" / L"main.lua",
        "function render() end\n");
    Write(fullBackupData / L"widgets" / L"storage" /
        L"package-id" / L"instance-id.json",
        "{ \"counter\": 27 }\n");
    Write(fullBackupState / L"PrivateState" /
        L"SnowDesktop.widget-secrets.bin",
        "encrypted-private-state-must-not-be-exported");
    Write(fullBackupData / L"SnowDesktop_crash.log",
        "excluded log\n");
    Write(fullBackupData / L"SnowDesktop.log",
        "excluded diagnostic log\n");
    Write(fullBackupData / L"SnowDesktop.log.1",
        "excluded rotated diagnostic log\n");
    Write(fullBackupData / L"crashdumps" / L"test.dmp",
        "excluded dump\n");
    Write(fullBackupData / L"widgets" / L"staging" /
        L"temporary.txt", "excluded staging\n");
    Write(fullBackupData / L"widgets" / L"quarantine" /
        L"bad.txt", "excluded quarantine\n");
    std::filesystem::path longBackupRelative;
    for (int index = 0; index < 4; ++index)
    {
        longBackupRelative /=
            std::wstring(32, static_cast<wchar_t>(L'k' + index));
    }
    longBackupRelative /= L"long-state.json";
    Write(fullBackupData / longBackupRelative,
        "{ \"longPath\": true }\n");

    snowdesktop::backup::FullDataBackupManager fullBackupManager(
        fullBackupState, fullBackupData, "1.0.1.0", "portable");
    const auto createdFullBackup = fullBackupManager.Create();
    Expect(createdFullBackup.ok &&
        std::filesystem::is_regular_file(
            createdFullBackup.backup.root / L"backup.json"),
        "complete data backup is created with a manifest");
    Expect(std::filesystem::is_regular_file(
            createdFullBackup.backup.data /
                L"SnowDesktop.layout.json") &&
        std::filesystem::is_regular_file(
            createdFullBackup.backup.data / L"widgets" / L"storage" /
                L"package-id" / L"instance-id.json") &&
        Read(createdFullBackup.backup.data /
            L"SnowDesktop.calendar.json") == originalCalendar &&
        Read(createdFullBackup.backup.data /
            L"SnowDesktop.widget-notifications.json") ==
                originalNotificationSchedules,
        "complete backup preserves layout, settings, calendar, widget notifications, packages, and storage");
    Expect(!std::filesystem::exists(
            createdFullBackup.backup.data /
                L"SnowDesktop_crash.log") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"SnowDesktop.log") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"SnowDesktop.log.1") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"crashdumps") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"widgets" / L"staging") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"widgets" / L"quarantine"),
        "complete backup excludes logs, dumps, staging, and quarantine");
    Expect(!std::filesystem::exists(
            createdFullBackup.backup.root / L"PrivateState") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data /
                L"SnowDesktop.widget-secrets.bin"),
        "complete backup excludes the sibling widget secret store");
    const auto longBackupFile = std::filesystem::absolute(
        createdFullBackup.backup.data / longBackupRelative);
    const std::wstring extendedLongBackupFile =
        longBackupFile.wstring().starts_with(LR"(\\?\)")
            ? longBackupFile.wstring()
            : std::wstring(LR"(\\?\)") +
                longBackupFile.wstring();
    Expect(longBackupFile.wstring().size() > MAX_PATH &&
        GetFileAttributesW(extendedLongBackupFile.c_str()) !=
            INVALID_FILE_ATTRIBUTES,
        "complete backup supports destination paths beyond MAX_PATH");
    const auto completeBackupList = fullBackupManager.List();
    Expect(!completeBackupList.empty() &&
        !completeBackupList.front().migrationRollback &&
        completeBackupList.front().fileCount >= 4,
        "complete backup appears in the managed backup list");

    const auto exportedBackup =
        root / L"exports" / L"complete.snowbackup";
    const auto exportResult = fullBackupManager.Export(
        createdFullBackup.backup, exportedBackup);
    Expect(exportResult.ok &&
        std::filesystem::is_regular_file(exportedBackup),
        "complete backup exports as a standard snowbackup archive");

    Write(fullBackupData / L"SnowDesktop.layout.json",
        modifiedLayout);
    Write(fullBackupData / L"SnowDesktop.calendar.json",
        "{ \"schemaVersion\": 1, \"events\": [1] }\n");
    Write(fullBackupData / L"SnowDesktop.widget-notifications.json",
        "{ \"schemaVersion\": 1, \"entries\": [1] }\n");
    const auto queuedRestore =
        fullBackupManager.QueueRestore(createdFullBackup.backup);
    Expect(queuedRestore.ok,
        "complete backup restore is queued without touching active data");
    Expect(Read(fullBackupData / L"SnowDesktop.layout.json") ==
            modifiedLayout,
        "queued complete backup restore leaves active data unchanged");
    const auto appliedRestore =
        snowdesktop::migration::ApplyPending(fullBackupState);
    Expect(appliedRestore.ok && appliedRestore.applied &&
        Read(fullBackupData / L"SnowDesktop.layout.json") ==
            originalLayout &&
        Read(fullBackupData / L"SnowDesktop.calendar.json") ==
            originalCalendar &&
        Read(fullBackupData / L"SnowDesktop.widget-notifications.json") ==
            originalNotificationSchedules,
        "complete backup atomically restores layout, calendar, and widget notification schedules on the next startup");
    Expect(Read(appliedRestore.backup /
            L"SnowDesktop.layout.json") == modifiedLayout,
        "pre-restore active data is retained as a rollback backup");

    const auto backupsAfterRestore = fullBackupManager.List();
    const auto rollbackBackup = std::find_if(
        backupsAfterRestore.begin(), backupsAfterRestore.end(),
        [](const snowdesktop::backup::BackupInfo& backup) {
            return backup.migrationRollback;
        });
    Expect(rollbackBackup != backupsAfterRestore.end(),
        "pre-restore data is visible in the complete backup list");
    if (rollbackBackup != backupsAfterRestore.end())
    {
        const auto queuedRollback =
            fullBackupManager.QueueRestore(*rollbackBackup);
        const auto appliedRollback =
            snowdesktop::migration::ApplyPending(fullBackupState);
        Expect(queuedRollback.ok && appliedRollback.ok &&
            appliedRollback.applied &&
            Read(fullBackupData / L"SnowDesktop.layout.json") ==
                modifiedLayout,
            "a pre-migration backup can restore the previous data");
    }

    const auto importedBackupState =
        root / L"imported-complete-data-backup";
    const auto importedBackupData =
        importedBackupState / L"data";
    Write(importedBackupData / L"SnowDesktop.layout.json",
        "{ \"source\": \"before-archive-import\" }\n");
    snowdesktop::backup::FullDataBackupManager importBackupManager(
        importedBackupState, importedBackupData,
        "1.0.1.0", "installed");
    const auto importResult =
        importBackupManager.ImportAndQueue(exportedBackup);
    if (!importResult.ok)
    {
        std::cerr << "complete backup import error: "
            << importResult.error << '\n';
    }
    Expect(importResult.ok,
        "a snowbackup archive is verified and queued for restore");
    const auto appliedImport =
        snowdesktop::migration::ApplyPending(importedBackupState);
    Expect(appliedImport.ok && appliedImport.applied &&
        Read(importedBackupData / L"SnowDesktop.layout.json") ==
            originalLayout,
        "a snowbackup archive restores complete data after restart");

    const auto corruptedBackup =
        root / L"exports" / L"corrupted.snowbackup";
    std::filesystem::copy_file(exportedBackup, corruptedBackup,
        std::filesystem::copy_options::overwrite_existing, ec);
    Expect(CorruptArchivePayload(
            corruptedBackup, originalLayout),
        "complete backup corruption test modifies an archive payload");
    const auto corruptedImport =
        importBackupManager.ImportAndQueue(corruptedBackup);
    Expect(!corruptedImport.ok &&
        Read(importedBackupData / L"SnowDesktop.layout.json") ==
            originalLayout,
        "corrupted backup is rejected without replacing active data");

    const auto unsafeBackup =
        root / L"exports" / L"unsafe.snowbackup";
    MakeUnsafeArchive(unsafeBackup, { "../escape.txt" });
    const auto unsafeImport =
        importBackupManager.ImportAndQueue(unsafeBackup);
    Expect(!unsafeImport.ok &&
        !std::filesystem::exists(
            importedBackupState / L"escape.txt"),
        "backup archive path traversal is rejected");

    const auto deletedBackup =
        fullBackupManager.Delete(createdFullBackup.backup);
    Expect(deletedBackup.ok &&
        !std::filesystem::exists(createdFullBackup.backup.root),
        "complete backup can be deleted from managed storage");

    const auto automaticPaths = TestPaths(root / L"automatic-migration");
    constexpr const char* analogPackageId =
        "64107f41-197a-426a-8f86-6eeb020f56b0";
    MakePackage(automaticPaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    Write(automaticPaths.builtin / L"analog_clock.lua",
        "-- deliberately different from the replacement\n"
        "function render() error('old shipped component') end\n");
    Write(automaticPaths.builtin / L"analog_clock.widget.json",
        "{\n"
        "  \"name\": \"Analog Clock\",\n"
        "  \"nameKey\": \"lua_widget.analog_clock.name\",\n"
        "  \"version\": \"0.9.0\",\n"
        "  \"permissions\": [\"ui.input\"]\n"
        "}\n");
    const auto customLegacyRoot = automaticPaths.installed.parent_path();
    Write(customLegacyRoot / L"my_widget.lua",
        "function render() end\n");
    Write(customLegacyRoot / L"my_widget.widget.json",
        "{\n"
        "  \"name\": \"My Widget\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"permissions\": []\n"
        "}\n");
    const auto legacyStorage =
        automaticPaths.registry.parent_path().parent_path() /
            L"SnowDesktop.storage.json";
    const std::string legacyStorageText =
        "{\n  \"widget-instance.text\": \"keep me\"\n}\n";
    Write(legacyStorage, legacyStorageText);

    WidgetPackageManager automaticManager(automaticPaths);
    error.clear();
    Expect(automaticManager.Initialize(error),
        "manager initializes while replacing shipped loose components");
    const auto& automaticMigrations =
        automaticManager.AutomaticLegacyMigrationResults();
    Expect(automaticMigrations.size() == 1 &&
        automaticMigrations.front().ok,
        "shipped loose component is replaced without user interaction");
    Expect(!std::filesystem::exists(
        automaticPaths.builtin / L"analog_clock.lua") &&
        !std::filesystem::exists(
            automaticPaths.builtin / L"analog_clock.widget.json"),
        "replaced shipped loose files are deleted");
    Expect(automaticManager.ResolveLegacyPackageId(
        L"analog_clock.lua").value_or("") == analogPackageId,
        "legacy layout name resolves to the immutable built-in package id");
    Expect(automaticMigrations.front().backupDirectory.empty(),
        "shipped component files do not create a permanent migration backup");
    Expect(std::filesystem::is_empty(automaticPaths.migrations),
        "migrations directory remains reserved for user-authored components");
    const auto pendingStorage =
        automaticManager.PendingLegacyStoragePath();
    std::ifstream pendingStorageFile(pendingStorage, std::ios::binary);
    const std::string pendingStorageText(
        (std::istreambuf_iterator<char>(pendingStorageFile)),
        std::istreambuf_iterator<char>());
    Expect(pendingStorageText == legacyStorageText,
        "legacy instance storage is transactionally staged for engine import");

    const auto userLegacy = automaticManager.FindLegacyPackages();
    Expect(userLegacy.size() == 1 &&
        userLegacy.front().legacyName == L"my_widget.lua",
        "only user-authored loose components are offered in the migration UI");
    Expect(std::filesystem::exists(customLegacyRoot / L"my_widget.lua"),
        "user-authored loose component is not changed automatically");
    const auto userMigration =
        automaticManager.MigrateLegacy(userLegacy.front());
    Expect(userMigration.ok,
        "user-authored loose component migrates after explicit action");
    Expect(!userMigration.backupDirectory.empty() &&
        std::filesystem::exists(userMigration.backupDirectory),
        "explicit user migration retains its recovery backup");
    Expect(!std::filesystem::exists(customLegacyRoot / L"my_widget.lua"),
        "explicit user migration removes the loose source after backup");

    // MSIX replaces the read-only application directory as one unit. The old
    // official loose files therefore no longer exist when the upgraded
    // process first reads a legacy layout.
    const auto packagedUpgradePaths =
        TestPaths(root / L"packaged-folder-only-upgrade");
    MakePackage(packagedUpgradePaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    WidgetPackageManager packagedUpgradeManager(packagedUpgradePaths);
    error.clear();
    Expect(packagedUpgradeManager.Initialize(error),
        "manager initializes for an MSIX folder-only upgrade");
    Expect(packagedUpgradeManager.AutomaticLegacyMigrationResults().empty(),
        "folder-only MSIX upgrade does not require retired install files");
    Expect(packagedUpgradeManager.ResolveLegacyPackageId(
        L"analog_clock.lua").value_or("") == analogPackageId,
        "MSIX legacy layout maps to the built-in folder package without loose files");

    // A user may have created a component that happens to use an old official
    // filename. Its writable loose pair must remain eligible for the migration
    // wizard instead of being silently rebound to SnowDesktop's package.
    const auto packagedCollisionPaths =
        TestPaths(root / L"packaged-custom-name-collision");
    MakePackage(packagedCollisionPaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    const auto collisionRoot =
        packagedCollisionPaths.installed.parent_path();
    Write(collisionRoot / L"analog_clock.lua",
        "function render() end\n");
    Write(collisionRoot / L"analog_clock.widget.json",
        "{\n"
        "  \"name\": \"My Analog Clock\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"permissions\": []\n"
        "}\n");
    WidgetPackageManager packagedCollisionManager(packagedCollisionPaths);
    error.clear();
    Expect(packagedCollisionManager.Initialize(error),
        "manager initializes with a custom legacy filename collision");
    Expect(!packagedCollisionManager.ResolveLegacyPackageId(
        L"analog_clock.lua").has_value(),
        "custom loose component is not mistaken for an MSIX built-in");
    const auto packagedCollisionLegacy =
        packagedCollisionManager.FindLegacyPackages();
    Expect(packagedCollisionLegacy.size() == 1 &&
        packagedCollisionLegacy.front().legacyName == L"analog_clock.lua",
        "custom filename collision remains visible to the migration wizard");

    std::filesystem::remove_all(root, ec);
    if (failures)
        std::cerr << failures
            << " application data lifecycle test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
