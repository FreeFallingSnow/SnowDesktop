#pragma once

#include "widget_package.h"

#include <windows.h>
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::widget_menu
{
enum class Source { Builtin, Installed, Development };

struct Entry
{
    std::wstring packageId;
    std::wstring displayName;
    std::wstring searchText;
    Source source = Source::Installed;
};

struct Metadata
{
    std::wstring packageId;
    std::wstring name;
    std::wstring description;
    std::wstring publisher;
    bool compatible = true;
    bool valid = false;
};

// UI-thread menu metadata only: runtime loading and permission validation must
// continue reading their own inputs. Keep file probes so development edits are
// visible even when the package manager's registered manifest has not changed.
class Catalogue
{
public:
    template<class Loader>
    std::vector<Entry> Build(std::span<const widget::InstalledPackage> packages,
        const std::string& language, Loader&& load)
    {
        std::vector<Entry> entries;
        std::unordered_set<std::string> active;
        for (const auto& package : packages)
        {
            if (!package.active || !package.enabled) continue;
            const auto& id = package.manifest.id;
            active.insert(id);
            const auto path = package.root / L"widget.json";
            const auto stamp = ReadStamp(path);
            auto found = rows_.find(id);
            Metadata metadata;
            if (stamp && found != rows_.end() &&
                found->second.path == path && found->second.stamp == *stamp &&
                found->second.language == language &&
                found->second.version == package.manifest.version &&
                found->second.sha256 == package.sha256 &&
                found->second.entry == package.manifest.entry)
            {
                metadata = found->second.metadata;
            }
            else
            {
                metadata = load(package);
                // Do not retain failed reads or a file changed during parsing.
                rows_.erase(id);
                if (metadata.valid && stamp && ReadStamp(path) == stamp)
                    rows_.emplace(id, Row{path, *stamp, language,
                        package.manifest.version, package.sha256,
                        package.manifest.entry, metadata});
            }
            if (!metadata.compatible) continue;
            Entry entry;
            entry.packageId = metadata.packageId;
            entry.displayName = metadata.name.empty()
                ? entry.packageId : metadata.name;
            entry.searchText = entry.displayName + L"\n" + entry.packageId +
                L"\n" + metadata.description + L"\n" + metadata.publisher;
            entry.source = package.builtin ? Source::Builtin :
                package.development ? Source::Development : Source::Installed;
            entries.push_back(std::move(entry));
        }
        std::erase_if(rows_, [&](const auto& row) {
            return !active.contains(row.first);
        });
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.packageId < right.packageId;
        });
        return entries;
    }

private:
    struct Stamp
    {
        DWORD attributes, createdLow, createdHigh, modifiedLow, modifiedHigh;
        DWORD sizeLow, sizeHigh;
        bool operator==(const Stamp&) const = default;
    };
    static std::optional<Stamp> ReadStamp(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return std::nullopt;
        return Stamp{data.dwFileAttributes, data.ftCreationTime.dwLowDateTime,
            data.ftCreationTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime,
            data.ftLastWriteTime.dwHighDateTime, data.nFileSizeLow, data.nFileSizeHigh};
    }
    struct Row
    {
        std::filesystem::path path;
        Stamp stamp;
        std::string language, version, sha256, entry;
        Metadata metadata;
    };
    std::unordered_map<std::string, Row> rows_;
};
}
