"""Real 7/5.1 execution and a deliberately delayed initialization boundary."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import powershell_runtime as runtime


def main(repo):
    root = Path(tempfile.mkdtemp(prefix='SnowDesktop PowerShell runtime '))
    fallback = Path(os.environ['WINDIR']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    core = Path(os.environ['ProgramFiles']) / 'PowerShell/7/pwsh.exe'
    script = root / "target's script.ps1"
    script.write_text('''param([string]$Value,[int]$Code=0,[int]$Delay=0)
[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'executed.txt'),"executed`n")
@{value=$Value;major=$PSVersionTable.PSVersion.Major;runtime=$env:SNOWDESKTOP_ENTRY_POWERSHELL} | ConvertTo-Json -Compress
if($Delay){Start-Sleep -Seconds $Delay}
exit $Code
''', encoding='utf-8-sig')
    marker = root / 'executed.txt'
    selector = root/'selector.cmd'
    selector.write_text('@echo off\ncall "'+str(repo/'scripts/powershell_runtime.bat')+'"\nif errorlevel 1 exit /b 2\necho %SNOWDESKTOP_ENTRY_POWERSHELL%\n')
    for engine in [fallback] + ([core] if core.is_file() else []):
        marker.unlink(missing_ok=True)
        result = runtime.run([str(engine), '-NoProfile', '-File', str(script), '-Value', "路径 with space and 'quote'", '-Code', '39'],
                             cwd=str(root), capture_output=True, text=True, timeout=15)
        assert result.returncode == 39 and marker.read_text().splitlines() == ['executed']
        payload = json.loads(result.stdout)
        assert payload['value'] == "路径 with space and 'quote'" and Path(payload['runtime']) == engine
        expected = 7 if engine == core else 5
        assert payload['major'] == expected
        env = dict(os.environ, **{runtime.PIN: str(engine)})
        batch = subprocess.run(['cmd.exe', '/d', '/c', str(selector)],
                               env=env, capture_output=True, text=True, timeout=5)
        assert batch.returncode == 0 and batch.stdout.strip() == str(engine)
        expression = ". '" + str(repo/'scripts/build_entry.ps1').replace("'", "''") + "'; Get-BuildPowerShell"
        result = runtime.run([str(engine), '-NoProfile', '-Command', expression], env=env, cwd=str(root), capture_output=True, text=True, timeout=15)
        assert result.returncode == 0 and result.stdout.strip() == str(engine), result.stderr
        # Run only the real version-read statement, never squash/commit/tag.
        version_line = next(line for line in (repo/'scripts/squash_release_to_main.bat').read_text(encoding='utf-8-sig').splitlines() if line.startswith('for /f "usebackq'))
        version_probe = root/'read-version.cmd'
        (root/'version.json').write_bytes((repo/'version.json').read_bytes())
        version_probe.write_text('@echo off\nsetlocal\n'+version_line+'\necho %VERSION%\n')
        version = subprocess.run(['cmd.exe','/d','/c',str(version_probe)], env=dict(env, PSExecutionPolicyPreference='Bypass'), cwd=str(root), capture_output=True, text=True, timeout=6)
        assert version.returncode == 0 and version.stdout.strip() == json.loads((root/'version.json').read_text(encoding='utf-8-sig'))['version'], version.stderr
        print('PASS argument, exit-code and pinned-runtime compatibility: ' + str(engine), flush=True)

    # Missing 7 and absent PATH still have a real 5.1 fallback, without probes.
    env = dict(os.environ, PROGRAMFILES=str(root/'absent'), PROGRAMW6432=str(root/'absent'), PATH='')
    env.pop(runtime.PIN, None)
    assert Path(runtime.powershell_candidates(env)[0]) == fallback
    batch = subprocess.run(['cmd.exe', '/d', '/c', str(selector)], env=env, capture_output=True, text=True, timeout=5)
    assert batch.returncode == 0 and Path(batch.stdout.strip()) == fallback
    print('PASS missing PowerShell 7 selects Windows PowerShell 5.1', flush=True)

    original = subprocess.Popen
    launches = []
    def delayed(command, **options):
        launches.append(command[0])
        if len(launches) == 1:
            bootstrap = Path(command[-1])
            body = bootstrap.read_text(encoding='utf-8-sig')
            bootstrap.write_text('Start-Sleep -Seconds 6; ' + body, encoding='utf-8-sig')
        return original(command, **options)
    marker.unlink(missing_ok=True)
    preferred = str(core if core.is_file() else fallback)
    started = time.monotonic()
    with patch.object(runtime.subprocess, 'Popen', delayed):
        result = runtime.run([preferred, '-NoProfile', '-File', str(script), '-Value', 'startup-recovery'],
                             cwd=str(root), capture_output=True, text=True, timeout=15, startup_timeout=3)
    assert result.returncode == 0 and len(launches) == 2 and Path(launches[1]) == fallback
    assert marker.read_text().splitlines() == ['executed'] and time.monotonic()-started < 15
    evidence = list((root/'.build/powershell-startup').glob('*.json'))
    assert len(evidence) == 1 and json.loads(evidence[0].read_text())['scriptStarted'] is False
    print('PASS delayed initialization recovers once via 5.1, retains first failure and executes target once', flush=True)

    # Execution has begun: never retry a business timeout, including side effects.
    launches.clear()
    def counted(command, **options):
        launches.append(command[0])
        return original(command, **options)
    marker.unlink(missing_ok=True)
    with patch.object(runtime.subprocess, 'Popen', counted):
        try:
            runtime.run([preferred, '-NoProfile', '-File', str(script), '-Value', 'business-timeout', '-Delay', '8'],
                        cwd=str(root), capture_output=True, text=True, timeout=4, startup_timeout=3)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise AssertionError('Expected target timeout')
    assert len(launches) == 1 and marker.read_text().splitlines() == ['executed']
    assert len(list((root/'.build/powershell-startup').glob('*.json'))) == 1
    print('PASS target timeout preserves side effect once and is never retried', flush=True)

    # Exercise the PowerShell asynchronous guard itself, including 5.1 parsing.
    broken = root/'broken pwsh.exe'
    broken.write_bytes(b'not an executable')
    guard = root/'guard.ps1'
    guard.write_text('''param([string]$Entry,[string]$Target,[string]$Broken)
. $Entry
$env:SNOWDESKTOP_ENTRY_POWERSHELL=$Broken
$out=Join-Path $PSScriptRoot 'guard.out';$err=Join-Path $PSScriptRoot 'guard.err'
$p=Start-BuildPowerShellScript -Script $Target -Arguments '-Value "async guard,with comma" -Code 41' -WorkingDirectory $PSScriptRoot -OutputPath $out -ErrorPath $err
$p.WaitForExit();$code=$p.ExitCode;$p.Dispose();exit $code
''', encoding='utf-8-sig')
    for engine in [fallback] + ([core] if core.is_file() else []):
        marker.unlink(missing_ok=True)
        result = runtime.run([str(engine), '-NoProfile', '-File', str(guard), '-Entry', str(repo/'scripts/build_entry.ps1'),
                              '-Target', str(script), '-Broken', str(broken)], cwd=str(root), capture_output=True, text=True, timeout=20)
        assert result.returncode == 41 and marker.read_text().splitlines() == ['executed'], result.stderr
        first = json.loads((root/'guard.out.startup-1.json').read_text(encoding='utf-8-sig'))
        assert first['scriptStarted'] is False and Path(first['runtime']) == broken
        payload = json.loads((root/'guard.out.retry').read_text(encoding='utf-8-sig'))
        assert payload['value'] == 'async guard,with comma' and payload['major'] == 5
    print('PASS asynchronous guard launch failure falls back to 5.1 on both parent engines', flush=True)

    # A detached waiter must not keep a completed parent's stdout pipe open.
    detached = root/'detached.ps1'
    detached.write_text('''param([string]$Entry,[string]$Target)
. $Entry
$p=Start-BuildPowerShellScript -Script $Target -Arguments '-Value detached -Delay 12' -WorkingDirectory $PSScriptRoot -OutputPath (Join-Path $PSScriptRoot 'detached.out') -ErrorPath (Join-Path $PSScriptRoot 'detached.err')
@{pid=$p.Id;startTicks=$p.StartTime.ToUniversalTime().Ticks.ToString()} | ConvertTo-Json -Compress | Set-Content (Join-Path $PSScriptRoot 'detached.json') -Encoding UTF8
$p.Dispose();Write-Output 'parent exited';exit 0
''', encoding='utf-8-sig')
    for engine in [fallback] + ([core] if core.is_file() else []):
        try:
            result = runtime.run([str(engine), '-NoProfile', '-File', str(detached), '-Entry', str(repo/'scripts/build_entry.ps1'), '-Target', str(script)],
                                 cwd=str(root), capture_output=True, text=True, timeout=6)
            assert result.returncode == 0 and result.stdout.strip() == 'parent exited'
            child = json.loads((root/'detached.json').read_text(encoding='utf-8-sig'))
            verify = "$p=Get-Process -Id " + str(child['pid']) + "; if($p.StartTime.ToUniversalTime().Ticks.ToString() -ne '"+child['startTicks']+"'){exit 2};exit 0"
            alive = runtime.run([str(engine), '-NoProfile', '-Command', verify], cwd=str(root), capture_output=True, timeout=5)
            assert alive.returncode == 0
        finally:
            receipt = root/'detached.json'
            if receipt.is_file():
                child = json.loads(receipt.read_text(encoding='utf-8-sig'))
                stop = "$p=Get-Process -Id " + str(child['pid']) + " -ErrorAction SilentlyContinue; if($p -and $p.StartTime.ToUniversalTime().Ticks.ToString() -eq '"+child['startTicks']+"'){Stop-Process -InputObject $p};exit 0"
                runtime.run([str(engine), '-NoProfile', '-Command', stop], cwd=str(root), capture_output=True, timeout=5)
                receipt.unlink()
    print('PASS detached child survives parent completion without inheriting parent output pipes, on both engines', flush=True)
    print('Evidence fixture: ' + str(root), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    main(parser.parse_args().repo.resolve())
