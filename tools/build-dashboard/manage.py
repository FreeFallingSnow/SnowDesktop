"""Manual local dashboard lifecycle. No PowerShell, downloads or installation."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from urllib.error import URLError
from urllib.request import ProxyHandler, Request, build_opener

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('start','stop','status'),nargs='?',default='start')
    parser.add_argument('--port',type=int)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    root=args.root.resolve();directory=root/'.build/dashboard';preferred=directory/'preferred-port.json'
    for part in (root/'.build',directory):
        if part.exists() and (part.is_symlink() or getattr(part.stat(),'st_file_attributes',0)&0x400):parser.exit(3,'Receipt directories must not be reparse points.\n')
    if args.port is None:
        args.port=8765
        try:
            if preferred.is_symlink() or getattr(preferred.stat(),'st_file_attributes',0)&0x400:raise ValueError('Linked preference')
            with preferred.open('rb') as stream:raw=stream.read(1025)
            if len(raw)>1024:raise ValueError('Oversized preference')
            data=json.loads(raw)
            if data.get('schemaVersion')!=1 or type(data.get('port')) is not int:raise ValueError('Invalid preference')
            args.port=data['port']
        except FileNotFoundError:pass
        except (OSError,ValueError):parser.exit(3,'Invalid dashboard port preference; preserve it and select --port explicitly.\n')
    if not 1024<=args.port<=65535:parser.error('Port must be 1024..65535')
    receipt=directory/('server-'+str(args.port)+'.json')
    url='http://127.0.0.1:'+str(args.port)+'/'
    opener=build_opener(ProxyHandler({})) # Never route localhost through an external proxy.
    def owned():
        try:
            if receipt.is_symlink() or getattr(receipt.stat(),'st_file_attributes',0)&0x400:return None
            data=json.loads(receipt.read_bytes()[:262144])
            with opener.open(Request(url+'api/health'),timeout=1) as response:health=json.loads(response.read(4096))
            if health.get('service')=='SnowDesktop build dashboard' and health.get('instance')==data.get('instance') and not health.get('fixture'):return data
        except (OSError,ValueError,URLError):pass
        return None
    def remember():
        temporary=directory/('preferred-'+os.urandom(8).hex()+'.tmp')
        try:
            with temporary.open('x',encoding='utf-8') as stream:
                json.dump({'schemaVersion':1,'port':args.port},stream)
                stream.flush();os.fsync(stream.fileno())
            os.replace(str(temporary),str(preferred))
        finally:
            if temporary.exists():temporary.unlink()
    existing=owned()
    if args.command=='status':
        print(json.dumps(existing) if existing else 'No verified dashboard instance; no listener was changed.')
        return 0 if existing else 1
    if args.command=='stop':
        if not existing:parser.exit(3,'No verified instance; an unknown listener will not be stopped.\n')
        token=existing['instance']
        if len(token)!=32 or any(x not in 'abcdef0123456789' for x in token):parser.exit(3,'Invalid instance receipt.\n')
        (directory/('stop-'+token+'.request')).write_text(token)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            if not owned():print('Dashboard stopped; the build is independent.');return 0
            time.sleep(.2)
        parser.exit(3,'Stop request sent, but shutdown has not been confirmed.\n')
    if existing:remember();print(url);return 0
    with socket.socket() as probe:
        probe.settimeout(.5)
        if probe.connect_ex(('127.0.0.1',args.port))==0:parser.exit(3,'Port occupied by an unverified listener. Choose --port; no process was stopped.\n')
    if sys.version_info<(3,8):parser.exit(3,'An installed Python 3.8+ is required; no tools will be installed.\n')
    directory.mkdir(parents=True,exist_ok=True)
    for part in (root/'.build',directory):
        if part.is_symlink() or getattr(part.stat(),'st_file_attributes',0)&0x400:parser.exit(3,'Receipt directories must not be reparse points.\n')
    stdout=directory/('server-'+str(args.port)+'.out');stderr=directory/('server-'+str(args.port)+'.err')
    with open(str(stdout),'ab') as out,open(str(stderr),'ab') as err:
        flags=(subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        command=[sys.executable,str(Path(__file__).with_name('server.py')),'--port',str(args.port),'--root',str(root)]
        if os.name=='nt':
            try:
                process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB,close_fds=True)
            except OSError as error:
                if error.winerror!=5:raise
                # A restrictive parent Job may refuse breakaway. Its policy is
                # retained; the optional observer then lives within that Job.
                process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=flags,close_fds=True)
        else:
            process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=flags,close_fds=True)
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        if owned():remember();print(url);return 0
        if process.poll() is not None:parser.exit(3,'Server exited; inspect .build/dashboard log. No unrelated listener was stopped.\n')
        time.sleep(.2)
    parser.exit(3,'Startup unconfirmed; inspect the log. No unrelated process was stopped.\n')

if __name__=='__main__':
    sys.exit(main())
