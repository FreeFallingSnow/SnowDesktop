"""HTTP safety + real headless Edge rendering on explicitly labeled fixtures."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener

sys.dont_write_bytecode=True

class IsolatedPortRace(Exception):
    """Only server bind exit 3 before assertions; finally releases fixture resources."""

def free_port():
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));return sock.getsockname()[1]

def main(repo,browser):
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-dashboard-'));state=root/'.build/collaboration';state.mkdir(parents=True)
    port=free_port();url='http://127.0.0.1:'+str(port)
    server=subprocess.Popen([sys.executable,str(repo/'tools/build-dashboard/server.py'),'--fixture-root',str(root),'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    opener=build_opener(ProxyHandler({}))
    def get(path='/api/status',headers=None,method='GET'):
        try:
            with opener.open(Request(url+path,headers=headers or {},method=method),timeout=3) as response:return response.status,response.read(),response.headers
        except HTTPError as error:return error.code,error.read(),error.headers
    def until(fn):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                if fn():return
            except (URLError,OSError):
                if startup and server.poll() == 3:
                    raise IsolatedPortRace('Fixture server lost its ephemeral listen port before assertions')
            time.sleep(.1)
        raise AssertionError('dashboard wait timeout')
    def write(name,value):(state/name).write_bytes(json.dumps(value,ensure_ascii=False).encode())
    bid='d'*32
    def task(name='task-a',check=None,date='2026-10-02T10:00:00Z'):
        result={'id':name,'state':'finished','editRevision':1,'registeredUtc':date,'reopenedUtc':date,'testPlan':{'scope':'module','suites':['selected'],'tests':['Alpha'],'reason':'fixture scoped input'},'ownedFiles':['src/fixture.cpp']}
        if check:result['check']=check
        return result
    def current(phase='editing',participants=None,owner=None):
        return {'schemaVersion':1,'repositoryRoot':str(root),'current':{'id':bid,'protocolVersion':2,'phase':phase,'participants':participants or [task(),dict(task('task-b'),state='editing')],'createdUtc':'2026-10-02T10:00:00Z','owner':owner,'inputStart':{'algorithm':'fixture','digest':'f'*64,'fileCount':2}}}
    edge=None;debug=None
    startup=True
    try:
        write('state.json',current());until(lambda:get('/api/health')[0]==200)
        startup=False
        assert get('/api/status')[0]==200 and json.loads(get()[1])['fixture']
        for path,headers,method,code in [('/api/status',{'Host':'evil.example'},'GET',403),('/api/status',{'Origin':'http://evil.example'},'GET',403),('/api/status',{'Sec-Fetch-Site':'cross-site'},'GET',403),('/api/status?path=C:/Windows','GET' if False else {},'GET',400),('/api/batches/%2e%2e%2fstate.json',{},'GET',400),('/api/status',{},'POST',405),('/api/exec',{},'GET',404)]:
            assert get(path,headers,method)[0]==code,(path,code)
        headers=get('/')[2];assert "script-src 'self'" in headers['Content-Security-Policy'] and 'Access-Control-Allow-Origin' not in headers
        # An unrelated occupied listener is never terminated or repurposed.
        conflict=subprocess.run([sys.executable,str(repo/'tools/build-dashboard/manage.py'),'start','--root',str(root),'--port',str(port)],capture_output=True,text=True)
        assert conflict.returncode==3 and get('/api/health')[0]==200
        before={x.name:x.read_bytes() for x in state.glob('*.json')}
        for _ in range(5):get('/api/status')
        assert before=={x.name:x.read_bytes() for x in state.glob('*.json')},'HTTP must not mutate coordination state'
        print('PASS readonly HTTP, loopback, Host/Origin/CORS, traversal, verbs, CSP, occupied-port refusal')
        spec=importlib.util.spec_from_file_location('server_unit',repo/'tools/build-dashboard/server.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        assert module.redact('token=fixture-secret password:abc')=='token=[redacted] password:[redacted]'
        lines=['compile.cpp']*25000+['1/2 Test #1: Alpha .... Passed','2/2 Test #2: Beta .... Failed','token=secret-value','<script>window.dashboardXss=1</script>']
        (state/(bid+'.log')).write_text('\n'.join(lines)+'\n')
        time.sleep(2.1);data=json.loads(get()[1])['current']['log'];assert len(data['lines'])<=180 and data['testCompleted']==2 and data['testTotal']==2
        assert b'secret-value' not in get()[1] and data['prefixOmitted']
        print('PASS bounded incremental long-log tail, actual test counts and credential redaction')
        if browser:
            edge_path=Path(r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
            assert edge_path.exists(),'Edge unavailable; no browser installation allowed'
            debug=free_port();profile=root/'edge-profile'
            edge=subprocess.Popen([str(edge_path),'--headless=new','--disable-gpu','--remote-debugging-port='+str(debug),'--user-data-dir='+str(profile),'about:blank'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
            def debug_ready():
                with opener.open('http://127.0.0.1:'+str(debug)+'/json/version',timeout=2) as response:return response.status==200
            until(debug_ready)
            def render(name,expected):
                time.sleep(2.1)
                p=subprocess.run(['node',str(repo/'tools/build-dashboard/browser_check.js'),str(debug),url+'/',expected,str(root/(name+'.png'))],capture_output=True,text=True,encoding="utf-8",timeout=25)
                assert p.returncode==0,(name,p.stderr,p.stdout);print('BROWSER '+name+' '+p.stdout.strip())
            render('waiting','编辑中')
            write('state.json',current(participants=[task(check={'status':'pending','source':'builtin-basic','editRevision':1}),dict(task('task-b'),state='editing')]))
            render('checking','编辑中')
            # Live owner identity from this fixture's Python process.
            kernel=ctypes.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ctypes.c_void_p;handle=kernel.GetCurrentProcess();values=[ctypes.c_ulonglong() for _ in range(4)];kernel.GetProcessTimes(ctypes.c_void_p(handle),*[ctypes.byref(x) for x in values]);owner={'pid':os.getpid(),'startTicks':str(values[0].value+504911232000000000)}
            write('state.json',current('building',[task(),task('task-b')],owner));render('running','构建 / 测试中')
            write('state.json',current(participants=[task(date='2000-01-01T00:00:00Z'),dict(task('peer'),state='editing')]))
            render('stale','编辑中')
            write('state.json',current('building',[task()],{'pid':2147483647,'startTicks':'0'}));render('interrupted','运行已中断')
            write('state.json',{'schemaVersion':1,'repositoryRoot':str(root),'current':None})
            result={'schemaVersion':1,'batchId':bid,'outcome':'passed','exitCode':0,'completedUtc':'2026-10-02T10:00:00Z','participants':[task('<script>window.dashboardXss=1</script>')],'inputCheck':'stable','inputEnd':{'algorithm':'fixture','digest':'c'*64,'fileCount':2}}
            write(bid+'.json',result);render('passed','历史 · 验证通过')
            result['outcome']='failed';result['exitCode']=17;result['error']='fixture test failed <script>window.dashboardXss=1</script>';write(bid+'.json',result);render('failed-long-log','历史 · 验证失败')
        # Actual new view models: durable wait, logical repair and bounded retry evidence.
        child='e'*32;ticket='f'*32
        write(ticket+'.wait.json',{'id':ticket,'participant':'await-owner','condition':'files','status':'eligible','attempt':1,'reason':'编辑窗口曾可用','next':'begin / claim 后才能写入','createdUtc':'2026-10-02T10:00:00Z','deadlineUtc':'2026-10-02T10:30:00Z'})
        value=current('building',[task()],owner if browser else None);value['current'].update(id=child,logicalBatchId=bid,repairOf=bid,attempt=2);write('state.json',value)
        write(child+'.coverage.json',{'batchId':child,'mode':'selected','status':'passed-after-retry','selected':['build_dashboard'],'completed':[{'name':'build_dashboard','status':'passed-after-retry'}],'tasks':[{'participant':'task-a','requested':['build_dashboard'],'status':'passed-after-retry'}]})
        write(child+'.retry.json',{'batchId':child,'status':'passed-after-retry','selected':['build_dashboard'],'policy':{'maxAttempts':3},'tests':{'build_dashboard':{'status':'passed-after-retry','failureCount':1,'firstReason':'首次端口竞争 <script>window.dashboardXss=1</script>','finalReason':'重试通过','attempts':[{'attempt':1,'status':'failed'},{'attempt':2,'status':'passed','reason':'已通过'}]}}})
        attempts=state/(child+'.retry');attempts.mkdir();(attempts/'build_dashboard.attempt2.log').write_text('controlled retry log\ntoken=fixture-secret\n')
        time.sleep(2.1);view=json.loads(get()[1]);assert view['waits'][0]['status']=='eligible' and view['current']['repairOf']==bid and view['current']['attempt']==2
        assert view['current']['retry']['tests'][0]['failureCount']==1 and view['current']['coverage']['selected']==['build_dashboard']
        code,body,_=get('/api/retries/'+child+'/build_dashboard/2/log');assert code==200 and b'fixture-secret' not in body
        assert get('/api/retries/'+child+'/Other/2/log')[0]==404 and get('/api/retries/'+child+'/build_dashboard/4/log')[0]==404
        if browser:render('repair-and-retry','构建 / 测试中')
        print('PASS durable wait, linked repair attempt, original retry denominator, first/final causes, fixed log route and redaction')
        # Stop only this fixture's verified nonce; build state is independent.
        info=json.loads((root/'.build/dashboard'/('server-'+str(port)+'.json')).read_text())
        (root/'.build/dashboard'/('stop-'+info['instance']+'.request')).write_text(info['instance'])
        server.wait(timeout=8);assert server.returncode==0
        print('DASHBOARD PASSED; browser screenshots/evidence: '+str(root))
    finally:
        if edge and edge.poll() is None:
            # This process/profile was launched exclusively by the fixture.
            edge.terminate();edge.wait(timeout=8)
        if server.poll() is None:server.terminate();server.wait(timeout=8)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1]);parser.add_argument('--browser',action='store_true');args=parser.parse_args()
    try:
        main(args.repo,args.browser)
    except IsolatedPortRace as error:
        # main's finally has released its server/browser. No assertion/timeout is reclassified.
        sys.path.insert(0,str(args.repo/'scripts'))
        from build_test_retry import emit_resource_signal
        name='build_dashboard_browser' if args.browser else 'build_dashboard'
        if emit_resource_signal(name,error):
            print(str(error),file=sys.stderr);sys.exit(75)
        raise
