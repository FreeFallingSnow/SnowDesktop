#include "build_tool_test_support.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <iostream>

namespace
{
using namespace build_test;
std::chrono::steady_clock::time_point deadline;
Result Run(const std::vector<std::wstring>& args, const fs::path& root, int seconds = 25)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count();
    require(remaining > 0, "component profiling suite reached its bounded deadline");
    return build_test::run(args, root, std::min(seconds, static_cast<int>(remaining)));
}
Json ReadJson(const fs::path& file) { return Json::parse(read(file)); }
void Exercise(const fs::path& tool, const fs::path& host)
{
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const auto root = temporary("widget-performance-profile");
    const auto package = root / L"component";
    write(package / L"widget.json", R"json({
      "schemaVersion":2,"apiVersion":2,"dataVersion":1,
      "id":"fc818c70-8884-4d2e-b9d3-e9b54cf4051a","slug":"profile-fixture",
      "version":"1.0.0","entry":"main.lua","name":"Profile fixture",
      "description":"Known timer and allocation workload","author":"SnowDesktop","license":"MIT",
      "defaultSize":{"columns":2,"rows":1},"permissions":[],
      "requiredFeatures":["draw.immediate","lifecycle.model"]
    })json");
    write(package / L"main.lua", R"lua(
return widget.define({
  setup=function() schedule.every("measure",100) return {ticks=0} end,
  event=function(context,model,event)
    if event.kind=="schedule" then
      local values={} for i=1,96 do values[i]=i+model.ticks end
      assert(values[96]==96+model.ticks)
      model.ticks=model.ticks+1
    end
  end,
  render=function(context,model)
    local values={} for i=1,64 do values[i]=i end
    assert(values[64]==64)
    draw.text(8,8,tostring(model.ticks),14,0xFFFFFF)
  end
})
)lua");
    const auto sourceBefore = read(package / L"main.lua");
    const auto capabilities = Run({tool.wstring(), L"capabilities"}, root);
    require(capabilities.code == 0 && Json::parse(capabilities.out).at("performanceProfile").at("schemaVersion").integer() == 1,
        "CLI advertises its callable performance evaluation contract");
    require(Json::parse(capabilities.out).at("performanceProfile").at("moduleFiltering").boolean &&
        Json::parse(capabilities.out).at("performanceProfile").at("inputSampling").boolean,
        "CLI declares the optional filter and sampling contracts explicitly");
    const auto normal = Run({tool.wstring(), L"preview", package.wstring(), (root / L"ordinary.png").wstring(),
        L"--host", host.wstring(), L"--content-only"}, root, 35);
    require(normal.code == 0 && !Json::parse(normal.out).has("performance"),
        "ordinary preview remains independent of performance capture: " + normal.out + normal.err);
    for (const auto& item : fs::recursive_directory_iterator(root))
        require(item.path().extension() != L".pftrace", "ordinary preview creates no trace");

    for (const auto* scenario : {L"redraw", L"idle", L"filtered"})
    {
        const auto output = root / scenario;
        const bool filtered = std::wstring_view(scenario) == L"filtered";
        std::vector<std::wstring> arguments{tool.wstring(), L"profile", package.wstring(), output.wstring(), L"--host", host.wstring(),
            L"--seconds", L"2", L"--warmup-seconds", L"0", L"--workload", filtered ? L"redraw" : scenario, L"--scope-cpu", L"--offline"};
        if (filtered) arguments.insert(arguments.end(), {L"--modules", L"resource", L"--input-sample-every", L"4"});
        const auto result = Run(arguments, root, 55);
        require(result.code == 0, "real package profile succeeds: " + result.out + result.err);
        const auto metadata = ReadJson(output / L"evaluation.json");
        const auto& profile = metadata.at("performance");
        require(metadata.at("ok").boolean && profile.at("processRole").str() == "widget-preview" &&
            profile.at("drawCalls").number > 0, "evaluation records actual isolated drawing and process role");
        if (std::wstring_view(scenario) != L"idle")
            require(profile.at("drawCalls").number > 2,
                "redraw must continue after the initial frame; scheduler timer signals must not be lost");
        const auto manifest = ReadJson(output / L"session.json");
        require(manifest.at("completion").boolean && manifest.at("processRole").str() == "widget-preview" &&
            manifest.at("finalWidgetSampleConfirmed").boolean, "native session is complete with a real final engine resource sample");
        require(fs::file_size(output / L"trace.pftrace") > 0 && fs::file_size(output / L"preview.png") > 0,
            "evaluation retains native trace and preview artifact");
        const auto report = fs::path(wide(profile.at("report").str()));
        require(fs::is_regular_file(report), "fixed analyzer publishes a readable report");
        const auto summary = ReadJson(report.parent_path() / L"summary.json");
        require(summary.at("quality").at("valid").boolean && summary.at("effectiveDurationMs").number > 0,
            "fixed SQL accepts the native measurement window");
        bool timer = false, allocations = false;
        for (const auto& scope : summary.at("scopes").items)
            if (scope.at("module").str() == "widget" && scope.at("phase").str() == "timer.dispatch" && scope.at("calls").number > 0) timer = true;
        for (const auto& counter : summary.at("counters").items)
            if (counter.at("metric").str() == "lua_allocated_bytes_total" && counter.at("samples").number >= 2 &&
                counter.at("delta").kind == Json::Kind::Number && counter.at("delta").number > 0) allocations = true;
        require((filtered || timer) && allocations, "actual timers and successful Lua allocation traffic run independently of scope selection");
        if (filtered)
        {
            require(profile.at("moduleFilter").items.size() == 1 && profile.at("moduleFilter").items[0].str() == "resource" &&
                profile.at("inputSampleEvery").integer() == 4 && summary.at("inputDetailSampling").at("oneIn").integer() == 4 &&
                manifest.at("enabledCategories").items.size() == 2 && manifest.at("probeFiltering").at("categoryFilteredCalls").number > 0,
                "requested filter and sampling appear in native evidence, evaluation and fixed report");
            for (const auto& scope : summary.at("scopes").items)
                require(scope.at("module").str() != "widget" && scope.at("module").str() != "lua" &&
                    scope.at("module").str() != "author.preview" && scope.at("module").str() != "widget.render",
                    "resource selection excludes business scopes without disabling the actual workload");
        }
        const auto originalManifest = read(output / L"session.json");
        const auto duplicate = Run({tool.wstring(), L"profile", package.wstring(), output.wstring(), L"--host", host.wstring()}, root);
        require(duplicate.code != 0 && read(output / L"session.json") == originalManifest,
            "profiling refuses an existing evidence directory without overwriting it");
    }
    const auto unsupported = Run({tool.wstring(), L"profile", package.wstring(), (root / L"old-host").wstring(),
        L"--host", executable().wstring(), L"--offline"}, root);
    require(unsupported.code != 0 && unsupported.err.find("performance.capability") != std::string::npos,
        "an old host result cannot masquerade as a successful performance evaluation");
    const auto partialHost = Run({tool.wstring(), L"profile", package.wstring(), (root / L"old-filter-host").wstring(),
        L"--host", executable().wstring(), L"--modules", L"resource", L"--input-sample-every", L"4"}, root);
    require(partialHost.code != 0 && partialHost.err.find("performance.filterCapability") != std::string::npos,
        "a previous profile-capable host cannot silently ignore new selection options");
    const auto invalid = Run({tool.wstring(), L"profile", package.wstring(), (root / L"invalid").wstring(),
        L"--seconds", L"0", L"--host", host.wstring()}, root);
    require(invalid.code != 0 && !fs::exists(root / L"invalid"), "invalid workload arguments create no capture artifacts");
    const auto invalidModule = Run({tool.wstring(), L"profile", package.wstring(), (root / L"invalid-module").wstring(),
        L"--modules", L"resource,unknown", L"--host", host.wstring()}, root);
    require(invalidModule.code != 0 && !fs::exists(root / L"invalid-module"), "unknown module cannot launch a broad unfiltered capture");
    require(read(package / L"main.lua") == sourceBefore, "evaluation leaves component source unchanged");
    std::cout << "Component profiling artifacts retained: " << root.string() << '\n';
}
}
int wmain(int argc, wchar_t** argv)
{
    // Negative protocol fixture: an earlier host returns ordinary preview JSON
    // with no performance result. This fixture performs no component rendering.
    if (argc >= 8 && std::wstring_view(argv[1]) == L"--widget-author-preview")
    {
        bool requestedFilter = false;
        for (int index = 8; index < argc; ++index)
            requestedFilter = requestedFilter || std::wstring_view(argv[index]).find(L"@profile.modules=") != std::wstring_view::npos;
        std::ofstream(fs::path(argv[7])) << (requestedFilter
            ? "{\"ok\":true,\"performance\":{\"report\":\"legacy-report.md\"}}\n"
            : "{\"ok\":true,\"stage\":\"complete\"}\n");
        return 0;
    }
    try
    {
        build_test::require(argc == 3, "tool and host paths are required");
        Exercise(fs::path(argv[1]), fs::path(argv[2]));
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
