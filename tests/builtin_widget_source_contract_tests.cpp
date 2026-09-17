#include "test_source_boundary.h"
#include "../src/json_value.h"
#include <regex>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    std::string source = stream.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
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

std::vector<std::string> QuotedStrings(std::string_view source)
{
    std::vector<std::string> result;
    std::size_t offset = 0;
    while ((offset = source.find('"', offset)) != std::string_view::npos)
    {
        const std::size_t end = source.find('"', offset + 1);
        Check(end != std::string_view::npos,
            "unterminated quoted contract value");
        result.emplace_back(source.substr(offset + 1, end - offset - 1));
        offset = end + 1;
    }
    return result;
}

std::vector<std::string> FirstQuotedInitializerFields(std::string_view source)
{
    // Data-table extraction tolerates wrapped initializers; it does not parse
    // C++ control flow or establish that these contracts are executed.
    const std::regex field(R"field(\{\s*[^{}"]*"([^"\\]*)")field");
    const std::string text(source);
    std::vector<std::string> result;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), field);
         it != std::sregex_iterator(); ++it)
        result.push_back((*it)[1].str());
    return result;
}

void TestPublishedV2Catalog(const fs::path& repository)
{
    Check(FirstQuotedInitializerFields(R"({Kind::A,
        "one", 2}, {Kind::B, "two", 3})") ==
            std::vector<std::string>{"one", "two"},
        "static catalog extraction tolerates wrapped initializer fields");
    const std::string registry = ReadFile(
        repository / "src" / "widget_api_registry.cpp");
    const std::string viewContract = ReadFile(
        repository / "src" / "widget_view_contract.cpp");
    const std::string luaLs = ReadFile(repository / "widgets" /
        "snowdesktop-lua-widget" / "library" / "snowdesktop-v2.lua");
    const std::string api = ReadFile(repository / "widgets" /
        "snowdesktop-lua-widget" / "references" / "api-v2.md");
    const std::string skill = ReadFile(repository / "widgets" /
        "snowdesktop-lua-widget" / "SKILL.md");
    const std::string published = api + '\n' + skill + '\n' + luaLs;

    const auto features = QuotedStrings(Section(registry,
        "kHostFeatures = {",
        "using FunctionParameter = SystemFunctionParameterContract;"));
    Check(!features.empty(),
        "host feature catalog must be available for documentation checks");
    for (const auto& feature : features)
    {
        Check(published.find(feature) != std::string::npos,
            std::string("host feature is absent from authoring artifacts: ") +
                feature);
    }

    const auto systemFunctions = FirstQuotedInitializerFields(
        Section(registry, "kSystemFunctionContracts = {{",
            "constexpr std::array<SystemDataTopicContract"));
    const auto dataTopics = FirstQuotedInitializerFields(
        Section(registry, "kSystemDataTopicContracts = {{",
            "constexpr std::array<SystemTaskContract"));
    const auto tasks = FirstQuotedInitializerFields(
        Section(registry, "kSystemTaskContracts = {{",
            "kV2SandboxLibraries"));
    Check(!systemFunctions.empty() && !dataTopics.empty() && !tasks.empty(),
        "system function/data/task catalog must be available for documentation checks");
    for (const auto& name : systemFunctions)
    {
        Check(luaLs.find("function " + name + "(") != std::string::npos,
            std::string("system function is absent from LuaLS: ") + name);
        Check(api.find(name) != std::string::npos,
            std::string("system function is absent from API v2 docs: ") + name);
    }
    for (const auto* catalog : { &dataTopics, &tasks })
    {
        for (const auto& name : *catalog)
        {
            Check(luaLs.find("'" + name + "'") != std::string::npos,
                std::string("system capability is absent from LuaLS: ") +
                    name);
            Check(api.find(name) != std::string::npos,
                std::string("system capability is absent from API v2 docs: ") +
                    name);
        }
    }

    const auto nodes = FirstQuotedInitializerFields(Section(viewContract,
        "constexpr auto kContracts",
        "constexpr auto kValidationDiagnostics"));
    const auto properties = QuotedStrings(Section(viewContract,
        "constexpr auto kProperties", "constexpr auto kCommonProperties"));
    const auto events = FirstQuotedInitializerFields(Section(viewContract,
        "constexpr auto kEventContracts", "template <std::size_t Size>"));
    Check(!nodes.empty() && !properties.empty(),
        "view node/property catalog must be available for documentation checks");
    Check(!events.empty(),
        "view event catalog must be available for documentation checks");
    for (const auto& node : nodes)
    {
        Check(luaLs.find("function view." + node + "(") !=
                std::string::npos,
            std::string("view node is absent from LuaLS: ") + node);
        Check(api.find(node) != std::string::npos,
            std::string("view node is absent from API v2 docs: ") + node);
    }
    for (const auto& property : properties)
    {
        Check(luaLs.find("---@field " + property) != std::string::npos,
            std::string("view property is absent from LuaLS: ") + property);
    }
    for (const auto& event : events)
    {
        Check(luaLs.find("---@field " + event) != std::string::npos,
            std::string("view event is absent from LuaLS: ") + event);
        Check(api.find(event) != std::string::npos,
            std::string("view event is absent from API v2 docs: ") + event);
    }
}

JsonValue ReadManifest(const fs::path& path)
{
    JsonValue value;
    std::string error;
    Check(ParseJson(ReadFile(path), value, &error) && value.IsObject(),
        "built-in manifest must be a valid JSON object");
    return value;
}

bool NumberEquals(const JsonValue& manifest, const char* key, int expected)
{
    const auto* value = manifest.Find(key);
    return value && value->IsNumber() && value->number == expected;
}

void TestBuiltinPackages(const fs::path& repository)
{
    constexpr std::array<std::string_view, 11> packages{
        "agenda", "analog-clock", "digital-clock", "media-controls",
        "month-calendar", "pomodoro", "quick-launcher", "reminders",
        "rss-reader", "sticky-note", "system-monitor" };
    std::vector<std::string> actual;
    for (const auto& entry : fs::directory_iterator(repository / "widgets"))
        if (entry.is_directory() && fs::is_regular_file(entry.path() / "widget.json"))
            actual.push_back(entry.path().filename().string());
    std::sort(actual.begin(), actual.end());
    Check(std::vector<std::string>(packages.begin(), packages.end()) == actual,
        "the distributed built-in set must match the reviewed package names");
    for (const auto package : packages)
    {
        const auto directory = repository / "widgets" / package;
        const auto manifest = ReadManifest(directory / "widget.json");
        Check(NumberEquals(manifest, "schemaVersion", 2) && NumberEquals(manifest, "apiVersion", 2),
            "built-in packages declare schema/API v2 as numbers");
        const auto* slug = manifest.Find("slug");
        Check(slug && slug->IsString() && slug->string == package,
            "manifest slug agrees with its distributed package directory");
        const auto* entry = manifest.Find("entry");
        Check(entry && entry->IsString() && entry->string == "main.lua" &&
                fs::is_regular_file(directory / entry->string), "the declared built-in entry exists");
        const auto file = std::string("widgets/") + std::string(package) + "/main.lua";
        Check(snowdesktop::test::CheckSourceBoundaries(repository, {
            {file.c_str(), "", "", {"sys.", "imgui.", "http.request(", "http.get(",
                "media.current(", "media.playPause(", "media.next(", "media.previous(",
                "desktop.search(", "desktop.open(", "everything.search("}},
        }), "built-in entries do not advertise legacy library calls");
    }
    Check(NumberEquals(ReadManifest(repository / "widgets/reminders/widget.json"), "dataVersion", 3),
        "reminder storage compatibility keeps the selection migration version");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "expected the repository root argument");
    const fs::path root(argv[1]);
    TestPublishedV2Catalog(root);
    TestBuiltinPackages(root);
    Check(snowdesktop::test::CheckSourceBoundaries(root, {
        {"widgets/reminders/main.lua", "local function render(", "local function event(",
         {"storage.set(", "storage.remove(", "storage.transaction("}},
        {"widgets/sticky-note/main.lua", "elseif value.id == \"note.clear\" then",
         "elseif value.id == \"note.resetStyle\" then", {"control.focus("}},
        {"widgets/sticky-note/main.lua", "", "", {"storage.set(\"textColor\""}},
        {"widgets/media-controls/main.lua", "", "",
         {"task.start(\"app.search\"", "type = \"appSearch\"", "data.subscribe(\"app.indexStatus\""}},
        {"src/widget_engine.cpp", "bool WidgetEngine::LoadWidget(", "void WidgetEngine::RenderAll(",
         {"legacyContract", "currentContract ?"}},
        {"src/widget_engine.cpp", "", "", {"lua_WidgetSetTimer", "lua_WidgetCancelTimer", "lua_WidgetEditText"}},
        {"src/widget_engine.cpp", "static ID2D1Bitmap1* LoadImageBitmap(",
         "static ID2D1Bitmap1* LoadRuntimeImageBitmap(", {"CreateDecoderFromFilename"}},
        {"src/widget_engine.cpp", "static std::optional<std::wstring> ResolveResourceHandlePath(",
         "static std::optional<std::wstring> CurrentPackageResourcePath(", {"ResolveCurrentPackageAsset"}},
        {"src/widget_engine.cpp", "static int lua_ResourceExists(", "static int lua_ResourceImage(", {"is_regular_file"}},
        {"widgets/pomodoro/main.lua", "", "", {"red * 299", "green * 587", "blue * 114"}},
    }), "built-in resource, render and legacy-API source boundaries");
    std::cout << "Built-in manifest/publication data and source boundaries passed; no widget event or VM execution was tested\n";
    return 0;
}
