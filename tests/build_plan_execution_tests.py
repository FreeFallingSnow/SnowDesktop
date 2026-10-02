"""Exercise the real test manager against small compiled CMake/CTest stand-ins."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main(repo):
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-plan-execution-'));scripts=root/'scripts';scripts.mkdir();binroot=root/'fake-bin';binroot.mkdir()
    for file in ('build_job.cs','build_entry.ps1','test_manager.ps1','build_protocol.ps1'):shutil.copyfile(repo/'scripts'/file,scripts/file)
    source=root/'standin.cs'
    source.write_text(r'''using System;using System.IO;using System.Linq;
class StandIn {
static int Main(string[] args){
 string root=Directory.GetCurrentDirectory(), exe=Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
 string dir=Path.Combine(root,".build","Release","tests");Directory.CreateDirectory(dir);
 if(exe=="cmake") { if(args.Contains("--build"))File.AppendAllText(Path.Combine(root,"build.count"),"build\n");foreach(string n in new[]{"Alpha","Beta"})File.WriteAllText(Path.Combine(dir,"SnowDesktop"+n+"Tests.exe"),"fixture binary "+n);Console.WriteLine("mock configure/build");return 0; }
 if(args.Contains("--show-only=json-v1")){
 string json="{\"tests\":[";bool first=true;
 foreach(string n in new[]{"Alpha","Beta"}){
   int r=Array.IndexOf(args,"-R");if(r>=0&&!System.Text.RegularExpressions.Regex.IsMatch(n,args[r+1]))continue;
   if(!first)json+=",";first=false;string path=Path.Combine(dir,"SnowDesktop"+n+"Tests.exe").Replace("\\","\\\\");
   bool blocked=n=="Beta"&&File.Exists("behavior")&&File.ReadAllText("behavior")=="environment";
   json+="{\"name\":\""+n+"\",\"command\":[\""+path+"\"],\"properties\":[{\"name\":\"LABELS\",\"value\":[\"core\""+(blocked?",\"environment-blocked\"":"")+"]}]}";
 }Console.WriteLine(json+"]}");return 0; }
 int output=Array.IndexOf(args,"--output-junit");if(output<0)return 8;
 string pattern=args[Array.IndexOf(args,"-R")+1];string xml="<testsuites><testsuite>";
 string mode=File.Exists("behavior")?File.ReadAllText("behavior"):"pass";
 foreach(string n in new[]{"Alpha","Beta"})if(System.Text.RegularExpressions.Regex.IsMatch(n,pattern))xml+="<testcase name='"+n+"' time='0.1'>"+(mode=="skipped"?"<skipped/>":mode=="failed"&&n=="Beta"?"<failure message='fixture failure'/>":"")+"</testcase>";
 File.WriteAllText(args[output+1],xml+"</testsuite></testsuites>");Console.WriteLine("mock test run "+mode);return mode=="failed"?17:0;
}}
''')
    csc=Path(os.environ['WINDIR'])/'Microsoft.NET/Framework64/v4.0.30319/csc.exe';assert csc.exists()
    for name in ('cmake','ctest'):
        p=subprocess.run([str(csc),'/nologo','/target:exe','/out:'+str(binroot/(name+'.exe')),str(source)],capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
    state=root/'.build/collaboration';state.mkdir(parents=True);bid='c'*32
    request={'suites':['selected'],'tests':['Alpha'],'requiredFull':False,'reason':'independent fixture'}
    plan={'schemaVersion':1,'batchId':bid,'configuration':'Release','mode':'selected','suites':['selected'],'tests':['Alpha','Beta'],'tasks':[{'participant':'a','requirement':request},{'participant':'b','requirement':dict(request,tests=['Beta'])}]}
    def run(mode,expected):
        (root/'behavior').write_text(mode);(state/(bid+'.plan.json')).write_text(json.dumps(plan))
        env=dict(os.environ,PATH=str(binroot)+os.pathsep+os.environ['PATH'])
        p=subprocess.run(['powershell.exe','-NoProfile','-File',str(scripts/'test_manager.ps1'),'-Mode','plan','-PlanBatch',bid],cwd=str(root),env=env,capture_output=True,text=True,timeout=20)
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
    print('PASS real test manager: selection, target build, binary hashes, per-task coverage, failures, skipped-not-pass, explicit environment block and zero-selection not-run receipt')
    print('PLAN EXECUTION evidence: '+str(root))

if __name__=='__main__':main(Path(__file__).resolve().parents[1])
