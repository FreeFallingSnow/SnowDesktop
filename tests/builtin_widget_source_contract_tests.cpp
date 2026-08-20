#include <cstdlib>
#include <array>
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

std::vector<std::string> FirstQuotedInitializerFields(
    std::string_view source)
{
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < source.size())
    {
        const std::size_t end = source.find('\n', offset);
        const std::string_view line = source.substr(offset,
            end == std::string_view::npos ? source.size() - offset
                                          : end - offset);
        const std::size_t initializer = line.find('{');
        const std::size_t quote = line.find('"', initializer);
        if (initializer != std::string_view::npos &&
            quote != std::string_view::npos)
        {
            const std::size_t quoteEnd = line.find('"', quote + 1);
            Check(quoteEnd != std::string_view::npos,
                "unterminated contract initializer value");
            result.emplace_back(
                line.substr(quote + 1, quoteEnd - quote - 1));
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}

void TestPublishedV2Catalog(const fs::path& repository)
{
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
    Check(features.size() == 200,
        "host feature catalog size must match the reviewed v2 contract");
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
            "constexpr std::array<std::string_view, 24> kV2SandboxLibraries"));
    Check(systemFunctions.size() == 15 && dataTopics.size() == 25 &&
            tasks.size() == 41,
        "system function/data/task catalog sizes must match the reviewed v2 contract");
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
    Check(nodes.size() == 44 && properties.size() == 146,
        "view node/property catalog sizes must match the reviewed v2 contract");
    Check(events.size() == 17,
        "view event catalog size must match the reviewed v2 contract");
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

void TestStickyNoteClearBlur(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "widgets" / "sticky-note" / "main.lua");
    const std::string_view clearAction = Section(source,
        "elseif value.id == \"note.clear\" then",
        "elseif value.id == \"note.resetStyle\" then");
    const std::size_t blur = clearAction.find("control.blur(\"note\")");
    const std::size_t remove = clearAction.find("storage.remove(\"text\")");
    Check(blur != std::string_view::npos &&
            remove != std::string_view::npos && blur < remove,
        "sticky-note clear must blur the editor before removing its storage");
    Check(clearAction.find("control.focus(") == std::string_view::npos,
        "sticky-note clear must not restore the blue input focus cue");

    const std::string manifest = ReadFile(
        repository / "widgets" / "sticky-note" / "widget.json");
    Check(manifest.find("\"control.blur\"") != std::string::npos,
        "sticky-note must declare the control.blur host feature");
}

void TestStickyNotePresetTextColors(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "widgets" / "sticky-note" / "main.lua");
    const std::string themeSource = ReadFile(repository / "widgets" /
        "sticky-note" / "modules" / "theme.lua");
    const std::string_view colors = Section(themeSource,
        "noteTheme.presetTextColors = {", "\n}\n\nfunction");
    for (const std::string_view preset : {
        "classic", "white", "pink", "blue", "green", "purple" })
    {
        Check(colors.find(std::string(preset) + " = 0x000000") !=
                std::string_view::npos,
            "every light sticky-note preset must use black text");
    }
    Check(colors.find("dark = 0xFFFFFF") != std::string_view::npos,
        "the dark sticky-note preset must use white text");
    Check(CountOccurrences(source,
            "textColor = presetTextColors.") == 7,
        "every sticky-note preset must persist its assigned text color");

    Check(source.find(
            "local noteTheme = module.require(\"modules/theme.lua\")") !=
            std::string::npos,
        "sticky-note must load its tested theme color rules");
    Check(source.find("noteTheme.resolveTextColor(") !=
            std::string::npos,
        "sticky-note must resolve rendered text through its theme rules");

    const std::string_view resolveColor = Section(themeSource,
        "function noteTheme.resolveTextColor(", "\nend\n\nreturn noteTheme");
    const std::size_t follow = resolveColor.find(
        "not followsPersonalization");
    const std::size_t preset = resolveColor.find(
        "noteTheme.presetTextColors[preset]");
    const std::size_t contentTheme = resolveColor.find(
        "contentTheme == 1");
    Check(follow != std::string_view::npos &&
            preset != std::string_view::npos &&
            contentTheme != std::string_view::npos &&
            follow < preset && preset < contentTheme,
        "sticky-note must resolve preset text before the global theme fallback");
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

void TestMediaControlsArtwork(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "widgets" / "media-controls" / "main.lua");
    const std::string manifest = ReadFile(
        repository / "widgets" / "media-controls" / "widget.json");

    Check(source.find("data.subscribe(\"media.artwork\"") !=
            std::string::npos &&
            manifest.find("\"data.media.artwork\"") !=
                std::string::npos,
        "media controls must declare and subscribe to current artwork");
    Check(source.find("storage.get(\"showArtwork\") == \"0\"") !=
            std::string::npos &&
            source.find("lua_widget.media_control.show_artwork") !=
                std::string::npos,
        "media artwork must expose a localized visibility setting");
    Check(source.find("artwork.sessionId ~= session.id") !=
            std::string::npos &&
            source.find("draw.imageFit(artwork.image") !=
                std::string::npos,
        "media controls must reject stale artwork and draw the host handle");
    Check(source.find("type = \"appReference\"") != std::string::npos &&
            source.find("binding = \"idlePlayer\"") != std::string::npos &&
            source.find("slots.binding(\"idlePlayer\")") !=
                std::string::npos &&
            manifest.find("\"idlePlayer\"") != std::string::npos &&
            manifest.find("\"app.reference\"") != std::string::npos,
        "media controls must persist the idle launcher as an app reference binding");
    Check(source.find("launcherBinding:pick()") != std::string::npos &&
            source.find("ref = launcher.reference") != std::string::npos &&
            manifest.find("\"slots.hostPicker\"") != std::string::npos &&
            manifest.find("\"settings.appReference\"") !=
                std::string::npos,
        "media controls must choose and launch the bound idle application through host APIs");
    Check(source.find("id = \"launcher.current\"") !=
            std::string::npos &&
            source.find("label = launcher and launcher.title") !=
                std::string::npos &&
            source.find("lua_widget.media_control.clear_launcher") !=
                std::string::npos,
        "media controls menu must show the current launcher and expose an explicit clear action");
    Check(source.find("task.start(\"app.search\"") == std::string::npos &&
            source.find("type = \"appSearch\"") == std::string::npos &&
            source.find("data.subscribe(\"app.indexStatus\"") ==
                std::string::npos &&
            manifest.find("\"app.discovery\"") == std::string::npos,
        "media controls must not retain title-based app search or discovery permission");
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
    for (const std::string_view legacyFunction : {
            "lua_WidgetSetTimer", "lua_WidgetCancelTimer",
            "lua_WidgetEditText" })
    {
        Check(source.find(legacyFunction) == std::string_view::npos,
            "formal VM source must not retain an API v1 widget callback");
    }

    const std::string luaLs = ReadFile(repository / "widgets" /
        "snowdesktop-lua-widget" / "library" / "snowdesktop-v2.lua");
    for (const std::string_view legacyDeclaration : {
            "function widget.setTimer", "function widget.cancelTimer",
            "function widget.editText" })
    {
        Check(luaLs.find(legacyDeclaration) == std::string_view::npos,
            "API v2 LuaLS must not advertise an API v1 widget function");
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
            imageHandle.find("packageImageCache.Acquire(") !=
                std::string_view::npos,
        "package image handles must bind to decoded content, not mutable paths");
    for (const std::string_view stableError : {
        "loadPhaseRequired", "invalidName", "notDeclared", "typeMismatch",
        "hostUnavailable", "unavailable", "quotaExceeded", "decodeFailed",
        "deviceUnavailable" })
    {
        Check(imageHandle.find(stableError) != std::string_view::npos,
            "package image loading must expose stable error codes");
    }
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

    const std::string_view resourceStatus = Section(source,
        "static int lua_ResourceStatus(",
        "\nstatic ComPtr<ID2D1Bitmap1> BitmapFromHBitmap(");
    Check(resourceStatus.find("\"pending\"") == std::string_view::npos &&
            resourceStatus.find("\"unavailable\"") !=
                std::string_view::npos,
        "synchronously created resource handles must report ready or a stable error");
}

void TestAudioAnalysisSubscriptionOptions(const fs::path& repository)
{
    const std::string source = ReadFile(
        repository / "src" / "widget_engine.cpp");
    const std::string_view subscribe = Section(source,
        "static int lua_DataSubscribe(",
        "\nstatic int lua_TaskStart(");
    for (const std::string_view option : {
        "\"features\"", "\"updateHz\"", "\"waveformPoints\"",
        "\"spectrumBins\"", "mutually exclusive",
        "WidgetAudioAnalysisConfiguration",
        "audio.output.analysis requires whenHidden" })
    {
        Check(subscribe.find(option) != std::string_view::npos,
            "audio analysis subscription option contract is missing");
    }

    const std::string_view actions = Section(source,
        "void WidgetEngine::ApplyWidgetDataBrokerActions()",
        "\nvoid WidgetEngine::ReconcileFilesystemWatches()");
    Check(actions.find(
            "SubscriptionSnapshots(\"audio.output.analysis\")") !=
            std::string_view::npos &&
            actions.find("audioConfiguration()") !=
                std::string_view::npos,
        "audio provider must aggregate eligible subscription configurations");
}

void TestAllBuiltinWidgetsUseV2(const fs::path& repository)
{
    static constexpr std::array<std::string_view, 11> packages = {
        "agenda", "analog-clock", "digital-clock", "media-controls",
        "month-calendar", "pomodoro", "quick-launcher", "reminders",
        "rss-reader", "sticky-note", "system-monitor"
    };
    const fs::path widgets = repository / "widgets";
    std::size_t discovered = 0;
    for (const auto& entry : fs::directory_iterator(widgets))
    {
        if (entry.is_directory() &&
            fs::is_regular_file(entry.path() / "widget.json"))
            ++discovered;
    }
    Check(discovered == packages.size(),
        "the built-in package list must stay explicit and complete");

    for (const std::string_view package : packages)
    {
        const fs::path directory = widgets / package;
        const std::string manifest = ReadFile(directory / "widget.json");
        Check(manifest.find("\"schemaVersion\": 2") !=
                std::string::npos &&
                manifest.find("\"apiVersion\": 2") !=
                    std::string::npos,
            "every built-in package must declare schema/API v2");

        const std::string source = ReadFile(directory / "main.lua");
        Check(source.find("return widget.define(") != std::string::npos,
            "every built-in entry must return a widget.define descriptor");
        for (const std::string_view legacyCall : {
            "sys.", "imgui.", "http.request(", "http.get(",
            "media.current(", "media.playPause(", "media.next(",
            "media.previous(", "desktop.search(", "desktop.open(",
            "everything.search(" })
        {
            Check(source.find(legacyCall) == std::string::npos,
                "built-in entries must not call an API v1 library");
        }
    }
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "expected the repository root argument");
    TestPublishedV2Catalog(fs::path(argv[1]));
    TestRemindersRenderPurity(fs::path(argv[1]));
    TestStickyNoteClearBlur(fs::path(argv[1]));
    TestStickyNotePresetTextColors(fs::path(argv[1]));
    TestMediaControlsArtwork(fs::path(argv[1]));
    TestV2OnlyWidgetActivation(fs::path(argv[1]));
    TestPackageResourceRenderPurity(fs::path(argv[1]));
    TestAudioAnalysisSubscriptionOptions(fs::path(argv[1]));
    TestAllBuiltinWidgetsUseV2(fs::path(argv[1]));
    std::cout << "Built-in widget source contract tests passed\n";
    return 0;
}
