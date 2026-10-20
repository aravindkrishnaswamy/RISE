#!/usr/bin/env python3
"""Read-only centerline spectra and available radial profiles; no simulation."""
import csv,json,sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(Path(__file__).parent))
from r255_reduce_fds import csv_data
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration')
OUT=ROOT/'docs/data/fire_r257';OUT.mkdir(exist_ok=True)
START=10.466040127212182;DX=.030593116067411014

def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)

def welch(x,**options):
 # SciPy-compatible periodic Hann, least-squares linear segment detrend,
 # one-sided density scaling. Six 64-sample segments for a 235-sample record.
 assert options==dict(fs=16,window='hann',nperseg=64,noverlap=32,detrend='linear',scaling='density')
 n=64;window=.5-.5*np.cos(2*np.pi*np.arange(n)/n)
 design=np.column_stack([np.ones(n),np.arange(n)])
 powers=[]
 for start in range(0,len(x)-n+1,32):
  segment=np.asarray(x[start:start+n]);segment=segment-design@np.linalg.lstsq(design,segment,rcond=None)[0]
  power=np.abs(np.fft.rfft(segment*window))**2/(16*np.sum(window**2));power[1:-1]*=2
  powers.append(power)
 return np.fft.rfftfreq(n,1/16),np.mean(powers,axis=0)

def main():
 rise=np.genfromtxt(BASE/'r257_readonly_comparison/r251_centerline.csv',names=True,delimiter=',')
 dev=csv_data(BASE/'r255_comparison/fds_run/r255_fds_devc.csv')
 rad=list(csv.DictReader((ROOT/'docs/data/fire_r254/frames_radial_profiles.csv').open()))
 widths=list(csv.DictReader((ROOT/'docs/data/fire_r254/frames_height_profile.csv').open()))
 spectra=[];summary=[];series=[];profiles=[];half=[]
 fig,axes=plt.subplots(3,2,figsize=(11,10),layout='constrained')
 rfig,ra=plt.subplots(3,2,figsize=(11,10),layout='constrained')
 for row,z in enumerate([32,65,98]):
  p=rise[rise['z']==z];t=START+p['frame']/16
  assert len(t)==235 and dev['Time'][0]<=t[0]
  # FDS device CSV prints E15.7: at 25s the last unit is 1 microsecond.
  assert abs(dev['Time'][-1]-t[-1])<=.5e-6
  dev['Time'][-1]=t[-1]
  fields=[('theta',p['theta_K'],dev[f'T{z:03}']+273.15-300.,'Temperature rise (K)'),('w',p['w_m_s'],.5*(dev[f'WF{z:03}']+dev[f'WF{z+1:03}']),'Vertical velocity (m/s)')]
  for col,(key,a,raw,label) in enumerate(fields):
   b=np.interp(t,dev['Time'],raw)
   for time,av,bv in zip(t,a,b):series.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'time_s':time,'r251':av,'fds':bv})
   f,pa=welch(a,fs=16,window='hann',nperseg=64,noverlap=32,detrend='linear',scaling='density')
   _,pb=welch(b,fs=16,window='hann',nperseg=64,noverlap=32,detrend='linear',scaling='density')
   ma=float(a.mean());mb=float(b.mean())
   for i in range(len(f)):spectra.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'frequency_Hz':f[i],'r251_PSD':pa[i],'fds_PSD':pb[i],'r251_PSD_over_mean_squared':pa[i]/ma**2,'fds_PSD_over_mean_squared':pb[i]/mb**2})
   r={'z':z,'quantity':key,'height_m':(z+.5)*DX,'r251_mean':ma,'fds_mean':mb,'r251_rms':float(np.std(a)),'fds_rms':float(np.std(b)),'r251_rms_over_mean':float(np.std(a)/ma),'fds_rms_over_mean':float(np.std(b)/mb)}
   for lo,hi in [(0.25,1),(1,3),(3,6),(6,8)]:
    mask=(f>=lo)&(f<hi);ea=float(pa[mask].sum()*.25);eb=float(pb[mask].sum()*.25)
    r[f'band_{lo}_{hi}_absolute_ratio']=ea/eb;r[f'band_{lo}_{hi}_normalized_ratio']=(ea/ma**2)/(eb/mb**2)
   summary.append(r)
   ax=axes[row,col];ax.semilogy(f[1:],pa[1:],label='RISE r251');ax.semilogy(f[1:],pb[1:],label='FDS VLES / Superbee');ax.set(xlabel='Frequency (Hz)',ylabel=label+' squared / Hz',title=f'z={(z+.5)*DX:.3f} m');ax.legend();ax.grid(alpha=.25)
   rings=[v for v in rad if int(v['z'])==z]
   for v in rings:profiles.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'r_m':float(v['r_m']),'r251_mean':float(v[key+'_mean']),'fds_mean':'unavailable: no retained T/w cross-sectional fields'})
   w=widths[z];half.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'r251_halfwidth_m':float(w[key+'_halfwidth_m']),'fds_halfwidth_m':'unavailable'})
   ax=ra[row,col];ax.plot([float(v['r_m']) for v in rings],[float(v[key+'_mean']) for v in rings],label='RISE r251');ax.axvline(float(w[key+'_halfwidth_m']),color='C0',ls=':',label='RISE half-width');ax.set(xlabel='Radius (m)',ylabel=label,title=f'z={(z+.5)*DX:.3f} m');ax.text(.98,.85,'FDS profile unavailable',ha='right',transform=ax.transAxes);ax.legend();ax.grid(alpha=.25)
 fig.savefig(OUT/'centerline_spectra.png',dpi=160);rfig.savefig(OUT/'available_radial_profiles.png',dpi=160)
 write('spectra.csv',spectra);write('centerline_timeseries.csv',series);write('spectral_summary.csv',summary);write('available_radial_profiles.csv',profiles);write('halfwidths.csv',half)
 result={'statistics_start_s':float(t[0]),'statistics_end_s':float(t[-1]),'samples':235,'sample_rate_Hz':16,'spectra_method':'Welch Hann 64 samples, 32 overlap, segment linear detrending; 0.25Hz bins; common RISE time grid, FDS interpolated from instantaneous 32Hz devices; absolute and mean-squared normalized PSD retained','caveats':['Only six overlapping 4-second segments; short nonstationary record, no causal damping attribution from spectral amplitudes alone.','No pre-decimation anti-alias filter; interpret near 8Hz cautiously.','FDS radial T/w histories were not retained; mass flux does not determine T and w separately.','FDS smoke3d effective flame temperature is a visualization quantity, not an off-axis thermodynamic-temperature/velocity record.'],'summary':summary,'halfwidths':half,'decision':'pending measured interpretation'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
