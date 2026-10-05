#!/usr/bin/env python3
"""Run only the new actual context resource probe controls; no owner simulation."""
import argparse, hashlib, json, shutil, subprocess, sys
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def require(condition, detail):
    if not condition:
        raise RuntimeError(str(detail))


def main():
    if not __debug__:
        raise RuntimeError('context verifier requires non-optimized Python')
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for source, name in ((root / 'bin/librise.a', 'librise.a'),
                         (root / 'src/Library/Utilities/FireProductionAdvectionMac.o', 'native.o')):
        with source.open('rb') as src, (out / name).open('xb') as dst:
            shutil.copyfileobj(src, dst)
    sources = [root / p for p in (
        'src/Library/Utilities/FireProductionForce.h',
        'src/Library/Utilities/FireProductionAdvectionMac.mm',
        'src/Library/Utilities/FireProductionTransport.h',
        'src/Library/Utilities/FireProductionSource.cpp',
        'tools/fire/r248_context_probe.cpp',
        'tools/fire/r248_context_probe_verify.py',
        'docs/FIRE_SMOKE_CONTEXT_RESOURCE_PROBE_R248.md',
        )] + [out / 'librise.a', out / 'native.o']
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

    harness = root / 'tools/fire/r248_context_probe.cpp'
    native = root / 'src/Library/Utilities/FireProductionAdvectionMac.mm'
    library = out / 'librise.a'
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
        require(len(rows) == 5, 'failed validation: len(rows) == 5')
        for i, row in enumerate(rows):
            require(row['mode'] == i, "failed validation: row['mode'] == i")
            require(row['passed'] == (i in (0, 2, 3)), "failed validation: row['passed'] == (i in (0, 2, 3))")
            require(row['refusal'] == {0: 0, 1: 1, 2: 0, 3: 0, 4: 2}[i], "failed validation: row['refusal'] == {0: 0, 1: 1, 2: 0, 3: 0, 4: 2}[i]")
            require(row['before_attempts'] == ([0,0,0] if i <= 2 else [1,1,1]), "failed validation: row['before_attempts'] == ([0,0,0] if i <= 2 else [1,1,1])")
            require(row['after_attempts'] == ([0,0,0] if i < 2 else [1,1,1]), "failed validation: row['after_attempts'] == ([0,0,0] if i < 2 else [1,1,1])")
            require(row['factory_entered'] == (i in (2,3)), "failed validation: row['factory_entered'] == (i in (2,3))")
            require(row['slots_known'] == (i in (2,3)), "failed validation: row['slots_known'] == (i in (2,3))")
            require(row['pipeline_slots'] == ([43,43,11] if i in (2,3) else [0,0,0]), "failed validation: row['pipeline_slots'] == ([43,43,11] if i in (2,3) else [0,0,0])")
            require(row['telemetry_known'] == (i != 4), "failed validation: row['telemetry_known'] == (i != 4)")
            if i != 4:
                for k in ('before_rss','after_rss','before_footprint','after_footprint','peak'):
                    require(0 < row[k] < 2 << 30, 'failed validation: 0 < row[k] < 2 << 30')
                require(row['after_rss'] < (2 << 30) - (32 << 20), "failed validation: row['after_rss'] < (2 << 30) - (32 << 20)")
                require(row['after_footprint'] < (2 << 30) - (32 << 20), "failed validation: row['after_footprint'] < (2 << 30) - (32 << 20)")
            if i in (2,3):
                require(row['registry'] > 0, "failed validation: row['registry'] > 0")
                require(0 < row['pipeline_thread_limit_min'], "failed validation: 0 < row['pipeline_thread_limit_min']")
                require(0 <= row['static_threadgroup_max'] <= row['device_threadgroup_limit'], "failed validation: 0 <= row['static_threadgroup_max'] <= row['device_threadgroup_limit']")
                require(row['device_threadgroup_limit'] > 0, "failed validation: row['device_threadgroup_limit'] > 0")
        require(rows[2]['registry'] == rows[3]['registry'], "failed validation: rows[2]['registry'] == rows[3]['registry']")
        reports.append({'mode': name, 'green': 3, 'red': 2, 'rows': rows})
    for binary in ('probe', 'probe_sanitized'):
        # Passing a read-only regular descriptor as stdout makes printf fail.
        log = out / (binary + '_publication_failure.log')
        with (out / 'strict_run.log').open('rb') as readonly, log.open('xb') as err:
            result = subprocess.run([str(out / binary)], cwd=root, stdout=readonly, stderr=err)
        require(result.returncode == 2, (binary, result.returncode))
        require(not log.read_bytes(), 'unexpected runtime/sanitizer diagnostic')
        stages.append({'name': binary + '_publication_failure', 'exit_code': result.returncode,
                       'expected_exit_code': 2, 'log': str(log), 'sha256': sha(log)})
    refusal_log = out / 'optimized_python_refusal.log'
    denied_output = out / 'optimized_denied'
    with refusal_log.open('xb') as log:
        refusal = subprocess.run([sys.executable, '-O', str(Path(__file__).resolve()),
            '--output', str(denied_output)], cwd=root, stdout=log, stderr=subprocess.STDOUT)
    require(refusal.returncode == 1 and not denied_output.exists(), 'failed validation: refusal.returncode == 1 and not denied_output.exists()')
    require('context verifier requires non-optimized Python' in refusal_log.read_text(), "failed validation: 'context verifier requires non-optimized Python' in refusal_log.read_text()")
    stages.append({'name': 'optimized_python_refusal', 'exit_code': refusal.returncode,
        'expected_exit_code': 1, 'log': str(refusal_log), 'sha256': sha(refusal_log)})
    for p, pin in zip(sources, pins):
        require(sha(p) == pin['sha256'], f'source/runtime changed during qualification: {p}')
    for p in out.iterdir():
        if p.is_file() and p not in sources:
            pins.append({'path': str(p), 'sha256': sha(p)})
    identity = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True)),
        'embedded_git_identity': False,
        'python_version': sys.version, 'python_optimize': sys.flags.optimize,
        'clang': subprocess.check_output(['clang++', '--version'], text=True)}
    result = {'schema': 'rise.fire.r248.context-probe-validation.v1', 'pins': pins,
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
