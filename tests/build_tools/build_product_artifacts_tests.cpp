#include "../build_tool_test_support.h"
#include "product_deployment_fixture.h"
#include <iostream>

using namespace build_test;
namespace {
void build_routing(const fs::path& repo) {
    const auto root=temporary("packaging-build-routing");
    write(root/L"scripts/build_manager.ps1",R"PS(param($Command,$Participant,$TaskName,$TaskRevision,$TaskPhase)
$ErrorActionPreference='Stop'
$state=Join-Path $PSScriptRoot '../.build/collaboration/state.json'
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($state))
@{taskLedger=@{tasks=@(@{id=$Participant;revision=0})}}|ConvertTo-Json -Depth 5|Set-Content $state
)PS",true);
    write(root/L"scripts/build_run.ps1",R"PS(param($Command,$Action,$Task,$RunId,$CandidateAction,$Snapshot,$Seconds)
$ErrorActionPreference='Stop'
if($Command -eq 'wait'){Write-Output 'build output is not JSON';exit 0}
if($Command -ne 'start'){throw 'Use structured start receipts, not invoke logs'}
$id=if($Action -eq 'snapshot'){'c'*32}else{'d'*32}
$record=@{id=$id;status='passed';candidate=@{id=('f'*32)};artifact=@{id=$id}}
$path=Join-Path $PSScriptRoot ('../.build/collaboration/runs/'+$id+'.json')
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
$record|ConvertTo-Json -Depth 5|Set-Content $path
if($Action -eq 'candidate'){@{snapshot=$Snapshot;action=$CandidateAction}|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot '../built.json')}
@{id=$id}|ConvertTo-Json
)PS",true);
    const auto driver=root/L"routing.ps1";
    write(driver,"param($Root)\n$ErrorActionPreference='Stop'\n. "+ps_literal(repo/L"scripts/packaging_input.ps1")+
        "\n@{run=(Invoke-SnowDesktopPackagingBuild $Root)}|ConvertTo-Json\n",true);
    Bridge bridge(repo,root);
    const auto result=bridge.invoke(driver,{utf8(root.wstring())},{},30);
    require(result.code==0,"Packaging build routing failed: "+result.out+result.err);
    require(Json::parse(result.out).at("run").str()==std::string(32,'d')&&
        Json::parse(read(root/L"built.json")).at("snapshot").str()==std::string(32,'f')&&
        Json::parse(read(root/L"built.json")).at("action").str()=="release",
        "Default packaging lost the captured snapshot or producing Run identity");
}
void product_artifacts(const fs::path& repo, bool oldInventory) {
    const auto root=temporary("product-artifacts");
    init_git(root);
    copy(repo,root,{"version.json","LICENSE","THIRD_PARTY_NOTICES.md","README.md","README.en.md",
        "packaging/steam-identity.json","packaging/AppxManifest.xml.in","assets/icon/icon.png","assets/icon/icon_small.png",
        "third_party/everything/LICENSE.txt","third_party/fluentui-system-icons/LICENSE","third_party/microsoft-ui-xaml/LICENSE",
        "third_party/font-awesome/LICENSE.txt","third_party/imgui/LICENSE.txt","third_party/lua/LICENSE.txt",
        "third_party/minhook/LICENSE.txt","third_party/pinyin-data/LICENSE"});
    // Freeze the production toolset too; no fixture substitutes a collector,
    // deployment reader, snapshot store, source copy or payload copy.
    fs::create_directories(root/L"scripts");
    for(const auto& item:fs::directory_iterator(repo/L"scripts"))
        if(item.is_regular_file())fs::copy_file(item.path(),root/L"scripts"/item.path().filename(),fs::copy_options::overwrite_existing);
    if(oldInventory){
        auto source=read(root/L"scripts/build_candidate_artifacts.ps1");
        const auto begin=source.find("        # The deployment manifest is the inventory");
        const auto end=source.find("        foreach($name in @('SnowDesktop.exe'",begin);
        require(begin!=std::string::npos&&end!=std::string::npos,"Missing inventory control location");
        source.erase(begin,end-begin);write(root/L"scripts/build_candidate_artifacts.ps1",source,true);
    }
    const auto driver=root/L".codex-probes/product.ps1";
    write(driver,std::string(R"PS(param($Root)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $Root 'scripts/packaging_input.ps1')
)PS")+product_deployment_fixture()+R"PS(
# The fixture supplies completed compiler output and the execution-lease
# boundary. All file identities and snapshot/materialization logic are real.
function Assert-EntryExecutionOwner($Root) {}
function Get-CandidateEntryContext($Source) { return $script:productContext }
$first=$null;$second=$null;$slotPath=$null
foreach($generation in @('A','B')) {
    [IO.File]::WriteAllText((Join-Path $Root 'README.md'),'source-'+$generation)
    [IO.File]::WriteAllText((Join-Path $Root 'version.json'),'{"version":"1.2.3.0"}')
    $identity=Get-BuildInputIdentity $Root
    $captured=New-CandidateSnapshot $Root 'fixture' 1 'Release' '' $identity.digest {}
    $snapshot=Read-CandidateManifest $Root $captured.id
    $slot=Enter-CandidateSlot $snapshot 'same-workspace'
    try {
        $null=Set-CandidateSlotSource $snapshot $slot
        $output=Join-Path $slot.output 'Release'
        [void][IO.Directory]::CreateDirectory($output)
        New-TestProductDeployment $output $generation
        foreach($name in @('SnowDesktop.exe','snowwidget.exe','SnowDesktop.pdb','widgets/example/widget.lua','lang/en-US.json')) {
            $file=Join-Path $output $name
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
            [IO.File]::WriteAllText($file,$generation+':'+$name)
        }
        $run=if($generation -eq 'A'){'a'*32}else{'b'*32}
        $script:productContext=[pscustomobject]@{output=$slot.output;source=$slot.source;configuration='Release';
            runRoot=$Root;runId=$run;nonce='fixture';snapshot=$snapshot.manifest.id;manifestDigest=$snapshot.digest}
        $saved=Publish-CandidateRuntime $script:productContext 'batch-product'
        $record=[pscustomobject]@{id=$run;repositoryRoot=$Root;status='passed';exitCode=0;completedUtc=[DateTime]::UtcNow.ToString('o');
            inputStart=[pscustomobject]@{digest=$snapshot.manifest.sourceDigest};
            inputEnd=[pscustomobject]@{digest=$snapshot.manifest.sourceDigest};artifact=$saved}
        $path=Join-Path $Root ('.build/collaboration/runs/'+$run+'.json')
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path));Write-CandidateJson $record $path
        if($generation -eq 'A'){$first=$saved;$slotPath=$slot.directory}else{$second=$saved}
    } finally { $slot.lease.Dispose() }
}
$context=Get-SnowDesktopPackagingContext $Root ('a'*32)
$package=Join-Path $Root 'artifacts/v1.2.3.0/fixture-package'
$null=Copy-SnowDesktopDeploymentPayload -BuildOutput $context.runtime -Destination $package -RuntimeDirectory SnowDesktop.Runtime
Assert-SnowDesktopPackagingContext $context
@{saved=$first.runtime;second=$second.runtime;package=$package;source=$context.source;
    slot=$slot.directory;firstSlot=$slotPath;version=$context.version;provenance=$context.provenance}|ConvertTo-Json -Depth 5
)PS",true);
    Bridge bridge(repo,root,false,true);
    const auto result=bridge.invoke(driver,{utf8(root.wstring())},{},90);
    if(oldInventory){
        require(result.code!=0&&result.err.find("Deployment manifest file is missing")!=std::string::npos,
            "Old collector did not fail saved-deployment validation: "+result.out+result.err);return;
    }
    require(result.code==0,"Product artifact fixture failed: "+result.out+result.err);
    const auto data=Json::parse(result.out);
    require(data.at("firstSlot").str()==data.at("slot").str(),"Fixture did not reuse the compiler workspace");
    const auto saved=fs::path(wide(data.at("saved").str())),second=fs::path(wide(data.at("second").str()));
    for(const auto* name:{"SnowDesktop.winmd","App.xbf","SettingsShell.xbf","Assets/Fonts/Example.ttf",
        "Assets/Settings/Icons/example.svg","SnowDesktop.Runtime/Microsoft.ui.xaml.dll","SnowDesktop.pdb"}){
        require(read(saved/name)=="A:"+std::string(name),"Saved A omitted or changed "+std::string(name));
        require(read(second/name)=="B:"+std::string(name),"Second compiler output was not B");
    }
    const auto packaged=fs::path(wide(data.at("package").str()));
    require(read(packaged/L"SnowDesktop.winmd")=="A:SnowDesktop.winmd"&&
        read(packaged/L"Assets/Settings/Icons/example.svg")=="A:Assets/Settings/Icons/example.svg",
        "Packaging A after B mixed executable resources");
    require(read(fs::path(wide(data.at("source").str()))/L"README.md")=="source-A"&&read(root/L"README.md")=="source-B",
        "Packaging used live source instead of A's snapshot");
    require(!fs::exists(fs::path(wide(data.at("slot").str()))/L"source/artifacts"),"Packaging contaminated compiler source");
    // Corrupt both saved dependencies and source copies: neither may pass.
    const auto probe=root/L".codex-probes/probe.ps1";
    write(probe,"param($Root)\n$ErrorActionPreference='Stop'\n. (Join-Path $Root 'scripts/packaging_input.ps1')\nGet-SnowDesktopPackagingContext $Root ('a'*32)|Out-Null\n",true);
    const auto winmd=read(saved/L"SnowDesktop.winmd");write(saved/L"SnowDesktop.winmd","wrong-generation");
    require(bridge.invoke(probe,{utf8(root.wstring())}).code!=0,"Corrupt saved WinMD was accepted");write(saved/L"SnowDesktop.winmd",winmd);
    write(fs::path(wide(data.at("source").str()))/L"README.md","wrong-source");
    require(bridge.invoke(probe,{utf8(root.wstring())}).code!=0,"Corrupt saved source was accepted");
    const auto control=run({executable().wstring(),repo.wstring(),L"--old-inventory"},root,110,{},true);
    require(control.code==0,"Old-inventory negative control failed unexpectedly: "+control.out+control.err);
    std::cout<<"PASS complete deployment export, same-workspace A/B reuse, A payload/source packaging, corruption rejection and old collector failure\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){
    const bool control=argc==3&&std::string(argv[2])=="--old-inventory";
    if(!control)build_routing(repo);
    product_artifacts(repo,control);
});}
