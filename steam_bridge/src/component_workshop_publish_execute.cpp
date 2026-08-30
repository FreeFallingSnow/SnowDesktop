// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "component_workshop_publish.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::steam_bridge
{
bool ExecuteComponentPublishPlan(const ComponentPublishPlan& plan,
    SteamWorkshopCore& steam,
    const ComponentPublishProgressCallback& progress,
    ComponentPublishResult& result, CoreError& error)
{
    result = {};
    error = {};
    const std::size_t submissionTotal =
        std::max<std::size_t>(1, plan.localizations.size());
    const auto publish = [&](const PublishRequest& request,
        std::size_t index, std::string language)
    {
        return steam.Publish(request,
            [&](const PublishProgress& value)
            {
                if (progress)
                    progress(ComponentPublishProgress{
                        index, submissionTotal, language, value });
            }, error);
    };

    PublishRequest primary;
    primary.package = plan.package;
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
