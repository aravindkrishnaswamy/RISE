#!/usr/bin/env python3
"""Run only the new native resource probe controls; no owner simulation."""
import argparse, hashlib, json, subprocess
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sources = [root / p for p in (
        'src/Library/Utilities/FireProductionForce.h',
        'src/Library/Utilities/FireProductionAdvectionMac.mm',
        'src/Library/Utilities/FireProductionTransport.h',
        'src/Library/Utilities/FireProductionSource.cpp',
        'tools/fire/r248_preconstruction_probe.cpp',
        'tools/fire/r248_preconstruction_probe_verify.py',
        'docs/FIRE_SMOKE_PRECONSTRUCTION_RESOURCE_PROBE_R248.md',
        'bin/librise.a')]
    pins = [{'path': str(p), 'sha256': sha(p)} for p in sources]
    link = ['-L/opt/homebrew/lib', '-L/opt/homebrew/opt/open-image-denoise/lib',
            '-Wl,-rpath,/opt/homebrew/lib', '-Wl,-rpath,/opt/homebrew/opt/open-image-denoise/lib',
            '-lOpenImageDenoise', '-lpng', '-lz', '-ltiff', '-lOpenEXR', '-lOpenEXRUtil',
            '-lOpenEXRCore', '-lIex', '-lIlmThread', '-lImath', '-lopenpgl', '-lopenvdb',
            '-ltbb', '-framework', 'Foundation', '-framework', 'Metal']
    base = ['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-pedantic',
            '-fno-fast-math', '-ffp-contract=off']
    stages = []

    def run(name, command):
        log = out / (name + '.log')
        with log.open('xb') as f:
            result = subprocess.run(command, cwd=root, stdout=f, stderr=subprocess.STDOUT)
        stages.append({'name': name, 'exit_code': result.returncode, 'log': str(log),
                       'sha256': sha(log), 'command': command})
        if result.returncode:
            raise RuntimeError(f'{name} failed exit {result.returncode}; retained {log}')
        if 'warning:' in log.read_text(errors='replace') or 'error:' in log.read_text(errors='replace'):
            raise RuntimeError(f'{name} compiler/runtime diagnostic: {log}')

    harness = root / 'tools/fire/r248_preconstruction_probe.cpp'
    native = root / 'src/Library/Utilities/FireProductionAdvectionMac.mm'
    library = root / 'bin/librise.a'
    run('strict_build', base + ['-I.', str(harness), str(library)] + link + ['-o', str(out / 'probe')])
    run('strict_run', [str(out / 'probe')])
    sanitize = ['-fsanitize=address,undefined']
    run('sanitizer_native_build', base + sanitize + ['-fobjc-arc', '-DCOLORS_RGB', '-DMERSENNE53',
        '-Isrc/Library', '-isystem', '/opt/homebrew/include', '-c', str(native), '-o', str(out / 'native_sanitized.o')])
    run('sanitizer_link', base + sanitize + ['-I.', str(harness), str(out / 'native_sanitized.o'), str(library)]
        + link + ['-o', str(out / 'probe_sanitized')])
    run('sanitizer_run', [str(out / 'probe_sanitized')])
    reports = []
    for name in ('strict_run', 'sanitizer_run'):
        rows = [json.loads(line) for line in (out / (name + '.log')).read_text().splitlines()]
        assert len(rows) == 4 and rows[-1] == {'green': 6, 'red': 17,
            'owner_numerical_work': False, 'complete_admission': False}
        for row, cells in zip(rows[:3], (64, 240, 512)):
            assert row['cells'] == cells and row['element_bytes'] == 108 * cells
            assert row['capacity_bytes'] >= row['element_bytes']
            assert 0 < row['rss'] < 256 << 20 and 0 < row['peak_rss'] < 256 << 20
            assert row['footprint'] > 0 and row['device_limit'] > 0 and row['max_buffer'] > 0
        reports.append({'mode': name, 'rows': rows})
    for p, pin in zip(sources, pins):
        assert sha(p) == pin['sha256'], f'source/runtime changed during qualification: {p}'
    for p in out.iterdir():
        if p.is_file():
            pins.append({'path': str(p), 'sha256': sha(p)})
    identity = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True)),
        'embedded_git_identity': False,
        'clang': subprocess.check_output(['clang++', '--version'], text=True)}
    result = {'schema': 'rise.fire.r248.preconstruction-probe-validation.v1', 'pins': pins,
        'stages': stages, 'reports': reports, 'build_identity': identity,
        'complete_resource_admission': False, 'native_observer_accepted': False,
        'new_simulation_launched': False, 'physical_acceptance': False}
    report = out / 'validation.json'
    with report.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps({'validation': str(report), 'pins': len(pins), 'status': 'scoped_controls_passed'}))


if __name__ == '__main__':
    main()
