# r271 section 36: a reacting run longer than the 4 s A/B window, split into time windows.
# Run from the scratch dir holding fds_mean3d.npy and fds_hrr_z.npy (tools/fire/r270 products).
# argv[1]: dir with <arm>_planes.bin (dump.cpp: [frame][8 layers][T,u,v,w][y][x]) and <arm>_hrr.bin (dump_hrr.cpp:
# [frame][109], total, column, ?, then 106 layers); argv[2]: arm prefix; argv[3]: frame count; argv[4:]: windows a-b
# in seconds from the first frame (frames are 1/16 s apart). Prints, per window: centre excess T, centre w, upward
# mass flux and temperature width against FDS (full window) and McCaffrey's centreline excess T; heat-release
# heights; grid-scale share of w' (k dx >= 0.75 pi); then the mass flux in consecutive 2 s blocks.
import numpy as np,sys
W,ARM,NF=sys.argv[1],sys.argv[2],int(sys.argv[3]);WIN=[tuple(float(v) for v in w.split('-')) for w in sys.argv[4:]]
N=69;NZ=106;DX=0.030593116067411014;A=DX*DX;DT=.0625;ZK=[9,16,24,32,49,65,81,98];z=(np.arange(NZ)+.5)*DX
MC={9:873,16:551,24:367,32:247,49:122,65:76,98:39}
P=np.transpose(np.fromfile(f'{W}/{ARM}_planes.bin',dtype='<f4').reshape(NF,8,4,N,N),(0,1,2,4,3)).astype(np.float64)
H=np.fromfile(f'{W}/{ARM}_hrr.bin').reshape(NF,109);F=np.load('fds_mean3d.npy');fh=np.load('fds_hrr_z.npy')
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def share(p):
    p=p-p.mean(0);E=(np.abs(np.fft.fft2(p))**2).mean(0);return E[K>=.75*np.pi].sum()/E[K>0].sum()
def row(T,w,rw):
    th=T-300;thc=th[33:36,33:36].mean();return thc,w[33:36,33:36].mean(),np.clip(rw,0,None).sum()*A,np.sqrt(th[th>0].sum()*A/(np.pi*thc))
def sl(a,b):return slice(int(round(a/DT)),int(round(b/DT)))
def heights(l):c=np.cumsum(l)/l.sum();return [z[np.searchsorted(c,q)]+.5*DX for q in (.5,.9,.99)]
fds=[row(F[3][k],F[2][k],F[8][k]) for k in ZK]
R={w:[row(P[sl(*w),i,0].mean(0),P[sl(*w),i,3].mean(0),(352.6*P[sl(*w),i,3]/P[sl(*w),i,0]).mean(0)) for i in range(8)] for w in WIN}
names=['FDS (full)']+[f'{a:g}-{b:g} s' for a,b in WIN]
for j,(q,f) in enumerate((('Centre excess T (K)','{:7.0f}'),('Centre w (m/s)','{:7.2f}'),('Upward mass flux (kg/s)','{:7.4f}'),('Temperature width b_T (m)','{:7.3f}'))):
    print(f'\n{q}'+('; McC = McCaffrey' if j==0 else '')+'\n   z(m)  '+'  '.join(f'{n:>10s}' for n in names)+('       McC' if j==0 else '')+'   ratio to FDS per window')
    for i,k in enumerate(ZK):
        v=[fds[i][j]]+[R[w][i][j] for w in WIN]
        print(f'  {z[k]:5.2f}  '+'  '.join(f'{f.format(x):>10s}' for x in v)+(f'  {MC.get(k,float("nan")):8.0f}' if j==0 else '')+'   '+' '.join(f'{x/v[0]:5.2f}' for x in v[1:]))
print('\nHeat-release heights (m below which 50/90/99% of the mean heat is released), mean total kW, rms %')
print('   FDS r263 (full)  '+'  '.join(f'{v:.3f}' for v in heights(fh)))
for w in WIN:
    h=H[sl(*w)];print(f'   {w[0]:g}-{w[1]:g} s'.ljust(19)+'  '+'  '.join(f'{v:.3f}' for v in heights(h[:,3:].mean(0)))+f'   {h[:,0].mean()/1e3:6.2f}  {100*h[:,0].std()/h[:,0].mean():4.1f}')
print("\nGrid-scale share of w' by height\n   z(m)  "+'  '.join(f'{n:>10s}' for n in names[1:]))
for i,k in enumerate(ZK):print(f'  {z[k]:5.2f}  '+'  '.join(f'{share(P[sl(*w),i,3]):10.3f}' for w in WIN))
nb=int(NF*DT/2)
print('\nUpward mass flux (kg/s) in consecutive 2 s blocks, then centre excess T (K)\n   z(m)  '+'  '.join(f'{2*b:>3d}-{2*b+2:<3d}' for b in range(nb))+'   spread %')
for j,f in ((2,'{:7.4f}'),(0,'{:7.0f}')):
    for i,k in enumerate(ZK):
        v=[row(P[sl(2*b,2*b+2),i,0].mean(0),P[sl(2*b,2*b+2),i,3].mean(0),(352.6*P[sl(2*b,2*b+2),i,3]/P[sl(2*b,2*b+2),i,0]).mean(0))[j] for b in range(nb)]
        print(f'  {z[k]:5.2f}  '+'  '.join(f.format(x) for x in v)+f'   {100*(max(v)-min(v))/np.mean(v):5.1f}')
