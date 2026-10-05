#include "build_tool_test_support.h"
#include <iostream>
namespace build_test {
void shell_recovery_tests(const fs::path& repo){
    for(const std::string mode:{"configure-failure","normal","interrupted"}){
        auto root=temporary("shell-"+mode);copy(repo,root,{"scripts/powershell_runtime.bat","scripts/build_entry.ps1","scripts/build_preflight.ps1","scripts/build_job.cs","scripts/build.bat"});
        fs::create_directories(root/L"bin");fs::copy_file(standin(),root/L"bin/cmake.exe");
        write(root/L"scripts/arrange_build_output.ps1","param($BuildOutput)\nexit 0\n",true);
        write(root/L"restored-shell.ps1","[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'shell-token.txt'),[string]$env:SNOWDESKTOP_EXECUTION_TOKEN)\nStart-Sleep -Seconds 60\n",true);
        auto preflight=read(root/L"scripts/build_preflight.ps1");replace(preflight,"if($MyInvocation.InvocationName -ne '.')",R"PS(
function Get-ReadOnlyPreflight([string]$Root,[string]$Configuration='Release') {
 $owners=@();if(-not [IO.File]::Exists((Join-Path $Root 'restored.json'))){$owners=@([pscustomobject]@{pid=12345;kind='hook';startTicks='639000000000000000'})}
 [pscustomobject]@{status=$(if($owners.Count){'blocked'}else{'clear'});owners=$owners;unknownPids=@()}
}
function Get-Process {param($Id,$Name,$ErrorAction)
 if($Id -eq 12345){return [pscustomobject]@{Id=12345;StartTime=[DateTime]::SpecifyKind((New-Object DateTime 639000000000000000),[DateTimeKind]::Utc)}}
 Microsoft.PowerShell.Management\Get-Process @PSBoundParameters
}
function Stop-Process {param($InputObject,$ErrorAction)
 if($InputObject.Id -ne 12345){throw 'Fixture refuses real process termination'}
 [IO.File]::WriteAllText((Join-Path $PSScriptRoot '../mock-stop'),'observed hook only')
}
function Start-Process {param($FilePath,$WindowStyle)
 if([IO.Path]::GetFileName($FilePath) -ne 'explorer.exe'){throw 'Fixture refuses real Shell action'}
 $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
 $child=Microsoft.PowerShell.Management\Start-Process -FilePath (Get-BuildPowerShell) -ArgumentList ('-NoProfile -File "'+(Join-Path $root 'restored-shell.ps1')+'"') -WindowStyle Hidden -PassThru
 [IO.File]::WriteAllText((Join-Path $root 'restored.json'),(ConvertTo-Json @{pid=$child.Id;startTicks=$child.StartTime.ToUniversalTime().Ticks.ToString()}))
}
if($MyInvocation.InvocationName -ne '.')
)PS");write(root/L"scripts/build_preflight.ps1",preflight,true);
        Env env{{L"PATH",(root/L"bin").wstring()+L";"+environment(L"PATH")},{L"SNOWDESKTOP_STANDIN_MODE",mode=="configure-failure"?L"configure-failure":L"shell"}};
        if(mode=="interrupted")write(root/L"hold-native","hold");
        Child process({powershell().wstring(),L"-NoProfile",L"-File",(root/L"scripts/build_entry.ps1").wstring(),L"-Action",L"release",L"-ReloadShell"},root,env);
        Json shell,native;
        try{
            if(mode=="interrupted"){until([&]{return fs::exists(root/L"native-started")||!process.running();});require(fs::exists(root/L"native-started"),"Native boundary did not start: "+process.err());process.stop();}
            auto result=process.wait(25);require(mode=="interrupted"?result.code!=0:result.code==(mode=="configure-failure"?19:0),"Shell scenario exit mismatch "+mode+result.out+result.err);
            shell=Json::parse(read(root/L"restored.json"));require(owner_alive(shell),"Restored shell surrogate died with private job");until([&]{return fs::exists(root/L"shell-token.txt");});require(read(root/L"shell-token.txt").empty(),"Restored shell retained execution authority");
            if(mode!="configure-failure"){
                require(read(root/L"native-token.txt").size()==64,"Native child lost delegated authority");native=Json::parse(read(root/L"native-child.json"));until([&]{return !owner_alive(native);},5);
            }else{
                write(root/L"descendant-preflight.ps1",R"PS($ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1');$entry=Enter-BuildEntry $PSScriptRoot
try{Add-Type -Path (Join-Path $PSScriptRoot 'scripts/build_job.cs');$command='"'+(Get-BuildPowerShell)+'" -NoProfile -File "'+(Join-Path $PSScriptRoot 'scripts/build_preflight.ps1')+'" -ReloadShell';exit [SnowDesktop.Build.Job]::RunLeasedCommand($PSScriptRoot,(Join-Path $PSScriptRoot 'descendant-preflight.log'),$command)}finally{Exit-BuildEntry $entry}
)PS",true);auto original=read(root/L"restored.json");auto blocked=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"descendant-preflight.ps1").wstring()},root);require(blocked.code!=0&&read(root/L"restored.json")==original&&read(root/L"descendant-preflight.log").find("live execution lease owner")!=std::string::npos,"Descendant obtained destructive preflight authority");
            }
        }catch(...){for(const auto& path:{root/L"restored.json",root/L"native-child.json"})if(fs::exists(path))stop_owner(Json::parse(read(path)));throw;}
        stop_owner(shell);stop_owner(native);std::cout<<"PASS shell recovery "<<mode<<"\n";
    }
}
}
