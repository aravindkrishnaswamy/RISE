#!/usr/bin/env python3
"""Read-only inlet formula and retained HRR histories; missing FDS planes stay missing."""
from pathlib import Path
import csv,json,math
import numpy as np
from r255_reduce_fds import csv_data
from r257_compare_retained import welch
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r259_inlet_forcing';OUT=ROOT/'docs/data/fire_r259';DX=.030593116067411014

def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def metrics(t,y):
 mean=float(np.trapezoid(y,t)/(t[-1]-t[0]));rms=float(np.sqrt(np.trapezoid((y-mean)**2,t)/(t[-1]-t[0])));f,p=welch(y,fs=16,window='hann',nperseg=64,noverlap=32,detrend='linear',scaling='density')
 return {'mean':mean,'rms':rms,'rms_over_mean':rms/mean,'power_1_le_f_lt_3_Hz':float(p[(f>=1)&(f<3)].sum()*.25)},f,p

def main():
 a=np.genfromtxt(WORK/'r251_planes.csv',names=True,delimiter=',');assert len(a)==235*106
 t=np.array([a[a['frame']==i]['time_s'][0] for i in range(235)]);planes=a['slab_heat_W'].reshape(235,106)
 fraction=np.clip((.7-np.arange(106)*DX)/DX,0,1);rise_region=planes@fraction
 fds=csv_data(BASE/'r258_fds_les/fds_run/r258_fds_hrr.csv');ends=fds['Time'];mid=.5*(ends[:-1]+ends[1:]);fds_global=np.interp(t,mid,fds['HRR'][1:]*1000)
 signals=[('RISE','whole_domain',planes.sum(axis=1)),('RISE','0_to_0p7m',rise_region),('FDS','whole_domain_proxy_only',fds_global)]
 for z in [3,9,16]:signals.append(('RISE',f'plane_z{z}_center_{(z+.5)*DX:.6f}m',planes[:,z]/DX))
 rows=[];series=[];spectra=[]
 for code,region,signal in signals:
  m,f,p=metrics(t,signal);units='W/m' if region.startswith('plane_') else 'W';rows.append({'code':code,'region':region,'units':units,**m,'FDS_comparable_region_available':region in ['whole_domain','whole_domain_proxy_only']})
  for time,value in zip(t,signal):series.append({'code':code,'region':region,'time_s':float(time),'value':float(value),'units':units})
  for freq,power in zip(f,p):spectra.append({'code':code,'region':region,'frequency_Hz':float(freq),'PSD':float(power),'units':units+' squared / Hz'})
 write('forcing_summary.csv',rows);write('retained_heat_timeseries.csv',series);write('forcing_spectra.csv',spectra)
 write('r251_base_cell_rates.csv',[{'time_s':float(r['time_s']),'z':int(r['z']),'height_m':float(r['height_m']),'center_HRR_W_m3':float(r['center_rate_W_m3']),'rim_x38_HRR_W_m3':float(r['rim_rate_W_m3'])} for r in a if int(r['z']) in [3,9,16]])
 inlet=np.genfromtxt(WORK/'r251_inlet_history.csv',names=True,delimiter=',');assert len(inlet)==204*69
 face_rows=[]
 old=list(csv.DictReader((ROOT/'docs/data/fire_r258/burner_inflow_profile.csv').open()))
 for row in old:
  x=int(row['x']);y=int(row['y']);p=inlet[(inlet['x']==x)&(inlet['y']==y)];weight=p['weight'];assert abs(weight.sum()-1)<1e-12
  avg=lambda k:float(np.dot(weight,p[k]))
  j=float(p[0]['j_kg_m2_s']);u=avg('u_face_m_s');uf=float(row['FDS_inflow_w_m_s'])
  face_rows.append({'x':x,'y':y,'j_kg_m2_s':j,'rho_CH4_kg_m3':float(p[0]['rho_CH4_kg_m3']),'mean_rho_cell_kg_m3':avg('rho_cell_kg_m3'),'mean_rho_dual_kg_m3':avg('rho_dual_kg_m3'),'expected_pure_CH4_u_m_s':float(p[0]['u_expected_CH4_m_s']),'mean_predicted_dual_u_m_s':avg('u_expected_dual_m_s'),'measured_RISE_face_u_m_s':u,'measured_FDS_face_u_m_s':uf,'RISE_effective_density_j_over_mean_u':j/u,'FDS_effective_density_j_over_mean_u':j/uf})
 write('inlet_face_balance.csv',face_rows)
 rho=float(inlet[0]['rho_CH4_kg_m3']);area=69*DX**2;nominal_area=math.pi*.3**2/4;mdot=sum(r['j_kg_m2_s']*DX**2 for r in face_rows)
 u=float(np.mean([r['measured_RISE_face_u_m_s'] for r in face_rows]));uf=float(np.mean([r['measured_FDS_face_u_m_s'] for r in face_rows]));expected=mdot/(rho*area)
 result={'statistics_start_s':float(t[0]),'statistics_end_s':float(t[-1]),'heat_summary':rows,'heat_limit':'FDS physical regional and plane HRR not retained; whole-domain comparison is a proxy and cannot select mixing-time versus eligibility or establish regional forcing equality','inlet':{'measurement':'both native bottom z-face, z=0; RISE checkpoint component[2][x+nx*y]; FDS W-VELOCITY device XYZ=(facecenter,0), face index NINT(ZK)=0','rho_CH4_kg_m3':rho,'active_area_m2':area,'nominal_circle_area_m2':nominal_area,'supplied_mdot_kg_s':mdot,'expected_pure_CH4_mean_face_u_m_s':expected,'expected_using_nominal_area_u_m_s':mdot/(rho*nominal_area),'measured_RISE_mean_face_u_m_s':u,'measured_FDS_mean_face_u_m_s':uf,'RISE_to_FDS_ratio':u/uf,'RISE_relative_to_pure_CH4':u/expected-1,'FDS_relative_to_pure_CH4':uf/expected-1,'max_abs_RISE_dual_formula_error_m_s':float(np.max(np.abs(inlet['u_face_m_s']-inlet['u_expected_dual_m_s']))),'max_abs_prescribed_momentum_error_kg_m2_s':float(np.max(np.abs(inlet['momentum_face_kg_m2_s']-inlet['j_kg_m2_s']))),'formula':'RISE u=j/[0.5*rho_inj+0.5*rho_cell]; FDS UN=MFT/RHO_F, with wall mixture density from species diffusive boundary solve','RISE_aggregate_effective_density_jbar_over_ubar':mdot/(area*u),'FDS_aggregate_effective_density_jbar_over_ubar':mdot/(area*uf),'classification':'density-definition difference; no area or face-versus-cell measurement mismatch; pure-methane prescribed-speed expectation is not what either boundary implementation uses'}}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 import matplotlib.pyplot as plt
 fig,axes=plt.subplots(3,2,figsize=(12,10),layout='constrained')
 groups=[signals[:1]+signals[2:3],signals[1:2],signals[3:]]
 for row,group in enumerate(groups):
  for code,region,signal in group:
   normalized=row==2;display=signal/signal.mean() if normalized else signal/1000
   label=code+' '+region
   axes[row,0].plot(t,display,label=label,lw=.8)
   m,f,power=metrics(t,signal)
   axes[row,1].semilogy(f[1:],power[1:]/(signal.mean()**2 if normalized else 1e6),label=label)
  axes[row,0].set(xlabel='Time (s)',ylabel='Mean-normalized plane HRR' if row==2 else 'Heat release (kW)')
  axes[row,1].set(xlabel='Frequency (Hz)',ylabel='Normalized PSD (1/Hz)' if row==2 else 'PSD (kW²/Hz)')
  for ax in axes[row]:ax.legend(fontsize=7);ax.grid(alpha=.2)
  if row>0:axes[row,0].set_title('FDS physical regional/plane HRR not retained')
 fig.savefig(OUT/'retained_forcing.png',dpi=150)

if __name__=='__main__':main()
