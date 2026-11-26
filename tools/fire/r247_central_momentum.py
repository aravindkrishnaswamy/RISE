#!/usr/bin/env python3
"""Pure central-face momentum localization from caller-owned binary32 operands.

This prerequisite admits no producer files. Separate packet admission must bind
beginning, provisional, terminal and actual Heun operands before empirical use.
"""
import json
import math
import numpy as np

TERMS = ('advection', 'buoyancy', 'stress', 'phase_source', 'combined_projection', 'total', 'closure_residual')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def central_momentum(beginning, provisional, terminal, rates, stage_rates, dimensions, dt, layers=80):
    require(type(dimensions) is tuple and len(dimensions) == 3 and
            all(type(v) is int and v >= 2 for v in dimensions), 'typed domain')
    nx, ny, nz = dimensions
    require(nx % 2 == ny % 2 == 0 and type(layers) is int and 1 <= layers <= nz, 'central support')
    require(type(dt) is float and math.isfinite(dt) and 0 < dt <= float(np.finfo(np.float32).max) and
            float(np.float32(dt)) == dt, 'exact promoted binary32 dt')
    counts = ((nx+1)*ny*nz, nx*(ny+1)*nz, nx*ny*(nz+1))
    offsets = (0, counts[0], counts[0]+counts[1]); faces = sum(counts)
    require(all(isinstance(a, np.ndarray) and a.dtype == np.float32 for a in
                (beginning, provisional, terminal, rates, stage_rates)), 'binary32 momentum operands')
    require(beginning.shape == provisional.shape == terminal.shape == (faces,) and
            rates.shape == (4, faces) and stage_rates.shape == (4, 2, faces), 'momentum wire views')
    for a in (beginning, provisional, terminal):
        require(np.isfinite(a).all(), 'finite momentum')
    for term in range(4):
        require(np.isfinite(rates[term]).all() and np.isfinite(stage_rates[term, 0]).all() and
                np.isfinite(stage_rates[term, 1]).all(), 'finite actual/stage rates')

    def support(height, profiles):
        groups = [set(), set(), set()]
        associations = 0
        for y in (ny//2-1, ny//2):
            for x in (nx//2-1, nx//2):
                for z in range(height):
                    for axis, xx, yy in ((0, x, y), (0, x+1, y), (1, x, y), (1, x, y+1)):
                        groups[axis].add((xx, yy, z)); associations += 1
                for z in range(height+1):
                    groups[2].add((x, y, z)); associations += 1
        require(tuple(map(len, groups)) == (6*height, 6*height, 4*(height+1)) and
                associations == 4*(5*height+1), 'unique-face/column-association coverage')
        output = []; vertical = [[] for _ in range(height+1)] if profiles else None
        for axis, group in enumerate(groups):
            values = []
            for x, y, z in sorted(group, key=lambda pos: (pos[2], pos[1], pos[0])):
                local = x+(nx+(axis == 0))*(y+(ny+(axis == 1))*z)
                packed = offsets[axis]+local
                require(0 <= local < counts[axis], 'staggered address')
                actual = [float(rates[t, packed]) for t in range(4)]
                for t in range(4):
                    # Match the recorded binary32 Heun operation tree exactly.
                    try:
                        with np.errstate(over='raise', invalid='raise'):
                            pair = np.float32(stage_rates[t, 0, packed]+stage_rates[t, 1, packed])
                            expected = np.float32(np.float32(-.5 if t == 0 else .5)*pair)
                    except FloatingPointError as error:
                        raise ValueError('recorded binary32 stage sum overflow') from error
                    require(math.isfinite(float(expected)) and rates[t, packed].view(np.uint32) == expected.view(np.uint32), 'actual Heun stage binding')
                m0, m1, m2 = (float(a[packed]) for a in (beginning, provisional, terminal))
                combined = (m2-m1)/dt; total = (m2-m0)/dt
                # Same named producer closure: stress+buoyancy+advection+source+projection.
                closure = total-(actual[2]+actual[1]+actual[0]+actual[3]+combined)
                row = [m0, m1, m2]+actual+[combined, total, closure]
                require(all(math.isfinite(v) for v in row), 'derived momentum overflow')
                values.append(row)
                if profiles and axis == 2:
                    vertical[z].append(row)
            output.append(dict(axis=axis, unique_faces=len(group), summary=summarize(values)))
        return dict(layers=height, column_associations=associations, unique_faces=sum(map(len, groups)),
                    axes=output, vertical_face_profiles=[dict(z_face=z, unique_faces=len(rows), summary=summarize(rows))
                                                        for z, rows in enumerate(vertical)] if profiles else None)

    def summarize(rows):
        require(len(rows) > 0, 'nonempty face support')
        keys = ('beginning', 'provisional', 'terminal')+TERMS
        return {key: dict(unit='kg/m2/s' if i < 3 else 'kg/m2/s2',
                          mean=math.fsum(row[i]/len(rows) for row in rows),
                          mean_absolute=math.fsum(abs(row[i])/len(rows) for row in rows))
                for i, key in enumerate(keys)}

    return dict(support='unique faces bounding fixed central2x2 columns; face averages, not volume or boundary integrals',
                stage_support='beginning M0, provisional R1 momentum, projected R2 momentum; actual recorded Heun rates',
                native=support(nz, False), common=support(layers, True),
                common_upper_classification='native_pressure_open_boundary' if nz == layers else 'interior_transfer_plane',
                pressure_restoration_split_available=False, cumulative_full_history_available=False,
                dt_s=dt, rate_order=list(TERMS[:4]))


def self_test():
    dims = (4, 6, 81); nx, ny, nz = dims
    fx = (nx+1)*ny*nz; fy = nx*(ny+1)*nz; faces = fx+fy+nx*ny*(nz+1)
    m0 = np.zeros(faces, np.float32); m1 = np.ones(faces, np.float32); m2 = np.full(faces, 3, np.float32)
    stages = np.zeros((4, 2, faces), np.float32)
    for t, v in enumerate((2, 4, 6, 8)):
        stages[t] = v
    rates = np.array([stages[t, 0]*(-1 if t == 0 else 1) for t in range(4)], np.float32)
    r = central_momentum(m0, m1, m2, rates, stages, dims, .5)
    require(r['common']['unique_faces'] == 1284 and r['common']['column_associations'] == 1604 and
            r['native']['unique_faces'] == 1300, 'deduplicated support GREEN')
    s = r['common']['axes'][2]['summary']
    require([s[t]['mean'] for t in TERMS] == [-2, 4, 6, 8, 4, 6, -14] and
            s['advection']['mean_absolute'] == 2 and s['beginning']['unit'] == 'kg/m2/s', 'owned stage/rate units GREEN')
    require(len(r['common']['vertical_face_profiles']) == 81 and
            r['common_upper_classification'] == 'interior_transfer_plane', 'z80 support GREEN')
    # A shared x face contributes once, not twice; and z80 is included while
    # lateral faces at z80 are excluded from common80 but retained natively.
    special = (nx//2)+(nx+1)*((ny//2-1)+ny*0)
    altered = m0.copy(); altered[special] = 480
    edge = altered.copy(); edge[(nx//2)+(nx+1)*((ny//2-1)+ny*80)] = 480
    changed = central_momentum(edge, m1, m2, rates, stages, dims, .5)
    require(changed['common']['axes'][0]['summary']['beginning']['mean'] == 1 and
            changed['native']['axes'][0]['summary']['beginning']['mean'] == 960/486, 'shared face and lateral exclusion GREEN')
    z80 = fx+fy+(nx//2-1)+nx*((ny//2-1)+ny*80)
    shifted = m0.copy(); shifted[z80] = 4
    top = central_momentum(shifted, m1, m2, rates, stages, dims, .5)
    require(top['common']['vertical_face_profiles'][80]['summary']['beginning']['mean'] == 1, 'included upper zface GREEN')
    args = [m0, m1, m2, rates, stages, dims, .5]; reds = 0
    variants = [(0, m0.astype(np.float64)), (1, m1[:-1]), (3, rates[:3]), (4, stages[:, :1]),
                (5, (3, 6, 81)), (5, (4, 6, True)), (6, .1), (6, 0.), (6, float('inf'))]
    corrupt = rates.copy(); corrupt[0, special] = 0; variants.append((3, corrupt))
    corrupt_stages = stages.copy(); corrupt_stages[1, 0, z80] = 0; variants.append((4, corrupt_stages))
    nonfinite = m0.copy(); nonfinite[0] = np.nan; variants.append((0, nonfinite))
    overflow = stages.copy(); overflow[0, :, special] = np.finfo(np.float32).max
    variants.append((4, overflow))
    for i, value in variants:
        bad = args.copy(); bad[i] = value
        try: central_momentum(*bad)
        except ValueError: reds += 1
    for height in (True, 0, 82):
        try: central_momentum(*args, layers=height)
        except ValueError: reds += 1
    zeros = stages.copy(); zeros[2, :, special] = 0
    signed_zero = rates.copy(); signed_zero[2, special] = np.float32(-0.)
    bad = args.copy(); bad[3] = signed_zero; bad[4] = zeros
    try: central_momentum(*bad)
    except ValueError: reds += 1
    require(reds == 17, 'momentum operand/support/stage REDs')
    near_max = stages.copy(); near_max[1, 0, special] = np.finfo(np.float32).max
    near_max[1, 1, special] = np.float32(2.**102)
    near_rates = rates.copy()
    with np.errstate(over='raise', invalid='raise'):
        near_rates[1, special] = np.float32(.5)*np.float32(near_max[1, 0, special]+near_max[1, 1, special])
    finite = central_momentum(m0, m1, m2, near_rates, near_max, dims, .5)
    require(math.isfinite(finite['common']['axes'][0]['summary']['buoyancy']['mean']), 'finite rounded maximum GREEN')
    return dict(status='synthetic_central_momentum_arithmetic_only', green=6, red=reds,
                native_payloads_read=0, empirical=False, full_derivation_accepted=False)


if __name__ == '__main__':
    print(json.dumps(self_test(), sort_keys=True, allow_nan=False))
