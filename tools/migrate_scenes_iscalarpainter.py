#!/usr/bin/env python3
"""
migrate_scenes_iscalarpainter.py — convert pre-IScalarPainter scenes to
the post-refactor scene language.

v3 (2026-05): cross-file aware.  RISE scene files frequently
`> run scenes/colors.RISEscript` to pull in shared `uniformcolor_painter`
chunks (`color_white`, `color_silver`, etc.) and `spectral_painter`
chunks.  v1/v2 of this tool only inspected the local file; v3 pre-
scans every `.RISEscene` and `.RISEscript` under the root and builds
a global painter table.

Each scalar slot (per `MATERIAL_SCALAR_PARAMS`) bound to a name:
  - is inline numeric / triple → no change.
  - is uniform RGB → inline single literal (`tau 1.0`).
  - is per-channel RGB → inline triple (`tau 0.90 0.91 0.98`).
  - is a spectral_painter `file ...` → emit a `scalar_painter file ...`
    chunk at the top of THIS file (named identically with `_scalar`
    suffix) and rewrite the slot to reference it.
  - is unknown → warn, leave alone.

Implementation is strictly line-by-line so indentation and brace
placement are preserved.

Usage:
    python3 tools/migrate_scenes_iscalarpainter.py [--dry-run] [--root scenes]
"""

import argparse
import pathlib
import re
import sys

MATERIAL_SCALAR_PARAMS = {
    'dielectric_material': ('tau', 'ior', 'scattering'),
    'polished_material': ('tau', 'ior', 'scattering'),
    'perfectrefractor_material': ('ior',),
    'subsurfacescattering_material': ('ior', 'absorption', 'scattering'),
    'randomwalk_sss_material': ('ior', 'absorption', 'scattering'),
    'cooktorrance_material': ('facets', 'ior', 'extinction'),
    'ggx_material': ('alphax', 'alphay', 'ior', 'extinction'),
    'schlick_material': ('roughness', 'isotropy'),
    'isotropic_phong_material': ('N',),
    'ashikminshirley_anisotropicphong_material': ('nu', 'nv'),
    'ward_isotropic_material': ('alpha',),
    'ward_anisotropic_material': ('alphax', 'alphay'),
    'orennayar_material': ('roughness',),
    'sheen_material': ('sheen_roughness',),
    'translucent_material': ('ext', 'N', 'scattering'),
    'phong_luminaire_material': ('N',),
    'biospec_skin_material': (
        'thickness_SC', 'thickness_epidermis', 'thickness_papillary_dermis',
        'thickness_reticular_dermis', 'ior_SC', 'ior_epidermis',
        'ior_papillary_dermis', 'ior_reticular_dermis',
        'concentration_eumelanin', 'concentration_pheomelanin',
        'melanosomes_in_epidermis', 'hb_ratio',
        'whole_blood_in_papillary_dermis', 'whole_blood_in_reticular_dermis',
        'bilirubin_concentration', 'betacarotene_concentration_SC',
        'betacarotene_concentration_epidermis',
        'betacarotene_concentration_dermis', 'folds_aspect_ratio',
    ),
    'donner_jensen_skin_bssrdf_material': (
        'melanin_fraction', 'melanin_blend', 'hemoglobin_epidermis',
        'carotene_fraction', 'hemoglobin_dermis', 'epidermis_thickness',
        'ior_epidermis', 'ior_dermis', 'blood_oxygenation',
    ),
    'generic_human_tissue_material': ('sca', 'g'),
}


def parse_chunks_global(roots):
    """Pre-scan every `.RISEscene` / `.RISEscript` and build a global
    dict.  Returns (uniformcolor, spectral_file) where:
      uniformcolor[name] = (r, g, b)
      spectral_file[name] = (file_path, scale)
    """
    uniform = {}
    spectral = {}
    for root in roots:
        for path in pathlib.Path(root).rglob('*.RISEscene'):
            _scan_file(path, uniform, spectral)
        for path in pathlib.Path(root).rglob('*.RISEscript'):
            _scan_file(path, uniform, spectral)
    return uniform, spectral


def _scan_file(path, uniform, spectral):
    try:
        lines = path.read_text(errors='replace').splitlines()
    except Exception:
        return
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s in ('uniformcolor_painter', 'spectral_painter'):
            kind = s
            # Find opening brace and consume body up to closing brace.
            j = i + 1
            while j < len(lines) and lines[j].strip() != '{':
                j += 1
            if j >= len(lines):
                i = j
                continue
            j += 1
            body = []
            while j < len(lines) and lines[j].strip() != '}':
                body.append(lines[j])
                j += 1
            name = None
            color_rgb = None
            file_path = None
            scale = 1.0
            for bl in body:
                bs = bl.strip()
                m = re.match(r'name\s+(\S+)', bs)
                if m:
                    name = m.group(1)
                    continue
                m = re.match(r'color\s+(\S+)\s+(\S+)\s+(\S+)', bs)
                if m:
                    try:
                        color_rgb = tuple(float(m.group(k)) for k in (1, 2, 3))
                    except ValueError:
                        pass
                    continue
                m = re.match(r'file\s+(\S+)', bs)
                if m:
                    file_path = m.group(1)
                    continue
                m = re.match(r'scale\s+(\S+)', bs)
                if m:
                    try:
                        scale = float(m.group(1))
                    except ValueError:
                        pass
                    continue
            if name:
                if kind == 'uniformcolor_painter' and color_rgb:
                    uniform[name] = color_rgb
                elif kind == 'spectral_painter' and file_path:
                    spectral[name] = (file_path, scale)
            i = j + 1
        else:
            i += 1


def migrate_file(path, uniform, spectral, warnings, dry_run=False):
    """Walk a single scene line-by-line.  Returns (changed: bool)."""
    txt = path.read_text(errors='replace')
    lines = txt.splitlines(keepends=True)

    out = []
    new_scalar_painters = []  # accumulated scalar_painter blocks to prepend
    minted = set()  # spectral painter names already minted as _scalar

    current_material = None
    in_brace = False
    changed = False

    for line in lines:
        stripped = line.strip()
        if current_material is None and stripped in MATERIAL_SCALAR_PARAMS:
            current_material = stripped
            out.append(line)
            continue
        if current_material is not None:
            if not in_brace:
                if stripped == '{':
                    in_brace = True
                    out.append(line)
                    continue
                current_material = None
                in_brace = False
                out.append(line)
                continue
            if stripped == '}':
                current_material = None
                in_brace = False
                out.append(line)
                continue
            params = MATERIAL_SCALAR_PARAMS[current_material]
            m = re.match(r'^(\s*)(\S+)\s+(.*?)\s*$', line)
            if m:
                indent, param, value = m.group(1), m.group(2), m.group(3)
                if param in params and value:
                    if value[0].isdigit() or value[0] in '+-.':
                        out.append(line)
                        continue
                    if value in uniform:
                        r, g, b = uniform[value]
                        if r == g == b:
                            new_val = f'{r:g}'
                        else:
                            new_val = f'{r:g} {g:g} {b:g}'
                        out.append(f'{indent}{param}\t\t\t{new_val}\n')
                        changed = True
                        continue
                    if value in spectral:
                        sname = f'{value}_scalar'
                        if sname not in minted:
                            file_path, scale = spectral[value]
                            if scale == 1.0:
                                blk = (
                                    f'scalar_painter\n'
                                    f'{{\n'
                                    f'\tname\t\t\t\t{sname}\n'
                                    f'\tfile\t\t\t\t{file_path}\n'
                                    f'}}\n\n'
                                )
                            else:
                                blk = (
                                    f'scalar_painter\n'
                                    f'{{\n'
                                    f'\tname\t\t\t\t{sname}_base\n'
                                    f'\tfile\t\t\t\t{file_path}\n'
                                    f'}}\n'
                                    f'scalar_painter\n'
                                    f'{{\n'
                                    f'\tname\t\t\t\t{sname}\n'
                                    f'\tbase\t\t\t\t{sname}_base\n'
                                    f'\tscale\t\t\t\t{scale:g}\n'
                                    f'}}\n\n'
                                )
                            new_scalar_painters.append(blk)
                            minted.add(sname)
                        out.append(f'{indent}{param}\t\t\t{sname}\n')
                        changed = True
                        continue
                    warnings.append(
                        f'  {path}: {current_material} param `{param}` value `{value}` — unknown name (cross-file or undefined?)'
                    )
            out.append(line)
            continue
        out.append(line)

    # Prepend minted scalar_painter blocks above the first material
    # chunk (so they're parsed before any material references them).
    if new_scalar_painters:
        joined = ''.join(out)
        mat_pos = None
        for mat_kind in MATERIAL_SCALAR_PARAMS:
            m = re.search(r'(?m)^\s*' + re.escape(mat_kind) + r'\b', joined)
            if m and (mat_pos is None or m.start() < mat_pos):
                mat_pos = m.start()
        if mat_pos is not None:
            joined = joined[:mat_pos] + ''.join(new_scalar_painters) + joined[mat_pos:]
            out = [joined]

    if changed and not dry_run:
        path.write_text(''.join(out))
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='scenes')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    print('Pre-scanning all .RISEscene / .RISEscript for global painter table...',
          file=sys.stderr)
    uniform, spectral = parse_chunks_global([args.root])
    print(f'  {len(uniform)} uniformcolor_painters, {len(spectral)} spectral_painters',
          file=sys.stderr)

    root = pathlib.Path(args.root)
    changed_count = 0
    warning_count = 0
    warnings = []
    for path in sorted(root.rglob('*.RISEscene')):
        try:
            changed = migrate_file(path, uniform, spectral, warnings,
                                   dry_run=args.dry_run)
        except Exception as e:
            print(f'  ERROR {path}: {e}', file=sys.stderr)
            continue
        if changed:
            changed_count += 1
            print(f'  {"would migrate" if args.dry_run else "migrated"}: {path}')
    for w in warnings:
        warning_count += 1
        print(w, file=sys.stderr)
    print(f'\n{changed_count} scenes {"would be " if args.dry_run else ""}migrated; {warning_count} warnings.', file=sys.stderr)


if __name__ == '__main__':
    main()
