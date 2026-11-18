#!/usr/bin/env python3
"""Build isolated case tests under sanitizers, then prove two specific mutants fail."""
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

def main():
    out = pathlib.Path(sys.argv[1]).resolve()
    out.mkdir(exist_ok=False)
    source = ROOT/'src/Library/Utilities/FireCase.cpp'
    original = source.read_text()
    branch = 'if(!authored.domainStudy.empty()) {'
    assert original.count(branch) == 2
    source_rehash = original.replace(branch, 'if(false && !authored.domainStudy.empty()) {', 1)
    needle = 'i+baseline.nx/2u+derived.nx*(j+baseline.ny/2u)'
    assert original.count(needle) == 2
    first, rest = original.split(needle, 1)
    misplaced_pilot = first+needle+rest.replace(needle, 'i+baseline.nx/2u+1u+derived.nx*(j+baseline.ny/2u)', 1)
    results = []
    for name, text, expected in [('sanitized_green', original, 0),
                                 ('source_rehash_red', source_rehash, 1),
                                 ('pilot_translation_red', misplaced_pilot, 1)]:
        cpp = out/(name+'.cpp'); cpp.write_text(text)
        binary = out/name
        # Instrument the new domain invariants. Historical digest compatibility
        # is checked separately by the full native FireCaseTest and producer gate.
        # Case, record and CBOR paths are instrumented; optics
        # dependency uses its canonical make object (covered by the clean build).
        cmd = ['c++', '-std=gnu++17', '-O3', '-flto', '-ffast-math', '-fno-finite-math-only',
               '-funroll-loops', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I'+str(ROOT/'src/Library'), '-I'+str(ROOT/'src/Library/Utilities'),
               str(ROOT/'tests/FireCaseTest.cpp'), str(cpp),
               str(ROOT/'src/Library/Utilities/FireSimulationRecords.cpp'),
               str(ROOT/'src/Library/Utilities/RISECBOR64.cpp'),
               str(ROOT/'src/Library/Utilities/FireOptics.o'), '-Wl,-dead_strip', '-o', str(binary)]
        with (out/(name+'_build.log')).open('w') as log:
            code = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        if code:
            raise RuntimeError(name+' build failed; inspect '+str(out))
        with (out/(name+'.log')).open('w') as log:
            code = subprocess.call([str(binary), '--r244-domain-only'], stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        log_text = (out/(name+'.log')).read_text()
        if code != expected or 'Sanitizer' in log_text or 'runtime error:' in log_text:
            raise RuntimeError(name+' unexpected test/sanitizer result')
        if expected and 'r244 embeds the exact source bytes and complete pilot mask' not in log_text:
            raise RuntimeError(name+' failed without the intended translation assertion')
        results.append(dict(name=name, exit_code=code, expected_exit_code=expected,
                            source_sha256=hashlib.sha256(text.encode()).hexdigest(),
                            log_sha256=hashlib.sha256(log_text.encode()).hexdigest()))
        print(name, code, flush=True)
    if source.read_text() != original:
        raise RuntimeError('worktree source changed during isolated gate')
    (out/'completion.json').write_text(json.dumps(dict(results=results,
        finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()), indent=2)+'\n')

if __name__ == '__main__':
    main()
