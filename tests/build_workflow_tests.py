"""Isolated Windows coordinator regression; native build boundaries are fake."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import sys

sys.dont_write_bytecode=True
PS = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')

def run_tests(repo):
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-workflow-'))
    scripts=root/'scripts';scripts.mkdir()
    for name in ('build_manager.ps1','build_inputs.ps1','build_job.cs','build_protocol.ps1','build_ownership.ps1','build_preflight.ps1','build_waiter.ps1'):
        shutil.copyfile(repo/'scripts'/name,scripts/name)
    (root/'src').mkdir()
    for name in ('a.txt','b.txt','other.txt'):(root/'src'/name).write_text('original')
    (root/'.gitignore').write_text('.build/\n*.out\n*.err\n*.request\n*.log\n*.count\nfail-test\nhold-build\nmessage.txt\nunit.ps1\n')
    (scripts/'fake.ps1').write_text('''param([string]$Phase)
$root=Split-Path $PSScriptRoot -Parent
[IO.File]::AppendAllText((Join-Path $root ($Phase+'.count')),"run`n")
while(Test-Path (Join-Path $root 'hold-build')){Start-Sleep -Milliseconds 50}
if($Phase -eq 'test' -and (Test-Path (Join-Path $root 'fail-test'))){Write-Output 'controlled failure';exit 17}
Write-Output "controlled $Phase passed";exit 0
''')
    (scripts/'build.bat').write_text('@echo off\npowershell.exe -NoProfile -File "%~dp0fake.ps1" -Phase build\nexit /b %ERRORLEVEL%\n')
    (scripts/'build_batch_tests.ps1').write_text('param([string]$Batch)\n& (Join-Path $PSScriptRoot fake.ps1) -Phase test; exit $LASTEXITCODE\n')
    def git(*args):
        p=subprocess.run(['git','-c','user.name=Fixture','-c','user.email=fixture@example.invalid',*args],cwd=str(root),capture_output=True,text=True)
        assert p.returncode==0,p.stderr
        return p.stdout.strip()
    git('init','--quiet');git('add','.');git('commit','--quiet','-m','fixture')
    state_root=root/'.build/collaboration'
    owned=[]
    def call(*args,code=0):
        p=subprocess.run([PS,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(scripts/'build_manager.ps1'),*map(str,args)],cwd=str(root),capture_output=True,text=True,timeout=20)
        assert p.returncode==code,(args,p.returncode,p.stderr,p.stdout)
        return json.loads(p.stdout) if p.stdout.strip().startswith('{') else p.stdout
    def state():
        # Reader shares delete with the atomic writer, just like the dashboard.
        import importlib.util
        if not hasattr(state,'reader'):
            spec=importlib.util.spec_from_file_location('dashboard_reader',repo/'tools/build-dashboard/server.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);state.reader=mod.read_json
        return state.reader(state_root,'state.json')
    def until(predicate):
        end=time.monotonic()+25
        while time.monotonic()<end:
            if predicate():return
            time.sleep(.05)
        raise AssertionError('Timed out; fixture='+str(root))
    def begin(task):return call('begin',task)
    def plan(task,batch,rev,suite='selected',tests='Alpha',scope='module',inputs='src/a.txt'):
        return call('plan',task,'-Batch',batch,'-Revision',rev,'-Scope',scope,'-Suites',suite,'-Tests',tests,'-Inputs',inputs,'-Reason','reviewed independent fixture input')
    def ready(task,batch,rev):
        out=call('ready',task,'-Batch',batch,'-Revision',rev)
        if out.get('waiter'):owned.append(out['waiter'])
        return out
    def read_result(batch):return state.reader(state_root,batch+'.json')
    try:
        a=begin('a');b=begin('b');bid=a['batchId'];assert bid==b['batchId']
        plan('a',bid,0);plan('b',bid,0,'none','','docs','src/b.txt')
        start=time.monotonic();ready('a',bid,0);assert time.monotonic()-start<5,'ready must return without waiting for peer'
        until(lambda: state()['current']['participants'][0].get('check',{}).get('status')=='passed')
        assert not (root/'build.count').exists(),'ready A must wait for editing B'
        reopened=begin('a');assert reopened['editRevision']==1
        call('finish','a','-Batch',bid,'-Revision',0,code=2)
        call('finish','a','-Batch',bid,code=2)
        ready('b',bid,0);time.sleep(.8);assert not (root/'build.count').exists()
        call('ready','a','-Batch',bid,'-Revision',1,code=2) # stale plan cannot be re-used
        plan('a',bid,1,tests='Beta');ready('a',bid,1)
        until(lambda: read_result(bid) is not None)
        result=read_result(bid);assert result['outcome']=='passed',result
        assert (root/'build.count').read_text().count('run')==1
        assert (root/'test.count').read_text().count('run')==1
        assert result['testingPlan']['tests']==['Beta']
        assert call('wait','a','-Batch',bid,'-Revision',1)==call('wait','b','-Batch',bid,'-Revision',0)
        print('PASS nonblocking ready, read-only check, reopen, stale finish, frozen selective plan, one pipeline, common result')

        # Content digests do not invalidate on a commit of unchanged content.
        (root/'src/a.txt').write_text('changed input')
        unit=root/'unit.ps1'
        unit.write_text(". $PSScriptRoot/scripts/build_inputs.ps1; Get-BuildInputIdentity $PSScriptRoot | ConvertTo-Json")
        def digest():return json.loads(subprocess.check_output([PS,'-NoProfile','-File',str(unit)],text=True))['digest']
        before=digest();git('add','src/a.txt');git('commit','--quiet','-m','content already hashed');assert before==digest()
        print('PASS HEAD-only transaction retains content identity')

        # A different task's staged bytes survive an owned-path commit.
        c=begin('commit-a');bid2=c['batchId'];call('claim','commit-a','-Batch',bid2,'-Revision',0,'-Files','src/a.txt')
        d=begin('commit-b');call('claim','commit-b','-Batch',bid2,'-Revision',0,'-Files','src/a.txt',code=2)
        (root/'src/a.txt').write_text('owned commit');(root/'src/other.txt').write_text('other staged')
        git('add','src/other.txt');index=git('ls-files','--stage','src/other.txt')
        (root/'message.txt').write_text('fixture owned commit')
        receipt=call('commit','commit-a','-Batch',bid2,'-Revision',0,'-Files','src/a.txt','-MessageFile',str(root/'message.txt'))
        assert receipt['otherIndexEntriesUnchanged'] and index==git('ls-files','--stage','src/other.txt')
        assert git('diff-tree','--no-commit-id','--name-only','-r','HEAD')=='src/a.txt'
        print('PASS ownership conflict and commit-only preserves other staged files')
        call('recover','commit-a','-Batch',bid2,'-ConfirmStopped','-Reason','fixture editor stopped')
        call('recover','commit-b','-Batch',bid2,'-ConfirmStopped','-Reason','fixture editor stopped',code=0)

        e=begin('failure');bid3=e['batchId'];(root/'fail-test').write_text('17')
        failed=call('finish','failure','-Batch',bid3,'-Revision',0,code=17);assert failed['outcome']=='failed'
        (root/'fail-test').unlink();f=begin('next');assert f['batchId']!=bid3
        call('finish','next','-Batch',f['batchId'],'-Revision',0);assert state()['current'] is None
        print('PASS failure result saved, lease released, next batch isolated')

        g=begin('stale');bid4=g['batchId'];plan('stale',bid4,0);h=begin('peer')
        # Force an interrupted check marker in this isolated fixture only.
        st=state();entry=st['current']['participants'][0];entry['state']='finished';entry['check']={'status':'running','owner':{'pid':2147483647,'startTicks':'0'},'editRevision':0}
        entry['registeredUtc']='2000-01-01T00:00:00Z'
        (state_root/'state.json').write_bytes(json.dumps(st).encode())
        proc=subprocess.Popen([PS,'-NoProfile','-File',str(scripts/'build_manager.ps1'),'finish','peer','-Batch',bid4,'-Revision','0','-WaitSeconds','1'],cwd=str(root),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        proc.communicate(timeout=8);assert proc.returncode==2
        assert state()['current']['phase']=='editing','crashed check or stale age must not permit freeze'
        call('check','stale','-Batch',bid4,'-Revision',0) # explicit read-only recovery
        call('finish','peer','-Batch',bid4,'-Revision',0)
        print('PASS stale registration and crashed check retain barrier until explicit recheck')

        # Literal inventory resolution: no shell/regex and no silent empty pass.
        unit.write_text("""$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. $PSScriptRoot/scripts/build_protocol.ps1
$e=[pscustomobject]@{editRevision=0};$bad=0
foreach($name in @('Alpha;whoami','Alpha|Beta','$(whoami)')){try{New-TaskPlan $e module selected $name src/a.txt mapping|Out-Null}catch{$bad++}}
if($bad -ne 3){throw 'injection accepted'}
$inv=@([pscustomobject]@{name='Alpha';properties=@([pscustomobject]@{name='LABELS';value=@('core')})},[pscustomobject]@{name='Beta';properties=@([pscustomobject]@{name='LABELS';value=@('integration')})},[pscustomobject]@{name='Manual';properties=@([pscustomobject]@{name='LABELS';value=@('manual')})})
$p=[pscustomobject]@{tests=@('Manual','Alpha');suites=@('full');mode='full';tasks=@()};$names=@(Resolve-PlanTests $p $inv)
if(($names -join ',') -ne 'Alpha,Beta,Manual'){throw 'incorrect dedup/manual union'}
$p.tests=@('Missing');try{Resolve-PlanTests $p $inv|Out-Null;throw 'empty pass'}catch{if($_ -match 'empty pass'){throw}}
'PASS literal plans, merged dedup, explicit manual and unknown test rejection'
""")
        p=subprocess.run([PS,'-NoProfile','-File',str(unit)],text=True,capture_output=True);assert p.returncode==0,p.stderr;print(p.stdout.strip())
        # Legacy live batches cannot be upgraded underneath loaded callers.
        legacy=begin('legacy');legacy_id=legacy['batchId'];st=state();st['current'].pop('protocolVersion');(state_root/'state.json').write_bytes(json.dumps(st).encode())
        call('ready','legacy','-Batch',legacy_id,'-Revision',0,code=2)
        call('plan','legacy','-Batch',legacy_id,'-Revision',0,code=2)
        assert state()['current']['participants'][0]['state']=='editing'
        call('finish','legacy','-Batch',legacy_id) # legacy initial revision stays compatible
        print('PASS no forced migration of legacy active batch')

        # Check failure does not silently mark editing complete or authorize a
        # freeze; a later explicit recheck has no semantic repair authority.
        (root/'src/a.txt').write_text('trailing whitespace   \n')
        failcheck=begin('check-failure');fid=failcheck['batchId'];peer=begin('checking-peer')
        plan('check-failure',fid,0);out=ready('check-failure',fid,0)
        until(lambda: state()['current']['participants'][0].get('check',{}).get('status')=='failed')
        assert state()['current']['phase']=='editing'
        reopened=begin('check-failure');assert reopened['editRevision']==1
        (root/'src/a.txt').write_text('repaired input\n');plan('check-failure',fid,1);ready('check-failure',fid,1)
        call('finish','checking-peer','-Batch',fid,'-Revision',0)
        assert read_result(fid)['outcome']=='passed'
        issue=call('issue','check-failure','-Batch',fid,'-Reason','explicit fixture handoff','-Assignee','checking-peer')
        call('issue','checking-peer','-Batch',fid,'-IssueId',issue['id'],'-IssueState','resolved','-Reason','reviewed fixture resolution','-Assignee','checking-peer')
        print('PASS failed checks require reopen/repair and explicit handoff remains batch-associated')

        # A declared input changes after ready: frozen plan cannot reuse it.
        fresh=begin('freshness');sid=fresh['batchId'];peer=begin('freshness-peer');plan('freshness',sid,0);ready('freshness',sid,0)
        until(lambda: state()['current']['participants'][0].get('check',{}).get('status')=='passed')
        (root/'src/a.txt').write_text('unexpected change after ready')
        call('finish','freshness-peer','-Batch',sid,'-Revision',0,code=2)
        assert state()['current']['phase']=='editing' and state()['current']['planStatus']=='blocked'
        reopened=begin('freshness');plan('freshness',sid,reopened['editRevision']);ready('freshness',sid,reopened['editRevision'])
        call('finish','freshness-peer','-Batch',sid,'-Revision',0)
        print('PASS changed declared input blocks freeze until explicit new revision and plan')
        print('WORKFLOW PASSED; evidence fixture: '+str(root))
    finally:
        # Only worker identities returned by this fixture's ready calls.
        import importlib.util
        spec=importlib.util.spec_from_file_location('reader_cleanup',repo/'tools/build-dashboard/server.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
        for owner in owned:
            if mod.owner_state(owner)=='alive':
                handle=ctypes.windll.kernel32.OpenProcess(1,False,owner['pid'])
                if handle:ctypes.windll.kernel32.TerminateProcess(handle,4);ctypes.windll.kernel32.CloseHandle(handle)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);args=parser.parse_args();run_tests(args.repo)
