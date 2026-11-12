"""Save an exact set of owned language keys under the coordinator's Git lease."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from build_shared_resources import (
    COMMIT_PENDING, GIT_CHECKOUT_OVERRIDES, HEX, WRITE_PENDING, decode_source, desired_value, key_value, load_key_ownership,
    patch_bytes, safe_file, save_key_ownership, source, strict_json,
)
from build_wait_tasks import atomic, lease, utc


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def git(root, *args, index=None, data=None, codes=(0,)):
    env = os.environ.copy()
    if index is not None:
        env['GIT_INDEX_FILE'] = str(index)
    completed = subprocess.run(['git.exe', '--literal-pathspecs', *args], cwd=root, env=env,
                               input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode not in codes:
        detail = completed.stderr.decode('utf-8', 'replace').strip()
        raise ValueError('Git ' + args[0] + ' failed: ' + detail)
    return completed.returncode, completed.stdout


def output(root, *args, index=None, data=None):
    return git(root, *args, index=index, data=data)[1]


def git_path(root, name):
    return Path(output(root, 'rev-parse', '--path-format=absolute', '--git-path', name).decode('utf-8').strip())


def head(root):
    name = git(root, 'symbolic-ref', '--quiet', 'HEAD', codes=(0, 1))[1].decode('utf-8').strip()
    commit = output(root, 'rev-parse', '--verify', 'HEAD').decode('ascii').strip()
    return name or 'HEAD', commit


def index_entries(root, index):
    records = output(root, 'ls-files', '--stage', '-v', '-z', index=index).split(b'\0')
    return [tuple(record.split(b'\t', 1)) for record in records if record]


def add_keys_to_index(root, index, relative, keys, already_staged=False):
    entries = output(root, 'ls-files', '--stage', '-v', '-z', '--', relative, index=index).split(b'\0')
    if len(entries) != 2 or not entries[0].startswith(b'H ') or not entries[0].endswith(b'\t' + relative.encode('ascii')):
        raise ValueError('Language commit requires one tracked regular index entry: ' + relative)
    mode, oid, stage = entries[0][2:].split(b'\t', 1)[0].decode('ascii').split()
    if mode not in ('100644', '100755') or stage != '0':
        raise ValueError('Language commit does not use unmerged or special index entries: ' + relative)
    raw = output(root, 'cat-file', 'blob', oid)
    text, values = decode_source(raw)
    changes = {}
    for entry in keys:
        key = entry['key']
        before = key_value(values, key)
        if before == desired_value(entry['after']) and already_staged:
            continue  # The real index may already stage this exact value.
        if before != entry['before']:
            raise ValueError('Language key differs from its expected HEAD or staged value: ' + relative + ':' + key)
        changes[key] = entry['after']
    if changes:
        updated = patch_bytes(raw, text, changes)
        oid = output(root, 'hash-object', '-w', '--stdin', data=updated).decode('ascii').strip()
        output(root, 'update-index', '--cacheinfo', mode + ',' + oid + ',' + relative, index=index)


def check_worktree(root, groups):
    for relative, entries in groups.items():
        values = source(safe_file(root, relative))[2]
        for entry in entries:
            if key_value(values, entry['key']) != desired_value(entry['after']):
                raise ValueError('Owned key changed since apply: ' + relative + ':' + entry['key'])


def group_selected(root, folder, participant, request_ids):
    whitelist = strict_json((root / 'scripts/shared_resources.json').read_bytes())['languageFiles']
    receipts = {}
    for request in request_ids:
        record = strict_json((folder / (request + '.resource.json')).read_text(encoding='utf-8-sig'))
        if (record['requestId'] != request or record['participant'] != participant or
                record['file'] not in whitelist or record['status'] != 'applied' or record.get('keyOwnershipVersion') != 1):
            raise ValueError('Request is not an applied key ownership receipt for this task: ' + request)
        receipts[request] = record
    groups = {}
    for entry in load_key_ownership(folder):
        if entry['requestId'] not in request_ids:
            continue
        origin = receipts[entry['requestId']]
        if (any(entry[name] != origin[name] for name in ('participant', 'batchId', 'editRevision', 'file')) or
                entry['key'] not in origin['expected']):
            raise ValueError('Language receipt does not match current key ownership')
        groups.setdefault(entry['file'], []).append(entry)
    selected = {entry['requestId'] for entries in groups.values() for entry in entries}
    for request in request_ids:
        if request not in selected:
            raise ValueError('No pending keys for request ' + request + '; use the latest applied receipt for each key')
    return groups


def journal_paths(folder, index, transaction):
    if not HEX.fullmatch(transaction):
        raise ValueError('Invalid language commit recovery journal')
    return (folder / COMMIT_PENDING, index.with_name(index.name + '.lock'),
            index.with_name(index.name + '.snowdesktop-resource-' + transaction))


def owns_lock(index_lock, prepared):
    return index_lock.exists() and prepared.exists() and os.path.samefile(index_lock, prepared)


def discard_prepared(pending, index_lock, prepared):
    install = prepared.with_name(prepared.name + '.install')
    if install.exists():
        if not prepared.exists() or not os.path.samefile(install, prepared):
            raise ValueError('Unknown prepared index installation file; preserve the language commit journal')
        install.unlink()
    if owns_lock(index_lock, prepared):
        index_lock.unlink()
    if prepared.exists():
        prepared.unlink()
    pending.unlink()


def recover(root, folder, index, result_path, intent):
    receipt = intent['receipt']
    pending, index_lock, prepared = journal_paths(folder, index, intent['transaction'])
    if result_path.exists():
        recorded = strict_json(result_path.read_bytes())
        if (recorded != receipt or load_key_ownership(folder) != intent['ownersAfter'] or
                git(root, 'merge-base', '--is-ancestor', receipt['commit'], 'HEAD', codes=(0, 1))[0] != 0):
            raise ValueError('Completed language commit or ownership differs from its journal; preserve it for diagnosis')
        # The receipt is already durable. Git may have legitimately updated the
        # real index if the previous process removed its lock before exiting.
        discard_prepared(pending, index_lock, prepared)
        return receipt
    if not intent.get('refPublicationReady'):
        # This intent never gave a child permission to publish the ref. A new
        # operation must revalidate the current registration and Git inputs.
        discard_prepared(pending, index_lock, prepared)
        return None
    ref, current_head = head(root)
    current_index = sha256(index.read_bytes())
    if ref != intent['ref']:
        raise ValueError('HEAD now refers to a different branch; preserve the language commit journal and index lock')
    if current_head == receipt['parent'] and current_index == intent['indexBefore']:
        if not owns_lock(index_lock, prepared):
            raise ValueError('Prepared native index lock is missing; preserve the language commit journal')
        # An orphaned update-ref could still run. Finish the same exact CAS,
        # keeping the journal intact if that process or another ref writer wins.
        owners = load_key_ownership(folder)
        if owners != intent['ownersBefore'] or sha256(prepared.read_bytes()) != intent['indexAfter']:
            raise ValueError('Prepared language ownership or index changed; preserve the journal and lock')
        keys = {(item['file'], item['key'], item['requestId']) for item in receipt['resourceKeys']}
        groups = {}
        for entry in owners:
            if (entry['file'], entry['key'], entry['requestId']) in keys:
                groups.setdefault(entry['file'], []).append(entry)
        check_worktree(root, groups)
        publish_ref(root, pending, intent)
        ref, current_head = head(root)
        current_index = sha256(index.read_bytes())
        if ref != intent['ref']:
            raise ValueError('HEAD now refers to a different branch; preserve the language commit journal and index lock')
    if current_head != receipt['commit'] or current_index not in (intent['indexBefore'], intent['indexAfter']):
        raise ValueError('HEAD or index changed during language commit recovery; preserve the journal and lock')
    if current_index == intent['indexBefore'] and current_index != intent['indexAfter']:
        if not owns_lock(index_lock, prepared) or sha256(prepared.read_bytes()) != intent['indexAfter']:
            raise ValueError('Prepared language index lock is missing or changed; preserve the journal')
        install = prepared.with_name(prepared.name + '.install')
        if not install.exists():
            os.link(str(prepared), str(install))
        elif not os.path.samefile(install, prepared):
            raise ValueError('Unknown prepared index installation file; preserve the language commit journal')
        os.replace(str(install), str(index))
    owners = load_key_ownership(folder)
    if owners not in (intent['ownersBefore'], intent['ownersAfter']):
        raise ValueError('Language key ownership changed during commit recovery; preserve the journal')
    if owners != intent['ownersAfter']:
        save_key_ownership(folder, intent['ownersAfter'])
    atomic(result_path, receipt)
    discard_prepared(pending, index_lock, prepared)
    return receipt


def publish_ref(root, pending, intent):
    if not intent.get('refPublicationReady'):
        intent['refPublicationReady'] = True
        atomic(pending, intent)
    receipt = intent['receipt']
    output(root, 'update-ref', '-m', 'commit: SnowDesktop language keys',
           intent['ref'], receipt['commit'], receipt['parent'])


def assert_registration(root, folder, args, request_ids):
    if not HEX.fullmatch(args.batch) or args.revision < 0:
        raise ValueError('A registered batch and edit revision are required')
    with lease(folder / 'state.lock'):
        state = strict_json((folder / 'state.json').read_text(encoding='utf-8-sig'))
        if state['schemaVersion'] != 1 or Path(state['repositoryRoot']).resolve() != root:
            raise ValueError('Repository identity differs from the collaboration state')
        current = state.get('current') or {}
        batch = current if current.get('id') == args.batch else next(
            (candidate for candidate in state.get('activeCandidates', []) if candidate['id'] == args.batch), None)
        if batch is None:
            batch = strict_json((folder / (args.batch + '.json')).read_text(encoding='utf-8-sig'))
            if batch['batchId'] != args.batch or Path(batch['repositoryRoot']).resolve() != root:
                raise ValueError('Invalid saved batch result for language commit')
        members = [member for member in batch['participants'] if member['id'] == args.participant]
        if len(members) != 1 or members[0].get('editRevision', 0) != args.revision:
            raise ValueError('Language commit requires the current registered edit revision')
        member = members[0]
        if batch is current and current['phase'] == 'editing' and member['state'] == 'editing':
            return
        if member['state'] != 'finished' or not set(request_ids).issubset(member.get('resourceCommitCheckpoint', [])):
            raise ValueError('Resource requests are outside the ready checkpoint for this edit revision')


def commit(args):
    root = args.root.resolve()
    if any(name in os.environ for name in GIT_CHECKOUT_OVERRIDES):
        raise ValueError('Language commit requires the registered checkout and its ordinary Git index')
    if Path(output(root, 'rev-parse', '--show-toplevel').decode('utf-8').strip()).resolve() != root:
        raise ValueError('Git checkout root differs from language resource ownership')
    folder = root / '.build/collaboration'
    if not folder.is_dir():
        raise ValueError('No collaboration state for this Git checkout')
    for path in (root / '.build', folder):
        if path.is_symlink() or getattr(path.stat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('State directory does not follow reparse points')
    request_ids = sorted(set(args.requests.split(',')))
    if len(request_ids) > 128 or any(not HEX.fullmatch(request) for request in request_ids):
        raise ValueError('Provide 1..128 applied resource request IDs')
    # The process that actually modifies Git owns the lease. Caller termination
    # cannot let a second writer into an unfinished index/ref transaction.
    with lease(folder / 'git.lock'):
        return commit_locked(root, folder, args, request_ids)


def commit_locked(root, folder, args, request_ids):
    if (folder / WRITE_PENDING).exists():
        raise ValueError('A language write needs recovery; retry resource apply before committing')
    operation = sha256(json.dumps([args.participant, args.batch, args.revision, request_ids]).encode('utf-8'))
    result_path = folder / ('resource-commit-' + operation + '.json')
    index = git_path(root, 'index')
    pending = folder / COMMIT_PENDING
    if pending.exists():
        intent = strict_json(pending.read_bytes())
        receipt = intent['receipt']
        if (intent['operation'] != operation or receipt['participant'] != args.participant or
                receipt['batchId'] != args.batch or receipt['editRevision'] != args.revision or
                receipt['resourceRequests'] != request_ids):
            raise ValueError('A different language commit is interrupted; retry its original command before proceeding')
        recovered = recover(root, folder, index, result_path, intent)
        if recovered:
            return recovered
    assert_registration(root, folder, args, request_ids)
    if result_path.exists():
        receipt = strict_json(result_path.read_bytes())
        if git(root, 'merge-base', '--is-ancestor', receipt['commit'], 'HEAD', codes=(0, 1))[0] != 0:
            raise ValueError('Previously saved language commit is no longer in HEAD history')
        return receipt
    for name in ('MERGE_HEAD', 'CHERRY_PICK_HEAD', 'REVERT_HEAD', 'rebase-apply', 'rebase-merge', 'sequencer'):
        if git_path(root, name).exists():
            raise ValueError('Finish or resolve the in-progress Git operation before committing language keys')
    for hook in ('pre-commit', 'prepare-commit-msg', 'commit-msg', 'post-commit'):
        hook_path = git_path(root, 'hooks/' + hook)
        if hook_path.is_file() and os.access(hook_path, os.X_OK):
            raise ValueError('Language key commit cannot safely run the configured Git hook: ' + hook)
    groups = group_selected(root, folder, args.participant, request_ids)
    check_worktree(root, groups)
    ref, parent = head(root)
    index_before = index.read_bytes()
    transaction = secrets.token_hex(16)
    _, index_lock, prepared = journal_paths(folder, index, transaction)
    if index_lock.exists():
        raise ValueError('Git index is locked by another operation; retry after it releases the lock')
    with tempfile.TemporaryDirectory(dir=folder, prefix='resource-commit-') as workspace:
        workspace = Path(workspace)
        candidate = workspace / 'candidate.index'
        staged = workspace / 'staged.index'
        staged.write_bytes(index_before)
        output(root, 'read-tree', parent, index=candidate)
        other_paths = {path.encode('ascii') for path in groups}
        other_before = [entry for entry in index_entries(root, staged) if entry[1] not in other_paths]
        for relative, keys in groups.items():
            add_keys_to_index(root, candidate, relative, keys)
            add_keys_to_index(root, staged, relative, keys, already_staged=True)
        other_after = [entry for entry in index_entries(root, staged) if entry[1] not in other_paths]
        if other_before != other_after:
            raise ValueError('Unrelated staged entries would change; language commit refused')
        output(root, 'diff', '--cached', '--check', parent, index=candidate)
        tree = output(root, 'write-tree', index=candidate).decode('ascii').strip()
        signing = git(root, 'config', '--type=bool', '--get', 'commit.gpgsign', codes=(0, 1))[1].strip() == b'true'
        sign_arg = ['-S'] if signing else []
        new_commit = output(root, 'commit-tree', tree, '-p', parent, '-F', str(args.message), *sign_arg).decode('ascii').strip()
        all_owners = load_key_ownership(folder)
        committed = [entry for keys in groups.values() for entry in keys]
        receipt = {'parent': parent, 'commit': new_commit, 'paths': sorted(groups), 'utc': utc(),
                   'batchId': args.batch, 'participant': args.participant, 'editRevision': args.revision,
                   'resourceRequests': request_ids, 'resourceKeys': [
                       {name: entry[name] for name in ('file', 'key', 'requestId')} for entry in committed],
                   'otherIndexEntriesUnchanged': True, 'workingFilesUnchanged': True,
                   'limitation': 'Language commit uses commit-tree with configured signing and refuses configured commit hooks.'}
        new_index = staged.read_bytes()
        with prepared.open('xb') as stream:
            stream.write(new_index)
            stream.flush()
            os.fsync(stream.fileno())
        intent = {'transaction': transaction, 'operation': operation, 'ref': ref, 'receipt': receipt,
                  'indexBefore': sha256(index_before), 'indexAfter': sha256(new_index),
                  'ownersBefore': all_owners, 'ownersAfter': [entry for entry in all_owners if entry not in committed]}
        try:
            atomic(pending, intent)
        except BaseException:
            prepared.unlink()
            raise
        try:
            # Git honors an existing index.lock. A hardlink makes ownership
            # verifiable on restart even if interrupted just after acquisition.
            os.link(str(prepared), str(index_lock))
            if head(root) != (ref, parent) or index.read_bytes() != index_before:
                raise ValueError('HEAD or staged index changed while preparing language commit')
            check_worktree(root, groups)
        except (ValueError, OSError):
            # We haven't launched update-ref, so known prepublication failures
            # may clean our prepared files without risking an orphaned writer.
            discard_prepared(pending, index_lock, prepared)
            raise
        publish_ref(root, pending, intent)
        return recover(root, folder, index, result_path, intent)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--participant', required=True)
    parser.add_argument('--batch', required=True)
    parser.add_argument('--revision', required=True, type=int)
    parser.add_argument('--requests', required=True)
    parser.add_argument('--message', required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(commit(args), ensure_ascii=True))
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
