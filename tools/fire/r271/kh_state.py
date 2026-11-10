#!/usr/bin/env python3
"""r271 section 29: cold uniform-density shear-layer base state for the growth-rate twin test (xp_driver with
RISE_R271_XP_CKPT=none; make_fds_xp.py with the companion zero-methane file). T = 300 K everywhere. Velocity = discrete MAC
curl of a streamfunction A_y = Psi(z) g(x) (exactly divergence-free, zero on every boundary face): u = u0(z) g(x),
w = -Psi(z) g'(x), u0(z) = (dU/2) tanh(2 (z - z0)/d) h(z), h = 1 within 0.35 m of z0 and sin^2 to 0 at 0.5 m (u0 odd about z0,
so Psi = int u0 dz returns to 0); g = 1 for |x| < 0.6 m, sin^2 to 0 at 0.9 m. Around the centre (|x| < 0.6, |z - z0| < 0.35)
this is the steady parallel tanh layer. argv: out state (4 fields, xp_state.py layout), out companion (5 fields, Y = 0),
dU m/s, z0 m, d (vorticity thickness, m)."""
import numpy as np,sys
N=69;NZ=106;DX=.030593116067411014;dU,z0,d=(float(x) for x in sys.argv[3:6])
def taper(a,inner,outer):a=np.abs(a);return np.where(a<=inner,1.,np.where(a>=outer,0.,np.cos(.5*np.pi*(a-inner)/(outer-inner))**2))
zf=np.arange(NZ+1)*DX;zc=(np.arange(NZ)+.5)*DX;xf=(np.arange(N+1)-34.5)*DX
h=taper(zc-z0,.35,.5);u0=.5*dU*np.tanh(2*(zc-z0)/d)*h;u0-=h*u0.sum()/h.sum()  # remove the grid-offset net flux so Psi closes
Psi=np.r_[0.,np.cumsum(u0)*DX]  # Psi at z faces
assert abs(Psi[-1])<1e-9*max(1,abs(Psi).max()),Psi[-1]
Ay=(-Psi)[:,None,None]*taper(xf,.6,.9)[None,None,:]*np.ones((NZ+1,N,N+1))   # [z face][y cell][x face]
U=-(Ay[1:]-Ay[:-1])/DX                                                    # U = dAz/dy - dAy/dz
W=(Ay[:,:,1:]-Ay[:,:,:-1])/DX                                             # W = dAy/dx - dAx/dy
V=np.zeros((NZ,N+1,N));T=np.full((NZ,N,N),300.)
div=(U[:,:,1:]-U[:,:,:-1]+W[1:]-W[:-1])/DX
a=np.concatenate([T.ravel(),U.ravel(),V.ravel(),W.ravel()]);a.astype('<f8').tofile(sys.argv[1])
np.concatenate([a,np.zeros(N*N*NZ)]).astype('<f8').tofile(sys.argv[2])
print(f'{sys.argv[1]}: shear layer dU {dU} m/s at z0 {z0} m, vorticity thickness {d} m ({d/DX:.1f} cells); |u| max {abs(U).max():.3f}, '
      f'|w| max {abs(W).max():.3f}, boundary |u| {max(abs(U[:,:,0]).max(),abs(U[:,:,-1]).max()):.1e}, |w| floor/top {abs(W[0]).max():.1e}/{abs(W[-1]).max():.1e}, max |div| {abs(div).max():.1e} /s')
