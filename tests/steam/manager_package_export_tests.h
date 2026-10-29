// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../../steam_bridge/src/manager_package_export.h"
#include "../test_temporary_directory.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

inline void TestManagerPackageExport()
{
    using namespace snowdesktop::steam_bridge;
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto write = [&](const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output), "package export fixture writes its owned file");
    };
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };
    const auto initialize = [&](PackagedWidget& artifact,
        const std::filesystem::path& directory, std::string_view bytes) {
        require(std::filesystem::create_directory(directory),
            "each package export fixture owns a new artifact directory");
        artifact.temporaryDirectory = directory;
        artifact.packagePath = directory / L"package.snowwidget";
        artifact.packageId = "11111111-2222-3333-4444-555555555555";
        artifact.sha256.assign(64, 'a');
        artifact.version = "1.0.0";
        write(artifact.packagePath, bytes);
    };
    snowdesktop::test::TemporaryDirectory temporary;
    const auto exports = temporary.path / L"exports";
    std::filesystem::path exported;
    std::filesystem::path staging;
    std::string error;
    {
        PackagedWidget artifact;
        initialize(artifact, temporary.path / L"first-staging", "validated package bytes");
        staging = artifact.temporaryDirectory;
        require(ExportManagerPackage(artifact, exports, exported, error) &&
            error.empty() && exported == exports /
                (L"11111111-2222-3333-4444-555555555555-" + std::wstring(64, L'a') + L".snowwidget"),
            "a real export uses only the verified UUID and complete digest as its filename");
    }
    require(!std::filesystem::exists(staging) &&
        read(exported) == "validated package bytes",
        "the persistent export survives PackagedWidget destruction and temporary package cleanup");

    const auto originalExport = exported;
    {
        PackagedWidget artifact;
        initialize(artifact, temporary.path / L"second-staging", "validated package bytes");
        require(ExportManagerPackage(artifact, exports, exported, error) &&
            exported == originalExport && error.empty() &&
            std::distance(std::filesystem::directory_iterator(exports),
                std::filesystem::directory_iterator{}) == 1,
            "identical bytes reuse the persistent file without creating another export or leaving temporary files");
        write(artifact.packagePath, "different bytes with the same reported identity");
        require(!ExportManagerPackage(artifact, exports, exported, error) &&
            exported.empty() && !error.empty() &&
            read(originalExport) == "validated package bytes" &&
            std::distance(std::filesystem::directory_iterator(exports),
                std::filesystem::directory_iterator{}) == 1,
            "a conflicting destination is preserved and failure cannot expose a stale successful output path");

        artifact.packageId = "../unsafe-id";
        exported = originalExport;
        const auto invalidRoot = temporary.path / L"invalid-exports";
        require(!ExportManagerPackage(artifact, invalidRoot, exported, error) &&
            exported.empty() && !std::filesystem::exists(invalidRoot),
            "an invalid UUID is rejected before any output directory or path is created");
        artifact.packageId = "11111111-2222-3333-4444-555555555555";
        artifact.sha256.assign(64, 'z');
        require(!ExportManagerPackage(artifact, invalidRoot, exported, error) &&
            exported.empty() && !std::filesystem::exists(invalidRoot),
            "a nonhex digest cannot become an export path");
        artifact.sha256.assign(63, 'a');
        require(!ExportManagerPackage(artifact, invalidRoot, exported, error) &&
            exported.empty() && !std::filesystem::exists(invalidRoot),
            "a truncated digest cannot become an export path");
    }
    {
        PackagedWidget artifact;
        initialize(artifact, temporary.path / L"linked-staging", "linked destination bytes");
        const auto linkExports = temporary.path / L"linked-exports";
        require(std::filesystem::create_directory(linkExports),
            "the linked destination fixture owns its export root");
        const auto outside = temporary.path / L"outside.snowwidget";
        write(outside, "linked destination bytes");
        const auto destination = linkExports / originalExport.filename();
        std::filesystem::create_hard_link(outside, destination);
        require(!ExportManagerPackage(artifact, linkExports, exported, error) &&
            exported.empty() && read(outside) == "linked destination bytes" &&
            read(destination) == "linked destination bytes",
            "even identical bytes in a linked destination are refused without changing either file");
    }
}
