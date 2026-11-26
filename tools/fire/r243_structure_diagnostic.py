#!/usr/bin/env python3
"""NON-PHYSICAL DIAGNOSTIC: orthographic temperature-emission integration.

This is an external visualization, not a RISE physical preview or producer.
It reads the immutable r241 VDBs. No generated VDB or physical sidecar exists.
"""
import csv
import hashlib
import json
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = pathlib.Path('/Volumes/SSD/RISE/fire_production_calibration/r241_tier8_full_window.v1')
LABEL = 'NON-PHYSICAL DIAGNOSTIC'
MANIFEST_SHA = 'debf9365443bde85e5d5909e8c24b7c4c0d49804d9660eb495ae0747a3cbb346'
TIMES_SHA = 'f3254f2d1e16d6a514e275e8b619c41d572d949b52241f39f0062b009f4fe314'
KNOTS = [300, 600, 1000, 1400, 1800, 2200]
COLORS = [[0, 0, 0], [0.18, 0.04, 0.40], [0.05, 0.30, 0.70],
          [0.02, 0.72, 0.65], [0.85, 0.85, 0.08], [1, 1, 0.90]]
FONT_PATH = '/System/Library/Fonts/Supplemental/Arial.ttf'


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def record(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write('\n')


def ramp(temperature):
    return np.stack([np.interp(temperature, KNOTS, np.array(COLORS)[:, c])
                     for c in range(3)], axis=-1)


def images(temperature, dx):
    # Orthographic rays along +y. Piecewise-constant voxels, no scattering
    # or extinction. J = ramp(T)*max(T-600,0)/1600 in arbitrary units/m.
    # Fixed 0.03-m display scale; never normalized independently per frame.
    j = ramp(temperature) * np.maximum((temperature-600)/1600, 0)[..., None]
    integral = j.sum(axis=1, dtype=np.float64)*dx
    emission = 1-np.exp(-integral/0.03)
    center_slice = ramp(temperature[:, temperature.shape[1]//2, :])
    return emission, center_slice


def draw_frame(temperature, dx, index, time_s):
    canvas = Image.new('RGB', (1040, 760), '#101722')
    d = ImageDraw.Draw(canvas)
    font = ImageFont.truetype(FONT_PATH, 18)
    small = ImageFont.truetype(FONT_PATH, 15)
    title = ImageFont.truetype(FONT_PATH, 26)
    d.text((28, 18), LABEL + ' | tier-8 temperature structure', font=title, fill='#ffcd70')
    d.text((28, 57), f'Simulation t = {time_s:.6f} s | source frame {index:03d} | 16 fps, real time', font=font, fill='white')
    emission, center_slice = images(temperature, dx)
    for pos, field, heading, width, x0, nx, nz in [
            (50, emission[:40,18:51], 'Base zoom: temperature-mapped emission',437,18,33,40),
            (570, center_slice, 'Full-height central temperature slice',345,0,69,106)]:
        # Nearest pixel replication preserves cell structure, no invented detail.
        im = Image.fromarray(np.uint8(np.clip(field[::-1], 0, 1)*255))
        im = im.resize((width, 530), Image.Resampling.NEAREST)
        canvas.paste(im, (pos, 125))
        d.text((pos, 95), heading, font=font, fill='white')
        d.rectangle((pos, 125, pos+width, 655), outline='#6a7785')
        for z in ([0, 0.3, 0.6, 0.9, 1.2] if nz==40 else [0, 1, 2, 3]):
            yy = 655-int(z/(nz*dx)*530)
            d.line((pos-5, yy, pos, yy), fill='white')
            d.text((pos-40, yy-9), str(z), font=small, fill='white')
        d.text((pos, 664), f'x: {x0*dx:.3f} to {(x0+nx)*dx:.3f} m | height: {nz*dx:.3f} m', font=small, fill='white')
        # Source disc projected on x, cell-volume convention from solver.
        halfwidth = 0.15/(nx*dx)*width
        center = pos+width/2
        d.line((center-halfwidth, 654, center+halfwidth, 654), fill='#ffcd70', width=4)
    d.text((492, 350), 'z (m)', font=small, fill='white')
    bar = ramp(np.linspace(300,2200,360))[None, :, :]
    canvas.paste(Image.fromarray(np.uint8(bar*255)).resize((360,14)), (565, 702))
    for temperature in KNOTS:
        x = 565+(temperature-300)/1900*359
        d.line((x,716,x,720),fill='white')
        d.text((x,723),str(temperature),font=small,fill='white',anchor='mt')
    d.text((955,723),'K',font=small,fill='white')
    d.text((28,700), 'Arbitrary emission; fixed display scale; no soot optics.', font=small, fill='#c3d1e3')
    d.text((28,724), 'Structure only. Not a physical preview or validation result.', font=small, fill='#ffcd70')
    return canvas


def main():
    if len(sys.argv) != 2 or 'nonphysical_diagnostic' not in pathlib.Path(sys.argv[1]).name:
        raise ValueError('output directory must include nonphysical_diagnostic')
    out = pathlib.Path(sys.argv[1])
    extractor = ROOT/'bin/r243_temperature_extract'
    if sha(SOURCE/'sequence_manifest.rise-fire.cbor') != MANIFEST_SHA or sha(SOURCE/'frame_times.csv') != TIMES_SHA:
        raise ValueError('source sequence identity mismatch')
    rows = list(csv.DictReader((SOURCE/'frame_times.csv').open()))
    # Whole 14.625-second statistics sequence; the requested short clip uses
    # frames 64..127 (14.466..18.404 s), with 64 real snapshots, no interpolation.
    if len(rows) != 235 or [int(r['frame']) for r in rows] != list(range(235)):
        raise ValueError('source frame coverage mismatch')
    times = np.array([float(r['simulation_time_s']) for r in rows])
    if np.max(abs(np.diff(times)-1/16)) > 1e-8:
        raise ValueError('source cadence mismatch')
    out.mkdir(exist_ok=False)
    common = dict(label=LABEL, physical_preview_eligible=False,
        artifact_kind='external_temperature_visualization',
        source_manifest_sha256=MANIFEST_SHA, source_times_sha256=TIMES_SHA,
        extractor_sha256=sha(extractor), script_sha256=sha(pathlib.Path(__file__)),
        ramp_knots_K=KNOTS, ramp_RGB=COLORS,
        emission='J=RGB(T)*max((T-600)/1600,0); I=sum_y(J)*dx; display=1-exp(-I/0.03)',
        emission_crop_x_indices=[18,50],emission_crop_z_indices=[0,39],
        viewing_direction='+y', spatial_reconstruction='piecewise constant, nearest display pixels',
        coordinate_convention='solver cell volumes [i*dx,(i+1)*dx], not physical radiance',
        colorbar_applies_to='central slice and local emissivity hue, not integrated emission brightness',
        temporal_interpolation=False, native_fps=16, width=1040, height=760)
    record(out/'nonphysical_diagnostic_launch.json',common)
    inventory = []
    stats = []
    for row in rows:
        index = int(row['frame'])
        source = SOURCE/'frames'/f'frame_{index:05d}.vdb'
        if sha(source) != row['vdb_sha256']:
            raise ValueError(f'VDB hash mismatch {source}')
        data = subprocess.run([str(extractor), str(source)], check=True, stdout=subprocess.PIPE).stdout
        header, payload = data.split(b'\n',1)
        metadata = json.loads(header)
        t = np.frombuffer(payload, dtype='<f4').reshape(tuple(metadata['shape_xyz'][::-1])).astype(np.float64)
        dx = metadata['dx_m']
        if not np.isfinite(t).all() or t.min()<0 or t.max()>2300:
            raise ValueError('invalid temperature range')
        image = draw_frame(t, dx, index, float(row['simulation_time_s']))
        name = f'nonphysical_diagnostic_temperature_{index:04d}.png'
        image.save(out/name)
        sidecar = dict(common, source_file=str(source),source_vdb_sha256=row['vdb_sha256'],
            source_frame=index,simulation_time_s=float(row['simulation_time_s']),
            file=name,sha256=sha(out/name),grid=metadata,temperature_min_K=float(t.min()),temperature_max_K=float(t.max()))
        record(out/(name+'.provenance.json'),sidecar)
        inventory.append(dict(file=name,sha256=sidecar['sha256'],sidecar_sha256=sha(out/(name+'.provenance.json'))))
        hot = t>1300
        heights = np.nonzero(np.any(hot,axis=(1,2)))[0]
        stats.append([index,row['simulation_time_s'],t.min(),t.max(),
                      (heights[-1]+1)*dx if len(heights) else 0,
                      float(hot.sum())*dx**3])
        if index%16==0: print(f'frame {index}/235 T_max={t.max():.2f}',flush=True)
    with (out/'nonphysical_diagnostic_structure_metrics.csv').open('x') as stream:
        writer=csv.writer(stream)
        writer.writerow(['frame','simulation_time_s','T_min_K','T_max_K','top_of_cells_above_1300K_m','volume_above_1300K_m3'])
        writer.writerows(stats)
    record(out/'nonphysical_diagnostic_frames.sha256.json',inventory)


if __name__ == '__main__':
    main()
