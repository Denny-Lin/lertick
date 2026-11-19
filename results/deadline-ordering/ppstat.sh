#!/bin/bash
# ppstat.sh DIR: prediction accuracy of PRED_PLACE counters per workload.
python3 - "$1" <<'PY'
import re, sys
for f in ["msgproc8","msgthr8","hb16"]:
    t=open("%s/%s.bss"%(sys.argv[1],f)).read()
    g=lambda k: [int(x) for x in re.search(r'"%s":\[([0-9,]*)\]'%k,t).group(1).split(",")]
    n,w,pr,e=g("pp_n"),g("pp_wait_ns"),g("pp_pred_ns"),g("pp_err_ns")
    pk=[int(re.search(r'"%s":([0-9]+)'%k,t).group(1)) for k in ("nr_pp_pick_local","nr_pp_pick_shared")]
    for i,nm in ((1,"local"),(2,"shared")):
        print(f,nm,"n",n[i],"wait %.2f ms pred %.2f ms err %.2f ms"%(w[i]/n[i]/1e6,pr[i]/n[i]/1e6,e[i]/n[i]/1e6))
    print(f,"would pick local/shared",pk)
PY
