#include "../build_tool_test_support.h"
#include <iostream>

using namespace build_test;
namespace {
fs::path fixture_parent(const fs::path& repo) {
    const auto result=run({L"git",L"rev-parse",L"--path-format=absolute",L"--git-common-dir"},repo);
    auto text=result.out;while(!text.empty()&&(text.back()=='\r'||text.back()=='\n'))text.pop_back();
    const auto common=fs::path(wide(text));
    return (result.code==0&&common.filename()==L".git"?common.parent_path():repo)/L".codex-probes/cnd";
}
void prepare(Coordinator& c,const fs::path& repo,const std::string& control) {
    copy(repo,c.root,{"scripts/build_candidate.ps1"});
    if(!control.empty()) {
        auto module=read(c.scripts/L"build_candidate.ps1");
        if(control=="live-inputs") {
            replace(module,"[IO.File]::Copy($object,$path,$true)","[IO.File]::Copy((Join-Path $manifest.sourceRoot $file.path),$path,$true)");
            replace(module,"if((Get-FileDigest $path) -ne $file.sha256){throw 'Candidate copied content failed integrity verification.'}","# Controlled legacy live-source copy has no frozen-byte check.");
        }else if(control=="per-snapshot-slot") {
            replace(module,"$key=$Snapshot.manifest.context.digest.Substring(0,16)+'-'+$Name", "$key=$Snapshot.manifest.id.Substring(0,16)+'-'+$Name");
        }else{require(false,"Unknown candidate control");}
        write(c.scripts/L"build_candidate.ps1",module,true);
    }
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+
        "\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Candidate fixture has no optional dashboard'}}\n",true);
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/CMakeLists.txt\n!/README.md\n/CMakeUserPresets.json\n/generated/\n");
    write(c.root/L"README.md","Candidate fixture documentation is part of the snapshot.\n");
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(FrozenCandidate LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_executable(CandidateProgram src/main.cpp)
target_compile_features(CandidateProgram PRIVATE cxx_std_20)
target_compile_options(CandidateProgram PRIVATE /W4 /WX /Z7)
)CMAKE");
    write(c.root/L"CMakePresets.json","{\"version\":3}\n");
    write(c.root/L"src/main.cpp","int main(){return 0;}\n");
    write(c.root/L"src/common.h","#pragma once\ninline constexpr int base=4;\n");
    write(c.root/L"src/obsolete.txt","tracked file deleted before capture\n");
    require(c.git({L"add",L"--all"}).code==0&&c.git({L"commit",L"-m",L"Fixture baseline"}).code==0,"Cannot establish candidate fixture baseline");
    write(c.root/L"CMakeUserPresets.json","{\"version\":3}\n");
    write(c.root/L"generated/seed.h","#pragma once\ninline constexpr int seed=3;\n");
    write(c.root/L"src/new.h","#pragma once\ninline constexpr int extra=1;\n");
    write(c.root/L"src/main.cpp","#include \"common.h\"\n#include \"new.h\"\n#include \"../generated/seed.h\"\nint main(){return base+seed+extra;}\n");
    fs::remove(c.root/L"src/obsolete.txt");
    c.call({"task-open","snap","-TaskName","Frozen source snapshot fixture"});
    c.gate("preflight-blocked");
}
std::string start(Coordinator& c,const std::string& extra="generated/seed.h",const Env& env={}) {
    const auto result=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","start","-Task","snap","-Action","snapshot","-SnapshotInputs",extra},0,env);
    const auto id=result.at("id").str();const auto receipt=c.stateRoot/L"runs"/wide(id+".launch.json");
    until([&]{return fs::exists(receipt);},12);c.workers.push_back(Json::parse(read(receipt)));return id;
}
Json finish(Coordinator& c,const std::string& id,int code=0) {
    const auto result=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","snap","-RunId",id,"-Seconds","60"},{},70);
    require(result.code==code,"Snapshot Run returned unexpected code: "+result.out+result.err);
    const auto view=Json::parse(result.out);
    require(view.at("terminal").boolean&&view.at("stageError").str().empty(),"Snapshot Run did not retain complete terminal observations");
    return view;
}
Json capture(Coordinator& c) {
    const auto view=finish(c,start(c));
    require(view.at("status").str()=="passed"&&view.at("record").has("candidate"),"Snapshot Run lost its candidate artifact");
    const auto& candidate=view.at("record").at("candidate");
    require(candidate.at("snapshotReady").boolean&&!candidate.at("executionReady").boolean&&!candidate.at("externalDependenciesClosed").boolean,"Snapshot preparation overstated execution or dependency closure");
    require(!view.at("record").at("resourceAdmission").at("required").boolean,"Snapshot unexpectedly occupied compiler capacity");
    require(!fs::exists(c.root/L"preflight-arguments.txt")&&c.calls()==0,"Snapshot capture performed preflight/build process actions");
    return candidate;
}
Json inspect(Coordinator& c,const std::string& id) {return c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","inspect","-Snapshot",id});}
Json materialize(Coordinator& c,const std::string& id) {return c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","materialize","-Snapshot",id,"-Task","snap","-Slot","release"});}
const Json& input(const Json& manifest,const std::string& path) {
    for(const auto& file:manifest.at("files").items)if(file.at("path").str()==path)return file;
    require(false,"Snapshot omitted input: "+path);return manifest;
}
void command(const std::vector<std::wstring>& args,const fs::path& cwd) {
    const auto result=run(args,cwd,40);
    require(result.code==0,"Actual candidate compiler command failed: "+result.out+result.err);
    require(result.out.find("warning C")==std::string::npos&&result.out.find("warning D")==std::string::npos&&result.out.find("warning LNK")==std::string::npos,"Candidate compiler/linker warning: "+result.out+result.err);
}
void build(const Json& slot,bool configure=false) {
    const auto source=fs::path(wide(slot.at("source").str())),output=fs::path(wide(slot.at("output").str()));
    if(configure)command({L"cmake",L"-S",source.wstring(),L"-B",output.wstring(),L"-G",L"Visual Studio 18 2026",L"-A",L"x64"},source);
    command({L"cmake",L"--build",output.wstring(),L"--config",L"Release",L"--target",L"CandidateProgram",L"--",L"/nr:false"},source);
}
void program(const Json& slot,int expected) {
    const auto output=fs::path(wide(slot.at("output").str()));
    const auto result=run({(output/L"Release/CandidateProgram.exe").wstring()},output);
    require(result.code==expected,"Frozen candidate program used live/incorrect inputs: "+std::to_string(result.code));
}
void rejects(Coordinator& c,const std::vector<std::string>& args,const std::string& reason,const Env& env={}) {
    const auto result=c.bridge->invoke(c.scripts/L"build_candidate.ps1",args,env);
    require(result.code==2&&result.err.find(reason)!=std::string::npos,"Candidate rejection missed its boundary: "+result.out+result.err);
}
void metadata_checks(Coordinator& c,const Json& candidate,const Json& manifest) {
    const auto id=candidate.at("id").str();const auto probe=c.root/L"read-manifest.ps1";
    write(probe,R"PS(param([string]$Id,[string]$Relative)
$ErrorActionPreference='Stop'
. $PSScriptRoot/scripts/build_entry.ps1
. $PSScriptRoot/scripts/build_protocol.ps1
. $PSScriptRoot/scripts/build_inputs.ps1
. $PSScriptRoot/scripts/build_candidate.ps1
try {
 if($PSBoundParameters.ContainsKey('Relative')){@{relative=(Get-CandidateRelativePath $Relative)}|ConvertTo-Json}
 else{$snapshot=Read-CandidateManifest $PSScriptRoot $Id;@{id=$snapshot.manifest.id;digest=$snapshot.digest;objectsVerified=$false}|ConvertTo-Json}
}catch{[Console]::Error.WriteLine($_.Exception.Message);exit 2}
)PS",true);
    require(c.bridge->call(probe,{"-Relative","src\\nested/file.cpp"}).at("relative").str()=="src/nested/file.cpp","Candidate relative path normalization changed");
    for(const std::string invalid:{"","../outside","./source","src//a","src/a.","src/a ","C:/outside","src/COM1.txt","src/a:stream","src/<","src/*"}){
        const auto result=c.bridge->invoke(probe,{"-Relative",invalid});
        require(result.code==2&&result.err.find("Unsafe candidate relative path")!=std::string::npos,"Candidate metadata accepted an unsafe relative path");
    }
    const auto outside=temporary("metadata-outside");write(outside/L"keep.txt","foreign content unchanged\n");
    const auto validation=c.root/L".build/candidate-slots/validation/source";
    fs::create_directories(validation.parent_path());
    const auto linked=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(validation)+" -Target "+ps_literal(outside)+" -ErrorAction Stop | Out-Null")},c.root);
    require(linked.code==0,"Cannot establish unrelated validation junction: "+linked.err);
    require(c.bridge->call(probe,{"-Id",id}).at("id").str()==id,"Metadata-only read depended on an unrelated validation filesystem path");
    require(fs::remove(validation),"Cannot remove owned validation junction");
    const auto object=fs::path(wide(candidate.at("manifest").str())).parent_path().parent_path()/L"objects"/wide(input(manifest,"src/common.h").at("sha256").str());
    const auto retained=fs::path(object.wstring()+L".retained");fs::rename(object,retained);
    const auto objectLink=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(object)+" -Target "+ps_literal(outside)+" -ErrorAction Stop | Out-Null")},c.root);
    require(objectLink.code==0,"Cannot establish actual object reparse fixture: "+objectLink.err);
    require(c.bridge->call(probe,{"-Id",id}).at("id").str()==id,"Metadata-only read accessed an unused content object");
    rejects(c,{"-Command","inspect","-Snapshot",id},"cannot traverse a reparse point");
    const DWORD attributes=GetFileAttributesW(object.c_str());
    require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&RemoveDirectoryW(object.c_str()),"Cannot remove owned object junction");
    fs::rename(retained,object);
    require(inspect(c,id).at("id").str()==id&&read(outside/L"keep.txt")=="foreign content unchanged\n","Object verification did not recover or changed foreign content");
}
void scenario(const fs::path& repo,const std::string& control) {
    Coordinator c(repo,"s",false,fixture_parent(repo));prepare(c,repo,control);
    const auto a=capture(c);const auto aId=a.at("id").str();const auto manifest=inspect(c,aId);
    require(input(manifest,"src/main.cpp").at("state").str()=="present"&&input(manifest,"src/new.h").at("state").str()=="present"&&input(manifest,"src/obsolete.txt").at("state").str()=="deleted","Dirty/new/deleted files were not captured");
    input(manifest,"CMakeUserPresets.json");input(manifest,"generated/seed.h");input(manifest,"README.md");
    if(control.empty())metadata_checks(c,a,manifest);
    write(c.root/L"src/main.cpp","int main(){return 19;}\n");
    write(c.root/L"generated/seed.h","#pragma once\ninline constexpr int seed=9;\n");
    write(c.root/L"src/obsolete.txt","restored only in the live tree\n");
    const auto first=materialize(c,aId);const auto source=fs::path(wide(first.at("source").str())),output=fs::path(wide(first.at("output").str()));
    require(!fs::exists(source/L"src/obsolete.txt")&&read(source/L"README.md").find("documentation")!=std::string::npos,"Snapshot deleted/documentation state changed after live edits");
    build(first,true);program(first,8);
    if(control=="live-inputs")return;
    const auto exe=output/L"Release/CandidateProgram.exe";
    const auto exeTime=fs::last_write_time(exe),sourceTime=fs::last_write_time(source/L"src/main.cpp"),headerTime=fs::last_write_time(source/L"src/common.h");
    write(output/L"candidate-cache-sentinel.txt","preserve unrelated build cache\n");
    const auto same=materialize(c,aId);require(same.at("copied").integer()==0&&same.at("kept").integer()>0,"Same snapshot copied unchanged inputs");
    build(same);program(same,8);
    require(fs::last_write_time(exe)==exeTime&&fs::last_write_time(source/L"src/main.cpp")==sourceTime,"Same snapshot rebuilt unchanged program or input");
    fs::remove(c.root/L"src/new.h");write(c.root/L"src/newer.txt","new candidate input\n");
    write(c.root/L"src/main.cpp","#include \"common.h\"\n#include \"../generated/seed.h\"\nint main(){return base+seed+2;}\n");
    const auto b=capture(c);const auto bId=b.at("id").str();require(b.at("objectsReused").integer()>0,"Unchanged content objects were not reused");
    const auto next=materialize(c,bId);
    require(next.at("source").str()==first.at("source").str()&&next.at("output").str()==first.at("output").str(),"Changed source moved the stable candidate slot");
    require(!fs::exists(source/L"src/new.h")&&fs::exists(source/L"src/newer.txt")&&fs::exists(source/L"src/obsolete.txt"),"Candidate slot did not apply added/restored/deleted inputs");
    require(read(output/L"candidate-cache-sentinel.txt")=="preserve unrelated build cache\n"&&fs::last_write_time(source/L"src/common.h")==headerTime,"Candidate materialization discarded unrelated build cache or untouched input time");
    build(next);program(next,15);
    if(control=="per-snapshot-slot")return;
    rejects(c,{"-Command","materialize","-Snapshot",bId},"active registered Task");
    rejects(c,{"-Command","materialize","-Snapshot",bId,"-Task","snap","-Slot","release"},"observed tools/environment changed",{{L"SNOWDESKTOP_STEAMWORKS_SDK_ROOT",L"X:/changed-candidate-context"}});
    const auto savedMain=read(source/L"src/main.cpp");write(source/L"src/main.cpp","manual edit must survive\n");
    rejects(c,{"-Command","materialize","-Snapshot",bId,"-Task","snap","-Slot","release"},"modified outside its lease");
    require(read(source/L"src/main.cpp")=="manual edit must survive\n","Candidate rejection overwrote manual input");write(source/L"src/main.cpp",savedMain);
    write(c.root/L"hold-slot.ps1",R"PS(param([string]$Id)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_protocol.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_inputs.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate.ps1')
$snapshot=Read-CandidateManifest $PSScriptRoot $Id -VerifyObjects
$slot=Enter-CandidateSlot $snapshot 'release'
try{
 $null=Set-CandidateSlotSource $snapshot $slot
 [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'slot-ready'),'ready')
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while((Test-Path (Join-Path $PSScriptRoot 'hold-slot')) -and $clock.Elapsed.TotalSeconds -lt 15){Start-Sleep -Milliseconds 30}
}finally{$slot.lease.Dispose()}
)PS",true);
    write(c.root/L"hold-slot","hold\n");
    auto holder=std::make_unique<Child>(std::vector<std::wstring>{powershell().wstring(),L"-NoProfile",L"-File",(c.root/L"hold-slot.ps1").wstring(),L"-Id",wide(bId)},c.root);
    until([&]{return fs::exists(c.root/L"slot-ready");},12);
    rejects(c,{"-Command","materialize","-Snapshot",aId,"-Task","snap","-Slot","release"},"slot is busy");
    build(next);program(next,15);
    fs::remove(c.root/L"hold-slot");require(holder->wait().code==0,"Candidate slot holder failed");
    const auto manifestPath=fs::path(wide(a.at("manifest").str()));const auto savedManifest=read(manifestPath);
    write(manifestPath,savedManifest+" ");rejects(c,{"-Command","inspect","-Snapshot",aId},"manifest integrity check failed");write(manifestPath,savedManifest);
    const auto object=manifestPath.parent_path().parent_path()/L"objects"/wide(input(manifest,"src/common.h").at("sha256").str());const auto savedObject=read(object);
    write(object,"corrupt content\n");rejects(c,{"-Command","inspect","-Snapshot",aId},"content object integrity check failed");write(object,savedObject);
    const auto sourceHandle=CreateFileW((c.root/L"src/common.h").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(sourceHandle!=INVALID_HANDLE_VALUE,"Cannot hold candidate input writer");
    Json failed;
    try{failed=finish(c,start(c),2);}catch(...){CloseHandle(sourceHandle);throw;}
    CloseHandle(sourceHandle);
    require(failed.at("status").str()=="failed"&&!failed.at("record").has("candidate"),"Capture published a candidate despite an active input writer");
    const auto registration=c.begin("editor");const auto batch=registration.at("batchId").str();const auto queuedId=start(c);
    until([&]{const auto view=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","status","-RunId",queuedId});return view.at("record").at("status").str()=="queued"&&!view.at("record").at("waitingFor").str().empty();});
    c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","cancel","-Task","snap","-RunId",queuedId});
    const auto cancelled=finish(c,queuedId,4);
    require(cancelled.at("status").str()=="cancelled"&&!cancelled.at("record").has("candidate"),"Queued snapshot cancellation lost its owner or published content");
    c.plan("editor",batch,0,"none","","tool","src/a.txt");c.finish("editor",batch);
    const auto legacy=finish(c,start(c,"generated/seed.h",{{L"SNOWDESKTOP_ENTRY_POWERSHELL",powershell(true).wstring()}}));
    require(legacy.at("status").str()=="passed"&&legacy.at("record").has("candidate"),"Windows PowerShell snapshot capture failed");
    const auto legacyId=legacy.at("record").at("candidate").at("id").str();
    const auto legacyInspect=run({powershell(true).wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_candidate.ps1").wstring(),L"-Command",L"inspect",L"-Snapshot",wide(legacyId)},c.root,25);
    require(legacyInspect.code==0&&Json::parse(legacyInspect.out).at("id").str()==legacyId,"Windows PowerShell snapshot inspection failed: "+legacyInspect.out+legacyInspect.err);
    const auto target=temporary("candidate-reparse-target");write(target/L"outside.txt","outside bytes must survive\n");
    const auto junction=c.root/L"src/escape";
    const auto linkCommand="New-Item -ItemType Junction -Path "+ps_literal(junction)+" -Target "+ps_literal(target)+" -ErrorAction Stop | Out-Null";
    const auto link=run({powershell().wstring(),L"-NoProfile",L"-Command",wide(linkCommand)},c.root);
    require(link.code==0,"Cannot establish candidate reparse boundary fixture: "+link.out+link.err);
    const auto escaped=finish(c,start(c,"src/escape/outside.txt"),2);
    require(escaped.at("status").str()=="failed"&&!escaped.at("record").has("candidate")&&read(target/L"outside.txt")=="outside bytes must survive\n","Candidate followed or modified an escaped source");
    fs::remove(junction);
    const auto invalid=finish(c,start(c,".build/forbidden-input.h"),2);
    require(invalid.at("status").str()=="failed"&&!invalid.at("record").has("candidate"),"Snapshot accepted shared output/state as source input");
    for(const auto& mutation:std::vector<std::pair<std::string,std::string>>{{"live-inputs","Frozen candidate program used live/incorrect inputs"},{"per-snapshot-slot","Changed source moved the stable candidate slot"}}){
        const auto result=run({executable().wstring(),repo.wstring(),wide("--"+mutation.first)},c.root,65,{},true);
        require(result.code==1&&result.err.find(mutation.second)!=std::string::npos,"Candidate negative control did not reach its business assertion: "+result.out+result.err);
    }
    std::cout<<"PASS durable snapshot capture; dirty/new/deleted/ignored inputs; frozen compiled program after live edits; stable incremental output; exclusive slot and corruption/context/manual-edit/reparse rejection; queued cancellation; both legacy controls fail\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){scenario(repo,argc>2?std::string(argv[2]).substr(2):"");});}
