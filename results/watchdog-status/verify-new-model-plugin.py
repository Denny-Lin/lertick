#!/usr/bin/env python3
"""Isolated QEMU integration, not an accuracy or throughput benchmark."""
import argparse,hashlib,json,os,pathlib,re,signal,subprocess,sys,time
SCX=pathlib.Path('/home/ubuntu/lertick-burst-init/build/scx_lertick')
LW=pathlib.Path('/home/ubuntu/lerwatt-temporal-forecast')
STATE=pathlib.Path('/sys/kernel/sched_ext/state')
sys.path.insert(0,str(LW/'tests'))
from verify_runtime_forecast import knobs,stop

def save(p,a):p.write_text(json.dumps(a,indent=2)+'\n')
def rows(p):return [json.loads(v) for v in p.read_text().splitlines() if v.startswith('{')]
def wait_for(predicate,process):
 end=time.monotonic()+15
 while not predicate():
  if process.poll() is not None or time.monotonic()>end:raise RuntimeError('readiness failed')
  time.sleep(.05)
def final_exit(p):
 r=[r for r in rows(p) if r.get('event')=='exit']
 assert len(r)==1 and r[0]['default_restored'] and not r[0]['unexpected_fallback'],r
 assert r[0]['watchdog_expected'] is False and r[0]['watchdog_triggered'] is False
 return r[0]
def standalone(out,name):
 d=out/name;d.mkdir()
 with (d/'stderr.log').open('w') as f:
  subprocess.run([str(SCX),'--duration','3','--stats-json',str(d/'stats.jsonl')],stderr=f,stdout=f,check=True,timeout=30)
 assert STATE.read_text().strip()=='disabled'
 assert not pathlib.Path('/sys/fs/bpf/lertick/lw_contract').exists()
 save(d/'summary.json',{'passed':True,'exit':final_exit(d/'stats.jsonl'),'defaults_used':True})

def integration(out):
 d=out/'three-cluster';d.mkdir();root=d/'root'
 evidence={'completed':False,'duration_s':250,'scope':'One simulated board and real BPF/daemon integration; no accuracy or throughput claim.'}
 logs=[];workers=[];sim=daemon=scx=None;original=None
 env=dict(os.environ,LERWATT_SIM_ROOT=str(root))
 def launch(args,name):
  f=(d/name).open('w');logs.append(f)
  return subprocess.Popen([str(x) for x in args],stdout=f,stderr=f,env=env)
 try:
  sim=launch([LW/'build/lerwatt-sim','--root',root,'--load','procstat','--cooling','passive','--ambient','25','--initial-temp','25','--duration','280','--log',d/'truth.jsonl'],'sim.log')
  wait_for(lambda:(root/'.lerwatt-sim').exists(),sim)
  original=knobs(root);assert len(original)==30;save(d/'original-knobs.json',original)
  daemon=launch([LW/'build/lerwattd','--sysroot',root,'--policy','mpc','--forecast','temporal','--cross-cluster','--duration','250','--telemetry',d/'telemetry.jsonl'],'daemon.log')
  wait_for(lambda:(d/'telemetry.jsonl').exists(),daemon)
  start=time.monotonic()+.3
  code='''import sys,time
start=float(sys.argv[1]);x=1
while time.monotonic()<start:time.sleep(.01)
while time.monotonic()<start+270:
 elapsed=time.monotonic()-start;cycle=int(elapsed)
 duty=.3 if int(elapsed/15)%2==0 else .9
 end=start+cycle+duty
 while time.monotonic()<end:x=(x*1664525+1013904223)&0xffffffff
 time.sleep(max(0,start+cycle+1-time.monotonic()))
'''
  workers=[launch(['taskset','-c',cpu,'python3','-c',code,str(start)],f'worker{cpu}.log') for cpu in range(8)]
  scx=launch([SCX,'--lerwatt','--track-all','--model-init','ewma','--model-update','carry','--duration','240','--sample-ms','200','--stats-json',d/'scx.jsonl'],'scx.log')
  wait_for(lambda:(d/'scx.jsonl').exists() and '"event":"registered"' in (d/'scx.jsonl').read_text(),scx)
  dump_at=time.monotonic()+230
  while time.monotonic()<dump_at:
   if daemon.poll() is not None or scx.poll() is not None or sim.poll() is not None:raise RuntimeError('unexpected early exit')
   time.sleep(.5)
  maps={}
  for name in ('lw_contract','lt_demand'):
   for attempt in range(10):
    result=subprocess.check_output(['bpftool','-j','map','dump','pinned','/sys/fs/bpf/lertick/'+name],text=True)
    (d/f'{name}-{attempt}.json').write_text(result)
    decoded=[r.get('formatted',r) for r in json.loads(result)]
    assert all(isinstance(r['key'],int) and isinstance(r['value'],dict) for r in decoded), 'map dump needs semantic BTF fields'
    records={r['key']:r['value'] for r in decoded}
    if all(records[k]['version']==1 and records[k]['seq']>0 and records[k]['seq']%2==0 for k in range(3)):
     maps[name]=[{'key':k,'value':records[k]} for k in range(3)];break
    time.sleep(.05)
   else:raise RuntimeError('no consistent published map snapshot')
  assert all(0<r['value']['pred_util']<=1024 for r in maps['lt_demand'])
  assert scx.wait(timeout=40)==0
  assert daemon.wait(timeout=40)==0
  evidence['restored_knobs']=knobs(root)==original
  evidence['journal_removed']=not (root/'run/lerwatt/restore.journal').exists()
  stop(sim)
  t=rows(d/'telemetry.jsonl');s=rows(d/'scx.jsonl')
  registered=next(r for r in s if r.get('event')=='registered')
  assert registered['model_init']=='ewma' and registered['model_update']=='carry'
  topology=registered['lerwatt_topology'];assert topology['clusters']==[[0,1,2,3],[4,5],[6,7]] and topology['source']=='sim'
  snapshots=[r for r in s if r.get('event')=='snapshot']
  cpuutil={cpu:max(c['util'] for row in snapshots for c in row['cpus'] if c['cpu']==cpu) for cpu in range(8)}
  simultaneous=sum(all(c['busy']>.5 for c in row['clusters']) for row in t)
  assert simultaneous>0 and all(v>100 for v in cpuutil.values())
  assert snapshots[-1]['stats']['contract_applied']>0
  assert STATE.read_text().strip()=='disabled' and not pathlib.Path('/sys/fs/bpf/lertick/lw_contract').exists() and not pathlib.Path('/sys/fs/bpf/lertick/lt_demand').exists()
  stderr=(d/'sim.log').read_text();throttle=re.search(r'(\d+) throttle steps',stderr);bad=re.search(r'(\d+) bad writes',stderr)
  peaks=re.search(r'peak ([\d.]+) / ([\d.]+) / ([\d.]+) C',stderr)
  assert sim.returncode==0 and throttle and int(throttle[1])==0 and bad and int(bad[1])==0 and peaks and max(map(float,peaks.groups()))<=84
  writes=re.search(r'(\d+) write errors',(d/'daemon.log').read_text())
  assert writes and int(writes[1])==0
  assert evidence['restored_knobs'] and evidence['journal_removed']
  evidence.update(completed=True,topology=topology,cpu_util_max=cpuutil,simultaneous_busy_periods=simultaneous,joint_periods=sum(any(c['joint'] for c in row['clusters']) for row in t),temporal_points=sum(c['forecast_source']=='temporal' for row in t for c in row['clusters']),maps=maps,exit=final_exit(d/'scx.jsonl'),final_stats=snapshots[-1]['stats'],simulator_summary=stderr.splitlines()[-1])
 except BaseException as error:
  evidence['failure']=repr(error);raise
 finally:
  stop(daemon);stop(scx)
  for w in workers:stop(w)
  stop(sim)
  if original is not None:
   evidence['restored_knobs']=knobs(root)==original;evidence['journal_removed']=not (root/'run/lerwatt/restore.journal').exists()
   if not evidence['restored_knobs'] or not evidence['journal_removed']:evidence['completed']=False
  evidence.update(daemon_exit=daemon.returncode if daemon else None,scx_exit=scx.returncode if scx else None,simulator_exit=sim.returncode if sim else None)
  save(d/'summary.json',evidence)
  for f in logs:f.close()
 return evidence

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
 assert os.geteuid()==0 and os.cpu_count()==8 and STATE.read_text().strip()=='disabled'
 assert subprocess.check_output(['systemd-detect-virt','--vm'],text=True).strip()=='qemu'
 for d in pathlib.Path('/proc').iterdir():
  if not d.name.isdigit():continue
  try:name=pathlib.Path((d/'cmdline').read_bytes().split(b'\0')[0].decode()).name
  except (OSError,UnicodeError):continue
  assert name not in ('lerwattd','lerwatt-sim')
 a.output.mkdir(parents=True,exist_ok=False)
 save(a.output/'environment.json',{'kernel':os.uname().release,'cpus':8,'sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [SCX,LW/'build/lerwattd',LW/'build/lerwatt-sim',pathlib.Path(__file__)]}})
 standalone(a.output,'standalone-before');r=integration(a.output);standalone(a.output,'standalone-after');save(a.output/'summary.json',{'passed':True,'integration':r,'standalone_before_and_after':True});print('PASS three-cluster model/plugin integration and both default standalone runs',flush=True)
if __name__=='__main__':main()
