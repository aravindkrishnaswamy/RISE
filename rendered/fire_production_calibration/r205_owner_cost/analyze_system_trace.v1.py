#!/usr/bin/env python3
"""Reproduce the target-only sampled CPU observation; not a wall-time fit."""
import collections
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

directory = Path(__file__).resolve().parent
toc_path = directory / "whole_owner_metal.v1.toc.xml"
tables_path = directory / "whole_owner_metal.v1.target_tables.xml"
toc = ET.parse(toc_path).getroot()
run = toc.find("run")
target = run.find("info/target/process")
assert target.get("name") == "FireSequenceTest" and target.get("return-exit-status") == "0"
pid = target.get("pid")
schemas = [table.get("schema") for table in run.find("data")]
export = ET.parse(tables_path).getroot()
identities = {element.get("id"): element for element in export.iter() if element.get("id")}

def resolve(element):
    visited = set()
    while element is not None and element.get("ref"):
        key = element.get("ref")
        if key in visited:
            raise ValueError("cyclic trace reference")
        visited.add(key)
        element = identities[key]
    return element

def belongs_to_target(row):
    process = resolve(row.find("process"))
    if process is None:
        return False
    process_id = resolve(process.find("pid"))
    return process_id is not None and process_id.text == pid

leaf, ancestor = collections.Counter(), collections.Counter()
cpu_samples = owner_samples = 0
encoder_rows = gpu_rows = 0
for node in export:
    match = re.search(r"table\[(\d+)\]$", node.get("xpath"))
    if not match:
        raise ValueError("unrecognized exported table")
    schema = schemas[int(match.group(1)) - 1]
    for row in node.findall("row"):
        if not belongs_to_target(row):
            continue
        if schema == "metal-application-encoders-list":
            encoder_rows += 1
        elif schema == "metal-gpu-intervals":
            gpu_rows += 1
        elif schema == "time-profile":
            state = resolve(row.find("thread-state"))
            if state is None or state.text != "Running":
                raise ValueError("CPU observation contains non-running samples")
            trace, weight = resolve(row.find("tagged-backtrace")), resolve(row.find("weight"))
            if trace is None or weight is None:
                continue
            stack = resolve(trace.find("backtrace"))
            if stack is None:
                continue
            frames = [resolve(frame).get("name", "") for frame in stack]
            cpu_samples += 1
            owners = [frame for frame in frames if "ResidentProjectedHeunMetalOwner" in frame]
            if not owners:
                continue
            owner_samples += 1
            milliseconds = int(weight.text) / 1e6
            leaf[frames[0]] += milliseconds
            ancestor[owners[0]] += milliseconds

assert encoder_rows > 0 and gpu_rows > 0 and owner_samples > 0
result = dict(schema="rise.fire.owner.sampled-cpu.v1", target_pid=int(pid),
    scope="one diagnostic three-step prefix; sampled CPU time, not elapsed wall or an optimization result",
    inputs={path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (toc_path, tables_path)},
    target_cpu_samples=cpu_samples, owner_stack_samples=owner_samples,
    target_encoder_creation_rows=encoder_rows, target_gpu_rows=gpu_rows,
    owner_sampled_leaf_ms=dict(leaf.most_common()),
    owner_sampled_nearest_owner_ancestor_ms=dict(ancestor.most_common()),
    limitation="Generic Compute Command labels do not identify force/projection kernels. "
               "GPU rows may be execution slices; their row count is not a kernel dispatch count. "
               "System tables contain other processes; only rows whose resolved PID matches target are used.")
print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
