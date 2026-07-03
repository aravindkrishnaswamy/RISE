import numpy as np
# Normal-incidence characteristic-matrix TMM. Stack authored ambient->substrate.
# Reflection from the AIR side (what the camera sees on the crystal top).
n_air=1.0; n_sap=1.77
def R_of(stack, lam):   # stack: list of (n, d_nm); lam in nm
    M=np.eye(2,dtype=complex)
    for (n,d) in stack:
        delta=2*np.pi*n*d/lam
        c=np.cos(delta); s=np.sin(delta)
        Mj=np.array([[c, 1j*s/n],[1j*n*s, c]],dtype=complex)
        M=M@Mj
    B,C=M@np.array([1.0, n_sap],dtype=complex)
    r=(n_air*B - C)/(n_air*B + C)
    return abs(r)**2
def report(name, stack):
    lams=np.arange(420,681,10.0)
    Rs=np.array([R_of(stack,l) for l in lams])
    # crude colour: mean R in blue(420-490), green(500-580), red(590-680)
    b=Rs[(lams<=490)].mean(); g=Rs[(lams>=500)&(lams<=580)].mean(); r=Rs[(lams>=590)].mean()
    print(f"{name:28s} Rmean={Rs.mean()*100:5.2f}%  Rmax={Rs.max()*100:5.2f}%  spread={ (Rs.max()-Rs.min())*100:5.2f}%  B/G/R={b*100:.2f}/{g*100:.2f}/{r*100:.2f}%")
    return Rs, lams

# bare sapphire single interface for reference
print("bare sapphire R ~ %.2f%%"%(((n_sap-1)/(n_sap+1))**2*100))
report("single MgF2 99.6",       [(1.38,99.6)])
report("QHQ 540 (current)",      [(1.38,97.8),(2.05,131.7),(1.63,82.8)])
# try tuning: shift design wavelength / thicknesses for flat neutral
for lam0 in (500,520,560,580):
    report(f"QHQ @{lam0}", [(1.38,lam0/(4*1.38)),(2.05,lam0/(2*2.05)),(1.63,lam0/(4*1.63))])
# 2-layer V-ish and 4-layer variants
report("MgF2/Al2O3 QQ @520",     [(1.38,520/(4*1.38)),(1.63,520/(4*1.63))])
report("4-layer BBAR",           [(1.46,110),(2.10,30),(1.46,35),(2.10,120)])

print("\n--- search: minimize (meanR + 1.5*spread) over 2- and 3-layer families ---")
best=None
# 2-layer: MgF2 outer (fixed ~1.38), inner index in [1.55,1.75], both QWOT-ish, scan lam0 + thickness scales
import itertools
def score(stack):
    lams=np.arange(420,681,5.0); Rs=np.array([R_of(stack,l) for l in lams])
    return Rs.mean()+1.5*(Rs.max()-Rs.min()), Rs.mean(), (Rs.max()-Rs.min())
for n2 in np.arange(1.58,1.74,0.02):
    for lam0 in np.arange(490,571,10):
        for sc1 in (0.9,1.0,1.1):
            for sc2 in (0.9,1.0,1.1):
                st=[(1.38, sc1*lam0/(4*1.38)),(n2, sc2*lam0/(4*n2))]
                s,m,sp=score(st)
                if best is None or s<best[0]: best=(s,m,sp,("2L",n2,lam0,sc1,sc2,st))
print("best 2L: score=%.4f meanR=%.2f%% spread=%.2f%%"%(best[0]*100,best[1]*100,best[2]*100), "n2=%.2f lam0=%d sc=%.1f/%.1f"%best[3][1:5])
print("  layers:", [(round(n,3),round(d,1)) for n,d in best[3][5]])
# 3-layer QHQ scan
best3=None
for nH in (1.95,2.05,2.15,2.30):
    for nM in np.arange(1.60,1.72,0.02):
        for lam0 in np.arange(490,571,10):
            st=[(1.38,lam0/(4*1.38)),(nH,lam0/(2*nH)),(nM,lam0/(4*nM))]
            s,m,sp=score(st)
            if best3 is None or s<best3[0]: best3=(s,m,sp,(nH,nM,lam0,st))
print("best 3L: score=%.4f meanR=%.2f%% spread=%.2f%%"%(best3[0]*100,best3[1]*100,best3[2]*100),"nH=%.2f nM=%.2f lam0=%d"%best3[3][:3])
print("  layers:", [(round(n,3),round(d,1)) for n,d in best3[3][3]])
