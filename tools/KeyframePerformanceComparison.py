#!/usr/bin/env python3
"""Interleaved DL-532 cross-build hashes and timing; uses unchanged child harness."""
import argparse,json,math,os,pathlib,statistics,subprocess
p=argparse.ArgumentParser()
p.add_argument('baseline');p.add_argument('current');p.add_argument('--output',required=True)
p.add_argument('--n',type=int,default=8);p.add_argument('--spp',type=int,default=8)
a=p.parse_args()
if a.n<8 or a.spp<2: p.error('require n>=8 and spp>=2')
root=pathlib.Path(a.output).resolve();root.parent.mkdir(parents=True,exist_ok=True)
bins=[str(pathlib.Path(x).resolve()) for x in (a.baseline,a.current)]
rows=[]
def child(build,scene,ras,salt,index):
    out=str(root)+'.%s.%s.%s.%d'%(build,scene,ras,index)
    with open(out+'.log','w') as f:
        r=subprocess.run([bins[build],'--child',scene,ras,'1','1',str(salt),str(a.spp),out],stdout=f,stderr=subprocess.STDOUT)
    if r.returncode: raise RuntimeError('child failed: '+out)
    fields=pathlib.Path(out).read_text().split()
    ok,h,mean,seconds,frames=int(fields[0]),int(fields[1]),float(fields[2]),float(fields[3]),int(fields[4])
    if ok!=1 or frames<1 or not math.isfinite(mean) or not math.isfinite(seconds) or seconds<=0:
        raise RuntimeError('invalid child: '+out)
    return dict(hash=h,mean=mean,seconds=seconds)
# Four distinct salts, both camera cases, all six existing RGB/spectral fixtures.
for scene in ('combo','ext_combo'):
    for ras in ('pt','pts','bdpt','bdpts','vcm','vcms'):
        for i in range(4):
            results={b:child(b,scene,ras,9100+i,i) for b in ((0,1) if i%2==0 else (1,0))}
            if results[0]['hash']!=results[1]['hash']: raise RuntimeError('pixel hash changed: '+scene+' '+ras+' '+str(i))
            rows.append(dict(scene=scene,ras=ras,salt=9100+i,baseline=results[0],current=results[1]))
print('cross-build pixel hashes: 48/48 identical',flush=True)
timings=[]
for i in range(a.n):
    r={b:child(b,'combo_big','pt',9300+i,100+i) for b in ((0,1) if i%2==0 else (1,0))}
    if r[0]['hash']!=r[1]['hash']:raise RuntimeError('benchmark hash changed')
    timings.append(dict(salt=9300+i,baseline=r[0],current=r[1],speedup=r[0]['seconds']/r[1]['seconds']))
    print('pair',i,r[0]['seconds'],r[1]['seconds'],flush=True)
def stats(xs):return dict(mean=statistics.mean(xs),sd=statistics.stdev(xs),n=len(xs))
summary=dict(hashes=rows,timings=timings,baseline=stats([r['baseline']['seconds'] for r in timings]),
             current=stats([r['current']['seconds'] for r in timings]),speedup=stats([r['speedup'] for r in timings]))
root.write_text(json.dumps(summary,indent=2));print(json.dumps({k:summary[k] for k in ('baseline','current','speedup')},indent=2))
