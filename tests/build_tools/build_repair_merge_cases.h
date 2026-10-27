#pragma once

#include "../build_tool_test_support.h"
#include <iostream>

namespace repair_merge_cases {
using namespace build_test;

inline std::vector<std::string> request(Coordinator& c, const std::string& task,
    const std::string& parent, int revision = 0)
{
    auto args = c.bound("repair", task, parent, revision);
    args.insert(args.end(), {"-Reason", "reviewed independent failure joins the existing editing barrier"});
    return args;
}

inline const Json& member(const Json& batch, const std::string& id)
{
    for (const auto& entry : batch.at("participants").items)
        if (entry.at("id").str() == id) return entry;
    throw std::runtime_error("Missing expected merged member " + id);
}

inline std::string failed_parent(Coordinator& c, const std::string& task,
    const std::string& test, const std::string& sharedTest = "")
{
    const auto parent = c.begin(task).at("batchId").str();
    c.plan(task, parent, 0, "selected", test);
    if (!sharedTest.empty()) {
        c.begin("shared");
        c.plan("shared", parent, 0, "selected", sharedTest, "tool", "scripts/build_protocol.ps1");
    }
    write(c.root / L"fail-build", "11");
    if (!sharedTest.empty()) c.ready("shared", parent);
    c.finish(task, parent, 0, 11);
    c.gate("fail-build", false);
    return parent;
}

inline void merged_sources(const fs::path& repo, bool originalGate)
{
    Coordinator c(repo, originalGate ? "repair-merge-original" : "repair-merge");
    if (originalGate) {
        // Restore the exact original production restriction in this isolated
        // coordinator copy. No production-only test switch is introduced.
        auto script = read(c.scripts / L"build_manager.ps1");
        const std::string marker = "                # Merge gate: all unfrozen editing sources share one barrier.";
        const std::string oldGate = "                if($current -and (Get-Field $current 'repairOf') -ne $Batch){throw ('Another batch is active: '+$current.id+' / '+$current.phase+' / '+(($current.participants|ForEach-Object {$_.id+':'+$_.state}) -join ', ')+'. Read scripts/build.bat status; use one window wait before repair. No registration displaced.')}\n";
        replace(script, marker, oldGate + marker);
        write(c.scripts / L"build_manager.ps1", script, true);
    }
    const auto first = failed_parent(c, "a", "Alpha", "ManualOne");
    const auto second = failed_parent(c, "b", "Beta", "ManualTwo");
    const auto firstEvidence = read(c.stateRoot / wide(first + ".json"));
    const auto secondEvidence = read(c.stateRoot / wide(second + ".json"));
    const auto a = c.call(request(c, "a", first));
    const auto child = a.at("batchId").str();
    const int aRevision = a.at("editRevision").integer();
    c.begin("keeper");
    c.plan("keeper", child, 0, "selected", "Gamma", "tool", "src/other.txt");
    auto keeperClaim = c.bound("claim", "keeper", child);
    keeperClaim.insert(keeperClaim.end(), {"-Files", "src/other.txt"});
    c.call(keeperClaim);
    auto claim = c.bound("claim", "a", child, aRevision);
    claim.insert(claim.end(), {"-Files", "src/a.txt"});
    c.call(claim);
    write(c.root / L"src/a.txt", "first independent repair\n");
    c.ready("a", child, aRevision);
    const auto beforeMerge = c.state().at("current");
    const auto merge = c.bridge->invoke(c.scripts / L"build_manager.ps1", request(c, "b", second));
    require(merge.code == 0,
        "Different failed parents must join the same unfrozen editing batch: " + merge.out + merge.err);
    const auto b = Json::parse(merge.out);
    const int bRevision = b.at("editRevision").integer();
    const auto current = c.state().at("current");
    require(b.at("batchId").str() == child && current.at("participants").items.size() == 4 &&
        current.at("repairSources").items.size() == 2,
        "Cross-parent merge created another batch or duplicated the shared member");
    require(member(current, "a").at("editRevision").integer() == aRevision &&
        member(current, "a").at("state").str() == "finished" &&
        member(current, "a").at("ownedFiles").dump() == member(beforeMerge, "a").at("ownedFiles").dump(),
        "Merging another parent reopened a ready owner or replaced its claim");
    require(member(current, "shared").at("repairRequirements").items.size() == 1 &&
        member(current, "shared").at("testPlan").at("tests").dump() == Json::array({"ManualOne"}).dump(),
        "Shared member lost its primary or additional source requirements");
    for (const auto& parent : {first, second}) {
        const auto mapping = Json::parse(read(c.stateRoot / wide(parent + ".repair.json")));
        require(mapping.at("childBatchId").str() == child && mapping.at("logicalBatchId").str() == parent &&
            mapping.at("attempt").integer() == 2, "A merged parent lost its independent attempt mapping");
    }
    // Reconcile a publication gap on the non-primary parent, without reopening.
    fs::remove(c.stateRoot / wide(second + ".repair.json"));
    const auto replay = c.call(request(c, "b", second));
    require(replay.at("batchId").str() == child && replay.at("editRevision").integer() == bRevision &&
        c.state().at("current").at("repairSources").items.size() == 2,
        "Non-primary mapping recovery duplicated/reopened the repair");
    c.call(request(c, "b", second, 1), 2);
    c.call(request(c, "outsider", second), 2);
    c.call({"repair-abandon", "-Batch", second, "-Reason", "cannot abandon another active merged source"}, 2);
    auto bClaim = c.bound("claim", "b", child, bRevision);
    bClaim.insert(bClaim.end(), {"-Files", "src/other.txt"});
    c.call(bClaim, 2);
    bClaim.back() = "src/b.txt";
    c.call(bClaim);
    write(c.root / L"src/b.txt", "second independent repair\n");
    c.gate("hold-build");
    write(c.root / L"fail-test", "13");
    c.ready("keeper", child);
    c.ready("b", child, bRevision);
    until([&] { return c.state().at("current").at("phase").str() == "building"; }, 35);
    c.call(request(c, "b", second), 2);
    c.call({"repair-abandon", "-Batch", first, "-Reason", "frozen merged source"}, 2);
    c.gate("hold-build", false);
    until([&] { return fs::exists(c.stateRoot / wide(child + ".json")); }, 40);
    const auto result = c.result(child);
    require(result.at("outcome").str() == "failed" && result.at("exitCode").integer() == 13 && c.calls() == 4,
        "Merged sources did not execute exactly one shared build/test pipeline");
    require(result.at("repairSources").items.size() == 2 &&
        result.at("testingPlan").at("tests").dump() == Json::array({"Alpha", "Beta", "Gamma", "ManualOne", "ManualTwo"}).dump() &&
        result.at("testingPlan").at("mode").str() == "selected",
        "Frozen merged plan dropped a source/manual test or silently escalated full coverage");
    for (const auto& entry : result.at("participants").items)
        require(entry.at("check").at("status").str() == "passed", "A merged requirement reused an old ready conclusion");
    require(read(c.stateRoot / wide(first + ".json")) == firstEvidence &&
        read(c.stateRoot / wide(second + ".json")) == secondEvidence,
        "Merging repairs changed immutable parent failure evidence");
    const auto failedChild = read(c.stateRoot / wide(child + ".json"));
    c.call(request(c, "a", first), 2);
    c.gate("fail-test", false);
    const auto next = c.call(request(c, "b", child, bRevision));
    const auto nextChild = next.at("batchId").str();
    c.ready("b", nextChild, next.at("editRevision").integer());
    until([&] { return fs::exists(c.stateRoot / wide(nextChild + ".json")); }, 40);
    require(c.result(nextChild).at("outcome").str() == "passed" && c.calls() == 6 &&
        read(c.stateRoot / wide(child + ".json")) == failedChild,
        "Further repair did not reference the latest common child exactly once");
    std::cout << "PASS independent repair sources, overlap, claims, immutable mappings, frozen gate and one shared pipeline\n";
}

inline void open_v4_batch(const fs::path& repo)
{
    Coordinator c(repo, "repair-merge-open-v4");
    const auto parent = failed_parent(c, "repairer", "Alpha");
    // Raw bridge calls retain production v4 rather than Coordinator's legacy
    // fixture override. Keep an actual editor open so this never needs a
    // synthetic candidate compiler/output tree.
    const auto live = c.bridge->call(c.scripts / L"build_manager.ps1", {"begin", "live"});
    const auto child = live.at("batchId").str();
    c.bridge->call(c.scripts / L"build_manager.ps1", {"begin", "keeper"});
    c.plan("live", child, 0, "selected", "Beta");
    c.plan("keeper", child, 0, "none", "", "docs");
    auto claim = c.bound("claim", "live", child);
    claim.insert(claim.end(), {"-Files", "src/other.txt"});
    c.call(claim);
    c.ready("live", child);
    require(c.state().at("current").at("membershipSealed").boolean,
        "Production first-ready membership seal was not exercised");
    const auto merged = c.call(request(c, "repairer", parent));
    const auto state = c.state().at("current");
    require(merged.at("batchId").str() == child && state.at("protocolVersion").integer() == 4 &&
        state.at("phase").str() == "editing" && state.at("participants").items.size() == 3 &&
        member(state, "live").at("state").str() == "finished" &&
        member(state, "live").at("ownedFiles").dump() == Json::array({"src/other.txt"}).dump() && c.calls() == 1,
        "A repair could not join a sealed but unfrozen normal v4 batch without replacing its ready owner");
    c.call(request(c, "live", parent), 2);
    write(c.scripts / L"merged-plan-fixture.ps1", R"PS(
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
$primary=[pscustomobject]@{scope='tool';suites=@('selected');tests=@('Alpha');inputs=@('src/a.txt');source='task-declared';requiredFull=$false;projectResources=[pscustomobject]@{policy='project-weighted-v1';policyHash='same';commonGitDirectory='same';compatibilityMode='shared';cpuSlots=2;memoryMiB=1024;reservedMemoryMiB=1024}}
$other=$primary|ConvertTo-Json -Depth 10|ConvertFrom-Json
$other.tests=@('Beta');$other.inputs=@();$other.projectResources.cpuSlots=3;$other.projectResources.memoryMiB=2048
$entry=[pscustomobject]@{testPlan=$primary;repairRequirements=@([pscustomobject]@{parentBatchId='other';requirement=$other})}
$weighted=Get-EffectiveTaskPlan $entry
$other.PSObject.Properties.Remove('projectResources')
$exclusive=Get-EffectiveTaskPlan $entry
[pscustomobject]@{weighted=$weighted;exclusive=$exclusive;primary=$primary}|ConvertTo-Json -Depth 15
)PS", true);
    const auto projection = c.bridge->call(c.scripts / L"merged-plan-fixture.ps1", {});
    const auto& weighted = projection.at("weighted");
    require(weighted.at("tests").dump() == Json::array({"Alpha", "Beta"}).dump() &&
        weighted.at("inputs").items.empty() && weighted.at("projectResources").at("cpuSlots").integer() == 3 &&
        weighted.at("projectResources").at("memoryMiB").integer() == 2048 &&
        weighted.at("projectResources").at("reservedMemoryMiB").integer() == 2048 &&
        !projection.at("exclusive").has("projectResources") &&
        projection.at("primary").at("tests").dump() == Json::array({"Alpha"}).dump(),
        "Merged requirements lost global input scope, resource maxima, exclusive fallback or original plan isolation");
    std::cout << "PASS repair joins an existing v4 editing batch after first ready without a second pipeline\n";
}

inline void run(const fs::path& repo)
{
    const auto control = build_test::run({executable().wstring(), repo.wstring(), L"--repair-merge-control"}, repo, 90);
    const auto output = control.out + control.err;
    std::cout << "Repair merge original-policy control: expected exit=1; actual exit=" << control.code << '\n'
        << output << std::flush;
    require(control.code == 1 && output.find("Different failed parents must join the same unfrozen editing batch") != std::string::npos &&
        output.find("Another batch is active:") != std::string::npos,
        "Original repair policy control did not fail at the cross-parent business assertion");
    merged_sources(repo, false);
    open_v4_batch(repo);
}
}
