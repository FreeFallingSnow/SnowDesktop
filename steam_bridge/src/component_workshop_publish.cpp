// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "component_workshop_publish.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::steam_bridge
{
std::string_view ComponentPublishActionName(ComponentPublishAction action)
{
    switch (action)
    {
    case ComponentPublishAction::Create: return "create";
    case ComponentPublishAction::UpdateContent: return "update-content";
    case ComponentPublishAction::UpdateMetadata: return "update-metadata";
    }
    return "create";
}

bool BuildComponentPublishPlan(const WorkshopProject& project,
    const WidgetInspection& inspection, const PackagedWidget& package,
    const ComponentPublishOptions& options, ComponentPublishPlan& plan,
    std::string& error)
{
    plan = {};
    error.clear();
    if (!inspection.valid || inspection.packageId.empty() ||
        package.packagePath.empty() || package.packageId.empty() ||
        package.version.empty() || package.sha256.empty())
    {
        error = "component package has not been inspected and packed";
        return false;
    }
    if (inspection.packageId != package.packageId ||
        inspection.version != package.version)
    {
        error = "packed component identity does not match inspection";
        return false;
    }
    if (!project.packageId.empty() &&
        project.packageId != inspection.packageId)
    {
        error = "local project packageId does not match the component package";
        return false;
    }

    const bool creating = !project.publishedFileId.has_value();
    const auto& preferences = project.publishPreferences;
    if (preferences.textSource == WorkshopTextSource::Package)
    {
        plan.localizations = BuildSteamWorkshopLocalizations(
            inspection.name, inspection.description,
            inspection.localizations);
    }
    else if (preferences.textSource == WorkshopTextSource::ManualEnglish)
    {
        plan.localizations.push_back(SteamWorkshopLocalization{
            "english", preferences.manualEnglishTitle,
            preferences.manualEnglishDescription });
    }
    if (creating && preferences.textSource == WorkshopTextSource::Steam)
    {
        error = "a new Workshop item cannot preserve Steam-managed text";
        return false;
    }
    if (creating && (plan.localizations.empty() ||
            plan.localizations.front().language != "english" ||
            plan.localizations.front().title.empty()))
    {
        error = "creating a Workshop item requires an English title";
        return false;
    }

    if (creating &&
        preferences.previewSource == WorkshopAssetSource::Steam)
    {
        error = "a new Workshop item cannot preserve a Steam-managed preview";
        return false;
    }
    if (creating && preferences.tagsSource == WorkshopAssetSource::Steam)
    {
        error = "a new Workshop item cannot preserve Steam-managed tags";
        return false;
    }
    if (creating ||
        preferences.previewSource == WorkshopAssetSource::Local)
    {
        const auto preview = project.primaryPreview.empty() ?
            inspection.preview : project.primaryPreview;
        if (!preview.empty()) plan.preview = preview;
    }
    if (creating && !plan.preview)
    {
        error = "creating a Workshop item requires a primary preview";
        return false;
    }
    if (creating || preferences.tagsSource == WorkshopAssetSource::Local)
        plan.tags = project.tags;

    plan.publishedFileId = project.publishedFileId;
    plan.package = package.packagePath;
    plan.packageId = package.packageId;
    plan.version = package.version;
    plan.sha256 = package.sha256;
    plan.updateContent = creating || options.forceContent ||
        package.sha256 != project.lastPublishedSha256;
    plan.action = creating ? ComponentPublishAction::Create :
        (plan.updateContent ? ComponentPublishAction::UpdateContent :
            ComponentPublishAction::UpdateMetadata);
    plan.metadata = BuildWorkshopMetadata(package.packageId, package.version);
    plan.visibility = options.visibility;
    plan.changeNote = options.changeNote;
    plan.timeout = options.timeout;
    return true;
}

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
    const auto primaryResult = publish(
        primary, 1, primaryLanguage);
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
