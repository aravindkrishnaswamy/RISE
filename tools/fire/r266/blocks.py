# block-to-block variability of the 3 s (48-frame) statistics the A/B used
import numpy as np
exec(open('full.py').read().split("print('z_m")[0])
for n,a in runs.items():
 print(n)
 for k in [5,7]:
  h=(Z[k]+.5)*DX;row=[]
  for s0 in range(0,235-47,47):
   s=st(a[s0:s0+48,k,0],a[s0:s0+48,k,3]);row.append(f"t={START+s0/16:5.1f}: dTc {s['tc']:5.1f} wr {s['wr']:.2f} b/z {s['b']/h:.3f} me/b {s['me']/s['b']:.2f}")
  print(f' {h:.2f} m\n  '+'\n  '.join(row))
