"""Real local wait/repair runners; only native build and transient CTest boundaries are fixtures."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
PS = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')


CREATED = []
def new_fixture(prefix):
    root=Path(tempfile.mkdtemp(prefix=prefix));CREATED.append(root);return root

def cleanup_fixtures():
    temp=Path(tempfile.gettempdir()).resolve()
    for root in reversed(CREATED):
        resolved=root.resolve()
        if not str(resolved).lower().startswith(str(temp).lower()+os.sep) or not root.name.startswith(('SnowDesktop-wait-retry-','SnowDesktop-retry-process-')):
            raise AssertionError('Refusing cleanup outside the named temporary fixture')
        records=[]
        for state in (root/'.build/collaboration',root/'repair/.build/collaboration'):
            for file in list(state.glob('*.wait.json'))+[state/'state.json']:
                if not file.exists():continue
                value=json.loads(file.read_bytes())
                if file.name=='state.json':
                    current=value.get('current') or {};records.append(current.get('owner'))
                    records += [x.get('waiter') for x in current.get('participants',[])]
                else:records.append(value.get('owner'))
        for owner in records:
            if not owner or owner.get('pid')==os.getpid():continue
            pid=int(owner['pid']);ticks=str(owner.get('startTicks',''))
            if not ticks.isdigit():raise AssertionError('Invalid fixture process identity')
            # Only a fixture-owned PID with the same creation time AND script directory can be stopped.
            patterns=[str(root).replace("'","''"),str(resolved).replace("'","''")]
            script="$p=Get-Process -Id "+str(pid)+" -ErrorAction SilentlyContinue;if($p -and $p.StartTime.ToUniversalTime().Ticks.ToString() -eq '"+ticks+"'){$c=(Get-CimInstance Win32_Process -Filter 'ProcessId="+str(pid)+"').CommandLine;if($c -and ($c.Contains('"+patterns[0]+"') -or $c.Contains('"+patterns[1]+"'))){Stop-Process -Id "+str(pid)+";Start-Sleep -Milliseconds 100}}"
            result=subprocess.run([PS,'-NoProfile','-Command',script+';exit 0'],capture_output=True,timeout=10)
            if result.returncode:raise AssertionError('Fixture process cleanup failed; user processes were not targeted')
        deadline=time.monotonic()+2
        while True:
            try:
                if os.name=='nt':
                    # Native Force removes readonly Git objects in this already verified fixture tree.
                    command="Remove-Item -LiteralPath '"+str(resolved).replace("'","''")+"' -Recurse -Force -ErrorAction Stop"
                    removed=subprocess.run([PS,'-NoProfile','-Command',command],capture_output=True,timeout=10)
                    if removed.returncode:raise PermissionError(removed.stderr.decode('utf-8',errors='replace'))
                else:shutil.rmtree(str(resolved))
                break
            except PermissionError:
                if time.monotonic()>=deadline:raise
                time.sleep(.05)
    print('PASS named temporary fixtures cleaned; only exact fixture process identities were targeted')

def process_fixture(repo):
    root=new_fixture('SnowDesktop-retry-process-')
    (root/'scripts').mkdir();(root/'tools/build-dashboard').mkdir(parents=True);(root/'bin').mkdir()
    for name in ('build_wait_tasks.py','build_test_retry.py','build_inputs.ps1','build_batch_tests.ps1','build_runtime.ps1'):
        shutil.copyfile(repo/'scripts'/name,root/'scripts'/name)
    shutil.copyfile(repo/'tools/build-dashboard/server.py',root/'tools/build-dashboard/server.py')
    (root/'.gitignore').write_text('.build/\nbin/\n')
    (root/'source.cpp').write_text('source identity')
    (root/'scripts/test_manager.ps1').write_text('''param([string]$Mode,[string]$PlanBatch)
    $root=Split-Path $PSScriptRoot -Parent
    $dir=Join-Path $root '.build/collaboration'
    $work=Join-Path $dir ($PlanBatch+'.retry')
    [IO.File]::AppendAllText((Join-Path $root '.build/initial.count'),"once`n")
    $report=Join-Path $work 'initial.xml'
    [IO.File]::WriteAllText($report,'<testsuite><testcase name="build_dashboard"><failure>structured resource fixture</failure></testcase><testcase name="Deterministic"/></testsuite>')
    @{schemaVersion=1;batchId=$PlanBatch;mode='selected';status='failed';selected=@('build_dashboard','Deterministic');completed=@();tasks=@(@{participant='fixture';requested=@('build_dashboard','Deterministic');status='failed'});report=$report;error='initial fixture failed'}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $dir ($PlanBatch+'.coverage.json')) -Encoding UTF8
    if([IO.File]::ReadAllText((Join-Path $root '.build/control')) -ne 'assertion'){
     @{test='build_dashboard';runToken=$env:SNOWDESKTOP_RETRY_RUN_TOKEN;exitCode=75;failureClass='isolated-port-race';cleanupComplete=$true;sideEffects='none';reason='controlled ephemeral port failure'}|ConvertTo-Json|Set-Content (Join-Path $env:SNOWDESKTOP_RETRY_SIGNAL_DIR 'build_dashboard.json') -Encoding UTF8
    }
    exit 1
    ''',encoding='utf-8')
    csharp=r'''using System;using System.IO;
    public class FakeCTest {
     public static int Main(string[] args) {
      string root=Directory.GetCurrentDirectory();
      if(Array.IndexOf(args,"--show-only=json-v1")>=0){Console.Write("{\"tests\":[{\"name\":\"build_dashboard\",\"properties\":[{\"name\":\"LABELS\",\"value\":[\"retry-isolated-resource\"]}]},{\"name\":\"Deterministic\",\"properties\":[]}]}");return 0;}
      File.AppendAllText(Path.Combine(root,".build/ctest.count"),"once\n");
      string mode=File.ReadAllText(Path.Combine(root,".build/control"));
      string report=args[Array.IndexOf(args,"--output-junit")+1];
      bool failed=mode=="always" || mode=="change";
      File.WriteAllText(report,"<testsuite><testcase name=\"build_dashboard\">"+(failed?"<failure>resource fixture</failure>":"")+"</testcase></testsuite>");
      if(failed){
       string token=Environment.GetEnvironmentVariable("SNOWDESKTOP_RETRY_RUN_TOKEN");
       string signals=Environment.GetEnvironmentVariable("SNOWDESKTOP_RETRY_SIGNAL_DIR");
       File.WriteAllText(Path.Combine(signals,"build_dashboard.json"),"{\"test\":\"build_dashboard\",\"runToken\":\""+token+"\",\"exitCode\":75,\"failureClass\":\"isolated-port-race\",\"cleanupComplete\":true,\"sideEffects\":\"none\",\"reason\":\"controlled resource failure\"}");
      }
      if(mode=="change")File.WriteAllText(Path.Combine(root,"source.cpp"),"changed during retry");
      return failed?8:0;
     }
    }'''
    source=root/'bin/fixture.cs';source.write_text(csharp)
    csc=Path(os.environ['WINDIR'])/'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    subprocess.run([str(csc),'/nologo','/out:'+str(root/'bin/ctest.exe'),str(source)],check=True,capture_output=True)
    for arguments in (['init','--quiet'],['add','.'],['-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','--quiet','-m','fixture']):
        subprocess.run(['git',*arguments],cwd=str(root),check=True,capture_output=True)
    state=root/'.build/collaboration';state.mkdir(parents=True)
    ps=str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')
    env=dict(os.environ,PATH=str(root/'bin')+os.pathsep+os.environ['PATH'])
    for index,(mode,expected,attempts) in enumerate((('once',0,1),('always',1,2),('assertion',1,0),('change',5,1))):
        batch=str(index+1)*32
        (state/(batch+'.plan.json')).write_text(json.dumps({'schemaVersion':1,'batchId':batch,'configuration':'Release','mode':'selected','buildRequired':False}))
        (root/'.build/control').write_text(mode)
        (root/'source.cpp').write_text('initial '+mode)
        old=(root/'.build/ctest.count').read_text().count('once') if (root/'.build/ctest.count').exists() else 0
        run=subprocess.run([ps,'-NoProfile','-File',str(root/'scripts/build_batch_tests.ps1'),'-Batch',batch],cwd=str(root),env=env,capture_output=True,text=True,timeout=45)
        assert run.returncode==expected,(mode,run.returncode,run.stdout,run.stderr)
        ledger=json.loads((state/(batch+'.retry.json')).read_text())
        assert len(ledger['selected'])==2 and ledger['initialExitCode']==1
        new=(root/'.build/ctest.count').read_text().count('once') if (root/'.build/ctest.count').exists() else 0
        assert new-old==attempts,(mode,new-old)
        coverage=json.loads((state/(batch+'.coverage.json')).read_text())
        if mode=='once':assert coverage['status']=='passed-after-retry' and coverage['flaky']==['build_dashboard'] and coverage['tasks'][0]['status']=='passed-after-retry'
        if mode=='always':assert ledger['tests']['build_dashboard']['failureCount']==3 and ledger['tests']['build_dashboard']['status']=='exhausted-failed'
        if mode=='change':assert ledger['status']=='invalidated'
        prior=(root/'.build/initial.count').read_text()
        replay=subprocess.run([sys.executable,str(root/'scripts/build_test_retry.py'),'--root',str(root),'--batch',batch],cwd=str(root),env=env,capture_output=True,text=True,timeout=10)
        assert replay.returncode==expected and prior==(root/'.build/initial.count').read_text()
        print('PASS real retry entry '+mode+': initial once, retries '+str(attempts)+', exit '+str(expected),flush=True)
    print('RETRY PROCESS PASSED; evidence '+str(root),flush=True)

def real_selection_fixture(repo):
    import datetime as dt
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-real-ctest-selection-'))
    (root/'scripts').mkdir();state=root/'.build/collaboration';state.mkdir(parents=True)
    for name in ('build_job.cs','build_entry.ps1','test_manager.ps1','build_protocol.ps1'):
        shutil.copyfile(repo/'scripts'/name,root/'scripts'/name)
    (root/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
    project(RealSelection NONE)
    enable_testing()
    foreach(name Alpha Beta Gamma)
     add_test(NAME ${name} COMMAND powershell.exe -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/probe.ps1" -Name ${name})
     set_tests_properties(${name} PROPERTIES LABELS "core" TIMEOUT 10)
    endforeach()
    ''')
    (root/'probe.ps1').write_text('''param([string]$Name)
    [IO.File]::AppendAllText((Join-Path $PSScriptRoot '.build/executed.txt'),$Name+"`n")
    if($Name -eq 'Gamma'){exit 23}
    if($Name -eq 'Beta' -and (Test-Path (Join-Path $PSScriptRoot '.build/fail-beta'))){exit 31}
    exit 0
    ''')
    (root/'CMakePresets.json').write_text(json.dumps({'version':3,'configurePresets':[{'name':'tests','binaryDir':'${sourceDir}/.build/ctest'}],
      'testPresets':[{'name':'all-tests','configurePreset':'tests','configuration':'Release','output':{'outputOnFailure':True}}]}))
    ps=str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')
    log=[]
    def invoke(*args,expected):
        p=subprocess.run([ps,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(root/'scripts/test_manager.ps1'),*args],cwd=str(root),capture_output=True,text=True,timeout=30)
        log.append({'args':list(args),'exitCode':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
        assert (p.returncode==0)==(expected==0),(args,p.returncode,p.stdout,p.stderr)
        return p
    def plan(batch):
        requirement={'tests':['Alpha','Beta'],'suites':['selected'],'requiredFull':False,'reason':'real CTest literal selection fixture'}
        (state/(batch+'.plan.json')).write_text(json.dumps({'schemaVersion':1,'batchId':batch,'configuration':'Release','mode':'selected','suites':['selected'],'tests':['Alpha','Beta'],'tasks':[{'participant':'real-ctest','requirement':requirement}]}))
    try:
        batch='1'*32;plan(batch);invoke('-Mode','plan','-PlanBatch',batch,expected=0)
        coverage=json.loads((state/(batch+'.coverage.json')).read_text(encoding='utf-8-sig'))
        assert coverage['selected']==['Alpha','Beta'] and len(coverage['completed'])==2 and coverage['status']=='passed',coverage
        assert (root/'.build/executed.txt').read_text().splitlines()==['Alpha','Beta']
        print('PASS real CMake/CTest engine: requested 2, selected 2, actually executed 2, JUnit completed 2; Gamma never ran',flush=True)
        before=(root/'.build/executed.txt').read_bytes()
        invoke('-Mode','name','-Filter','^Missing$',expected=1)
        assert before==(root/'.build/executed.txt').read_bytes()
        invoke('-Mode','name','-Filter','^(?:Alpha|Beta)$',expected=1)
        assert before==(root/'.build/executed.txt').read_bytes()
        print('PASS real zero-match and unsupported noncapturing regex: nonzero manager exit, zero extra executions, never pass',flush=True)
        (root/'.build/fail-beta').write_text('31');batch2='2'*32;plan(batch2);invoke('-Mode','plan','-PlanBatch',batch2,expected=1)
        failed=json.loads((state/(batch2+'.coverage.json')).read_text(encoding='utf-8-sig'))
        assert len(failed['completed'])==2 and failed['status']=='failed' and failed['tasks'][0]['failed']==['Beta']
        assert (root/'.build/executed.txt').read_text().splitlines()==['Alpha','Beta','Alpha','Beta']
        print('PASS real deterministic Beta failure: 2 executed, 1 failed, per-task failure attribution, nonzero exit',flush=True)
        report={'schemaVersion':1,'runId':'real-ctest-selection-20261002','task':'waiting-efficiency-20261002','status':'passed',
                'scope':'real CMake/CTest engine with isolated tiny tests; no host compilation',
                'testManagerSHA256':__import__('hashlib').sha256((repo/'scripts/test_manager.ps1').read_bytes()).hexdigest(),
                'actualRequested':2,'actualSelected':2,'actualExecuted':2,'zeroMatchRejected':True,'invalidRegexRejected':True,
                'failedRunExecuted':2,'failedRunFailed':['Beta'],'commands':log,'completedUtc':dt.datetime.now(dt.timezone.utc).isoformat()}
        directory=repo/'.build/verification';directory.mkdir(parents=True,exist_ok=True)
        path=directory/('real-ctest-selection-20261002-'+__import__('secrets').token_hex(4)+'.json');path.write_bytes(json.dumps(report,ensure_ascii=False,indent=2).encode('utf-8'))
        print('REAL CTEST EVIDENCE '+str(path),flush=True)
    finally:
        resolved=root.resolve();temp=Path(tempfile.gettempdir()).resolve()
        assert str(resolved).lower().startswith(str(temp).lower()+os.sep) and resolved.name.startswith('SnowDesktop-real-ctest-selection-')
        subprocess.run([ps,'-NoProfile','-Command',"Remove-Item -LiteralPath '"+str(resolved).replace("'","''")+"' -Recurse -Force -ErrorAction Stop"],check=True,capture_output=True,timeout=10)

def main(repo):
    root=new_fixture('SnowDesktop-wait-retry-')
    (root/'scripts').mkdir();(root/'tools/build-dashboard').mkdir(parents=True)
    for name in ('build_wait_tasks.py','build_test_retry.py','build_entry.ps1','build_runtime.ps1','build_manager.ps1','build_protocol.ps1',
                 'build_inputs.ps1','build_ownership.ps1','build_preflight.ps1','build_job.cs','build_waiter.ps1'):
        shutil.copyfile(repo/'scripts'/name,root/'scripts'/name)
    shutil.copyfile(repo/'tools/build-dashboard/server.py',root/'tools/build-dashboard/server.py')
    sys.path.insert(0,str(root/'scripts'))
    import build_wait_tasks as waits
    import build_test_retry as retry
    reader=waits.reader(root)
    state_root=root/'.build/collaboration';state_root.mkdir(parents=True)
    owned=[]
    def read(name):return reader.read_json(state_root,name)
    def until(fn,seconds=12):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            value=fn()
            if value:return value
            time.sleep(.05)
        raise AssertionError('Bounded fixture condition timed out: '+str(root))
    def cli(*args,code=0):
        run=subprocess.run([sys.executable,str(root/'scripts/build_wait_tasks.py'),'watch',*map(str,args),'--root',str(root)],capture_output=True,text=True,encoding='utf-8',timeout=15)
        assert run.returncode==code,(args,run.returncode,run.stdout,run.stderr)
        return json.loads(run.stdout) if code==0 else run.stderr
    def ticket(**options):
        arguments=['start','wait-editor','--condition',options.pop('condition','files'),'--timeout',options.pop('timeout',10)]
        for key,value in options.items():arguments+=['--'+key,str(value)]
        out=cli(*arguments);owned.append(out.get('owner'));return out
    def done(t):return until(lambda:(lambda x:x if x and x['status']!='waiting' else None)(read(t['id']+'.wait.json')))
    # Finished claims preserve provenance and permit handoff; age cannot close an editing registration.
    bid='a'*32
    state={'schemaVersion':1,'repositoryRoot':str(root),'current':{'id':bid,'phase':'editing','owner':None,
           'participants':[{'id':'peer','state':'editing','registeredUtc':'2000-01-01T00:00:00Z','ownedFiles':['src/a.txt']}]}}
    waits.atomic(state_root/'state.json',state)
    initial=(state_root/'state.json').read_bytes()
    a=ticket(files='src/a.txt')
    b=cli('start','wait-editor','--condition','files','--files','src/a.txt','--timeout',10)
    assert a['id']==b['id'] and a['owner']==b['owner'],'Duplicate calls must reuse a worker'
    time.sleep(2.2)
    assert read(a['id']+'.wait.json')['status']=='waiting'
    assert initial==(state_root/'state.json').read_bytes(),'Wait must not complete a stale editor or run checks'
    state['current']['participants'][0]['state']='finished';waits.atomic(state_root/'state.json',state)
    assert done(a)['status']=='eligible','Finished provenance must not block a new task'
    state['current']=None;waits.atomic(state_root/'state.json',state)
    outcome=done(a);assert outcome['status']=='eligible' and outcome['observations']<=6
    receipt=(state_root/(a['id']+'.wait-attempt1.json')).read_bytes()
    assert cli('status','--ticket',a['id'])['status']=='eligible'
    assert cli('resume','--ticket',a['id'],'--reason','observe saved result')['status']=='eligible'
    assert receipt==(state_root/(a['id']+'.wait-attempt1.json')).read_bytes()
    print('PASS one durable worker, stale editor retained, finished provenance released for handoff, condition progression, immutable receipt')

    state['current']={'id':bid,'phase':'building','owner':waits.process_owner(os.getpid()),'participants':[{'id':'peer','state':'finished'}]}
    waits.atomic(state_root/'state.json',state)
    frozen=ticket(condition='window',new='') if False else cli('start','window-editor','--condition','window','--timeout',10)
    owned.append(frozen['owner']);time.sleep(.3)
    assert read(frozen['id']+'.wait.json')['status']=='waiting'
    waits.atomic(state_root/(bid+'.json'),{'batchId':bid,'outcome':'failed','exitCode':1})
    assert done(frozen)['status']=='attention','Saved result before retirement cannot grant a window'
    # Crash waits are only resumed explicitly. Kill only a fixture-owned worker by exact process identity.
    (state_root/(bid+'.json')).unlink()
    crashed=cli('start','crash-editor','--condition','window','--timeout',10);owned.append(crashed['owner'])
    subprocess.run([PS,'-NoProfile','-Command','Stop-Process -Id '+str(crashed['owner']['pid'])],check=True,capture_output=True)
    until(lambda:reader.owner_state(crashed['owner'])=='exited')
    assert cli('status','--ticket',crashed['id'])['status']=='interrupted'
    restarted=cli('resume','--ticket',crashed['id'],'--timeout',10,'--reason','fixture worker exited; safe readonly restart');owned.append(restarted['owner'])
    assert restarted['attempt']==2
    state['current']=None;waits.atomic(state_root/'state.json',state)
    assert done(restarted)['status']=='eligible'
    # Hard time budget and diagnostic check failure; no busy/infinite wait.
    state['current']={'id':bid,'phase':'editing','owner':None,'participants':[{'id':'wait-editor','state':'finished','editRevision':3},
          {'id':'peer','state':'editing','ownedFiles':['src/a.txt']}]};waits.atomic(state_root/'state.json',state)
    bounded=cli('start','timeout-editor','--condition','files','--files','src/a.txt','--timeout',1);owned.append(bounded['owner'])
    assert done(bounded)['status']=='timed-out'
    peerwait=ticket(condition='peers',batch=bid,revision=3)
    state['current']['participants'][1].update(state='finished',check={'status':'failed'});waits.atomic(state_root/'state.json',state)
    assert done(peerwait)['status']=='attention'
    print('PASS frozen/result race, explicit crashed-worker recovery, bounded timeout, peer failure wakes only for attention')
    (state_root/'state.json').unlink()
    observed={'batchId':bid,'outcome':'passed','participants':[{'id':'wait-editor','editRevision':2}]}
    request={'condition':'result','batchId':bid,'participant':'wait-editor','editRevision':3}
    assert waits.condition(request,{'current':None},observed)[0]=='attention'
    observed['participants'][0]['editRevision']=3
    assert waits.condition(request,{'current':None},observed)[0]=='completed'
    blocked={'id':bid,'phase':'editing','participants':[{'id':'wait-editor','editRevision':3,'state':'finished','check':{'status':'failed'}}]}
    assert waits.condition(request,{'current':blocked},None)[0]=='attention'
    blocked['participants'][0]['check']['status']='passed';blocked.update(planStatus='blocked',planError='stale plan')
    assert waits.condition(request,{'current':blocked},None)[0]=='attention'
    blocked['participants'].append({'id':'peer-editing','state':'editing','check':{'status':'invalidated'}})
    assert waits.condition(request,{'current':blocked},None)[0] is None
    blocked['phase']='building'
    assert waits.condition(request,{'current':blocked},None)[0] is None
    print('PASS saved wait result remains bound to participant and edit revision')

    # Real retry policy, scheduler and immutable evidence; substitute only CTest process outcomes.
    def scenario(label,outcomes,signal=True,change=False,metadata_label=True):
        batch=('b'+str(len(list(state_root.glob('*.retry.json'))))) .encode().hex().ljust(32,'0')[:32]
        work=state_root/(batch+'.retry');work.mkdir();(work/'signals-1').mkdir()
        token='c'*32
        name='build_dashboard'
        def write_signal(directory):
            waits.atomic(directory/(name+'.json'),{'test':name,'runToken':token,'exitCode':75,'failureClass':retry.FAILURE_CLASS,'cleanupComplete':True,'sideEffects':'none','reason':'controlled isolated port acquisition'})
        if signal:write_signal(work/'signals-1')
        ledger={'schemaVersion':1,'batchId':batch,'runToken':token,'owner':waits.process_owner(os.getpid()),'status':'running','tests':{
            name:{'name':name,'status':'failed','firstReason':'first XML failure','finalReason':'first XML failure','failureCount':1,'attempts':[{'attempt':1,'status':'failed'}]},
            'Deterministic':{'name':'Deterministic','status':'passed','failureCount':0,'attempts':[{'attempt':1,'status':'passed'}]}}}
        coverage={'selected':[name,'Deterministic'],'tasks':[{'requested':[name,'Deterministic'],'status':'failed'}]}
        metadata={name:{'properties':[{'name':'LABELS','value':[retry.LABEL] if metadata_label else []}]}}
        calls=[];identities=[0]
        def identity():
            identities[0]+=1
            return 'changed' if change and identities[0]>1 else 'same'
        def execute(test,report,log,signals,timeout):
            status=outcomes[len(calls)];calls.append(test);log.write_text('controlled attempt '+str(len(calls)))
            report.write_text('<testsuite><testcase name="'+test+'">'+('<failure>controlled failure</failure>' if status=='failed' else '')+'</testcase></testsuite>')
            if status=='failed' and signal:write_signal(signals)
            return 8 if status=='failed' else 0
        code=retry.retry_failed(ledger,coverage,metadata,'same',identity,lambda:True,execute,work,budget=100,backoffs=(0,0))
        retry.export_coverage(state_root/(batch+'.coverage.json'),coverage,ledger)
        assert len(coverage['selected'])==2 and len(coverage['completed'])==2,'Retry must not increase denominator'
        assert len(ledger['tests']['Deterministic']['attempts'])==1,'Passed tests must not be repeated'
        return code,ledger,calls,work
    code,ledger,calls,work=scenario('one transient',['passed']);assert code==0 and calls==['build_dashboard']
    assert ledger['tests']['build_dashboard']['status']=='passed-after-retry' and ledger['tests']['build_dashboard']['failureCount']==1
    assert (work/'build_dashboard.attempt2.json').exists()
    code,ledger,calls,_=scenario('continuous',['failed','failed']);assert code==1 and len(calls)==2
    assert ledger['tests']['build_dashboard']['status']=='exhausted-failed' and ledger['tests']['build_dashboard']['failureCount']==3
    code,ledger,calls,_=scenario('assertion',[],signal=False);assert code==1 and not calls
    code,ledger,calls,_=scenario('unreviewed',[],metadata_label=False);assert code==1 and not calls
    code,ledger,calls,_=scenario('source changes',[],change=True);assert code==5 and not calls
    print('PASS transient success marked flaky, finite exhaustion, deterministic/unreviewed no retry, changed input abort, stable denominator')

    # Child crashes after a side effect has a durable completed attempt but stale summary.
    crashbatch='f'*32;work=state_root/(crashbatch+'.retry');work.mkdir()
    code="""import os,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1]);from build_wait_tasks import atomic,process_owner
w=Path(sys.argv[2]);batch=sys.argv[3]
atomic(w.parent/(batch+'.retry.json'),{'batchId':batch,'status':'retrying','owner':process_owner(os.getpid()),'tests':{'build_dashboard':{'status':'retrying'}}})
(w/'side-effect.count').write_text('once')
atomic(w/'build_dashboard.attempt2.json',{'attempt':2,'status':'passed','exitCode':0})
os._exit(19)
"""
    crashed=subprocess.run([sys.executable,'-c',code,str(root/'scripts'),str(work),crashbatch]);assert crashed.returncode==19
    status=subprocess.run([sys.executable,str(root/'scripts/build_test_retry.py'),'--root',str(root),'--batch',crashbatch,'--status'],capture_output=True,text=True)
    assert json.loads(status.stdout)['status']=='interrupted'
    replay=subprocess.run([sys.executable,str(root/'scripts/build_test_retry.py'),'--root',str(root),'--batch',crashbatch],capture_output=True,text=True)
    assert replay.returncode==4 and (work/'side-effect.count').read_text()=='once'
    assert json.loads((work/'build_dashboard.attempt2.json').read_text())['status']=='passed'
    print('PASS crash evidence retained, completed side effect never replayed, explicit new repair required')

    process_fixture(repo)
    real_selection_fixture(repo)

    # Real coordinator continuation on a separate Git fixture; build/test subprocess boundary only is fake.
    repairroot=root/'repair';(repairroot/'scripts').mkdir(parents=True);(repairroot/'src').mkdir()
    for name in ('build_entry.ps1','build_runtime.ps1','build_manager.ps1','build_protocol.ps1','build_inputs.ps1','build_ownership.ps1','build_preflight.ps1','build_job.cs','build_waiter.ps1'):
        shutil.copyfile(repo/'scripts'/name,repairroot/'scripts'/name)
    (repairroot/'src/a.txt').write_text('original')
    (repairroot/'.gitignore').write_text('.build/\n*.count\nfail-build\n')
    (repairroot/'scripts/fake.ps1').write_text('''param([string]$Phase)
$root=Split-Path $PSScriptRoot -Parent
[IO.File]::AppendAllText((Join-Path $root ($Phase+'.count')),"run`n")
if($Phase -eq 'build' -and (Test-Path (Join-Path $root 'fail-build'))){exit 11};exit 0
''')
    (repairroot/'scripts/build.bat').write_text('@echo off\npowershell.exe -NoProfile -File "%~dp0fake.ps1" -Phase build\nexit /b %ERRORLEVEL%\n')
    (repairroot/'scripts/build_batch_tests.ps1').write_text('param([string]$Batch)\n& (Join-Path $PSScriptRoot fake.ps1) -Phase test;exit $LASTEXITCODE\n')
    for arguments in (['init','--quiet'],['add','.'],['-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','--quiet','-m','fixture']):
        subprocess.run(['git',*arguments],cwd=str(repairroot),check=True,capture_output=True)
    def manager(*args,expected=0,asynchronous=False):
        argv=[PS,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(repairroot/'scripts/build_manager.ps1'),*map(str,args)]
        if asynchronous:return subprocess.Popen(argv,cwd=str(repairroot),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        result=subprocess.run(argv,cwd=str(repairroot),capture_output=True,text=True,timeout=25)
        assert result.returncode==expected,(args,result.returncode,result.stderr,result.stdout)
        return json.loads(result.stdout) if result.stdout.strip().startswith('{') else result.stdout
    sr=repairroot/'.build/collaboration'
    manager('begin','a');b=manager('begin','b');parent=b['batchId']
    (repairroot/'fail-build').write_text('controlled compilation failure')
    first=manager('finish','a','-Batch',parent,'-Revision',0,'-WaitSeconds',15,asynchronous=True)
    failed=manager('finish','b','-Batch',parent,'-Revision',0,expected=11)
    first.communicate(timeout=20);assert first.returncode==11
    assert failed['outcome']=='failed' and not (repairroot/'test.count').exists()
    saved=(sr/(parent+'.json')).read_bytes()
    (repairroot/'fail-build').unlink()
    one=manager('repair','a','-Batch',parent,'-Revision',0,'-Reason','repair fixture compiler failure',asynchronous=True)
    two=manager('repair','b','-Batch',parent,'-Revision',0,'-Reason','join same repair',asynchronous=True)
    outputs=[]
    for process in (one,two):
        stdout,stderr=process.communicate(timeout=25);assert process.returncode==0,stderr;outputs.append(json.loads(stdout))
    assert outputs[0]['batchId']==outputs[1]['batchId'] and outputs[0]['attempt']==2
    child=outputs[0]['batchId'];assert child!=parent
    manager('repair-abandon','-Batch',parent,'-Reason','must not clear active repair',expected=2)
    assert json.loads((sr/'state.json').read_text(encoding='utf-8-sig'))['current']['participants'][0]['testPlan']['originalRequirement']
    manager('ready','a','-Batch',child,'-Revision',0,expected=2)
    # Publication crash boundary: child state durable but repair mapping missing.
    (sr/(parent+'.repair.json')).unlink()
    duplicate=manager('repair','a','-Batch',parent,'-Revision',0,'-Reason','reconcile saved child after mapping interruption')
    assert duplicate['batchId']==child
    assert duplicate['editRevision']==outputs[0]['editRevision']
    manager('claim','a','-Batch',child,'-Revision',outputs[0]['editRevision'],'-Files','src/a.txt')
    (repairroot/'src/a.txt').write_text('fixed input')
    manager('ready','a','-Batch',child,'-Revision',outputs[0]['editRevision'])
    manager('ready','b','-Batch',child,'-Revision',outputs[1]['editRevision'])
    until(lambda:reader.read_json(sr,child+'.json'),seconds=30)
    repaired=reader.read_json(sr,child+'.json')
    assert repaired['outcome']=='passed' and repaired['repairOf']==parent and repaired['logicalBatchId']==parent and repaired['attempt']==2,repaired
    assert saved==(sr/(parent+'.json')).read_bytes(),'Failure evidence must be immutable'
    assert (repairroot/'build.count').read_text().count('run')==2 and (repairroot/'test.count').read_text().count('run')==1
    manager('repair','a','-Batch',parent,'-Revision',0,'-Reason','cannot replay old parent',expected=2)
    independent=manager('begin','independent');manager('recover','independent','-Batch',independent['batchId'],'-ConfirmStopped','-Reason','fixture editor stopped')
    # Explicit abandonment refuses active children and never erases the original result.
    manager('repair-abandon','-Batch',parent,'-Reason','no further old-parent repair')
    assert saved==(sr/(parent+'.json')).read_bytes()
    manager('repair','a','-Batch',parent,'-Revision',0,'-Reason','abandoned old parent',expected=2)
    # Only the repairer re-registers: prior ready member/request is carried and rechecked automatically.
    manager('begin','a');carried=manager('begin','b');parent2=carried['batchId'];(repairroot/'fail-build').write_text('11')
    manager('plan','b','-Batch',parent2,'-Revision',0,'-Scope','module','-Suites','selected','-Tests','ManualPreview','-Inputs','src/a.txt','-Reason','explicit manual acceptance requirement')
    first=manager('finish','b','-Batch',parent2,'-Revision',0,'-WaitSeconds',15,asynchronous=True)
    manager('finish','a','-Batch',parent2,'-Revision',0,expected=11);first.communicate(timeout=20)
    parent_bytes=(sr/(parent2+'.json')).read_bytes();(repairroot/'fail-build').unlink()
    solo=manager('repair','a','-Batch',parent2,'-Revision',0,'-Reason','one repairer; original peer request stays')
    manager('claim','a','-Batch',solo['batchId'],'-Revision',solo['editRevision'],'-Files','src/a.txt',expected=2)
    manager('claim','a','-Batch',solo['batchId'],'-Revision',solo['editRevision'],'-Files','src/a.txt','-AdoptExistingChanges')
    (repairroot/'src/a.txt').write_text('second fixed input');manager('ready','a','-Batch',solo['batchId'],'-Revision',solo['editRevision'])
    until(lambda:reader.read_json(sr,solo['batchId']+'.json'),seconds=30)
    final=reader.read_json(sr,solo['batchId']+'.json');assert final['outcome']=='passed' and final['testingPlan']['mode']=='full',final
    assert len(final['participants'])==2 and all(x['check']['status']=='passed' for x in final['participants'])
    assert 'ManualPreview' in final['testingPlan']['tests'],'Full automatic escalation must preserve explicit manual requirements'
    assert parent_bytes==(sr/(parent2+'.json')).read_bytes()
    assert (repairroot/'build.count').read_text().count('run')==4 and (repairroot/'test.count').read_text().count('run')==2
    print('PASS failed compile retains evidence/requests, concurrent single repair attempt, stale revisions rejected, one repaired pipeline, independent task, explicit abandon')
    print('WAIT/RETRY/REPAIR PASSED; evidence: '+str(root))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);args=parser.parse_args()
    try:
        main(args.repo.resolve())
    finally:
        cleanup_fixtures()
