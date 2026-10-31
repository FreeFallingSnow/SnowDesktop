#include "build_tool_test_support.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <string_view>
using namespace build_test;
namespace {
void run_tests(const fs::path& repo){
    auto root=temporary("selection");Bridge bridge(repo,root);auto script=root/L"selection.ps1";
    // Exercise the real batch argument dispatch with an isolated launch sentinel.
    // An accidental default-full branch cannot reach the repository test manager.
    copy(repo,root,{"scripts/test.bat"});
    write(root/L"scripts/powershell_runtime.bat","@echo off\r\nset \"SNOWDESKTOP_ENTRY_POWERSHELL=%~dp0unexpected.cmd\"\r\nexit /b 0\r\n");
    write(root/L"scripts/unexpected.cmd","@echo off\r\necho unexpected>\"%~dp0unexpected.txt\"\r\nexit /b 91\r\n");
    const auto defaultEntry=run({L"cmd.exe",L"/d",L"/c",(root/L"scripts/test.bat").wstring()},root);
    require(defaultEntry.code==2&&defaultEntry.out.find("Usage:")!=std::string::npos&&!fs::exists(root/L"scripts/unexpected.txt"),"No-argument test entry launched work instead of showing usage");
    write(script,"param([string]$Operation)\n$ErrorActionPreference='Stop';Set-StrictMode -Version Latest\n$repositoryRoot="+ps_literal(repo)+R"PS(
foreach($import in @(@{path='scripts/test_manager.ps1';functions=@('Get-TestSelection','Get-TestBuildRoots','Get-TestConfirmedBuildTargets','Invoke-FilteredTests','Get-TestRunOptions','Assert-HostRuntimeAvailable','Assert-CompleteTestReport')},@{path='scripts/build_protocol.ps1';functions=@('Get-Field','Resolve-PlanTests')},@{path='scripts/test_output.ps1';functions=@('Test-IsolatedOutput')},@{path='scripts/build_entry.ps1';functions=@('Read-EntryJson','Test-CandidateSourceRoot','Get-CandidateEntryContext')})){
 $tokens=$null;$errors=$null;$source=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot $import.path),[ref]$tokens,[ref]$errors)
 foreach($functionName in $import.functions){$definition=$source.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName},$false);. ([scriptblock]::Create($definition.Extent.Text))}
}
$repositoryRoot=$PSScriptRoot # Function-only probes operate on their isolated fixture, including inside a candidate checkout.
$request=Get-Content -Raw (Join-Path $PSScriptRoot 'request.json')|ConvertFrom-Json
$script:fixture=$request.inventory|ConvertTo-Json -Depth 12 -Compress;$script:queryArguments=@();$script:invocations=@();$script:runtimeInspections=0;$script:runtimeLocks=@($request.locks);$Mode='name'
function Get-HostRuntimeLocks {++$script:runtimeInspections;$script:runtimeLocks}
function Write-Host {$script:messages+=@(($args -join ' '))}
function ctest {$script:queryArguments=@($args);$global:LASTEXITCODE=0;$script:fixture}
function Invoke-Checked {param([string]$FilePath,[string[]]$Arguments=@(),[string[]]$ExpectedTests=@());$script:invocations+=@{file=$FilePath;arguments=$Arguments;expected=$ExpectedTests}}
$value=$null;$errorText='';$script:messages=@()
try{
 switch($Operation){
 'selection' {$value=Get-TestSelection -CTestFilterArguments @($request.filter)}
 'invoke' {if($request.aggregate){Invoke-FilteredTests -CTestFilterArguments @($request.filter) -BuildPreset tests}else{Invoke-FilteredTests -CTestFilterArguments @($request.filter)}}
 'options' {$value=Get-TestRunOptions -Mode $request.mode -Filter $request.text}
 'plan' {$value=@(Resolve-PlanTests $request.plan @($request.inventory.tests))}
 'report' {Assert-CompleteTestReport -Report ([xml]$request.xml) -ExpectedTests @($request.expected)}
 'output' {Test-IsolatedOutput $PSScriptRoot}
 }
}catch{$errorText=$_.Exception.Message}
@{value=$value;error=$errorText;calls=$script:invocations;query=$script:queryArguments;inspections=$script:runtimeInspections;messages=$script:messages}|ConvertTo-Json -Depth 18
)PS",true);
    Json request=Json::object({{"inventory",Json::object({{"tests",Json::array({})}})},{"locks",Json::array({})},{"filter",Json::array({})},{"aggregate",false}});
    auto probe=[&](const std::string& operation){write(root/L"request.json",request.dump());return bridge.call(script,{operation});};auto tests=[&](Json value){request["inventory"]["tests"]=value;};auto expectError=[&](const std::string& operation,const std::string& text){auto result=probe(operation);require(result.at("error").str().find(text)!=std::string::npos,"Expected error "+text+": "+result.dump());return result;};
    auto binary="C:/isolated-tests/SnowDesktopSelectionFixtureTests.exe";
    tests(Json::array({Json::object({{"name","unbuilt"},{"properties",Json::array({Json::object({{"name","REQUIRED_FILES"},{"value",Json::array({binary})}})})}})}));request["filter"]=Json::array({"-R","^unbuilt$"});auto result=probe("invoke");require(result.at("error").str().empty()&&result.at("calls").items.size()==2,"Cold selection failed");require(result.at("calls").items[0].at("arguments").dump()==Json::array({"--build","--preset","tests","--target","SnowDesktopSelectionFixtureTests"}).dump()&&result.at("calls").items[1].at("arguments").dump()==Json::array({"--preset","tests","-R","^unbuilt$"}).dump(),"Cold selection changed target/filter");
    auto built=Json::object({{"name","built"},{"command",Json::array({binary,"case-a"})},{"properties",Json::array({})}}),alias=built;alias["name"]="alias";alias["command"]=Json::array({binary,"case-b"});tests(Json::array({built,alias}));request["filter"]=Json::array({"-L","widget"});result=probe("selection");require(result.at("value").at("Tests").items.size()==2&&result.at("value").at("Targets").dump()==Json::array({"SnowDesktopSelectionFixtureTests"}).dump(),"Aliases lost cases or repeated targets");
    tests(Json::array({Json::object({{"name","script"},{"command",Json::array({"powershell.exe","test.ps1"})},{"properties",Json::array({})}})}));result=probe("invoke");require(result.at("calls").items.size()==1&&result.at("calls").items[0].at("file").str()=="ctest","Script fixture invented native target");tests(Json::array({}));require(expectError("invoke","did not match").at("calls").items.empty(),"Empty selection invoked build/test");tests(Json::array({Json::object({{"name","missing"},{"properties",Json::array({})}})}));expectError("selection","Cannot resolve the build target");tests(Json::array({Json::object({{"name","ambiguous"},{"properties",Json::array({Json::object({{"name","REQUIRED_FILES"},{"value",Json::array({binary,"C:/isolated-tests/SnowDesktopOtherTests.exe"})}})})}})}));expectError("selection","Cannot resolve the build target");
    auto preview=Json::object({{"name","preview"},{"command",Json::array({"C:/tests/SnowDesktopWidgetAuthorPreviewCliTests.exe",utf8((repo/L".build/Release/SnowDesktop.exe").wstring())})},{"properties",Json::array({Json::object({{"name","LABELS"},{"value",Json::array({"host-runtime"})}})})}});request["locks"]=Json::array({"owned fixture process"});tests(Json::array({preview}));require(expectError("invoke","Host runtime is in use").at("calls").items.empty(),"Occupied host built before preflight");tests(Json::array({built}));request["aggregate"]=true;expectError("invoke","Host runtime is in use");request["aggregate"]=false;result=probe("invoke");require(result.at("inspections").integer()==0&&result.at("calls").items.size()==2,"Ordinary selection inspected occupied host");request["locks"]=Json::array({});request["aggregate"]=true;tests(Json::array({preview}));result=probe("invoke");require(result.at("calls").items.size()==2,"Aggregate arranged host twice");
    auto labelled=[&](const std::string& name,Json labels){return Json::object({{"name",name},{"properties",Json::array({Json::object({{"name","LABELS"},{"value",labels}})})}});};
    tests(Json::array({labelled("application",Json::array({"core"})),labelled("tool",Json::array({"tools","core"})),labelled("manual-tool",Json::array({"tools","manual"}))}));
    request["plan"]=Json::object({{"mode","full"},{"suites",Json::array({"full"})},{"tests",Json::array({})},{"tasks",Json::array({})}});
    require(probe("plan").at("value").dump()==Json::array({"application"}).dump(),"Full plan selected build tools");
    request["plan"]["mode"]="selected";request["plan"]["suites"]=Json::array({"tools"});
    require(probe("plan").at("value").dump()==Json::array({"tool"}).dump(),"Tools plan selected application/manual cases");
    request["plan"]["mode"]="full";request["plan"]["suites"]=Json::array({"full","tools"});
    require(probe("plan").at("value").dump()==Json::array({"application","tool"}).dump(),"Merged explicit tools plan lost required coverage");
    request["plan"]["mode"]="selected";request["plan"]["suites"]=Json::array({"selected"});request["plan"]["tests"]=Json::array({"manual-tool"});
    require(probe("plan").at("value").dump()==Json::array({"manual-tool"}).dump(),"Explicit manual tool selection was excluded");
    request["plan"]["tests"]=Json::array({});expectError("plan","zero tests");
    // Read the unfiltered CTest declarations before exercising mode selection.
    // The independent oracle below is the documented label contract, never a
    // Get-TestRunOptions result or the presence of a particular product test.
    const auto declaredQuery=run({L"ctest",L"--preset",L"all-tests",L"--show-only=json-v1"},repo);
    require(declaredQuery.code==0,"Cannot read actual CTest declarations: "+declaredQuery.out+declaredQuery.err);
    const auto declarations=Json::parse(declaredQuery.out).at("tests");
    require(!declarations.items.empty(),"Actual CTest declarations are empty");
    struct DeclaredTest {
        std::string name;
        bool manual=false,tools=false,core=false,integration=false;
    };
    std::vector<DeclaredTest> declared;
    std::set<std::string> declaredNames,manualNames;
    for(const auto& test:declarations.items){
        DeclaredTest entry;entry.name=test.at("name").str();
        require(!entry.name.empty()&&declaredNames.insert(entry.name).second,
            "Actual CTest declarations contain an empty or duplicate name");
        for(const auto& property:test.at("properties").items)
            if(property.at("name").str()=="LABELS")
                for(const auto& label:property.at("value").items){
                    const auto value=label.str();
                    if(value=="manual")entry.manual=true;
                    else if(value=="tools")entry.tools=true;
                    else if(value=="core")entry.core=true;
                    else if(value=="integration")entry.integration=true;
                }
        if(entry.manual)manualNames.insert(entry.name);
        declared.push_back(std::move(entry));
    }
    require(!manualNames.empty(),"Actual declarations must supply a manual entry for explicit-selection integration");
    const auto manualName=*manualNames.begin();
    const auto exactNamePattern=[](const std::string& name){
        std::string pattern="^";
        for(const char value:name){
            if(std::string_view(R"(\.^$|()[]{}*+?)").find(value)!=std::string_view::npos)pattern+='\\';
            pattern+=value;
        }
        return pattern+"$";
    };
    const auto describe=[](const std::set<std::string>& names){
        std::string text;
        for(const auto& name:names){if(!text.empty())text+=",";text+=name;}
        return text;
    };
    for(const std::string mode:{"full","core","fast","tools","name","label"}){
        std::set<std::string> expected;
        for(const auto& test:declared){
            const bool automaticApplication=!test.tools&&!test.manual;
            const bool selected=mode=="full"?automaticApplication:
                mode=="core"?automaticApplication&&test.core:
                mode=="fast"?automaticApplication&&!test.integration:
                mode=="tools"?test.tools&&!test.manual:
                mode=="name"?test.name==manualName:test.manual;
            if(selected)expected.insert(test.name);
        }
        require(!expected.empty(),"Declared label contract has zero tests for mode "+mode);
        request["mode"]=mode;
        request["text"]=mode=="name"?exactNamePattern(manualName):mode=="label"?"^manual$":"";
        const auto options=probe("options").at("value");
        std::vector<std::wstring> args={L"ctest",L"--preset",wide(options.at("TestPreset").str()),L"--show-only=json-v1"};
        for(const auto& arg:options.at("CTestFilterArguments").items)args.push_back(wide(arg.str()));
        const auto queried=run(args,repo);
        require(queried.code==0,"Actual "+mode+" CTest query failed: "+queried.out+queried.err);
        const auto inventory=Json::parse(queried.out).at("tests");
        require(!inventory.items.empty(),"Actual preset selected zero tests for mode "+mode);
        std::set<std::string> observed;
        for(const auto& test:inventory.items)
            require(observed.insert(test.at("name").str()).second,"Actual preset repeated a test for mode "+mode);
        require(observed==expected,"Actual "+mode+" selection differs from declared labels; expected="+
            describe(expected)+"; observed="+describe(observed));
    }
    for(const auto& mode:{"name","label"}){request["mode"]=mode;request["text"]="";expectError("options","non-empty");}
    request["expected"]=Json::array({"a"});request["xml"]="<testsuite><testcase name='a' status='run'/></testsuite>";require(probe("report").at("error").str().empty(),"Valid report rejected");request["xml"]="<testsuite/>";expectError("report","no executed test cases");for(const std::string status:{"<skipped/>","<failure/>","<error/>"}){request["xml"]="<testsuite><testcase name='a'>"+status+"</testcase></testsuite>";expectError("report","not fully verified");}request["xml"]="<testsuite><testcase name='a' status='notrun'/></testsuite>";expectError("report","not fully verified");for(const auto& xml:{"<testsuite><testcase name='b'/></testsuite>","<testsuite><testcase name='a'/><testcase name='a'/></testsuite>"}){request["xml"]=xml;expectError("report","does not match");}
    write(root/L".build/Release/tests/SnowDesktopFixtureTests.exe","fixture");fs::create_directories(root/L".build/Release/SnowDesktop.Runtime/payload");require(probe("output").at("error").str().empty(),"Valid isolated output rejected");for(const auto& name:{"SnowDesktopMisplacedTests.exe","misplaced.dll"}){write(root/L".build/Release"/name,"fixture");expectError("output","escaped its dedicated");fs::remove(root/L".build/Release"/name);}fs::create_directories(root/L".build/Release/payload");expectError("output","escaped its dedicated");fs::remove(root/L".build/Release/payload");fs::remove(root/L".build/Release/tests/SnowDesktopFixtureTests.exe");expectError("output","escaped its dedicated");
    auto presets=Json::parse(read(repo/L"CMakePresets.json"));bool jobs=false;for(const auto& preset:presets.at("testPresets").items)if(preset.at("name").str()=="all-tests")jobs=preset.at("execution").at("jobs").integer()>1;require(jobs,"Common test preset must enable bounded parallel execution");
    for(const auto& preset:presets.at("buildPresets").items){const auto name=preset.at("name").str();if(name=="tests")require(preset.at("targets").dump()==Json::array({"SnowDesktopAppTests"}).dump(),"Routine build compiles tool targets");if(name=="tools-tests")require(preset.at("targets").dump()==Json::array({"SnowDesktopBuildToolTests"}).dump(),"Tools build includes unrelated application targets");if(name=="all-tests")require(preset.at("targets").dump()==Json::array({"SnowDesktopTests"}).dump(),"Unified native inventory aggregate missing");}
    std::cout<<"PASS cold target selection, aliases, runtime guard, actual application/tools presets, isolation, reports and parallel jobs\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,run_tests);}
