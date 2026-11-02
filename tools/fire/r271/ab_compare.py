# r271 A/B: compact viscous stencil (floor on; optional third arm sc_* = compact scalar nu_t too) vs r270 over the same 64 frames (15.03-18.97 s).
# Run from the scratch dir that holds meander.py/fds_mean3d.npy/fds_hrr_z.npy (tools/fire/r270 copies) and the r270
# 235-frame extracts; argv[1] is the A/B extract dir (ab_*, r270_*, r270w0_*, r270w2_* from dump/mean3d/dump_hrr).
# r270 windows 0 (10.47-14.40 s) and 2 (19.03-22.97 s) show how much a 4 s window varies with no change at all.
import numpy as np,sys,os
W=sys.argv[1];os.environ['PLANES']='r270_planes.bin'
exec(open('meander.py').read().split("xy=(np.arange")[0])
N=69;NZ=106;A=DX*DX;cp=1005;z=(np.arange(NZ)+.5)*DX
Full=np.transpose(np.fromfile('r270_planes.bin',dtype='<f4').reshape(235,8,4,69,69),(0,1,2,4,3))
Hfull=np.fromfile('r270_hrr.bin').reshape(235,109)
def planes(n):return np.transpose(np.fromfile(f'{W}/{n}_planes.bin',dtype='<f4').reshape(64,8,4,69,69),(0,1,2,4,3))
P={'r270 w0':Full[0:64],'r270 w2':Full[137:201],'r270':planes('r270'),'compact':planes('ab')}
if os.path.exists(f'{W}/sc_planes.bin'):P['compact+scalar']=planes('sc')
H={'r270 w0':Hfull[0:64],'r270 w2':Hfull[137:201],'r270':np.fromfile(f'{W}/r270_hrr.bin').reshape(64,109),'compact':np.fromfile(f'{W}/ab_hrr.bin').reshape(64,109)}
if 'compact+scalar' in P:H['compact+scalar']=np.fromfile(f'{W}/sc_hrr.bin').reshape(64,109)
assert np.array_equal(P['r270'],Full[73:137]) and np.array_equal(H['r270'],Hfull[73:137])
kx=np.fft.fftfreq(69)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def share(p):
    p=p-p.mean(0);E=(np.abs(np.fft.fft2(p))**2).mean(0);return E[K>=.75*np.pi].sum()/E[K>0].sum()
def lap(p):
    p=p-p.mean(0);c=p[:,1:-1,1:-1]-.25*(p[:,2:,1:-1]+p[:,:-2,1:-1]+p[:,1:-1,2:]+p[:,1:-1,:-2]);return np.sqrt((c**2).mean()/(p**2).mean())
print('1. Grid-scale share of fluctuation energy (kD >= 0.75 pi) and Laplacian/rms index')
print('   z(m) qty  '+'  '.join(f'{n:>15s}' for n in P))
for k,zz in enumerate(Z[:6]):
    for c,q in ((3,'w'),(0,'T')):
        print(f'  {(zz+.5)*DX:5.2f}  {q}   '+'  '.join(f'{share(p[:,k,c]):6.3f} / {lap(p[:,k,c]):5.2f}' for p in P.values()))
def band(x,a,b,dt=.0625):
    y=x-x.mean();Y=np.fft.rfft(y);f=np.fft.rfftfreq(len(y),dt);Y[(f<a)|(f>=b)]=0;return np.sqrt(np.mean(np.fft.irfft(Y,len(y))**2))
def peak(x,dt=.0625):
    y=(x-x.mean())*np.hanning(len(x));Y=np.abs(np.fft.rfft(y,8192))**2;f=np.fft.rfftfreq(8192,dt);m=(f>1)&(f<5);return f[m][Y[m].argmax()]
print('\n2. Puffing from total and centre-column heat release (rms as % of mean; 4 s window, spectral peak by padded periodogram)')
print('   signal   run          mean kW  rms %  2-3.5 Hz %  3.5-8 Hz %  peak Hz')
for s,i in (('total',0),('column',1)):
    for n,h in H.items():
        x=h[:,i];m=x.mean();print(f'   {s:7s}  {n:10s}  {m/1e3:7.2f}  {100*x.std()/m:5.1f}   {100*band(x,2,3.5)/m:6.1f}      {100*band(x,3.5,8)/m:6.1f}     {peak(x):.2f}')
print('\n3. Heat-release heights (m below which 50/90/99% of the mean heat is released)')
fds=np.load('fds_hrr_z.npy')
def heights(l):c=np.cumsum(l)/l.sum();return [z[np.searchsorted(c,q)]+.5*DX for q in (.5,.9,.99)]
print('   FDS r263 (full window)  '+'  '.join(f'{v:.3f}' for v in heights(fds)))
for n,h in H.items():print(f'   {n:22s}  '+'  '.join(f'{v:.3f}' for v in heights(h[:,3:].mean(0))))
F=np.load('fds_mean3d.npy')
M={'FDS (full)':(F[3],F[2],F[8])}
for n,f in (('r270 w0','r270w0'),('r270 w2','r270w2'),('r270','r270'),('compact','ab'),('compact+scalar','sc'))[:5 if 'compact+scalar' in P else 4]:
    Q=np.fromfile(f'{W}/{f}_mean3d.bin').reshape(7,NZ,N,N).transpose(0,1,3,2);M[n]=(Q[0],Q[3],352.6*Q[6])
print('\n4. Mean plume by height: centre excess T (K), centre w (m/s), upward mass flux (kg/s), temperature width b_T (m)')
print('   z(m)  '+''.join(f'{n:>27s}' for n in M))
for k in [3,9,16,24,32,49,65,98]:
    row=[]
    for T,w,rw in M.values():
        th=T[k]-300;thc=th[33:36,33:36].mean();wc=w[k][33:36,33:36].mean()
        bT=np.sqrt(th[th>0].sum()*A/(np.pi*thc));up=np.clip(rw[k],0,None).sum()*A
        row.append(f'{thc:6.0f} {wc:4.2f} {up:6.4f} {bT:5.3f}')
    print(f'  {z[k]:5.2f}  '+'  '.join(f'{r:>25s}' for r in row))
