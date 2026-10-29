exec(open('meander.py').read().split("xy=(np.arange")[0].replace("R=np.fromfile","R=0#").replace("R=np.transpose","#"))
A=DX*DX
for k in [98,102,105]:
 t,M=read_slice(RUN/f'{P}_1_{idx[("MASS FLUX Z",k)]}.sf');s=(t>=START)&(t<=END);M=M[s];t=t[s]
 w=np.gradient(t);w/=w.sum()
 up=(np.maximum(M,0).sum((1,2))*A*w).sum();dn=(np.minimum(M,0).sum((1,2))*A*w).sum()
 print(k,(k+.5)*DX,'FDS up %.4f down %.4f net %.4f kg/s'%(up,dn,up+dn))
