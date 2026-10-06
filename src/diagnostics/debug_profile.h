#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::debug_profile
{
struct Configuration
{
    bool enabled = false;
    bool pendingReset = false;
    bool pendingDesktopChange = false;
    std::filesystem::path desktop;
};

struct Paths
{
    std::filesystem::path normalData;
    std::filesystem::path root;
    std::filesystem::path data;
    std::filesystem::path control;
    std::filesystem::path defaultDesktop;
};

struct Session
{
    Paths paths;
    Configuration configuration;
    bool initialized = false;
};

Paths ResolvePaths(const std::filesystem::path& normalData);
bool Read(const Paths& paths, Configuration& config, std::string& error);
bool Write(const Paths& paths, const Configuration& config, std::string& error);
bool PathsOverlap(const std::filesystem::path& left, const std::filesystem::path& right);
bool ValidateDesktop(const Paths& paths, const std::filesystem::path& desktop,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error);
bool Prepare(const Paths& paths, const Configuration& config,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error);
// Never removes the profile root or Desktop. Refuses reparse points before deletion.
bool Clear(const Paths& paths, std::string& error);
bool Initialize(const std::filesystem::path& normalData,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error);
inline Session runtimeSession;
inline const Session& Current() { return runtimeSession; }
inline bool Enabled() { return runtimeSession.initialized && runtimeSession.configuration.enabled; }
bool AcknowledgeDesktopChange(std::string& error);
}
