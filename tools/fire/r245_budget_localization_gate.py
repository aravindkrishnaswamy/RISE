#!/usr/bin/env python3
"""Prove the localizer refuses resealed inputs under a stale audit; no run mutation."""
import argparse
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from r245_budget_audit_gate import reseal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('verification', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    script = Path(__file__).with_name('r245_budget_localization.py')
    proof = json.loads((args.verification/'full_budget_audit.json').read_text())
    results = []
    with tempfile.TemporaryDirectory(prefix='r245-localizer-reds-') as tmp:
        root = Path(tmp)
        run = root/'run'; budgets = run/'budgets'; budgets.mkdir(parents=True)
        for path in (args.run/'budgets').glob('step_*.csv'):
            (budgets/path.name).symlink_to(path)
        for path in (args.run/'budgets').glob('frame_*.columns.csv'):
            (budgets/path.name).symlink_to(path)
        final = budgets/f"step_{proof['steps']:06d}.csv"
        column = budgets/'frame_00000.columns.csv'
        verify = root/'verify'; verify.mkdir()
        for name in ('completion.json','full_budget_audit.json','full_window_audit.json'):
            shutil.copyfile(args.verification/name,verify/name)

        def control(name, expected_refusal=None):
            # Unchanged GREEN consumes original sidecars; mutated REDs own their
            # new valid seals, leaving the real run and audit untouched.
            process = subprocess.run([sys.executable,str(script),str(run),str(verify),str(root/name)],capture_output=True,text=True)
            if expected_refusal:
                assert process.returncode != 0 and expected_refusal in process.stderr, process.stderr
            else:
                assert process.returncode == 0, process.stderr
            results.append({'name':name,'expected':'RED' if expected_refusal else 'GREEN','exit_code':process.returncode,
                            'refusal':expected_refusal})

        for path in (args.run/'budgets').glob('*.columns.csv.payload-v2.json'):
            (budgets/path.name).symlink_to(path)
        (budgets/(final.name+'.payload-v2.json')).symlink_to(Path(str(args.run/'budgets'/final.name)+'.payload-v2.json'))
        control('unchanged')
        original = (args.run/'budgets'/final.name).read_text()
        changed = []
        for line in original.splitlines():
            if line.startswith('# statistics_source '):
                values = line.split(' ',2)[2].split(','); values[0] = '9000000000'
                line = '# statistics_source '+','.join(values)
            changed.append(line)
        final.unlink(); final.write_text('\n'.join(changed)+'\n')
        seal = Path(str(final)+'.payload-v2.json'); seal.unlink(); reseal(final,proof['case'])
        control('resealed_final_source','final ledger differs from audited bytes')
        final.unlink(); final.symlink_to(args.run/'budgets'/final.name)
        seal.unlink(); seal.symlink_to(Path(str(args.run/'budgets'/final.name)+'.payload-v2.json'))
        original = (args.run/'budgets'/column.name).read_text()
        rows = list(csv.DictReader(io.StringIO(original)))
        next(row for row in rows if row['axis'] == '2')['stress_rate_kg_per_m2_s2'] = '9000000000'
        stream = io.StringIO(); writer = csv.DictWriter(stream,fieldnames=list(rows[0]),lineterminator='\n')
        writer.writeheader(); writer.writerows(rows)
        column.unlink(); column.write_text(stream.getvalue())
        # Valid resealing alone must not make newly changed bytes audited.
        seal = Path(str(column)+'.payload-v2.json'); seal.unlink(); reseal(column,proof['case'])
        control('resealed_columns','columns differ from audited bytes')
        column.unlink(); column.symlink_to(args.run/'budgets'/column.name)
        seal.unlink(); seal.symlink_to(Path(str(args.run/'budgets'/column.name)+'.payload-v2.json'))
        foreign = dict(proof,case='0'*64)
        (verify/'full_budget_audit.json').write_text(json.dumps(foreign))
        control('foreign_audit','foreign run')
        stale = dict(proof); stale.pop('final_step_sha256')
        (verify/'full_budget_audit.json').write_text(json.dumps(stale))
        control('stale_audit_without_input_pins','final ledger differs from audited bytes')
    args.output.write_text(json.dumps({'status':'PASS','controls':results},indent=2)+'\n')
    print('PASS unchanged GREEN and four stale/resealed/foreign audit REDs')


if __name__ == '__main__':
    main()
