#!/usr/bin/env python3
"""r271 section 29 (growth-rate comparison): add a small divergence-free velocity perturbation to a transplant state.
The perturbation is the exact discrete MAC curl of a random vector potential A (edge-located, Gaussian-smoothed white noise
times a smooth envelope around the plume base), so its discrete divergence is zero to round-off in both codes' MAC grids and
neither projection removes it. A is zero on and near the floor, so the floor W plane is untouched.
Layout (xp_state.py): float64 T[z][y][x], U[z][y][x+1], V[z][y+1][x], W[z+1][y][x], then any trailing fields unchanged.
argv: base state, out state, rms (m/s, target rms of the perturbation's cell-centred w over r < 0.15 m, layers 0..16 when
z_lo = 0, else the layers of z_lo..z_hi),
seed, [smoothing sigma in cells, default 1.5], [envelope 'r_max:z_lo:z_hi' m, default 0.20:0.0:0.55]."""
import numpy as np,sys
N=69;NZ=106;DX=.030593116067411014
src,out,rms,seed=sys.argv[1],sys.argv[2],float(sys.argv[3]),int(sys.argv[4])
sig=float(sys.argv[5]) if len(sys.argv)>5 else 1.5
rmax,zlo,zhi=(float(x) for x in (sys.argv[6] if len(sys.argv)>6 else '0.20:0.0:0.55').split(':'))
a=np.fromfile(src,dtype='<f8');nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
U=a[nc:nc+nu].reshape(NZ,N,N+1);V=a[nc+nu:nc+2*nu].reshape(NZ,N+1,N);W=a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N)
rng=np.random.default_rng(seed)
def smooth(f):  # Gaussian filter, sigma in cells, via FFT on a zero-padded box (envelope keeps it local)
    p=[(0,8)]*3;g=np.pad(f,p);F=np.fft.fftn(g)
    ks=[np.fft.fftfreq(n)*2*np.pi for n in g.shape];K2=sum(np.meshgrid(*[k**2 for k in ks],indexing='ij'))
    return np.real(np.fft.ifftn(F*np.exp(-.5*sig**2*K2)))[:f.shape[0],:f.shape[1],:f.shape[2]]
def env(zc,yc,xc):  # smooth bump: radial (1-(r/rmax)^2)^2, vertical sin^2 ramps over 0.06 m at each end, 0 outside
    Z,Y,X=np.meshgrid(zc,yc,xc,indexing='ij');r=np.hypot(X,Y)
    er=np.where(r<rmax,(1-(r/rmax)**2)**2,0.)
    ez=np.clip((Z-zlo)/.06,0,1)*np.clip((zhi-Z)/.06,0,1);ez=np.sin(.5*np.pi*ez)**2
    return er*ez
xc=(np.arange(N)-34)*DX;xf=(np.arange(N+1)-34.5)*DX;zc=(np.arange(NZ)+.5)*DX;zf=np.arange(NZ+1)*DX
# Ax at (cell x, y face, z face), Ay at (x face, cell y, z face), Az at (x face, y face, cell z); arrays [z][y][x]
Ax=smooth(rng.standard_normal((NZ+1,N+1,N)))*env(zf,xf,xc)
Ay=smooth(rng.standard_normal((NZ+1,N,N+1)))*env(zf,xc,xf)
Az=smooth(rng.standard_normal((NZ,N+1,N+1)))*env(zc,xf,xf)
dU=(Az[:,1:,:]-Az[:,:-1,:])/DX-(Ay[1:,:,:]-Ay[:-1,:,:])/DX
dV=(Ax[1:,:,:]-Ax[:-1,:,:])/DX-(Az[:,:,1:]-Az[:,:,:-1])/DX
dW=(Ay[:,:,1:]-Ay[:,:,:-1])/DX-(Ax[:,1:,:]-Ax[:,:-1,:])/DX
div=(dU[:,:,1:]-dU[:,:,:-1]+dV[:,1:,:]-dV[:,:-1,:]+dW[1:]-dW[:-1])/DX
wc=.5*(dW[1:]+dW[:-1]);X,Y=np.meshgrid(xc,xc,indexing='xy');m=np.hypot(X,Y)<.15
ks=slice(0,17) if zlo==0 else slice(int(zlo/DX),int(np.ceil(zhi/DX)))  # normalisation layers: 0..16, or the envelope's band
s=rms/np.sqrt((wc[ks][:,m]**2).mean());dU*=s;dV*=s;dW*=s
assert abs(dW[0]).max()==0 and abs(dU).max()>0
uc=.5*(dU[:,:,1:]+dU[:,:,:-1]);vc=.5*(dV[:,1:,:]+dV[:,:-1,:])
U+=dU;V+=dV;W+=dW;a.astype('<f8').tofile(out)
print(f'{out}: seed {seed} sigma {sig} env r<{rmax} z {zlo}-{zhi} m; scale {s:.3g}; rms w/u/v (r<0.15, layers {ks.start}-{ks.stop-1}) '
      f'{rms:.2e}/{np.sqrt((uc[ks][:,m]**2).mean()):.2e}/{np.sqrt((vc[ks][:,m]**2).mean()):.2e}; max |du| {max(abs(dU).max(),abs(dV).max(),abs(dW).max()):.2e}; '
      f'max |div| {abs(div).max()*s:.2e} /s')
