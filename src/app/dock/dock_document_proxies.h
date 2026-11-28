#pragma once

#include "dock_platform_helpers.h"
#include "dock/dock_window_preview_types.h"
#include "dock/dock_window_rules.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace snowdesktop::dock_document_proxies
{

inline std::wstring ReadCaption(HWND window)
{
    const int length = std::clamp(GetWindowTextLengthW(window), 0, 4096);
    if (length == 0) return {};
    std::wstring caption(static_cast<std::size_t>(length) + 1, L'\0');
    caption.resize(static_cast<std::size_t>(GetWindowTextW(window, caption.data(), length + 1)));
    return caption;
}

inline bool IsUncloakedProxy(HWND window)
{
    if (!IsDockTaskbarDocumentProxyCandidate(window)) return false;
    DWORD cloak = 0;
    return FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloak, sizeof(cloak))) || cloak == 0;
}

class Cohorts
{
public:
    void Add(HWND window, DWORD processId, std::wstring title)
    {
        if (title.empty()) return;
        wchar_t className[256]{};
        if (!GetClassNameW(window, className, static_cast<int>(std::size(className)))) return;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        auto cohort = std::ranges::find_if(cohorts_, [&](const auto& entry) {
            return entry.processId == processId && entry.style == style &&
                entry.exStyle == exStyle && _wcsicmp(entry.className.c_str(), className) == 0;
        });
        if (cohort == cohorts_.end())
        {
            cohorts_.push_back({processId, className, style, exStyle, {}, {}});
            cohort = std::prev(cohorts_.end());
        }
        if (std::ranges::none_of(cohort->distinctTitles, [&](const auto& existing) {
                return _wcsicmp(existing.c_str(), title.c_str()) == 0;
            }))
            cohort->distinctTitles.push_back(title);
        cohort->items.push_back({window, std::move(title)});
    }

    std::span<const DockWindowPreviewItem> Preferred(std::span<const HWND> hostWindows = {}) const
    {
        const Cohort* preferred = nullptr;
        std::size_t count = 0;
        for (const auto& cohort : cohorts_)
        {
            if (!dock_window_rules::IsTaskbarDocumentProxyCohortEligible(
                    cohort.items.size(), cohort.distinctTitles.size())) continue;
            if (!hostWindows.empty() && std::ranges::none_of(hostWindows, [&](HWND window) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(window, &pid);
                    return pid == cohort.processId;
                })) continue;
            ++count;
            preferred = &cohort;
        }
        if (!preferred || !dock_window_rules::ShouldPreferTaskbarDocumentProxyCohort(count)) return {};
        return preferred->items;
    }

private:
    struct Cohort
    {
        DWORD processId;
        std::wstring className;
        LONG_PTR style;
        LONG_PTR exStyle;
        std::vector<DockWindowPreviewItem> items;
        std::vector<std::wstring> distinctTitles;
    };
    std::vector<Cohort> cohorts_;
};

inline bool SharesProcess(HWND first, HWND second)
{
    DWORD firstPid = 0, secondPid = 0;
    GetWindowThreadProcessId(first, &firstPid);
    GetWindowThreadProcessId(second, &secondPid);
    return firstPid && firstPid == secondPid;
}

inline HWND ResolveFrame(HWND proxy)
{
    if (!IsDockTaskbarDocumentProxyCandidate(proxy)) return nullptr;
    const HWND owner = GetAncestor(proxy, GA_ROOTOWNER);
    if (owner != proxy && IsDockTaskWindow(owner) && SharesProcess(proxy, owner)) return owner;
    struct Search { HWND proxy; HWND frame = nullptr; bool ambiguous = false; };
    Search search{proxy};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Search*>(parameter);
        if (window == state.proxy || !SharesProcess(state.proxy, window) || !IsDockTaskWindow(window)) return TRUE;
        if (!state.frame)
        {
            state.frame = window;
            return TRUE;
        }
        state.ambiguous = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.ambiguous ? nullptr : search.frame;
}

inline bool IsActiveDocument(HWND frame, HWND proxy)
{
    const auto document = ReadCaption(proxy);
    const auto active = ReadCaption(frame);
    return !document.empty() && StrStrIW(active.c_str(), document.c_str()) != nullptr;
}

inline void SelectDocument(HWND proxy, HWND frame)
{
    if (proxy && IsDockTaskbarDocumentProxyCandidate(proxy) && SharesProcess(proxy, frame))
        SetForegroundWindow(proxy);
}

} // namespace snowdesktop::dock_document_proxies
