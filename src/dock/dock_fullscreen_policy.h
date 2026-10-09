#pragma once

#include <optional>
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

enum class DockFullscreenPolicy
{
    Allow = 0,
    BlockGestures = 1,
    FullProtection = 2
};

enum class DockRevealSource
{
    AssociatedSurface,
    Gesture,
    Hotkey
};

struct DockFullscreenException
{
    bool operator==(const DockFullscreenException&) const = default;
    std::wstring executable;
    DockFullscreenPolicy policy = DockFullscreenPolicy::Allow;
};

struct DockFullscreenApplication
{
    bool operator==(const DockFullscreenApplication&) const = default;
    std::wstring name, executable;
};

namespace snowdesktop::dock_fullscreen
{
inline DockFullscreenPolicy Normalize(int value) noexcept
{
    return value >= 0 && value <= 2 ? static_cast<DockFullscreenPolicy>(value)
        : DockFullscreenPolicy::FullProtection;
}

inline DockFullscreenPolicy LoadPolicy(
    std::optional<int> current, std::optional<bool> legacy,
    DockFullscreenPolicy fallback = DockFullscreenPolicy::FullProtection) noexcept
{
    if (current) return Normalize(*current);
    if (legacy) return *legacy ? DockFullscreenPolicy::BlockGestures : DockFullscreenPolicy::Allow;
    return fallback;
}

inline std::wstring NormalizeExecutable(std::wstring path)
{
    if (path.size() > 32767 || path.find(L'\0') != std::wstring::npos) return {};
    const auto first = path.find_first_not_of(L" \t\r\n\"");
    if (first == std::wstring::npos) return {};
    path = path.substr(first, path.find_last_not_of(L" \t\r\n\"") - first + 1);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    if (!(path.size() >= 3 && path[1] == L':' && path[2] == L'\\') && !path.starts_with(L"\\\\")) return {};
    wchar_t expanded[32768]{};
    const DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(expanded)), expanded, nullptr);
    if (!count || count >= std::size(expanded)) return {};
    path.assign(expanded, count);
    if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".exe") != 0) return {};
    return path;
}

inline bool SameExecutable(const std::wstring& a, const std::wstring& b) noexcept
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline void NormalizeExceptions(std::vector<DockFullscreenException>& entries)
{
    std::vector<DockFullscreenException> normalized;
    for (auto entry : entries)
    {
        entry.executable = NormalizeExecutable(std::move(entry.executable));
        if (entry.executable.empty()) continue;
        entry.policy = Normalize(static_cast<int>(entry.policy));
        const auto found = std::find_if(normalized.begin(), normalized.end(),
            [&](const auto& item) { return SameExecutable(item.executable, entry.executable); });
        if (found != normalized.end()) *found = std::move(entry);
        else if (normalized.size() < 128) normalized.push_back(std::move(entry));
    }
    entries = std::move(normalized);
}

inline DockFullscreenPolicy SelectPolicy(DockFullscreenPolicy fallback,
    const std::vector<DockFullscreenException>& entries, const std::wstring& executable)
{
    const auto normalized = NormalizeExecutable(executable);
    for (const auto& entry : entries)
        if (!normalized.empty() && SameExecutable(entry.executable, normalized)) return entry.policy;
    return fallback;
}

inline bool BlocksReveal(DockFullscreenPolicy policy, DockRevealSource source,
    bool foregroundFullscreen, bool sameMonitor) noexcept
{
    if (!foregroundFullscreen) return false;
    if (policy == DockFullscreenPolicy::FullProtection) return true;
    // A hotkey takes global keyboard focus even when the cursor is on another
    // display. Gestures are local and do not create a keyboard session.
    if (source == DockRevealSource::Gesture)
        return sameMonitor && policy == DockFullscreenPolicy::BlockGestures;
    return false;
}

inline bool StartsKeyboardSession(DockRevealSource source) noexcept
{
    return source != DockRevealSource::Gesture;
}
} // namespace snowdesktop::dock_fullscreen
