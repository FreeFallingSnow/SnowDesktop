// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include "manager_publish_preparation.h"

#include "manager_prepared_preview.h"
#include "manager_workspace_state.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::steam_bridge
{
ManagerPublishPreparation::Request ManagerPublishPreparation::Begin(
    const WorkshopProject& project, const ManagerProjectDraft& draft)
{
    Invalidate();
    Request request{project, revision_, draft.publishPublic};
    draft.ApplyPreferences(request.project);
    request.project.publishPreferences.tagsSource = project.publishedFileId ?
        WorkshopAssetSource::Steam : WorkshopAssetSource::Local;
    return request;
}

std::unique_ptr<ManagerPublishPreparation::Prepared> ManagerPublishPreparation::Prepare(
    Request request, std::string& error) const
{
    auto result = std::make_unique<Prepared>();
    result->project = std::move(request.project);
    result->revision_ = request.revision;
    auto& project = result->project;
    WidgetInspection inspection;
    if (!packageTool_.Inspect(project.sourceDirectory, inspection, error) ||
        !packageTool_.Pack(project.sourceDirectory, inspection, result->artifact, error,
            packageTool_.FindPublishedBaseline(project, inspection.version)))
        return {};

    ComponentPublishOptions options;
    if (!project.publishedFileId) options.visibility = request.publishPublic ? 0 : 2;
    if (!BuildComponentPublishPlan(project, inspection, result->artifact, options,
            result->plan, error))
        return {};

    auto& plan = result->plan;
    if (plan.preview && !plan.preparedPreviews)
    {
        result->frozenPreview_ = FreezeManagerPreview(*plan.preview,
            result->artifact.temporaryDirectory, error);
        if (!result->frozenPreview_) return {};
        plan.preview = result->frozenPreview_->Path();
    }
    project.packageId = std::move(inspection.packageId);
    return result;
}

ManagerPublishPreparation::Completion ManagerPublishPreparation::Complete(
    std::unique_ptr<Prepared> result, std::string_view selectedLocalId,
    ProjectStore& store, std::string& error)
{
    const auto& snapshot = result->project;
    if (revision_ != result->revision_ || selectedLocalId != snapshot.localId)
        return Completion::Stale;

    auto& projects = store.Projects();
    const auto project = std::ranges::find(projects, snapshot.localId, &WorkshopProject::localId);
    if (project == projects.end()) return Completion::MissingProject;
    project->packageId = snapshot.packageId;
    project->publishPreferences = snapshot.publishPreferences;
    if (!store.Save(error)) return Completion::SaveFailed;
    prepared_ = std::move(result);
    return Completion::Ready;
}

void ManagerPublishPreparation::Invalidate()
{
    ++revision_;
    prepared_.reset();
}

const ComponentPublishPlan* ManagerPublishPreparation::PlanFor(std::string_view localId) const
{
    return prepared_ && prepared_->project.localId == localId ? &prepared_->plan : nullptr;
}

std::unique_ptr<const ManagerPublishPreparation::Prepared>
ManagerPublishPreparation::Take(std::string_view localId)
{
    if (!PlanFor(localId)) return {};
    return std::move(prepared_);
}
}
