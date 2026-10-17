#!/usr/bin/env python3
"""Qualify the actual caller builder's refusal seam; no numerical owner execution."""
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / 'build/make/rise'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [root / name for name in (
        'tests/FireSequenceTest.cpp', 'tests/FireRunHostMemory.h',
        'src/Library/Utilities/FireProductionForce.h',
        'src/Library/Utilities/FireProductionAdvectionMac.mm',
        'src/Library/Utilities/FireProductionTransport.h',
        'src/Library/Utilities/FireProductionSource.cpp',
        'docs/FIRE_SMOKE_CALLER_INGRESS_REFUSAL_R248.md',
        'tools/fire/r248_caller_ingress_verify.py',
        'docs/data/fire_r245/r248_preconstruction_probe_review.json')]
    dry = subprocess.check_output(['make', '-n', '-W', '../../../tests/FireSequenceTest.cpp',
                                   'build-test/FireSequenceTest'], cwd=build, text=True)
    (out / 'dryrun.txt').write_text(dry)
    commands = [shlex.split(line) for line in dry.splitlines() if line.startswith('c++ ')]
    compile_command = next(cmd for cmd in commands if '-c' in cmd and
                           '../../../tests/FireSequenceTest.cpp' in cmd)
    link_command = next(cmd for cmd in commands if '-c' not in cmd and
                        '../../../bin/tests/FireSequenceTest' in cmd)
    objects = [(build / arg).resolve() for arg in link_command if arg.endswith('.o')]
    assert objects and all(obj.is_file() for obj in objects)
    sources += objects + [root / 'bin/tests/FireSequenceTest',
                         build / 'RendererBuildIdentity.generated.h']
    pins = [{'path': str(path), 'sha256': sha(path)} for path in sources]
    stages = []

    def run(name, command, cwd):
        log = out / (name + '.log')
        with log.open('x') as stream:
            result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
        stages.append({'name': name, 'command': command, 'cwd': str(cwd),
                       'exit_code': result.returncode, 'log': str(log)})
        if result.returncode or 'warning:' in log.read_text() or 'error:' in log.read_text():
            raise RuntimeError(f'{name} failed; unsealed evidence retained in {log}')

    strict = out / 'FireSequenceTest.strict'
    shutil.copyfile(root / 'bin/tests/FireSequenceTest', strict)
    strict.chmod(0o755)
    run('strict', [str(strict), '--fire-r248-caller-ingress-fixture'], root)
    instrumented = out / 'caller_sanitized.o'
    compile_command = [arg for arg in compile_command if arg not in
                       ('-flto', '-MMD', '-MP', '-Wno-c++11-long-long')]
    compile_command = ['-O1' if arg == '-O3' else arg for arg in compile_command]
    compile_command[compile_command.index('-o') + 1] = str(instrumented)
    compile_command += ['-fsanitize=address,undefined', '-g']
    run('sanitizer_compile', compile_command, build)
    sanitized = out / 'FireSequenceTest.sanitized'
    link_command[link_command.index('-o') + 1] = str(sanitized)
    link_command = [str(instrumented) if arg == '../../../tests/FireSequenceTest.o' else arg
                    for arg in link_command]
    link_command += ['-fsanitize=address,undefined']
    run('sanitizer_link', link_command, build)
    run('sanitizer', [str(sanitized), '--fire-r248-caller-ingress-fixture'], root)
    reports = []
    for mode in ('strict', 'sanitizer'):
        lines = (out / (mode + '.log')).read_text().splitlines()
        assert len(lines) == 4 and lines[-1] == (
            'R248_CALLER_INGRESS_SUMMARY green=6 red=30 numerical_owner_work=0 complete_admission=0')
        rows = []
        for line, dims in zip(lines[:3], ((4, 4, 4), (6, 8, 5), (8, 8, 8))):
            assert line.startswith('R248_CALLER_INGRESS ')
            row = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
            x, y, z = dims
            cells = x * y * z
            faces = (x + 1) * y * z + x * (y + 1) * z + x * y * (z + 1)
            assert row['cells'] == cells and row['ordinary_elements'] == 72 * cells + 8 * faces
            assert row['copy_lower'] == 184 * cells + 8 * faces
            assert row['ordinary_capacity'] >= row['ordinary_elements'] + 4 * (17 + 19)
            assert row['retained_output_unchanged'] == row['refused_before_copy'] == 1
            assert 0 < row['rss'] < 2 << 30 and 0 < row['peak'] < 2 << 30
            assert 0 < row['footprint'] < (2 << 30) - (32 << 20)
            rows.append(row)
        reports.append({'mode': mode, 'rows': rows})
    for path, pin in zip(sources, pins):
        assert sha(path) == pin['sha256'], f'input changed during controls: {path}'
    pins += [{'path': str(path), 'sha256': sha(path)} for path in out.iterdir() if path.is_file()]
    report = {'schema': 'rise.fire.r248.caller-ingress-validation.v1', 'pins': pins,
              'stages': stages, 'reports': reports,
              'build_identity': {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root,
                                   text=True).strip(), 'dirty': bool(subprocess.check_output(
                                   ['git', 'status', '--porcelain'], cwd=root, text=True)),
                                   'relabel_embedded_identity': False,
                                   'compiler': subprocess.check_output(['c++', '--version'], text=True)},
              'sanitizer_scope': 'New caller translation unit instrumented; linked dependencies retain their prior instrumentation',
              'complete_resource_admission': False, 'authenticated_observer': False,
              'native_parity': False, 'new_simulation': False, 'physical_acceptance': False}
    with (out / 'validation.json').open('x') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'pins': len(pins), 'green': 6, 'red': 30, 'validation': str(out / 'validation.json')}))


if __name__ == '__main__':
    main()
