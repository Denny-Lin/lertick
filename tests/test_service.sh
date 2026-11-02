#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixture-only service transactions: no host systemd, BPF, sysfs or sudo changes.
set -eu
exec python3 - <<'PY'
from pathlib import Path
import json, os, shutil, subprocess, tempfile
project=Path.cwd()
mux=r'''#!/usr/bin/env python3
import hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
name=Path(sys.argv[0]).name;args=sys.argv[1:];env=os.environ
sf=Path(env['STUB_SERVICE']);kf=Path(env['STUB_STATE']);lf=Path(env['STUB_LOG'])
def log(tool,**extra):
 with lf.open('a') as f:f.write(json.dumps(dict(tool=tool,args=args,**extra))+'\n')
def get():return json.loads(sf.read_text())
def put(value):sf.write_text(json.dumps(value))
if name=='id':print(env.get('STUB_UID','0'));sys.exit(0)
if name=='uname':print('Linux');sys.exit(0)
if name=='stat':print('1000 755' if args[-1]==env.get('STUB_BAD_OWNER') else '0 755');sys.exit(0)
if name=='cat':
 if args==['/sys/kernel/sched_ext/state']:print(kf.read_text());sys.exit(0)
 if args==['/sys/kernel/sched_ext/root/ops']:print(get().get('ops','scx_lertick'));sys.exit(0)
 os.execv('/bin/cat',['cat',*args])
if name=='sha256sum':
 for arg in args:print(hashlib.sha256(Path(arg).read_bytes()).hexdigest()+'  '+arg)
 sys.exit(0)
if name=='flock':log('transaction-lock');os.fstat(9);sys.exit(1 if env.get('STUB_LOCK_FAIL') else 0)
if name=='cp':log('copy',maintenance=env.get('STUB_MAINTENANCE')=='1');shutil.copyfile(args[0],args[1]);sys.exit(0)
if name=='sudo':log('sudo');os.execvpe(args[0],args,dict(env,STUB_UID='0'))
if name=='native_build.sh':log('build');sys.exit(0)
if name=='systemctl':
 log('systemctl');s=get();cmd=args[0]
 if cmd=='show':
  if 'FragmentPath' in ' '.join(args):print(env['STUB_UNIT'] if Path(env['STUB_UNIT']).exists() else s.get('foreign_unit',''))
  else:print('ActiveState='+('active' if s['active'] else 'inactive')+'\nUnitFileState='+('enabled' if s['enabled'] else 'disabled'))
  sys.exit(0)
 if cmd=='is-active':sys.exit(0 if s['active'] else 3)
 if cmd=='is-enabled':sys.exit(0 if s['enabled'] else 1)
 if cmd=='reset-failed':
  if env.get('STUB_RESET_NOT_LOADED')=='1':print('Unit lertick.service not loaded.',file=sys.stderr);sys.exit(1)
  sys.exit(0)
 if cmd=='daemon-reload':sys.exit(0)
 if cmd in ['stop','disable','restart']:
  if s['active']:kf.write_text('disabled')
  s['active']=False
  if cmd=='disable':s['enabled']=False
  put(s)
  if cmd!='restart':sys.exit(0)
 if cmd=='enable':s['enabled']=True;put(s)
 if cmd in ['start','enable','restart']:
  if s['active']:sys.exit(0)
  code=Path(env['STUB_PREFIX']+'/sbin/scx_lertick').read_text().splitlines()[-1];log('registered')
  if 'BAD_HEALTH' in code or 'BAD_START' in code:
   kf.write_text('disabled');s['active']=False;put(s);log('not-ready');sys.exit(1)
  s['active']=True;s['ops']='scx_lertick';put(s);kf.write_text('enabled');log('ready');sys.exit(0)
 raise SystemExit('unexpected systemctl '+repr(args))
if name=='scx_lertick':
 log('loader',revision=Path(sys.argv[0]).read_text().splitlines()[-1])
 if '--help' in args:sys.exit(0)
 if '--version' in args:print('test-version');sys.exit(0)
 if '--verify' in args:
  log('verify',old_active=get()['active'],maintenance=env.get('STUB_MAINTENANCE')=='1')
  if 'BAD_VERIFY' in Path(sys.argv[0]).read_text().splitlines()[-1]:sys.exit(1)
  sys.exit(0)
 if '--maintenance' in args:
  if 'BAD_MAINTENANCE' in Path(sys.argv[0]).read_text().splitlines()[-1]:sys.exit(1)
  if kf.read_text().strip() not in ['disabled','unavailable']:sys.exit(1)
  log('maintenance');cmd=args[args.index('--')+1:]
  sys.exit(subprocess.run(cmd,env=dict(env,STUB_MAINTENANCE='1')).returncode)
 raise SystemExit('unexpected loader '+repr(args))
raise SystemExit('unexpected tool '+name)
'''
def exe(path,text):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text);path.chmod(0o755)
with tempfile.TemporaryDirectory(prefix='lertick-service-fixtures-') as td:
 w=Path(td).resolve();tree=w/'source';prefix=w/'prefix';tools=w/'tools';tools.mkdir()
 unitdir=w/'etc/systemd/system';systemdrun=w/'run/systemd/system';systemdrun.mkdir(parents=True)
 unit=unitdir/'lertick.service';lockdir=w/'run/lertick-deploy'
 text=(project/'scripts/service_manager.sh').read_text().replace('unit_dir=/etc/systemd/system',f'unit_dir={unitdir}').replace('lock_dir=/run/lertick-deploy',f'lock_dir={lockdir}').replace('[ -d /run/systemd/system ]',f'[ -d {systemdrun} ]')
 exe(tree/'scripts/service_manager.sh',text);exe(tree/'deploy.sh',(project/'deploy.sh').read_text());exe(tree/'build/scx_lertick',mux+'\n# revision one\n');exe(tree/'scripts/native_build.sh',mux)
 exe(tree/'scripts/collect_report.py','#!/usr/bin/env python3\nprint("{}")\n')
 (tree/'templates').mkdir();(tree/'templates/lertick.service.in').write_text((project/'templates/lertick.service.in').read_text())
 (tree/'src/userspace').mkdir(parents=True);(tree/'src/userspace/scx_lertick.c').touch();(tree/'Makefile').touch()
 for name in ['id','uname','stat','cat','sha256sum','flock','cp','systemctl','sudo']:exe(tools/name,mux)
 service=w/'service.json';state=w/'state';log=w/'log';log.touch()
 def reset(active=False,enabled=False,ops='scx_lertick'):
  service.write_text(json.dumps(dict(active=active,enabled=enabled,ops=ops)));state.write_text('enabled' if active else 'disabled')
 reset()
 env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],STUB_SERVICE=str(service),STUB_STATE=str(state),STUB_LOG=str(log),STUB_PREFIX=str(prefix),STUB_UNIT=str(unit),STUB_UID='0')
 def calls():return [json.loads(x) for x in log.read_text().splitlines()]
 def clear():log.write_text('')
 def run(cmd,ok=True,extra=None,bundle=None):
  p=subprocess.run([str(tree/'scripts/service_manager.sh'),str(prefix),cmd,str(bundle or tree)],env=dict(env,**(extra or {})),capture_output=True,text=True,timeout=15)
  assert (p.returncode==0)==ok,(cmd,p.returncode,p.stdout,p.stderr)
  return p
 def marker(name):return (prefix/'lib/lertick'/name).read_text().strip()
 def changed(revision):exe(tree/'build/scx_lertick',mux+'\n# revision '+revision+'\n')
 # Installation prepares files and unit, but preserves the user's disabled/inactive intent.
 run('install');first=marker('current');assert not json.loads(service.read_text())['active'] and not json.loads(service.read_text())['enabled']
 assert not any(c['tool'] in ['ready','registered'] for c in calls())
 unittext=unit.read_text()
 for part in ['Type=notify','NotifyAccess=main','Restart=no','--duration 0','--summary-only','--sample-ms 60000','--notify','NoNewPrivileges=yes','LogRateLimitBurst=100']:
  assert part in unittext,part
 for file in (prefix/'lib/lertick/releases'/first).iterdir():assert not file.is_symlink() and not(file.stat().st_mode&0o222)
 # First enable must proceed even when reset-failed says the new unit is not loaded.
 clear();run('enable',extra={'STUB_RESET_NOT_LOADED':'1'});assert [c['args'][0] for c in calls() if c['tool']=='systemctl'].index('reset-failed')<[c['args'][0] for c in calls() if c['tool']=='systemctl'].index('enable');assert json.loads(service.read_text())['active'] and json.loads(service.read_text())['enabled'];assert [c['tool'] for c in calls()].index('registered')<[c['tool'] for c in calls()].index('ready')
 # Candidate is verified while old service remains active; only then does stop happen.
 changed('two');clear();run('update');second=marker('current');assert second!=first and marker('previous')==first
 cc=calls();verify=next(i for i,c in enumerate(cc) if c['tool']=='verify');stop=next(i for i,c in enumerate(cc) if c['tool']=='systemctl' and c['args'][0]=='stop')
 assert verify<stop and cc[verify]['old_active'];assert not any(c['tool']=='systemctl' and c['args'][0]=='reset-failed' for c in cc);
 assert all('/lib/lertick/releases/' in c['args'][c['args'].index('--')+1] for c in cc if c['tool']=='loader' and '--maintenance' in c['args']);assert json.loads(service.read_text())['active'] and json.loads(service.read_text())['enabled']
 assert all(c['maintenance'] for c in cc if c['tool']=='copy' and c['args'][1].startswith(str(prefix/'sbin')))
 # Same binary with helper-only change is a different complete release.
 with (tree/'deploy.sh').open('a') as f:f.write('\n# helper revision\n')
 run('update');third=marker('current');assert third!=second and marker('previous')==second
 # Failed verification must not stop the working service or change current.
 changed('BAD_VERIFY');clear();run('update',ok=False);assert marker('current')==third
 assert not any(c['tool']=='systemctl' and c['args'][0]=='stop' for c in calls())
 # A registered-but-unhealthy candidate never becomes ready; restore exactly one old bundle.
 changed('BAD_HEALTH');clear();run('update',ok=False);assert marker('current')==third and marker('previous')==second
 assert json.loads(service.read_text())['active'] and json.loads(service.read_text())['enabled']
 assert len([c for c in calls() if c['tool']=='not-ready'])==1 and len([c for c in calls() if c['tool']=='ready'])==1
 # Recovery uses immutable old loader even when candidate maintenance is broken.
 changed('BAD_MAINTENANCE BAD_START');clear();run('update',ok=False)
 assert marker('current')==third and marker('previous')==second
 assert json.loads(service.read_text())['active'] and json.loads(service.read_text())['enabled']
 maint=[c for c in calls() if c['tool']=='loader' and '--maintenance' in c['args']]
 assert len(maint)==2 and all('BAD_MAINTENANCE' not in c['revision'] for c in maint)
 # Foreign or foreground schedulers are not killed, stopped or promoted.
 changed('four');reset(False,True,'other_scheduler');state.write_text('enabled');clear();run('update',ok=False)
 assert marker('current')==third and not any(c['tool']=='systemctl' and c['args'][0] in ['start','stop','restart'] for c in calls())
 reset(False,True);state.write_text('enabled');run('rollback',ok=False)
 # Explicit stop and disable confirm fallback; updates do not start an inactive service.
 reset(True,True);run('stop');assert state.read_text()=='disabled' and json.loads(service.read_text())['enabled']
 changed('four');clear();run('update');fourth=marker('current');assert fourth!=third
 assert not json.loads(service.read_text())['active'] and json.loads(service.read_text())['enabled']
 run('rollback');assert marker('current')==third and not json.loads(service.read_text())['active']
 run('enable');run('disable');assert not json.loads(service.read_text())['active'] and not json.loads(service.read_text())['enabled']
 # An unknown unit is never overwritten; lock contention prevents any service changes.
 old=unit.read_text();unit.write_text('[Service]\nExecStart=/foreign\n');clear();run('update',ok=False);assert unit.read_text().startswith('[Service]')
 unit.write_text(old);clear();run('update',ok=False,extra={'STUB_LOCK_FAIL':'1'});assert not any(c['tool']=='systemctl' and c['args'][0] in ['start','stop','restart'] for c in calls())
 # One failed rollback startup is not retried indefinitely; default scheduler remains.
 changed('BAD_START');run('update');assert not json.loads(service.read_text())['active']
 run('enable',ok=False);assert state.read_text()=='disabled'
 # Remove only our installed files/unit; no host state was accessed.
 run('uninstall');assert not unit.exists() and not(prefix/'sbin/scx_lertick').exists()
print('test_service: notify readiness, long-running intent, immutable bundles, active verification, atomic update/rollback, foreign refusal and default fallback passed (fixtures only)')
PY
