# r271 section 26: momentum advection through a variable-density band (vd_state.py). v = A cos(k x') carried by u = U
# while T = 300-600 K varies along x (8 cells per wavelength); RISE (x<arm>.log vc, vs) against FDS (fds<arm> V slices),
# same projection as am_analyze.py (y-face v over z 1.9-2.7 m, x cells 10-58). Decay per cell crossed (molecular share
# removed with nu at 300 K) and phase speed / U, fitted from t >= 0.02 s. argv: RISE logs and FDS dirs as label=path.
import numpy as np,re,struct,sys,math
from pathlib import Path
N=69;H=.030593116067411014;U=0.5;NU=1.57e-5;n=4;k=2*math.pi/(n*H)
def rise(p):
    a=[[float(v) for v in re.findall(r' (?:t|vc|vs)=([-0-9.eE+]+)',l)] for l in Path(p).read_text().splitlines() if l.startswith('R271_BT step=')]
    a=np.array([r for r in a if len(r)==3]);_,i=np.unique(a[:,0],return_index=True);a=a[np.sort(i)];return a[:,0],a[:,1],a[:,2]
def fds(run):
    run=Path(run);sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for i,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/H-.5))),i)
    x=(np.arange(N)+.5)*H-34.5*H;c=np.cos(k*x);s=np.sin(k*x);pc=0;ps=0;c2=0;s2=0
    for kz in range(106):
        if not 1.9<=(kz+.5)*H<=2.7:continue
        with (run/f'xp_1_{idx[("V-VELOCITY",kz)]}.sf').open('rb') as f:
            for _ in range(4):m=struct.unpack('<i',f.read(4))[0];f.seek(m+4,1)
            ts=[];fs=[]
            while True:
                h=f.read(4)
                if len(h)<4:break
                ts.append(struct.unpack('<f',f.read(4))[0]);f.read(4);m=struct.unpack('<i',f.read(4))[0];fs.append(np.frombuffer(f.read(m),dtype='<f4').reshape(70,70,order='F'));f.read(4)
        v=np.array(fs,dtype=np.float64)[:,1:,:][:,10:59,10:59]
        pc=pc+(v*c[10:59,None]).sum((1,2));ps=ps+(v*s[10:59,None]).sum((1,2));c2+=(c[10:59]**2).sum()*49;s2+=(s[10:59]**2).sum()*49
    return np.array(ts),pc/c2,ps/s2
print(f'n={n}: decay per cell crossed and c/U (central reference c/U = {math.sin(k*H)/(k*H):.3f}); amplitude at t = 0.1/0.2/0.3 s over A')
for arg in sys.argv[1:]:
    label,p=arg.split('=',1);t,vc,vs=(rise if p.endswith('.log') else fds)(p)
    s=t>=0.02;amp=np.hypot(vc,vs);ph=np.unwrap(np.arctan2(vs,vc));tt=t[s];cells=U*(tt-tt[0])/H
    d=-np.polyfit(cells,np.log(amp[s])+NU*k*k*(tt-tt[0]),1)[0];cu=np.polyfit(tt,ph[s],1)[0]/(k*U)
    at=' '.join(f'{np.interp(x,t,amp)/amp[0]:.3f}' for x in (.1,.2,.3) if x<=t[-1]+1e-9)
    print(f'  {label:28s} decay {d:8.4f}  c/U {cu:6.3f}  over {cells[-1]:4.1f} cells | amp/A0 {at}')
