#!/usr/bin/env python3
"""Observe an existing bounded diagnostic; never grant run/resume authority."""
import argparse
import csv
import re
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("binary", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
if a.output.exists() or Path(str(a.output) + ".log").exists():
    raise SystemExit("immutable diagnostic destination already exists")
a.output.parent.mkdir(parents=True, exist_ok=True)
start = time.monotonic()
with Path(str(a.output) + ".log").open("x") as log, Path(str(a.output) + ".rss.csv").open("x") as rss:
    writer = csv.writer(rss)
    writer.writerow(["accepted_step", "elapsed_s", "process_rss_bytes"])
    with subprocess.Popen([str(a.binary), "--fire-production-owner-cost-prefix", str(a.output)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as child:
        for line in child.stdout:
            log.write(line)
            log.flush()
            match = re.search(r"capstone accepted step=(\d+)", line)
            if match:
                result = subprocess.run(["/bin/ps", "-p", str(child.pid), "-o", "rss="],
                                        text=True, capture_output=True, check=False)
                if result.returncode or not result.stdout.strip():
                    raise RuntimeError("RSS sampling failed; no memory claim admissible")
                writer.writerow([match.group(1), time.monotonic()-start, int(result.stdout.strip())*1024])
                rss.flush()
        code = child.wait()
    print(f"prefix exit={code}; observations={rss.name}", flush=True)
    raise SystemExit(code)
