#pragma once
#include "modern_menu.h"
#include "shell_extension_diagnostics.h"
#include "shell_extension_menu_cache.h"
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
    Presentation(const Request &source, Preferences prefs, std::wstring, std::wstring)
        : prefs_(std::move(prefs)), source_(source)
    {
        MenuTiming timing("first_screen");
        const auto context = ResolveContext(source);
        if (source.paths.empty() || std::none_of(prefs_.shown.begin(), prefs_.shown.end(),
                                                 [=](const auto &item) { return item.context == context; }))
            return;
        generation_ = MenuCacheGeneration();
        ticket_ = SharedMenuCache().Capture(source);
        cached_ = SharedMenuCache().Find(ticket_);
        timing.Record(cached_ ? "snapshot" : "miss");
        try
        {
            session_ = std::make_unique<Session>(source);
        }
        catch (...)
        {
            cached_.reset();
        }
    }
    ~Presentation()
    {
        // A quick dismissal must still warm the next right-click. Native menu
        // objects are released as soon as this bounded background query ends.
        if (session_ && !ready_)
            FinishQueryInBackground(std::move(session_), std::move(ticket_), generation_);
        for (auto image : images_)
            DeleteObject(image);
    }
    void Attach(std::vector<modern_menu::Item> &items, modern_menu::Options &options, UINT moreCommand)
    {
        MoveMoreToBottom(items, moreCommand);
        if (!session_)
            return;
        if (cached_)
            Insert(items, Convert(VisibleEntries(prefs_, cached_->entries, source_)), moreCommand);
        options.pollItems = [this,
                             moreCommand](const std::vector<modern_menu::Item> &current,
                                          bool canApply) -> std::optional<std::vector<modern_menu::Item>> {
            if (!session_)
                return {};
            if (!ready_)
            {
                ready_ = session_->Poll();
                if (ready_ && generation_ == MenuCacheGeneration())
                    SharedMenuCache().Store(ticket_, *ready_);
            }
            if (!ready_ || !canApply)
                return {};
            if (!ready_->ok && generation_ == MenuCacheGeneration())
                return current; // A failed refresh must not erase a valid display snapshot.
            auto result = current;
            std::erase_if(result, [](const auto &item) { return IsOurCommand(item.command); });
            if (ready_->ok && generation_ == MenuCacheGeneration())
                Insert(result, Convert(VisibleEntries(prefs_, ready_->entries, source_)), moreCommand);
            return result;
        };
    }
    bool Invoke(UINT command, POINT point)
    {
        const auto found = commands_.find(command);
        if (found == commands_.end() || !session_ || generation_ != MenuCacheGeneration())
            return false;
        try
        {
            if (!ready_)
                ready_ = session_->Poll();
            if (!ready_)
                return InvokeWhenReady(std::move(session_), found->second, point, generation_);
            if (!ready_->ok)
            {
                // Retry only an explicit cached click, always against fresh
                // command identities. A retained snapshot never executes tokens.
                session_.reset();
                session_ = std::make_unique<Session>(source_);
                return InvokeWhenReady(std::move(session_), found->second, point, generation_);
            }
            SharedMenuCache().Store(ticket_, *ready_);
            const auto token = ResolveCommand(*ready_, found->second);
            if (!token)
                return false;
            session_->Invoke(token, point);
            return true;
        }
        catch (...)
        {
            return false;
        }
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
    MenuSnapshotCache::Ticket ticket_;
    std::uint64_t generation_ = 0;
    UINT nextCommand_ = FirstCommand;
    std::map<UINT, CommandReference> commands_;
    std::set<UINT> converting_;
    std::optional<Reply> cached_;
    std::unique_ptr<Session> session_;
    std::optional<Reply> ready_;
    std::vector<HBITMAP> images_;
};
} // namespace snowdesktop::shell_extensions
