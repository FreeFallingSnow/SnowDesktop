#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

// Persisted bit positions: append modules; never reorder or reuse old values.
enum class DiagnosticLogModule : unsigned
{
    Runtime,
    Input,
    Dock,
    Taskbar,
    Tray,
    Shell,
    Icons,
    Layout,
    Widgets,
    Rendering,
    Menus,
    Navigation,
    System,
    Settings,
    Steam,
    Data,
    Count,
};

namespace snowdesktop::diagnostics
{
struct ModuleDescriptor
{
    DiagnosticLogModule module;
    std::string_view id;
    std::string_view labelKey;
};
inline constexpr std::array<ModuleDescriptor, 16> LogModules{{
    {DiagnosticLogModule::Runtime, "runtime", "settings.debug.logs.runtime"},
    {DiagnosticLogModule::Input, "input", "settings.debug.logs.input"},
    {DiagnosticLogModule::Dock, "dock", "settings.debug.logs.dock"},
    {DiagnosticLogModule::Taskbar, "taskbar", "settings.debug.logs.taskbar"},
    {DiagnosticLogModule::Tray, "tray", "settings.debug.logs.tray"},
    {DiagnosticLogModule::Shell, "shell", "settings.debug.logs.shell"},
    {DiagnosticLogModule::Icons, "icons", "settings.debug.logs.icons"},
    {DiagnosticLogModule::Layout, "layout", "settings.debug.logs.layout"},
    {DiagnosticLogModule::Widgets, "widgets", "settings.debug.logs.widgets"},
    {DiagnosticLogModule::Rendering, "rendering", "settings.debug.logs.rendering"},
    {DiagnosticLogModule::Menus, "menus", "settings.debug.logs.menus"},
    {DiagnosticLogModule::Navigation, "navigation", "settings.debug.logs.navigation"},
    {DiagnosticLogModule::System, "system", "settings.debug.logs.system"},
    {DiagnosticLogModule::Settings, "settings", "settings.debug.logs.settings"},
    {DiagnosticLogModule::Steam, "steam", "settings.debug.logs.steam"},
    {DiagnosticLogModule::Data, "data", "settings.debug.logs.data"},
}};
inline constexpr std::uint32_t AllLogModules = (1u << LogModules.size()) - 1u;
inline std::atomic<std::uint32_t> enabledLogModules{0};

constexpr std::uint32_t ModuleBit(DiagnosticLogModule module) noexcept
{
    const auto index = static_cast<unsigned>(module);
    return index < LogModules.size() ? (1u << index) : 0u;
}
constexpr std::uint32_t NormalizeLogModules(std::uint32_t modules) noexcept
{
    return modules & AllLogModules;
}
inline void SetLogModules(std::uint32_t modules) noexcept
{
    enabledLogModules.store(NormalizeLogModules(modules), std::memory_order_relaxed);
}
inline bool IsLogEnabled(DiagnosticLogModule module) noexcept
{
    return (enabledLogModules.load(std::memory_order_relaxed) & ModuleBit(module)) != 0;
}
} // namespace snowdesktop::diagnostics
