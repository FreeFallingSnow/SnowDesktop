#pragma once
#include "ui/menu/modern_menu.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_service.h"
#include "shell_extension_menu_rows.h"
#include <algorithm>
#include <set>

namespace snowdesktop::shell_extensions
{
inline void AddMoreManagementAction(std::vector<modern_menu::Item> &items, UINT moreCommand,
                                    UINT manageCommand, std::wstring label)
{
    const auto more = std::find_if(items.begin(), items.end(),
                                   [=](const auto &item) { return item.command == moreCommand; });
    if (more == items.end())
        return;
    more->inlineAction = true;
    more->inlineGroup = moreCommand;
    more->measureInlineAction = true;
    modern_menu::Item manage;
    manage.command = manageCommand;
    manage.label = std::move(label);
    manage.inlineAction = true;
    manage.inlineGroup = moreCommand;
    manage.compactInlineAction = true;
    manage.measureInlineAction = true;
    items.insert(std::next(more), std::move(manage));
}
inline void MoveMoreToBottom(std::vector<modern_menu::Item> &items, UINT moreCommand)
{
    const auto anchor = std::find_if(items.begin(), items.end(), [=](const auto &item) {
        return moreCommand && item.command == moreCommand;
    });
    if (anchor == items.end())
        return;
    auto end = std::next(anchor);
    if (anchor->inlineAction && anchor->inlineGroup)
        while (end != items.end() && end->inlineAction && end->inlineGroup == anchor->inlineGroup)
            ++end;
    std::vector<modern_menu::Item> footer(std::make_move_iterator(anchor), std::make_move_iterator(end));
    items.erase(anchor, end);
    // Removing More may leave its old group empty. Preserve other groups.
    std::vector<modern_menu::Item> ordered;
    for (auto &item : items)
        if (!item.separator || (!ordered.empty() && !ordered.back().separator))
            ordered.push_back(std::move(item));
    if (!ordered.empty() && !ordered.back().separator)
    {
        modern_menu::Item divider;
        divider.separator = true;
        ordered.push_back(divider);
    }
    ordered.insert(ordered.end(), std::make_move_iterator(footer.begin()),
                   std::make_move_iterator(footer.end()));
    items = std::move(ordered);
}
inline void ConfigureMoreManagementRow(std::vector<modern_menu::Item> &items, UINT moreCommand,
                                       UINT manageCommand, std::wstring label, bool hidden)
{
    if (!hidden)
    {
        AddMoreManagementAction(items, moreCommand, manageCommand, std::move(label));
        MoveMoreToBottom(items, moreCommand);
        return;
    }
    std::erase_if(items, [=](const auto &item) {
        return (moreCommand && item.command == moreCommand) ||
            (manageCommand && item.command == manageCommand);
    });
    // Keep separators between surviving groups, without leaving an empty footer.
    std::vector<modern_menu::Item> visible;
    for (auto &item : items)
        if (!item.separator || (!visible.empty() && !visible.back().separator))
            visible.push_back(std::move(item));
    while (!visible.empty() && visible.back().separator)
        visible.pop_back();
    items = std::move(visible);
}
inline void InsertBeforeMore(std::vector<modern_menu::Item> &items,
                             const std::vector<modern_menu::Item> &additions, UINT moreCommand,
                             bool separateFallback = true)
{
    MoveMoreToBottom(items, moreCommand);
    if (additions.empty())
        return;
    const auto anchor = std::find_if(items.begin(), items.end(), [moreCommand](const auto &item) {
        return moreCommand && item.command == moreCommand;
    });
    if (anchor != items.end())
        items.insert(anchor, additions.begin(), additions.end());
    else
    {
        if (separateFallback && !items.empty() && !items.back().separator)
        {
            modern_menu::Item divider;
            divider.separator = true;
            items.push_back(divider);
        }
        items.insert(items.end(), additions.begin(), additions.end());
    }
}
// Caller owns this bridge for the entire synchronous custom menu.
class Presentation
{
  public:
    static constexpr UINT FirstCommand = 0x71000000;
    Presentation(const Request &source, Preferences prefs, std::wstring, std::wstring,
                 MenuService &service = SharedMenuService(), std::function<void(bool)> completed = {})
        : prefs_(std::move(prefs)), source_(source), normalSource_(source), originalSource_(source), service_(service), completed_(std::move(completed))
    {
        if (source.paths.empty() || !service_.MenuEnabled(source_, prefs_)) return;
        refreshState_ = prefs_.showAll && !source.background;
        auto view = service_.MenuDisplay(source_, prefs_);
        initialRevision_ = view.revision;
        cached_ = std::move(view.snapshot);
        contexts_ = view.contexts;
        bool startShown = false, normalNeeded = false;
        const unsigned candidates = contexts_ ? contexts_ : source.background || source.context == Context::Desktop
            ? ContextBit(source.context == Context::Desktop ? Context::Desktop : Context::FolderBackground) : 3u;
        for (int i = 0; i < 4; ++i) if (candidates & (1u << i))
            for (const auto &id : EffectiveShownIds(prefs_, static_cast<Context>(i)))
            {
                const auto *pair = StatePairForId(id);
                if (pair && pair->id == "state:start-pin") startShown = true;
                else { normalNeeded = true; refreshState_ |= pair != nullptr; }
            }
        startLane_ = startShown && !normalNeeded && !source.background && source.context != Context::Desktop && source.paths.size() == 1;
        if (startShown && !startLane_) { normalNeeded = true; refreshState_ = true; }
        if (cached_)
        {
            // Pin state is external to the shortcut's timestamp. Display the
            // ordinary warm rows immediately, but materialize state commands
            // only from this opening's fresh native query.
            std::erase_if(cached_->entries, [this](const auto &e) {
                const auto *pair = StatePairForVerb(e.key);
                return pair && (refreshState_ || (startLane_ && pair->id == "state:start-pin"));
            });
        }
        normalDone_ = !normalNeeded || (cached_.has_value() && !cachedOriginal_ && !refreshState_ &&
            !view.attributionPending);
        // Retire any prewarm already in flight as well: it may have captured
        // external pin state before this opening.
        if (normalNeeded)
        {
            if (refreshState_) service_.Invalidate(source_);
            service_.Query(source_, QueryPriority::Menu, refreshState_ || !view.error.empty());
        }
        startDone_ = !startLane_;
        if (startLane_)
        {
            startSource_ = source_; startSource_.startPinOnly = true;
            startRevision_ = service_.View(startSource_).revision;
            service_.Invalidate(startSource_);
            service_.Query(startSource_, QueryPriority::Menu, true);
        }
    }
    ~Presentation()
    {
        // No query is owned by a popup. Closing only drops this immutable view;
        // the service completes valid in-flight work for the next opening.
        for (auto image : images_) DeleteObject(image);
    }
    void Attach(std::vector<modern_menu::Item> &items, modern_menu::Options &options, UINT moreCommand)
    {
        MoveMoreToBottom(items, moreCommand);
        if (cached_) UpdateNormal(items, cached_->entries, moreCommand, false, cachedOriginal_);
        if (!normalDone_)
        {
            const auto view = service_.MenuDisplay(normalSource_, prefs_, true);
            ShowRoots(items, view, moreCommand);
            SyncSources(view);
            PollSources(items, moreCommand);
        }
        if ((!normalDone_ || !startDone_ || !SourcesDone()) && !source_.paths.empty())
        {
            options.pollItemsFinished = [this] { return normalDone_ && startDone_ && SourcesDone(); };
            options.pollItemsStablePrefix = true;
            options.pollPrefetchedSubmenus = true;
            options.pollItems = [this, moreCommand](const auto &current, bool canApply) -> std::optional<std::vector<modern_menu::Item>> {
                if (!canApply)
                {
                    // The existing query continues while UI changes are held.
                    // Missing/failed/slow results never start another query.
                    return {};
                }
                auto updated = current;
                bool progressed = false;
                // The scheduler can discover the plan after the popup opens.
                // Read it before completing the ordinary stream, including a
                // sources-only plan with no aggregate payload at all.
                if (!normalDone_) SyncSources(service_.MenuDisplay(normalSource_, prefs_, true));
                progressed |= PollSources(updated, moreCommand);
                if (!normalDone_)
                {
                    auto view = service_.MenuDisplay(normalSource_, prefs_, true);
                    if (view.pending) progressed |= ShowRoots(updated, view, moreCommand);
                    if (!view.pending)
                    {
                        if (view.snapshot && (!refreshState_ || (view.revision > initialRevision_ && view.error.empty())))
                        {
                            auto additions = std::move(view.snapshot->entries);
                            const bool warm = cached_.has_value();
                            std::erase_if(additions, [&](const auto &e) {
                                const auto *pair = StatePairForVerb(e.key);
                                return (startLane_ && pair && pair->id == "state:start-pin") ||
                                    (warm && !cachedOriginal_ && refreshState_ && !pair);
                            });
                            progressed |= UpdateNormal(updated, additions, moreCommand, false,
                                normalSource_.originalShortcutOnly, true);
                        }
                        progressed |= RemoveRoots(updated);
                        normalDone_ = !view.attributionPending;
                        progressed |= normalDone_;
                    }
                }
                if (!startDone_)
                {
                    auto view = service_.MenuDisplay(startSource_, prefs_, true);
                    if (!view.pending)
                    {
                        if (view.snapshot && view.revision > startRevision_ && view.error.empty())
                            UpdateNormal(updated, view.snapshot->entries, moreCommand);
                        startDone_ = progressed = true;
                    }
                }
                // Each stream can publish independently; an empty/failed
                // stream also completes, without waiting for other extensions.
                if (progressed) return updated;
                return {};
            };
        }
    }
    bool Invoke(UINT command, POINT point, HWND owner = nullptr)
    {
        const auto found = commands_.find(command);
        if (found == commands_.end()) return false;
        const auto *pair = found->second.empty() ? nullptr : StatePairForVerb(std::get<1>(found->second.back()));
        const auto &query = startLane_ && pair && pair->id == "state:start-pin" ? startSource_ :
            originalCommands_.contains(command) ? originalSource_ : source_;
        service_.Execute(query,
            found->second, point, completed_, owner);
        return true;
    }

  private:
    bool ShowRoots(std::vector<modern_menu::Item> &items, const MenuView &view, UINT moreCommand)
    {
        if (cached_ || !view.roots || view.roots->entries.empty() || rootRevision_ == view.revision) return false;
        rootRevision_ = view.revision;
        return UpdateNormal(items, view.roots->entries, moreCommand, true);
    }
    bool RemoveRoots(std::vector<modern_menu::Item> &items)
    {
        if (rootCommands_.empty()) return false;
        std::erase_if(items, [&](const auto &item) {
            if (!rootCommands_.contains(item.command)) return false;
            rowEntries_.erase(item.command);
            return true;
        });
        rootCommands_.clear();
        presentation_detail::RemoveEmptySeparators(items);
        return true;
    }
    struct SourceLane { Request request; bool done = false; };
    bool SourcesDone() const
    {
        return std::all_of(sources_.begin(), sources_.end(), [](const auto &source) { return source.done; });
    }
    void SyncSources(const MenuView &view)
    {
        for (const auto &request : view.sources)
            if (std::none_of(sources_.begin(), sources_.end(), [&](const auto &source) { return source.request == request; }))
                sources_.push_back({request});
    }
    bool PollSources(std::vector<modern_menu::Item> &items, UINT moreCommand)
    {
        bool progressed = false;
        for (auto &source : sources_) if (!source.done)
        {
            if (!service_.MenuEnabled(source.request, prefs_))
            {
                source.done = true;
                progressed = true;
                continue;
            }
            auto view = service_.MenuDisplay(source.request, prefs_, true);
            if (view.pending || view.attributionPending) continue;
            if (view.snapshot && view.error.empty() && !view.snapshot->entries.empty())
            {
                auto additions = std::move(view.snapshot->entries);
                // State-changing commands retain their per-opening refresh and
                // dedicated execution rules, even when a source emits them too.
                std::erase_if(additions, [](const auto &entry) { return StatePairForVerb(entry.key) != nullptr; });
                UpdateNormal(items, additions, moreCommand);
            }
            source.done = true;
            progressed = true;
        }
        return progressed;
    }
    // Rebind visible IDs to the fresh exact reference, including descendants
    // whose parent acquired registration metadata. Never relax execution lookup.
    void RebindRow(modern_menu::Item &next, const modern_menu::Item &previous,
                   const Entry &nextEntry, const Entry &previousEntry)
    {
        const auto reference = commands_.at(next.command);
        const bool original = originalCommands_.contains(next.command);
        commands_[previous.command] = reference;
        if (original) originalCommands_.insert(previous.command);
        else originalCommands_.erase(previous.command);
        next.command = previous.command;
        // Reuse identical artwork, not the first available image. A complete
        // result must be able to correct an interim icon, in either size direction.
        if (previous.image && (!next.image ||
            std::tie(nextEntry.width, nextEntry.height, nextEntry.pixels) ==
            std::tie(previousEntry.width, previousEntry.height, previousEntry.pixels)))
            next.image = previous.image;
        std::set<UINT> used;
        for (size_t index = 0; index < next.children.size(); ++index)
        {
            auto &child = next.children[index];
            if (child.separator) continue;
            const auto &childReference = commands_.at(child.command);
            const auto old = std::find_if(previous.children.begin(), previous.children.end(), [&](const auto &candidate) {
                const auto found = commands_.find(candidate.command);
                return !candidate.separator && !used.contains(candidate.command) && found != commands_.end() &&
                    presentation_detail::SameDisplayReference(found->second.back(), childReference.back());
            });
            if (old != previous.children.end())
            {
                used.insert(old->command);
                RebindRow(child, *old, nextEntry.children[index],
                    previousEntry.children[static_cast<size_t>(std::distance(previous.children.begin(), old))]);
            }
        }
    }
    static bool IsOurCommand(UINT command)
    {
        return command >= FirstCommand && command < FirstCommand + 65536;
    }
    static void Insert(std::vector<modern_menu::Item> &items, const std::vector<modern_menu::Item> &additions,
                       UINT moreCommand)
    {
        if (additions.empty())
            return;
        const auto anchor = std::find_if(items.begin(), items.end(),
            [=](const auto &item) { return moreCommand && item.command == moreCommand; });
        if (anchor != items.end())
        {
            // Attach already established the host/footer boundary. Moving it
            // again here would create one separator per arriving query batch.
            items.insert(anchor, additions.begin(), additions.end());
            return;
        }
        if (!items.empty() && !items.back().separator &&
            std::none_of(items.begin(), items.end(), [](const auto &item) {
                return !item.separator && IsOurCommand(item.command);
            }))
        {
            modern_menu::Item divider;
            divider.separator = true;
            divider.command = FirstCommand;
            items.push_back(divider);
        }
        items.insert(items.end(), additions.begin(), additions.end());
    }
    bool UpdateNormal(std::vector<modern_menu::Item> &items, const std::vector<Entry> &entries,
                      UINT moreCommand, bool pending = false, bool original = false,
                      bool completeRoots = false)
    {
        bool changed = false;
        std::set<UINT> used;
        std::vector<modern_menu::Item> additions;
        for (const auto &entry : entries)
        {
            // Top-level extensions form one continuous section. Native
            // separators inside their submenus remain in the converted tree.
            if (entry.separator) continue;
            auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
                const auto row = rowEntries_.find(item.command);
                return !used.contains(item.command) && row != rowEntries_.end() &&
                    originalCommands_.contains(item.command) == original &&
                    presentation_detail::SameDisplayEntry(row->second, entry);
            });
            if (found == items.end() && completeRoots)
            {
                // Only this query's non-executable preview may refine its
                // identity after materializing children/native fallbacks.
                // Complete rows from other sources never match by caption.
                found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
                    return !used.contains(item.command) && rootCommands_.contains(item.command) &&
                        item.label == entry.label;
                });
            }
            if (found != items.end())
            {
                const auto command = found->command;
                used.insert(command);
                if (pending) continue; // A preview cannot downgrade a complete source row.
                const auto &previous = rowEntries_.at(command);
                auto merged = presentation_detail::MergeEntry(previous, entry, rootCommands_.contains(command));
                if (!rootCommands_.contains(command) && presentation_detail::SameEntry(previous, merged)) continue;
                auto converted = Convert({merged}, {}, original).front();
                RebindRow(converted, *found, merged, previous);
                *found = std::move(converted);
                rowEntries_[command] = std::move(merged);
                rootCommands_.erase(command);
                changed = true;
                continue;
            }
            auto converted = Convert({entry}, {}, original).front();
            converted.childrenPending = pending;
            rowEntries_[converted.command] = entry;
            if (pending) rootCommands_.insert(converted.command);
            additions.push_back(std::move(converted));
        }
        Insert(items, additions, moreCommand);
        if (changed || !additions.empty()) OrderRows(items);
        return changed || !additions.empty();
    }
    void OrderRows(std::vector<modern_menu::Item> &items)
    {
        std::vector<modern_menu::Item> roots;
        for (auto &item : items)
            if (rowEntries_.contains(item.command)) roots.push_back(std::move(item));
        std::stable_sort(roots.begin(), roots.end(), [&](const auto &left, const auto &right) {
            return presentation_detail::RootBefore(rowEntries_.at(left.command), rowEntries_.at(right.command));
        });
        auto root = roots.begin();
        for (auto &item : items)
            if (rowEntries_.contains(item.command)) item = std::move(*root++);
    }
    std::vector<modern_menu::Item> Convert(const std::vector<Entry> &entries,
                                           const CommandReference &parent = {}, bool original = false)
    {
        std::vector<modern_menu::Item> result;
        for (const auto &e : entries)
        {
            modern_menu::Item item;
            const auto reference = AppendReference(parent, e);
            item.command = ++nextCommand_;
            commands_[item.command] = reference;
            if (original) originalCommands_.insert(item.command);
            else originalCommands_.erase(item.command);
            item.label = e.label;
            item.accessKey = e.accessKey;
            item.enabled = e.enabled;
            item.checked = e.checked;
            item.separator = e.separator;
            item.children = Convert(e.children, reference, original);
            if (e.width > 0 && e.height > 0 && e.width <= 128 && e.height <= 128 &&
                e.pixels.size() == static_cast<size_t>(e.width * e.height * 4))
            {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = e.width;
                info.bmiHeader.biHeight = -e.height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void *pixels = nullptr;
                HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                if (image && pixels)
                {
                    memcpy(pixels, e.pixels.data(), e.pixels.size());
                    images_.push_back(image);
                    item.image = image;
                }
                else if (image)
                    DeleteObject(image);
            }
            result.push_back(std::move(item));
        }
        return result;
    }
    Preferences prefs_;
    Request source_;
    Request startSource_;
    Request normalSource_, originalSource_;
    MenuService &service_;
    std::function<void(bool)> completed_;
    unsigned contexts_ = 0;
    UINT nextCommand_ = FirstCommand;
    std::map<UINT, CommandReference> commands_;
    std::set<UINT> originalCommands_;
    std::map<UINT, Entry> rowEntries_;
    std::vector<SourceLane> sources_;
    std::optional<Reply> cached_;
    bool refreshState_ = false;
    bool cachedOriginal_ = false;
    bool startLane_ = false, normalDone_ = true, startDone_ = true;
    std::uint64_t initialRevision_ = 0;
    std::uint64_t startRevision_ = 0;
    std::vector<HBITMAP> images_;
    std::uint64_t rootRevision_ = UINT64_MAX;
    std::set<UINT> rootCommands_;
};
} // namespace snowdesktop::shell_extensions
