#pragma once

#include "platform/control/control_protocol.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::demo
{
// A fresh, real filesystem desktop for a full, normally launched portable host.
// Preparing a workspace neither copies binaries nor starts/stops an application.
// The caller copies only an authenticated runtime manifest into runtime, without
// data/, data.debug/ or SnowDesktop.runtime-context.json from the source install.
struct RuntimeWorkspace
{
    std::string id, locale;
    std::filesystem::path root, desktop, assets, runtime;
    std::filesystem::path normalData, activeData, debugControl, manifest;
    // Empty only for the explicitly generated fixture utility below. Existing
    // desktop copies retain their canonical source identity without rebinding
    // the targets embedded in shortcut files.
    std::filesystem::path sourceDesktop;
};

// parent must already exist and have no reparse-point ancestors. Always creates
// a new UUID child; never reuses, overwrites or deletes an existing workspace.
// Call on a COM-initialized thread (the complete desktop host already does so).
RuntimeWorkspace PrepareRuntimeWorkspace(const std::filesystem::path& parent,
    std::string_view locale = "zh-CN");

// Copy an existing real desktop byte-for-byte into a fresh owned workspace.
// File and directory last-write times are preserved, including empty files.
// Ordinary .lnk/.url files are opaque bytes; their targets are never resolved.
// Requires existing absolute directories with no reparse points, including all
// source descendants; refuses either direction of source/parent overlap. A
// missing or changing source fails explicitly and never generates substitutes.
RuntimeWorkspace PrepareRuntimeWorkspaceFromDesktop(const std::filesystem::path& parent,
    const std::filesystem::path& sourceDesktop, std::string_view locale = "zh-CN");

// Re-reads the ownership marker and checks every original fixture's path, type,
// timestamp, size and SHA-256. Runtime output/configuration may legitimately
// change after launch and is not confused with immutable fixture data.
control::Value ValidateRuntimeWorkspace(const RuntimeWorkspace& workspace);
}
