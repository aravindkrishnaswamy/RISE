import numpy as np, subprocess, json, time, os
from pathlib import Path
from numpy.polynomial.legendre import leggauss
root=Path("/tmp/dl423-offline");root.mkdir(exist_ok=True)
cells=[(.02,.9,1.5),(.05,.9,1.5),(.5,.9,1.5),(.05,.9,1.3),(.05,.9,1.7)]
results=[]
for alpha,f0,eta in cells:
 for n in [16,32,64,128]:
  x,w=leggauss(n//2);crit=np.sqrt(1-eta**-2);mu=np.r_[(x+1)*crit/2,crit+(x+1)*(1-crit)/2];dw=np.r_[w*crit/2,w*(1-crit)/2];weights=2*np.pi*mu*dw
  grid=root/f"grid-{eta}-{n}.txt";grid.write_text(str(n)+"\n"+"\n".join(f"{a:.17g} {b:.17g}"for a,b in zip(mu,dw)))
  out=root/f"R-{alpha}-{f0}-{eta}-{n}.bin";t=time.monotonic();subprocess.run(["/tmp/dl423-kernel-probe",str(grid),str(out),str(alpha),str(f0),str(eta),"2048"],check=True)
  data=np.fromfile(out,dtype=np.float64).reshape(n+6,n);R=data[:n];rows=data[n:];sin2=eta**2*(1-mu**2);mt=np.sqrt(np.maximum(0,1-sin2));rs=(eta*mu-mt)/(eta*mu+mt);rp=(mu-eta*mt)/(mu+eta*mt);F=np.where(sin2>=1,1,(rs**2+rp**2)/2);K=R*weights[None,:];Q=F
  esc=np.linalg.solve(np.eye(n)-Q[:,None]*K,1-F);vals=[]
  for j,theta in enumerate([0,45,70,80,85,89]):
   m=np.cos(np.deg2rad(theta));mi=np.sqrt(1-(1-m*m)/eta**2);fi=.5*((m-eta*mi)/(m+eta*mi))**2+.5*((eta*m-mi)/(eta*m+mi))**2;al=fi+(1-fi)*np.dot(rows[j]*weights,esc);vals.append(float(al))
  result=dict(alpha=alpha,F0=f0,eta=eta,n=n,phi=2048,seconds=time.monotonic()-t,albedo=vals,row_energy_min=float(K.sum(1).min()),row_energy_max=float(K.sum(1).max()),residual=float(np.max(np.abs((np.eye(n)-Q[:,None]*K)@esc-(1-F)))))
  results.append(result);print(json.dumps(result),flush=True);(root/"results.json").write_text(json.dumps(results,indent=2))
