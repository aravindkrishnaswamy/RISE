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


def check_pins(pins):
    for pin in pins:
        require(sha(Path(pin["path"])) == pin["sha256"], f'source/runtime changed during qualification: {pin["path"]}')


def main():
    if not __debug__:
        raise RuntimeError('context verifier requires non-optimized Python')
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(subprocess.check_output(['xcrun', '-f', 'clang++'], text=True).strip()).absolute()
    sdk = Path(subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()).resolve()
    sdk_metadata = sdk / 'SDKSettings.json'
    require(compiler.is_file() and sdk_metadata.is_file(), 'compiler/SDK metadata unavailable')
    native_names = ('FireProductionForceMac', 'FireProductionProjectionMac')
    for source, name in [(root / 'bin/librise.a', 'librise.a')] + [
            (root / ('src/Library/Utilities/' + name + '.o'), name + '.o') for name in native_names]:
        with source.open('rb') as src, (out / name).open('xb') as dst:
            shutil.copyfileobj(src, dst)
    sources = [root / p for p in (
        'src/Library/Utilities/FireProductionForce.h',
        'src/Library/Utilities/FireProductionProjection.h',
        'src/Library/Utilities/FireProductionAdvectionMac.mm',
        'src/Library/Utilities/FireProductionForceMac.mm',
        'src/Library/Utilities/FireProductionProjectionMac.mm',
        'tools/fire/r248_adapter_context_probe.cpp',
        'tools/fire/r248_adapter_context_probe_verify.py',
        'docs/FIRE_SMOKE_ADAPTER_CONTEXT_RESOURCE_PROBE_R248.md')]
    sources += [out / 'librise.a'] + [out / (name + '.o') for name in native_names]
    sources += [compiler, sdk_metadata]
    pins = [{'path': str(p), 'sha256': sha(p)} for p in sources]
    link = ['-L/opt/homebrew/lib', '-L/opt/homebrew/opt/open-image-denoise/lib',
            '-Wl,-rpath,/opt/homebrew/lib', '-Wl,-rpath,/opt/homebrew/opt/open-image-denoise/lib',
            '-lOpenImageDenoise', '-lpng', '-lz', '-ltiff', '-lOpenEXR', '-lOpenEXRUtil',
            '-lOpenEXRCore', '-lIex', '-lIlmThread', '-lImath', '-lopenpgl', '-lopenvdb',
            '-ltbb', '-framework', 'Foundation', '-framework', 'Metal']
    base = [str(compiler), '-isysroot', str(sdk), '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-pedantic',
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

    harness = root / 'tools/fire/r248_adapter_context_probe.cpp'
    library = out / 'librise.a'
    run('strict_build', base + ['-I.', str(harness), str(library)] + link + ['-o', str(out / 'probe')])
    run('strict_run', [str(out / 'probe')])
    sanitize = ['-fsanitize=address,undefined']
    sanitized = []
    for name in native_names:
        native = root / ('src/Library/Utilities/' + name + '.mm')
        obj = out / (name + '_sanitized.o')
        run(name + '_sanitizer_build', base + sanitize + ['-fobjc-arc', '-DCOLORS_RGB', '-DMERSENNE53',
            '-Isrc/Library', '-isystem', '/opt/homebrew/include', '-c', str(native), '-o', str(obj)])
        sanitized.append(str(obj))
    run('sanitizer_link', base + sanitize + ['-I.', str(harness)] + sanitized + [str(library)]
        + link + ['-o', str(out / 'probe_sanitized')])
    run('sanitizer_run', [str(out / 'probe_sanitized')])
    reports = []
    for name in ('strict_run', 'sanitizer_run'):
        rows = [json.loads(line) for line in (out / (name + '.log')).read_text().splitlines()]
        require(len(rows) == 10, 'exact10 native rows')
        for index, row in enumerate(rows):
            i = index % 5
            kind, slots = ('force', 14) if index < 5 else ('projection', 21)
            require(row['kind'] == kind and row['mode'] == i, 'exact family/order')
            require(row['passed'] == (i in (0,2,3)), 'pass/refusal distinction')
            require(row['refusal'] == {0:0,1:1,2:0,3:0,4:2}[i], 'actual refusal site')
            require(row['before_attempts'] == (0 if i <= 2 else 1), 'cold/warm before attempts')
            require(row['after_attempts'] == (0 if i < 2 else 1), 'cold/warm after attempts')
            require(row['factory_entered'] == (i in (2,3)), 'factory entry')
            require(row['slots_known'] == (i in (2,3)), 'slot availability')
            require(row['pipeline_slots'] == (slots if i in (2,3) else 0), 'actual slots')
            require(row['telemetry_known'] == (i != 4), 'sample availability')
            if i != 4:
                for k in ('before_rss','after_rss','before_footprint','after_footprint','peak'):
                    require(0 < row[k] < 2 << 30, 'fixture memory observation')
                require(max(row['after_rss'],row['after_footprint']) < (2 << 30)-(32 << 20), 'fixture reserve')
            if i in (2,3):
                require(row['registry'] > 0 and row['pipeline_thread_limit_min'] > 0, 'actual device/pipeline')
                require(0 <= row['static_threadgroup_max'] <= row['device_threadgroup_limit'] and
                        row['device_threadgroup_limit'] > 0, 'static threadgroup query')
        require(rows[2]['registry'] == rows[3]['registry'] == rows[7]['registry'] == rows[8]['registry'], 'same actual device')
        reports.append({'mode':name, 'green':6, 'row_red':4, 'publication_red':2, 'rows':rows})
    for binary in ('probe','probe_sanitized'):
        for kind, arguments in (('force',[]),('projection',['projection'])):
            log = out / (binary + '_' + kind + '_publication_failure.log')
            with (out / 'strict_run.log').open('rb') as readonly, log.open('xb') as err:
                result = subprocess.run([str(out / binary)] + arguments, cwd=root, stdout=readonly, stderr=err)
            require(result.returncode == 2, (binary,kind,result.returncode))
            require(not log.read_bytes(), 'unexpected runtime/sanitizer diagnostic')
            stages.append({'name':binary + '_' + kind + '_publication_failure', 'exit_code':result.returncode,
                'expected_exit_code':2, 'log':str(log), 'sha256':sha(log)})
    refusal_log = out / 'optimized_python_refusal.log'
    denied_output = out / 'optimized_denied'
    with refusal_log.open('xb') as log:
        refusal = subprocess.run([sys.executable, '-O', str(Path(__file__).resolve()),
            '--output', str(denied_output)], cwd=root, stdout=log, stderr=subprocess.STDOUT)
    require(refusal.returncode == 1 and not denied_output.exists(), 'failed validation: refusal.returncode == 1 and not denied_output.exists()')
    require('context verifier requires non-optimized Python' in refusal_log.read_text(), "failed validation: 'context verifier requires non-optimized Python' in refusal_log.read_text()")
    stages.append({'name': 'optimized_python_refusal', 'exit_code': refusal.returncode,
        'expected_exit_code': 1, 'log': str(refusal_log), 'sha256': sha(refusal_log)})
    fingerprint_red = []
    for target in (compiler, sdk_metadata):
        mismatched = [{'path': str(target), 'sha256': '0' * 64}]
        try:
            check_pins(mismatched)
        except RuntimeError:
            fingerprint_red.append(str(target))
        else:
            raise RuntimeError('mismatched fingerprint admitted')
    require(len(fingerprint_red) == 2, 'compiler/SDK mismatch controls')
    check_pins(pins)
    for p in out.iterdir():
        if p.is_file() and p not in sources:
            pins.append({'path': str(p), 'sha256': sha(p)})
    identity = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True)),
        'embedded_git_identity': False,
        'python_version': sys.version, 'python_optimize': sys.flags.optimize,
        'clang': subprocess.check_output([str(compiler), '--version'], text=True),
        'compiler_invocation': str(compiler), 'compiler_executable': str(compiler.resolve()), 'sdk_path': str(sdk),
        'sdk_metadata': str(sdk_metadata), 'sdk_scope': 'SDKSettings.json only; full SDK tree and driver unqualified'}
    result = {'schema': 'rise.fire.r248.adapter-context-probe-validation.v1', 'pins': pins,
        'stages': stages, 'reports': reports, 'fingerprint_mismatch_red': fingerprint_red, 'build_identity': identity,
        'complete_resource_admission': False, 'native_observer_accepted': False,
        'new_simulation_launched': False, 'physical_acceptance': False}
    report = out / 'validation.json'
    with report.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps({'validation': str(report), 'pins': len(pins), 'status': 'scoped_controls_passed'}))


if __name__ == '__main__':
    main()
