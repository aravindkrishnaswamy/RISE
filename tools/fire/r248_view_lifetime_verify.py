#!/usr/bin/env python3
"""Adversarial synthetic identity/overwrite controls; no native observation."""
from dataclasses import replace
import importlib.util
import json
from pathlib import Path
import sys


def main():
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=False)
    p = Path(__file__).with_name('r248_view_lifetime.py')
    spec = importlib.util.spec_from_file_location('lease_model', p)
    m = importlib.util.module_from_spec(spec); sys.modules[spec.name] = m; spec.loader.exec_module(m)
    reds = []; greens = 0
    def refuse(name, fn):
        try: fn()
        except (ValueError, TypeError): reds.append(name)
        else: raise AssertionError(name)
    a = m.Model(); b = m.Model()
    s = a.stage('R0'); other = b.stage('R0')
    v = a.view(s, 'assembled_target', 32, 4, 12, 32)
    r = a.schedule(v, 'target_pre_finalize')
    refuse('overwrite_queued', lambda: a.overwrite(v))
    refuse('release_queued', lambda: a.discard(s))
    refuse('copied_result', lambda: a.complete(replace(r)))
    refuse('copied_view', lambda: a.schedule(replace(v), 'target_pre_finalize'))
    refuse('cross_owner_stage', lambda: a.view(other, 'assembled_target', 32, 0, 4, 4))
    refuse('wrong_operation', lambda: a.schedule(v, 'chemical_binary64'))
    a.complete(r); greens += 1
    refuse('complete_twice', lambda: a.complete(r))
    after = a.overwrite(v)
    refuse('stale_view_schedule', lambda: a.schedule(v, 'target_pre_finalize'))
    refuse('stale_view_overwrite', lambda: a.overwrite(v))
    refuse('copied_before', lambda: a.target(replace(v), after))
    refuse('unissued_target_parent', lambda: a.target(v, after, m.Target(v, after, None)))
    t = a.target(v, after); a.bind(r, t); greens += 1
    refuse('bind_twice', lambda: a.bind(r, t))
    a.schedule(after, 'target_pre_finalize')
    # Complete the intentionally queued second-generation capture before cleanup.
    pending = next(x for x, state in a.reads.items() if state == 'queued'); a.complete(pending)
    newer = a.overwrite(after)
    refuse('skipped_finalization_generation', lambda: a.target(v, newer))
    refuse('stale_after_target', lambda: a.target(v, after))
    t2 = a.target(after, newer, t)
    refuse('bind_copied_target', lambda: a.bind(pending, replace(t2)))
    refuse('duplicate_issuance', lambda: a.target(after, newer, t))
    a.bind(pending, t2); greens += 1
    child = a.stage('R1', s)
    refuse('parent_release_before_child', lambda: a.discard(s))
    a.discard(child); a.discard(s)
    assert not a.stages and not a.views and not a.reads and not a.targets; greens += 1
    refuse('evicted_result', lambda: a.bind(r, t))
    refuse('evicted_view', lambda: a.schedule(newer, 'target_pre_finalize'))
    refuse('evicted_stage', lambda: a.stage('R1', s))
    for name, fn in [('bool_limit', lambda: m.Model(True)), ('zero_limit', lambda: m.Model(0)),
                     ('R2_no_parent', lambda: m.Model().stage('R2')), ('unknown_stage', lambda: m.Model().stage('R3'))]:
        refuse(name, fn)
    c = m.Model(1); z = c.stage('R0')
    for name, args in [('bool_width', ('assembled_target', True, 0, 4, 4)),
                       ('chemical_narrowing', ('chemical_pair', 32, 0, 4, 4)),
                       ('misaligned', ('assembled_target', 32, 1, 4, 8)),
                       ('outside', ('assembled_target', 32, 8, 4, 8)),
                       ('uint64_extent', ('assembled_target', 32, 0, 4, 1 << 64)),
                       ('empty_extent', ('assembled_target', 32, 0, 0, 0))]:
        refuse(name, lambda args=args: c.view(z, *args))
    cv = c.view(z, 'chemical_pair', 64, 0, 16, 16); cr = c.schedule(cv, 'chemical_binary64')
    refuse('view_registry_bound', lambda: c.view(z, 'chemical_pair', 64, 0, 16, 16))
    refuse('read_registry_bound', lambda: c.schedule(cv, 'chemical_binary64'))
    refuse('stage_registry_bound', lambda: c.stage('R0'))
    c.complete(cr); c.discard(z); greens += 1
    # Independent temporal expectation: only current descriptor schedules; only
    # completed capture survives the immediately subsequent writer for binding.
    for i in range(100):
        x = m.Model(); st = x.stage('R0'); view = x.view(st, 'assembled_target', 32, 0, 4, 4)
        captures = []
        for j in range(8):
            rr = x.schedule(view, 'target_pre_finalize'); x.complete(rr)
            vv = x.overwrite(view); tt = x.target(view, vv); x.bind(rr, tt)
            assert rr.view.generation == j and vv.generation == j + 1
            captures.append(rr); view = vv; greens += 1
        x.discard(st)
        assert all(rr not in x.reads for rr in captures) and not x.views and not x.targets
        greens += 1
    report = {'synthetic_green_checks': greens, 'reds': reds, 'operation_channels': m.OPERATIONS,
              'native_authentication': False, 'gpu_command_ordering_qualified': False,
              'full_accepted_history_chain': False, 'measured_memory': False, 'new_simulation': False}
    (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report))


if __name__ == '__main__': main()
