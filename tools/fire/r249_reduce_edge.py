#!/usr/bin/env python3
"""Reduce the one-cell edge variant on r249's retained r241 observations."""
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
ROOT = Path(__file__).resolve().parents[2]
WORK = Path('/Volumes/SSD/RISE/fire_production_calibration/r249_tier8_fuel_balance')
OUT = ROOT/'docs/data/fire_r249'

def main():
    rows = list(csv.DictReader((WORK/'edge_mass_samples.csv').open()))
    layers = list(csv.DictReader((WORK/'edge_height_samples.csv').open()))
    baseline = list(csv.DictReader((OUT/'eligibility_mass_samples.csv').open()))
    assert len(rows) == len(baseline) == 156 and len(layers) == len(rows)*106
    assert [r['step'] for r in rows] == [r['step'] for r in baseline]
    times = np.array([float(r['time_s']) for r in rows])
    duration = times[-1]-times[0]
    fields = ['b_kg', 'c_kg', 'e_kg']
    values = np.array([[float(r[k]) for k in fields] for r in layers]).reshape(156,106,3)
    heights = np.array([float(r['height_m']) for r in layers[:106]])
    for j,k in enumerate(fields):
        assert np.max(abs(values[:,:,j].sum(axis=1)-[float(r[k]) for r in rows])) < 1e-15
        if k != 'e_kg':
            assert max(abs(float(a[k])-float(b[k])) for a,b in zip(rows,baseline)) < 1e-15
    mean = np.trapezoid(values,times,axis=0)/duration
    totals = mean.sum(axis=0)
    below = mean[heights<1].sum(axis=0)
    ratio = below[2]/below[1]
    with (OUT/'edge_height_profile.csv').open('w') as stream:
        w=csv.writer(stream,lineterminator='\n');w.writerow(['z','height_m']+['mean_'+k for k in fields])
        for z in range(106):w.writerow([z,heights[z]]+list(mean[z]))
    with (OUT/'edge_mass_samples.csv').open('w') as stream:
        w=csv.DictWriter(stream,fieldnames=rows[0],lineterminator='\n');w.writeheader();w.writerows(rows)
    bands=[]
    for lo,hi in [(0,.25),(.25,.5),(.5,1),(1,2),(2,4)]:
        mask=(heights>=lo)&(heights<hi)
        bands.append(dict(lower_m=lo,upper_m=min(hi,106*heights[0]*2),**{k:float(mean[mask,j].sum()) for j,k in enumerate(fields)}))
    paths=[WORK/'edge_mass_samples.csv',WORK/'edge_height_samples.csv',WORK/'edge_build.sh',WORK/'edge_build.log',WORK/'native_edge_probe',ROOT/'tools/fire/r249_eligibility_probe.cpp',Path(__file__),OUT/'edge_mass_samples.csv',OUT/'edge_height_profile.csv']
    result=dict(scope='offline_retained_state_one_cell_flame_edge',baseline_commit='a2ce95979',states=156,start_s=times[0],end_s=times[-1],
        definition='Keep warm map immutable; waive 600 K only beside warm parcel-passing vertices; apply unchanged warm seeds and face graph plus floor exception.',
        mean_eligible_kg=dict(zip(fields,map(float,totals))),mean_eligible_below_1m_kg=dict(zip(fields,map(float,below))),
        e_fraction_of_c_below_1m=float(ratio),e_fraction_below_1m=float(below[2]/totals[2]),
        e_fraction_of_c_total=float(totals[2]/totals[1]),chosen_variant='e' if ratio>.5 else 'c',
        decision='Most means a majority of (c) eligible mass below 1 m; report total retention separately.',height_bands=bands,
        artifacts={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths})
    (OUT/'edge_comparison.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2))
if __name__ == '__main__':main()
