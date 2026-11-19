#!/usr/bin/env python3
"""Audit and plot either completed r244 variant; never reinterpret a short window."""
import argparse
import csv
import hashlib
import json
import pathlib
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use('Agg')
matplotlib.rcParams['svg.hashsalt'] = 'rise-r244-baseline-v1'
import matplotlib.pyplot as plt

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXTERNAL = pathlib.Path('/Volumes/SSD/RISE/fire_production_calibration')
SIM = EXTERNAL / 'r244_tier6_baseline.v1'
PREVIEW = EXTERNAL / 'r244_tier6_baseline_preview.v1'
SPECTRUM = EXTERNAL / 'r244_tier6_baseline_spectrum.v1'
OUT = ROOT / 'docs/data/fire_r244'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))

def require(ok, message):
    if not ok:
        raise ValueError(message)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['baseline', 'lateral'], default='baseline')
    variant = parser.parse_args().variant
    global SIM, PREVIEW, SPECTRUM
    if variant == 'lateral':
        SIM = EXTERNAL / 'r244_tier6_lateral_double.v1'
        PREVIEW = EXTERNAL / 'r244_tier6_lateral_preview.v1'
        SPECTRUM = EXTERNAL / 'r244_tier6_lateral_spectrum.v1'
    completion = EXTERNAL / ('r244_tier6_lateral_spectrum_launch.v1/completion.json'
        if variant == 'lateral' else 'r244_tier6_spectrum_launch.v1/completion.json')
    require(json.loads(completion.read_text())['exit_code'] == 0, 'pipeline incomplete')
    evidence = dict(line.split(' ', 1) for line in
                    (SPECTRUM / 'puffing_spectrum_evidence.v1').read_text().splitlines())
    artifacts = []
    for key, path in [('checkpoint', SIM/'production_final.checkpoint'),
                      ('frame_times', SIM/'frame_times.csv'),
                      *[(name, SPECTRUM/(name+'.csv')) for name in
                        ['display_inputs', 'centerline_signal', 'lit_area_signal', 'full_spectrum']]]:
        digest = sha(path)
        require(digest == evidence[key+'_sha256'], 'hash mismatch '+key)
        artifacts.append(dict(path=str(path), sha256=digest))
    times = rows(SIM/'frame_times.csv')
    displays = rows(SPECTRUM/'display_inputs.csv')
    lit_signal = rows(SPECTRUM/'lit_area_signal.csv')
    require(len(times) == len(displays) == len(lit_signal) == 235, 'incomplete frame coverage')
    for i, (frame, display, signal) in enumerate(zip(times, displays, lit_signal)):
        path = PREVIEW/f'temporal_display_{i:05d}.png'
        require(int(frame['frame']) == int(display['frame']) == int(signal['sample']) == i,
                'frame order mismatch')
        require(float(frame['simulation_time_s']) == float(display['time_s']) == float(signal['time_s']),
                'frame time mismatch')
        require(sha(path) == display['display_sha256'], 'foreign display')
        with Image.open(path) as image:
            require(image.size == (64, 64), 'display size changed')
            lit = int((np.asarray(image.convert('RGB')).max(axis=2) > 127).sum())
        require(lit == int(display['lit_pixels']) == int(signal['lit_pixels']), 'lit area mismatch')
    spectra = rows(SPECTRUM/'full_spectrum.csv')
    reports = {}
    fig, axes = plt.subplots(2, 2, figsize=(12, 7), constrained_layout=True)
    for row_index, (name, csv_name, value_column, label) in enumerate([
        ('centerline_heat_release', 'centerline_signal', 'centerline_heat_release_W', 'Centerline heat release'),
        ('display_lit_area', 'lit_area_signal', 'lit_pixels', 'Spectral-preview lit area')]):
        signal = rows(SPECTRUM/(csv_name+'.csv'))
        t = np.array([float(r['time_s']) for r in signal])
        y = np.array([float(r[value_column]) for r in signal])
        require(np.all(np.isfinite(y)) and np.all(np.diff(t) > 0), 'invalid signal')
        require(t[0] == float(times[0]['simulation_time_s']) and
                t[-1] == float(times[-1]['simulation_time_s']), 'observation span changed')
        uniform = np.interp(np.linspace(t[0], t[-1], 512), t, y)
        x = np.arange(512, dtype=float)
        slope = np.dot(x-x.mean(), uniform-uniform.mean())/np.dot(x-x.mean(), x-x.mean())
        residual = uniform-uniform.mean()-slope*(x-x.mean())
        independent_power = abs(np.fft.rfft(residual*np.hanning(512)))**2
        selected = [r for r in spectra if r['signal'] == name]
        require(len(selected) == 257 and [int(r['bin']) for r in selected] == list(range(257)),
                'incomplete spectrum')
        f = np.array([float(r['frequency_Hz']) for r in selected])
        power = np.array([float(r['power']) for r in selected])
        peak = 1 + int(np.argmax(power[1:]))
        require(peak == 1 + int(np.argmax(independent_power[1:])), 'independent peak mismatch')
        expected = int(np.argmin(abs(f-1.5/np.sqrt(.30))))
        slow = int(np.argmin(abs(f-.137)))
        reports[name] = dict(input_samples=len(t), first_time_s=t[0], last_time_s=t[-1],
            dominant_hz=f[peak], expected_nearest_hz=f[expected],
            expected_to_peak_power_ratio=power[expected]/power[peak],
            slow_nearest_hz=f[slow], slow_to_peak_power_ratio=power[slow]/power[peak],
            independent_fft_peak_hz=f[peak],
            independent_max_absolute_power_error_over_peak=float(abs(power-independent_power).max()/power[peak]))
        for column, upper in enumerate([f[-1], 4.0]):
            ax = axes[row_index, column]
            ax.semilogy(f[1:], power[1:]/power[peak], color=['#126782','#9c4264'][row_index])
            ax.axvline(1.5/np.sqrt(.30), color='#c66c00', linestyle='--', label='Expected 2.739 Hz')
            ax.axvline(f[slow], color='#727272', linestyle=':', label='Nearest 0.137 Hz bin')
            if row_index == 1:
                ax.axvline(8, color='#555555', linestyle='-.', label='Native 16 fps Nyquist')
            ax.set(xlim=(0, upper), xlabel='Frequency (Hz)', ylabel='Power / dominant-bin power',
                   title=f'{label} — '+('all exported bins' if column == 0 else '0–4 Hz detail'))
            ax.grid(True, which='major', alpha=.2)
            if (row_index == 0 and column == 1) or (row_index == 1 and column == 0):
                ax.legend(fontsize=8)
    fig.suptitle(f'r244 matched tier-6 {variant} — full 14.625 s statistics window\n'
                 '512-sample interpolation + affine detrend + symmetric Hann; puffing unresolved', fontsize=12)
    fig.savefig(OUT/f'{variant}_spectra.svg', metadata={'Date': None})
    fig.savefig(SPECTRUM/f'{variant}_spectra.png', dpi=150)
    plt.close(fig)
    (OUT/f'{variant}_full_spectrum.csv').write_bytes((SPECTRUM/'full_spectrum.csv').read_bytes())
    report = dict(status='single_variant_audited_comparison_separate', variant=variant, artifact_fidelity='simulation_evidence',
        note='Interpolation above the native 8 Hz lit-area Nyquist adds no independent information. '
             'FFT disagreement is reported, not used to change a physical gate. '
             'Source/display provenance was verified by the native preview producer; this audit checks '
             'the exported hashes, complete time span and decoded pixel counts.',
        source_artifacts=artifacts, signals=reports, verifier_sha256=sha(pathlib.Path(__file__)),
        figure_sha256=sha(OUT/f'{variant}_spectra.svg'))
    (OUT/f'{variant}_spectrum_audit.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n')
    print(json.dumps(reports, indent=2))

if __name__ == '__main__':
    main()
