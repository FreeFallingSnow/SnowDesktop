// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::steam_bridge
{
enum class SkillInstallState
{
    Unavailable,
    NotInstalled,
    UpdateAvailable,
    Current,
};

enum class AgentSkillTargetKind
{
    Shared,
    Codex,
    ClaudeCode,
    Cursor,
    GitHubCopilot,
    GeminiCli,
};

struct AgentSkillTarget
{
    AgentSkillTargetKind kind = AgentSkillTargetKind::Shared;
    std::string id;
    std::filesystem::path skillsRoot;
};

struct SkillInstallStatus
{
    SkillInstallState state = SkillInstallState::Unavailable;
    AgentSkillTarget agent;
    std::filesystem::path source;
    std::filesystem::path cliSource;
    std::filesystem::path target;
    unsigned int bundledRevision = 0;
    unsigned int installedRevision = 0;
};

std::vector<AgentSkillTarget> DefaultAgentSkillTargets();
SkillInstallStatus InspectAgentSkill(
    const std::filesystem::path& bundledSkill,
    const std::filesystem::path& bundledCli,
    AgentSkillTarget target,
    std::string& error);
bool InstallOrUpdateAgentSkill(const SkillInstallStatus& status,
    std::string& error);
bool UninstallAgentSkill(const SkillInstallStatus& status,
    std::string& error);
}
