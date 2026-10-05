"""PowerShell 7 preference, 5.1 fallback and startup-only recovery.

The bootstrap waits for an explicit permit before invoking any target. Only
initialization failures are retried; script failures and timeouts are returned
unchanged. The failed initialization and its output remain available as evidence.
"""
import base64
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid

PIN = 'SNOWDESKTOP_ENTRY_POWERSHELL'


def powershell_candidates(env=None):
    env = os.environ if env is None else env
    env = {key.upper(): value for key, value in env.items()}
    fallback = str(Path(env.get('SYSTEMROOT', env.get('WINDIR', r'C:\Windows'))) /
                   'System32/WindowsPowerShell/v1.0/powershell.exe')
    paths = [env.get(PIN)]
    for variable in ('PROGRAMFILES', 'PROGRAMW6432'):
        if env.get(variable):
            paths.append(str(Path(env[variable]) / 'PowerShell/7/pwsh.exe'))
    paths += [shutil.which('pwsh.exe', path=env.get('PATH')), fallback]
    existing = []
    for path in paths:
        if path and Path(path).is_absolute() and Path(path).is_file() and path not in existing:
            existing.append(path)
    if not existing:
        raise FileNotFoundError('Neither PowerShell 7 nor Windows PowerShell 5.1 is available')
    # Recovery uses 5.1 when available, even if 7 failed during initialization.
    return [existing[0], fallback if Path(fallback).is_file() else existing[0]]


def powershell_executable():
    selected = powershell_candidates()[0]
    os.environ[PIN] = selected
    os.environ['PSExecutionPolicyPreference'] = 'Bypass'
    return selected


def _literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def _target(arguments):
    lowered = [str(x).lower() for x in arguments]
    if '-file' in lowered:
        index = lowered.index('-file')
        script = arguments[index + 1]
        params = []
        for value in arguments[index + 2:]:
            value = str(value)
            params.append(value if re.fullmatch(r'-[A-Za-z][A-Za-z0-9]*(?::\$(?:true|false))?', value, re.I) else _literal(value))
        return '& ' + _literal(script) + ' ' + ' '.join(params) + '; if(-not $? -and $null -eq $LASTEXITCODE){exit 1}; exit $LASTEXITCODE'
    if '-command' in lowered:
        index = lowered.index('-command')
        if index + 2 != len(arguments):
            raise ValueError('Guarded -Command requires one expression')
        return '& { ' + arguments[index + 1] + ' }; if(-not $?){exit 1}; exit $LASTEXITCODE'
    raise ValueError('Guarded PowerShell requires -File or -Command')


def run(arguments, **options):
    """subprocess.run-compatible for explicit PowerShell -File/-Command calls.

    Other programs use subprocess.run directly. Total timeout includes both
    startup attempts and execution; a running target is never replayed.
    """
    if Path(str(arguments[0])).name.lower() not in ('pwsh.exe', 'powershell.exe'):
        return subprocess.run(arguments, **options)
    startup_seconds = options.pop('startup_timeout', 6.0)
    timeout = options.pop('timeout', None)
    check = options.pop('check', False)
    capture = options.pop('capture_output', False)
    input_data = options.pop('input', None)
    if capture:
        if 'stdout' in options or 'stderr' in options:
            raise ValueError('capture_output cannot be combined with stdout/stderr')
        options.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if input_data is not None:
        options['stdin'] = subprocess.PIPE
    else:
        options.setdefault('stdin', subprocess.DEVNULL)
    env = {key.upper(): value for key, value in options.get('env', os.environ).items()}
    env['PSEXECUTIONPOLICYPREFERENCE'] = 'Bypass'
    if options.get('text') or options.get('universal_newlines'):
        options.setdefault('encoding', 'utf-8')
        options.setdefault('errors', 'replace')
    if os.name == 'nt':
        options.setdefault('creationflags', subprocess.CREATE_NO_WINDOW)
    # An explicit caller path is respected; the second attempt is bounded 5.1.
    env[PIN] = str(arguments[0])
    runtimes = powershell_candidates(env)
    started = time.monotonic()
    target = _target(arguments)
    failure = None
    with tempfile.TemporaryDirectory(prefix='SnowDesktop-powershell-') as folder:
        for attempt, runtime in enumerate(runtimes, 1):
            ready = Path(folder) / ('ready-' + str(attempt))
            permit = Path(folder) / ('permit-' + str(attempt))
            bootstrap = ("[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $ProgressPreference='SilentlyContinue'; "
                         "[IO.File]::WriteAllText(" + _literal(ready) + ",'ready'); $startupWait=[Diagnostics.Stopwatch]::StartNew(); "
                         "while(-not [IO.File]::Exists(" + _literal(permit) + ")){if($startupWait.Elapsed.TotalSeconds -gt 15){exit 124}; [Threading.Thread]::Sleep(20)}; " + target)
            encoded = base64.b64encode(bootstrap.encode('utf-16-le')).decode('ascii')
            command = [runtime, '-NoProfile', '-NonInteractive', '-InputFormat', 'Text',
                       '-OutputFormat', 'Text', '-EncodedCommand', encoded]
            options['env'] = dict(env, **{PIN: runtime})
            remaining = None if timeout is None else timeout - (time.monotonic() - started)
            if remaining is not None and remaining <= 0:
                raise subprocess.TimeoutExpired(arguments, timeout)
            launch_limit = startup_seconds if remaining is None else min(startup_seconds, remaining)
            process = None
            granted = False
            try:
                process = subprocess.Popen(command, **options)
                deadline = time.monotonic() + launch_limit
                while process.poll() is None and not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                if ready.exists() and process.poll() is None:
                    permit.write_text('execute', encoding='ascii')
                    granted = True
                    remaining = None if timeout is None else max(0, timeout - (time.monotonic() - started))
                    try:
                        stdout, stderr = process.communicate(input_data, timeout=remaining)
                    except subprocess.TimeoutExpired as exc:
                        process.kill() if process.poll() is None else None
                        try:
                            stdout, stderr = process.communicate(timeout=1)
                        except subprocess.TimeoutExpired as drain:
                            stdout, stderr = drain.output, drain.stderr
                        raise subprocess.TimeoutExpired(arguments, timeout, stdout, stderr) from exc
                    except BaseException:
                        process.kill() if process.poll() is None else None
                        try:
                            process.communicate(timeout=1)
                        except subprocess.TimeoutExpired:
                            pass
                        raise
                    result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
                    if check:
                        result.check_returncode()
                    return result
                process.kill() if process.poll() is None else None
                stdout, stderr = process.communicate()
                failure = {'exitCode': process.returncode, 'stdout': stdout, 'stderr': stderr}
            except OSError as exc:
                if granted:
                    raise
                failure = {'error': str(exc)}
            # This point is reachable only when no execution permit was issued.
            evidence = Path(options.get('cwd') or os.getcwd()) / '.build/powershell-startup'
            evidence.mkdir(parents=True, exist_ok=True)
            record = evidence / (str(uuid.uuid4()) + '.json')
            details = dict(failure, runtime=runtime, arguments=list(arguments), attempt=attempt,
                           scriptStarted=False, elapsedSeconds=time.monotonic()-started,
                           pid=None if process is None else process.pid)
            for field in ('stdout', 'stderr'):
                if isinstance(details.get(field), bytes):
                    details[field] = details[field].decode('utf-8', errors='replace')
            record.write_text(json.dumps(details, ensure_ascii=False, indent=2), encoding='utf-8')
            print('PowerShell initialization failed before target execution; evidence: ' + str(record),
                  file=__import__('sys').stderr, flush=True)
    raise RuntimeError('PowerShell initialization failed twice; target was not executed: ' + str(failure))
