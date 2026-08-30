// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "package_tool.h"
#include "steam_workshop_core.h"
#include "workshop_project.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::steam_bridge
{
enum class ComponentPublishAction
{
    Create,
    UpdateContent,
    UpdateMetadata,
};

struct ComponentPublishOptions
{
    bool forceContent = false;
    std::optional<int> visibility;
    std::string changeNote;
    std::chrono::seconds timeout = std::chrono::minutes(30);
};

struct ComponentPublishPlan
{
    ComponentPublishAction action = ComponentPublishAction::Create;
    bool updateContent = true;
    std::optional<std::uint64_t> publishedFileId;
    std::filesystem::path package;
    std::string packageId;
    std::string version;
    std::string sha256;
    std::vector<SteamWorkshopLocalization> localizations;
    std::optional<std::filesystem::path> preview;
    std::optional<std::vector<std::string>> tags;
    std::string metadata;
    std::optional<int> visibility;
    std::string changeNote;
    std::chrono::seconds timeout = std::chrono::minutes(30);
};

struct ComponentPublishProgress
{
    std::size_t submissionIndex = 0;
    std::size_t submissionTotal = 0;
    std::string language;
    PublishProgress steam;
};

using ComponentPublishProgressCallback =
    std::function<void(const ComponentPublishProgress&)>;

struct ComponentPublishResult
{
    bool baseSubmitted = false;
    bool created = false;
    bool contentUploaded = false;
    std::uint64_t publishedFileId = 0;
    std::size_t localizedSubmissions = 0;
    bool needsLegalAgreement = false;
    std::string communityUrl;
    std::string failedLanguage;
};

std::string_view ComponentPublishActionName(ComponentPublishAction action);

bool BuildComponentPublishPlan(const WorkshopProject& project,
    const WidgetInspection& inspection, const PackagedWidget& package,
    const ComponentPublishOptions& options, ComponentPublishPlan& plan,
    std::string& error);

bool ExecuteComponentPublishPlan(const ComponentPublishPlan& plan,
    SteamWorkshopCore& steam,
    const ComponentPublishProgressCallback& progress,
    ComponentPublishResult& result, CoreError& error);
}
