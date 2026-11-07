#include "dock_tray_rules.h"
#include "dock_fullscreen_policy.h"

#include <windows.h>
#include <algorithm>
#include <utility>

namespace snowdesktop::dock_tray
{
ClickAction ResolveClick(bool normalWindow, std::size_t instances, bool singleClickLaunch, bool doubleGesture)
{
    if (normalWindow || !instances) return ClickAction::NormalDock;
    if (instances > 1) return doubleGesture ? ClickAction::Consume : ClickAction::OpenPicker;
    if (doubleGesture) return singleClickLaunch ? ClickAction::Consume : ClickAction::ActivateTray;
    return singleClickLaunch ? ClickAction::ActivateTray : ClickAction::WaitForDoubleClick;
}

std::wstring ExecutableName(std::wstring_view path)
{
    const auto at = path.find_last_of(L"\\/");
    return std::wstring(at == std::wstring_view::npos ? path : path.substr(at + 1));
}

bool Equal(std::wstring_view a, std::wstring_view b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring NormalizeTarget(std::wstring path, bool matchPath)
{
    if (matchPath) return dock_fullscreen::NormalizeExecutable(std::move(path));
    if (path.empty() || path.size() > 255 || path.find(L'\0') != std::wstring::npos ||
        path.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return {};
    const auto first = path.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    path = path.substr(first, path.find_last_not_of(L" \t\r\n") - first + 1);
    if (path.size() <= 4 || !Equal(std::wstring_view(path).substr(path.size() - 4), L".exe")) return {};
    return path;
}

std::vector<DockTrayRule> Defaults()
{
    return {{L"QQ.exe"}, {L"Weixin.exe"}, {L"WeChat.exe"}, {L"Feishu.exe"}};
}

bool Matches(const DockTrayRule& rule, std::wstring_view executable)
{
    return !executable.empty() && Equal(rule.executable,
        rule.matchPath ? executable : std::wstring_view(ExecutableName(executable)));
}

const DockTrayRule* FindRule(std::span<const DockTrayRule> rules, std::wstring_view executable)
{
    const DockTrayRule* result = nullptr;
    for (const auto& rule : rules)
        if (Matches(rule, executable) && (!result || (rule.matchPath && !result->matchPath))) result = &rule;
    return result;
}

void NormalizeRules(std::vector<DockTrayRule>& rules)
{
    std::vector<DockTrayRule> valid;
    for (auto rule : rules)
    {
        rule.executable = NormalizeTarget(std::move(rule.executable), rule.matchPath);
        if (rule.executable.empty()) continue;
        const auto duplicate = std::find_if(valid.begin(), valid.end(), [&](const auto& existing) {
            return existing.matchPath == rule.matchPath && Equal(existing.executable, rule.executable);
        });
        if (duplicate != valid.end()) *duplicate = std::move(rule);
        else if (valid.size() < 128) valid.push_back(std::move(rule));
    }
    rules = std::move(valid);
}
}
