#!/usr/bin/env python3
"""Report the three requested tier-8 observables from existing accepted ledgers."""
import argparse
import csv
from decimal import Decimal
import hashlib
import io
import json
import math
from pathlib import Path

def height_at_power(rows, target):
    cumulative=0.0
    for row in rows:
        power=float(row['mean_heat_W'])
        if cumulative+power>=target and power>0:
            lo=float(row['lower_m']);hi=float(row['upper_m'])
            return lo+(hi-lo)*(target-cumulative)/power
        cumulative+=power
    return None

def measure(directory):
    summary_path=directory/'production_capstone_summary.txt'
    summary=dict(line.split('=',1) for line in summary_path.read_text().splitlines() if '=' in line)
    paths=sorted((directory/'budgets').glob('step_*.csv'))
    if len(paths)!=int(summary['accepted_steps']):raise ValueError('incomplete accepted fuel budget')
    budget=paths[-1];text=budget.read_text()
    record=list(csv.DictReader(io.StringIO('\n'.join(x for x in text.splitlines() if not x.startswith('#')))))
    row=next(r for r in record if r['scope']=='statistics' and r['field']=='1')
    q={k:Decimal(v) for k,v in row.items() if k not in ('scope','field','units')}
    inlet=sum(q[k+'_in'] for k in ('xmin','xmax','ymin','ymax','bottom','top'))
    outlet=sum(q[k+'_out'] for k in ('xmin','xmax','ymin','ymax','bottom','top'))
    burned=-q['source_increment'];change=q['ending_inventory']-q['beginning_inventory']
    residual=change+burned-inlet+outlet
    profile_path=directory/'budgets/heat_release_height.csv'
    profile=list(csv.DictReader(profile_path.open()))
    duration=float(profile[0]['statistics_duration_s'])
    realized=sum(float(r['mean_heat_W']) for r in profile)
    below=0.0
    for r in profile:
        lo=float(r['lower_m']);hi=float(r['upper_m']);p=float(r['mean_heat_W'])
        below+=p*max(0.,min(hi,1.5)-lo)/(hi-lo)
    nominal=33000.0;heskestad=.235*33.0**.4-1.02*.30
    h99=height_at_power(profile,.99*nominal)
    result=dict(scope='complete_tier8_statistics_window_accepted_R1_flux_and_source_integrals',
        accepted_time_s=float(summary['accepted_time_s']),statistics_duration_s=duration,
        supplied_kg=float(inlet),consumed_kg=float(burned),consumed_fraction=float(burned/inlet),
        consumed_at_least_95_percent=burned>=Decimal('.95')*inlet,
        beginning_inventory_kg=float(q['beginning_inventory']),ending_inventory_kg=float(q['ending_inventory']),inventory_change_kg=float(change),
        outward_methane_kg=float(outlet),top_outward_methane_kg=float(q['top_out']),top_outward_fraction_of_supply=float(q['top_out']/inlet),
        balance_residual_kg=float(residual),reported_solver_residual_kg=float(q['closure_residual']),interstep_change_kg=float(q['interstep_inventory_change']),
        balance_residual_fraction_of_supply=float(residual/inlet),
        mean_hrr_W=realized,heat_below_1_5m_fraction=below/realized if realized else None,
        mean_flame_height_99_percent_input_m=h99,heskestad_expected_m=heskestad,
        flame_height_relative_error=(h99/heskestad-1) if h99 is not None else None,
        height_99_percent_realized_heat_m=height_at_power(profile,.99*realized),
        height_definition='99% of nominal input HRR integrated vertically through the time-averaged plane profile; null if unreached. Intracell interpolation. Realized-heat quantile reported separately.',
        flame_height_reference='https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=907227 (p.7; nominal-input normalization)',
        limitations=['Statistics initial inventory uses the existing clipped-linear increment, not a saved interior state.',
                    'No new numerical conservation tolerance or near-zero export cutoff is imposed; report residual and export relative to supply.',
                    'This HRR-based flame-height proxy differs from the visual intermittency definition of experimental Heskestad data.'],
        artifacts={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [summary_path,budget,profile_path,Path(__file__)]})
    if not all(math.isfinite(v) for v in result.values() if type(v) is float):raise ValueError('nonfinite observation')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=measure(args.directory)
    rendered=json.dumps(result,indent=2,allow_nan=False)+'\n'
    if args.output:args.output.write_text(rendered)
    print(rendered,end='')
