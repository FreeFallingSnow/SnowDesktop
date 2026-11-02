// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT
#include "component_workshop_publish.h"
#include "test_temporary_directory.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace snowdesktop::steam_bridge;
namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

// Exercise production planning and submission routing; only Steam transport is
// replaced. No remote items are created. The filename policy protects manual media.
int main(int argc, char* argv[])
{
    Check(argc == 2, "real PNG fixture path is provided");
    snowdesktop::test::TemporaryDirectory temporary;
    const auto source = temporary.path / "source.png";
    std::filesystem::copy_file(argv[1], source);
    WidgetInspection inspection;
    inspection.valid = true;
    inspection.packageId = "b6a2874c-97b4-4f5d-a02b-d08a077c7efe";
    inspection.version = "1.0.6";
    inspection.name = "Single Monitor";
    inspection.preview = source;
    inspection.previewGallery = std::vector<std::filesystem::path>{source, source};
    PackagedWidget package;
    package.temporaryDirectory = temporary.path / "artifacts";
    std::filesystem::create_directory(package.temporaryDirectory);
    package.packagePath = package.temporaryDirectory / "package.snowwidget";
    package.packageId = inspection.packageId;
    package.version = inspection.version;
    package.sha256 = "package-sha";
    WorkshopProject project;
    ComponentPublishPlan plan;
    std::string detail;
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail), detail.c_str());
    Check(plan.previewGallery && plan.previewGallery->size() == 2 && plan.preview != source,
        "the plan freezes all gallery images and primary cover");
    const auto prefix = ComponentGalleryPrefix(package.packageId);
    const auto name = plan.previewGallery->front().path.filename().string();
    Check(ComponentManagedGalleryFilename(name, prefix), "uploaded names identify managed images");
    for (const auto& manual : {std::string("manual.png"), std::string{}, prefix + "0-not-a-hash.png",
            prefix + "7-" + std::string(64, 'a') + ".png", prefix + "0-" + std::string(64, 'a') + ".mp4"})
        Check(!ComponentManagedGalleryFilename(manual, prefix), "manual images, videos and malformed names cannot be removed");
    Check(!ComponentManagedGalleryFilename(name, ComponentGalleryPrefix("another-package")),
        "images belonging to another package are preserved");
    // Source changes after confirmation cannot change the frozen upload.
    { std::ofstream changed(source, std::ios::binary | std::ios::app); changed << 'x'; }
    std::vector<PublishRequest> requests;
    ComponentPublishTransport transport;
    transport.publish = [&](const PublishRequest& request, const PublishProgressCallback&, CoreError&) -> std::optional<PublishResult> {
        requests.push_back(request);
        return PublishResult{!request.publishedFileId, 42, false, "test"};
    };
    ComponentPublishPersistence persistence;
    persistence.prepareCreateItem = [] { return true; };
    persistence.persistCreatedItem = [](std::uint64_t) { return true; };
    plan.localizations.push_back({"schinese", "单项监控", ""});
    ComponentPublishResult result;
    CoreError error;
    Check(ExecuteComponentPublishPlan(plan, transport, {}, result, error, persistence), error.message.c_str());
    Check(requests.size() == 2 && requests.front().syncAdditionalPreviews && requests.front().additionalPreviews.size() == 2 &&
        !requests.back().syncAdditionalPreviews && requests.back().additionalPreviews.empty(),
        "gallery is submitted once, independent of localized listing text");
    Check(requests.front().validateStagedPreviews(requests.front().additionalPreviews), "upload callbacks accept the confirmed image bytes");
    { std::ofstream changed(plan.previewGallery->front().path, std::ios::binary | std::ios::app); changed << 'x'; }
    Check(!requests.front().validateStagedPreviews(requests.front().additionalPreviews), "staging detects tampered images");
    const auto submitted = requests.size();
    Check(!ExecuteComponentPublishPlan(plan, transport, {}, result, error, persistence) &&
        error.code == "stalePreparation" && requests.size() == submitted,
        "changed prepared images abort before any Steam submission");
    requests.clear();
    plan = {};
    project.publishedFileId = 42;
    project.publishPreferences.previewSource = WorkshopAssetSource::Steam;
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        !plan.preview && !plan.previewGallery, "preserve-Steam policy preserves cover and gallery");
    project.publishPreferences.previewSource = WorkshopAssetSource::Local;
    inspection.previewGallery.reset();
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.preview && !plan.previewGallery, "legacy local-cover updates preserve additional previews");
    inspection.previewGallery.emplace();
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.previewGallery && plan.previewGallery->empty(), "explicit empty gallery remains an explicit removal request");
    const auto frozenPath = *plan.preview;
    plan = {};
    Check(!std::filesystem::exists(frozenPath), "prepared image ownership cleans up the temporary copies");
    std::cout << "Component gallery planning, freeze integrity, routing and managed-image ownership passed\n";
}
