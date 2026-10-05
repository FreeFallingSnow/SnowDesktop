"""One planned test run, then bounded retries of explicitly classified isolated failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
from build_wait_tasks import atomic, lease, process_owner, reader, utc

# Reviewed tests only. A label by itself cannot opt an arbitrary test into retries.
# Only pre-assertion ephemeral port acquisition failure, after cleanup, is retryable.
POLICY = {'build_dashboard': 60, 'build_dashboard_browser': 180}
LABEL = 'retry-isolated-resource'
FAILURE_CLASS = 'isolated-port-race'

def digest(path):
    sha = hashlib.sha256()
    with open(str(path), 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(chunk)
    return sha.hexdigest()

def emit_resource_signal(test, reason):
    """Called only after a reviewed fixture has released all its own resources."""
    directory = os.environ.get('SNOWDESKTOP_RETRY_SIGNAL_DIR')
    token = os.environ.get('SNOWDESKTOP_RETRY_RUN_TOKEN')
    if test not in POLICY or not directory or not re.fullmatch(r'[a-f0-9]{32}', token or ''):
        return False
    root = Path(directory)
    if not root.is_dir() or root.is_symlink() or getattr(root.stat(), 'st_file_attributes', 0) & 0x400:
        return False
    atomic(root/(test + '.json'), {'schemaVersion': 1, 'test': test, 'runToken': token,
                                 'exitCode': 75, 'failureClass': FAILURE_CLASS,
                                 'cleanupComplete': True, 'sideEffects': 'none', 'reason': str(reason)[:2000]})
    return True

def cases(path):
    if not path or not Path(path).is_file():
        return {}
    result = {}
    for case in ET.parse(str(path)).getroot().iter('testcase'):
        name = case.get('name')
        if name in result:
            raise ValueError('Duplicate JUnit test identity')
        failure = case.find('failure')
        error = case.find('error')
        status = ('failed' if failure is not None or error is not None else
                  'not-run' if case.find('skipped') is not None or case.get('status') in ('notrun', 'disabled') else 'passed')
        reason = ((failure.text or failure.get('message', '')) if failure is not None else
                  (error.text or error.get('message', '')) if error is not None else status)
        result[name] = {'name': name, 'status': status, 'reason': reason[:4000], 'seconds': case.get('time')}
    return result

def labels(test):
    return next((x.get('value', []) for x in test.get('properties', []) if x['name'] == 'LABELS'), [])

def classify(name, metadata, signals, token):
    signal_path = signals/(name + '.json')
    if name not in POLICY or LABEL not in labels(metadata) or not signal_path.is_file():
        return None
    with open(str(signal_path), 'rb') as stream:
        signal = json.loads(stream.read(8192))
    if (signal.get('test') == name and signal.get('runToken') == token and signal.get('exitCode') == 75 and
            signal.get('failureClass') == FAILURE_CLASS and signal.get('cleanupComplete') is True and
            signal.get('sideEffects') == 'none'):
        return signal
    return None

def source_identity(repo, work):
    # Reuse the coordinator's exact source/environment algorithm, never HEAD alone.
    script = work/'source-identity.ps1'
    if not script.exists():
        script.write_bytes(b"$ErrorActionPreference='Stop'\n$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))\n. (Join-Path $root 'scripts/build_inputs.ps1')\nGet-BuildInputIdentity $root | ConvertTo-Json\n")
    ps = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')
    run = subprocess.run([ps, '-NoProfile', '-File', str(script)], cwd=str(repo), capture_output=True, timeout=30)
    if run.returncode:
        raise ValueError('Cannot verify source identity; retries are blocked')
    return json.loads(run.stdout.decode('utf-8-sig'))['digest']

def export_coverage(path, coverage, ledger):
    completed = []
    for name in coverage.get('selected', []):
        item = ledger['tests'].get(name, {'status': 'not-run', 'finalReason': 'No matching execution evidence'})
        completed.append({'name': name, 'status': item['status'], 'reason': item.get('finalReason'),
                          'failureCount': item.get('failureCount', 0), 'attemptCount': len(item.get('attempts', []))})
    coverage['completed'] = completed
    coverage['initialReport'] = ledger.get('initialReport')
    coverage['retryLedger'] = ledger['batchId'] + '.retry.json'
    coverage['flaky'] = [x['name'] for x in completed if x['status'] == 'passed-after-retry']
    coverage['status'] = ledger['status']
    coverage['error'] = ledger.get('reason', '')
    if 'postChecks' in ledger:
        coverage['postChecks'] = ledger['postChecks']
    for task in coverage.get('tasks', []):
        if task.get('status') == 'not-required':
            continue
        actual = [x for x in completed if x['name'] in task.get('requested', [])]
        task['failed'] = [x['name'] for x in actual if x['status'] not in ('passed', 'passed-after-retry')]
        task['status'] = ('invalidated' if ledger['status']=='invalidated' else 'failed' if task['failed'] else 'not-run' if len(actual) != len(task['requested']) else
                          'passed-after-retry' if any(x['status'] == 'passed-after-retry' for x in actual) else 'passed')
        check = coverage.get('postChecks', {}).get('outputIsolation', {})
        if task.get('requiresOutputIsolation', coverage.get('mode') == 'full') and task['status'] in ('passed', 'passed-after-retry') and check.get('status') != 'passed':
            task['status'] = 'failed' if check.get('status') == 'failed' else 'not-run'
    atomic(path, coverage)

def retry_failed(ledger, coverage, metadata, baseline, identity, stable_artifacts, execute, work,
                 max_attempts=3, budget=300, backoffs=(1, 3)):
    """Inject only process/time/input boundaries in fixtures; policy and scheduling stay real."""
    deadline = time.monotonic() + budget
    record = work.parent/(ledger['batchId'] + '.retry.json')
    for name in coverage['selected']:
        item = ledger['tests'][name]
        if item['status'] != 'failed':
            continue
        signal = classify(name, metadata.get(name, {}), work/'signals-1', ledger['runToken'])
        if not signal:
            item['retryDecision'] = 'not-eligible: no reviewed metadata and cleanup-complete structured failure'
            atomic(record, ledger)
            continue
        item['failureClass'] = signal['failureClass']
        item['firstReason'] = signal['reason']
        for attempt in range(2, max_attempts + 1):
            if identity() != baseline or not stable_artifacts():
                item.update(status='invalidated', finalReason='Source/tool/test artifact identity changed; no retry executed.')
                ledger.update(status='invalidated', reason=item['finalReason'])
                atomic(record, ledger)
                return 5
            delay = backoffs[min(attempt - 2, len(backoffs) - 1)]
            if time.monotonic() + delay + 1 >= deadline:
                item.update(status='environment-blocked', finalReason='Automatic retry time budget exhausted.')
                break
            item.update(status='retrying', finalReason='Cleanup was reported; bounded backoff before retry.')
            ledger['status'] = 'retrying'
            atomic(record, ledger)
            time.sleep(delay)
            if identity() != baseline or not stable_artifacts():
                item.update(status='invalidated', finalReason='Input changed during retry backoff.')
                ledger.update(status='invalidated', reason=item['finalReason'])
                atomic(record, ledger)
                return 5
            remaining = max(1, int(deadline - time.monotonic()))
            timeout = min(POLICY[name], remaining)
            if timeout < POLICY[name]:
                item.update(status='environment-blocked', finalReason='Remaining budget cannot cover the declared test timeout.')
                break
            signals = work/('signals-' + str(attempt))
            signals.mkdir(exist_ok=True)
            report = work/(name + '.attempt' + str(attempt) + '.xml')
            log = work/(name + '.attempt' + str(attempt) + '.log')
            pending = {'attempt': attempt, 'status': 'running', 'sourceStartDigest': baseline, 'startedUtc': utc(), 'report': str(report), 'log': str(log)}
            item['attempts'].append(pending)
            atomic(record, ledger)  # A crash leaves uncertainty; this batch is never blindly replayed.
            code = execute(name, report, log, signals, timeout)
            actual = cases(report)
            status = actual.get(name, {}).get('status', 'not-run') if set(actual) == {name} else 'environment-blocked'
            reason = actual.get(name, {}).get('reason', 'No exact single-test JUnit evidence')
            observed = identity()
            artifacts_stable = stable_artifacts()
            pending.update(sourceEndDigest=observed, artifactsStable=artifacts_stable)
            if observed != baseline or not artifacts_stable:
                status, reason = 'invalidated', 'Source/tool/test artifact changed during retry.'
            if code != 0 and status == 'passed':
                status, reason = 'environment-blocked', 'CTest failed despite a passing test report.'
            pending.update(status=status, exitCode=code, completedUtc=utc(), reason=reason)
            # Independent immutable attempt evidence is durable before the mutable display summary.
            atomic(work/(name + '.attempt' + str(attempt) + '.json'), pending)
            item['failureCount'] += int(status == 'failed')
            item.update(status='passed-after-retry' if status == 'passed' else status, finalReason=reason)
            atomic(record, ledger)
            if status == 'invalidated':
                ledger.update(status='invalidated', reason=reason)
                atomic(record, ledger)
                return 5
            if status != 'failed' or not classify(name, metadata.get(name, {}), signals, ledger['runToken']):
                break
        if item['status'] == 'failed':
            item['status'] = 'exhausted-failed' if len(item['attempts']) == max_attempts else 'failed'
        atomic(record, ledger)
    all_status = [x['status'] for x in ledger['tests'].values()]
    ledger['status'] = ('passed-after-retry' if all(x in ('passed', 'passed-after-retry') for x in all_status) and
                        'passed-after-retry' in all_status else 'passed' if all(x == 'passed' for x in all_status) and all_status else 'failed')
    ledger['reason'] = 'Finite per-test retries preserve first failures; selected inventory is unchanged.'
    atomic(record, ledger)
    return 0 if ledger['status'] in ('passed', 'passed-after-retry') else 1

def complete_postchecks(ledger, coverage, result, baseline, identity, stable_artifacts, execute):
    """Resume only an unexecuted full-run checkpoint; never retry a failed one."""
    if coverage.get('mode') != 'full':
        return result
    check = dict(coverage.get('postChecks', {}).get('outputIsolation') or
                 {'status': 'not-run', 'startedUtc': None, 'completedUtc': None, 'exitCode': None, 'error': ''})
    ledger['postChecks'] = dict(coverage.get('postChecks', {}), outputIsolation=check)
    if check['status'] == 'failed':
        ledger.update(status='failed', reason=check.get('error') or 'Full-test output isolation failed.')
        return check.get('exitCode') or 1
    if result:
        return result
    if check['status'] not in ('passed', 'not-run'):
        ledger.update(status='environment-blocked', reason='Unknown full-test checkpoint state; no replay authorized.')
        return 1
    if identity() != baseline or not stable_artifacts():
        ledger.update(status='invalidated', reason='Inputs changed before the full-test checkpoint.')
        return 5
    if check['status'] == 'not-run':
        check = execute()
        ledger['postChecks']['outputIsolation'] = check
    observed = identity()
    stable = stable_artifacts()
    check.update(sourceStartDigest=baseline, sourceEndDigest=observed, artifactsStable=stable)
    if observed != baseline or not stable:
        ledger.update(status='invalidated', reason='Inputs changed during the full-test checkpoint.')
        return 5
    if check.get('status') != 'passed' or check.get('exitCode') != 0:
        ledger.update(status='failed' if check.get('status') == 'failed' else 'environment-blocked',
                      reason=check.get('error') or 'Full-test output isolation was not verified.')
        return check.get('exitCode') or 1
    return 0

def run_output_checkpoint(repo, work, ps):
    receipt, log, script = (work/'output-isolation.json', work/'output-isolation.log', work/'output-isolation.ps1')
    # Fixed private script, parameterized paths, no shell interpolation. This
    # child inherits the surrounding Job and cannot configure/build/run CTest.
    script.write_bytes(b"param([string]$Root,[string]$Receipt)\n$ErrorActionPreference='Stop'\n"
                       b". (Join-Path $Root 'scripts/test_output.ps1')\n$check=New-OutputIsolationCheck\n"
                       b"try {Invoke-OutputIsolationCheck $Root $check} catch {[Console]::Error.WriteLine($_.Exception.Message)}\n"
                       b"$check | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Receipt -Encoding UTF8\n"
                       b"exit $check.exitCode\n")
    pending = {'status': 'not-run', 'startedUtc': utc(), 'completedUtc': None, 'exitCode': None,
               'error': 'Checkpoint has not returned execution evidence.', 'log': str(log)}
    atomic(work/'output-isolation.pending.json', pending)
    with open(str(log), 'xb') as stream:
        try:
            run = subprocess.run([ps, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                                  '-Root', str(repo), '-Receipt', str(receipt)], cwd=str(repo),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=30)
        except subprocess.TimeoutExpired:
            return dict(pending, completedUtc=utc(), exitCode=1, error='Output isolation checkpoint timed out before completed evidence.')
    if not receipt.is_file():
        return dict(pending, completedUtc=utc(), exitCode=run.returncode or 1, error='Output isolation checkpoint produced no receipt.')
    check = json.loads(receipt.read_text(encoding='utf-8-sig'))
    if check.get('status') not in ('passed', 'failed') or check.get('exitCode') != run.returncode:
        return dict(pending, completedUtc=utc(), exitCode=run.returncode or 1, error='Output isolation checkpoint receipt/exit mismatch.')
    return dict(check, log=str(log), receipt=str(receipt))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--batch', required=True)
    p.add_argument('--status', action='store_true')
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = p.parse_args()
    if not re.fullmatch(r'[a-f0-9]{32}', args.batch):
        p.error('A frozen batch ID is required')
    repo = args.root.resolve()
    directory = repo/'.build/collaboration'
    for parent in (repo/'.build', directory):
        if parent.is_symlink() or getattr(parent.stat(), 'st_file_attributes', 0) & 0x400:
            p.error('State directory must not be a reparse point')
    module = reader(repo)
    path = directory/(args.batch + '.retry.json')
    previous = module.read_json(directory, path.name)
    if args.status:
        if previous and previous.get('status') in ('running', 'retrying') and module.owner_state(previous.get('owner')) == 'exited':
            previous = dict(previous, status='interrupted', reason='Runner exited. Preserve completed attempt files and repair in a new attempt; no replay authorized.')
        print(json.dumps(previous, ensure_ascii=False)); return 0
    with lease(directory/(args.batch + '.retry.lock')):
        previous = module.read_json(directory, path.name)
        if previous:
            print('This execution already has retry evidence; no test/build was replayed. Inspect --status; use repair for new inputs.', file=sys.stderr)
            return previous.get('exitCode', 4)
        work = directory/(args.batch + '.retry')
        work.mkdir(exist_ok=True)
        if work.is_symlink() or getattr(work.stat(), 'st_file_attributes', 0) & 0x400:
            p.error('Retry evidence directory must not be a reparse point')
        token = secrets.token_hex(16)
        signals = work/'signals-1'
        signals.mkdir()
        baseline = source_identity(repo, work)
        tools = [Path(sys.executable), Path(shutil.which('ctest'))]
        edge = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)'))/'Microsoft/Edge/Application/msedge.exe'
        if edge.is_file():
            tools.append(edge)
        tool_hashes = {str(x): digest(x) for x in tools}
        ledger = {'schemaVersion': 1, 'batchId': args.batch, 'status': 'running', 'runToken': token,
                  'owner': process_owner(os.getpid()), 'startedUtc': utc(), 'selected': [], 'tests': {},
                  'sourceDigest': baseline, 'toolHashes': tool_hashes,
                  'policy': {'maxAttempts': 3, 'retryBudgetSeconds': 300, 'backoffSeconds': [1, 3],
                             'eligibleTests': sorted(POLICY), 'failureClass': FAILURE_CLASS}}
        atomic(path, ledger)
        ps = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')
        env = dict(os.environ, SNOWDESKTOP_RETRY_SIGNAL_DIR=str(signals), SNOWDESKTOP_RETRY_RUN_TOKEN=token)
        code = subprocess.call([ps, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                str(repo/'scripts/test_manager.ps1'), '-Mode', 'plan', '-PlanBatch', args.batch], cwd=str(repo), env=env)
        coverage_path = directory/(args.batch + '.coverage.json')
        coverage = module.read_json(directory, coverage_path.name) or {}
        ledger['initialExitCode'] = code
        ledger['initialReport'] = coverage.get('report')
        actual = cases(coverage.get('report'))
        ledger['selected'] = coverage.get('selected', [])
        for name in ledger['selected']:
            observed = actual.get(name, {'status': 'not-run', 'reason': 'No execution evidence'})
            ledger['tests'][name] = {'name': name, 'status': observed['status'], 'firstReason': observed['reason'],
                                    'finalReason': observed['reason'], 'failureCount': int(observed['status'] == 'failed'),
                                    'attempts': [{'attempt': 1, 'status': observed['status'], 'report': ledger['initialReport'], 'exitCode': code}]}
        if not ledger['selected'] or set(actual) != set(ledger['selected']):
            ledger.update(status='environment-blocked', reason='Configure/build/report/inventory failure; no automatic retry.', exitCode=code or 1, completedUtc=utc())
            atomic(path, ledger); return ledger['exitCode']
        artifacts = coverage.get('testExecutablesBeforeRun', [])
        ledger['testExecutables'] = artifacts
        def stable_artifacts():
            return (all(Path(x).is_file() and digest(Path(x)) == sha for x, sha in tool_hashes.items()) and
                    all((repo/x['executable']).is_file() and digest(repo/x['executable']) == x['sha256'] for x in artifacts))
        identity = lambda: source_identity(repo, work)
        if identity() != baseline or not stable_artifacts():
            ledger.update(status='invalidated', reason='Input identity changed; no automatic retry.', exitCode=5, completedUtc=utc())
            atomic(path, ledger); export_coverage(coverage_path, coverage, ledger); return 5
        inventory = subprocess.run(['ctest', '--preset', 'all-tests', '--show-only=json-v1'], cwd=str(repo), capture_output=True)
        if inventory.returncode:
            ledger.update(status='environment-blocked', reason='Cannot validate retry metadata.', exitCode=1, completedUtc=utc())
            atomic(path, ledger); return 1
        metadata = {x['name']: x for x in json.loads(inventory.stdout)['tests']}
        def execute(name, report, log, attempt_signals, timeout):
            print('Retrying only ' + name + '; immutable evidence: ' + str(log), flush=True)
            retry_env = dict(os.environ, SNOWDESKTOP_RETRY_SIGNAL_DIR=str(attempt_signals), SNOWDESKTOP_RETRY_RUN_TOKEN=token)
            with open(str(log), 'xb') as stream:
                return subprocess.call(['ctest', '--preset', 'all-tests', '-R', '^' + re.escape(name) + '$',
                                        '--output-junit', str(report), '--timeout', str(timeout)], cwd=str(repo), env=retry_env,
                                       stdout=stream, stderr=subprocess.STDOUT)
        result = retry_failed(ledger, coverage, metadata, baseline, identity, stable_artifacts, execute, work)
        check = coverage.get('postChecks', {}).get('outputIsolation', {})
        if code and result == 0 and not any(x['status'] == 'passed-after-retry' for x in ledger['tests'].values()) and check.get('status') != 'failed':
            ledger.update(status='environment-blocked', reason='Initial CTest pipeline failed without a retryable failing case.')
            result = 1
        result = complete_postchecks(ledger, coverage, result, baseline, identity, stable_artifacts,
                                     lambda: run_output_checkpoint(repo, work, ps))
        ledger.update(exitCode=result, completedUtc=utc())
        atomic(path, ledger)
        export_coverage(coverage_path, coverage, ledger)
        return result

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, ET.ParseError) as error:
        print('Retry runner stopped safely: ' + str(error), file=sys.stderr)
        sys.exit(2)
