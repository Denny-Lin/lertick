#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stub-only deployment/build tests. Never load BPF or change a host scheduler.
set -eu
exec python3 - <<'PY'
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile
import time

project = Path.cwd()
assert (project / 'deploy.sh').is_file(), 'run from the repository root'

mux = r'''#!/usr/bin/env python3
import hashlib,json,os,signal,subprocess,sys,time
from pathlib import Path
name=Path(sys.argv[0]).name; args=sys.argv[1:]; env=os.environ
def log(kind,**more):
    with open(env['STUB_LOG'],'a') as f:f.write(json.dumps(dict(tool=kind,args=args,**more))+'\n')
def state():return Path(env['STUB_STATE_FILE']).read_text().strip()
if name=='uname':
    print({'-s':'Linux','-m':'aarch64','-r':env.get('STUB_KERNEL','7.0.0-test')}.get(args[0] if args else '', 'Linux'));sys.exit(0)
if name=='id':print(env.get('STUB_UID','1000'));sys.exit(0)
if name=='cat':
    if args==['/sys/kernel/sched_ext/state']:
        if state()=='MISSING':sys.exit(1)
        print(state());sys.exit(0)
    if args==['/sys/kernel/sched_ext/root/ops']:print(env.get('STUB_OPS','scx_lertick'));sys.exit(0)
    os.execv('/bin/cat',['cat',*args])
if name=='sudo':
    log('sudo'); child=dict(env,STUB_UID='0');os.execvpe(args[0],args,child)
if name=='stat':
    target=args[-1]
    print('1000 755' if target==env.get('STUB_BAD_OWNER') else '0 777' if target==env.get('STUB_BAD_MODE') else '0 755');sys.exit(0)
if name=='cp':
    log('copy',locked=env.get('STUB_MAINTENANCE')=='1');shutil=__import__('shutil');shutil.copyfile(args[0],args[1]);sys.exit(0)
if name=='native_build.sh':log('build');sys.exit(0)
if name=='preflight.sh':log('preflight');sys.exit(0)
if name=='build_libbpf.sh':log('deps');sys.exit(0)
if name.startswith('clang'):
    print('  bpf - BPF target' if '-print-targets' in args else env.get('STUB_CLANG_VERSION','clang version 18.1.3'));sys.exit(0)
if name in ['cc','alternate-cc']:print(env.get('STUB_CC_VERSION','cc version 1.0'));sys.exit(0)
if name=='bpftool':print(env.get('STUB_BPFTOOL_VERSION','bpftool v7.7.0'));sys.exit(0)
if name=='pkg-config':
    if '--modversion' in args:print(env.get('STUB_LIBBPF_VERSION','1.7.0'))
    sys.exit(0)
if name=='sha256sum':
    for path in args:
        digest=env.get('STUB_BTF_HASH','abc') if path=='/sys/kernel/btf/vmlinux' else hashlib.sha256(Path(path).read_bytes()).hexdigest()
        print(digest+'  '+path)
    sys.exit(0)
if name=='flock':
    log('build-lock');os.fstat(9);sys.exit(1 if env.get('STUB_LOCK_FAIL')=='1' else 0)
if name=='git':sys.exit(0)
if name=='make':
    root=Path(args[args.index('-C')+1]);cached=(root/'build/vmlinux.h').exists();log('make',cached=cached)
    for file in ['vmlinux.h','scx_lertick.bpf.o','scx_lertick.bpf.skel.h','scx_lertick']:(root/'build'/file).write_text('built\n')
    sys.exit(0)
if name=='scx_lertick':
    log('loader')
    if '--help' in args:print('--check --verify --maintenance');sys.exit(0)
    if '--check' in args:print('{"event":"kernel_check"}');sys.exit(0)
    if '--verify' in args:log('verify-no-attach');print('{"event":"verified"}');sys.exit(0)
    if '--maintenance' in args:
        if state() not in ['disabled','MISSING']:sys.exit(1)
        log('maintenance');command=args[args.index('--')+1:];sys.exit(subprocess.run(command,env=dict(env,STUB_MAINTENANCE='1')).returncode)
    log('attach');Path(env['STUB_STATE_FILE']).write_text('enabled')
    def finish(signum=0,frame=None):
        Path(env['STUB_STATE_FILE']).write_text('enabled' if env.get('STUB_LEAVE_ENABLED')=='1' else 'disabled')
        Path(env['STUB_CHILD_DONE']).write_text(str(signum));sys.exit(0)
    signal.signal(signal.SIGTERM,finish)
    Path(env['STUB_CHILD_STARTED']).write_text(str(os.getpid()))
    print('{"event":"registered"}',flush=True)
    if env.get('STUB_WAIT')=='1':
        while True:time.sleep(.02)
    finish()
raise SystemExit('unexpected stub '+name)
'''

def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)

with tempfile.TemporaryDirectory(prefix='lertick-deploy-fixtures-') as directory:
    work = Path(directory).resolve()
    tree = work / 'checkout'
    tools = work / 'tools'
    tools.mkdir()
    for name in ['uname','id','cat','sudo','stat','cp','cc','alternate-cc','clang-18','bpftool','pkg-config','sha256sum','flock','git','make']:
        executable(tools/name, mux)
    (tree/'src/userspace').mkdir(parents=True)
    (tree/'src/userspace/scx_lertick.c').touch()
    (tree/'Makefile').touch()
    executable(tree/'deploy.sh', (project/'deploy.sh').read_text())
    executable(tree/'build/scx_lertick', mux)
    executable(tree/'scripts/native_build.sh', mux)
    state = work/'state'; state.write_text('disabled')
    log = work/'calls.jsonl'; log.touch()
    env = dict(os.environ, PATH=str(tools)+os.pathsep+os.environ['PATH'], STUB_LOG=str(log),
               STUB_STATE_FILE=str(state), STUB_UID='1000',
               STUB_CHILD_STARTED=str(work/'child-started'), STUB_CHILD_DONE=str(work/'child-done'))
    for variable in ['PREFIX','DESTDIR','CLANG','BPFTOOL','CC','PKG_CONFIG','PKG_CONFIG_PATH','LERTICK_DEPS_DIR','LERTICK_LIBBPF_PREFIX']:
        env.pop(variable, None)
    def calls():return [json.loads(line) for line in log.read_text().splitlines()]
    def clear():log.write_text('')
    def invoke(*args, extra=None, binary=None, ok=True):
        result=subprocess.run([str(binary or tree/'deploy.sh'),*args],env=dict(env,**(extra or {})),capture_output=True,text=True,timeout=10)
        assert (result.returncode==0)==ok,(args,result.returncode,result.stdout,result.stderr)
        return result
    def wait_for(path):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            if path.exists():return
            time.sleep(.02)
        raise AssertionError(f'timed out waiting for {path}')

    clear()
    quoted="telemetry a'b.jsonl"
    result=invoke('plan','--stats-json',quoted)
    command_line=result.stdout.splitlines()[1]
    assert shlex.split(command_line)==['sudo',str(tree/'build/scx_lertick'),'--mode','online','--duration','30','--track-all','--cpuperf','auto','--stats-json',quoted]
    assert calls()==[], 'plan invoked build, sudo or loader'
    for args in [('run','--fault-stall'),('run','--partial'),('run','--cpuperf','max'),('run','--duration','0'),
                 ('run','--duration','3601'),('run','--sample-ms','99'),('check','--mode','online'),('plan','--mode','invalid')]:
        invoke(*args,ok=False)
    clear();state.write_text('enabled');invoke('check',ok=False);invoke('verify',ok=False);invoke('run',ok=False)
    assert not any(x['tool'] in ['loader','sudo','build'] for x in calls()), 'active scheduler was not rejected early'
    state.write_text('disabled');clear();invoke('check');invoke('verify')
    assert any(x['tool']=='verify-no-attach' for x in calls()) and not any(x['tool']=='attach' for x in calls())
    assert state.read_text()=='disabled'
    clear();result=invoke('run')
    assert '"event"' not in result.stdout
    attached=[x for x in calls() if x['tool']=='loader' and '--track-all' in x['args']]
    assert attached and attached[-1]['args']==['--mode','online','--duration','30','--track-all','--cpuperf','auto']
    result=invoke('run','--stats-json','-');assert '"event":"registered"' in result.stdout
    invoke('run',extra={'STUB_LEAVE_ENABLED':'1'},ok=False);state.write_text('disabled')
    for file in [work/'child-started',work/'child-done']:file.unlink(missing_ok=True)
    process=subprocess.Popen([str(tree/'deploy.sh'),'run'],env=dict(env,STUB_WAIT='1'),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        wait_for(work/'child-started');process.send_signal(signal.SIGTERM);stdout,stderr=process.communicate(timeout=5)
        assert process.returncode==143,(process.returncode,stderr)
        assert (work/'child-done').read_text()==str(int(signal.SIGTERM)) and state.read_text()=='disabled'
    finally:
        if process.poll() is None:process.kill();process.wait(timeout=5)

    stage=work/'stage with spaces';clear()
    invoke('install',extra={'DESTDIR':str(stage)})
    installed=stage/'usr/local'
    assert (installed/'bin/lertick').is_file() and (installed/'sbin/scx_lertick').is_file()
    assert not any(x['tool'] in ['loader','sudo','maintenance','attach'] for x in calls()), 'staging executed privileged runtime'
    invoke('status',binary=installed/'bin/lertick')
    invoke('run',extra={'DESTDIR':str(stage)},ok=False)
    invoke('uninstall',extra={'DESTDIR':str(stage)})
    assert not (installed/'sbin/scx_lertick').exists()
    prefix=work/'protected-prefix';clear()
    invoke('install',extra={'PREFIX':str(prefix)})
    copies=[x for x in calls() if x['tool']=='copy']
    assert copies and all(x['locked'] for x in copies), 'installation escaped maintenance ownership'
    state.write_text('enabled');invoke('uninstall',extra={'PREFIX':str(prefix)},ok=False)
    assert (prefix/'sbin/scx_lertick').exists()
    state.write_text('MISSING');invoke('uninstall',extra={'PREFIX':str(prefix)})
    assert not (prefix/'sbin/scx_lertick').exists(), 'unsupported kernel prevented file maintenance'
    state.write_text('disabled')
    invoke('install',extra={'PREFIX':str(prefix),'STUB_BAD_OWNER':str(work)},ok=False)
    invoke('install',extra={'PREFIX':str(prefix),'STUB_BAD_MODE':str(work)},ok=False)
    link=work/'symlink-prefix';link.symlink_to(prefix)
    invoke('install',extra={'PREFIX':str(link)},ok=False)
    invoke('install',extra={'PREFIX':'/'},ok=False)
    invoke('install-locked',ok=False)

    # Run the actual build orchestrator with fake tools and kernel metadata.
    executable(tree/'scripts/native_build.sh',(project/'scripts/native_build.sh').read_text())
    executable(tree/'scripts/preflight.sh',mux)
    executable(tree/'scripts/build_libbpf.sh',mux)
    def build(extra=None,ok=True):
        result=subprocess.run([str(tree/'scripts/native_build.sh')],env=dict(env,**(extra or {})),capture_output=True,text=True,timeout=10)
        assert (result.returncode==0)==ok,(result.returncode,result.stdout,result.stderr)
    clear();build()
    assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is False
    clear();build()
    assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is True
    for change in [{'STUB_BTF_HASH':'different'},{'STUB_CLANG_VERSION':'clang version 18.2.0'},
                   {'STUB_BPFTOOL_VERSION':'bpftool v7.8.0'},{'STUB_LIBBPF_VERSION':'1.7.1'},
                   {'STUB_CC_VERSION':'cc version 2.0'},{'CC':'alternate-cc'}]:
        build()  # Restore the same baseline so each changed input is isolated.
        clear();build(change)
        assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is False
        clear();build(change)
        assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is True
    build();clear();build()
    assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is True
    (tree/'Makefile').write_text('CFLAGS += -DTEST_RECIPE_REVISION=1\n')
    clear();build()
    assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is False, 'changed Makefile reused the loader'
    with (tree/'scripts/native_build.sh').open('a') as file:file.write('\n# build recipe revision\n')
    clear();build()
    assert [x for x in calls() if x['tool']=='make'][-1]['cached'] is False, 'changed native recipe reused the loader'
    clear();build({'STUB_LOCK_FAIL':'1'},ok=False)
    assert not any(x['tool'] in ['deps','make'] for x in calls()), 'concurrent-build refusal came too late'
    clear();build({'STUB_UID':'0'},ok=False)
    assert calls()==[], 'root dependency build was allowed'

print('test_deploy: plan/check/verify, finite defaults, signals, trust paths, maintenance, staging and build fingerprints passed (stubs only)')
PY
