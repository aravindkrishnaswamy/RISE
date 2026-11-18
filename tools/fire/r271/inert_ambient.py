# r271 non-reacting plume (section 23, read-only): where do FDS's local near-floor fluctuations come from? Local (per cell,
# in time) rms of u, v, w and theta, averaged over radial bands, at near-field layers 0, 1, 3 and 9, 6-14 s, FDS (cell
# averages) against RISE arms. Bands: burner interior, burner edge, just outside, near ambient, far ambient.
# argv[1]: dir with <arm>_near.bin; argv[2:]: arms.
import numpy as np,sys,os
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
exec(open(Path(__file__).with_name('inert_shear.py')).read().split('rad=np.arange')[0])
Rb=np.sqrt(69*DX*DX/np.pi)
B=[('interior',RR<Rb-DX),('edge',np.abs(RR-Rb)<DX/2),('outside',(RR>Rb+DX/2)&(RR<Rb+3*DX)),
   ('near amb 0.25-0.45',(RR>.25)&(RR<.45)),('far amb 0.6-1.0',(RR>.6)&(RR<1.0))]
LK=(0,1,3,9)
def table(T,u,v,w):
    out={}
    for k in LK:
        r=[np.sqrt(q[:,k].var(0)) for q in (u,v,w,T)]
        out[k]=[[x[m].mean() for x in r] for _,m in B]
    return out
res={}
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    res[arm]=table(a[:,:,0],a[:,:,1],a[:,:,2],a[:,:,3]);del a
T,u,v,w=fds_layers(idx,['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY']);res['FDS']=table(T,u,v,w)
print('Local rms, 6-14 s: u v w (m/s) theta (K), band means')
for k in LK:
    print(f'  layer {k} (z {(k+.5)*DX:.3f} m)')
    for bi,(bn,_) in enumerate(B):
        print(f'    {bn:20s}'+''.join(f'   {n[:13]:>13s} '+' '.join(f'{x:.3f}' for x in r[k][bi][:3])+f' {r[k][bi][3]:5.1f}' for n,r in res.items()))
# Horizontal scale of the fluctuations over the burner: 2-D spectra of w' and theta' on the full plane (time-mean removed),
# energy shares by k dx: >=8 dx (<0.25 pi), 4-8 dx, 2.7-4 dx, grid (>=0.75 pi).
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
bands=[(K>0)&(K<.25*np.pi),(K>=.25*np.pi)&(K<.5*np.pi),(K>=.5*np.pi)&(K<.75*np.pi),K>=.75*np.pi]
def shares(p):
    E=(np.abs(np.fft.fft2(p-p.mean(0)))**2).mean(0);t=E[K>0].sum();return [E[b].sum()/t for b in bands]
def sp(T,w):return {k:(shares(w[:,k]),shares(T[:,k])) for k in LK}
sres={}
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    sres[arm]=sp(a[:,:,0],a[:,:,3]);del a
sres['FDS']=sp(T,w)
print("\nHorizontal spectral shares of w' | theta' : >=8dx 4-8dx 2.7-4dx grid")
for k in LK:
    print(f'  layer {k}')
    for n,r in sres.items():print(f'    {n:22s} '+' '.join(f'{x:.2f}' for x in r[k][0])+'  | '+' '.join(f'{x:.2f}' for x in r[k][1]))
