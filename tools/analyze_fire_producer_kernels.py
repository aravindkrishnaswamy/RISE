#!/usr/bin/env python3
"""Decode correlated Metal encoder counters; never sum them as exclusive time."""
import argparse
import collections
import hashlib
import json
import math
import pathlib
import statistics
import tempfile


def fields(line):
    return dict(word.split("=", 1) for word in line.split()[1:])


def summarize(path):
    raw = path.read_bytes()
    commands = []
    grouped = collections.defaultdict(list)
    command = None
    for line in raw.decode().splitlines():
        if line.startswith("PRODUCER_COMMAND_V1 "):
            command = fields(line)
            if (not math.isfinite(float(command["device_ms"])) or float(command["device_ms"]) <= 0
                    or int(command["encoders"]) <= 0
                    or int(command["cpu_end"]) <= int(command["cpu_begin"])
                    or int(command["gpu_end"]) <= int(command["gpu_begin"])):
                raise ValueError("invalid command duration or clock endpoints")
            command["intervals"] = []
            commands.append(command)
        elif line.startswith("PRODUCER_KERNEL_V1 "):
            row = fields(line)
            if command is None or any(row[key] != command[key]
                                      for key in ("stage", "raw_iteration")):
                raise ValueError("unbound kernel interval")
            if int(row["ordinal"]) != len(command["intervals"]):
                raise ValueError("missing or reordered kernel interval")
            first, last = int(row["begin_tick"]), int(row["end_tick"])
            scale = ((int(command["cpu_end"]) - int(command["cpu_begin"])) /
                     (int(command["gpu_end"]) - int(command["gpu_begin"]))) / 1e6
            if not int(command["gpu_begin"]) <= first <= last <= int(command["gpu_end"]):
                raise ValueError("counter interval outside correlated clock endpoints")
            duration = (last - first) * scale
            if not math.isclose(scale, float(row["ms_per_tick"]), rel_tol=1e-12):
                raise ValueError("wrong timestamp units")
            if not math.isclose(duration, float(row["device_ms"]), rel_tol=1e-12):
                raise ValueError("wrong encoder duration")
            command["intervals"].append((first, last))
            grouped[row["kernel"]].append(duration)
    if not commands:
        raise ValueError("no producer counters")
    unions = []
    elapsed = []
    for command in commands:
        intervals = sorted(command["intervals"])
        if len(intervals) != int(command["encoders"]):
            raise ValueError("incomplete command")
        union = 0
        first, last = intervals[0]
        for begin, end in intervals[1:]:
            if begin > last:
                union += last - first
                first, last = begin, end
            else:
                last = max(last, end)
        union += last - first
        scale = ((int(command["cpu_end"]) - int(command["cpu_begin"])) /
                 (int(command["gpu_end"]) - int(command["gpu_begin"]))) / 1e6
        unions.append(union * scale)
        elapsed.append(float(command["device_ms"]))
        if unions[-1] > elapsed[-1] + 0.01:
            raise ValueError("encoder union exceeds command elapsed time")
    return {
        "schema": "rise.fire.producer_kernel_profile.v1",
        "input_sha256": hashlib.sha256(raw).hexdigest(),
        "units": "milliseconds", "kernel_scope": "inclusive_encoder_intervals",
        "warning": "Kernel intervals can overlap. Only their union is additive.",
        "uncaptured_scope": "force-RHS encoders in another translation unit, blits, and gaps",
        "producer_commands": len(commands),
        "command_elapsed_total_ms": sum(elapsed),
        "encoder_union_total_ms": sum(unions),
        "uncaptured_total_ms": sum(elapsed) - sum(unions),
        "kernels": {name: {
            "calls": len(values), "mean_ms": statistics.mean(values),
            "stdev_ms": statistics.stdev(values) if len(values) > 1 else 0.0,
            "p95_ms": sorted(values)[math.ceil(0.95 * len(values)) - 1],
            "inclusive_total_ms": sum(values),
        } for name, values in sorted(grouped.items(), key=lambda item: -sum(item[1]))},
    }


def self_test():
    # Overlapping [100,400] and [300,700]: sum is 700 ticks, union is 600.
    valid = ("PRODUCER_COMMAND_V1 stage=0 raw_iteration=0 cpu_begin=0 cpu_end=1000 "
             "gpu_begin=0 gpu_end=1000 device_ms=0.001 encoders=2\n"
             "PRODUCER_KERNEL_V1 stage=0 raw_iteration=0 ordinal=0 kernel=A threads=1 "
             "begin_tick=100 end_tick=400 ms_per_tick=0.000001 device_ms=0.0003\n"
             "PRODUCER_KERNEL_V1 stage=0 raw_iteration=0 ordinal=1 kernel=B threads=1 "
             "begin_tick=300 end_tick=700 ms_per_tick=0.000001 device_ms=0.0004\n")
    with tempfile.TemporaryDirectory(prefix="rise-producer-counter-reds-") as directory:
        path = pathlib.Path(directory) / "fixture.log"
        path.write_text(valid)
        result = summarize(path)
        assert math.isclose(result["encoder_union_total_ms"], 0.0006)
        mutants = [valid.replace("ms_per_tick=0.000001", "ms_per_tick=0.0000416667"),
                   valid.replace("ordinal=1", "ordinal=0"),
                   valid.replace("end_tick=700", "end_tick=1100"),
                   valid.rsplit("PRODUCER_KERNEL_V1", 1)[0],
                   valid.replace("stage=0 raw_iteration=0 ordinal=1", "stage=1 raw_iteration=0 ordinal=1")]
        mutants += [valid.replace("device_ms=0.001 encoders", "device_ms=" + value + " encoders")
                    for value in ("nan", "inf", "-inf", "0", "-1")]
        mutants += [valid.replace("encoders=2", "encoders=0"),
                    valid.replace("gpu_end=1000", "gpu_end=0")]
        for mutant in mutants:
            path.write_text(mutant)
            try:
                summarize(path)
            except ValueError:
                continue
            raise AssertionError("counter mutant escaped")
    print("PRODUCER_COUNTER_REDS passed=1 count=12 overlap_union=1")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path, nargs="?")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if args.log is None:
            parser.error("log is required")
        report = json.dumps(summarize(args.log), indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(report)
        else:
            print(report, end="")
