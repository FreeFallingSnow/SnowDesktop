#pragma once

#include <windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::performance::process_resources
{
struct Counters
{
    bool memoryAvailable = false, extendedAvailable = false, timesAvailable = false;
    bool handlesAvailable = false, gdiAvailable = false, userAvailable = false;
    std::uint64_t privateCommit = 0, workingSet = 0, privateWorkingSet = 0;
    std::uint64_t sharedCommit = 0, pageFaults = 0, created = 0, cpu100ns = 0;
    DWORD handles = 0, gdi = 0, user = 0;
};
Counters ReadCounters(HANDLE process) noexcept;
void RecordHostCounters();

enum class ImageGroup : std::size_t { Main, Graphics, Text, ShellUi, Runtime, Other, Count };
ImageGroup ClassifyImage(std::wstring_view path, bool mainImage) noexcept;
struct MemoryGroup
{
    std::uint64_t committedRegionBytes = 0, reservedAddressBytes = 0;
    std::uint64_t residentBytes = 0, privateResidentBytes = 0, shareableResidentBytes = 0;
};
struct AddressSpace
{
    bool complete = false;
    DWORD error = 0;
    std::size_t regions = 0, queriedPages = 0, imageAllocations = 0, unnamedImages = 0;
    // PRIVATE, IMAGE, MAPPED. Region bytes are not process private commit.
    std::array<MemoryGroup, 3> types{};
    std::array<MemoryGroup, static_cast<std::size_t>(ImageGroup::Count)> images{};
    std::size_t workspaceBytes = 0;
};
struct ScanLimits
{
    std::size_t regions = 65536, pages = 2 * 1024 * 1024;
    std::chrono::milliseconds time{200};
};
AddressSpace ReadAddressSpace(HANDLE process, const ScanLimits& limits = {});

enum class ProgramKind { Excluded, SameExecutable, SteamBridge, WorkshopManager, Launcher, WidgetTool };
ProgramKind ClassifyFamilyProgram(std::wstring_view hostPath, std::wstring_view candidatePath) noexcept;
struct FamilyMember
{
    DWORD pid = 0, parent = 0, threads = 0;
    Counters counters;
    ProgramKind program = ProgramKind::SameExecutable;
};
struct Family
{
    bool complete = false;
    std::size_t unavailable = 0;
    std::vector<FamilyMember> members;
};
// Living descendants from the same installed program paths, with a validated
// creation-time chain. External applications and Explorer are excluded.
Family ReadFamily();

class Sampler
{
public:
    void Sample(bool includeDescendants, bool force = false);
private:
    std::chrono::steady_clock::time_point next_{};
    std::vector<std::wstring> previousOwners_;
};
}
