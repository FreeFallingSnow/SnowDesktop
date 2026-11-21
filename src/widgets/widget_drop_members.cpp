#include "app/app.h"
#include "widgets/widget_folder_search.h"

WidgetContainer::DropMembers WidgetContainer::FilteredDropMembers(const DesktopWidget& model) const
{
    return DropMembers(model);
}

WidgetContainer::DropMembers WidgetContainer::VisibleDropMembers(const DesktopWidget& model) const
{
    return FilteredDropMembers(model);
}

WidgetContainer::DropMembers FolderMapping::FilteredDropMembers(const DesktopWidget& model) const
{
    DropMembers view(model);
    view.indices = &GetVisibleEntryIndices();
    return view;
}

WidgetContainer::DropMembers FileCategories::FilteredDropMembers(const DesktopWidget& model) const
{
    DropMembers view(model);
    view.keys = &GetSearchResultKeys();
    return view;
}

WidgetContainer::DropMembers CollectionGroup::FilteredDropMembers(const DesktopWidget& model) const
{
    DropMembers view(model);
    view.keys = &GetVisibleItemKeys();
    return view;
}

void ScrollingItemWidget::AppendSearchDropRow(DropMembers& view, const std::wstring& ownerId,
    size_t member, const std::wstring& key, const FolderEntry* recursive) const
{
    const auto index = app_->FindWidgetIndexById(ownerId);
    const auto* owner = index < app_->widgets_.size() ? &app_->widgets_[index] : nullptr;
    if (!key.empty() && owner)
        member = std::find(owner->itemKeys.begin(), owner->itemKeys.end(), key) - owner->itemKeys.begin();
    const auto& path = recursive ? recursive->fullPath :
        key.empty() && owner && member < owner->folderEntries.size() ? owner->folderEntries[member].fullPath : key;
    view.search->push_back({owner, member, path});
}

WidgetContainer::DropMembers ScrollingItemWidget::VisibleDropMembers(const DesktopWidget& model) const
{
    if (!folderSearch_ || (!folderSearch_->available && !folderSearch_->pending)) return FilteredDropMembers(model);
    DropMembers view(model);
    view.search.emplace().reserve(folderSearch_->rows.size());
    for (const auto& row : folderSearch_->rows)
        AppendSearchDropRow(view, row.sourceId, row.folderIndex, row.desktopKey, row.recursive.get());
    return view;
}

WidgetContainer::DropMembers FileGroup::FilteredDropMembers(const DesktopWidget& model) const
{
    if (!IsGroupSearchActive())
        if (auto* source = GetActiveSourceContainer()) return source->FilteredDropMembers(model);
    DropMembers view(model);
    if (IsGroupSearchActive())
    {
        const auto& rows = GetGroupSearchResults();
        view.search.emplace().reserve(rows.size());
        for (const auto& row : rows) AppendSearchDropRow(view, row.sourceId, row.folderEntryIndex, row.desktopKey, nullptr);
    }
    return view;
}
