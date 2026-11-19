// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component_workshop_publish.h"
#include "package_tool.h"
#include "workshop_project.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::steam_bridge
{
struct ManagerProjectDraft;
class ManagerPreparedPreview;

class ManagerPublishPreparation
{
public:
    struct Request
    {
        WorkshopProject project;
        std::uint64_t revision;
        bool publishPublic;
    };

    class Prepared
    {
    public:
        WorkshopProject project;
        PackagedWidget artifact;
        ComponentPublishPlan plan;

    private:
        friend class ManagerPublishPreparation;
        std::uint64_t revision_ = 0;
        // Remove the frozen preview before the package owner removes its directory.
        std::shared_ptr<ManagerPreparedPreview> frozenPreview_;
    };

    enum class Completion { Ready, Stale, MissingProject, SaveFailed };

    explicit ManagerPublishPreparation(const PackageTool& packageTool)
        : packageTool_(packageTool) {}

    // The Manager serializes state access with its project lock. Prepare runs
    // on its worker and uses only the request and the package tool.
    Request Begin(const WorkshopProject& project, const ManagerProjectDraft& draft);
    std::unique_ptr<Prepared> Prepare(Request request, std::string& error) const;
    Completion Complete(std::unique_ptr<Prepared> result, std::string_view selectedLocalId,
        ProjectStore& store, std::string& error);
    void Invalidate();
    const ComponentPublishPlan* PlanFor(std::string_view localId) const;
    std::unique_ptr<const Prepared> Take(std::string_view localId);

private:
    const PackageTool& packageTool_;
    std::uint64_t revision_ = 0;
    std::unique_ptr<Prepared> prepared_;
};
}
