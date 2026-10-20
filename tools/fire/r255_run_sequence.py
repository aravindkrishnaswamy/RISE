#!/usr/bin/env python3
"""One bounded, explicitly authorized serial campaign; no scheduler or recurrence."""
from pathlib import Path
import csv,hashlib,json,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r255_comparison');OUT=ROOT/'docs/data/fire_r255'
PYTHON=Path('/Users/aravind/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3')
EXPECTED='d1cebdd779a5d18a9e5b92ec162ddfcfc51aaae1a8963104bcdc633fb7bab033'
def status(stage,**more):
 (BASE/'campaign_status.json').write_text(json.dumps({'stage':stage,'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),**more},indent=2)+'\n');print(stage,more,flush=True)
def run(stage,args,log,stdout_path=None,env=None):
 status(stage)
 with Path(log).open('w') as errors:
  if stdout_path:
   with Path(stdout_path).open('w') as output:
    p=subprocess.Popen([str(a) for a in args],cwd=ROOT,stdout=output,stderr=errors,env=env);status(stage,pid=p.pid);code=p.wait()
  else:
   p=subprocess.Popen([str(a) for a in args],cwd=ROOT,stdout=errors,stderr=subprocess.STDOUT,env=env);status(stage,pid=p.pid);code=p.wait()
 if code:raise RuntimeError(f'{stage} exited {code}; see {log}')
def completed_table():
 f=json.loads((OUT/'fds_results.json').read_text());t=json.loads((OUT/'tier6_results.json').read_text());l=json.loads((OUT/'limiter_results.json').read_text())
 s=dict(x.split('=',1) for x in (BASE/'tier6_run/production_capstone_summary.txt').read_text().splitlines() if '=' in x)
 t6q=float(s['integrated_fuel_kg'])*50027364.880448513/(t['statistics_duration_s']*1000)
 fm=f['selected_mass'][-1];tm=t['selected_mass'][-1]
 rows=[['Maximum McCaffrey ΔT error','+87.29%',f"{100*f['max_temperature_error_station']['fds_temperature_relative_error']:+.2f}%",f"{100*t['max_temperature_error_station']['temperature_relative_error']:+.2f}%",'—'],['Maximum McCaffrey w error','+25.01%',f"{100*f['max_velocity_error_station']['fds_velocity_relative_error']:+.2f}%",f"{100*t['max_velocity_error_station']['velocity_relative_error']:+.2f}%",'—'],['Realized HRR (kW)','32.837',f"{f['mean_hrr_kW']:.3f}",f'{t6q:.3f}','—'],['R<0.8m mass near 3m (kg/s)','0.7424 @3.013m',f"{fm['mass_r08_kg_s']:.4f} @{fm['height_m']:.3f}m",f"{tm['mass_r08_kg_s']:.4f} @{tm['height_m']:.3f}m",'—'],['Heskestad mass there (kg/s)','1.3564',f"{fm['heskestad_total_kg_s']:.4f}",f"{tm['heskestad_total_kg_s']:.4f}",'—']]
 for r in l['selected']:rows.append([f"Limiter at slab {r['height_m']:.3f}m: fraction / mean(1−alpha)",'not retained','—','—',f"{100*r['limited_fraction']:.2f}% / {r['mean_low_order_blend']:.6f}"])
 table='| Quantity | r251 tier8 | FDS6.11.1 | Tier6 | Limiter continuation |\n|---|---:|---:|---:|---:|\n'+''.join('| '+' | '.join(r)+' |\n' for r in rows)
 (BASE/'comparison_table.md').write_text(table+'\nAll simulations have stopped. Raw outputs and reductions need final owner review.\n')
 (OUT/'comparison_table.md').write_text(table)

def main():
 raise RuntimeError('r255 sequence superseded: tier6 cancelled by owner; use r256 observer diagnostic')
if __name__=='__main__':
 try:main()
 except Exception as e:status('stopped_error',error=str(e));raise
