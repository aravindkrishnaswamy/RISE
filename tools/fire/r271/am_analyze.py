# r271 section 25: momentum advection at 3-6 cells. v = A cos(k x') carried by u = U, RISE (am_n<n>.log: vc, vs projections of
# the y-face v over z 1.9-2.7 m, cells 10-58) against FDS (fdsam_n<n> V slices, same projection). Amplitude decay per cell
# crossed (molecular share removed) and phase speed / U; reference for 2nd-order central advection: no decay,
# c/U = sin(k dx)/(k dx). argv: n values.
import numpy as np,re,struct,sys,math
from pathlib import Path
B=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale');N=69;H=.030593116067411014;U=0.5;NU=1.57e-5
def rise(n):
    a=[[float(v) for v in re.findall(r' (?:t|vc|vs)=([-0-9.eE+]+)',l)] for l in (B/f'am_n{n}.log').read_text().splitlines() if l.startswith('R271_BT step=')]
    a=np.array([r for r in a if len(r)==3]);_,i=np.unique(a[:,0],return_index=True);a=a[np.sort(i)];return a[:,0],a[:,1],a[:,2]
def fds(n):
    run=B/f'fdsam_n{n}';k=2*np.pi/(n*H)
    sl=[l for l in (run/'am.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for i,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m:idx.setdefault((q,int(round(float(m.group(1))/H-.5))),i)
    x=(np.arange(N)+.5)*H-34.5*H;c=np.cos(k*x);s=np.sin(k*x);pc=0;ps=0;c2=0;s2=0
    for kz in range(106):
        if not 1.9<=(kz+.5)*H<=2.7:continue
        with (run/f'am_1_{idx[("V-VELOCITY",kz)]}.sf').open('rb') as f:
            for _ in range(4):m=struct.unpack('<i',f.read(4))[0];f.seek(m+4,1)
            ts=[];fs=[]
            while True:
                h=f.read(4)
                if len(h)<4:break
                ts.append(struct.unpack('<f',f.read(4))[0]);f.read(4);m=struct.unpack('<i',f.read(4))[0];fs.append(np.frombuffer(f.read(m),dtype='<f4').reshape(70,70,order='F'));f.read(4)
        v=np.array(fs,dtype=np.float64)[:,1:,:][:,10:59,10:59]  # x cells 10..58, y faces 10..58
        pc=pc+(v*c[10:59,None]).sum((1,2));ps=ps+(v*s[10:59,None]).sum((1,2));c2+=(c[10:59]**2).sum()*49;s2+=(s[10:59]**2).sum()*49
    return np.array(ts),pc/c2,ps/s2
print('   n   code   decay per cell crossed   c/U     (central reference: decay 0, c/U = sin(k dx)/(k dx))')
for n in map(int,sys.argv[1:]):
    k=2*math.pi/(n*H)
    for name,f in (('RISE',rise),('FDS',fds)):
        try:t,vc,vs=f(n)
        except Exception as e:print(f'  {n:2d}   {name}  missing ({e})');continue
        s=t>=0.02;amp=np.hypot(vc,vs)[s];ph=np.unwrap(np.arctan2(vs,vc))[s];t=t[s];cells=U*(t-t[0])/H
        d=-np.polyfit(cells,np.log(amp)+NU*k*k*(t-t[0]),1)[0];cu=np.polyfit(t,ph,1)[0]/(k*U)
        print(f'  {n:2d}   {name:4s}   {d:8.4f}   over {cells[-1]:4.1f} cells   {cu:6.3f}     ref c/U {math.sin(k*H)/(k*H):.3f}')
