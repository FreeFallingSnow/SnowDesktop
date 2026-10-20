#pragma once

#include "../../src/steam/steam_workshop_validation_cache.h"
#include "../test_temporary_directory.h"

#include <fstream>
#include <stdexcept>

inline void TestWorkshopValidationCache()
{
    using namespace snowdesktop::widget;
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    snowdesktop::test::TemporaryDirectory temporary;
    PackagePaths paths;
    paths.staging = temporary.path / L"staging";
    WidgetPackageManager manager(paths);
    const auto source = temporary.path / L"source";
    const auto archive = temporary.path / L"package.snowwidget";
    std::filesystem::create_directory(source);
    const auto write = [&](const auto& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        require(static_cast<bool>(file), "cache fixture writes its private source");
    };
    write(source / L"widget.json", R"json({
        "schemaVersion":2,"apiVersion":2,"dataVersion":1,
        "id":"34354286-9b63-4ddc-9ce2-b1c45fa7e012","slug":"cache-test",
        "name":"Cached Widget","version":"1.0.0","entry":"main.lua",
        "minHostVersion":"1.0.7.0","author":"Test","license":"MIT",
        "description":"Content validation cache fixture",
        "defaultSize":{"columns":1,"rows":1},
        "permissions":[],"optionalPermissions":[],
        "requiredFeatures":[],"optionalFeatures":[]
    })json");
    write(source / L"main.lua", "return widget.define({model = {value = 'A'}})");
    const auto exportArchive = [&](const auto& destination) {
        PackageArtifact artifact;
        ValidationReport report;
        std::string error;
        require(manager.ExportDirectory(source, destination, artifact, report, error),
            "cache fixture exports a valid real component archive");
    };
    exportArchive(archive);
    int validations = 0;
    const auto validate = [&] {
        ++validations;
        SteamWorkshopValidationCache::Result result;
        result.report = manager.ValidateArchive(archive, &result.manifest);
        return result;
    };
    SteamWorkshopValidationCache cache(2);
    auto result = cache.Validate(archive, paths.staging, validate);
    require(result.report.Ok() && !result.reused && validations == 1,
        "first background inspection fully validates the archive");
    const std::string identity = result.manifest.id;
    for (int repeat = 0; repeat != 12; ++repeat)
    {
        result = cache.Validate(archive, paths.staging, validate);
        require(result.reused && result.manifest.id == identity && result.report.Ok(),
            "unchanged background inspections reuse the exact validated manifest");
    }
    require(validations == 1, "unchanged inspections avoid repeated extraction");
    result = validate();
    require(result.report.Ok() && !result.reused && validations == 2,
        "explicit validation still reaches the complete validator");

    const auto previousSize = std::filesystem::file_size(archive);
    const auto previousTime = std::filesystem::last_write_time(archive);
    write(source / L"main.lua", "return widget.define({model = {value = 'B'}})");
    const auto replacement = temporary.path / L"replacement.snowwidget";
    exportArchive(replacement);
    std::filesystem::copy_file(replacement, archive,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(archive, previousTime);
    require(std::filesystem::file_size(archive) == previousSize,
        "replacement changes bytes while retaining the same size and timestamp");
    result = cache.Validate(archive, paths.staging, validate);
    require(result.report.Ok() && !result.reused && validations == 3,
        "content changes invalidate the cache even with identical metadata");
    result = cache.Validate(archive, paths.staging, validate);
    require(result.reused && validations == 3,
        "new successful content becomes reusable after complete validation");

    write(archive, "not a valid component archive");
    result = cache.Validate(archive, paths.staging, validate);
    require(!result.report.Ok() && !result.reused && validations == 4,
        "changed invalid content never inherits a cached success");
    result = cache.Validate(archive, paths.staging, validate);
    require(!result.report.Ok() && !result.reused && validations == 5,
        "validation failures are retried rather than cached as successful content");
    std::filesystem::copy_file(replacement, archive,
        std::filesystem::copy_options::overwrite_existing);

    bool writePrevented = false;
    result = cache.Validate(archive, paths.staging / L"another-context", [&] {
        const HANDLE writer = CreateFileW(archive.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD writerError = GetLastError();
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        writePrevented = writer == INVALID_HANDLE_VALUE &&
            writerError == ERROR_SHARING_VIOLATION;
        return validate();
    });
    require(writePrevented && result.report.Ok() && !result.reused,
        "different validation contexts revalidate under a lease that excludes writers");

    // Repeated input arrives through a new source object in each host poll.
    auto& processCache = SteamWorkshopValidationCache::ForProcess();
    result = processCache.Validate(archive, paths.staging, validate);
    require(result.report.Ok() && !result.reused,
        "process cache initially validates this private archive");
    const int beforeProcessHit = validations;
    result = SteamWorkshopValidationCache::ForProcess().Validate(
        archive, paths.staging, validate);
    require(result.reused && validations == beforeProcessHit,
        "validation survives reconstruction of the polling source");

    // A small capacity must retain recently inspected items and bound history.
    SteamWorkshopValidationCache bounded(2);
    const auto second = temporary.path / L"second.snowwidget";
    const auto third = temporary.path / L"third.snowwidget";
    std::filesystem::copy_file(archive, second);
    std::filesystem::copy_file(archive, third);
    const auto validatePath = [&](const auto& path) {
        return bounded.Validate(path, paths.staging, [&] {
            SteamWorkshopValidationCache::Result item;
            item.report = manager.ValidateArchive(path, &item.manifest);
            return item;
        });
    };
    require(!validatePath(archive).reused && !validatePath(second).reused,
        "bounded cache fills with two valid component archives");
    require(validatePath(archive).reused, "cache inspection refreshes recency");
    require(!validatePath(third).reused && validatePath(archive).reused &&
        !validatePath(second).reused,
        "bounded history evicts the least recently inspected archive");
    std::filesystem::remove(archive);
    result = processCache.Validate(archive, paths.staging, validate);
    require(!result.report.Ok() && !result.reused,
        "deleted packages cannot be returned from a previous validation");
}
