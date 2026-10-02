"""Durable observational waits. A completed wait never grants editing permission."""
import argparse
import contextlib
import ctypes
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HEX = re.compile(r'^[a-f0-9]{32}$')
TASK = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')
TERMINAL = {'eligible', 'completed', 'attention', 'timed-out', 'cancelled', 'interrupted'}

def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()

def reader(repo):
    spec = importlib.util.spec_from_file_location('wait_readonly', repo/'tools/build-dashboard/server.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def atomic(path, value):
    temporary = path.with_name(path.name + '.' + secrets.token_hex(8) + '.tmp')
    try:
        with open(str(temporary), 'xb') as stream:
            stream.write(json.dumps(value, ensure_ascii=False).encode('utf-8'))
            stream.flush()
            os.fsync(stream.fileno())
        deadline = time.monotonic() + 2
        while True:
            try:
                os.replace(str(temporary), str(path))
                break
            except OSError as error:
                # A Windows reader/scanner can temporarily deny replacement. Never delete the old snapshot.
                if os.name != 'nt' or getattr(error, 'winerror', None) not in (5, 32, 33) or time.monotonic() >= deadline:
                    raise
                time.sleep(.025)
    finally:
        if temporary.exists():
            temporary.unlink()

@contextlib.contextmanager
def lease(path):
    """Kernel ownership, automatically released on process exit."""
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        handle = kernel.CreateFileW(str(path), 0xC0000000, 0, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise BlockingIOError(ctypes.get_last_error(), 'Wait worker lease is occupied')
        try:
            yield
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    else:
        import fcntl
        with open(str(path), 'a+b') as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield

def process_owner(pid):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise OSError(ctypes.get_last_error(), 'Cannot identify wait worker')
    values = [ctypes.c_ulonglong() for _ in range(4)]
    try:
        if not kernel.GetProcessTimes(ctypes.c_void_p(handle), *[ctypes.byref(x) for x in values]):
            raise OSError('Cannot read process creation time')
        return {'pid': pid, 'startTicks': str(values[0].value + 504911232000000000)}
    finally:
        kernel.CloseHandle(ctypes.c_void_p(handle))

def paths(value):
    result = sorted(set(x.replace('\\', '/').rstrip('/') for x in value.split(',') if x))
    for path in result:
        if path == '.' or path.startswith('/') or re.search(r'(^|/)\.\.(/|$)|[:*?"<>|]', path):
            raise ValueError('Use literal repository relative files')
    return result

def overlaps(a, b):
    a, b = a.lower(), b.lower()
    return a == b or a.startswith(b + '/') or b.startswith(a + '/')

def condition(ticket, state, result):
    current = state.get('current')
    expected = ticket.get('batchId')
    if current and result and result.get('batchId') == current.get('id'):
        return 'attention', 'Result is durable but retirement is interrupted; use status/recover.'
    if ticket['condition'] in ('peers', 'result'):
        if result:
            members = [x for x in result.get('participants', []) if x.get('id') == ticket['participant']]
            if len(members) != 1 or members[0].get('editRevision', 0) != ticket['editRevision']:
                return 'attention', 'Saved result does not match the ticket participant/revision; use the latest receipt.'
            status = 'completed' if result.get('outcome') in ('passed', 'skipped') else 'attention'
            return status, 'Saved batch result: ' + str(result.get('outcome'))
        if not current or current.get('id') != expected:
            return 'attention', 'Expected batch is absent and has no result. Preserve metadata and diagnose.'
        own = [x for x in current['participants'] if x['id'] == ticket['participant']]
        if len(own) != 1 or own[0].get('editRevision', 0) != ticket['editRevision']:
            return 'attention', 'Registration/revision changed; resume with the latest begin receipt.'
        if current['phase'] == 'editing' and not any(x['state']=='editing' for x in current['participants']) and current.get('planStatus') == 'blocked':
            return 'attention', 'Frozen plan needs attention: ' + str(current.get('planError', 'inspect status'))[:2000]
        active = [x for x in current['participants'] if x['state'] == 'finished']
        if any((x.get('check') or {}).get('status') in ('failed', 'invalidated', 'interrupted') for x in active):
            return 'attention', 'A registered check needs a repair/decision; no repeated check was started.'
        if current['phase'] == 'building':
            return None, 'Frozen build is running.'
        peers = [x for x in current['participants'] if x['id'] != ticket['participant'] and x['state'] != 'withdrawn']
        if any(x['state'] == 'editing' for x in peers):
            return None, 'Peer editors are active; age is never treated as completion.'
        if any((x.get('check') or {}).get('status') in ('failed', 'invalidated', 'interrupted') for x in peers):
            return 'attention', 'A peer check needs a repair/decision.'
        if ticket['condition'] == 'peers' and all((x.get('check') or {}).get('status') not in ('pending', 'running') for x in peers):
            return 'eligible', 'Peer edit/check barrier is closed. Revalidate through begin/claim before writing.'
        return None, 'Waiting for saved batch result.'
    if current:
        if current['phase'] != 'editing':
            return None, 'Current batch is frozen.'
        if result and current.get('id') == expected:
            return 'attention', 'Retirement requires explicit recovery.'
        if ticket['condition'] == 'files':
            own_editing = any(x['id'] == ticket['participant'] and x['state'] == 'editing' for x in current['participants'])
            for entry in current['participants']:
                if entry['state'] == 'withdrawn' or entry['id'] == ticket['participant']:
                    continue
                if not entry.get('ownedFiles'):
                    return None, 'A peer has no declared ownership; file availability cannot be proved.'
                if any(overlaps(a, b) for a in ticket['files'] for b in entry['ownedFiles']):
                    if own_editing:
                        return 'attention', 'Your editing registration prevents batch retirement. Coordinate the shared file/handoff; waiting cannot release another owner.'
                    return None, 'A peer still owns a requested file, including ready registrations.'
    return 'eligible', 'An edit/claim window was observed. Execute the saved next step; permissions are rechecked atomically.'

def diagnostic(module, directory, ticket):
    receipt = module.read_json(directory, ticket['id'] + '.wait-attempt' + str(ticket['attempt']) + '.json')
    if receipt:
        return receipt
    if ticket['status'] == 'waiting' and (module.owner_state(ticket.get('owner')) == 'exited' or
            (not ticket.get('owner') and module.age(ticket['createdUtc']) > 5)):
        ticket = dict(ticket, status='interrupted', reason='Worker exited. Explicit resume is required; editors were not completed.')
    return ticket

def worker(repo, directory, module, ticket_id):
    with contextlib.ExitStack() as stack:
        deadline = time.monotonic() + 10
        while True:
            try:
                stack.enter_context(lease(directory/(ticket_id + '.wait-worker.lock')))
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    return
                time.sleep(.1)
        ticket = module.read_json(directory, ticket_id + '.wait.json')
        if ticket['status'] != 'waiting':
            return
        ticket['owner'] = process_owner(os.getpid())
        atomic(directory/(ticket_id + '.wait.json'), ticket)
        heartbeat = time.monotonic()
        while True:
            if (directory/(ticket_id + '.wait-cancel')).exists():
                status, reason = 'cancelled', 'Explicit cancellation; registrations were not changed.'
            elif dt.datetime.now(dt.timezone.utc) >= dt.datetime.fromisoformat(ticket['deadlineUtc']):
                status, reason = 'timed-out', 'Bounded wait expired; inspect/resume explicitly. Registrations were retained.'
            else:
                try:
                    state = module.read_json(directory, 'state.json') or {'current': None}
                    if state.get('repositoryRoot') and Path(state['repositoryRoot']).resolve() != repo:
                        raise ValueError('Repository identity mismatch')
                    current = state.get('current')
                    if current and current.get('phase') not in ('editing', 'building'):
                        raise ValueError('Unknown batch phase')
                    if current:
                        for entry in current['participants']:
                            check = entry.get('check') or {}
                            if check.get('status') == 'running' and module.owner_state(check.get('owner')) == 'exited':
                                check['status'] = 'interrupted'  # This local snapshot is never written back.
                    result_id = ticket.get('batchId') or (current or {}).get('id')
                    result = module.read_json(directory, result_id + '.json') if result_id and HEX.fullmatch(result_id) else None
                    if result and result.get('batchId') != result_id:
                        raise ValueError('Saved result identity mismatch')
                    if current and current.get('phase') == 'building' and module.owner_state(current.get('owner')) == 'exited':
                        status, reason = 'attention', 'Build owner exited; diagnose and explicitly recover the frozen batch.'
                    else:
                        status, reason = condition(ticket, state, result)
                except (OSError, ValueError, KeyError, TypeError) as error:
                    status, reason = 'attention', type(error).__name__ + ': state unavailable; no registration changed.'
            ticket['observations'] += 1
            ticket['reason'] = reason
            if status:
                ticket.update(status=status, completedUtc=utc())
                # Terminal receipt first. A crash before the summary is updated is reconciled by status/resume.
                atomic(directory/(ticket_id + '.wait-attempt' + str(ticket['attempt']) + '.json'), ticket)
                atomic(directory/(ticket_id + '.wait.json'), ticket)
                return
            if time.monotonic() - heartbeat >= 15:
                ticket['heartbeatUtc'] = utc()
                atomic(directory/(ticket_id + '.wait.json'), ticket)
                heartbeat = time.monotonic()
            time.sleep(2)  # Local observation only: no checks, builds, model/API calls or permission grants.

def spawn(repo, directory, ticket):
    with open(str(directory/(ticket['id'] + '.wait.out')), 'ab') as out:
        child = subprocess.Popen([sys.executable, str(repo/'scripts/build_wait_tasks.py'), 'watch', '_worker',
                                  '--root', str(repo), '--ticket', ticket['id']], stdin=subprocess.DEVNULL,
                                 stdout=out, stderr=out, close_fds=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    ticket['owner'] = process_owner(child.pid)
    atomic(directory/(ticket['id'] + '.wait.json'), ticket)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('entry', choices=('watch',))
    p.add_argument('action', choices=('start', 'status', 'resume', 'cancel', '_worker'))
    p.add_argument('participant', nargs='?')
    p.add_argument('--condition', choices=('window', 'files', 'peers', 'result'), default='window')
    p.add_argument('--batch', default='')
    p.add_argument('--revision', type=int, default=-1)
    p.add_argument('--files', default='')
    p.add_argument('--ticket', default='')
    p.add_argument('--timeout', type=int, default=1800)
    p.add_argument('--next', default='Read this ticket once, then call begin/claim with the latest receipt before editing.')
    p.add_argument('--reason', default='')
    p.add_argument('--new', action='store_true')
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = p.parse_args()
    if not 1 <= args.timeout <= 86400:
        p.error('Timeout must be 1..86400 seconds')
    repo = args.root.resolve()
    directory = repo/'.build/collaboration'
    directory.mkdir(parents=True, exist_ok=True)
    for part in (repo/'.build', directory):
        if part.is_symlink() or getattr(part.stat(), 'st_file_attributes', 0) & 0x400:
            p.error('State parent must not be a reparse point')
    module = reader(repo)
    if args.action == 'start':
        if not TASK.fullmatch(args.participant or '') or (args.batch and not HEX.fullmatch(args.batch)):
            p.error('Valid participant and optional batch IDs required')
        requested = paths(args.files)
        if args.condition == 'files' and not requested:
            p.error('File wait requires --files')
        if args.condition in ('peers', 'result') and (not args.batch or args.revision < 0):
            p.error('Peer/result wait requires saved --batch and --revision')
        key = hashlib.sha256(json.dumps([args.participant, args.condition, args.batch, args.revision, requested]).encode()).hexdigest()
        with lease(directory/('wait-' + key + '.lock')):
            pointer = module.read_json(directory, 'wait-' + key + '.json')
            if pointer and not args.new:
                prior = module.read_json(directory, pointer['id'] + '.wait.json')
                if prior:
                    print(json.dumps(diagnostic(module, directory, prior), ensure_ascii=False)); return 0
            if pointer and args.new:
                prior = module.read_json(directory, pointer['id'] + '.wait.json')
                if prior and diagnostic(module, directory, prior)['status'] == 'waiting':
                    raise ValueError('Existing wait is active. Observe/cancel it; do not create a duplicate worker.')
            ticket = {'schemaVersion': 1, 'id': secrets.token_hex(16), 'participant': args.participant,
                      'condition': args.condition, 'batchId': args.batch, 'editRevision': args.revision,
                      'files': requested, 'status': 'waiting', 'attempt': 1, 'createdUtc': utc(),
                      'deadlineUtc': (dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=args.timeout)).isoformat(),
                      'next': args.next[:2000], 'owner': None, 'observations': 0, 'reason': 'Local worker queued.'}
            atomic(directory/(ticket['id'] + '.wait.json'), ticket)
            atomic(directory/('wait-' + key + '.json'), {'id': ticket['id']})
            with lease(directory/(ticket['id'] + '.wait-worker.lock')):
                spawn(repo, directory, ticket)
            print(json.dumps(ticket, ensure_ascii=False)); return 0
    if not HEX.fullmatch(args.ticket):
        p.error('Use --ticket from watch start')
    if args.action == '_worker':
        worker(repo, directory, module, args.ticket); return 0
    ticket = module.read_json(directory, args.ticket + '.wait.json')
    if not ticket:
        p.error('Unknown wait ticket')
    shown = diagnostic(module, directory, ticket)
    if args.action == 'status':
        print(json.dumps(shown, ensure_ascii=False)); return 0
    if args.action == 'cancel':
        if shown['status'] == 'waiting':
            atomic(directory/(args.ticket + '.wait-cancel'), {'requestedUtc': utc()})
        print(json.dumps(shown, ensure_ascii=False)); return 0
    if not args.reason.strip():
        p.error('Explicit resume requires --reason after diagnosis')
    with lease(directory/(args.ticket + '.wait-worker.lock')):
        shown = diagnostic(module, directory, module.read_json(directory, args.ticket + '.wait.json'))
        if shown['status'] not in ('interrupted', 'timed-out', 'cancelled'):
            print(json.dumps(shown, ensure_ascii=False)); return 0
        receipt = directory/(args.ticket + '.wait-attempt' + str(shown['attempt']) + '.json')
        if not receipt.exists():
            atomic(receipt, shown)
        cancel = directory/(args.ticket + '.wait-cancel')
        if cancel.exists():
            cancel.unlink()
        ticket = dict(shown, status='waiting', attempt=shown['attempt'] + 1, owner=None,
                      deadlineUtc=(dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=args.timeout)).isoformat(),
                      reason=args.reason[:2000], observations=0)
        ticket.pop('completedUtc', None)
        atomic(directory/(args.ticket + '.wait.json'), ticket)
        spawn(repo, directory, ticket)
    print(json.dumps(ticket, ensure_ascii=False)); return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(2)
