#!/usr/bin/env python3
"""r271 section 29: exact MAC velocity divergence of the paired runs, by the control's local temperature.
RISE: ckpt_state files (xp_state.py layout, faces) <dir>/<v>_<ctrl|e1>_<i>.bin; FDS: run dirs' U/V/W face slices (xp.fds).
Layers 1..64 (layer 0 needs the burner face), r < 0.25 m. Per bin: rms of the control's divergence D (1/s) and of the
perturbed-minus-control dD, at matched times. argv: RISE state dir, variant (prod|nosgs), FDS ctrl dir, FDS pert dir, indices."""
import numpy as np,sys,os,re,struct
from pathlib import Path
N=69;NZ=106;DX=.030593116067411014;TB=[300,350,600,900,2000];K0,K1=1,65
xy=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='xy');DISK=np.hypot(X,Y)<.25  # [y][x]
def rise(p):
    a=np.fromfile(p,dtype='<f8');nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
    T=a[:nc].reshape(NZ,N,N);U=a[nc:nc+nu].reshape(NZ,N,N+1);V=a[nc+nu:nc+2*nu].reshape(NZ,N+1,N);W=a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N)
    D=(U[:,:,1:]-U[:,:,:-1]+V[:,1:]-V[:,:-1]+W[1:]-W[:-1])/DX;return T[K0:K1],D[K0:K1]
def fdsread(run):
    sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    def frames(q,k):
        out=[];ts=[]
        with (run/f'xp_1_{idx[(q,k)]}.sf').open('rb') as f:
            for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
            while True:
                h=f.read(4)
                if len(h)<4:break
                tt=struct.unpack('<f',f.read(4))[0];f.read(4);hn=f.read(4)
                if len(hn)<4:break
                n=struct.unpack('<i',hn)[0];d=f.read(n)
                if len(d)<n:break
                out.append(np.frombuffer(d,dtype='<f4').reshape((70,70),order='F').astype(np.float64).T);f.read(4);ts.append(tt)  # .T -> [j][i]
        return np.array(ts),np.array(out)
    t=frames('TEMPERATURE',0)[0];m=len(t)
    T=np.stack([frames('TEMPERATURE',k)[1][:m,1:,1:]+273.15 for k in range(K0,K1)],1)
    U=np.stack([frames('U-VELOCITY',k)[1][:m,1:,:] for k in range(K0,K1)],1)      # [t][k][j][i face]
    V=np.stack([frames('V-VELOCITY',k)[1][:m,:,1:] for k in range(K0,K1)],1)      # [t][k][j face][i]
    W=np.stack([frames('W-VELOCITY',k)[1][:m,1:,1:] for k in range(K0-1,K1)],1)   # top faces of layers K0-1..K1-1
    D=(U[...,1:]-U[...,:-1]+V[:,:,1:]-V[:,:,:-1]+W[:,1:]-W[:,:-1])/DX;return t,T,D
def stats(T,Dc,dD):
    o=[]
    for a,b in zip(TB[:-1],TB[1:]):
        m=(T>=a)&(T<b)&DISK[None];o.append((np.sqrt((Dc[m]**2).mean()),np.sqrt((dD[m]**2).mean())))
    return o
d,v,fc,fp=sys.argv[1],sys.argv[2],Path(sys.argv[3]),Path(sys.argv[4]);idx=sys.argv[5:]
tr=[float(x) for x in open(os.path.join(os.path.dirname(d.rstrip('/')),f'H_{v}_ctrl.bin.times'))]
tf,Tf,Df=fdsread(fc);_,_,Dp=fdsread(fp)
print(f'{v}: rms D of the control / rms dD (pert - ctrl), 1/s, per control-temperature bin, layers {K0}-{K1-1}, r<0.25 m')
print('  code  t s  '+''.join(f'{f"{a}-{b}K":>22s}' for a,b in zip(TB[:-1],TB[1:])))
for i in idx:
    t=tr[int(i)-1];T,Dc=rise(f'{d}/{v}_ctrl_{i}.bin');_,Dq=rise(f'{d}/{v}_e1_{i}.bin')
    print(f'  RISE {t:5.3f}'+''.join(f'   {a:8.3g} / {b:<9.3g}' for a,b in stats(T,Dc,Dq-Dc)))
    j=int(np.argmin(abs(tf-t)))
    print(f'  FDS  {tf[j]:5.3f}'+''.join(f'   {a:8.3g} / {b:<9.3g}' for a,b in stats(Tf[j],Df[j],Dp[j]-Df[j])))
