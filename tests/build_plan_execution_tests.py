"""Exercise the real test manager against small compiled CMake/CTest stand-ins."""
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from powershell_runtime import powershell_executable, run as run_process
import tempfile

def main(repo):
    # Temporary repositories must obtain their own lease; the outer build's
    # credential is valid only for the real repository and its owner tree.
    os.environ.pop('SNOWDESKTOP_EXECUTION_TOKEN',None)
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-plan-execution-'));scripts=root/'scripts';scripts.mkdir();binroot=root/'fake-bin';binroot.mkdir()
    for file in ('build_job.cs','build_entry.ps1','test_manager.ps1','test_output.ps1','build_protocol.ps1'):shutil.copyfile(repo/'scripts'/file,scripts/file)
    manager=(scripts/'test_manager.ps1').read_text(encoding='utf-8-sig')
    start=manager.index('function Get-HostRuntimeLocks {');end=manager.index('function Assert-HostRuntimeAvailable {',start)
    manager=manager[:start]+"function Get-HostRuntimeLocks {if([IO.File]::Exists((Join-Path $repositoryRoot 'occupied-host'))){'controlled fixture host owner'}}\n\n"+manager[end:]
    (scripts/'test_manager.ps1').write_text(manager,encoding='utf-8-sig')
    (scripts/'arrange_build_output.ps1').write_text("param($BuildOutput,[switch]$AllowMissingFirstPartyRuntime)\n[IO.File]::AppendAllText((Join-Path $PSScriptRoot '../arrange.count'),\"arrange`n\")\nexit 0\n")
    definitions=(repo/'CMakeLists.txt').read_text(encoding='utf-8-sig')
    for target in ('SnowDesktopThemeWorkflowTests','SnowDesktopWidgetAuthorPreviewCliTests'):
        declaration=re.search(r'snow_add_test\(\s*'+target+r'\b.*?\)',definitions,re.S)
        # The helper's name is intentionally read from the real declaration.
        if not declaration:declaration=re.search(r'\w+\(\s*'+target+r'\b.*?\)',definitions,re.S)
        assert declaration and 'host-runtime' in declaration[0], target
    source=root/'standin.cs'
    source.write_text(r'''using System;using System.IO;using System.Linq;
class StandIn {
static string[] Names=new[]{"Alpha","Beta","theme_workflow","widget_author_preview_cli"};
static string Target(string n){return n=="theme_workflow"?"SnowDesktopThemeWorkflowTests":n=="widget_author_preview_cli"?"SnowDesktopWidgetAuthorPreviewCliTests":"SnowDesktop"+n+"Tests";}
static int Main(string[] args){
 string root=Directory.GetCurrentDirectory(), exe=Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
 string dir=Path.Combine(root,".build","Release","tests");Directory.CreateDirectory(dir);
 if(exe=="cmake") { if(args.Contains("--build")){File.AppendAllText(Path.Combine(root,"build.count"),"build\n");File.AppendAllText(Path.Combine(root,"build-args.txt"),string.Join(" ",args)+"\n");}foreach(string n in Names)File.WriteAllText(Path.Combine(dir,Target(n)+".exe"),"fixture binary "+n);Directory.CreateDirectory(Path.Combine(root,".build/Release/SnowDesktop.Runtime"));Console.WriteLine("mock configure/build");return 0; }
 if(args.Contains("--show-only=json-v1")){
 string json="{\"tests\":[";bool first=true;
 foreach(string n in Names){
   int r=Array.IndexOf(args,"-R");if(r>=0&&!System.Text.RegularExpressions.Regex.IsMatch(n,args[r+1]))continue;
   if(!first)json+=",";first=false;string path=Path.Combine(dir,Target(n)+".exe").Replace("\\","\\\\");
   string behavior=File.Exists("behavior")?File.ReadAllText("behavior"):"pass";bool host=n!="Alpha"&&n!="Beta";
   string command=behavior=="cold"?"":"\"command\":[\""+path+"\""+(host?",\""+Path.Combine(root,".build/Release/SnowDesktop.exe").Replace("\\","\\\\")+"\"":"")+"],";
   bool blocked=n=="Beta"&&File.Exists("behavior")&&File.ReadAllText("behavior")=="environment";
   json+="{\"name\":\""+n+"\","+command+"\"properties\":[{\"name\":\"LABELS\",\"value\":[\"core\""+(blocked?",\"environment-blocked\"":"")+(host&&behavior!="legacy"?",\"host-runtime\"":"")+"]},{\"name\":\"REQUIRED_FILES\",\"value\":[\""+path+"\"]}]}";
 }Console.WriteLine(json+"]}");return 0; }
 int output=Array.IndexOf(args,"--output-junit");if(output<0)return 8;
 string pattern=args[Array.IndexOf(args,"-R")+1];string xml="<testsuites><testsuite>";
 string mode=File.Exists("behavior")?File.ReadAllText("behavior"):"pass";
 foreach(string n in Names)if(System.Text.RegularExpressions.Regex.IsMatch(n,pattern))xml+="<testcase name='"+n+"' time='0.1'>"+(mode=="skipped"?"<skipped/>":mode=="failed"&&n=="Beta"?"<failure message='fixture failure'/>":"")+"</testcase>";
 File.WriteAllText(args[output+1],xml+"</testsuite></testsuites>");Console.WriteLine("mock test run "+mode);return mode=="failed"?17:0;
}}
''')
    csc=Path(os.environ['WINDIR'])/'Microsoft.NET/Framework64/v4.0.30319/csc.exe';assert csc.exists()
    for name in ('cmake','ctest'):
        p=run_process([str(csc),'/nologo','/target:exe','/out:'+str(binroot/(name+'.exe')),str(source)],capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
    state=root/'.build/collaboration';state.mkdir(parents=True);bid='c'*32
    request={'suites':['selected'],'tests':['Alpha'],'requiredFull':False,'reason':'independent fixture'}
    plan={'schemaVersion':1,'batchId':bid,'configuration':'Release','mode':'selected','suites':['selected'],'tests':['Alpha','Beta'],'tasks':[{'participant':'a','requirement':request},{'participant':'b','requirement':dict(request,tests=['Beta'])}]}
    def run(mode,expected):
        (root/'behavior').write_text(mode);(state/(bid+'.plan.json')).write_text(json.dumps(plan))
        env=dict(os.environ,PATH=str(binroot)+os.pathsep+os.environ['PATH'])
        p=run_process([powershell_executable(),'-NoProfile','-File',str(scripts/'test_manager.ps1'),'-Mode','plan','-PlanBatch',bid],cwd=str(root),env=env,capture_output=True,timeout=20)
        assert (p.returncode==0)==expected,(mode,p.stdout,p.stderr)
        return json.loads((state/(bid+'.coverage.json')).read_text(encoding='utf-8-sig'))
    coverage=run('pass',True);assert coverage['status']=='passed' and [x['status'] for x in coverage['tasks']]==['passed','passed']
    assert len(coverage['testExecutablesBeforeRun'])==2 and all(len(x['sha256'])==64 for x in coverage['testExecutablesBeforeRun'])
    coverage=run('failed',False);assert coverage['status']=='failed' and coverage['tasks'][1]['failed']==['Beta'] and coverage['tasks'][0]['status']=='passed'
    coverage=run('skipped',False);assert coverage['status']=='failed' and all(x['status']=='not-run' for x in coverage['tasks'])
    before=(root/'build.count').read_bytes()
    coverage=run('environment',False);assert coverage['status']=='environment-blocked' and coverage['selected']==['Alpha','Beta'] and coverage['blocked']==['Beta'] and coverage['completed']==[]
    assert coverage['tasks'][1]['status']=='environment-blocked' and before==(root/'build.count').read_bytes(),'Unavailable required tests must block before target compilation'
    plan['tests']=['Missing'];p=None
    coverage=run('pass',False);assert coverage['status']=='not-run' and coverage['selectionStatus']=='pending' and coverage['selected']==[] and coverage['completed']==[]
    for name,target in (('theme_workflow','SnowDesktopThemeWorkflowTests'),('widget_author_preview_cli','SnowDesktopWidgetAuthorPreviewCliTests')):
        plan['tests']=[name];plan['tasks']=[{'participant':'host','requirement':dict(request,tests=[name])}]
        for mode in ('cold','legacy'):
            (root/'occupied-host').write_text('controlled occupancy')
            before=(root/'build.count').read_bytes();arranges=(root/'arrange.count').read_bytes() if (root/'arrange.count').exists() else b''
            coverage=run(mode,False)
            assert 'Host runtime is in use' in coverage['error'] and before==(root/'build.count').read_bytes()
            assert ((root/'arrange.count').read_bytes() if (root/'arrange.count').exists() else b'')==arranges
            (root/'occupied-host').unlink();coverage=run(mode,True)
            assert coverage['status']=='passed' and coverage['selected']==[name]
            assert target in (root/'build-args.txt').read_text().splitlines()[-1]
            assert (root/'arrange.count').read_bytes()==arranges+b'arrange\n'
    plan['tests']=['Alpha'];plan['tasks']=[{'participant':'no-host','requirement':request}]
    (root/'occupied-host').write_text('irrelevant host occupancy');arranges=(root/'arrange.count').read_bytes()
    assert run('cold',True)['status']=='passed' and arranges==(root/'arrange.count').read_bytes()
    (root/'occupied-host').unlink()
    plan.update(mode='full',suites=['full'],tests=[]);plan['tasks']=[{'participant':'full','requirement':dict(request,suites=['full'],tests=[],requiredFull=True)}]
    coverage=run('pass',True);assert coverage['postChecks']['outputIsolation']['status']=='passed' and coverage['tasks'][0]['status']=='passed'
    coverage=run('failed',False);assert coverage['postChecks']['outputIsolation']['status']=='not-run'
    (root/'.build/Release/escaped.dll').write_bytes(b'controlled output violation')
    coverage=run('pass',False);assert coverage['postChecks']['outputIsolation']['status']=='failed' and coverage['tasks'][0]['status']=='failed'
    assert coverage['postChecks']['outputIsolation']['exitCode']==1 and coverage['postChecks']['outputIsolation']['error'] in coverage['error']
    print('PASS real test manager: selection, target build, binary hashes, per-task coverage, failures, skipped-not-pass, explicit environment block and zero-selection not-run receipt')
    print('PASS dynamic host-runtime metadata before binaries exist, legacy host command fallback, host occupancy guards/arrangement, nonhost independence and full postcheck passed/not-run/failed')
    print('PLAN EXECUTION evidence: '+str(root))

if __name__=='__main__':main(Path(__file__).resolve().parents[1])
