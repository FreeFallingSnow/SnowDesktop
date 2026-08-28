// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "authoring_toolchain.h"
#include "bridge_json.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace snowdesktop::steam_bridge
{
namespace
{
constexpr wchar_t kSkillDirectoryName[] = L"snowdesktop-lua-widget";

std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required <= 1) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        name, value.data(), required);
    if (length == 0 || length >= required) return {};
    value.resize(length);
    return std::filesystem::path(value);
}

std::filesystem::path UserProfilePath()
{
    PWSTR profile = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile,
            KF_FLAG_DEFAULT, nullptr, &profile)))
        return {};
    const std::filesystem::path result(profile);
    CoTaskMemFree(profile);
    return result;
}

std::wstring ComparablePath(const std::filesystem::path& path)
{
    std::wstring value = path.lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return std::towlower(character); });
    return value;
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsRegularNonReparseFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool ContainsExistingReparsePoint(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path())
    {
        current /= component;
        if (!std::filesystem::exists(current, error))
        {
            error.clear();
            continue;
        }
        if (error || IsReparsePoint(current)) return true;
    }
    return false;
}

bool SkillTreeIsSafe(const std::filesystem::path& path,
    std::string& error)
{
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(path, filesystemError) ||
        filesystemError || ContainsExistingReparsePoint(path))
    {
        error = "the Agent Skill destination is unsafe or is not a directory";
        return false;
    }
    for (std::filesystem::recursive_directory_iterator iterator(path,
             std::filesystem::directory_options::skip_permission_denied,
             filesystemError), end;
         !filesystemError && iterator != end; iterator.increment(filesystemError))
    {
        if (iterator->is_symlink(filesystemError) ||
            IsReparsePoint(iterator->path()))
        {
            error = "the Agent Skill destination contains a link or reparse point";
            return false;
        }
    }
    if (filesystemError)
    {
        error = "cannot inspect the Agent Skill destination";
        return false;
    }
    return true;
}

std::optional<unsigned int> ReadSkillRevision(
    const std::filesystem::path& directory, std::string& error)
{
    const auto manifest = directory / L"skill.json";
    if (!std::filesystem::is_regular_file(manifest)) return std::nullopt;
    std::ifstream input(manifest, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    JsonValue root;
    if (!input || !ParseJson(text, root, error) || !root.IsObject())
    {
        if (error.empty()) error = "skill.json must be a JSON object";
        return std::nullopt;
    }
    const auto schema = JsonUnsigned(root, "schemaVersion");
    const auto id = JsonString(root, "id");
    const auto revision = JsonUnsigned(root, "revision");
    if (!schema || *schema != 1 || !id || *id != "snowdesktop-lua-widget" ||
        !revision || *revision == 0 ||
        *revision > static_cast<std::uint64_t>(UINT_MAX))
    {
        error = "skill.json contains an unsupported identity or revision";
        return std::nullopt;
    }
    return static_cast<unsigned int>(*revision);
}

bool CopySkillTree(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error)
{
    std::error_code filesystemError;
    std::filesystem::create_directory(destination, filesystemError);
    if (filesystemError)
    {
        error = "cannot create the temporary Skill directory";
        return false;
    }
    for (std::filesystem::recursive_directory_iterator iterator(source,
             std::filesystem::directory_options::skip_permission_denied,
             filesystemError), end;
         !filesystemError && iterator != end; iterator.increment(filesystemError))
    {
        const auto relative = std::filesystem::relative(
            iterator->path(), source, filesystemError);
        if (filesystemError || relative.empty()) break;
        const auto target = destination / relative;
        if (IsReparsePoint(iterator->path()) || iterator->is_symlink(filesystemError))
        {
            error = "the bundled Skill cannot contain links or reparse points";
            return false;
        }
        if (iterator->is_directory(filesystemError))
            std::filesystem::create_directories(target, filesystemError);
        else if (iterator->is_regular_file(filesystemError))
        {
            std::filesystem::create_directories(
                target.parent_path(), filesystemError);
            if (!filesystemError)
                std::filesystem::copy_file(iterator->path(), target,
                    std::filesystem::copy_options::none, filesystemError);
        }
        else
        {
            error = "the bundled Skill contains an unsupported file type";
            return false;
        }
        if (filesystemError) break;
    }
    if (filesystemError)
    {
        error = "cannot copy the bundled Skill: " + filesystemError.message();
        return false;
    }
    return true;
}
}

std::vector<AgentSkillTarget> DefaultAgentSkillTargets()
{
    const auto profile = UserProfilePath();
    if (profile.empty()) return {};

    auto codexHome = EnvironmentPath(L"CODEX_HOME");
    if (codexHome.empty()) codexHome = profile / L".codex";
    auto claudeHome = EnvironmentPath(L"CLAUDE_CONFIG_DIR");
    if (claudeHome.empty()) claudeHome = profile / L".claude";

    std::vector<AgentSkillTarget> candidates = {
        { AgentSkillTargetKind::Shared, "shared",
            profile / L".agents" / L"skills" },
        { AgentSkillTargetKind::Codex, "codex", codexHome / L"skills" },
        { AgentSkillTargetKind::ClaudeCode, "claude-code",
            claudeHome / L"skills" },
        { AgentSkillTargetKind::Cursor, "cursor",
            profile / L".cursor" / L"skills" },
        { AgentSkillTargetKind::GitHubCopilot, "github-copilot",
            profile / L".copilot" / L"skills" },
        { AgentSkillTargetKind::GeminiCli, "gemini-cli",
            profile / L".gemini" / L"skills" },
    };

    std::vector<AgentSkillTarget> result;
    for (auto& candidate : candidates)
    {
        if (candidate.skillsRoot.empty()) continue;
        const auto comparable = ComparablePath(candidate.skillsRoot);
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const AgentSkillTarget& existing) {
                return ComparablePath(existing.skillsRoot) == comparable;
            });
        if (!duplicate) result.push_back(std::move(candidate));
    }
    return result;
}

SkillInstallStatus InspectAgentSkill(
    const std::filesystem::path& bundledSkill,
    const std::filesystem::path& bundledCli,
    AgentSkillTarget target, std::string& error)
{
    SkillInstallStatus status;
    status.agent = std::move(target);
    status.source = bundledSkill;
    status.cliSource = bundledCli;
    status.target = status.agent.skillsRoot / kSkillDirectoryName;
    error.clear();
    std::error_code filesystemError;
    if (bundledSkill.empty() || bundledCli.empty() ||
        status.agent.skillsRoot.empty() ||
        !std::filesystem::is_directory(bundledSkill, filesystemError) ||
        filesystemError || ContainsExistingReparsePoint(bundledSkill) ||
        !IsRegularNonReparseFile(bundledCli))
    {
        error = "the bundled SnowDesktop Skill or CLI is unavailable";
        return status;
    }
    const auto bundledRevision = ReadSkillRevision(bundledSkill, error);
    if (!bundledRevision)
    {
        if (error.empty()) error = "the bundled Skill manifest is missing";
        return status;
    }
    status.bundledRevision = *bundledRevision;

    if (!std::filesystem::exists(status.target, filesystemError))
    {
        if (filesystemError)
        {
            error = "cannot inspect the Agent Skill destination";
            return status;
        }
        status.state = SkillInstallState::NotInstalled;
        return status;
    }
    if (!std::filesystem::is_directory(status.target, filesystemError) ||
        filesystemError || ContainsExistingReparsePoint(status.target))
    {
        error = "the Agent Skill destination is unsafe or is not a directory";
        return status;
    }
    std::string installedError;
    const auto installedRevision =
        ReadSkillRevision(status.target, installedError);
    if (installedRevision) status.installedRevision = *installedRevision;
    status.state = installedRevision &&
        *installedRevision >= *bundledRevision
        ? SkillInstallState::Current : SkillInstallState::UpdateAvailable;
    return status;
}

bool InstallOrUpdateAgentSkill(const SkillInstallStatus& status,
    std::string& error)
{
    error.clear();
    if (status.state == SkillInstallState::Unavailable ||
        status.source.empty() || status.target.empty())
    {
        error = "the SnowDesktop Skill cannot be installed from this build";
        return false;
    }
    const auto skillsRoot = status.target.parent_path();
    std::error_code filesystemError;
    std::filesystem::create_directories(skillsRoot, filesystemError);
    if (filesystemError || ContainsExistingReparsePoint(skillsRoot))
    {
        error = "the Agent Skill root is unavailable or unsafe";
        return false;
    }
    if (std::filesystem::exists(status.target, filesystemError) &&
        (filesystemError || IsReparsePoint(status.target)))
    {
        error = "the existing Agent Skill cannot be a reparse point";
        return false;
    }

    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto staging = skillsRoot /
        (std::wstring(L".snowdesktop-lua-widget.install-") + suffix);
    const auto backup = skillsRoot /
        (std::wstring(L".snowdesktop-lua-widget.backup-") + suffix);
    if (std::filesystem::exists(staging, filesystemError) ||
        std::filesystem::exists(backup, filesystemError))
    {
        error = "a conflicting Skill installation transaction already exists";
        return false;
    }
    if (!CopySkillTree(status.source, staging, error))
    {
        std::filesystem::remove_all(staging, filesystemError);
        return false;
    }
    const auto installedCli = staging / L"bin" / L"snowwidget.exe";
    std::filesystem::create_directories(
        installedCli.parent_path(), filesystemError);
    if (!filesystemError)
        std::filesystem::copy_file(status.cliSource, installedCli,
            std::filesystem::copy_options::overwrite_existing,
            filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove_all(staging, filesystemError);
        error = "cannot install the bundled snowwidget CLI";
        return false;
    }

    const bool hadExisting = std::filesystem::exists(status.target,
        filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove_all(staging, filesystemError);
        error = "cannot inspect the existing Agent Skill";
        return false;
    }
    if (hadExisting)
    {
        std::filesystem::rename(status.target, backup, filesystemError);
        if (filesystemError)
        {
            std::filesystem::remove_all(staging, filesystemError);
            error = "cannot stage the existing Agent Skill for replacement";
            return false;
        }
    }
    std::filesystem::rename(staging, status.target, filesystemError);
    if (filesystemError)
    {
        std::error_code rollbackError;
        if (hadExisting)
            std::filesystem::rename(backup, status.target, rollbackError);
        std::filesystem::remove_all(staging, rollbackError);
        error = "cannot activate the installed Agent Skill";
        return false;
    }
    if (hadExisting)
        std::filesystem::remove_all(backup, filesystemError);
    return true;
}

bool UninstallAgentSkill(const SkillInstallStatus& status,
    std::string& error)
{
    error.clear();
    if (status.target.empty() || status.agent.skillsRoot.empty() ||
        ComparablePath(status.target.parent_path()) !=
            ComparablePath(status.agent.skillsRoot) ||
        ComparablePath(status.target.filename()) !=
            ComparablePath(kSkillDirectoryName))
    {
        error = "the Agent Skill uninstall target is invalid";
        return false;
    }

    std::error_code filesystemError;
    if (!std::filesystem::exists(status.target, filesystemError))
    {
        if (filesystemError)
        {
            error = "cannot inspect the Agent Skill destination";
            return false;
        }
        return true;
    }
    if (!SkillTreeIsSafe(status.target, error)) return false;
    std::string manifestError;
    if (!ReadSkillRevision(status.target, manifestError))
    {
        error = "the Agent Skill destination is not managed by SnowDesktop";
        return false;
    }

    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto removing = status.target.parent_path() /
        (std::wstring(L".snowdesktop-lua-widget.remove-") + suffix);
    if (std::filesystem::exists(removing, filesystemError) || filesystemError)
    {
        error = "a conflicting Skill uninstall transaction already exists";
        return false;
    }
    std::filesystem::rename(status.target, removing, filesystemError);
    if (filesystemError)
    {
        error = "cannot stage the Agent Skill for removal";
        return false;
    }
    std::filesystem::remove_all(removing, filesystemError);
    if (filesystemError)
    {
        std::error_code rollbackError;
        if (std::filesystem::exists(removing, rollbackError))
            std::filesystem::rename(removing, status.target, rollbackError);
        error = "cannot remove the Agent Skill";
        return false;
    }
    return true;
}
}
