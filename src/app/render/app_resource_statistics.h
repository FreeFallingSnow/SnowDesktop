#pragma once

#include "diagnostics/performance_trace.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace snowdesktop::app_resource_statistics
{
// These are selected owned payloads, not heap allocation sizes. In particular,
// inline string contents can overlap the separately reported vector storage.
struct Payload
{
    std::size_t entries = 0, vectorStorage = 0, nestedVectorStorage = 0;
    std::size_t textBytes = 0, textFields = 0;
    template<class Char, class Traits, class Allocator>
    void Text(const std::basic_string<Char, Traits, Allocator>& text)
    { textBytes += text.size() * sizeof(Char); ++textFields; }
    template<class Vector> void VectorStorage(const Vector& values, bool nested = false)
    {
        const auto bytes = values.capacity() * sizeof(typename Vector::value_type);
        if (nested) nestedVectorStorage += bytes;
        else { entries += values.size(); vectorStorage += bytes; }
    }
    template<class Vector> void Strings(const Vector& values)
    { VectorStorage(values, true); for (const auto& value : values) Text(value); }
    void Record(std::string_view group) const
    {
        if (!performance::Enabled()) return;
        const std::string prefix(group);
        const auto emit = [&](const char* suffix, std::size_t amount) {
            performance::Value("host.model.memory", prefix + suffix, {}, static_cast<double>(amount));
        };
        emit("_entries", entries);
        emit("_vector_storage_bytes", vectorStorage);
        emit("_nested_vector_storage_bytes", nestedVectorStorage);
        emit("_selected_text_payload_bytes", textBytes);
        emit("_selected_text_fields", textFields);
    }
};

inline void Record(const auto& items, const auto& widgets, const auto& dockPopup,
    const auto& pages, const auto& dock, const auto& apps, const auto& results,
    const auto& resultIndices, const auto& layout, const auto& itemIndex)
{
    if (!performance::Enabled()) return;
    performance::Scope scope("profiler", "sample.host_models");
    Payload desktop; desktop.VectorStorage(items);
    for (const auto& item : items)
    {
        desktop.Text(item.name); desktop.Text(item.parsingName); desktop.Text(item.layoutKey);
        desktop.Text(item.desktopIconClsid); desktop.Text(item.typeName); desktop.Text(item.gridCell.pageId);
    }
    desktop.Record("desktop");
    Payload widget, folder;
    widget.VectorStorage(widgets);
    const auto visitWidget = [&](const auto& value) {
        widget.Text(value.id); widget.Text(value.title); widget.Text(value.customTitle); widget.Text(value.scriptTitle);
        widget.Text(value.demoIconCategory); widget.Text(value.dockIconAssetId); widget.Text(value.sourceFolderPath);
        widget.Text(value.sourceDesktopItemKey); widget.Text(value.gridCell.pageId); widget.Text(value.activeCategoryId);
        widget.Text(value.packageId); widget.Text(value.packageSourceProvider);
        widget.Text(value.packageSourceExternalItemId); widget.Text(value.packageSourceUrl);
        widget.Strings(value.autoCollectCategoryIds); widget.Strings(value.categoryTabOrder);
        widget.Strings(value.itemKeys); widget.Strings(value.childWidgetIds);
        widget.VectorStorage(value.missingItems, true);
        for (const auto& missing : value.missingItems) widget.Text(missing.key);
        folder.VectorStorage(value.folderEntries);
        for (const auto& entry : value.folderEntries)
        { folder.Text(entry.name); folder.Text(entry.displayName); folder.Text(entry.fullPath); folder.Text(entry.typeName); }
    };
    for (const auto& value : widgets) visitWidget(value);
    // The popup is an inline DesktopApp member, outside widgets_' vector.
    visitWidget(dockPopup);
    widget.Record("widgets_and_dock_popup"); folder.Record("folder_entries");
    Payload page; page.VectorStorage(pages);
    for (const auto& value : pages) { page.Text(value.id); page.Text(value.monitorId); }
    page.Record("pages");
    Payload dockEntries; dockEntries.VectorStorage(dock);
    for (const auto& value : dock)
    { dockEntries.Text(value.reference); dockEntries.Strings(value.folderItemKeys); dockEntries.Strings(value.categoryTabOrder); }
    dockEntries.Record("dock_entries");
    Payload applications; applications.VectorStorage(apps);
    for (const auto& value : apps)
    { applications.Text(value.name); applications.Text(value.parsingName); applications.Text(value.iconCacheIdentity); }
    applications.Record("quick_nav_apps");
    Payload search; search.VectorStorage(results); search.VectorStorage(resultIndices, true);
    for (const auto& value : results)
    { search.Text(value.name); search.Text(value.path); search.Text(value.modifiedText); search.Text(value.iconCacheKey); }
    search.Record("quick_nav_results");
    const auto map = [](const char* name, const auto& values) {
        Payload payload; payload.entries = values.size();
        for (const auto& [key, value] : values) { (void)value; payload.Text(key); }
        payload.Record(name);
        const std::string prefix(name);
        performance::Value("host.model.memory", prefix + "_map_value_storage_lower_bound_bytes", {},
            static_cast<double>(values.size() * sizeof(typename std::remove_cvref_t<decltype(values)>::value_type)));
        performance::Value("host.model.memory", prefix + "_bucket_storage_bytes_estimate", {},
            static_cast<double>(values.bucket_count() * sizeof(void*)));
    };
    map("layout_records", layout); map("desktop_key_index", itemIndex);
}
}
