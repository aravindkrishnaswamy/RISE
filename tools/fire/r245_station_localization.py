#!/usr/bin/env python3
"""Read pinned r244 checkpoint station histories; no replay or physical qualification."""
import csv
import hashlib
import json
import pathlib
import struct
import numpy as np
from r244_spectrum_report import EXTERNAL, ROOT, rows, require, sha, plt

OUT = ROOT/'docs/data/fire_r245'
WORK = EXTERNAL/'r245_station_localization.v1'
PINS = {
    'baseline': ('r244_tier6_baseline.v1', '5500362fa825928bdc6b8cefd7464e5bd3b70bd729ee8672144b2ff9893e6a13'),
    'lateral': ('r244_tier6_lateral_double.v1', 'f419de029f152bb6eecef6b2f045ea7ce7e3375d22f60507a89eeb03cfddedf4')}


def extract(variant):
    folder, expected = PINS[variant]
    path = EXTERNAL/folder/'production_final.checkpoint'
    payload = path.read_bytes()
    require(hashlib.sha256(payload).hexdigest() == expected, 'unrecognized checkpoint bytes')
    require(payload[:16] == b'RISEFIRECHKPT1\0\0', 'checkpoint magic')
    version, endian, size, checksum = struct.unpack_from('<QQQQ', payload, 16)
    require(version == 13 and endian == 0x0102030405060708 and size+48 == len(payload), 'checkpoint header')
    # This is deliberately a pinned block extractor, not a general checkpoint loader.
    # WriteSolverFrameValues serializes probeTimeS immediately before the vectors below.
    signal = rows(EXTERNAL/f'r244_tier6_{variant}_spectrum.v1/centerline_signal.csv')
    times = np.array([float(r['time_s']) for r in signal], dtype='<f8')
    heat = np.array([float(r['centerline_heat_release_W']) for r in signal], dtype='<f8')
    anchor = struct.pack('<Q', len(times))+times.tobytes()
    offset = payload.find(anchor)
    require(offset >= 48 and payload.find(anchor, offset+1) == -1, 'probe anchor not unique')
    anchor_offset = offset
    fields = {}
    for name in ['probeTimeS', 'probeCenterlineHeatReleaseW', 'acceptedMaximumEOSResidualHistory',
                 'acceptedTimeStepHistoryS', 'stationProbeTimeS', 'stationProbeHeightM',
                 'stationProbeTemperatureK', 'stationProbeReactionWPerM3', 'stationProbeVerticalVelocityMPerS']:
        count, = struct.unpack_from('<Q', payload, offset); offset += 8
        require(count <= 1000000 and offset+8*count <= len(payload), 'invalid vector extent')
        fields[name] = np.frombuffer(payload, dtype='<f8', count=count, offset=offset).copy()
        offset += 8*count
        require(np.isfinite(fields[name]).all(), 'nonfinite vector')
    require(np.array_equal(fields['probeTimeS'], times) and
            np.array_equal(fields['probeCenterlineHeatReleaseW'], heat), 'native probe binding')
    diagnostics = rows(EXTERNAL/folder/'monitored_diagnostics.csv')
    require(np.array_equal(fields['acceptedTimeStepHistoryS'], [float(r['dt_s']) for r in diagnostics]),
            'accepted dt binding')
    # EOS residual and post-restoration manifold deviation are distinct diagnostics.
    require(len(fields['acceptedMaximumEOSResidualHistory']) == len(diagnostics),
            'accepted EOS history coverage')
    count = len(times)
    heights = fields['stationProbeHeightM'].reshape(count, 80)
    require(np.array_equal(fields['stationProbeTimeS'].reshape(count, 80), np.repeat(times[:, None], 80, axis=1)),
            'station time binding')
    require(np.array_equal(heights, np.repeat(heights[:1], count, axis=0)), 'height ordering')
    dx = .040790821423214683
    require(np.array_equal(heights[0], (np.arange(80)+.5)*dx), 'station heights changed')
    values = {name: fields[field].reshape(count, 80) for name, field in [
        ('temperature_K', 'stationProbeTemperatureK'), ('reaction_W_per_m3', 'stationProbeReactionWPerM3'),
        ('vertical_velocity_m_per_s', 'stationProbeVerticalVelocityMPerS')]}
    reaction_integral = values['reaction_W_per_m3'].sum(axis=1)*dx**3
    check = dict(checkpoint_sha256=expected, checkpoint_path=str(path), version=version,
                 probe_block_offset=anchor_offset, probe_block_end=offset,
                 probe_samples=count, heights=80, first_time_s=float(times[0]), last_time_s=float(times[-1]),
                 centerline_reaction_reconstruction_max_abs_W=float(abs(reaction_integral-heat).max()),
                 centerline_reaction_reconstruction_max_abs_over_peak=float(abs(reaction_integral-heat).max()/max(abs(heat))))
    return times, heights[0], values, check


def analyze(times, y):
    # Full observation window retained. This sensitivity sampling avoids the native
    # drastic512-point downsample but is not an antialiasing or uncertainty certificate.
    n = 8192
    t = np.linspace(times[0], times[-1], n)
    uniform = np.stack([np.interp(t, times, y[:, z]) for z in range(80)], axis=1)
    x = np.arange(n, dtype=float); x -= x.mean()
    residual = uniform-uniform.mean(axis=0)-x[:, None]*(x@uniform/np.dot(x, x))[None, :]
    power = abs(np.fft.rfft(residual*np.hanning(n)[:, None], axis=0))**2
    frequency = np.fft.rfftfreq(n, d=(times[-1]-times[0])/(n-1))
    return frequency, power


def main():
    OUT.mkdir(exist_ok=True); WORK.mkdir(exist_ok=True)
    reports = {}; summaries = []; retained = {}
    for variant in PINS:
        t, h, values, check = extract(variant)
        retained[variant] = (t, h, values)
        np.savez_compressed(WORK/f'{variant}_station_history.npz', time_s=t, height_m=h, **values)
        check['retained_history_sha256'] = sha(WORK/f'{variant}_station_history.npz')
        reports[variant] = check
        for name, y in values.items():
            f, power = analyze(t, y)
            expected = int(np.argmin(abs(f-1.5/np.sqrt(.30))))
            for z in range(80):
                p = power[:, z]; total = p[1:].sum(); peak = 1+int(np.argmax(p[1:]))
                summaries.append(dict(variant=variant, observable=name, height_index=z, height_m=h[z],
                    # Descriptive sample statistics; not time-integrated empirical rows.
                    sample_mean=y[:, z].mean(), sample_std=y[:, z].std(), dominant_hz=f[peak] if total else None,
                    lowest_two_bin_positive_power_fraction=float(p[1:3].sum()/total) if total else None,
                    expected_three_bin_positive_power_fraction=float(p[expected-1:expected+2].sum()/total) if total else None))
    with (OUT/'station_spectral_summary.csv').open('w', newline='') as stream:
        writer=csv.DictWriter(stream, fieldnames=list(summaries[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(summaries)
    fig, axes = plt.subplots(3, 2, figsize=(12, 10), constrained_layout=True)
    for col, variant in enumerate(PINS):
        t, h, values = retained[variant]
        for row, (name, y) in enumerate(values.items()):
            f, p = analyze(t, y)
            positive_peak = p[1:].max(axis=0)
            normalized = np.divide(p, positive_peak, out=np.zeros_like(p), where=positive_peak>0)
            select = (f>0)&(f<=4)
            im = axes[row,col].pcolormesh(f[select], h, normalized[select].T, shading='nearest',
                cmap='magma', vmin=0, vmax=1)
            axes[row,col].axvline(1.5/np.sqrt(.30), color='cyan', linestyle='--', linewidth=1)
            axes[row,col].set(xlabel='Frequency (Hz)', ylabel='Height (m)', title=f'{variant}: {name}')
    fig.colorbar(im, ax=axes, label='Power / own station peak')
    fig.suptitle('r245 preliminary localization — centerline stations, full r244 windows\n'
                 '8192-point processing sensitivity; descriptive signals, not signed budgets')
    fig.savefig(WORK/'station_spectra.png', dpi=140); plt.close(fig)
    result=dict(status='preliminary_station_localization_not_causal_budget_verdict', inputs=reports,
        extraction='Pinned checkpoint SHA-256 + unique complete native probe-time vector anchor; next vectors read in WriteSolverFrameValues order. Full native loader not reimplemented; no checkpoint mutation or resume.',
        processing='8192 uniform interpolated samples over entire observation span, affine detrend, symmetric Hann; no shortening; descriptive bands bins1–2 and expected nearest±1. Null dominant/fractions indicate zero positive power.',
        limitations=['Central2x2 columns are not plume-following; lateral displacement can reduce measured reaction/temperature.',
                    'Interpolated sampling is not an antialiasing certificate. No period uncertainty or physical gate is inferred.',
                    'Time-resolved signed pressure, advection, stress, buoyancy and per-boundary mass/enthalpy fluxes are absent from these retained histories.',
                    'Final checkpoint and VDB fields cannot reconstruct historical accepted R0/R1/R2 face budgets without retained operands or a new instrumented full run.'],
        summary_sha256=sha(OUT/'station_spectral_summary.csv'), figure_path=str(WORK/'station_spectra.png'),
        figure_sha256=sha(WORK/'station_spectra.png'), script_sha256=sha(pathlib.Path(__file__)))
    (OUT/'station_localization.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps(reports, indent=2))


if __name__ == '__main__': main()
