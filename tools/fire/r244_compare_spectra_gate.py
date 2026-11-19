#!/usr/bin/env python3
"""Real copied-input GREEN/RED controls for r244 comparison provenance."""
import contextlib
import io
import json
import pathlib
import shutil
import tempfile
import r244_compare_spectra as compare


def main():
    source_root, source_out = compare.EXTERNAL, compare.OUT
    gate = pathlib.Path(tempfile.mkdtemp(prefix='r244_compare_gate.', dir=source_root))
    results = []
    for variant_to_mutate in [None, 'baseline', 'lateral']:
        label = variant_to_mutate or 'green'
        base = gate/label; out = base/'evidence'; out.mkdir(parents=True)
        for v in compare.VARIANTS:
            folder = f'r244_tier6_{v}_spectrum.v1'
            (base/folder).mkdir()
            for name in ['full_spectrum.csv', 'centerline_signal.csv']:
                shutil.copyfile(source_root/folder/name, base/folder/name)
            audit = json.loads((source_out/f'{v}_spectrum_audit.json').read_text())
            for artifact in audit['source_artifacts']:
                artifact['path'] = artifact['path'].replace(str(source_root), str(base), 1)
            (out/f'{v}_spectrum_audit.json').write_text(json.dumps(audit))
        if variant_to_mutate:
            path = base/f'r244_tier6_{variant_to_mutate}_spectrum.v1/centerline_signal.csv'
            # Keep full_spectrum and historical audit hashes intact; alter a consumed signal.
            lines = path.read_text().splitlines()
            fields = lines[1].split(','); fields[-1] = '0'; lines[1] = ','.join(fields)
            path.write_text('\n'.join(lines)+'\n')
        compare.EXTERNAL, compare.OUT = base, out
        error = None
        try:
            with contextlib.redirect_stdout(io.StringIO()): compare.main()
        except ValueError as exc:
            error = str(exc)
        if variant_to_mutate:
            assert error == 'centerline changed since audit', error
            assert not (out/'domain_comparison.json').exists()
            assert not (out/'domain_comparison_spectra.svg').exists()
            assert not (out/'domain_comparison_full_spectrum.csv').exists()
            compare.plt.close('all')
        else:
            assert error is None and (out/'domain_comparison.json').is_file()
        results.append(dict(case=label, passed=True, refusal=error))
    record = dict(scope='Copied native CSVs and relocated audit paths; no simulation or render',
                  artifact_directory=str(gate), results=results,
                  gate_sha256=compare.sha(pathlib.Path(__file__)),
                  comparison_script_sha256=compare.sha(pathlib.Path(compare.__file__)))
    (gate/'completion.json').write_text(json.dumps(record, indent=2)+'\n')
    (source_out/'comparison_input_gate.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__': main()
