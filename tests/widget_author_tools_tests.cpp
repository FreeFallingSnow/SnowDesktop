#include "widget_author_permissions.h"
#include "widget_package.h"
#include "widget_api_registry.h"
#include "gpu_diagnostics.h"
#include "widget_gpu_lua.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include "json_value.h"
#include "test_temporary_directory.h"
#include <fstream>
#include <limits>
#include <span>
#include <vector>



#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void TestGpuLuaDetails()
{
    using namespace snowdesktop::widget_runtime;
    WidgetGpuDataSnapshot snapshot;
    snapshot.warmingUp = false;
    WidgetGpuAdapterDataSnapshot idle;
    idle.id = "idle"; idle.name = "same name"; idle.usageAvailable = true;
    idle.sharedUsageAvailable = true; idle.sharedUsedBytes = 9007199254740993ull;
    idle.engines = { { 0, 2, "", 0, 0, 1 }, { 1, 2, "", 0, 0, 2 } };
    WidgetGpuAdapterDataSnapshot missing;
    missing.id = "missing"; missing.name = idle.name; missing.usagePercent = 99;
    missing.dedicatedUsageAvailable = true; missing.dedicatedUsedBytes = 0;
    snapshot.adapters = { idle, missing };
    Check(!WidgetGpuValueAvailable(snapshot, false) && WidgetGpuValueAvailable(snapshot, true) &&
        !WidgetGpuValueAvailable({}, true), "details expose topology without changing legacy availability or inventing devices");
    auto* state = luaL_newstate();
    Check(state != nullptr, "GPU serialization Lua state is created");
    luaL_openlibs(state);
    PushWidgetGpuAdapters(state, snapshot, false); lua_setglobal(state, "legacy");
    PushWidgetGpuAdapters(state, snapshot, true); lua_setglobal(state, "details");
    snapshot.warmingUp = true;
    PushWidgetGpuAdapters(state, snapshot, true); lua_setglobal(state, "warming");
    const int result = luaL_dostring(state, R"lua(
        local count = 0
        for _ in pairs(legacy[1]) do count = count + 1 end
        assert(count == 7 and legacy[1].usageAvailable == nil and legacy[1].engines == nil,
            "old subscribers retain exactly their existing adapter fields")
        assert(legacy[1].sharedUsedBytes == 9007199254740993 and math.type(legacy[1].sharedUsedBytes) == "integer",
            "64-bit memory counters must not round through a floating point value")
        assert(details[1].id == "idle" and details[2].id == "missing" and details[1].name == details[2].name,
            "same-name adapters must retain distinct identities")
        assert(details[1].usageAvailable == true and details[1].usagePercent == 0 and
            details[2].usageAvailable == false and details[2].usagePercent == 99,
            "valid idle and invalid stale counters must remain distinguishable")
        assert(details[1].dedicatedUsageAvailable == false and details[1].sharedUsageAvailable == true and
            details[2].dedicatedUsageAvailable == true and details[2].dedicatedUsedBytes == 0 and
            details[2].sharedUsageAvailable == false, "memory channels have independent validity")
        assert(#details[1].engines == 2 and details[1].engines[1].type == "" and
            details[1].engines[1].engineIndex == 2 and details[1].engines[2].physicalIndex == 1 and
            details[1].engines[2].usagePercent == 0, "engine identity must not depend on type labels")
        assert(#details[2].engines == 0 and warming[1].usageAvailable == false and #warming[1].engines == 0 and
            warming[1].sharedUsageAvailable == true, "warm-up hides usage engines without hiding valid memory")
    )lua");
    const std::string error = result == LUA_OK ? "" : lua_tostring(state, -1);
    lua_close(state);
    if (!error.empty()) std::cerr << error << '\n';
    Check(result == LUA_OK, "production GPU serialization preserves legacy and detailed data contracts");
}

void TestGpuDiagnostics()
{
    using namespace snowdesktop::gpu_diagnostics;
    using namespace snowdesktop::widget_runtime;
    Options options; std::string error;
    const auto parse = [&](std::initializer_list<std::wstring_view> values) {
        return ParseOptions(std::span(values.begin(), values.size()), options, error);
    };
    Check(parse({L"output.jsonl"}) && options.samples == 11 && options.intervalMs == 1000,
        "diagnostic defaults give an initial baseline followed by ten bounded samples");
    Check(parse({L"output.jsonl", L"--samples", L"2", L"--interval-ms", L"250"}),
        "minimum explicit capture is accepted");
    for (const auto& invalid : std::vector<std::vector<std::wstring_view>>{
        {}, {L""}, {L"x",L"--unknown",L"1"}, {L"x",L"--samples"}, {L"x",L"--samples",L"1"},
        {L"x",L"--samples",L"121"}, {L"x",L"--samples",L"999999999999999999999999"},
        {L"x",L"--samples",L"2.5"}, {L"x",L"--samples",L"-2"}, {L"x",L"--interval-ms",L"249"},
        {L"x",L"--interval-ms",L"5001"}, {L"x",L"--samples",L"120",L"--interval-ms",L"5000"},
        {L"x",L"--samples",L"2",L"--samples",L"2"},
        {L"x",L"--interval-ms",L"250",L"--interval-ms",L"250"}})
        Check(!ParseOptions(invalid, options, error) && !error.empty(), "invalid or excessive capture is rejected before sampling");

    WidgetGpuDataSnapshot sample; sample.timestampMs = 123456789; sample.warmingUp = false;
    sample.error = "unavailable\n\"detail\"";
    WidgetGpuAdapterDataSnapshot missing; missing.id = "missing"; missing.name = "GPU \"A\"\\\n";
    missing.luid = std::numeric_limits<std::uint64_t>::max(); missing.usagePercent = 99; missing.dedicatedUsedBytes = 999;
    WidgetGpuAdapterDataSnapshot idle; idle.id = "idle"; idle.usageAvailable = true; idle.usagePercent = 0;
    idle.dedicatedUsageAvailable = true; idle.dedicatedUsedBytes = 0; idle.sharedMemoryBytes = 9007199254740993ull;
    idle.engines.push_back({0, 10, "", 80, 80, 2});
    sample.adapters = {missing, idle};
    WidgetGpuDiagnosticSample diagnostic; diagnostic.fileTime = 134348866025822004ull;
    diagnostic.collectStatus = 0xc0000bba; diagnostic.topologyStatus = 0x887a0001; diagnostic.intervalUs = 1000001;
    WidgetGpuCounterDiagnostic counter; counter.path = L"\\GPU Engine(*)\\Utilization Percentage";
    counter.formattedStatus = 0xc0000bbb;
    counter.formatted.push_back({L"pid_4_test", 0xc0000bbb, std::numeric_limits<double>::quiet_NaN(), -1});
    counter.raw.push_back({L"pid_4_test", 0xc0000bbb, 2, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), 134348866025822004ull});
    diagnostic.counters.push_back(counter);
    const auto field = [](const JsonValue& value, std::string_view key) -> const JsonValue& {
        const auto* found = value.Find(key); Check(found != nullptr, "diagnostic field is present"); return *found;
    };
    JsonValue record;
    Check(ParseJson(SerializeSample(7, sample, diagnostic), record), "diagnostic serialization produces valid JSON");
    Check(field(record,"kind").string == "sample" && field(record,"index").number == 7 &&
        field(record,"fileTime").string == "134348866025822004" && field(record,"error").string == sample.error &&
        field(record,"collectStatus").number == 0xc0000bba && field(record,"topologyStatus").number == 0x887a0001,
        "diagnostics retain exact timestamps, failure statuses and escaped errors");
    const auto& adapters = field(record,"adapters").array;
    Check(adapters.size() == 2 && field(adapters[0],"name").string == missing.name &&
        field(adapters[0],"luid").string == "18446744073709551615" &&
        field(adapters[0],"usagePercent").IsNull() && field(adapters[0],"dedicatedUsedBytes").IsNull(),
        "unavailable values cannot appear as measured zero or stale payloads; LUID precision is retained");
    Check(field(adapters[1],"usagePercent").IsNumber() && field(adapters[1],"usagePercent").number == 0 &&
        field(adapters[1],"dedicatedUsedBytes").string == "0" && field(adapters[1],"sharedUsedBytes").IsNull() &&
        field(adapters[1],"sharedMemoryBytes").string == "9007199254740993",
        "valid zero and independent memory validity survive export without 64-bit rounding");
    const auto& engines = field(adapters[1],"engines").array;
    Check(engines.size() == 1 && field(engines[0],"type").string.empty() &&
        field(engines[0],"engine").number == 10 && field(engines[0],"rawTotal").number == 80,
        "unnamed engine identity and pre-clamp totals remain inspectable");
    const auto& counters = field(record,"counters").array;
    const auto& formatted = field(counters[0],"formatted").array[0];
    const auto& raw = field(counters[0],"raw").array[0];
    Check(field(formatted,"percent").IsNull() && field(formatted,"bytes").string == "-1" &&
        field(raw,"first").string == "-9223372036854775808" && field(raw,"second").string == "9223372036854775807" &&
        field(raw,"status").number == 0xc0000bbb && field(raw,"multiCount").number == 2,
        "invalid raw rows retain their status and exact signed counters without invalid JSON numbers");
    sample.warmingUp = true;
    Check(ParseJson(SerializeSample(0, sample, diagnostic), record) &&
        field(field(record,"adapters").array[1],"usagePercent").IsNull(), "warming usage cannot be presented as current measured usage");

    int exitCode = 0;
    std::string kept;
    {
        snowdesktop::test::TemporaryDirectory temporary;
        const auto existing = temporary.path / L"existing capture.jsonl";
        { std::ofstream file(existing, std::ios::binary); file << "keep existing evidence"; }
        std::vector<std::wstring> args{L"snowwidget", L"gpu-diagnostics", existing.wstring(), L"--samples", L"2"};
        std::vector<wchar_t*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        exitCode = Run(static_cast<int>(argv.size()), argv.data(), "test");
        std::ifstream file(existing, std::ios::binary);
        kept.assign(std::istreambuf_iterator<char>(file), {});
    }
    // Check exits the process on failure, so release the owned fixture first.
    Check(exitCode == 1, "existing output refuses the real command path before GPU capture");
    Check(kept == "keep existing evidence", "diagnostic capture never truncates existing output");
}

void TestPermissionReport()
{
    snowdesktop::widget::PackageManifest manifest;
    manifest.id = "3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23";
    manifest.permissions = { "network.internet" };
    manifest.optionalPermissions = { "shell.launch" };
    manifest.networkDomains = { "feeds.example.com" };
    const auto report = snowdesktop::widget_authoring::
        BuildPermissionReport(manifest);
    Check(report.ok &&
            report.json.find("\"schemaVersion\":1") != std::string::npos &&
            report.json.find("\"risk\":\"externalCommunication\"") !=
                std::string::npos &&
            report.json.find("\"risk\":\"modification\"") !=
                std::string::npos &&
            report.json.find("\"id\":\"network.request\"") !=
                std::string::npos &&
            report.json.find("feeds.example.com") != std::string::npos &&
            report.json.find("\"requiresConsent\":true") !=
                std::string::npos,
        "permission report uses shared risk, task, and origin contracts");
}

}

int main()
{
    TestPermissionReport();
    TestGpuDiagnostics();
    TestGpuLuaDetails();
    Check(snowdesktop::widget_api::SupportsFeature("widget.confirmRemoval"),
        "host advertises removal confirmation for protected components");
    std::cout << "widget author tools tests passed\n";
    return 0;
}
