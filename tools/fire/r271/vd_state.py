# r271 section 26: variable-density momentum-advection state for both codes (xp_driver kind fds with
# RISE_R271_XP_CKPT=none, and make_fds_xp.py). In the band f(z) of build_bt.py: T = 300 + dT f(z) (1 + cos(kT x'))/2,
# u = U f(z) on the x faces, v = A f(z) cos(k x') on the y faces, w = 0 (divergence-free; T is advected without
# heating). x' from the domain centre. Writes <out>_rise.bin (T,U,V,W float64, xp_state.py layout) and <out>.bin (+ Y = 0).
# argv: out, n (v cells per wavelength), A m/s, U m/s, dT K, nT (T cells per wavelength).
import numpy as np,sys
N=69;NZ=106;H=.030593116067411014;out=sys.argv[1];n,A,U,dT,nT=(float(x) for x in sys.argv[2:7])
k=2*np.pi/(n*H);kT=2*np.pi/(nT*H);X0=34.5*H
def f(z):
    z=np.asarray(z,float);o=np.where((z>1.75)&(z<2.85),1.,0.)
    o=np.where((z>1.6)&(z<=1.75),np.sin(.5*np.pi*(z-1.6)/.15)**2,o);return np.where((z>=2.85)&(z<3.0),np.sin(.5*np.pi*(3.0-z)/.15)**2,o)
zc=(np.arange(NZ)+.5)*H;xc=(np.arange(N)+.5)*H-X0
T=300+dT*f(zc)[:,None,None]*(1+np.cos(kT*xc))[None,None,:]/2*np.ones((NZ,N,N))
Uf=U*f(zc)[:,None,None]*np.ones((NZ,N,N+1))
V=A*f(zc)[:,None,None]*np.cos(k*xc)[None,None,:]*np.ones((NZ,N+1,N))
W=np.zeros((NZ+1,N,N))
a=np.concatenate([T.ravel(),Uf.ravel(),V.ravel(),W.ravel()]);a.tofile(out+'_rise.bin');np.concatenate([a,np.zeros(N*N*NZ)]).tofile(out+'.bin')
print(f'T {T.min():.1f}-{T.max():.1f} K, |u| max {abs(Uf).max():.3f}, |v| max {abs(V).max():.3f}')
