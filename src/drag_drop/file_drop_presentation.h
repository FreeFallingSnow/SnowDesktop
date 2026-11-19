#pragma once
#include "core/drop_model.h"

namespace snowdesktop::file_drop_presentation
{
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
