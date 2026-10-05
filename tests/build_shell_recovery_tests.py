"""Actual leased entry/Job with a harmless process standing in for Explorer."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from powershell_runtime import powershell_executable, run as run_process
import tempfile
import time

PS = powershell_executable()


def fixture_process(record, root, stop=False):
    """Inspect/stop the recorded birth identity through one native handle."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_ulonglong)] * 4
    kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000 | 0x100000 | 1, False, int(record['pid']))
    if not handle:
        assert ctypes.get_last_error() == 87, ctypes.WinError(ctypes.get_last_error())
        return False
    try:
        times = [ctypes.c_ulonglong() for _ in range(4)]
        assert kernel.GetProcessTimes(handle, *[ctypes.byref(x) for x in times])
        if str(times[0].value + 504911232000000000) != str(record['startTicks']) or kernel.WaitForSingleObject(handle, 0) == 0:
            return False
        size = ctypes.c_ulong(32768)
        image = ctypes.create_unicode_buffer(size.value)
        assert kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size))
        expected = {os.path.normcase(str(Path(PS).resolve())), os.path.normcase(str((root/'bin/cmake.exe').resolve()))}
        assert os.path.normcase(str(Path(image.value).resolve())) in expected, 'Unexpected fixture process image; action refused'
        if stop:
            assert kernel.TerminateProcess(handle, 4)
            assert kernel.WaitForSingleObject(handle, 5000) == 0
        return True
    finally:
        kernel.CloseHandle(handle)


def run_shell_recovery_tests(repo):
    env = dict(os.environ)
    env.pop('SNOWDESKTOP_EXECUTION_TOKEN', None)
    evidence = []
    for mode in ('configure-failure', 'normal', 'interrupted'):
        root = Path(tempfile.mkdtemp(prefix='SnowDesktop-shell-recovery-'))
        scripts = root/'scripts'; scripts.mkdir()
        (root/'bin').mkdir()
        for name in ('powershell_runtime.bat', 'build_entry.ps1', 'build_preflight.ps1', 'build_job.cs', 'build.bat'):
            shutil.copyfile(repo/'scripts'/name, scripts/name)
        surrogate = root/'restored-shell.ps1'
        surrogate.write_text("[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'shell-token.txt'),[string]$env:SNOWDESKTOP_EXECUTION_TOKEN)\nStart-Sleep -Seconds 60\n")
        mocks = r'''
function Get-ReadOnlyPreflight([string]$Root,[string]$Configuration='Release') {
 $owners=@();if(-not [IO.File]::Exists((Join-Path $Root 'restored.json'))){
  $owners=@([pscustomobject]@{pid=12345;kind='hook';startTicks='639000000000000000'})
 }
 return [pscustomobject]@{status=$(if($owners.Count){'blocked'}else{'clear'});owners=$owners;unknownPids=@()}
}
function Get-Process {param($Id,$Name,$ErrorAction)
 if($Id -eq 12345){return [pscustomobject]@{Id=12345;StartTime=[DateTime]::SpecifyKind((New-Object DateTime 639000000000000000),[DateTimeKind]::Utc)}}
 return Microsoft.PowerShell.Management\Get-Process @PSBoundParameters
}
function Stop-Process {param($InputObject,$ErrorAction)
 if($InputObject.Id -ne 12345){throw 'Fixture refuses any real process termination'}
 [IO.File]::WriteAllText((Join-Path $PSScriptRoot '../mock-stop'),'observed hook only')
}
function Start-Process {param($FilePath,$WindowStyle)
 if([IO.Path]::GetFileName($FilePath) -ne 'explorer.exe'){throw 'Fixture refuses any real Shell action'}
 $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
 $child=Microsoft.PowerShell.Management\Start-Process -FilePath (Get-BuildPowerShell) -ArgumentList ('-NoProfile -File "'+(Join-Path $root 'restored-shell.ps1')+'"') -WindowStyle Hidden -PassThru
 [IO.File]::WriteAllText((Join-Path $root 'restored.json'),(ConvertTo-Json @{pid=$child.Id;startTicks=$child.StartTime.ToUniversalTime().Ticks.ToString()}))
}
'''
        preflight = (scripts/'build_preflight.ps1').read_text(encoding='utf-8-sig')
        anchor = "if($MyInvocation.InvocationName -ne '.')"
        assert anchor in preflight
        (scripts/'build_preflight.ps1').write_text(preflight.replace(anchor, mocks+'\n'+anchor, 1), encoding='utf-8-sig')
        if mode == 'configure-failure':
            (root/'bin/cmake.cmd').write_text('@echo off\nexit /b 19\n')
        else:
            source = root/'bin/standin.cs'
            source.write_text(r'''using System;using System.IO;using System.Diagnostics;using System.Threading;
class Boundary {static int Main(string[] args){
 string root=Directory.GetCurrentDirectory();
 if(args.Length>0&&args[0]=="child"){Thread.Sleep(60000);return 0;}
 File.WriteAllText(Path.Combine(root,"native-token.txt"),Environment.GetEnvironmentVariable("SNOWDESKTOP_EXECUTION_TOKEN")??"");
 string record=Path.Combine(root,"native-child.json");
 if(!File.Exists(record)){
  Process child=Process.Start(Environment.GetCommandLineArgs()[0],"child");
  File.WriteAllText(record,"{\"pid\":"+child.Id+",\"startTicks\":\""+child.StartTime.ToUniversalTime().Ticks+"\"}");
 }
 File.WriteAllText(Path.Combine(root,"native-started"),"started");
 if(File.Exists(Path.Combine(root,"hold-native")))Thread.Sleep(60000);
 return 0;
}}
''')
            csc = Path(os.environ['WINDIR'])/'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
            compiled = run_process([str(csc), '/nologo', '/out:'+str(root/'bin/cmake.exe'), str(source)], capture_output=True)
            assert compiled.returncode == 0, compiled.stdout+compiled.stderr
            (scripts/'arrange_build_output.ps1').write_text('param($BuildOutput)\nexit 0\n')
        local_env = dict(env, PATH=str(root/'bin')+os.pathsep+env['PATH'])
        if mode == 'interrupted': (root/'hold-native').write_text('hold')
        run = subprocess.Popen([PS, '-NoProfile', '-File', str(scripts/'build_entry.ps1'), '-Action', 'release', '-ReloadShell'], cwd=str(root), env=local_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            if mode == 'interrupted':
                deadline = time.monotonic()+15
                while not (root/'native-started').exists() and run.poll() is None and time.monotonic()<deadline: time.sleep(.05)
                assert (root/'native-started').exists(), 'Native boundary not started: '+str(root)
                run.terminate()  # Exact Popen owner handle, no user process action.
            stdout, stderr = run.communicate(timeout=15)
            if mode == 'interrupted': assert run.returncode != 0, (mode, stdout, stderr)
            else: assert run.returncode == (19 if mode == 'configure-failure' else 0), (mode, stdout, stderr)
            shell = json.loads((root/'restored.json').read_text(encoding='utf-8-sig'))
            assert fixture_process(shell, root), 'Restored Shell surrogate was killed with the private Job: '+str(root)
            deadline = time.monotonic()+15
            while not (root/'shell-token.txt').exists() and time.monotonic()<deadline: time.sleep(.05)
            assert (root/'shell-token.txt').read_text() == '', 'Restored user Shell inherited private execution authority'
            if mode != 'configure-failure':
                assert len((root/'native-token.txt').read_text()) == 64, 'Native child lost delegated build authority'
                child = json.loads((root/'native-child.json').read_text())
                deadline = time.monotonic()+5
                while fixture_process(child, root) and time.monotonic()<deadline: time.sleep(.05)
                assert not fixture_process(child, root), 'Contained native descendant survived Job close: '+str(root)
            else:
                # A live descendant credential authorizes build children, but
                # never a destructive preflight launched inside that Job.
                probe = root/'descendant-preflight.ps1'
                probe.write_text(r'''$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
$entry=Enter-BuildEntry $PSScriptRoot
try {
 Add-Type -Path (Join-Path $PSScriptRoot 'scripts/build_job.cs')
 $command='"'+(Get-BuildPowerShell)+'" -NoProfile -File "'+(Join-Path $PSScriptRoot 'scripts/build_preflight.ps1')+'" -ReloadShell'
 exit [SnowDesktop.Build.Job]::RunLeasedCommand($PSScriptRoot,(Join-Path $PSScriptRoot 'descendant-preflight.log'),$command)
} finally {Exit-BuildEntry $entry}
''')
                original = (root/'restored.json').read_bytes()
                blocked = run_process([PS, '-NoProfile', '-File', str(probe)], cwd=str(root), env=env, capture_output=True, timeout=15)
                assert blocked.returncode != 0 and (root/'restored.json').read_bytes() == original
                assert b'live execution lease owner' in (root/'descendant-preflight.log').read_bytes()
            evidence.append({'scenario': mode, 'root': str(root), 'exitCode': run.returncode, 'restoredSurvived': True, 'nativeContained': mode != 'configure-failure'})
        finally:
            if run.poll() is None: run.terminate(); run.wait(timeout=5)
            for path in (root/'restored.json', root/'native-child.json'):
                if path.exists(): fixture_process(json.loads(path.read_text(encoding='utf-8-sig')), root, stop=True)
    print('PASS leased owner Shell recovery survives completion, configure exit19 and owner interruption; native descendants remain contained')
    print('SHELL RECOVERY evidence: '+json.dumps(evidence))


if __name__ == '__main__': run_shell_recovery_tests(Path(__file__).resolve().parents[1])
