# r271 section 25 (read-only). RISE inputs are ckpt_layers/ckpt_div dumps of the inert compact run's checkpoints (paths
# relative to the working directory: run from /Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/dumps; regenerate with
# tools/fire/r271/ckpt_layers.cpp / ckpt_div.cpp); FDS from r271_inert slices.
import numpy as np,sys,json
from pathlib import Path
T0=Path('/Volumes/SSD/Working/RISE/tools/fire/r271');__file__=str(T0/'inert_shear.py')
sys.argv=['x','.','d'];exec(open(__file__).read().split('rad=np.arange')[0])
Rb=np.sqrt(69*DX*DX/np.pi);inn=RR<Rb-DX
fr=json.load(open('/Volumes/SSD/Working/RISE/docs/data/fire_r271/inert_source_profile.json'))['fractions']
Tt=np.array([300,500,1000,1500,2000.]);cpA=np.array([1011,1036,1149,1220,1260.]);cpM=np.array([2226,2839,4463,5357,5800.])
mask=None
def src(k,T,rho,Y):
    cp=(1-Y)*np.interp(T,Tt,cpA)+Y*np.interp(T,Tt,cpM);Q=26400*fr[k]/(69*DX**3)
    return Q/(rho*cp*T)
t=np.loadtxt('orig_div.bin.times');a=np.transpose(np.fromfile('orig_div.bin',dtype='<f4').reshape(len(t),8,4,N,N),(0,1,2,4,3)).astype(np.float64)
burner=None
F={q:np.stack([read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf') for k in range(8)],1) for q in ('DIVERGENCE','TEMPERATURE','DENSITY')}
F['TEMPERATURE']+=273.15
def Yfrom(rho,T):Wm=rho*8314.46*T/101325.;return np.clip((1/Wm-1/28.85)/(1/16.04-1/28.85),0,1)
print('burner interior, 6-14 s. total div: mean, temporal rms | source part Q/(rho cp T): mean, rms | rest (total-source): mean, rms | corr(total\',source\') | mean Y_CH4, rms')
for k in range(8):
    for n,(D,T,rho,Y) in (('RISE',(a[:,k,0],a[:,k,1],a[:,k,2],a[:,k,3])),('RISE Y from rho,T',(a[:,k,0],a[:,k,1],a[:,k,2],Yfrom(a[:,k,2],a[:,k,1]))),
                         ('FDS',(F['DIVERGENCE'][:,k],F['TEMPERATURE'][:,k],F['DENSITY'][:,k],Yfrom(F['DENSITY'][:,k],F['TEMPERATURE'][:,k])))):
        S=src(k,T,rho,Y);Rr=D-S
        r=lambda q:np.sqrt(q.var(0))[inn].mean()
        dp=(D-D.mean(0))[:,inn];sp=(S-S.mean(0))[:,inn];cc=(dp*sp).sum()/np.sqrt((dp**2).sum()*(sp**2).sum())
        print(f'  k{k} {n:18s} {D[:,inn].mean():6.2f} {r(D):6.3f} | {S[:,inn].mean():6.2f} {r(S):6.3f} | {Rr[:,inn].mean():6.2f} {r(Rr):6.3f} | {cc:5.2f} | Y {Y[:,inn].mean():.3f} {r(Y):.4f}')
