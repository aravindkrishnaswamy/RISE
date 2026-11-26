#!/usr/bin/env python3
"""Compare the complete matched r244 spectra without changing physical gates."""
import csv
import json
import pathlib
import numpy as np
from r244_spectrum_report import EXTERNAL, OUT, sha, rows, require, plt

VARIANTS = ['baseline', 'lateral']
NAMES = ['centerline_heat_release', 'display_lit_area']


def main():
    audits = {v: json.loads((OUT/f'{v}_spectrum_audit.json').read_text()) for v in VARIANTS}
    spectra = {}
    artifacts = []
    for v in VARIANTS:
        directory = EXTERNAL/f'r244_tier6_{v}_spectrum.v1'
        path = directory/'full_spectrum.csv'
        recorded = next(a['sha256'] for a in audits[v]['source_artifacts']
                        if a['path'] == str(path))
        require(sha(path) == recorded, 'spectrum changed since audit')
        spectra[v] = rows(path)
        artifacts.append(dict(path=str(path), sha256=recorded))
    report = {}
    combined = []
    fig, axes = plt.subplots(2, 2, figsize=(12, 7), constrained_layout=True)
    for row, name in enumerate(NAMES):
        data = {}
        for v in VARIANTS:
            selected = [r for r in spectra[v] if r['signal'] == name]
            require(len(selected) == 257, 'missing native bins')
            f = np.array([float(r['frequency_Hz']) for r in selected])
            power = np.array([float(r['power']) for r in selected])
            require(np.isfinite(power).all() and (power >= 0).all(), 'invalid power')
            data[v] = (f, power)
        f, base = data['baseline']
        wf, wide = data['lateral']
        require(np.array_equal(f, wf), 'frequency grids differ')
        b = audits['baseline']['signals'][name]
        w = audits['lateral']['signals'][name]
        require(b['input_samples'] == w['input_samples'] and
                b['first_time_s'] == w['first_time_s'] and
                b['last_time_s'] == w['last_time_s'], 'observation windows differ')
        expected = int(np.argmin(abs(f-1.5/np.sqrt(.30))))
        slow = int(np.argmin(abs(f-.137)))
        bp = 1+int(np.argmax(base[1:]))
        wp = 1+int(np.argmax(wide[1:]))
        report[name] = dict(baseline=b, lateral=w,
            lowest_nonzero_hz=float(f[1]),
            wide_over_baseline_raw_power_at_lowest_bin=float(wide[1]/base[1]),
            wide_over_baseline_raw_power_at_slow_bin=float(wide[slow]/base[slow]),
            wide_over_baseline_raw_power_at_expected_bin=float(wide[expected]/base[expected]))
        # A descriptive three-bin neighborhood, not a physical acceptance band.
        report[name]['expected_three_bin_neighborhood_hz'] = [float(f[expected-1]), float(f[expected+1])]
        report[name]['expected_three_bin_power_fraction_excluding_DC'] = {
            v: float(data[v][1][expected-1:expected+2].sum()/data[v][1][1:].sum())
            for v in VARIANTS}
        if name == 'centerline_heat_release':
            sensitivity = {}
            for v in VARIANTS:
                signal_path = EXTERNAL/f'r244_tier6_{v}_spectrum.v1/centerline_signal.csv'
                signal_digest = next(a['sha256'] for a in audits[v]['source_artifacts']
                                     if a['path'] == str(signal_path))
                require(sha(signal_path) == signal_digest, 'centerline changed since audit')
                artifacts.append(dict(path=str(signal_path), sha256=signal_digest))
                signal = rows(signal_path)
                t = np.array([float(r['time_s']) for r in signal])
                y = np.array([float(r['centerline_heat_release_W']) for r in signal])
                samples = 8192
                x = np.arange(samples, dtype=float)
                uniform = np.interp(np.linspace(t[0], t[-1], samples), t, y)
                xc = x-x.mean()
                residual = uniform-uniform.mean()-xc*np.dot(xc, uniform-uniform.mean())/np.dot(xc, xc)
                p = abs(np.fft.rfft(residual*np.hanning(samples)))**2
                sf = np.fft.rfftfreq(samples, d=(t[-1]-t[0])/(samples-1))
                peak = 1+int(np.argmax(p[1:]))
                nearest = int(np.argmin(abs(sf-1.5/np.sqrt(.30))))
                sensitivity[v] = dict(samples=samples, dominant_hz=float(sf[peak]),
                    expected_nearest_hz=float(sf[nearest]), expected_to_peak_power_ratio=float(p[nearest]/p[peak]),
                    positive_power_fraction_above_native_resampled_nyquist=float(p[sf>f[-1]].sum()/p[1:].sum()))
            report[name]['8192_sample_processing_sensitivity_not_replacement'] = sensitivity
        for k in range(len(f)):
            combined.append(dict(signal=name, bin=k, frequency_Hz=f[k],
                baseline_power=base[k], lateral_power=wide[k],
                baseline_power_over_peak=base[k]/base[bp],
                lateral_power_over_peak=wide[k]/wide[wp]))
        label = 'Centerline heat release' if row == 0 else 'Thresholded spectral-preview lit area'
        for col in range(2):
            ax = axes[row, col]
            for v, color in [('baseline', '#126782'), ('lateral', '#b45d25')]:
                vf, p = data[v]
                divisor = max(p[1:]) if col else 1.0
                ax.semilogy(vf[1:], p[1:]/divisor, color=color,
                            label='Baseline' if v == 'baseline' else 'Doubled lateral domain')
            ax.axvline(1.5/np.sqrt(.30), color='#555555', linestyle='--', linewidth=1,
                       label='Expected 2.739 Hz')
            ax.axvline(f[slow], color='#888888', linestyle=':', linewidth=1)
            unit = 'W²' if row == 0 else 'pixel-count²'
            ax.set(xlim=(0, 4), xlabel='Frequency (Hz)',
                   ylabel='Power / own peak' if col else f'Raw DFT power ({unit})',
                   title=label+(' — shape' if col else ' — same scale'))
            ax.grid(alpha=.2)
            if row == 0: ax.legend(fontsize=8)
    fig.suptitle('r244: doubling the lateral domain does not establish expected puffing\n'
                 'Full matched 14.625 s windows; affine detrend + Hann; tier 6 is diagnostic', fontsize=12)
    fig.savefig(OUT/'domain_comparison_spectra.svg', metadata={'Date': None})
    fig.savefig(EXTERNAL/'r244_tier6_lateral_spectrum.v1/domain_comparison_spectra.png', dpi=150)
    plt.close(fig)
    with (OUT/'domain_comparison_full_spectrum.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(combined[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(combined)
    result = dict(status='domain_doubling_does_not_resolve_puffing_budget_localization_required',
        artifact_fidelity='simulation_evidence', signals=report, source_artifacts=artifacts,
        method='Same observation span, 512-point linear interpolation, affine detrend, symmetric Hann; '
               'all 257 native bins retained in CSV. Figure shows 0–4 Hz.',
        limits=['Raw powers compare the same observable and DFT convention; they are not a PSD or cross-observable energy.',
                'The first two bins cover only approximately one and two cycles; low-frequency power can reflect nonstationarity, not a confirmed periodic recirculation mode.',
                'Lit area depends on the fixed exposure, threshold, finite camera view and spectral emission; it is not a direct flow or flame-height measurement.',
                'Native lit-area Nyquist is 8 Hz; interpolated bins above this contain no additional independent information.',
                'Native centerline processing downsamples by interpolation without an antialias filter. An 8192-sample sensitivity is separately reported; its ratios differ, though the dominant frequency classes and unresolved verdict persist. Native evidence is unchanged.',
                'Tier 6 has 7.35 cells across the burner; no refined empirical puffing qualification or domain-multiplier amendment follows from this comparison.',
                'Single realization per variant; no uncertainty interval or causal boundary diagnosis is claimed.'],
        next_step='Budget localization before proposing a solver or boundary fix; no soot/tier-10 window yet.',
        verifier_sha256=sha(pathlib.Path(__file__)),
        figure_sha256=sha(OUT/'domain_comparison_spectra.svg'),
        csv_sha256=sha(OUT/'domain_comparison_full_spectrum.csv'))
    (OUT/'domain_comparison.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
