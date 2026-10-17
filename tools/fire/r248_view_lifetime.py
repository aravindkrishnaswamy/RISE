#!/usr/bin/env python3
"""Synthetic typed lease/overwrite/target-binding control model only.

Python object identity simulates issuance; it is not native authority, security,
GPU ordering, authenticated field provenance, or an accepted history chain.
"""
from dataclasses import dataclass


def require(ok, message):
    if not ok:
        raise ValueError(message)


def integer(value, maximum, name):
    require(type(value) is int and 0 <= value <= maximum, name)
    return value


# This first model covers two explicit operation/channel bindings only.
OPERATIONS = {'chemical_binary64': ('chemical_pair', 64),
              'target_pre_finalize': ('assembled_target', 32)}


@dataclass(frozen=True, eq=False)
class Stage:
    name: str
    parent: object


@dataclass(frozen=True, eq=False)
class View:
    stage: Stage
    allocation: int
    generation: int
    role: str
    width: int
    offset: int
    length: int
    size: int


@dataclass(frozen=True, eq=False)
class Read:
    view: View
    operation: str


@dataclass(frozen=True, eq=False)
class Target:
    before: View
    after: View
    parent: object


class Model:
    """Bounded synthetic registry. Caller events are not Metal command evidence."""
    def __init__(self, limit=64):
        integer(limit, 4096, 'bounded model registry limit')
        require(limit > 0, 'positive model registry limit')
        self.limit = limit
        self.stages = set()
        self.views = {}
        self.reads = {}
        self.targets = set()
        self.next_allocation = 0

    def stage(self, name, parent=None):
        require(type(name) is str and name in ('R0', 'R1', 'R2'), 'typed stage name')
        require(parent is None or parent in self.stages, 'issued stage parent')
        require((name == 'R0' and parent is None) or
                (name == 'R1' and parent is not None and parent.name == 'R0') or
                (name == 'R2' and parent is not None and parent.name == 'R1'), 'stage parent order')
        require(len(self.stages) < self.limit, 'stage registry bound')
        s = Stage(name, parent); self.stages.add(s)
        return s

    def view(self, stage, role, width, offset, length, size):
        require(stage in self.stages, 'issued live stage')
        require(type(role) is str and role in ('chemical_pair', 'assembled_target'), 'typed channel role')
        require(type(width) is int and width == (64 if role == 'chemical_pair' else 32), 'channel bit width')
        for n in (offset, length, size): integer(n, (1 << 64) - 1, 'typed uint64 view extent')
        unit = width // 8
        require(length > 0 and offset % unit == 0 and length % unit == 0 and
                offset <= size and length <= size - offset, 'aligned in-allocation extent')
        require(len(self.views) < self.limit, 'view registry bound')
        integer(self.next_allocation + 1, (1 << 64) - 1, 'allocation identity overflow')
        self.next_allocation += 1
        v = View(stage, self.next_allocation, 0, role, width, offset, length, size)
        self.views[v.allocation] = v
        return v

    def live(self, v):
        require(type(v) is View and self.views.get(v.allocation) is v and
                v.stage in self.stages, 'issued current live view generation')

    def schedule(self, view, operation):
        self.live(view)
        require(type(operation) is str and operation in OPERATIONS, 'typed operation')
        require((view.role, view.width) == OPERATIONS[operation], 'operation/channel binding')
        require(len(self.reads) < self.limit, 'result registry bound')
        r = Read(view, operation); self.reads[r] = 'queued'
        return r

    def complete(self, result):
        require(type(result) is Read and self.reads.get(result) == 'queued', 'issued queued read')
        self.live(result.view)
        self.reads[result] = 'captured'

    def overwrite(self, view):
        self.live(view)
        require(not any(r.view is view and state == 'queued' for r, state in self.reads.items()),
                'no overwrite before queued capture completes')
        integer(view.generation + 1, (1 << 64) - 1, 'generation overflow')
        v = View(view.stage, view.allocation, view.generation + 1, view.role,
                 view.width, view.offset, view.length, view.size)
        self.views[v.allocation] = v
        return v

    def target(self, before, after, parent=None):
        self.live(after)
        require(type(before) is View and before.role == 'assembled_target' and
                after.role == before.role and after.stage is before.stage and
                before.allocation == after.allocation and after.generation == before.generation + 1,
                'same stage allocation immediate finalization generation')
        # A copied descriptor cannot stand in for the issued pre-final view:
        require(any(r.view is before and state == 'captured' and r.operation == 'target_pre_finalize'
                    for r, state in self.reads.items()), 'issued captured pre-finalization view')
        require(parent is None or parent in self.targets, 'issued target parent')
        require(parent is None or parent.after.stage is before.stage or
                parent.after.stage is before.stage.parent, 'target parent stage lineage')
        require(not any(t.before is before for t in self.targets), 'unique finalization issuance')
        require(len(self.targets) < self.limit, 'target registry bound')
        t = Target(before, after, parent); self.targets.add(t)
        return t

    def bind(self, result, target):
        require(type(result) is Read and self.reads.get(result) == 'captured', 'issued captured result')
        require(target in self.targets and result.operation == 'target_pre_finalize' and
                result.view is target.before, 'exact pre-final view target binding')
        self.live(target.after)
        self.reads[result] = 'bound'

    def discard(self, stage):
        require(stage in self.stages, 'issued stage to discard')
        require(not any(s.parent is stage for s in self.stages), 'discard children first')
        require(not any(r.view.stage is stage and state == 'queued' for r, state in self.reads.items()),
                'complete queued reductions before release')
        # Diagnostic publication rights are removed even for bound records.
        self.reads = {r: state for r, state in self.reads.items() if r.view.stage is not stage}
        self.targets = {t for t in self.targets if t.before.stage is not stage}
        self.views = {a: v for a, v in self.views.items() if v.stage is not stage}
        self.stages.remove(stage)
