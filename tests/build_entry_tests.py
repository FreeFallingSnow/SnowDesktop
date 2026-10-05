"""Real supported entries, isolated shared locks and dependency inventories.

The native build boundary is a marker executable; reaching it during editing
is a failure. CMake modules are configured on a compiler-free fixture tree.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
PS = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe')

def run_entry_tests(repo):
    fixture_env = os.environ.copy()
    fixture_env.pop('SNOWDESKTOP_EXECUTION_TOKEN', None)
    root=Path(tempfile.mkdtemp(prefix='SnowDesktop-entry-')).resolve()
    scripts=root/'scripts';scripts.mkdir()
    for name in ('build.bat','build_debug.bat','build_entry.ps1','build_runtime.ps1',
                 'build_job.cs','test_manager.ps1','test_output.ps1','build_protocol.ps1','build_manager.ps1',
                 'build_inputs.ps1','build_ownership.ps1','build_missing_dependency.ps1'):
        shutil.copyfile(repo/'scripts'/name,scripts/name)
    (scripts/'build_preflight.ps1').write_text("param([switch]$ReloadShell)\nfunction Get-ReadOnlyPreflight($Root){return @{status='clear';observedUtc=[DateTime]::UtcNow.ToString('o');owners=@()}}\nif($MyInvocation.InvocationName -ne '.') {[IO.File]::WriteAllText((Join-Path $PSScriptRoot '../preflight.marker'),'called');exit 3}\n")
    directory=root/'.build/collaboration';directory.mkdir(parents=True)
    bid='b'*32
    state={'schemaVersion':1,'repositoryRoot':str(root),'current':{'id':bid,'phase':'editing',
        'owner':None,'participants':[{'id':'independent-terminal','state':'editing','registeredUtc':'2026-10-02T00:00:00Z'}]}}
    def save():(directory/'state.json').write_text(json.dumps(state),encoding='utf-8')
    save()
    def run(argv,code=2,env=None):
        p=subprocess.run(argv,cwd=str(root),env=fixture_env if env is None else env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=25)
        assert p.returncode==code,(argv,p.returncode,p.stdout,p.stderr)
        return p
    entries=[['cmd.exe','/d','/c','call scripts\\build.bat --reload-shell'],
             ['cmd.exe','/d','/c','call scripts\\build_debug.bat --reload-shell'],
             [PS,'-NoProfile','-File',str(scripts/'test_manager.ps1'),'-Mode','name','-Filter','Alpha'],
             [PS,'-NoProfile','-File',str(scripts/'build_entry.ps1'),'-Action','ide']]
    original=(directory/'state.json').read_bytes()
    # Disable the lease guard only in this fixture for a negative control.
    # HEAD can already contain the guard, so it is not a stable old baseline.
    current_batch=(scripts/'build.bat').read_bytes()
    guard_start=current_batch.index(b'rem Acquire execution authority before any preflight/process action or output write.')
    guard_end=current_batch.index(b'if defined RELOAD_SHELL (',guard_start)
    previous=current_batch[:guard_start]+current_batch[guard_end:]
    (scripts/'build.bat').write_bytes(previous)
    run(entries[0],code=3)
    assert (root/'preflight.marker').exists(),'negative control did not reach the original unguarded entry'
    (root/'preflight.marker').unlink();(scripts/'build.bat').write_bytes(current_batch)
    print('NEGATIVE CONTROL reproduced unguarded Release entry reaching preflight during editing',flush=True)
    for phase in ('editing','building'):
        state['current']['phase']=phase;save()
        for argv in entries:
            p=run(argv);assert bid in p.stderr and phase in p.stderr,p
            assert not (root/'preflight.marker').exists(),'lease rejection must precede process preflight'
    state['current']=None;save()
    held_script=root/'held.ps1'
    held_script.write_text("$l=[IO.File]::Open((Join-Path $PSScriptRoot '.build/collaboration/build.lock'),'OpenOrCreate','ReadWrite','None');[Console]::Out.WriteLine('locked');[Console]::Out.Flush();[Console]::ReadLine()|Out-Null;$l.Dispose()")
    holder=subprocess.Popen([PS,'-NoProfile','-File',str(held_script)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        assert holder.stdout.readline().strip()=='locked'
        for argv in entries:run(argv)
        assert not (root/'preflight.marker').exists()
        run([PS,'-NoProfile','-File',str(scripts/'build_manager.ps1'),'begin','new-terminal'])
    finally:
        holder.communicate('\n',timeout=5)
    # Forged environment flags and expired credentials cannot bypass a lease.
    env=dict(os.environ,SNOWDESKTOP_EXECUTION_TOKEN='forged')
    for argv in entries:run(argv,env=env)
    assert not (root/'preflight.marker').exists()
    # A legitimate standalone owner reaches preflight exactly once and preserves
    # its distinct output-ownership code. No native compilation follows.
    run(entries[0],code=3);assert (root/'preflight.marker').read_text()=='called'
    print('PASS Release/Debug/selected-test/IDE entries reject editing, frozen and leased outputs before preflight; forged flags refused; standalone code preserved',flush=True)

    # A read-only query needs neither an existing state directory nor Python.
    fresh=root/'read-only';(fresh/'scripts').mkdir(parents=True)
    for name in ('build_manager.ps1','build_entry.ps1','build_inputs.ps1','build_protocol.ps1','build_ownership.ps1','build_preflight.ps1'):
        shutil.copyfile(scripts/name,fresh/'scripts'/name)
    p=run([PS,'-NoProfile','-File',str(fresh/'scripts/build_manager.ps1'),'status'],code=0)
    assert json.loads(p.stdout)['current'] is None and not (fresh/'.build').exists()
    save();before={f.name:f.read_bytes() for f in directory.iterdir() if f.is_file()}
    run([PS,'-NoProfile','-File',str(scripts/'build_manager.ps1'),'status'],code=0)
    assert before=={f.name:f.read_bytes() for f in directory.iterdir() if f.is_file()}
    print('PASS status is an atomic read-only observation with no state-directory, lock or service creation',flush=True)

    # Exercise configure-time and generated-target guards through real CMake,
    # without configuring a compiler or touching SnowDesktop build artifacts.
    (root/'cmake').mkdir()
    shutil.copyfile(repo/'cmake/SnowDesktop.SharedBuildGuard.cmake',root/'cmake/Guard.cmake')
    (root/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\ninclude("${CMAKE_SOURCE_DIR}/cmake/Guard.cmake")\nsnowdesktop_check_shared_output()\nproject(Entry NONE)\nadd_custom_target(Native COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/native.marker")\nsnowdesktop_install_shared_guard()\n')
    (root/'CMakePresets.json').write_text(json.dumps({'version':3,'configurePresets':[{'name':'release','binaryDir':'${sourceDir}/.build'}],'buildPresets':[{'name':'release','configurePreset':'release','configuration':'Release'}]}))
    cmake=shutil.which('cmake');ctest=shutil.which('ctest');assert cmake and ctest
    p=run([cmake,'--preset','release'],code=1)
    assert 'live leased entry' in p.stderr and not (root/'.build/native.marker').exists()
    run([PS,'-NoProfile','-File',str(scripts/'build_entry.ps1'),'-Action','ide','-Targets','Native'],code=0)
    marker=root/'.build/native.marker';assert marker.exists();before_marker=marker.stat().st_mtime_ns
    run([cmake,'--build','--preset','release','--target','Native'],code=1)
    assert marker.stat().st_mtime_ns==before_marker,'expired lease cannot execute a generated IDE target'
    run([cmake,'-S',str(root),'-B',str(root/'independent')],code=0)
    run([cmake,'--build',str(root/'independent'),'--target','Native'],code=0)
    assert (root/'independent/native.marker').exists()
    print('PASS raw shared CMake configure and regenerated IDE targets refuse missing/expired authority; leased IDE runs once; independent output available',flush=True)

    # A clean CMake inventory still declares all six automatic regressions when
    # the Python executable is absent, unusable or too old. Native selection is
    # unaffected. No host compiler or project is configured here.
    source=root/'inventory';(source/'scripts').mkdir(parents=True)
    shutil.copyfile(scripts/'build_missing_dependency.ps1',source/'scripts/build_missing_dependency.ps1')
    shutil.copyfile(repo/'cmake/SnowDesktop.ToolTests.cmake',source/'ToolTests.cmake')
    (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(Inventory NONE)\nenable_testing()\nset(BUILD_TESTING ON)\nadd_test(NAME Native COMMAND "${CMAKE_COMMAND}" -E true)\nif(NO_PYTHON_FIXTURE)\n set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)\n set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)\nendif()\ninclude("${CMAKE_CURRENT_SOURCE_DIR}/ToolTests.cmake")\n')
    required={'build_workflow','build_plan_execution','build_dashboard','build_wait_retry','build_shared_resources','build_foreground_wait'}
    for case,python in [('absent',None),('broken',str(source/'absent.exe')),('working',sys.executable)]:
        output=root/('inventory-'+case)
        options=['-DSNOWDESKTOP_PYTHON_EXECUTABLE='+python] if python else ['-DNO_PYTHON_FIXTURE=ON']
        run([cmake,'-S',str(source),'-B',str(output),*options],code=0)
        inventory=json.loads(run([ctest,'--test-dir',str(output),'-C','Release','--show-only=json-v1'],code=0).stdout)['tests']
        assert {x['name'] for x in inventory}==required|{'build_dashboard_browser','Native'}
        blocked={x['name'] for x in inventory if 'environment-blocked' in next((p['value'] for p in x['properties'] if p['name']=='LABELS'),[])}
        assert blocked==(required|{'build_dashboard_browser'} if case!='working' else set())
        if case=='absent':
            run([ctest,'--test-dir',str(output),'-C','Release','-R','^Native$','--no-tests=error'],code=0)
            p=run([ctest,'--test-dir',str(output),'-C','Release','-R','^build_foreground_wait$','--no-tests=error','--output-on-failure'],code=8)
            assert 'ENVIRONMENT-BLOCKED' in p.stdout and 'not executed' in p.stdout
    print('PASS clean absent/broken/working-Python CMake inventories retain six automatic and one manual regression; native selection runs, missing dependency fails explicitly',flush=True)

    # Exercise real executable probes and the one-run fallback, not a command
    # existence mock. Broken and old aliases cannot trigger an enhancement run.
    runtime=root/'runtime';(runtime/'scripts').mkdir(parents=True);(runtime/'bin').mkdir()
    for name in ('build_runtime.ps1','build_batch_tests.ps1'):
        shutil.copyfile(repo/'scripts'/name,runtime/'scripts'/name)
    plans=runtime/'.build/collaboration';plans.mkdir(parents=True)
    (plans/(bid+'.plan.json')).write_text(json.dumps({'schemaVersion':1,'batchId':bid,'configuration':'Release','mode':'selected','buildRequired':False}))
    (runtime/'scripts/test_manager.ps1').write_text("param($Mode,$PlanBatch)\n[IO.File]::AppendAllText((Join-Path $PSScriptRoot '../fallback.count'),'once');exit 17")
    (runtime/'scripts/build_test_retry.py').write_text("from pathlib import Path\nPath(__file__).resolve().parents[1].joinpath('enhancement.count').write_text('once')\nraise SystemExit(19)\n")
    system_path=str(Path(os.environ['WINDIR'])/'System32')
    batch_command=[PS,'-NoProfile','-File',str(runtime/'scripts/build_batch_tests.ps1'),'-Batch',bid]
    missing_env=dict(os.environ,PATH=system_path)
    run(batch_command,code=17,env=missing_env)
    alias=runtime/'bin/python.exe';alias.write_bytes(b'broken executable fixture')
    broken_env=dict(os.environ,PATH=str(alias.parent)+os.pathsep+system_path)
    run(batch_command,code=17,env=broken_env)
    # A tiny executable that reports the unsupported-version probe code.
    alias.unlink();csharp=runtime/'old.cs';csharp.write_text('class OldPython {static int Main(){return 78;}}')
    csc=Path(os.environ['WINDIR'])/'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    run([str(csc),'/nologo','/target:exe','/out:'+str(alias),str(csharp)],code=0)
    run(batch_command,code=17,env=broken_env)
    assert (runtime/'fallback.count').read_text()=='once'*3 and not (runtime/'enhancement.count').exists()
    run(batch_command,code=19)
    assert (runtime/'enhancement.count').read_text()=='once' and (runtime/'fallback.count').read_text()=='once'*3,'Started enhancement failure must never replay the base test run'
    print('PASS no/invalid/old Python probes each fall back once with original code 17; started healthy enhancement failure 19 is not replayed',flush=True)

    # The ordinary batch coordinator must also tolerate a broken optional
    # monitor dependency before it reaches the base PowerShell registration.
    alias.write_bytes(b'broken executable fixture')
    p=run(['cmd.exe','/d','/c','call scripts\\build.bat begin no-python-terminal'],code=0,env=broken_env)
    registration=json.loads(p.stdout);assert registration['state']=='editing'
    p=run(['cmd.exe','/d','/c','call scripts\\build.bat watch wait terminal --condition window'],code=2,env=broken_env)
    assert 'enhancement unavailable' in p.stderr
    # Test the unchanged Python CLI argument contract, including flag syntax.
    (runtime/'scripts/build_wait_tasks.py').write_text('import sys,json\nprint(json.dumps(sys.argv[1:]))\nraise SystemExit(23)\n')
    p=run([PS,'-NoProfile','-File',str(runtime/'scripts/build_runtime.ps1'),'watch','watch','wait','terminal','--condition','window'],code=23)
    assert json.loads(p.stdout)==['watch','wait','terminal','--condition','window']
    print('PASS ordinary begin survives broken monitor dependency; unavailable watch reports 2; available watch forwards flags and original code 23',flush=True)

    # Required unavailable tests must get a task coverage receipt before any
    # compilation. The existing test-manager fixture exercises actual JUnit
    # accounting separately; here CTest metadata is a controlled boundary.
    print('ENTRY evidence: '+str(root),flush=True)

if __name__=='__main__':run_entry_tests(Path(__file__).resolve().parents[1])
