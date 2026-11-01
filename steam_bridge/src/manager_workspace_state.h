// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "steam_workshop_core.h"
#include "workshop_project.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::steam_bridge
{
struct ManagerPublishedPageRequest
{
    std::uint64_t revision = 0;
    std::string account;
    std::uint32_t page = 1;
};

enum class ManagerPublishedPageApplyKind
{
    Ignored,
    Applied,
    RetryAtPage,
    InvalidResponse,
};

struct ManagerPublishedPageApply
{
    ManagerPublishedPageApplyKind kind = ManagerPublishedPageApplyKind::Ignored;
    std::uint32_t retryPage = 0;
};

struct ManagerPublishedPageSnapshot
{
    std::string account;
    std::vector<PublishedItem> items;
    std::uint32_t page = 1;
    std::uint32_t requestedPage = 1;
    std::uint32_t totalPages = 0;
    std::uint32_t totalResults = 0;
    std::uint64_t selectedId = 0;
    bool loading = false;
    bool loaded = false;
    std::optional<CoreError> error;

    bool CanPrevious() const
    {
        return !loading && loaded && totalPages != 0 && page > 1;
    }
    bool CanNext() const
    {
        return !loading && loaded && page < totalPages;
    }
};

// Call under the manager mutex. A request is bound to both its account and
// revision; the currentAccount supplied on completion must be sampled AFTER
// the actual Steam query, not copied from the request.
class ManagerPublishedPages
{
public:
    void ResetForAccount(std::string account)
    {
        if (snapshot_.account == account) return;
        snapshot_ = {};
        snapshot_.account = std::move(account);
        ++revision_;
    }

    ManagerPublishedPageRequest BeginRequest(std::string account,
        std::uint32_t page)
    {
        ResetForAccount(std::move(account));
        snapshot_.loading = true;
        snapshot_.requestedPage = std::max(1u, page);
        snapshot_.error.reset();
        return {++revision_, snapshot_.account, snapshot_.requestedPage};
    }

    ManagerPublishedPageApply Apply(const ManagerPublishedPageRequest& request,
        std::string_view currentAccount, PublishedPage result)
    {
        ObserveAccount(currentAccount);
        if (!IsCurrent(request)) return {};
        snapshot_.loading = false;
        if (result.page != request.page ||
            (result.totalResults != 0 && result.totalPages == 0))
        {
            snapshot_.error = CoreError{kSteamOperationFailed,
                "invalid_published_page", "Steam returned an inconsistent published page"};
            return {ManagerPublishedPageApplyKind::InvalidResponse, 0};
        }
        // Steam can shrink the page count while a user is on the last page.
        // Keep the committed view until the caller fetches this valid page.
        if (result.totalResults != 0 && request.page > result.totalPages)
            return {ManagerPublishedPageApplyKind::RetryAtPage, result.totalPages};

        snapshot_.page = result.totalResults == 0 ? 1u : result.page;
        snapshot_.requestedPage = snapshot_.page;
        snapshot_.totalPages = result.totalResults == 0 ? 0u : result.totalPages;
        snapshot_.totalResults = result.totalResults;
        snapshot_.items = std::move(result.items);
        if (result.totalResults == 0) snapshot_.items.clear();
        const bool selectionPresent = std::any_of(snapshot_.items.begin(),
            snapshot_.items.end(), [&](const PublishedItem& item) {
                return item.publishedFileId == snapshot_.selectedId;
            });
        if (!selectionPresent)
            snapshot_.selectedId = snapshot_.items.empty() ? 0 :
                snapshot_.items.front().publishedFileId;
        snapshot_.loaded = true;
        snapshot_.error.reset();
        return {ManagerPublishedPageApplyKind::Applied, 0};
    }

    bool Fail(const ManagerPublishedPageRequest& request,
        std::string_view currentAccount, CoreError error)
    {
        ObserveAccount(currentAccount);
        if (!IsCurrent(request)) return false;
        snapshot_.loading = false;
        snapshot_.error = std::move(error);
        // page/items/selection still identify the last committed result.
        return true;
    }

    bool Select(std::uint64_t id)
    {
        if (std::none_of(snapshot_.items.begin(), snapshot_.items.end(),
                [id](const PublishedItem& item) { return item.publishedFileId == id; }))
            return false;
        snapshot_.selectedId = id;
        return true;
    }

    const ManagerPublishedPageSnapshot& Snapshot() const { return snapshot_; }

private:
    void ObserveAccount(std::string_view account)
    {
        if (snapshot_.account != account) ResetForAccount(std::string(account));
    }

    bool IsCurrent(const ManagerPublishedPageRequest& request) const
    {
        return snapshot_.loading && !request.account.empty() &&
            request.revision == revision_ && request.account == snapshot_.account &&
            request.page == snapshot_.requestedPage;
    }

    ManagerPublishedPageSnapshot snapshot_;
    std::uint64_t revision_ = 0;
};

struct ManagerProjectDraft
{
    std::string localId;
    std::string tagsText;
    std::string manualEnglishTitle;
    std::string manualEnglishDescription;
    bool publishPublic = true;
    bool reusePackageText = true;
    bool updatePreview = true;
    bool updateTags = true;
    bool tagsDirty = false;
    bool preferencesDirty = false;
    bool visibilityDirty = false;
    std::uint64_t revision = 0;

    bool EditTags(std::string text)
    {
        if (!Assign(tagsText, std::move(text))) return false;
        tagsDirty = true;
        return true;
    }
    bool EditTitle(std::string text)
    {
        if (!Assign(manualEnglishTitle, std::move(text))) return false;
        preferencesDirty = true;
        return true;
    }
    bool EditDescription(std::string text)
    {
        if (!Assign(manualEnglishDescription, std::move(text))) return false;
        preferencesDirty = true;
        return true;
    }
    bool SetPublishPublic(bool value)
    {
        if (!Assign(publishPublic, value)) return false;
        visibilityDirty = true;
        return true;
    }
    bool SetReusePackageText(bool value)
    {
        if (!Assign(reusePackageText, value)) return false;
        preferencesDirty = true;
        return true;
    }
    bool SetUpdatePreview(bool value)
    {
        if (!Assign(updatePreview, value)) return false;
        preferencesDirty = true;
        return true;
    }
    bool SetUpdateTags(bool value)
    {
        if (!Assign(updateTags, value)) return false;
        preferencesDirty = true;
        return true;
    }

    void ApplyPreferences(WorkshopProject& project) const
    {
        if (project.localId != localId) return;
        project.publishPreferences.textSource = reusePackageText ?
            WorkshopTextSource::Package : project.publishedFileId ?
                WorkshopTextSource::Steam : WorkshopTextSource::ManualEnglish;
        project.publishPreferences.previewSource = updatePreview ?
            WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
        project.publishPreferences.tagsSource = updateTags ?
            WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
        project.publishPreferences.manualEnglishTitle = manualEnglishTitle;
        project.publishPreferences.manualEnglishDescription = manualEnglishDescription;
    }

    void PreferencesSaved() { preferencesDirty = false; }
    void TagsSaved() { tagsDirty = false; }

private:
    template<typename Value>
    bool Assign(Value& destination, Value value)
    {
        if (destination == value) return false;
        destination = std::move(value);
        ++revision;
        return true;
    }
};

// Seed once from the persisted project. Empty input is a valid edited draft,
// never a reason to reload persisted tags or titles during a render frame.
// The manager keeps using its existing JoinTags/SplitTags for tag syntax.
class ManagerProjectDrafts
{
public:
    ManagerProjectDraft& ForProject(const WorkshopProject& project,
        std::string initialTagsText)
    {
        auto [found, inserted] = drafts_.try_emplace(project.localId);
        if (inserted)
        {
            auto& draft = found->second;
            draft.localId = project.localId;
            draft.tagsText = std::move(initialTagsText);
            draft.manualEnglishTitle = project.publishPreferences.manualEnglishTitle;
            draft.manualEnglishDescription = project.publishPreferences.manualEnglishDescription;
            draft.reusePackageText = project.publishPreferences.textSource == WorkshopTextSource::Package;
            draft.updatePreview = project.publishPreferences.previewSource == WorkshopAssetSource::Local;
            draft.updateTags = project.publishPreferences.tagsSource == WorkshopAssetSource::Local;
        }
        return found->second;
    }

    void Erase(std::string_view localId) { drafts_.erase(std::string(localId)); }

private:
    std::map<std::string, ManagerProjectDraft, std::less<>> drafts_;
};
}
