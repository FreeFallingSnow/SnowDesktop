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
    parser.add_argument('--port',type=int,default=8765)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    if not 1024<=args.port<=65535:parser.error('Port must be 1024..65535')
    root=args.root.resolve();directory=root/'.build/dashboard';receipt=directory/('server-'+str(args.port)+'.json')
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
    if existing:print(url);return 0
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
        process=subprocess.Popen([sys.executable,str(Path(__file__).with_name('server.py')),'--port',str(args.port),'--root',str(root)],stdin=subprocess.DEVNULL,stdout=out,stderr=err,creationflags=flags,close_fds=True)
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        if owned():print(url);return 0
        if process.poll() is not None:parser.exit(3,'Server exited; inspect .build/dashboard log. No unrelated listener was stopped.\n')
        time.sleep(.2)
    parser.exit(3,'Startup unconfirmed; inspect the log. No unrelated process was stopped.\n')

if __name__=='__main__':
    sys.exit(main())
