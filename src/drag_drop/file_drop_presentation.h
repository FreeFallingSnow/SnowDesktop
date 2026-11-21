#pragma once
#include "core/drop_model.h"

namespace snowdesktop::file_drop_presentation
{
// A member boundary is stored in its owner's model. Filtering and cross-folder
// search change the row at which the boundary is painted.
struct MemberView
{
    struct SearchRow
    {
        const DesktopWidget* owner;
        size_t index;
        std::wstring key;
    };

    explicit MemberView(const DesktopWidget& data) : model(data) {}
    const DesktopWidget& model;
    const std::vector<size_t>* indices = nullptr;
    const std::vector<std::wstring>* keys = nullptr;
    std::optional<std::vector<SearchRow>> search;

    size_t ModelCount() const
    {
        return model.type == DesktopWidgetType::FolderMapping ? model.folderEntries.size() : model.itemKeys.size();
    }
    size_t Count() const
    {
        if (search) return search->size();
        if (indices) return indices->size();
        return keys ? keys->size() : ModelCount();
    }
    const std::wstring& KeyAt(size_t row) const
    {
        if (search) return (*search)[row].key;
        if (keys) return (*keys)[row];
        if (model.type == DesktopWidgetType::FolderMapping)
            return model.folderEntries[indices ? (*indices)[row] : row].fullPath;
        return model.itemKeys[row];
    }
    size_t ModelBoundary(size_t row) const
    {
        if (row >= Count()) return ModelCount();
        if (search) return (*search)[row].index;
        if (indices) return (*indices)[row];
        if (!keys || keys == &model.itemKeys) return row;
        const auto& anchor = (*keys)[row];
        return std::find_if(model.itemKeys.begin(), model.itemKeys.end(), [&](const auto& key) {
            return _wcsicmp(key.c_str(), anchor.c_str()) == 0;
        }) - model.itemKeys.begin();
    }
    size_t VisibleBoundary(size_t index, const std::wstring& owner) const
    {
        if (search)
        {
            size_t end = search->size();
            for (size_t i = 0; i < search->size(); ++i)
            {
                const auto& row = (*search)[i];
                if (!row.owner || row.owner->id != owner) continue;
                if (row.index >= index) return i;
                end = i + 1;
            }
            return end;
        }
        if (index >= ModelCount()) return Count();
        if (indices) return std::find(indices->begin(), indices->end(), index) - indices->begin();
        if (!keys || keys == &model.itemKeys) return index;
        const auto& anchor = model.itemKeys[index];
        return std::find_if(keys->begin(), keys->end(), [&](const auto& key) {
            return _wcsicmp(key.c_str(), anchor.c_str()) == 0;
        }) - keys->begin();
    }
    bool ContainsOwner(const std::wstring& id) const
    {
        return model.id == id || (search && std::any_of(search->begin(), search->end(),
            [&](const auto& row) { return row.owner && row.owner->id == id; }));
    }
};

struct Gap
{
    size_t boundary;
    std::shared_ptr<FolderEntry> file;
    size_t position = 0;
};

// Indices in the model remain valid for selection and commit. Only visible
// positions change while a transfer owns an insertion boundary.
struct Layout
{
    std::vector<size_t> hidden;
    std::vector<Gap> gaps;
    void Arrange()
    {
        std::sort(hidden.begin(), hidden.end());
        hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
        std::stable_sort(gaps.begin(), gaps.end(), [](const auto& a, const auto& b) { return a.boundary < b.boundary; });
        for (size_t i = 0; i < gaps.size(); ++i)
        {
            const auto before = std::lower_bound(hidden.begin(), hidden.end(), gaps[i].boundary) - hidden.begin();
            gaps[i].position = gaps[i].boundary - static_cast<size_t>(before) + i;
        }
    }
    std::optional<size_t> Member(size_t index) const
    {
        const auto removed = std::lower_bound(hidden.begin(), hidden.end(), index);
        if (removed != hidden.end() && *removed == index) return {};
        const auto added = std::upper_bound(gaps.begin(), gaps.end(), index,
            [](size_t boundary, const Gap& gap) { return boundary < gap.boundary; });
        return index - static_cast<size_t>(removed - hidden.begin()) + static_cast<size_t>(added - gaps.begin());
    }
    size_t Count(size_t modelCount) const { return modelCount - std::min(modelCount, hidden.size()) + gaps.size(); }
    std::pair<size_t, size_t> ModelRange(std::pair<size_t, size_t> display, size_t count) const
    {
        return {std::min(count, display.first - std::min(display.first, gaps.size())),
            std::min(count, display.second + hidden.size())};
    }
};
}
