# r271 section 25: Rayleigh-Taylor growth, RISE (rt_n<n>/checkpoint_history dumped by ckpt_layers, 90 layers) against FDS
# (fdsrt_n<n> slices). Both: cell-centred w (two-face average) and T over cells z 1.9-2.7 m, x,y 10..58; each layer's core
# mean is removed before projecting onto cos(k x') (a uniform field projects onto cos over 49 cells with weight ~1/24, so
# the layer's bulk T and w would otherwise leak in). argv: n values; RISE dumps at r271_small_scale/rt_n<n>.bin.
import numpy as np,re,struct,sys
from pathlib import Path
B=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale');N=69;H=.030593116067411014
def proj(k,T,w,zs):
    x=(np.arange(N)+.5)*H-34.5*H;phi=np.cos(k*x)[:,None]*np.ones(N)[None,:];core=np.zeros((N,N),bool);core[10:59,10:59]=True
    pw=0;pt=0;nn=0
    for i in zs:
        ww=w[:,i][:,core];tt=T[:,i][:,core];ww=ww-ww.mean(1,keepdims=True);tt=tt-tt.mean(1,keepdims=True)
        pw=pw+(ww*phi[core]).sum(1);pt=pt+(tt*phi[core]).sum(1);nn+=(phi[core]**2).sum()
    return pw/nn,pt/nn
def rise(n):
    p=B/f'rt_n{n}.bin';t=np.loadtxt(str(p)+'.times',ndmin=1);a=np.fromfile(p,dtype='<f4').reshape(len(t),90,4,N,N)
    a=np.transpose(a,(0,1,2,4,3)).astype(np.float64)  # [t][z][c][x][y]
    zs=[i for i in range(90) if 1.9<=(i+.5)*H<=2.7];Aw,ac=proj(2*np.pi/(n*H),a[:,:,0],a[:,:,3],zs)
    return np.r_[0,t],np.r_[0,Aw],np.r_[np.nan,ac]
def fds(n):
    run=B/f'fdsrt_n{n}';k=2*np.pi/(n*H)
    sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for i,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m:idx.setdefault((q,int(round(float(m.group(1))/H-.5))),i)
    def rd(q,kz):
        with (run/f'xp_1_{idx[(q,kz)]}.sf').open('rb') as f:
            for _ in range(4):m=struct.unpack('<i',f.read(4))[0];f.seek(m+4,1)
            ts=[];fs=[]
            while True:
                h=f.read(4)
                if len(h)<4:break
                ts.append(struct.unpack('<f',f.read(4))[0]);f.read(4);m=struct.unpack('<i',f.read(4))[0];fs.append(np.frombuffer(f.read(m),dtype='<f4').reshape(70,70,order='F')[1:,1:]);f.read(4)
        return np.array(ts),np.array(fs,dtype=np.float64)
    zs=[kz for kz in range(106) if 1.9<=(kz+.5)*H<=2.7];T=[];W=[]
    for kz in range(106):
        if kz in zs:
            t,a=rd('TEMPERATURE',kz);_,top=rd('W-VELOCITY',kz);_,bot=rd('W-VELOCITY',kz-1);T.append(a+273.15);W.append(.5*(top+bot))
    T=np.stack(T,1);W=np.stack(W,1);Aw,ac=proj(k,T,W,range(len(zs)));return t,Aw,ac
for n in map(int,sys.argv[1:]):
    print(f'== n={n}: time, w-mode amplitude Aw (m/s), T-mode amplitude ac (K); growth rate of |Aw| fitted where 0.002<|Aw|<0.1 m/s')
    res={}
    for name,f in (('RISE',rise),('FDS',fds)):
        try:t,Aw,ac=f(n)
        except Exception as e:print('  ',name,'missing',e);continue
        res[name]=(t,Aw,ac);s=(abs(Aw)>.002)&(abs(Aw)<.1)
        g=np.polyfit(t[s],np.log(abs(Aw[s])),1)[0] if s.sum()>3 else np.nan
        print(f'  {name}: growth {g:.2f} 1/s over t {t[s].min() if s.any() else 0:.3f}-{t[s].max() if s.any() else 0:.3f} s')
    for tt in (0.02,0.05,0.1,0.15,0.2,0.25,0.3,0.35,0.4):
        print(f'   t={tt:.2f} '+'   '.join(f'{nm}: Aw {Aw[np.argmin(abs(t-tt))]:8.4f} ac {ac[np.argmin(abs(t-tt))]:7.2f}' for nm,(t,Aw,ac) in res.items() if abs(t[np.argmin(abs(t-tt))]-tt)<.01))
