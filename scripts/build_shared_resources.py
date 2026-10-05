"""Short, serialized language patches bound to a live collaboration edit revision."""
import argparse
import contextlib
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
from powershell_runtime import powershell_executable, run as run_process
import sys
import time

sys.dont_write_bytecode = True
from build_wait_tasks import atomic, lease, process_owner, reader, overlaps

HEX = re.compile(r'^[a-f0-9]{32}$')

def strict_json(raw):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ValueError('Duplicate JSON key: '+key)
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs)

def safe_file(root, relative):
    path = root / relative
    if root not in path.parents or not re.fullmatch(r'lang/[A-Za-z0-9-]+\.json',relative) or path.resolve() != path or not path.is_file():
        raise ValueError('Shared resource must be an existing regular repository file')
    for item in [path] + list(path.parents):
        if item == root.parent:
            break
        if getattr(item.stat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Shared resource does not follow reparse points')
    return path

def source(path):
    raw = path.read_bytes()
    text = raw.decode('utf-8-sig')
    values = strict_json(text)
    if not isinstance(values, dict) or any(not isinstance(value, str) for value in values.values()):
        raise ValueError('Shared language resources require flat string values; structural edits need an exclusive serial edit')
    return raw, text, values

def spans(text):
    """Locate flat object values without changing key order, whitespace or other entries."""
    decoder = json.JSONDecoder()
    index = 0
    def skip(value):
        while value < len(text) and text[value].isspace():
            value += 1
        return value
    index = skip(index)
    if text[index] != '{':
        raise ValueError('Expected language object')
    index = skip(index+1)
    result = {}
    while text[index] != '}':
        key, end = decoder.raw_decode(text, index)
        index = skip(end)
        if text[index] != ':':
            raise ValueError('Invalid language JSON')
        index = skip(index+1)
        _, end = decoder.raw_decode(text, index)
        result[key] = (index, end)
        index = skip(end)
        if text[index] == '}':
            break
        if text[index] != ',':
            raise ValueError('Invalid language JSON')
        index = skip(index+1)
    return result, index

def patch_bytes(raw, text, current, values):
    positions, closing = spans(text)
    updates = []
    additions = []
    for key, value in values.items():
        encoded = json.dumps(value, ensure_ascii=False)
        if key in current:
            start, end = positions[key]
            updates.append((start, end, encoded))
        else:
            additions.append(json.dumps(key, ensure_ascii=False)+': '+encoded)
    if additions:
        newline = '\r\n' if '\r\n' in text else '\n'
        indent = re.search(r'\n([ \t]*)"', text)
        indent = indent.group(1) if indent else '  '
        insertion = max(end for _, end in positions.values()) if positions else closing
        updates.append((insertion, insertion, (',' if positions else '')+newline+indent+(','+newline+indent).join(additions)))
    for start, end, value in sorted(updates, reverse=True):
        text = text[:start]+value+text[end:]
    strict_json(text)
    return (b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b'')+text.encode('utf-8')

def atomic_bytes(path, raw):
    temporary = path.with_name(path.name+'.'+secrets.token_hex(8)+'.tmp')
    try:
        with open(str(temporary), 'xb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        # No delete-before-replace or stale full-file rewrite on a transient error.
        os.replace(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()

def editor(root, args):
    state = strict_json((root/'.build/collaboration/state.json').read_text(encoding='utf-8-sig'))
    if Path(state['repositoryRoot']).resolve() != root:
        raise ValueError('Repository identity mismatch')
    batch = state.get('current')
    if not batch or batch['id'] != args.batch or batch['phase'] != 'editing' or batch.get('protocolVersion',1)<2:
        raise ValueError('Build input is frozen or batch retired; use begin for the next editing window')
    entries = [entry for entry in batch['participants'] if entry['id'] == args.participant]
    if len(entries) != 1 or entries[0]['state'] != 'editing' or entries[0].get('editRevision', 0) != args.revision:
        raise ValueError('Stale edit token or editor already ready; use the latest begin receipt')
    return state, batch, entries[0]

def refresh_ready(root, state, batch, actor, file):
    module = reader(root)
    for entry in batch['participants']:
        plan = entry.get('testPlan') or {}
        if entry['id']==actor or entry['state']!='finished' or not plan:
            continue
        if plan.get('inputs') and not any(overlaps(file,name) for name in plan['inputs']):
            continue
        check = entry.get('check') or {}
        if check.get('status') in ('running','failed','interrupted'):
            raise ValueError('A ready peer check is running/failed; wait for or diagnose it before this patch')
        entry['handoffPreviousCheck'] = dict(check)
        if not plan.get('handoffCarried'):
            plan['sourceBeforeHandoff'] = plan.get('source')
        # Existing ready workers already defer repair-carried checks until peers close.
        plan['source'] = 'repair-carried'
        plan['handoffCarried'] = True
        plan['inputIdentity'] = None
        check.update(status='pending',reason='A later authorized task updated a shared resource; recheck current inputs after editors close.')
        entry['check'] = check
        if module.owner_state(entry.get('waiter')) != 'alive':
            child=subprocess.Popen([powershell_executable(),'-NoProfile','-File',str(root/'scripts/build_waiter.ps1'),'-Participant',entry['id'],'-Batch',batch['id'],'-Revision',str(entry['editRevision'])],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
            entry['waiter']={**process_owner(child.pid),'editRevision':entry['editRevision']}
    atomic(root/'.build/collaboration/state.json',state)

def structural_guard(batch, actor, file, whitelist):
    for entry in batch['participants']:
        if entry['id']==actor or entry['state']!='editing':
            continue
        for owned in entry.get('ownedFiles',[]):
            if owned not in whitelist and overlaps(file,owned):
                raise ValueError('A structural/full-file edit occupies this resource: '+entry['id'])

@contextlib.contextmanager
def transaction(folder):
    deadline=time.monotonic()+5
    while True:
        stack=contextlib.ExitStack()
        try:
            stack.enter_context(lease(folder/'state.lock'))
            # A commit can wait for state.lock while holding git.lock: release both before queueing.
            stack.enter_context(lease(folder/'git.lock'))
        except BlockingIOError:
            stack.close()
            if time.monotonic()>=deadline:raise
            time.sleep(.05)
            continue
        try:
            yield
        finally:
            stack.close()
        return

def run(args):
    root = args.root.resolve()
    folder = root/'.build/collaboration'
    for parent in (root/'.build',folder):
        if parent.is_symlink() or getattr(parent.stat(),'st_file_attributes',0)&0x400:
            raise ValueError('State directory does not follow reparse points')
    if not 1 <= args.timeout <= 3600:
        raise ValueError('Resource timeout must be 1..3600 seconds')
    if args.command == 'status':
        if not HEX.fullmatch(args.request or ''):
            raise ValueError('Request ID required')
        return strict_json((folder/(args.request+'.resource.json')).read_text(encoding='utf-8-sig'))
    if not HEX.fullmatch(args.batch or '') or args.revision < 0 or not args.participant:
        raise ValueError('Participant, batch and live edit revision are required')
    with transaction(folder):
        state, batch, entry = editor(root, args)
        whitelist = strict_json((root/'scripts/shared_resources.json').read_text(encoding='utf-8-sig'))['languageFiles']
        if args.command == 'prepare':
            if args.file not in whitelist:
                raise ValueError('Only the explicit shared language file list is exempt')
            structural_guard(batch,args.participant,args.file,whitelist)
            keys = list(dict.fromkeys(args.keys.split(',')))
            if not keys or len(keys) > 200 or any(not key or len(key) > 256 for key in keys):
                raise ValueError('Declare 1..200 literal language keys')
            path = safe_file(root, args.file)
            raw, _, current = source(path)
            request = secrets.token_hex(16)
            expected = {key:{'exists':key in current,'value':current.get(key)} for key in keys}
            expires = (dt.datetime.now(dt.timezone.utc)+dt.timedelta(seconds=args.timeout)).isoformat()
            record = {'schemaVersion':1,'requestId':request,'participant':args.participant,'batchId':args.batch,'editRevision':args.revision,'file':args.file,'expected':expected,'expiresUtc':expires,'status':'prepared','preparedHash':hashlib.sha256(raw).hexdigest()}
            atomic(folder/(request+'.resource.json'), record)
            requests = folder/'requests'
            requests.mkdir(exist_ok=True)
            patch = requests/(request+'.json')
            atomic(patch, {key:current.get(key, '') for key in keys})
            return {**record,'patchFile':str(patch),'next':'Edit only the requested values in patchFile, then resource apply with this requestId. No file ownership transfer is needed.'}
        if not HEX.fullmatch(args.request or ''):
            raise ValueError('Request ID required')
        record_path = folder/(args.request+'.resource.json')
        record = strict_json(record_path.read_text(encoding='utf-8-sig'))
        if any(record[key] != value for key, value in [('participant',args.participant),('batchId',args.batch),('editRevision',args.revision)]):
            raise ValueError('Request belongs to a different editor/batch/revision')
        if dt.datetime.now(dt.timezone.utc) >= dt.datetime.fromisoformat(record['expiresUtc']):
            raise ValueError('Resource token expired; prepare again from current values')
        if record['file'] not in whitelist:
            raise ValueError('Resource no longer belongs to the explicit shared list')
        path = safe_file(root, record['file'])
        structural_guard(batch,args.participant,record['file'],whitelist)
        if record['status'] == 'applied':
            return record
        raw, text, current = source(path)
        if record['status'] == 'writing':
            if hashlib.sha256(raw).hexdigest() != record['afterHash']:
                raise ValueError('Interrupted write has uncertain outcome; inspect request status/file and prepare a fresh patch; no replay occurred')
            record.update(status='applied',reconciled=True)
            atomic(record_path,record)
            return record
        changes = strict_json((folder/'requests'/(args.request+'.json')).read_text(encoding='utf-8-sig'))
        if not isinstance(changes,dict) or set(changes) != set(record['expected']) or any(not isinstance(value,str) for value in changes.values()):
            raise ValueError('Patch must contain exactly the prepared keys with string values')
        for key, expected in record['expected'].items():
            if (key in current) != expected['exists'] or current.get(key) != expected['value']:
                raise ValueError('Same-key conflict: '+key+' changed since prepare; no resource was written')
        updated = patch_bytes(raw,text,current,changes)
        if updated != raw:
            refresh_ready(root,state,batch,args.participant,record['file'])
        record.update(status='writing',beforeHash=hashlib.sha256(raw).hexdigest(),afterHash=hashlib.sha256(updated).hexdigest())
        atomic(record_path,record)
        atomic_bytes(path,updated)
        record.update(status='applied',appliedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
        atomic(record_path,record)
        # Keep origin declarations for commits; patching itself grants no whole-file exclusivity.
        return record

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('entry',choices=['resource'])
    parser.add_argument('command',choices=['prepare','apply','status'])
    parser.add_argument('participant',nargs='?',default='')
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--batch',default='')
    parser.add_argument('--revision',type=int,default=-1)
    parser.add_argument('--file',default='')
    parser.add_argument('--keys',default='')
    parser.add_argument('--request',default='')
    parser.add_argument('--timeout',type=int,default=900)
    args=parser.parse_args()
    try:
        print(json.dumps(run(args),ensure_ascii=True))
    except (ValueError,OSError,KeyError,TypeError) as error:
        print(str(error),file=sys.stderr)
        return 2
    return 0

if __name__=='__main__':
    sys.exit(main())
