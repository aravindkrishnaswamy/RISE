#!/usr/bin/env python3
"""Frozen-source spatial moments, with signed power distinct from positive weights.

Pure arithmetic prerequisite for r247. This module does not admit producer files,
read native capsules, authenticate source provenance, or qualify physical fire.
"""
import json
import math
import numpy as np


def require(condition, message):
    if not condition:
        raise ValueError(message)


def source_moments(gas, soot, dx, common_layers=80):
    require(isinstance(gas, np.ndarray) and isinstance(soot, np.ndarray) and gas.dtype == np.float64 and
            soot.dtype == np.float64 and gas.shape == soot.shape and gas.ndim == 3, 'source type/shape')
    nz, ny, nx = gas.shape
    require(nx >= 2 and ny >= 2 and nx % 2 == ny % 2 == 0 and nz >= 1 and
            type(common_layers) is int and 1 <= common_layers <= nz, 'source domain/support')
    require(type(dx) is float and np.isfinite(dx) and dx > 0 and
            np.isfinite(gas).all() and np.isfinite(soot).all(), 'nonfinite geometry/source')
    chemical = gas+soot
    require(np.isfinite(chemical).all(), 'chemical sum overflow')
    positive = np.maximum(chemical, 0.0)
    negative = np.minimum(chemical, 0.0)
    volume = dx**3
    require(np.isfinite(volume) and volume > 0, 'cell volume overflow/underflow')

    def support(layers):
        weights = positive[:layers]
        total = float(weights.sum())
        scale = float(weights.max())
        scaled = np.ldexp(weights, -math.frexp(scale)[1]) if scale > 0 else weights
        scaled_total = float(scaled.sum())
        signed_power = float(chemical[:layers].sum()*volume)
        positive_power = float(total*volume)
        negative_power = float(negative[:layers].sum()*volume)
        columns = scaled[:, ny//2-1:ny//2+1, nx//2-1:nx//2+1]
        centroids = None; variances = None
        if total > 0:
            centroids = []; variances = []
            for axis, length in ((2, nx), (1, ny), (0, layers)):
                marginal = scaled.sum(axis=tuple(a for a in range(3) if a != axis))
                coordinates = np.arange(length, dtype=np.float64)+0.5
                centre = float(np.dot(marginal, coordinates)/scaled_total)
                # Centred squares avoid subtracting nearly equal raw second moments.
                variance = float(np.dot(marginal, (coordinates-centre)**2)/scaled_total)
                centroids.append(centre); variances.append(variance)
        values = [signed_power, positive_power, negative_power, total]
        if centroids is not None:
            values += centroids+variances
        require(np.isfinite(values).all(), 'moment overflow')
        return dict(layers=layers, signed_chemical_power_W=signed_power,
                    positive_chemical_power_W=positive_power, negative_chemical_power_W=negative_power,
                    positive_cells=int(np.count_nonzero(weights)),
                    positive_power_underflow=bool(total > 0 and positive_power == 0),
                    negative_power_underflow=bool(np.any(negative[:layers] < 0) and negative_power == 0),
                    central_four_column_positive_fraction=float(columns.sum()/scaled_total) if total > 0 else None,
                    centroid_cell_centre_xyz=centroids,
                    centroid_m_xyz=[x*dx for x in centroids] if centroids is not None else None,
                    variance_cell_squared_xyz=variances,
                    lateral_rms_m=float(np.sqrt(variances[0]+variances[1])*dx) if variances is not None else None,
                    vertical_std_m=float(np.sqrt(variances[2])*dx) if variances is not None else None,
                    layer_signed_chemical_power_W=(chemical[:layers].sum(axis=(1, 2))*volume).tolist())

    native = support(nz); common = support(common_layers)
    global_scale = float(positive.max())
    fraction_weights = np.ldexp(positive, -math.frexp(global_scale)[1]) if global_scale > 0 else positive
    common_fraction = float(fraction_weights[:common_layers].sum()/fraction_weights.sum()) if global_scale > 0 else None
    require(np.isfinite(native['layer_signed_chemical_power_W']).all() and
            np.isfinite(common['layer_signed_chemical_power_W']).all(), 'layer power overflow')
    return dict(coordinate_convention='Native origin plus (integer cell index + 0.5) times recorded binary32 solver dx',
                moment_weight='positive part of gas-plus-soot frozen chemical source, W/m3',
                temporal_support='one owned frozen-source snapshot, not terminal fields or cumulative energy',
                solver_dx_m=dx, native=native, common=common,
                power_representation='rounded binary64 W; explicit positive/negative underflow flags',
                common_positive_fraction=common_fraction)


def self_test():
    # Two positive points of unequal weights, one negative point. Independent
    # hand result: total positive4, signed3, centroid(2,1.25,0.5), lateral var15/16.
    gas = np.zeros((81, 4, 4), dtype=np.float64); soot = gas.copy()
    gas[0, 0, 0] = 1; soot[0, 1, 2] = 3; gas[0, 3, 3] = -1
    result = source_moments(gas, soot, 0.5)
    native = result['native']
    require(native['positive_chemical_power_W'] == .5 and native['signed_chemical_power_W'] == .375 and
            native['negative_chemical_power_W'] == -.125, 'signed units GREEN')
    require(native['centroid_cell_centre_xyz'] == [2, 1.25, .5] and
            native['centroid_m_xyz'] == [1, .625, .25], 'unequal-weight cell-centre GREEN')
    require(native['variance_cell_squared_xyz'] == [.75, .1875, 0] and
            native['central_four_column_positive_fraction'] == .75, 'spread/central support GREEN')
    translated_gas = np.roll(gas, 1, axis=0); translated_soot = np.roll(soot, 1, axis=0)
    translated = source_moments(translated_gas, translated_soot, .5)['native']
    require(translated['centroid_cell_centre_xyz'] == [2, 1.25, 1.5] and
            translated['variance_cell_squared_xyz'] == native['variance_cell_squared_xyz'], 'translation GREEN')
    gas[80, 0, 0] = 4
    split = source_moments(gas, soot, .5)
    require(split['common_positive_fraction'] == .5 and split['common']['signed_chemical_power_W'] == .375 and
            split['native']['signed_chemical_power_W'] == .875 and
            split['common']['centroid_cell_centre_xyz'] == [2, 1.25, .5], 'layer80 exclusion GREEN')
    zero = np.zeros_like(gas)
    for a in (zero, -np.ones_like(gas)):
        null = source_moments(a, zero, .5)
        require(null['native']['centroid_cell_centre_xyz'] is None and
                null['native']['central_four_column_positive_fraction'] is None and
                null['common_positive_fraction'] is None, 'zero positive support unavailable GREEN')
    tiny = np.zeros((80, 2, 2), dtype=np.float64)
    tiny[0, 0, 0] = np.nextafter(0., 1.)
    for axis in range(3):
        shifted = np.roll(tiny, 1, axis=axis)
        small = source_moments(shifted, np.zeros_like(tiny), .5)
        expected = [.5, .5, .5]; expected[2-axis] = 1.5
        require(small['native']['centroid_cell_centre_xyz'] == expected and
                small['common_positive_fraction'] == 1 and
                small['native']['positive_power_underflow'], 'subnormal translated support GREEN')
    tiny[0, 0, 0] = 1e-100
    small = source_moments(tiny, np.zeros_like(tiny), 1e-100)
    require(small['common_positive_fraction'] == 1 and small['native']['positive_power_underflow'],
            'dimensionless fraction independent of volume GREEN')
    red = 0
    nan = gas.copy(); nan[0, 0, 0] = np.nan
    for args in [(gas.astype(np.float32), soot, .5), (gas, soot[:, :, :3], .5), (nan, soot, .5),
                 (gas, soot, 0.), (gas, soot, float('inf')), (gas, soot, .5, True), (gas, soot, .5, 82)]:
        try:
            source_moments(*args)
        except ValueError:
            red += 1
    require(red == 7, 'type/shape/finite/geometry/support REDs')
    return dict(status='synthetic_arithmetic_prerequisite_only', hand_signed_centroid_green=True,
                translated_spread_green=True, common80_exclusion_green=True, zero_positive_unavailable_green=True,
                subnormal_translation_green=True, volume_independent_fraction_green=True,
                red=red, native_capsules_read=0, empirical=False)


if __name__ == '__main__':
    print(json.dumps(self_test(), sort_keys=True, allow_nan=False))
