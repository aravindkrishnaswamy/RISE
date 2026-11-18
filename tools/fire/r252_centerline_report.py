#!/usr/bin/env python3
"""Reconstruct existing McCaffrey rows from persisted full-window integrals."""
import csv, hashlib, json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'docs/data/fire_r252'

def analyze(label, path):
    lines = [line.split() for line in path.read_text().splitlines()]
    fields = {line[0]: line[1] for line in lines if len(line) == 2}
    duration = float(fields['retained_centerline_statistics_duration_s'])
    stations = [[int(line[1]), *map(float, line[2:])] for line in lines
                if line[0] == 'retained_station_integrals']
    dx = 2*stations[0][1]
    tip = max(row[1] for row in stations if row[4] >= .01*max(r[4] for r in stations))
    diameter = .24474492853928809  # Existing 33 kW D*, identical to r237.
    lower = max(tip, .2*33**.4)
    upper = stations[-1][1] + dx/2 - diameter/2
    rows = []
    for index, height, temp_integral, vel_integral, heat_integral in stations:
        expected_t = 23.3*(height/33**.4)**(-5/3)
        expected_u = 1.13*33**.2*(height/33**.4)**(-1/3)
        rise = temp_integral/duration - 300
        velocity = vel_integral/duration
        rows.append(dict(station=index, height_m=height, selected=lower < height <= upper,
                         temperature_rise_K=rise, mccaffrey_temperature_rise_K=expected_t,
                         temperature_relative_error=rise/expected_t-1,
                         velocity_m_per_s=velocity, mccaffrey_velocity_m_per_s=expected_u,
                         velocity_relative_error=velocity/expected_u-1,
                         mean_plane_heat_W=heat_integral/duration))
    with (OUT/f'{label}_centerline.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n"); writer.writeheader(); writer.writerows(rows)
    selected = [row for row in rows if row['selected']]
    result = dict(input=str(path), input_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  checkpoint_sha256=fields['checkpoint_sha256'], duration_s=duration,
                  cell_width_m=dx, station_count=len(rows), selected_count=len(selected),
                  flame_tip_1percent_peak_plane_m=tip, selection_lower_exclusive_m=lower,
                  selection_upper_inclusive_m=upper, characteristic_diameter_m=diameter,
                  authored_heat_release_kW=33,
                  max_temperature_error_station=max(selected, key=lambda row: abs(row['temperature_relative_error'])),
                  max_velocity_error_station=max(selected, key=lambda row: abs(row['velocity_relative_error'])))
    return result

if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    results = {label: analyze(label, path) for label, path in [
        ('r237', OUT/'r237_retained_statistics.txt'),
        ('r251', OUT/'r251_retained_statistics.txt')]}
    results['tier10_decision'] = 'do_not_launch' if any(abs(results['r251'][key][metric]) > .15
        for key, metric in [('max_temperature_error_station','temperature_relative_error'),
                            ('max_velocity_error_station','velocity_relative_error')]) else 'launch_from_zero'
    (OUT/'centerline_results.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(results, indent=2))
