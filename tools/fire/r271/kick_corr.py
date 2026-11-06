# r271 section 27: the two-step start-up kick. argv: label, step-0 state, step-1 state, step-2 state (ckpt_state /
# xp_state.py float64 layout T, U, V, W). Per layer band: rms of the step-1 and step-2 increments d1, d2 and corr(d1, d2)
# over the whole plane. A memoryless map started from a self-consistent state gives corr near +1 (smooth evolution);
# -1 with |d2| ~ |d1| is a kick that the next step undoes (the start state is off the solver's discrete manifold).
import numpy as np,sys
N=69;NZ=106
def load(p):
    a=np.fromfile(p,'<f8');o=0;out=[]
    for shape in [(NZ,N,N),(NZ,N,N+1),(NZ,N+1,N),(NZ+1,N,N)]:
        n=int(np.prod(shape));out.append(a[o:o+n].reshape(shape));o+=n
    return out
s0,s1,s2=(load(p) for p in sys.argv[2:5])
bands=[(0,1),(1,3),(3,8),(8,17),(17,34),(34,66)]
print(sys.argv[1]+': rms d1 / rms d2 / corr(d1,d2) by layer band')
for name,i in [('T',0),('U',1),('V',2),('W',3)]:
    line=f'  {name}'
    for a,b in bands:
        d1=(s1[i][a:b]-s0[i][a:b]).ravel();d2=(s2[i][a:b]-s1[i][a:b]).ravel()
        c=np.corrcoef(d1,d2)[0,1] if d1.std()>0 and d2.std()>0 else float('nan')
        line+=f' | k{a}-{b-1}: {np.sqrt((d1**2).mean()):.3g}/{np.sqrt((d2**2).mean()):.3g} {c:+.2f}'
    print(line)
