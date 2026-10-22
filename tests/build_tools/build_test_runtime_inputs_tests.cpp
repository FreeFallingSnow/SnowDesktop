#include "../build_tool_test_support.h"
#include <iostream>
#include <set>
using namespace build_test;
namespace {
const Json& named(const Json& rows,const std::string& name){for(const auto& row:rows.items)if(row.at("test").str()==name)return row;throw std::runtime_error("Missing runtime test: "+name);}
}
void runtime_input_fixture(const fs::path& root){
    require(read(root/L"data/message.txt")=="allowed","Semantic runtime file changed the program result");
    std::set<std::wstring> names;for(const auto& entry:fs::directory_iterator(root/L"icons"))names.insert(entry.path().filename().wstring());
    require(names==std::set<std::wstring>{L"001_a.png",L"002_b.png"},"Runtime directory shape changed the program result");
    require(!fs::is_directory(root/L"preferred"),"An earlier lookup directory appeared");
    require(!fs::exists(root/L"optional.txt"),"An absent optional runtime file appeared");
    require(read(root/L"tree/nested/value.txt")=="expected","Nested runtime content changed the program result");
}
void runtime_input_tests(const fs::path& repo,bool omitDirectories){
    const auto root=temporary("runtime 输入");
    copy(repo,root,{"scripts/build_test_dependencies.ps1","scripts/build_test_runtime_inputs.ps1"});
    fs::copy_file(repo/L"tests/build_tools/build_test_runtime_inputs_tests.cpp",root/L"runtime_rules.cpp");
    if(omitDirectories){auto text=read(root/L"scripts/build_test_runtime_inputs.ps1");replace(text,"$row.entries=@($directory.entries)","$row.entries=@()");write(root/L"scripts/build_test_runtime_inputs.ps1",text,true);}
    write(root/L"data/message.txt","allowed");write(root/L"icons/001_a.png","first");write(root/L"icons/002_b.png","second");
    write(root/L"tree/nested/value.txt","expected");write(root/L"independent.txt","independent");
    write(root/L"setup.ps1",R"PS(
$pin=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'runtime_rules.cpp')).Hash
$review=@(@{path='runtime_rules.cpp';sha256=$pin})
$alpha=@{test='Alpha';kind='reviewed-file-inputs-v1';reviewedSources=$review;inputs=@(
 @{kind='file';path='data/message.txt'},@{kind='directory-entries';path='icons'},
 @{kind='directory-presence';path='preferred';required=$false},@{kind='file';path='optional.txt';required=$false},
 @{kind='directory-tree';path='tree'})}
$beta=@{test='Beta';kind='reviewed-file-inputs-v1';reviewedSources=$review;inputs=@(@{kind='file';path='independent.txt'})}
@{schemaVersion=1;tests=@($alpha,$beta)}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $PSScriptRoot 'scripts/test_runtime_contracts.json') -Encoding UTF8
@{created=$true}|ConvertTo-Json
)PS",true);
    write(root/L"inspect.ps1",R"PS(param([string]$Changed='data/message.txt',[switch]$Capture)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_test_dependencies.ps1')
$context=[pscustomobject]@{root=$PSScriptRoot;files=@{}}
$alpha=Get-TestRuntimeInputs $context 'Alpha';$beta=Get-TestRuntimeInputs $context 'Beta';$unknown=Get-TestRuntimeInputs $context 'Unknown'
$rows=@([pscustomobject]@{test='Alpha';target='Alpha';status='observed';fileIds=$alpha.fileIds;runtimeInputs=$alpha},[pscustomobject]@{test='Beta';target='Beta';status='observed';fileIds=$beta.fileIds;runtimeInputs=$beta})
$manifest=[pscustomobject]@{root=$PSScriptRoot;tests=$rows;files=@($context.files.Keys|ForEach-Object {@{id=$_}})}
$path=Join-Path $PSScriptRoot 'before.json';$reference=Join-Path $PSScriptRoot 'before-reference.json'
if($Capture){[IO.File]::WriteAllText($path,($manifest|ConvertTo-Json -Depth 15),(New-Object Text.UTF8Encoding($false)));@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}|ConvertTo-Json|Set-Content -LiteralPath $reference -Encoding UTF8}
$check=Compare-TestRuntimeInputs $PSScriptRoot @(@{name='Alpha'},@{name='Beta'}) (Get-Content -LiteralPath $reference -Raw|ConvertFrom-Json)
@{alpha=$alpha;beta=$beta;unknown=$unknown;advice=(Get-TestDependencyAdvice $manifest @($Changed));check=$check}|ConvertTo-Json -Depth 18
)PS",true);
    Bridge bridge(repo,root);bridge.call(root/L"setup.ps1",{});
    const auto inspect=[&](const std::string& changed="data/message.txt"){return bridge.call(root/L"inspect.ps1",{"-Changed",changed});};
    const auto program=[&]{return run({executable().wstring(),root.wstring(),L"--runtime-fixture"},root);};
    const auto baseline=bridge.call(root/L"inspect.ps1",{"-Capture"});
    require(program().code==0,"Initial runtime business fixture failed");
    require(baseline.at("alpha").at("status").str()=="observed"&&baseline.at("alpha").at("sourceReview").str()=="valid"&&baseline.at("unknown").at("status").str()=="unreviewed","Declared/unknown runtime contracts were confused: "+baseline.dump());
    require(named(baseline.at("check").at("tests"),"Alpha").at("status").str()=="stable"&&!baseline.at("alpha").at("reuseAllowed").boolean&&baseline.at("alpha").at("runtimeClosure").str()=="unclosed","Observed file inputs falsely closed runtime dependencies");
    const auto alphaDigest=baseline.at("alpha").at("digest").str(),betaDigest=baseline.at("beta").at("digest").str();
    write(root/L"data/message.txt","blocked");const auto semantic=inspect();
    require(program().code==1&&semantic.at("alpha").at("digest").str()!=alphaDigest&&semantic.at("beta").at("digest").str()==betaDigest,"Semantic file read did not change both runtime behavior and its isolated identity");
    require(named(semantic.at("check").at("tests"),"Alpha").at("status").str()=="invalidated"&&named(semantic.at("check").at("tests"),"Beta").at("status").str()=="stable","File-input endpoint comparison lost its affected test");
    require(named(semantic.at("advice").at("tests"),"Alpha").at("recommendRun").boolean&&!named(semantic.at("advice").at("tests"),"Beta").at("recommendRun").boolean,"Runtime file advice invalidated an independent contract");
    write(root/L"data/message.txt","allowed");write(root/L"icons/003_c.png","new");const auto shape=inspect("icons/003_c.png");
    require(program().code==1&&shape.at("alpha").at("digest").str()!=alphaDigest,"Directory entries did not affect the runtime fingerprint");
    require(shape.at("advice").at("unmappedPaths").items.empty()&&named(shape.at("advice").at("tests"),"Alpha").at("recommendRun").boolean&&!named(shape.at("advice").at("tests"),"Beta").at("recommendRun").boolean,"New directory member became an unrelated global change");
    fs::remove(root/L"icons/003_c.png");write(root/L"icons/001_a.png","different image bytes");
    require(program().code==0&&inspect().at("alpha").at("digest").str()==alphaDigest,"Directory-entry semantics incorrectly depended on file contents");
    fs::create_directory(root/L"preferred");require(program().code==1&&inspect("preferred/new.txt").at("alpha").at("digest").str()!=alphaDigest,"An absent directory lookup was not fingerprinted");fs::remove(root/L"preferred");
    write(root/L"optional.txt","appeared");require(program().code==1&&inspect("optional.txt").at("alpha").at("digest").str()!=alphaDigest,"Optional absence was confused with an unobserved input");fs::remove(root/L"optional.txt");
    write(root/L"tree/nested/value.txt","changed");require(program().code==1&&inspect("tree/nested/value.txt").at("alpha").at("digest").str()!=alphaDigest,"Recursive runtime content was omitted");write(root/L"tree/nested/value.txt","expected");
    const auto source=read(root/L"runtime_rules.cpp");write(root/L"runtime_rules.cpp",source+"\n// changed reader\n");const auto stale=inspect();require(stale.at("alpha").at("status").str()=="incomplete"&&stale.at("alpha").at("sourceReview").str()=="stale","Reader source changed without invalidating its review");write(root/L"runtime_rules.cpp",source);
    Bridge ps5(repo,root,true);const auto legacy=ps5.call(root/L"inspect.ps1",{});require(legacy.at("alpha").at("digest").str()==alphaDigest&&legacy.at("beta").at("digest").str()==betaDigest,"PowerShell5.1 and7 runtime fingerprints differ");
    const auto before=read(root/L"before.json");write(root/L"before.json",before+" ");require(inspect().at("check").at("status").str()=="unavailable","Changed baseline bytes were trusted");write(root/L"before.json",before);
    const auto registryPath=root/L"scripts/test_runtime_contracts.json";auto registry=read(registryPath);if(registry.starts_with("\xef\xbb\xbf"))registry.erase(0,3);
    write(registryPath,"invalid json");require(inspect().at("alpha").at("status").str()=="incomplete","Invalid registry silently authorized file coverage");
    auto escaped=Json::parse(registry);escaped["tests"].items[0]["inputs"].items[0]["path"]="../outside.txt";write(registryPath,escaped.dump());require(inspect().at("alpha").at("status").str()=="incomplete","Runtime contract escaped the repository boundary");
    auto unknown=Json::parse(registry);unknown["tests"].items[0]["kind"]="closed-by-label";write(registryPath,unknown.dump());require(inspect().at("alpha").at("status").str()=="incomplete","Unsupported runtime contract was accepted");
    write(registryPath,registry);fs::rename(registryPath,registryPath.wstring()+L".saved");require(inspect().at("alpha").at("status").str()=="unreviewed","Missing registry retained earlier imported contracts");fs::rename(registryPath.wstring()+L".saved",registryPath);
    require(program().code==0&&inspect().at("alpha").at("digest").str()==alphaDigest,"Restored runtime inputs did not restore the original identity");
    std::cout<<"PASS semantic files, directory entries/content, absent lookups, source reviews, endpoint invalidation, advice, PowerShell5.1/7 and conservative unknown/error behavior\n";
}
