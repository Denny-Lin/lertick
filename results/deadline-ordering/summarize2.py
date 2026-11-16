# Per-trial summary: every scalar p99, throughput, fairness and gap field in trial.json,
# plus p99 by burst class from workload.jsonl when it has burst samples.
import json,glob,os,sys
def p99(v):
    v=sorted(v); return v[min(len(v)-1,int(.99*len(v)))] if v else None
arm,out=sys.argv[1],sys.argv[2]
for d in sorted(glob.glob(out+'/r0*-*')):
    if not os.path.isdir(d): continue
    rep,wl=os.path.basename(d).split('-')[:2]
    t=json.load(open(d+'/trial.json'))
    row={'arm':arm,'block':os.path.basename(out),'rep':rep,'workload':wl,'passed':t.get('passed')}
    for k,v in t.items():
        if isinstance(v,(int,float)) and not isinstance(v,bool) and any(s in k for s in ('p99','p95','p50','throughput','jain','gap','completed','rebuild')):
            row[k]=v
    roles={};m={}
    if os.path.exists(d+'/workload.jsonl'):
        for l in open(d+'/workload.jsonl'):
            if not l.strip(): continue
            e=json.loads(l)
            if e.get('event')=='worker': roles[e['tid']]=(e['role'],e['burst_target_ns']//1000)
            elif e.get('event')=='sample' and e.get('role')=='burst': m.setdefault(f"compl{roles[e['tid']][1]}",[]).append(e['completion_ns'])
    if os.path.exists(d+'/wake.jsonl'):
        for l in open(d+'/wake.jsonl'):
            if not l.strip(): continue
            e=json.loads(l)
            if e.get('event')=='wake_run' and e['tid'] in roles and roles[e['tid']][0]=='burst':
                m.setdefault(f"wake{roles[e['tid']][1]}",[]).append(e['latency_ns'])
    row.update({k:p99(v) for k,v in m.items()})
    print(json.dumps(row))
