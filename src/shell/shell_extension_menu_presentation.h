#pragma once
#include "ui/menu/modern_menu.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_service.h"
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
        : prefs_(std::move(prefs)), source_(source), normalSource_(source), service_(service), completed_(std::move(completed))
    {
        if (source.paths.empty() || !service_.MenuEnabled(source_, prefs_)) return;
        auto view = service_.MenuDisplay(source_, prefs_);
        initialRevision_ = view.revision;
        normalStarted_ = GetTickCount64();
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
        startLane_ = startShown && !source.background && source.context != Context::Desktop && source.paths.size() == 1;
        if (startShown && !startLane_) { normalNeeded = true; refreshState_ = true; }
        if (normalNeeded && CanQueryOriginalShortcutObjects(source_))
        {
            originalSource_ = source_; originalSource_.originalShortcutOnly = true;
            if (!cached_)
            {
                auto original = service_.MenuDisplay(originalSource_, prefs_);
                cached_ = std::move(original.snapshot);
                cachedOriginal_ = cached_.has_value();
            }
        }
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
            service_.Query(source_, QueryPriority::Menu, refreshState_ || cachedOriginal_);
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
        if (cached_) Insert(items, Convert(NewNormalEntries(cached_->entries), {}, cachedOriginal_), moreCommand);
        if (!normalDone_)
        {
            const auto view = service_.MenuDisplay(normalSource_, prefs_, true);
            ShowRoots(items, view, moreCommand);
            SyncSources(view);
            PollSources(items, moreCommand);
        }
        if ((!normalDone_ || !startDone_ || !SourcesDone()) && !source_.paths.empty())
        {
            options.pollItemsFinished = [this] { return normalDone_ && startDone_ && originalDone_ && SourcesDone(); };
            options.pollItemsStablePrefix = true;
            options.pollPrefetchedSubmenus = true;
            options.pollItems = [this, moreCommand](const auto &current, bool canApply) -> std::optional<std::vector<modern_menu::Item>> {
                if (!canApply)
                {
                    // A held button, open child or scroll position defers UI
                    // changes, but must not stop supervised query recovery.
                    if (!normalDone_)
                    {
                        auto view = service_.MenuDisplay(normalSource_, prefs_, true);
                        MaybeStartOriginalQuery(view);
                        if (!view.pending && !view.snapshot && !view.error.empty() && !normalRetried_)
                            RetryNormalQuery();
                    }
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
                    MaybeStartOriginalQuery(view);
                    if (!view.pending && !view.snapshot && !view.error.empty() && !normalRetried_)
                    {
                        // A failed prewarm may still be in backoff, or the helper
                        // may have exited during this opening. Recover once with
                        // a fresh query while this popup remains subscribed.
                        // Shortcut recovery isolates original file handlers from
                        // the delegated target aggregate that just failed.
                        RetryNormalQuery();
                    }
                    else if (!view.pending)
                    {
                        progressed |= RemoveRoots(updated);
                        if (view.snapshot && (!refreshState_ || (view.revision > initialRevision_ && view.error.empty())))
                        {
                            auto additions = std::move(view.snapshot->entries);
                            const bool warm = cached_.has_value();
                            std::erase_if(additions, [&](const auto &e) {
                                const auto *pair = StatePairForVerb(e.key);
                                return (startLane_ && pair && pair->id == "state:start-pin") ||
                                    (warm && !cachedOriginal_ && refreshState_ && !pair);
                            });
                            if (!normalSource_.originalShortcutOnly && (!originalCommands_.empty() || !sources_.empty()))
                                progressed |= SupplementChildren(updated, additions);
                            additions = NewNormalEntries(std::move(additions));
                            if (!additions.empty())
                            {
                                auto converted = Convert(additions, {}, normalSource_.originalShortcutOnly);
                                ReuseRootRows(converted);
                                Insert(updated, converted, moreCommand);
                                progressed = true;
                            }
                        }
                        normalDone_ = !view.attributionPending;
                        rootImages_.clear();
                        rootRows_.clear();
                        progressed |= normalDone_;
                    }
                }
                if (!originalDone_)
                {
                    auto view = service_.MenuDisplay(originalSource_, prefs_, true);
                    if (!view.pending)
                    {
                        if (view.snapshot && view.revision > originalRevision_ && view.error.empty())
                        {
                            auto additions = std::move(view.snapshot->entries);
                            std::erase_if(additions, [this](const auto &entry) {
                                const auto *pair = StatePairForVerb(entry.key);
                                return startLane_ && pair && pair->id == "state:start-pin";
                            });
                            additions = NewNormalEntries(std::move(additions));
                            if (!additions.empty())
                            {
                                Insert(updated, Convert(additions, {}, true), moreCommand);
                                progressed = true;
                            }
                        }
                        originalDone_ = !view.attributionPending;
                        progressed |= originalDone_;
                    }
                }
                if (!startDone_)
                {
                    auto view = service_.MenuDisplay(startSource_, prefs_, true);
                    if (!view.pending)
                    {
                        if (view.snapshot && view.revision > startRevision_ && view.error.empty())
                            Insert(updated, Convert(view.snapshot->entries), moreCommand);
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
        if (cached_ || rootsShown_ || !view.roots || view.roots->entries.empty()) return false;
        auto entries = NewNormalEntries(view.roots->entries);
        auto converted = Convert(entries);
        for (size_t i = 0; i < converted.size(); ++i)
        {
            auto &item = converted[i];
            if (!item.separator)
            {
                item.childrenPending = !entries[i].children.empty();
                rootCommands_.insert(item.command);
                rootReferences_.push_back(AppendReference({}, entries[i]));
                rootImages_[item.command] = item.image;
                rootRows_.push_back({item.command, item.label});
            }
        }
        Insert(items, converted, moreCommand);
        rootsShown_ = true;
        return !converted.empty();
    }
    void ReuseRootRows(std::vector<modern_menu::Item> &items)
    {
        std::set<UINT> used;
        for (const auto &item : items)
            if (rootImages_.contains(item.command)) used.insert(item.command);
        for (auto &item : items) if (!item.separator)
        {
            if (!rootImages_.contains(item.command))
            {
                // Provisional headers carry no executable identity. Descendant
                // normalization/native fallback may refine the final reference;
                // keep the same labelled header while binding its NEW reference.
                const auto old = std::find_if(rootRows_.begin(), rootRows_.end(), [&](const auto &row) {
                    return row.second == item.label && !used.contains(row.first);
                });
                if (old != rootRows_.end())
                {
                    commands_[old->first] = commands_.at(item.command);
                    item.command = old->first; used.insert(item.command);
                }
            }
            if (const auto image = rootImages_.find(item.command); image != rootImages_.end()) item.image = image->second;
        }
    }
    bool RemoveRoots(std::vector<modern_menu::Item> &items)
    {
        if (rootCommands_.empty()) return false;
        std::erase_if(items, [&](const auto &item) { return rootCommands_.contains(item.command); });
        for (const auto &reference : rootReferences_)
            if (const auto old = std::find(displayedNormal_.begin(), displayedNormal_.end(), reference); old != displayedNormal_.end())
                displayedNormal_.erase(old);
        rootReferences_.clear(); rootCommands_.clear();
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
        bool progressed = false, fallback = false;
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
                Insert(items, Convert(NewNormalEntries(std::move(additions))), moreCommand);
            }
            else fallback = true;
            source.done = true;
            progressed = true;
        }
        if (fallback && !sourcesRetried_)
        {
            sourcesRetried_ = true;
            normalDone_ = false;
            service_.QueryFullMenu(source_);
        }
        return progressed;
    }
    bool SupplementChildren(std::vector<modern_menu::Item> &items, const std::vector<Entry> &entries,
                            const CommandReference &parent = {})
    {
        bool changed = false, divider = false;
        std::vector<CommandReference> observed;
        for (const auto &entry : entries)
        {
            if (entry.separator) { divider = true; continue; }
            const auto reference = AppendReference(parent, entry);
            observed.push_back(reference);
            auto remaining = std::count(observed.begin(), observed.end(), reference);
            auto existing = items.end();
            for (auto item = items.begin(); item != items.end(); ++item)
            {
                const auto command = commands_.find(item->command);
                if (!item->separator && command != commands_.end() && command->second == reference && --remaining == 0)
                { existing = item; break; }
            }
            if (existing != items.end())
            {
                if (!entry.children.empty())
                    changed |= SupplementChildren(existing->children, entry.children, reference);
            }
            else if (!parent.empty())
            {
                if (divider && !items.empty() && !items.back().separator)
                { modern_menu::Item separator; separator.separator = true; items.push_back(separator); }
                // Keep command IDs of visible siblings, including repeated
                // canonical commands, distinct from the new child subtree.
                for (const auto &command : commands_) converting_.insert(command.first);
                auto additions = Convert({entry}, parent);
                items.insert(items.end(), std::make_move_iterator(additions.begin()), std::make_move_iterator(additions.end()));
                changed = true;
            }
            divider = false;
        }
        return changed;
    }
    void MaybeStartOriginalQuery(const MenuView &view)
    {
        if (originalQueued_ || cached_ || !originalSource_.originalShortcutOnly ||
            normalSource_.originalShortcutOnly || !view.pending || GetTickCount64() - normalStarted_ < 3000)
            return;
        // Preserve complete discovery while an isolated original-file query
        // can expose useful rows ahead of a slow shortcut target aggregate.
        originalQueued_ = true; originalDone_ = false;
        originalRevision_ = service_.View(originalSource_).revision;
        service_.Query(originalSource_, QueryPriority::Menu, true);
    }
    void RetryNormalQuery()
    {
        normalRetried_ = true;
        if (originalQueued_) { normalDone_ = true; return; }
        if (originalSource_.originalShortcutOnly)
        {
            normalSource_ = originalSource_;
            initialRevision_ = service_.View(normalSource_).revision;
        }
        if (sourcesRetried_ && !normalSource_.originalShortcutOnly) service_.QueryFullMenu(normalSource_);
        else service_.Query(normalSource_, QueryPriority::Menu, true);
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
        if (!items.empty() && !items.back().separator &&
            std::none_of(items.begin(), items.end(),
                         [=](const auto &item) { return moreCommand && item.command == moreCommand; }))
        {
            modern_menu::Item divider;
            divider.separator = true;
            divider.command = FirstCommand;
            items.push_back(divider);
        }
        InsertBeforeMore(items, additions, moreCommand, false);
    }
    std::vector<Entry> NewNormalEntries(std::vector<Entry> entries)
    {
        std::vector<Entry> result;
        const auto previous = displayedNormal_;
        std::vector<CommandReference> observed;
        for (auto &entry : entries)
        {
            if (entry.separator)
            {
                if (!result.empty() && !result.back().separator)
                    result.push_back(std::move(entry));
                continue;
            }
            const auto reference = AppendReference({}, entry);
            observed.push_back(reference);
            if (std::count(observed.begin(), observed.end(), reference) <=
                std::count(previous.begin(), previous.end(), reference))
                continue;
            displayedNormal_.push_back(reference);
            result.push_back(std::move(entry));
        }
        while (!result.empty() && result.back().separator)
            result.pop_back();
        return result;
    }
    std::vector<modern_menu::Item> Convert(const std::vector<Entry> &entries,
                                           const CommandReference &parent = {}, bool original = false)
    {
        if (parent.empty())
            converting_.clear();
        std::vector<modern_menu::Item> result;
        for (const auto &e : entries)
        {
            modern_menu::Item item;
            const auto reference = AppendReference(parent, e);
            const auto existing = std::find_if(commands_.begin(), commands_.end(), [&](const auto &command) {
                return command.second == reference && !converting_.contains(command.first);
            });
            item.command = existing == commands_.end() ? ++nextCommand_ : existing->first;
            commands_[item.command] = reference;
            if (original) originalCommands_.insert(item.command);
            else originalCommands_.erase(item.command);
            converting_.insert(item.command);
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
    std::set<UINT> converting_;
    std::set<UINT> originalCommands_;
    std::vector<CommandReference> displayedNormal_;
    std::vector<SourceLane> sources_;
    bool sourcesRetried_ = false;
    std::optional<Reply> cached_;
    bool refreshState_ = false;
    bool normalRetried_ = false;
    bool cachedOriginal_ = false;
    bool originalQueued_ = false, originalDone_ = true;
    ULONGLONG normalStarted_ = 0;
    std::uint64_t originalRevision_ = 0;
    bool startLane_ = false, normalDone_ = true, startDone_ = true;
    std::uint64_t initialRevision_ = 0;
    std::uint64_t startRevision_ = 0;
    std::vector<HBITMAP> images_;
    bool rootsShown_ = false;
    std::set<UINT> rootCommands_;
    std::vector<CommandReference> rootReferences_;
    std::map<UINT, HBITMAP> rootImages_;
    std::vector<std::pair<UINT, std::wstring>> rootRows_;
};
} // namespace snowdesktop::shell_extensions
