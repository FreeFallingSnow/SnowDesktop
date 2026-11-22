#pragma once
#include "runtime_module_policy.h"
#include <windows.h>
#include <span>
#include <string>
#include <vector>

namespace snowdesktop::runtime_modules
{
enum class TargetAccess : std::uint8_t { Unknown, Standard, Administrator };

struct Target
{
    std::uint32_t process = 0;
    std::wstring name;
    bool explorer = false;
    TargetAccess access = TargetAccess::Unknown;
    bool hook32 = false;
};
struct SkippedWindow
{
    std::uint32_t process = 0;
    std::wstring name;
    std::wstring title;
};
struct Status
{
    Module module = Module::Taskbar;
    bool allowed = true;
    unsigned active = 0;
    std::uint32_t error = 0;
    bool residencyKnown = true;
    std::vector<Target> targets;
    std::vector<Target> administratorTargets;
    std::vector<SkippedWindow> fullscreenSkipped;
};
struct Snapshot
{
    std::uint64_t policyRevision = 0;
    std::vector<Status> modules;
};
void SetWindowMinimizeTargets(std::span<const HWND> administrator, std::span<const HWND> fullscreenSkipped);

// Never connects a controller, loads a DLL or sends an attach message.
Snapshot Inspect(bool includeResidency = true);
}
