#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
namespace fs = std::filesystem;

void Check(bool condition, std::string_view message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Check(file.good(), "unable to read built-in widget source");
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::string_view Section(const std::string& source,
    std::string_view beginMarker, std::string_view endMarker)
{
    const std::size_t begin = source.find(beginMarker);
    Check(begin != std::string::npos, "source section start is missing");
    const std::size_t end = source.find(endMarker, begin + beginMarker.size());
    Check(end != std::string::npos, "source section end is missing");
    return std::string_view(source).substr(begin, end - begin);
}

std::size_t CountOccurrences(
    std::string_view source, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = source.find(needle, offset)) != std::string_view::npos)
    {
        ++count;
        offset += needle.size();
    }
    return count;
}

void TestRemindersRenderPurity(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "widgets" / "reminders" / "main.lua");
    const std::string_view render = Section(source,
        "local function render(", "\nlocal function event(");

    Check(render.find("storage.set(") == std::string_view::npos,
        "reminders render must not set persistent storage");
    Check(render.find("storage.remove(") == std::string_view::npos,
        "reminders render must not remove persistent storage");
    Check(render.find("storage.transaction(") == std::string_view::npos,
        "reminders render must not open a persistent storage transaction");
    Check(render.find("model.selectedId = nil") != std::string_view::npos,
        "reminders deselection must clear transient selection state");

    const std::string_view migration = Section(source,
        "local function migrateStorage(", "\nlocal function getPalette(");
    Check(migration.find("tx:remove(\"selectedId\")") !=
            std::string_view::npos,
        "reminders must clean legacy selectedId during storage migration");

    const std::string manifest = ReadFile(
        repository / "widgets" / "reminders" / "widget.json");
    Check(manifest.find("\"dataVersion\": 3") != std::string::npos,
        "reminders selection cleanup requires dataVersion 3");
}

void TestV2OnlyWidgetActivation(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "src" / "widget_engine.cpp");
    const std::string_view loadWidget = Section(source,
        "bool WidgetEngine::LoadWidget(",
        "\nvoid WidgetEngine::RenderAll(");
    const std::size_t contractGate = loadWidget.find(
        "IsExecutablePackageContract");
    const std::size_t sourceRead = loadWidget.find("ReadTextFile(path)");
    const std::size_t vmCreation = loadWidget.find("lua_newstate(");
    Check(contractGate != std::string_view::npos &&
            sourceRead != std::string_view::npos &&
            vmCreation != std::string_view::npos &&
            contractGate < sourceRead && sourceRead < vmCreation,
        "schema/API v2 must be accepted before reading or allocating an entry VM");
    Check(loadWidget.find("legacyContract") == std::string_view::npos &&
            loadWidget.find("currentContract ?") == std::string_view::npos,
        "LoadWidget must not retain an API v1 execution branch");

    const std::size_t registrationStart = source.find(
        "void WidgetEngine::RegisterDrawAPI(");
    Check(registrationStart != std::string::npos,
        "widget API registration function is missing");
    const std::string_view registration =
        std::string_view(source).substr(registrationStart);
    for (const std::string_view legacyLibrary : {
        "DescribeLibrary(\"sys\"", "DescribeLibrary(\"media\"",
        "DescribeLibrary(\"http\"", "DescribeLibrary(\"desktop\"",
        "DescribeLibrary(\"everything\"", "DescribeLibrary(\"imgui\"" })
    {
        Check(registration.find(legacyLibrary) == std::string_view::npos,
            "formal VM registration must not create API v1 library tables");
    }
}

void TestPackageResourceRenderPurity(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "src" / "widget_engine.cpp");
    const std::string_view bitmapLoad = Section(source,
        "static ID2D1Bitmap1* LoadImageBitmap(",
        "\nstatic ID2D1Bitmap1* LoadRuntimeImageBitmap(");
    Check(bitmapLoad.find("CreateDecoderFromFilename") ==
            std::string_view::npos,
        "package image drawing must not decode files on the render path");
    Check(bitmapLoad.find("packageImageCache.Find") !=
            std::string_view::npos,
        "package image drawing must consume the predecoded source cache");

    const std::string cacheSource = ReadFile(
        repository / "src" / "widget_package_image_cache.cpp");
    const std::string_view imageSourceLoad = Section(cacheSource,
        "const PackageImageSource* WidgetPackageImageCache::Acquire(",
        "\nconst PackageImageSource* WidgetPackageImageCache::Find(");
    Check(imageSourceLoad.find("CreateDecoderFromFilename") !=
            std::string_view::npos &&
            imageSourceLoad.find("sources_.find(contentKey)") !=
                std::string_view::npos &&
            imageSourceLoad.find("maximumTotalBytes_") !=
                std::string_view::npos,
        "package images must decode under a content-keyed host cache quota");

    const std::string_view imageHandle = Section(source,
        "static int lua_ResourceImage(",
        "\nstatic int lua_ResourceFont(");
    Check(imageHandle.find("Sha256File(*path)") !=
            std::string_view::npos &&
            imageHandle.find("__widget_resource_content_keys") !=
                std::string_view::npos &&
            imageHandle.find("packageImageCache.Acquire(contentKey, *path)") !=
                std::string_view::npos,
        "package image handles must bind to decoded content, not mutable paths");
    Check(cacheSource.find("WidgetPackageImageCache::Release(") !=
            std::string::npos &&
            cacheSource.find("references") != std::string::npos &&
            CountOccurrences(source, "ReleasePackageImageResources(") >= 6,
        "all VM disposal paths must release content-keyed image references");

    const std::string_view handleResolve = Section(source,
        "static std::optional<std::wstring> ResolveResourceHandlePath(",
        "\nstatic std::optional<std::wstring> CurrentPackageResourcePath(");
    Check(handleResolve.find("ResolveCurrentPackageAsset") ==
            std::string_view::npos &&
            handleResolve.find("__widget_resource_paths") !=
                std::string_view::npos,
        "resource handles must use paths resolved once during VM setup");

    const std::string_view exists = Section(source,
        "static int lua_ResourceExists(",
        "\nstatic int lua_ResourceImage(");
    Check(exists.find("is_regular_file") == std::string_view::npos &&
            exists.find("CurrentPackageResourcePath") !=
                std::string_view::npos,
        "resource.exists must not touch the filesystem from arbitrary callbacks");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "expected the repository root argument");
    TestRemindersRenderPurity(fs::path(argv[1]));
    TestV2OnlyWidgetActivation(fs::path(argv[1]));
    TestPackageResourceRenderPurity(fs::path(argv[1]));
    std::cout << "Built-in widget source contract tests passed\n";
    return 0;
}
