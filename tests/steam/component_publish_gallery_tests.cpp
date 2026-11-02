// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT
#include "component_workshop_publish.h"
#include "manager_preview_gallery.h"
#include "preview_cache.h"
#include "test_temporary_directory.h"
#include <objbase.h>
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

void PreviewCacheReplacement(const std::filesystem::path& directory)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "WIC COM initialization");
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "isolated WARP image device");
        const auto image = directory / L"live-preview.bmp";
        const auto writeImage = [&](std::uint32_t width, std::uint32_t height) {
            std::vector<unsigned char> bytes(54 + width * height * 4, 255);
            std::fill_n(bytes.begin(), 54, 0);
            bytes[0] = 'B'; bytes[1] = 'M';
            const auto field = [&](std::size_t offset, std::uint32_t value) {
                for (int index = 0; index < 4; ++index) bytes[offset + index] = static_cast<unsigned char>(value >> (index * 8));
            };
            field(2, static_cast<std::uint32_t>(bytes.size())); field(10, 54); field(14, 40);
            field(18, width); field(22, height); bytes[26] = 1; bytes[28] = 32;
            std::ofstream output(image, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        PreviewCache cache(directory / "cache");
        writeImage(32, 16);
        const auto stamp = std::filesystem::last_write_time(image);
        cache.RequestLocal(1, image); cache.Pump(device.Get());
        Check(cache.Get(1).view && cache.Get(1).width == 32 && cache.Get(1).height == 16,
            "initial image loads without package inspection or validation");
        writeImage(16, 32);
        std::filesystem::last_write_time(image, stamp + std::chrono::seconds(1));
        cache.RefreshLocal(); cache.Pump(device.Get());
        Check(cache.Get(1).width == 16 && cache.Get(1).height == 32,
            "an open gallery reloads a replaced image and its new aspect ratio");
        const auto preservedStamp = std::filesystem::last_write_time(image);
        writeImage(32, 16); std::filesystem::last_write_time(image, preservedStamp);
        cache.RefreshLocal(true); cache.Pump(device.Get());
        Check(cache.Get(1).width == 32 && cache.Get(1).height == 16,
            "explicit refresh reloads equal-size replacements with preserved timestamps");
        { std::ofstream corrupt(image, std::ios::binary | std::ios::trunc); corrupt << "broken image"; }
        cache.RefreshLocal(true); cache.Pump(device.Get());
        Check(!cache.Get(1).view && !cache.Get(1).error.empty(), "a decode failure cannot display a stale texture");
        writeImage(16, 32); cache.RefreshLocal(); cache.Pump(device.Get());
        Check(cache.Get(1).view && cache.Get(1).height == 32, "a corrected image recovers without restarting the manager");
    }
    CoUninitialize();
}

void RemotePreviewEdits()
{
    PublishedItem item;
    item.publishedFileId = 42; item.ownerSteamId = 100; item.consumerAppId = 200;
    item.previewUrl = "cover";
    item.additionalPreviews = {{"a", "manual.png", 0}, {"video", "", 1},
        {"b", "old.png", 0}, {"c", "remove.png", 0}};
    WorkshopPreviewUpdate draft;
    draft.expected = item;
    draft.edits = {{0, {}}, {2, "replace.png"}, {3, {}}};
    draft.additions = {"added.png"};
    Check(ValidateManagerPreviewUpdate(draft, item, 100, 200), "owned unchanged gallery accepts image edits");
    auto changed = item; std::swap(changed.additionalPreviews[0], changed.additionalPreviews[2]);
    Check(!ValidateManagerPreviewUpdate(draft, changed, 100, 200), "changed remote indices reject stale edits");
    Check(!ValidateManagerPreviewUpdate(draft, item, 101, 200), "account changes reject pending image edits");
    draft.edits.push_back({1, {}});
    Check(!ValidateManagerPreviewUpdate(draft, item, 100, 200), "image editing cannot remove a video");
    draft.edits.pop_back();
    std::vector<std::string> operations;
    Check(ApplyManagerPreviewUpdate(draft, [](const auto&) { return true; },
        [&](std::uint32_t index, const auto&) { operations.push_back("replace:" + std::to_string(index)); return true; },
        [&](std::uint32_t index) { operations.push_back("remove:" + std::to_string(index)); return true; },
        [&](const auto&) { operations.push_back("add"); return true; }), "preview edit routing succeeds");
    Check(operations == std::vector<std::string>{"replace:2", "remove:3", "remove:0", "add"},
        "replacements use original indices and removals preserve unedited media");
}
}

// Exercise production planning and submission routing; only Steam transport is
// replaced. No remote items are created. The filename policy protects manual media.
int main(int argc, char* argv[])
{
    Check(argc == 2, "real PNG fixture path is provided");
    snowdesktop::test::TemporaryDirectory temporary;
    PreviewCacheReplacement(temporary.path);
    RemotePreviewEdits();
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
    plan = {};
    project.previewGallery = std::vector<std::filesystem::path>{source};
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.previewGallery && plan.previewGallery->size() == 1, "manager's gallery overrides the component gallery in the upload plan");
    plan = {};
    project.previewGallery.emplace();
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.previewGallery && plan.previewGallery->empty(), "removing all manager images requests an explicit managed-gallery clear");
    plan = {};
    project.previewGallery.reset();
    inspection.previewGallery.reset();
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.preview && !plan.previewGallery, "legacy local-cover updates preserve additional previews");
    inspection.previewGallery.emplace();
    Check(BuildComponentPublishPlan(project, inspection, package, {}, plan, detail) &&
        plan.previewGallery && plan.previewGallery->empty(), "explicit empty gallery remains an explicit removal request");
    const auto frozenPath = *plan.preview;
    plan = {};
    Check(!std::filesystem::exists(frozenPath), "prepared image ownership cleans up the temporary copies");
    const auto component = temporary.path / "component";
    std::filesystem::create_directory(component);
    { std::ofstream manifest(component / "widget.json"); manifest << R"({"preview":"cover.png","previewGallery":["one.png","two.png"]})"; }
    project.sourceDirectory = component;
    auto local = ReadManagerLocalPreviews(project);
    Check(local.error.empty() && local.primary == component / "cover.png" && local.gallery && local.gallery->size() == 2,
        "all manifest pictures load before package validation, including unvalidated project records");
    ProjectStore store(temporary.path / "projects");
    Check(store.Load(detail), detail.c_str());
    WorkshopProject* stored = nullptr;
    Check(store.AddDirectory(component, stored, detail), detail.c_str());
    stored->primaryPreview = source; stored->previewGallery = std::vector<std::filesystem::path>{source, component / L"two.png"};
    Check(store.Save(detail), detail.c_str());
    ProjectStore reloaded(store.Root());
    Check(reloaded.Load(detail) && reloaded.Projects().front().previewGallery == stored->previewGallery,
        "custom gallery paths and order survive reopening the manager");
    reloaded.Projects().front().previewGallery.emplace(); Check(reloaded.Save(detail), detail.c_str());
    ProjectStore cleared(store.Root());
    Check(cleared.Load(detail) && cleared.Projects().front().previewGallery && cleared.Projects().front().previewGallery->empty(),
        "an explicitly empty gallery survives reopening");
    std::cout << "Component gallery planning, freeze integrity, routing and managed-image ownership passed\n";
}
