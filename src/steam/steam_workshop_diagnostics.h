#pragma once

#include "diagnostics/diagnostic_log.h"
#include "steam_workshop_sync.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace snowdesktop::widget
{
using SteamWorkshopDiagnosticSink = std::function<void(std::string_view)>;

inline const char* SteamWorkshopDiagnosticActionName(
    SteamWorkshopSyncActionKind kind) noexcept
{
    switch (kind)
    {
    case SteamWorkshopSyncActionKind::Install: return "install";
    case SteamWorkshopSyncActionKind::Update: return "update";
    case SteamWorkshopSyncActionKind::Uninstall: return "uninstall";
    }
    return "unknown";
}

// Diagnostics must never change subscription discovery or installation results.
inline void EmitSteamWorkshopDiagnostic(const SteamWorkshopDiagnosticSink& sink,
    std::string_view message) noexcept
{
    try { if (sink) sink(message); } catch (...) {}
}

inline void WriteSteamWorkshopDiagnostic(std::uint64_t queryId,
    std::string_view message) noexcept
{
    try
    {
        std::string line = "[WorkshopSync] pid=" +
            std::to_string(GetCurrentProcessId()) + " query=" +
            std::to_string(queryId) + " ";
        line.append(message.substr(0, 4096));
        for (char& character : line)
            if (character == '\r' || character == '\n' || character == '\0')
                character = ' ';
        if (message.size() > 4096) line += " [truncated]";
        const int count = MultiByteToWideChar(CP_UTF8, 0, line.data(),
            static_cast<int>(line.size()), nullptr, 0);
        if (count <= 0) return;
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(),
            static_cast<int>(line.size()), wide.data(), count);
        WriteDiagnosticLogEntry(wide.c_str());
    }
    catch (...) {}
}
}
