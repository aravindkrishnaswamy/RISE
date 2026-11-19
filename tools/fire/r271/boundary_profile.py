# r271 (c): side/top open-boundary profiles vs height from per-frame outermost-cell rings, FDS r263 vs RISE r264/r270 (run in dir with fds_ring.npz, r264/r270_ring.bin, fds_mean3d.npy, r264/r270_mean3d.bin).
import numpy as np
DX=.030593116067411014;N=69;NZ=106;A=DX*DX;RA=352.6;z=(np.arange(NZ)+.5)*DX
f=np.load('fds_ring.npz');S=f['side'].astype(np.float64);TOP=f['top'].astype(np.float64)
# common layout: side[t][var][side][z][along], var = u,v,w,T,rho,(p)
def rise(name):
 a=np.fromfile(name+'_ring.bin',dtype='<f4').reshape(235,-1,4)
 sd=a[:,:4*NZ*N].reshape(235,4,NZ,N,4).transpose(0,4,1,2,3).astype(np.float64)  # [t][T,u,v,w][side][z][along]
 tp=a[:,4*NZ*N:].reshape(235,N,N,4).transpose(0,3,2,1).astype(np.float64)       # [t][c][x][y]
 side=np.stack([sd[:,1],sd[:,2],sd[:,3],sd[:,0],RA/sd[:,0]],1);top=np.stack([tp[:,1],tp[:,2],tp[:,3],tp[:,0],RA/tp[:,0]],1)
 return side,top
runs={'FDS':(S[:,:5],TOP),'r264':rise('r264'),'r270':rise('r270')}
sgn=np.array([1,-1,1,-1.]);nrm=[0,0,1,1];tan=[1,1,0,0]
out={}
for k,(side,top) in runs.items():
 un=np.stack([sgn[s]*side[:,nrm[s],s] for s in range(4)],1)          # inward normal velocity [t][side][z][along]
 ut=np.stack([side[:,tan[s],s] for s in range(4)],1);w=side[:,2];T=side[:,3];rho=side[:,4]
 m=rho*un*A                                                            # inward mass flux per cell, kg/s
 o=dict(net=m.mean(0).sum((0,2)),inn=np.maximum(m,0).mean(0).sum((0,2)),outf=-np.minimum(m,0).mean(0).sum((0,2)),
  un=un.mean((0,1,3)),fout=(un<0).mean((0,1,3)),ut=np.abs(ut.mean(0)).mean((0,2)),utrms=np.sqrt((ut**2).mean((0,1,3))),
  w=w.mean((0,1,3)),wabs=np.abs(w).mean((0,1,3)),T=T.mean((0,1,3)),Tmax=T.mean(0).max((0,2)),
  head=(-0.5*1.177*(un**2+ut**2+w**2)*(un>0)).mean((0,1,3)))
 if k=='FDS':o['p']=S[:,5].mean((0,1,3))
 mz=(top[:,4]*top[:,2] if k!='FDS' else top[:,6])*A                    # top-cell upward mass flux
 o['top']=(np.maximum(mz,0).mean(0).sum(),-np.minimum(mz,0).mean(0).sum(),mz.mean(0).sum(),(mz<0).mean())
 out[k]=o
B=[(0,4),(4,10),(10,17),(17,25),(25,33),(33,50),(50,66),(66,82),(82,99),(99,106)]
print('Side boundary (outermost cell column, 4 sides) time means 10.466-25.091 s; RISE rho=352.6/T, FDS DENSITY slice')
print('A. inward mass flux by height band (kg/s): net | inflow part | outflow part')
print(' z band (m)     '+'   '.join(f'{k:>17s}' for k in out)+'   RISE r270/FDS net  share of r270 deficit')
dtot=out['FDS']['net'].sum()-out['r270']['net'].sum()
for a,b in B:
 s=lambda k,q:out[k][q][a:b].sum()
 print(f' {z[a]-DX/2:4.2f}-{z[b-1]+DX/2:4.2f}  '+'   '.join(f'{s(k,"net"):.3f}|{s(k,"inn"):.3f}|{s(k,"outf"):.3f}' for k in out)+
       f'   {s("r270","net")/s("FDS","net"):.3f}   {(s("FDS","net")-s("r270","net"))/dtot:+.3f}')
print(' total         '+'   '.join(f'{out[k]["net"].sum():.3f}|{out[k]["inn"].sum():.3f}|{out[k]["outf"].sum():.3f}' for k in out)+f'   {out["r270"]["net"].sum()/out["FDS"]["net"].sum():.3f}')
print('\nB. per-height boundary state (mean over the 4 sides x 69 cells): FDS / r264 / r270')
print(' z_m   u_in(m/s)              frac_out_cells        |mean u_tan|          rms u_tan            mean w                 T_mean(K)        head -rho|u|^2/2 on inflow (Pa)  FDS p (Pa)')
for k0 in [0,2,5,9,16,24,32,49,65,81,98,103,105]:
 g=lambda q:'/'.join(f'{out[k][q][k0]:+.4f}' for k in out)
 print(f' {z[k0]:4.2f} {g("un")}  {"/".join(f"{out[k]["fout"][k0]:.3f}" for k in out)}  {g("ut")}  {g("utrms")}  {g("w")}  {"/".join(f"{out[k]["T"][k0]:.1f}" for k in out)}  {g("head")}  {out["FDS"]["p"][k0]:+.4f}')
print('\nC. top boundary (top cell layer, cell-centred w): upward-out | downward-in | net out (kg/s) | fraction of cell-samples with downflow')
for k in out:
 u,d,n,fr=out[k]['top'];print(f' {k:5s} out {u:.4f}  in {d:.4f}  net {n:.4f}  downflow cells {fr:.3f}')
# D. square-shell fluxes from time means: is the deficit already present inside the domain?
F=np.load('fds_mean3d.npy')
def shells(ru,rv,i):
 j=N-1-i;return (ru[:,i,i:j+1].sum(1)-ru[:,j,i:j+1].sum(1)+rv[:,i:j+1,i].sum(1)-rv[:,i:j+1,j].sum(1))*A
M={'FDS':(F[6],F[7])}
for r in ['r264','r270']:
 R=np.fromfile(r+'_mean3d.bin').reshape(7,NZ,N,N).transpose(0,1,3,2);M[r]=(RA*R[4],RA*R[5])
print('\nD. net inward mean-flow mass flux through square shells i cells in from the side boundary (kg/s), integrated over heights z<1.0 m | 1.0-2.0 | 2.0-3.24, and RISE/FDS ratio')
print(' i  half-width(m)  '+'  '.join(f'{k:>20s}' for k in M)+'   r270/FDS by band')
for i in [0,1,2,5,10,15,20,25]:
 v={k:shells(*M[k],i) for k in M};bands=[(0,33),(33,66),(66,106)]
 print(f' {i:2d}  {(34.5-i)*DX:5.3f}        '+'  '.join('|'.join(f'{v[k][a:b].sum():.3f}' for a,b in bands) for k in M)+'   '+'|'.join(f'{v["r270"][a:b].sum()/v["FDS"][a:b].sum():.3f}' for a,b in bands))
