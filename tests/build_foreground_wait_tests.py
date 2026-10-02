"""Real foreground tool processes; no periodic status subprocesses or model calls."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode=True

def main(repo):
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-foreground-wait-'))
    for name in ['scripts/build_wait_tasks.py','tools/build-dashboard/server.py']:
        target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/name,target)
    directory=root/'.build/collaboration';directory.mkdir(parents=True)
    spec=importlib.util.spec_from_file_location('foreground_wait_impl',root/'scripts/build_wait_tasks.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    bid='a'*32
    state={'schemaVersion':1,'repositoryRoot':str(root),'current':{'id':bid,'phase':'building','owner':module.process_owner(os.getpid()),'participants':[{'id':'peer','state':'finished'}]}}
    def save():module.atomic(directory/'state.json',state)
    save();processes=[];owners=[]
    def cli(*args):
        result=subprocess.run([sys.executable,str(root/'scripts/build_wait_tasks.py'),'watch',*args,'--root',str(root)],capture_output=True,text=True,encoding='utf-8',timeout=10)
        assert result.returncode==0,(args,result.stderr,result.stdout);return json.loads(result.stdout)
    def begin(actor,extra=None):
        command=[sys.executable,str(root/'scripts/build_wait_tasks.py'),'watch','wait',actor,'--root',str(root),'--timeout','20']+(extra or [])
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8');processes.append(process)
        line=process.stderr.readline();match=re.search(r'ticket ([a-f0-9]{32})',line);assert match,(line,process.poll())
        ticket=json.loads((directory/(match.group(1)+'.wait.json')).read_text(encoding='utf-8'));owners.append(ticket['owner']);return process,ticket
    def output(process,status,code=0):
        stdout,stderr=process.communicate(timeout=6);assert process.returncode==code,(stdout,stderr);value=json.loads(stdout);assert value['status']==status,value;return value
    def kill_worker(owner):
        reader=module.reader(root)
        if reader.owner_state(owner)=='alive':
            kernel=ctypes.WinDLL('kernel32');kernel.OpenProcess.restype=ctypes.c_void_p;handle=kernel.OpenProcess(1,False,owner['pid']);assert handle
            try:assert kernel.TerminateProcess(ctypes.c_void_p(handle),4)
            finally:kernel.CloseHandle(ctypes.c_void_p(handle))
    try:
        p,t=begin('window');pid=p.pid;time.sleep(3);assert p.poll() is None and p.pid==pid
        q,u=begin('',extra=['--ticket',t['id']]);time.sleep(2);assert q.poll() is None and p.poll() is None and u['owner']==t['owner']
        started=time.monotonic();state['current']=None;save();a=output(p,'eligible');b=output(q,'eligible')
        assert time.monotonic()-started<4 and a['id']==b['id'] and a['owner']==t['owner']
        print('PASS foreground truly blocks, unchanged PID continues waiting, reconnect reuses worker, condition change returns promptly',flush=True)
        state['current']={'id':bid,'phase':'building','owner':module.process_owner(os.getpid()),'participants':[{'id':'retirement-race','state':'finished','editRevision':1}]};save()
        p,t=begin('retirement-race',extra=['--condition','result','--batch',bid,'--revision','1'])
        with module.lease(directory/'state.lock'):
            result={'batchId':bid,'outcome':'passed','participants':[{'id':'retirement-race','editRevision':1}]}
            module.atomic(directory/(bid+'.json'),result);time.sleep(2.2);assert p.poll() is None,'A live retirement transaction must not return interrupted/attention'
            state['current']=None;save()
        completed=output(p,'completed');assert completed['result']==result
        (directory/(bid+'.json')).unlink()
        print('PASS durable-result/retirement gap remains blocked under the metadata transaction and returns the common result after release',flush=True)
        state['current']={'id':bid,'phase':'editing','participants':[{'id':'file-editor','state':'editing','ownedFiles':['src/own.cpp']},{'id':'peer','state':'editing','ownedFiles':['src/peer.cpp']}]};save()
        p,t=begin('file-editor',extra=['--condition','files','--files','src/peer.cpp']);time.sleep(.3);assert p.poll() is None
        state['current']['participants'][1]['state']='finished';save();output(p,'eligible')
        assert state['current']['participants'][0]['state']=='editing'
        print('PASS active own edit waits for a peer file handoff without requiring batch retirement or origin conversation edits',flush=True)
        state['current']={'id':bid,'phase':'editing','participants':[{'id':'result','editRevision':1,'state':'finished','check':{'status':'passed'}},{'id':'peer','state':'editing'}]};save()
        p,t=begin('result',extra=['--condition','result','--batch',bid,'--revision','1']);time.sleep(.3);assert p.poll() is None
        state['current']['participants'][0]['check']['status']='failed';save();output(p,'attention',2)
        print('PASS declared check failure returns attention without repeated checks/builds',flush=True)
        state['current']={'id':bid,'phase':'building','owner':module.process_owner(os.getpid()),'participants':[{'id':'peer','state':'finished'}]};save()
        p,t=begin('disconnected');p.terminate();p.wait(timeout=5)
        q,u=begin('',extra=['--ticket',t['id']]);assert t['owner']==u['owner'];cli('cancel','--ticket',t['id']);output(q,'cancelled',2)
        time.sleep(.2);resumed=cli('resume','--ticket',t['id'],'--timeout','20','--reason','Explicitly resume this isolated cancelled wait');owners.append(resumed['owner'])
        q,u=begin('',extra=['--ticket',t['id']]);state['current']=None;save();assert output(q,'eligible')['attempt']==2
        print('PASS foreground disconnect retains worker/ticket, explicit cancellation and same-ticket resume',flush=True)
        state['current']={'id':bid,'phase':'building','owner':module.process_owner(os.getpid()),'participants':[]};save()
        p,t=begin('crashed-worker');kill_worker(t['owner']);output(p,'interrupted',2)
        resumed=cli('resume','--ticket',t['id'],'--timeout','20','--reason','Verified fixture worker exited');owners.append(resumed['owner'])
        q,u=begin('',extra=['--ticket',t['id']]);state['current']=None;save();assert output(q,'eligible')['attempt']==2
        state['current']={'id':bid,'phase':'building','owner':module.process_owner(os.getpid()),'participants':[]};save()
        p,t=begin('bounded',extra=['--timeout','1']);output(p,'timed-out',2)
        print('PASS worker crash releases lease, explicit recovery and bounded timeout; no build command invoked',flush=True)
        print('FOREGROUND WAIT PASSED; fixture: '+str(root),flush=True)
    finally:
        for process in processes:
            if process.poll() is None:process.terminate();process.wait(timeout=5)
        for owner in owners:kill_worker(owner)
        # Reap/close Windows handles before removing the isolated fixture.
        time.sleep(.1);shutil.rmtree(str(root))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);args=parser.parse_args();main(args.repo)
