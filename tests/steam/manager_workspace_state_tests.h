// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../../steam_bridge/src/manager_workspace_state.h"

#include <stdexcept>

inline void TestManagerWorkspaceState()
{
    using namespace snowdesktop::steam_bridge;
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto page = [](std::uint32_t number, std::uint32_t pages,
        std::uint32_t total, std::initializer_list<std::uint64_t> ids) {
        PublishedPage result;
        result.page = number;
        result.totalPages = pages;
        result.totalResults = total;
        for (const auto id : ids)
        {
            PublishedItem item;
            item.publishedFileId = id;
            item.title = std::to_string(id);
            result.items.push_back(std::move(item));
        }
        return result;
    };

    ManagerPublishedPages published;
    require(!published.Snapshot().loaded && !published.Snapshot().CanPrevious() &&
        !published.Snapshot().CanNext(), "unloaded pages expose no usable navigation");

    auto request = published.BeginRequest("author-a", 1);
    require(published.Snapshot().loading && published.Snapshot().page == 1,
        "starting a query does not announce an uncommitted page");
    require(published.Apply(request, "author-a", page(1, 3, 120, {101, 102})).kind ==
        ManagerPublishedPageApplyKind::Applied &&
        published.Snapshot().selectedId == 101 &&
        !published.Snapshot().CanPrevious() && published.Snapshot().CanNext(),
        "the first real result selects its first item and enables only valid navigation");
    require(published.Select(102), "a visible item can be selected");
    request = published.BeginRequest("author-a", 1);
    published.Apply(request, "author-a", page(1, 3, 120, {102, 103}));
    require(published.Snapshot().selectedId == 102,
        "a refresh preserves selection when the selected item remains present");
    request = published.BeginRequest("author-a", 1);
    published.Apply(request, "author-a", page(1, 3, 120, {103, 104}));
    require(published.Snapshot().selectedId == 103 && !published.Select(999),
        "a removed selection falls back to an existing item and cannot point outside the result");

    request = published.BeginRequest("author-a", 2);
    require(published.Snapshot().page == 1 && published.Snapshot().selectedId == 103 &&
        !published.Snapshot().CanNext(), "a pending page keeps the previous committed view");
    require(published.Fail(request, "author-a",
        CoreError{kSteamOperationTimedOut, "query_timed_out", "network timeout"}) &&
        published.Snapshot().page == 1 && published.Snapshot().selectedId == 103 &&
        published.Snapshot().items.front().publishedFileId == 103 &&
        published.Snapshot().error->code == "query_timed_out",
        "query failure retains the old page and selection with an explicit error");

    const auto superseded = published.BeginRequest("author-a", 2);
    const auto current = published.BeginRequest("author-a", 3);
    require(published.Apply(superseded, "author-a", page(2, 3, 120, {201})).kind ==
        ManagerPublishedPageApplyKind::Ignored && published.Snapshot().loading &&
        published.Snapshot().requestedPage == 3,
        "a late old request cannot replace the current request or stop its loading state");
    const auto shrunk = published.Apply(current, "author-a", page(3, 2, 80, {}));
    require(shrunk.kind == ManagerPublishedPageApplyKind::RetryAtPage &&
        shrunk.retryPage == 2 && published.Snapshot().page == 1 &&
        published.Snapshot().selectedId == 103,
        "a removed last page requests the actual last page instead of committing page three of two");
    request = published.BeginRequest("author-a", shrunk.retryPage);
    published.Apply(request, "author-a", page(2, 2, 80, {201}));
    require(published.Snapshot().page == 2 && published.Snapshot().selectedId == 201 &&
        published.Snapshot().CanPrevious() && !published.Snapshot().CanNext(),
        "the retry commits the valid page and its own selection");

    request = published.BeginRequest("author-a", 2);
    published.Apply(request, "author-a", page(2, 0, 0, {}));
    require(published.Snapshot().page == 1 && published.Snapshot().totalPages == 0 &&
        published.Snapshot().loaded && published.Snapshot().items.empty() &&
        published.Snapshot().selectedId == 0 && !published.Snapshot().CanPrevious() &&
        !published.Snapshot().CanNext(),
        "an empty author library is a loaded empty state with no impossible navigation");

    request = published.BeginRequest("author-a", 1);
    require(published.Apply(request, "author-b", page(1, 1, 1, {111})).kind ==
        ManagerPublishedPageApplyKind::Ignored &&
        published.Snapshot().account == "author-b" &&
        published.Snapshot().items.empty() && !published.Snapshot().loading &&
        !published.Snapshot().loaded && published.Snapshot().page == 1,
        "a fresh account observation invalidates the old account query and all remote view state");
    request = published.BeginRequest("author-b", 1);
    published.Apply(request, "author-b", page(1, 1, 1, {222}));
    require(published.Snapshot().selectedId == 222 &&
        !published.Fail(request, "author-b",
            CoreError{kSteamOperationFailed, "late_failure", "old callback"}),
        "a terminal callback cannot overwrite a successfully committed result");

    request = published.BeginRequest("author-b", 1);
    require(published.Apply(request, "author-b", page(2, 2, 60, {333})).kind ==
        ManagerPublishedPageApplyKind::InvalidResponse &&
        published.Snapshot().selectedId == 222 && published.Snapshot().error.has_value(),
        "an inconsistent response cannot corrupt the committed page");

    WorkshopProject first;
    first.localId = "project-a";
    first.tags = {"Widget", "Clock"};
    first.publishPreferences.manualEnglishTitle = "Saved title";
    first.publishPreferences.manualEnglishDescription = "Saved description";
    WorkshopProject second;
    second.localId = "project-b";
    second.publishPreferences.manualEnglishTitle = "Second title";

    ManagerProjectDrafts drafts;
    auto& firstDraft = drafts.ForProject(first, "Widget, Clock");
    require(firstDraft.EditTags("") && firstDraft.EditTitle("Typed title") &&
        firstDraft.EditDescription("Typed description") &&
        firstDraft.SetPublishPublic(false),
        "real input events update the first project draft including an intentionally empty tag field");
    const auto editedRevision = firstDraft.revision;
    auto& secondDraft = drafts.ForProject(second, "");
    require(secondDraft.manualEnglishTitle == "Second title" &&
        secondDraft.publishPublic && secondDraft.tagsText.empty(),
        "switching to another project initializes that project's independent saved values");
    secondDraft.EditTitle("Edited second title");

    auto& restored = drafts.ForProject(first, "Widget, Clock");
    require(restored.tagsText.empty() && restored.manualEnglishTitle == "Typed title" &&
        restored.manualEnglishDescription == "Typed description" &&
        !restored.publishPublic && restored.revision == editedRevision &&
        restored.tagsDirty && restored.preferencesDirty && restored.visibilityDirty,
        "switching away before blur and back preserves empty tags, manual text and private visibility");
    for (int frame = 0; frame != 3; ++frame)
        require(drafts.ForProject(first, "Widget, Clock").tagsText.empty(),
            "render-frame access never reloads saved nonempty tags over empty input");
    require(!restored.EditTitle("Typed title") && restored.revision == editedRevision,
        "unchanged frame values do not invalidate a prepared plan revision");

    restored.SetReusePackageText(false);
    restored.ApplyPreferences(first);
    require(first.publishPreferences.textSource == WorkshopTextSource::ManualEnglish &&
        first.publishPreferences.manualEnglishTitle == "Typed title" &&
        first.publishPreferences.manualEnglishDescription == "Typed description",
        "the same production draft writes the preferences used by a new item's publish planner");
    restored.ApplyPreferences(second);
    require(second.publishPreferences.manualEnglishTitle == "Second title",
        "applying a draft cannot overwrite another local project's preferences");

    first.publishedFileId = 700;
    require(drafts.ForProject(first, "Widget, Clock").manualEnglishTitle == "Typed title" &&
        !drafts.ForProject(first, "Widget, Clock").publishPublic,
        "publication completion does not reinitialize an existing draft");
    restored.ApplyPreferences(first);
    require(first.publishPreferences.textSource == WorkshopTextSource::Steam,
        "an existing item with package text disabled preserves Steam text at the planner boundary");

    drafts.Erase(first.localId);
    require(drafts.ForProject(first, "Widget, Clock").tagsText == "Widget, Clock" &&
        drafts.ForProject(first, "Widget, Clock").publishPublic,
        "explicitly removing a draft is the only operation that reseeds its saved values");
}
