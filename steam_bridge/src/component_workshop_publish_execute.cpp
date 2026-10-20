// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "component_workshop_publish.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace snowdesktop::steam_bridge
{
bool ExecuteComponentPublishPlan(const ComponentPublishPlan& plan,
    SteamWorkshopCore& steam,
    const ComponentPublishProgressCallback& progress,
    ComponentPublishResult& result, CoreError& error,
    const ComponentPublishPersistence& persistence)
{
    ComponentPublishTransport transport;
    transport.status = [&](CoreError& failure) -> std::optional<SteamStatus> {
        if (!steam.Initialize(failure)) return std::nullopt;
        return steam.Status();
    };
    transport.item = [&](std::uint64_t id, CoreError& failure) { return steam.FindItem(id, failure); };
    transport.publish = [&](const PublishRequest& request, const PublishProgressCallback& changed, CoreError& failure) {
        return steam.Publish(request, changed, failure);
    };
    return ExecuteComponentPublishPlan(plan, transport, progress, result, error, persistence);
}

bool ExecuteComponentPublishPlan(const ComponentPublishPlan& plan,
    ComponentPublishTransport& transport,
    const ComponentPublishProgressCallback& progress,
    ComponentPublishResult& result, CoreError& error,
    const ComponentPublishPersistence& persistence)
{
    result = {};
    error = {};
    if (plan.publishedFileId)
    {
        const auto status = transport.status(error);
        if (!status) return false;
        const auto item = transport.item(*plan.publishedFileId, error);
        if (!item) return false;
        std::uint64_t owner = 0;
        const auto parsed = std::from_chars(status->steamId.data(), status->steamId.data() + status->steamId.size(), owner);
        WorkshopProject project; project.packageId = plan.packageId;
        std::string detail;
        if (!status->loggedOn || parsed.ec != std::errc{} || parsed.ptr != status->steamId.data() + status->steamId.size() ||
            item->banned || !CanBindWorkshopItem(project, item->metadata, item->ownerSteamId, owner,
                item->consumerAppId, status->appId, detail))
        { error = {kInvalidArguments, "project_item_mismatch", detail.empty() ? "Workshop item identity or owner does not match this component" : detail}; return false; }
    }
    else if (!persistence.prepareCreateItem || !persistence.persistCreatedItem)
    { error = {kSteamOperationFailed, "writeFailed", "Workshop creation requires durable local ID persistence"}; return false; }
    const std::size_t submissionTotal =
        std::max<std::size_t>(1, plan.localizations.size());
    const auto publish = [&](const PublishRequest& request,
        std::size_t index, std::string language)
    {
        return transport.publish(request,
            [&](const PublishProgress& value)
            {
                if (progress)
                    progress(ComponentPublishProgress{
                        index, submissionTotal, language, value });
            }, error);
    };

    PublishRequest primary;
    primary.package = plan.package;
    primary.expectedPackageId = plan.packageId;
    primary.prepareCreateItem = persistence.prepareCreateItem;
    primary.persistCreatedItem = [&](std::uint64_t id) {
        result.created = true; result.publishedFileId = id;
        return persistence.persistCreatedItem && persistence.persistCreatedItem(id);
    };
    primary.updateContent = plan.updateContent;
    primary.preview = plan.preview;
    primary.publishedFileId = plan.publishedFileId;
    primary.metadata = plan.metadata;
    primary.tags = plan.tags;
    primary.visibility = plan.visibility;
    primary.changeNote = plan.changeNote;
    primary.timeout = plan.timeout;
    std::string primaryLanguage;
    if (!plan.localizations.empty())
    {
        const auto& localized = plan.localizations.front();
        primaryLanguage = localized.language;
        primary.language = localized.language;
        primary.title = localized.title;
        primary.description = localized.description;
    }
    const auto primaryResult = publish(primary, 1, primaryLanguage);
    if (!primaryResult)
    {
        result.failedLanguage = primaryLanguage;
        return false;
    }

    result.baseSubmitted = true;
    result.created = primaryResult->created;
    result.contentUploaded = plan.updateContent;
    result.publishedFileId = primaryResult->publishedFileId;
    result.needsLegalAgreement = primaryResult->needsLegalAgreement;
    result.communityUrl = primaryResult->communityUrl;
    result.localizedSubmissions = plan.localizations.empty() ? 0 : 1;

    for (std::size_t index = 1; index < plan.localizations.size(); ++index)
    {
        const auto& localized = plan.localizations[index];
        PublishRequest request;
        request.updateContent = false;
        request.expectedPackageId = plan.packageId;
        request.publishedFileId = result.publishedFileId;
        request.language = localized.language;
        request.title = localized.title;
        request.description = localized.description;
        request.timeout = plan.timeout;
        const auto localizedResult = publish(
            request, index + 1, localized.language);
        if (!localizedResult)
        {
            result.failedLanguage = localized.language;
            return false;
        }
        ++result.localizedSubmissions;
        result.needsLegalAgreement = result.needsLegalAgreement ||
            localizedResult->needsLegalAgreement;
    }
    return true;
}
}
