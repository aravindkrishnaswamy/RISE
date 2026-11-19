# r271 section 25: Rayleigh-Taylor initial state for both codes (xp_driver with RISE_R271_XP_CKPT=none; make_fds_xp.py):
# quiescent ambient air with a full-width hot layer, T = 300 + dT s(z) K, s = 1 between z = 1.8 m and the interface
# z_i = 2.3 m + eta cos(k x') (x' from the domain centre), tanh edges of width D; zero velocity; methane fraction 0.
# argv: out file, n (cells per wavelength), eta (cells) [, layer excess K (600), edge width in cells (1)].
# Writes the five-block file for make_fds_xp.py and <out>_rise.bin (T,U,V,W only) for xp_driver.
import numpy as np,sys
N=69;NZ=106;H=.030593116067411014;n=float(sys.argv[2]);eta=float(sys.argv[3])*H;dT=float(sys.argv[4]) if len(sys.argv)>4 else 600.;D=(float(sys.argv[5]) if len(sys.argv)>5 else 1.)*H;k=2*np.pi/(n*H)
z=(np.arange(NZ)+.5)*H;x=(np.arange(N)+.5)*H-34.5*H
Z,Y,X=np.meshgrid(z,x,x,indexing='ij')
zi=2.3+eta*np.cos(k*X)
T=300+dT*.25*(1-np.tanh((Z-zi)/D))*(1+np.tanh((Z-1.8)/D))
z0=np.zeros;nc=N*N*NZ
out=np.concatenate([T.ravel(),np.zeros((N+1)*N*NZ),np.zeros((N+1)*N*NZ),np.zeros(N*N*(NZ+1)),np.zeros(nc)])
out.astype('<f8').tofile(sys.argv[1]);out[:-nc].astype('<f8').tofile(sys.argv[1].replace('.bin','_rise.bin'));print(f'{sys.argv[1]}: T {T.min():.1f}-{T.max():.1f}, n={n}, eta={eta:.4f} m')
