#!/usr/bin/env python3
"""ctypes bridge for the quarantined transmission/LBL accumulator.

Not used by the production section-3.5 optically-thin Planck-mean record.
"""

from __future__ import annotations

import bz2
import ctypes
import math
from pathlib import Path
from typing import Iterator, Sequence

from fire_gas_opacity import C2_CM_K, PartitionSums


def iter_line_blocks(path: Path, compression: str,
                     block_lines: int = 65536) -> Iterator[tuple[bytes, int]]:
    if compression not in {"none", "bzip2"}:
        raise ValueError("native line archive compression is unsupported")
    opener = bz2.open if compression == "bzip2" else open
    with opener(path, "rb") as source:
        rows = []
        for raw in source:
            if raw.strip():
                rows.append(raw)
            if len(rows) == block_lines:
                yield b"".join(rows), len(rows)
                rows.clear()
        if rows:
            yield b"".join(rows), len(rows)


def _nested(values: Sequence[float | int], dimensions: Sequence[int]):
    if len(dimensions) == 1:
        return [values[index] for index in range(dimensions[0])]
    stride = math.prod(dimensions[1:])
    return [_nested(values[index * stride:(index + 1) * stride], dimensions[1:])
            for index in range(dimensions[0])]


def species_spectra_batch_native(
        library_path: Path, source: dict, root: Path,
        molar_masses_kg_per_mol: dict[int, float],
        partition_sums: PartitionSums, grid_cm1: list[float],
        temperatures_k: Sequence[float], pressure_pa: float,
        self_mole_fractions: Sequence[float], wing_cutoffs_cm1: Sequence[float],
        radiation_temperatures_k: Sequence[float],
        visible_wavenumber_interval_cm1: Sequence[float],
):
    library = ctypes.CDLL(str(library_path))
    function = library.RiseFireGasOpacityAccumulate
    function.restype = ctypes.c_int
    double_pointer = ctypes.POINTER(ctypes.c_double)
    function.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int,
        double_pointer, double_pointer, double_pointer, double_pointer,
        ctypes.c_int, double_pointer, ctypes.c_int, ctypes.c_double,
        double_pointer, ctypes.c_int, double_pointer, ctypes.c_int,
        ctypes.c_double, ctypes.c_double, ctypes.c_int,
        double_pointer, ctypes.c_int, ctypes.c_double, ctypes.c_double,
        double_pointer, ctypes.POINTER(ctypes.c_uint64), double_pointer,
        double_pointer, double_pointer, double_pointer,
        ctypes.c_char_p, ctypes.c_size_t,
    ]
    molecule = int(source["molecule_number"])
    isotopologues = sorted(molar_masses_kg_per_mol)
    capacity = max(isotopologues) + 1

    def doubles(values: Sequence[float]):
        return (ctypes.c_double * len(values))(*values)

    masses = [0.0] * capacity
    q_reference = [0.0] * capacity
    q_at_temperature = [0.0] * (len(temperatures_k) * capacity)
    q_minimum_in_temperature_cell = [0.0] * (
        max(1, len(temperatures_k) - 1) * capacity)
    for isotope in isotopologues:
        masses[isotope] = molar_masses_kg_per_mol[isotope]
        q_reference[isotope] = partition_sums.evaluate(molecule, isotope, 296.0)
        for temperature_index, temperature in enumerate(temperatures_k):
            q_at_temperature[temperature_index * capacity + isotope] = (
                partition_sums.evaluate(molecule, isotope, temperature))
        for cell in range(max(1, len(temperatures_k) - 1)):
            minimum = temperatures_k[cell]
            maximum = (temperatures_k[cell + 1]
                       if len(temperatures_k) > 1 else minimum)
            q_minimum_in_temperature_cell[cell * capacity + isotope] = (
                partition_sums.minimum(molecule, isotope, minimum, maximum))
    temperatures = doubles(temperatures_k)
    self_fractions = doubles(self_mole_fractions)
    cutoffs = doubles(wing_cutoffs_cm1)
    radiation = doubles(radiation_temperatures_k)
    masses_array = doubles(masses)
    q_reference_array = doubles(q_reference)
    q_temperature_array = doubles(q_at_temperature)
    q_minimum_cell_array = doubles(q_minimum_in_temperature_cell)
    state_count = (len(wing_cutoffs_cm1) * len(self_mole_fractions) *
                   len(temperatures_k))
    spectrum_count = state_count * len(grid_cm1)
    spectra = (ctypes.c_double * spectrum_count)()
    counts = (ctypes.c_uint64 * state_count)()
    tails = (ctypes.c_double * state_count)()
    center_numerators = (ctypes.c_double * (
        len(temperatures_k) * len(radiation_temperatures_k)))()
    visible_bounds = (ctypes.c_double * (
        len(self_mole_fractions) * len(temperatures_k)))()
    visible_cell_bounds = (ctypes.c_double * (
        max(1, len(self_mole_fractions) - 1) *
        max(1, len(temperatures_k) - 1)))()
    error = ctypes.create_string_buffer(512)
    archive_line_count = 0
    for item in source["files"]:
        path = Path(item["path"])
        local = path if path.is_absolute() else root / path
        for block, block_line_count in iter_line_blocks(local, item["compression"]):
            archive_line_count += block_line_count
            result = function(
                block, len(block), molecule, masses_array, q_reference_array,
                q_temperature_array, q_minimum_cell_array, capacity,
                temperatures, len(temperatures_k),
                pressure_pa, self_fractions, len(self_mole_fractions), cutoffs,
                len(wing_cutoffs_cm1), grid_cm1[0], grid_cm1[1] - grid_cm1[0],
                len(grid_cm1), radiation, len(radiation_temperatures_k),
                visible_wavenumber_interval_cm1[0],
                visible_wavenumber_interval_cm1[1], spectra, counts, tails,
                center_numerators, visible_bounds, visible_cell_bounds,
                error, len(error))
            if result:
                raise ValueError(error.value.decode("utf-8") or
                                 f"native opacity accumulator failed ({result})")
    center_means = list(center_numerators)
    for temperature_index in range(len(temperatures_k)):
        for radiation_index, radiation_temperature in enumerate(
                radiation_temperatures_k):
            index = temperature_index * len(radiation_temperatures_k) + radiation_index
            denominator = ((math.pi ** 4 / 15.0) *
                           (radiation_temperature / C2_CM_K) ** 4)
            center_means[index] /= denominator
    return (
        _nested(list(spectra), [len(wing_cutoffs_cm1), len(self_mole_fractions),
                                len(temperatures_k), len(grid_cm1)]),
        _nested(list(counts), [len(wing_cutoffs_cm1), len(self_mole_fractions),
                               len(temperatures_k)]),
        _nested(list(tails), [len(wing_cutoffs_cm1), len(self_mole_fractions),
                              len(temperatures_k)]),
        _nested(center_means, [len(temperatures_k), len(radiation_temperatures_k)]),
        _nested(list(visible_bounds), [len(self_mole_fractions), len(temperatures_k)]),
        _nested(list(visible_cell_bounds),
                [max(1, len(self_mole_fractions) - 1),
                 max(1, len(temperatures_k) - 1)]),
        archive_line_count,
    )
