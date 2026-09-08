// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "component_workshop_publish.h"

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
        if (preferences.manualEnglishTitle.empty())
        {
            error = "manual English text requires a title";
            return false;
        }
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

}
