#include "dock_fullscreen_policy.h"

#include <windows.h>
#include <algorithm>
#include <iterator>
#include <cwchar>
#include <utility>

namespace snowdesktop::dock_fullscreen
{
DockFullscreenPolicy Normalize(int value) noexcept
{
    return value >= 0 && value <= 2 ? static_cast<DockFullscreenPolicy>(value)
        : DockFullscreenPolicy::FullProtection;
}

DockFullscreenPolicy LoadPolicy(
    std::optional<int> current, std::optional<bool> legacy,
    DockFullscreenPolicy fallback) noexcept
{
    if (current) return Normalize(*current);
    if (legacy) return *legacy ? DockFullscreenPolicy::BlockGestures : DockFullscreenPolicy::Allow;
    return fallback;
}

std::wstring NormalizeExecutable(std::wstring path)
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

bool SameExecutable(const std::wstring& a, const std::wstring& b) noexcept
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void NormalizeExceptions(std::vector<DockFullscreenException>& entries)
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

DockFullscreenPolicy SelectPolicy(DockFullscreenPolicy fallback,
    const std::vector<DockFullscreenException>& entries, const std::wstring& executable)
{
    const auto normalized = NormalizeExecutable(executable);
    for (const auto& entry : entries)
        if (!normalized.empty() && SameExecutable(entry.executable, normalized)) return entry.policy;
    return fallback;
}

bool BlocksReveal(DockFullscreenPolicy policy, DockRevealSource source,
    bool monitorFullscreen) noexcept
{
    if (!monitorFullscreen) return false;
    if (policy == DockFullscreenPolicy::FullProtection) return true;
    // Every reveal belongs to its destination monitor, including hotkeys and
    // associated surfaces. Keyboard focus has its own foreground guard.
    return source == DockRevealSource::Gesture && policy == DockFullscreenPolicy::BlockGestures;
}

bool StartsKeyboardSession(DockRevealSource source,
    bool foregroundHasHigherInputPrivilege,
    bool foregroundFullscreen) noexcept
{
    // Summoning alone preserves the user's keyboard target. Associated
    // editors/popups keep their existing keyboard path; a privilege gap may
    // borrow that same path for either summon trigger, outside fullscreen.
    return !foregroundFullscreen && (source == DockRevealSource::AssociatedSurface ||
        foregroundHasHigherInputPrivilege);
}
} // namespace snowdesktop::dock_fullscreen
