"""Actual serialized resource command on independent, labeled fixture state."""
import argparse
import concurrent.futures
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode=True

def main(repo):
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-shared-resource-'))
    for name in ['scripts/build_shared_resources.py','scripts/shared_resources.json','scripts/build_wait_tasks.py','scripts/build_ownership.ps1','tools/build-dashboard/server.py']:
        target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/name,target)
    folder=root/'.build/collaboration';folder.mkdir(parents=True)
    (root/'lang').mkdir()
    file=root/'lang/en-US.json'
    file.write_bytes(b'\xef\xbb\xbf{\r\n  "Alpha": "old-a",\r\n  "Beta": "old-b",\r\n  "Unrelated": "keep"\r\n}\r\n')
    batch='a'*32
    state={'schemaVersion':1,'repositoryRoot':str(root),'current':{'id':batch,'protocolVersion':2,'phase':'editing','participants':[{'id':'a','state':'editing','editRevision':0,'ownedFiles':['lang/en-US.json']},{'id':'b','state':'editing','editRevision':0,'ownedFiles':['lang/en-US.json']}]}}
    def save():(folder/'state.json').write_text(json.dumps(state),encoding='utf-8')
    save()
    def call(action,actor='a',request=None,keys='Alpha',code=0,extra=None):
        command=[sys.executable,str(root/'scripts/build_shared_resources.py'),'resource',action,actor,'--root',str(root),'--batch',batch,'--revision','0']
        if action=='prepare':command+=['--file','lang/en-US.json','--keys',keys]
        if request:command+=['--request',request]
        if extra:command+=extra
        result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',timeout=10)
        assert result.returncode==code,(command,result.stdout,result.stderr)
        return json.loads(result.stdout) if code==0 else result.stderr
    def change(record,values):Path(record['patchFile']).write_text(json.dumps(values,ensure_ascii=False),encoding='utf-8')
    try:
        assert 'explicit shared language' in call('prepare',code=2,extra=['--file','src/unlisted.cpp'])
        a=call('prepare',keys='Alpha');b=call('prepare','b',keys='Beta')
        change(a,{'Alpha':'new-a'});change(b,{'Beta':'new-b'})
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            # State/kernel transaction is deliberately nonblocking; the busy caller retries the same request.
            def apply(actor,record):
                deadline=time.monotonic()+5
                while True:
                    result=subprocess.run([sys.executable,str(root/'scripts/build_shared_resources.py'),'resource','apply',actor,'--root',str(root),'--batch',batch,'--revision','0','--request',record['requestId']],capture_output=True,text=True,encoding='utf-8')
                    if result.returncode==0:return json.loads(result.stdout)
                    if 'lease is occupied' not in result.stderr or time.monotonic()>=deadline:raise AssertionError(result.stderr)
                    time.sleep(.05)
            results=list(pool.map(lambda item:apply(*item),[('a',a),('b',b)]))
        assert all(item['status']=='applied' for item in results)
        raw=file.read_bytes();values=json.loads(raw.decode('utf-8-sig'))
        assert values=={'Alpha':'new-a','Beta':'new-b','Unrelated':'keep'} and raw.startswith(b'\xef\xbb\xbf') and raw.count(b'\r\n')==5
        assert b'  "Unrelated": "keep"' in raw
        assert call('apply',request=a['requestId'])['afterHash']==results[0]['afterHash'] and raw==file.read_bytes()
        print('PASS simultaneous nonoverlapping keys, latest-file re-read, BOM/CRLF/order preservation and idempotent request')
        x=call('prepare',keys='Alpha');y=call('prepare','b',keys='Alpha')
        change(x,{'Alpha':'first'});change(y,{'Alpha':'second'});call('apply',request=x['requestId'])
        before=file.read_bytes();assert 'Same-key conflict' in call('apply','b',request=y['requestId'],code=2) and before==file.read_bytes()
        print('PASS same-key stale-value conflict refuses without overwriting')
        add=call('prepare',keys='New.Key');change(add,{'New.Key':'added'});call('apply',request=add['requestId']);assert json.loads(file.read_text(encoding='utf-8-sig'))['New.Key']=='added'
        assert b'"Alpha": "first"' in file.read_bytes() and b'"Beta": "new-b"' in file.read_bytes()
        expired=call('prepare',extra=['--timeout','1']);time.sleep(1.05);before=file.read_bytes();assert 'expired' in call('apply',request=expired['requestId'],code=2) and before==file.read_bytes()
        late=call('prepare');state['current']['participants'][0]['editRevision']=1;save();assert 'Stale edit token' in call('apply',request=late['requestId'],code=2)
        state['current']['participants'][0]['editRevision']=0;state['current']['participants'][0]['state']='finished';save();assert 'Stale edit token' in call('apply',request=late['requestId'],code=2)
        state['current']['participants'][0]['state']='editing';state['current']['phase']='building';save();assert 'frozen' in call('apply',request=late['requestId'],code=2)
        state['current']['phase']='editing';state['current']['participants'][1]['ownedFiles']=['lang'];save();assert 'structural' in call('prepare',code=2)
        print('PASS additions, expired request, old revision, ready editor, frozen build and active structural edit protection')
        state['current']['participants'][1]['ownedFiles']=[];save()
        unknown=call('prepare',keys='Alpha')
        record_path=folder/(unknown['requestId']+'.resource.json');record=json.loads(record_path.read_text(encoding='utf-8'));record.update(status='writing',afterHash='0'*64);record_path.write_text(json.dumps(record),encoding='utf-8')
        before=file.read_bytes();assert 'uncertain outcome' in call('apply',request=unknown['requestId'],code=2) and file.read_bytes()==before
        print('PASS interrupted intent has diagnostic evidence and never replays an uncertain write')
        kernel=ctypes.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ctypes.c_void_p;handle=kernel.GetCurrentProcess();times=[ctypes.c_ulonglong() for _ in range(4)];assert kernel.GetProcessTimes(ctypes.c_void_p(handle),*[ctypes.byref(item) for item in times])
        peer=state['current']['participants'][1];peer.update(state='finished',testPlan={'source':'task-declared','inputs':['lang/en-US.json'],'suites':['selected'],'tests':['Alpha']},check={'status':'passed'},waiter={'pid':os.getpid(),'startTicks':str(times[0].value+504911232000000000)})
        save();transfer=call('prepare',keys='Beta');change(transfer,{'Beta':'shared follow-up'});call('apply',request=transfer['requestId'])
        carried=json.loads((folder/'state.json').read_text(encoding='utf-8'))['current']['participants'][1]
        assert carried['check']['status']=='pending' and carried['testPlan']['source']=='repair-carried' and carried['testPlan']['handoffCarried'] and carried['testPlan']['tests']==['Alpha'] and carried['handoffPreviousCheck']['status']=='passed'
        print('PASS shared follow-up preserves ready peer requirements/evidence and schedules deferred current-input recheck without original conversation edits')
        # Actual ownership function: provenance remains, current occupancy can be transferred.
        fixture=root/'ownership.ps1'
        fixture.write_text('''$ErrorActionPreference='Stop'
function Get-Field($Object,$Name,$Default=$null){if($Object -and $Object.PSObject.Properties[$Name]){return $Object.$Name};return $Default}
function Set-Field($Object,$Name,$Value){$Object|Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force}
function Get-EditRevision($Object){return $Object.editRevision}
function Start-ReadyWorker($Current,$Entry){return [pscustomobject]@{pid=1;startTicks='1';editRevision=$Entry.editRevision}}
$repositoryRoot=$PSScriptRoot
. (Join-Path $PSScriptRoot 'scripts/build_ownership.ps1')
$a=[pscustomobject]@{id='a';state='finished';editRevision=0;ownedFiles=@('src/a.cpp');testPlan=[pscustomobject]@{source='task-declared';inputIdentity='old'};check=[pscustomobject]@{status='passed'}}
$b=[pscustomobject]@{id='b';state='editing';editRevision=0;ownedFiles=@()}
$current=[pscustomobject]@{participants=@($a,$b)}
Set-Ownership $current $b 'src/a.cpp' $true
if($b.ownedFiles[0] -ne 'src/a.cpp' -or $a.ownedFiles[0] -ne 'src/a.cpp' -or $a.check.status -ne 'pending' -or $a.testPlan.source -ne 'repair-carried' -or -not $a.testPlan.handoffCarried){throw 'Finished provenance/transfer/recheck incorrect'}
$a.state='editing'
try{Set-Ownership $current $b 'src/a.cpp' $true;throw 'Active claim accepted'}catch{if($_.Exception.Message -eq 'Active claim accepted'){throw}}
$a.ownedFiles=@('lang/en-US.json');Set-Ownership $current $b 'lang/en-US.json' $true
try{Set-Ownership $current $b 'lang' $true;throw 'Structural claim accepted'}catch{if($_.Exception.Message -eq 'Structural claim accepted'){throw}}
Write-Output 'PASS finished owner handoff, provenance, deferred recheck, active code conflict, shared language exemption and structural claim exclusion'
''',encoding='utf-8-sig')
        result=subprocess.run(['powershell.exe','-NoProfile','-File',str(fixture)],capture_output=True,text=True,timeout=15)
        assert result.returncode==0,(result.stdout,result.stderr);print(result.stdout.strip())
        print('SHARED RESOURCE PASSED; fixture: '+str(root))
    finally:
        # No workers or user applications are launched by this fixture.
        shutil.rmtree(str(root))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);args=parser.parse_args();main(args.repo)
