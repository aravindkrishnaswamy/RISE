"""DL-331 regression: the opt-in photon/PT band must control process status.

Run after building SMSPhotonSeedEnergyTest. This checks the diagnostic contract,
not energy parity: a correctly reported physical failure passes this contract.
--check-existing LOG RC audits a completed captured run (red-before evidence).
"""
import math
from pathlib import Path
import re
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check(output, rc):
    pattern = (r'DL331 mode=(\d+) n=(\d+) PT ([\d.eE+-]+) SE ([\d.eE+-]+)'
               r'(?: SD [\d.eE+-]+)? photon ([\d.eE+-]+) SE ([\d.eE+-]+)'
               r'(?: SD [\d.eE+-]+)? ratio [\d.eE+-]+ combined3sigma ([\d.eE+-]+) within=(\d+)')
    rows = re.findall(pattern, output)
    if {int(r[0]) for r in rows} != {0, 2} or len(rows) != 2:
        raise AssertionError('missing immersed/air photon comparison')
    failed = False
    for mode, n, pt, pt_se, photon, photon_se, band, within in rows:
        values = tuple(map(float, (pt, pt_se, photon, photon_se, band)))
        require(int(n) == 4 and all(math.isfinite(x) for x in values), "invalid replicate summary")
        pt, pt_se, photon, photon_se, band = values
        require(pt > 0 and pt_se >= 0 and photon_se >= 0, "invalid PT/SE")
        expected_band = 3 * math.hypot(pt_se, photon_se)
        require(math.isclose(band, expected_band, rel_tol=1e-7, abs_tol=1e-12), "incorrect three-SE band")
        expected_within = abs(pt - photon) <= expected_band
        require(int(within) in (0, 1) and bool(int(within)) == expected_within, "incorrect parity diagnostic")
        failed |= not expected_within
    expected_rc = int(failed)
    require(rc == expected_rc, f'energy status was {rc}, expected {expected_rc}')
    print(f'DL331 exit contract: PASS (energy rc={rc}; physical parity={not failed})')


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--check-existing':
        check(Path(sys.argv[2]).read_text(), int(sys.argv[3]))
    else:
        require(len(sys.argv) == 1, 'arguments: [--check-existing LOG RC]')
        command = ['bin/tests/SMSPhotonSeedEnergyTest', '4', 'air', '200000',
                   '1024', '16', '128', '0', '--gate']
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        print(result.stdout, end='')
        check(result.stdout, result.returncode)
