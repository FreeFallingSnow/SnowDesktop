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
}

int main(int argc, char** argv)
{
    Check(argc == 2, "expected the repository root argument");
    TestRemindersRenderPurity(fs::path(argv[1]));
    TestV2OnlyWidgetActivation(fs::path(argv[1]));
    std::cout << "Built-in widget source contract tests passed\n";
    return 0;
}
