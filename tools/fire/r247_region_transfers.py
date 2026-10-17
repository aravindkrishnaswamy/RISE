#!/usr/bin/env python3
"""Pure selected-interval scalar control-volume arithmetic prerequisite.

Caller-owned binary32 operands only. No producer admission, payload reads,
physical qualification or full-history cumulative budget is performed here.
"""
import json
import math
import numpy as np

FIELDS = ('mixture_fraction_density', 'CH4', 'O2', 'N2', 'CO2', 'H2O', 'CO', 'carbon', 'sensible_enthalpy', 'total_species')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def region_transfers(q0, q1, source, low, anti, alpha, burner, dx, dt, layers=80):
    require(all(isinstance(a, np.ndarray) and a.dtype == np.float32 for a in
                (q0, q1, source, low, anti, alpha)), 'binary32 operand type')
    require(q0.ndim == 4 and q0.shape[0] == 9 and q1.shape == source.shape == q0.shape, 'state layout')
    _, nz, ny, nx = q0.shape
    require(nx >= 2 and ny >= 2 and nz >= 1 and type(layers) is int and 1 <= layers <= nz, 'region domain')
    fx = (nx+1)*ny*nz; fy = nx*(ny+1)*nz; fz = nx*ny*(nz+1); faces = fx+fy+fz
    require(low.shape == anti.shape == (9, faces) and alpha.shape == (faces,), 'face layout')
    require(isinstance(burner, np.ndarray) and burner.dtype == np.uint8 and burner.shape == (ny, nx) and
            np.all((burner == 0) | (burner == 1)), 'burner mask')
    require(type(dx) is float and type(dt) is float and math.isfinite(dx) and math.isfinite(dt) and
            dx > 0 and dt > 0, 'geometry/clock')
    require(all(value <= float(np.finfo(np.float32).max) and float(np.float32(value)) == value
                for value in (dx, dt)), 'exact promoted binary32 geometry/clock')
    volume = dx**3; area_dt = dx**2*dt
    require(math.isfinite(volume) and math.isfinite(area_dt) and volume > 0 and area_dt > 0, 'step measure')
    # Validate one field at a time: no nine-field finite-check temporary.
    for field in range(9):
        require(all(np.isfinite(a[field]).all() for a in (q0, q1, source, low, anti)), 'nonfinite field')
    require(np.isfinite(alpha).all() and np.all(alpha >= 0) and np.all(alpha <= 1), 'limiter range')

    def planes(a):
        x = a[:fx].reshape(nz, ny, nx+1)
        y = a[fx:fx+fy].reshape(nz, ny+1, nx)
        z = a[fx+fy:].reshape(nz+1, ny, nx)
        return (x[:layers, :, 0], x[:layers, :, nx], y[:layers, 0, :], y[:layers, ny, :], z[0], z[layers])

    alpha_planes = planes(alpha)
    names = ('xmin', 'xmax', 'ymin', 'ymax', 'bottom', 'upper')
    orientation = (-1, 1, -1, 1, -1, 1)
    result = []
    for field, name in enumerate(FIELDS):
        indices = (field,) if field < 9 else tuple(range(1, 8))
        inventories = []
        for state in (q0, q1, source):
            # Total species is formed per cell before volume reduction.
            cells = state[indices[0], :layers].astype(np.float64)
            for i in indices[1:]:
                cells += state[i, :layers]
            inventories.append(float(cells.sum(dtype=np.float64)*volume))
        sides = {}
        for side, direction, index in zip(names, orientation, range(6)):
            # Sum species limited flux per FACE before signed in/out splitting.
            face_values = np.zeros(alpha_planes[index].shape, dtype=np.float64)
            for i in indices:
                lo = planes(low[i])[index].astype(np.float64)
                ad = planes(anti[i])[index].astype(np.float64)
                face_values += lo+alpha_planes[index]*ad
            increments = face_values*(direction*area_dt)
            require(np.isfinite(increments).all(), 'surface increment overflow')

            def signed(part):
                return dict(incoming=float(np.maximum(-part, 0).sum(dtype=np.float64)),
                            outgoing=float(np.maximum(part, 0).sum(dtype=np.float64)),
                            net_outward=float(part.sum(dtype=np.float64)))

            sides[side] = signed(increments)
            if side == 'bottom':
                sides[side]['burner'] = signed(increments[burner == 1])
                sides[side]['nonburner'] = signed(increments[burner == 0])
        net = math.fsum(sides[side]['net_outward'] for side in names)
        start, end, increment = inventories
        closure = math.fsum((end, -start, -increment, net))
        numbers = inventories+[net, closure]
        numbers += [sides[s][k] for s in names for k in ('incoming', 'outgoing', 'net_outward')]
        require(all(math.isfinite(v) for v in numbers), 'inventory/reduction overflow')
        result.append(dict(field=name, unit='J' if field == 8 else 'kg', beginning=start, candidate=end,
                           frozen_source_increment=increment, surfaces=sides, net_outward=net,
                           closure_residual=closure))
    return dict(layers=layers, native_layers=nz, upper_face=layers,
                upper_classification='native_pressure_open_boundary' if layers == nz else 'interior_transfer_plane',
                support='one selected accepted interval; Q0/R1 candidate and recorded limited flux',
                cumulative_full_history_available=False, dx_m=dx, dt_s=dt, fields=result)


def self_test():
    nx = ny = 2; nz = 81
    fx = (nx+1)*ny*nz; fy = nx*(ny+1)*nz; faces = fx+fy+nx*ny*(nz+1)
    state = np.zeros((9, nz, ny, nx), dtype=np.float32)
    q1 = state.copy(); source = state.copy()
    low = np.zeros((9, faces), dtype=np.float32); anti = low.copy(); alpha = np.zeros(faces, dtype=np.float32)
    mask = np.array([[1, 0], [0, 0]], dtype=np.uint8)
    # dx=.5, dt=.25 => volume1/8 and area*dt1/16.
    state[1, 0, 0, 0] = 8; q1[1, 0, 0, 0] = 12; source[1, 0, 0, 0] = 4
    state[1, 80] = 100; q1[1, 80] = 200; source[1, 80] = 300
    z0 = fx+fy; z80 = z0+80*4; z81 = z0+81*4
    low[1, z80:z80+4] = (2, -4, 0, 0)
    anti[1, z80:z80+4] = (4, 4, 0, 0); alpha[z80:z80+4] = .5
    low[1, z81:z81+4] = 100
    low[1, z0:z0+4] = (8, -8, 0, 0)
    # Opposite species on same face must cancel BEFORE sign splitting.
    low[2, z80:z80+4] = (-4, 2, 0, 0)
    result = region_transfers(state, q1, source, low, anti, alpha, mask, .5, .25)
    ch4 = result['fields'][1]; total = result['fields'][9]
    require((ch4['beginning'], ch4['candidate'], ch4['frozen_source_increment']) == (1., 1.5, .5), 'common inventories GREEN')
    require(ch4['surfaces']['upper'] == dict(incoming=.125, outgoing=.25, net_outward=.125), 'limited signed upper GREEN')
    require(total['surfaces']['upper'] == dict(incoming=0., outgoing=0., net_outward=0.), 'species counterflow GREEN')
    require(ch4['surfaces']['bottom']['burner']['incoming'] == .5 and
            ch4['surfaces']['bottom']['nonburner']['outgoing'] == .5 and ch4['closure_residual'] == .125,
            'bottom masks and nonforced closure GREEN')
    require(result['upper_classification'] == 'interior_transfer_plane', 'interior GREEN')
    # Every orientation/lateral truncation is hand checked separately.
    test_low = np.zeros_like(low)
    x = test_low[8, :fx].reshape(nz, ny, nx+1); y = test_low[8, fx:fx+fy].reshape(nz, ny+1, nx)
    x[0, 0, 0] = 16; x[0, 0, nx] = 32; y[0, 0, 0] = 48; y[0, ny, 0] = 64
    x[80, :, 0] = 1024; y[80, 0, :] = 1024
    enthalpy = region_transfers(state, q1, source, test_low, anti*0, alpha*0, mask, .5, .25)['fields'][8]
    require(enthalpy['unit'] == 'J' and enthalpy['net_outward'] == 2 and
            [enthalpy['surfaces'][s]['net_outward'] for s in ('xmin', 'xmax', 'ymin', 'ymax')] == [-1, 2, -3, 4],
            'six orientation/units/lateral exclusion GREEN')
    wide = region_transfers(state[:, :80].copy(), q1[:, :80].copy(), source[:, :80].copy(),
                            np.zeros((9, 3*2*80+2*3*80+2*2*81), np.float32),
                            np.zeros((9, 3*2*80+2*3*80+2*2*81), np.float32),
                            np.zeros(3*2*80+2*3*80+2*2*81, np.float32), mask, .5, .25)
    require(wide['upper_classification'] == 'native_pressure_open_boundary', 'wide boundary GREEN')
    # All three limiter endpoints have distinct expected upper net increments.
    for value, expected in ((0., -.125), (.5, .125), (1., .375)):
        a = alpha.copy(); a[z80:z80+4] = value
        r = region_transfers(state, q1, source, low, anti, a, mask, .5, .25)
        require(r['fields'][1]['surfaces']['upper']['net_outward'] == expected, 'alpha endpoints GREEN')
    args = [state, q1, source, low, anti, alpha, mask, .5, .25]
    variants = []
    for index, replacement in ((0, state.astype(np.float64)), (1, q1[:, :80]), (3, low[:, :-1]),
                               (5, np.full_like(alpha, -1)), (5, np.full_like(alpha, 2)),
                               (6, mask.astype(bool)), (6, np.full_like(mask, 2)), (7, 0.), (8, float('inf')),
                               (7, 1e-95), (7, 1e308), (7, .1), (8, .1)):
        bad = args.copy(); bad[index] = replacement; variants.append(bad)
    bad_low = low.copy(); bad_low[0, 0] = np.nan
    bad = args.copy(); bad[3] = bad_low; variants.append(bad)
    reds = 0
    for bad in variants:
        try:
            region_transfers(*bad)
        except ValueError:
            reds += 1
    for layers in (True, 0, 82):
        try:
            region_transfers(*args, layers=layers)
        except ValueError:
            reds += 1
    require(reds == 17, 'type/layout/finite/limiter/geometry REDs')
    tiny = np.zeros_like(state); tiny[1, 0, 0, 0] = np.nextafter(np.float32(0), np.float32(1))
    minimum_dx = float(tiny[1, 0, 0, 0])
    minimum = region_transfers(tiny, tiny, source*0, low*0, anti*0, alpha*0, mask, minimum_dx, minimum_dx)
    require(minimum['fields'][1]['beginning'] > 0, 'promoted binary32 subnormal geometry GREEN')
    return dict(status='synthetic_scalar_arithmetic_prerequisite_only', green=9, red=reds,
                native_payloads_read=0, empirical=False, full_history_budget=False)


if __name__ == '__main__':
    print(json.dumps(self_test(), sort_keys=True, allow_nan=False))
