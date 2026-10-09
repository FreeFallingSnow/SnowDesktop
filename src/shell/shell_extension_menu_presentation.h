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
        : prefs_(std::move(prefs)), source_(source), service_(service), completed_(std::move(completed))
    {
        if (source.paths.empty() || !service_.MenuEnabled(source_, prefs_)) return;
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
        startLane_ = startShown && !source.background && source.context != Context::Desktop && source.paths.size() == 1;
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
        normalDone_ = !normalNeeded || (cached_.has_value() && !refreshState_);
        // Retire any prewarm already in flight as well: it may have captured
        // external pin state before this opening.
        if (normalNeeded)
        {
            if (refreshState_) service_.Invalidate(source_);
            service_.Query(source_, QueryPriority::Menu, refreshState_);
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
        if (cached_) Insert(items, Convert(cached_->entries), moreCommand);
        if ((!normalDone_ || !startDone_) && !source_.paths.empty())
        {
            options.pollItemsFinished = [this] { return normalDone_ && startDone_; };
            options.pollItemsStablePrefix = true;
            options.pollItems = [this, moreCommand](const auto &current, bool canApply) -> std::optional<std::vector<modern_menu::Item>> {
                if (!canApply) return {};
                auto updated = current;
                bool progressed = false;
                if (!normalDone_)
                {
                    auto view = service_.MenuDisplay(source_, prefs_);
                    if (!view.pending && !view.snapshot && !view.error.empty() && !normalRetried_)
                    {
                        // A failed prewarm may still be in backoff, or the helper
                        // may have exited during this opening. Recover once with
                        // a fresh query while this popup remains subscribed.
                        normalRetried_ = true;
                        service_.Query(source_, QueryPriority::Menu, true);
                    }
                    else if (!view.pending)
                    {
                        if (view.snapshot && (!refreshState_ || (view.revision > initialRevision_ && view.error.empty())))
                        {
                            auto additions = std::move(view.snapshot->entries);
                            const bool warm = cached_.has_value();
                            std::erase_if(additions, [&](const auto &e) {
                                const auto *pair = StatePairForVerb(e.key);
                                return (startLane_ && pair && pair->id == "state:start-pin") ||
                                    (warm && refreshState_ && !pair);
                            });
                            Insert(updated, Convert(additions), moreCommand);
                        }
                        normalDone_ = progressed = true;
                    }
                }
                if (!startDone_)
                {
                    auto view = service_.MenuDisplay(startSource_, prefs_);
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
        service_.Execute(startLane_ && pair && pair->id == "state:start-pin" ? startSource_ : source_,
            found->second, point, completed_, owner);
        return true;
    }

  private:
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
    std::vector<modern_menu::Item> Convert(const std::vector<Entry> &entries,
                                           const CommandReference &parent = {})
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
            converting_.insert(item.command);
            item.label = e.label;
            item.accessKey = e.accessKey;
            item.enabled = e.enabled;
            item.checked = e.checked;
            item.separator = e.separator;
            item.children = Convert(e.children, reference);
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
    MenuService &service_;
    std::function<void(bool)> completed_;
    unsigned contexts_ = 0;
    UINT nextCommand_ = FirstCommand;
    std::map<UINT, CommandReference> commands_;
    std::set<UINT> converting_;
    std::optional<Reply> cached_;
    bool refreshState_ = false;
    bool normalRetried_ = false;
    bool startLane_ = false, normalDone_ = true, startDone_ = true;
    std::uint64_t initialRevision_ = 0;
    std::uint64_t startRevision_ = 0;
    std::vector<HBITMAP> images_;
};
} // namespace snowdesktop::shell_extensions
