"""Mathematical falsification probes. No RISE builds or source changes.
Numeric ranks are diagnostics; no computed threshold is a classification rule.
"""
from decimal import Decimal, localcontext
import argparse, json, math, time
from pathlib import Path
import numpy as np

def unit(v):return v/np.linalg.norm(v)
def basis(n):
 x=unit(np.cross(n,np.array([0.,0.,1.]) if abs(n[2])<.8 else np.array([0.,1.,0.])))
 return np.column_stack((x,np.cross(n,x)))
def snell(q,p,A,eta):
 n=unit(A@q);u=unit(q-p);c=np.dot(u,n);disc=1-eta*eta*(1-c*c)
 if c<=0 or disc<=0:return None
 ct=math.sqrt(disc);return eta*u+(ct-eta*c)*n

def point_derivative(q,p,A,eta):
 g=A@q;h=np.linalg.norm(g);n=g/h;E=basis(n);L=np.linalg.norm(q-p);u=(q-p)/L;c=np.dot(u,n);disc=1-eta*eta*(1-c*c)
 if c<=0 or disc<=1e-8:return None
 ct=math.sqrt(disc);beta=ct-eta*c;v=E.T@u
 B=eta/L*(np.eye(2)-np.outer(v,v))+beta*(E.T@A@E)/h
 return E,B,float(np.linalg.det(B)/ct),ct

def local_point_probes():
 rng=np.random.default_rng(0x117c0ffee);counts=0;min_low=math.inf;worst=0.;near=[]
 for major in [1.,1.5,3.,10.]:
  A=np.diag([1.,1.,1/(major*major)])
  for eta in [.5,1.,1.01,1.2,1.7,2.,3.,10.]:
   for _ in range(400):
    p=unit(rng.normal(size=3));p=p/math.sqrt(p@A@p)
    q=unit(rng.normal(size=3));q=q/math.sqrt(q@A@q)
    data=point_derivative(q,p,A,eta)
    if data is None:continue
    E,B,jac,ct=data;t=snell(q,p,A,eta)
    if eta<=1:min_low=min(min_low,float(np.linalg.eigvalsh(B)[0]))
    # Central derivatives along the actual ellipsoid; selected noncritical
    # points only. Very small determinant ratios are reported, never classified.
    deriv=[];step=2e-5
    for j in range(2):
     samples=[]
     for k in [-2,-1,1,2]:
      x=q+k*step*E[:,j];x=x/math.sqrt(x@A@x)
      value=snell(x,p,A,eta)
      if value is None:break
      samples.append(value)
     if len(samples)!=4:break
     deriv.append((samples[0]-8*samples[1]+8*samples[2]-samples[3])/(12*step))
    if len(deriv)!=2:continue
    numeric=float(np.dot(t,np.cross(deriv[0],deriv[1])))
    rel=abs(numeric-jac)/max(abs(jac),1e-7)
    worst=max(worst,rel);counts+=1
    if abs(jac)<1e-3:near.append({'major':major,'eta':eta,'jacobian':jac,'p':p.tolist(),'q':q.tolist()})
 return {'cases':counts,'max_normalized_analytic_vs_finite_difference_error':worst,'minimum_low_index_B_eigenvalue':min_low,'near_caustic_diagnostic_cases':len(near),'five_smallest_examples':sorted(near,key=lambda x:abs(x['jacobian']))[:5]}

def dsin(x):
 t=x;s=x;n=1
 while True:
  t=-t*x*x/Decimal((2*n)*(2*n+1));new=s+t
  if new==s:return s
  s=new;n+=1
def dcos(x):
 t=Decimal(1);s=t;n=1
 while True:
  t=-t*x*x/Decimal((2*n-1)*(2*n));new=s+t
  if new==s:return s
  s=new;n+=1
def dot(a,b):return sum((x*y for x,y in zip(a,b)),Decimal(0))
def norm(a):return dot(a,a).sqrt()
def dunit(a):
 length=norm(a);return [x/length for x in a]
def decimal_counterprobes():
 with localcontext() as ctx:
  ctx.prec=100;rows=[]
  for alpha in ['.1','.01','.001','.0001','.00001','.000001']:
   a=Decimal(alpha);s=dsin(a);c=dcos(a);ct=(1-4*s*s).sqrt();beta=ct-2*c
   tx=2*s+beta*2*s*c
   b1=ct-c;b2=ct+1/c-2*c;jac=4*c*b1*b2/ct
   rows.append({'alpha':alpha,'outgoing_x':str(tx),'solid_angle_jacobian':str(jac),'jacobian_over_alpha4':str(jac/a**4)})
  # Ellipsoid a=3,b=5,f=4 and eta=5/4. Source at +focus, far
  # lower-half exits. This is a real point-source collimator, not a reachable
  # same-child retained point (the focus lies strictly inside the ellipsoid).
  focus=[]
  for zc in ['-.1','-.3','-.6','-.9']:
   c=Decimal(zc);s=(1-c*c).sqrt();q=[3*s,Decimal(0),5*c];p=[Decimal(0),Decimal(0),Decimal(4)]
   u=dunit([x-y for x,y in zip(q,p)]);n=dunit([q[0]/9,Decimal(0),q[2]/25]);ci=dot(u,n);eta=Decimal(5)/4;ct=(1-eta*eta*(1-ci*ci)).sqrt();beta=ct-eta*ci
   t=[eta*x+beta*y for x,y in zip(u,n)]
   focus.append({'cos_meridian':zc,'outgoing':[str(x) for x in t],'max_error_from_minus_z':str(max(abs(t[0]),abs(t[1]),abs(t[2]+1))),'normal_cosine':str(ct)})
  return {'precision_decimal_digits':100,'sphere_eta2_paraxial_focus_is_not_atom':rows,'ellipsoid_exact_point_source_collimator':focus}

def phase_probe():
 # Boundary phase map: point2 and tangential incoming direction2 -> same point2
 # and tangential transmitted direction2. This exact block structure is the
 # proof; the probe exercises its implementation and near-critical domain.
 rng=np.random.default_rng(123);results=[]
 for eta in [.5,1.,1.2,2.,10.]:
  ratios=[]
  for _ in range(100):
   q=rng.normal(size=2);v=unit(rng.normal(size=2))*rng.uniform(.01,min(.9,.9/eta))
   def f(z):return np.r_[z[:2],eta*z[2:]]
   z=np.r_[q,v];h=1e-5;J=np.column_stack([(f(z+h*np.eye(4)[i])-f(z-h*np.eye(4)[i]))/(2*h) for i in range(4)])
   ratios.append(float(np.linalg.det(J)/(eta*eta)))
  results.append({'eta':eta,'min_numeric_det_over_eta2':min(ratios),'max_numeric_det_over_eta2':max(ratios)})
 return results
def focusing_phase_probe():
 # Same ellipsoid/focus as the exact Decimal example. The source plane has
 # four ray coordinates. At the exact focus its direction-only 2x2 block
 # vanishes, but the full position+direction map remains nonsingular.
 A=np.diag([1/9,1/9,1/25]);eta=1.25
 def f(z):
  p=np.array([z[0],z[1],4.]);u=np.array([z[2],z[3],-math.sqrt(1-z[2]*z[2]-z[3]*z[3])])
  aa=u@A@u;bb=2*p@A@u;cc=p@A@p-1
  length=(-bb+math.sqrt(bb*bb-4*aa*cc))/(2*aa);q=p+length*u
  t=snell(q,p,A,eta)
  return np.r_[q[:2],t[:2]]
 z=np.array([0.,0.,12/37,0.]);results=[]
 for h in [1e-3,1e-4,1e-5]:
  columns=[]
  for i in range(4):
   e=np.eye(4)[i]*h
   columns.append((f(z-2*e)-8*f(z-e)+8*f(z+e)-f(z+2*e))/(12*h))
  J=np.column_stack(columns)
  results.append({'step':h,'full_phase_4x4_det':float(np.linalg.det(J)),'full_phase_singular_values':np.linalg.svd(J,compute_uv=False).tolist(),'fixed_point_angular_2x2_det':float(np.linalg.det(J[2:,2:]))})
 return results

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output',type=Path,required=True)
 output=parser.parse_args().output
 start=time.monotonic();report={'warning':'Numerical diagnostics are falsification probes, never rank certificates or production classification rules.','point_source':local_point_probes(),'high_precision':decimal_counterprobes(),'boundary_phase':phase_probe(),'focusing_phase':focusing_phase_probe()};report['wall_seconds']=time.monotonic()-start
 assert report['point_source']['cases']>6000
 assert report['point_source']['max_normalized_analytic_vs_finite_difference_error']<1e-5
 assert report['point_source']['minimum_low_index_B_eigenvalue']>0
 assert all(Decimal(row['max_error_from_minus_z'])<Decimal('1e-90') for row in report['high_precision']['ellipsoid_exact_point_source_collimator'])
 assert all(Decimal(row['solid_angle_jacobian'])>0 for row in report['high_precision']['sphere_eta2_paraxial_focus_is_not_atom'])
 assert all(abs(row['full_phase_4x4_det']-1.5625)<1e-7 for row in report['focusing_phase'])
 report['diagnostic_validation']='PASS 6 analytic/finite-difference and high-precision consistency assertions; not a production rank classifier'
 output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ['high_precision','boundary_phase']},indent=2));print('wrote',output)
