#pragma once

#include "settings_ipc_channel.h"
#include "settings_ipc_values.h"
#include <iterator>
#include <limits>

namespace snowdesktop::settings_ipc
{
// Each operation runs once. Subsequent pages read that immutable result, so an
// import/delete is never repeated and concurrent refreshes cannot mix revisions.
using IconLibraryPage = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, IconLibrarySnapshot>;
class IconLibraryPages
{
public:
    IconLibraryPage Begin(IconLibrarySnapshot snapshot)
    {
        if (snapshot.entries.size() > (std::numeric_limits<std::uint32_t>::max)())
            throw ProtocolError("icon library exceeds paging capacity");
        snapshot_ = std::move(snapshot);
        ++token_;
        return Read(token_, 0);
    }
    IconLibraryPage Read(std::uint64_t token, std::uint32_t offset) const
    {
        if (!token || token != token_ || offset > snapshot_.entries.size())
            throw ProtocolError("stale icon library page");
        IconLibrarySnapshot page;
        page.revision = snapshot_.revision;
        if (!offset)
        {
            page.groups = snapshot_.groups;
            page.imported = snapshot_.imported;
            page.importedGroupId = snapshot_.importedGroupId;
            page.failures = snapshot_.failures;
            page.session = snapshot_.session;
            page.busy = snapshot_.busy;
            page.succeeded = snapshot_.succeeded;
            page.error = snapshot_.error;
            page.target = snapshot_.target;
            page.targetName = snapshot_.targetName;
            page.workshopAvailable=snapshot_.workshopAvailable;page.coverPath=snapshot_.coverPath;
            page.preparationId=snapshot_.preparationId;page.publishedUrl=snapshot_.publishedUrl;
            page.completedAction=snapshot_.completedAction;page.operationId=snapshot_.operationId;
            page.progress=snapshot_.progress;
        }
        std::size_t bytes = 0;
        for (std::size_t i = offset; i < snapshot_.entries.size() && page.entries.size() < 1024; ++i)
        {
            const auto size = Pack(snapshot_.entries[i]).size();
            if (!page.entries.empty() && bytes + size > 2 * 1024 * 1024) break;
            page.entries.push_back(snapshot_.entries[i]);
            bytes += size;
        }
        return {token_, static_cast<std::uint32_t>(snapshot_.entries.size()), offset, std::move(page)};
    }
    void Release(std::uint64_t token)
    {
        if (token == token_) { snapshot_ = {}; ++token_; }
    }
private:
    IconLibrarySnapshot snapshot_;
    std::uint64_t token_ = 0;
};

inline void BindIconLibraryPages(Channel& channel, IconLibraryAction action)
{
    const auto pages = std::make_shared<IconLibraryPages>();
    channel.Bind<IconLibraryPage, IconLibraryRequest>("iconLibrary.action", [pages, action = std::move(action)](auto request) {
        return pages->Begin(action ? action(std::move(request)) : IconLibrarySnapshot{});
    });
    channel.Bind<IconLibraryPage, std::uint64_t, std::uint32_t>("iconLibrary.page", [pages](auto token, auto offset) {
        return pages->Read(token, offset);
    });
    channel.Bind<void, std::uint64_t>("iconLibrary.release", [pages](auto token) { pages->Release(token); });
}

inline IconLibrarySnapshot CallIconLibraryPages(Channel& channel, const IconLibraryRequest& request)
{
    auto [token, total, offset, result] = channel.Call<IconLibraryPage>("iconLibrary.action", request);
    struct Release
    {
        Channel& channel;
        std::uint64_t token;
        ~Release() { try { channel.Notify("iconLibrary.release", token); } catch (...) {} }
    } release{channel, token};
    if (!token || offset || result.entries.size() > total)
        throw ProtocolError("invalid first icon library page");
    while (result.entries.size() < total)
    {
        const auto next = static_cast<std::uint32_t>(result.entries.size());
        auto [pageToken, pageTotal, pageOffset, page] = channel.Call<IconLibraryPage>("iconLibrary.page", token, next);
        if (pageToken != token || pageTotal != total || pageOffset != next || page.revision != result.revision ||
            page.entries.empty() || page.entries.size() > total - result.entries.size())
            throw ProtocolError("inconsistent icon library page");
        result.entries.insert(result.entries.end(), std::make_move_iterator(page.entries.begin()), std::make_move_iterator(page.entries.end()));
    }
    return result;
}
}
