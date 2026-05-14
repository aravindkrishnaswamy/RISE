#!/usr/bin/env python3
"""
migrate_scenes_iscalarpainter.py — convert pre-IScalarPainter scenes to
the post-refactor scene language.

Scope: replace named-`uniformcolor_painter` / `spectral_painter`
references in material scalar slots (tau / ior / scattering / roughness /
etc.) with either inline numeric values (when the named painter has
uniform RGB channels) or `r g b` triples (per-channel).  Cross-file
references and complex chains are reported as warnings for manual
review.

Implementation note: the v1 of this script used multi-line regex
substitution which silently collapsed indentation and the trailing
newline before each material chunk's closing brace.  v2 (this file)
parses scenes line-by-line so substitutions preserve formatting
exactly.  Each line is either:
  - a chunk header `kind { ` or a closing `}`,
  - a parameter `name value` inside a chunk body,
  - blank/comment.

That structure lets us rewrite single parameter lines in place
without affecting surrounding whitespace.

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
    'cooktorrance_material': ('masking', 'ior', 'extinction'),
    'ggx_material': ('alphaX', 'alphaY', 'ior', 'extinction'),
    'schlick_material': ('roughness', 'isotropy'),
    'isotropicphong_material': ('exponent',),
    'ashikminshirley_material': ('nu', 'nv'),
    'wardisotropic_material': ('alpha',),
    'wardanisotropic_material': ('alphax', 'alphay'),
    'orennayar_material': ('roughness',),
    'sheen_material': ('roughness',),
    'translucent_material': ('extinction', 'N', 'scattering'),
    'phongluminaire_material': ('exponent',),
    'biospecskin_material': (
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
    'donnerjensenskin_bssrdf_material': (
        'melanin_fraction', 'melanin_blend', 'hemoglobin_epidermis',
        'carotene_fraction', 'hemoglobin_dermis', 'epidermis_thickness',
        'ior_epidermis', 'ior_dermis', 'blood_oxygenation',
    ),
    'generichumantissue_material': ('sca', 'g'),
}


def collect_uniformcolor_painters(lines):
    """Scan `lines` for uniformcolor_painter chunks.  Returns dict[name]
    -> (r, g, b)."""
    out = {}
    in_chunk = False
    name = None
    r = g = b = None
    for raw in lines:
        stripped = raw.strip()
        if not in_chunk and stripped == 'uniformcolor_painter':
            in_chunk = 'pending_brace'
            name = None
            r = g = b = None
            continue
        if in_chunk == 'pending_brace' and stripped == '{':
            in_chunk = True
            continue
        if in_chunk is True:
            if stripped == '}':
                if name is not None and r is not None:
                    out[name] = (r, g, b)
                in_chunk = False
                continue
            m = re.match(r'name\s+(\S+)', stripped)
            if m:
                name = m.group(1)
                continue
            m = re.match(r'color\s+(\S+)\s+(\S+)\s+(\S+)', stripped)
            if m:
                try:
                    r, g, b = (float(m.group(i)) for i in (1, 2, 3))
                except ValueError:
                    pass
                continue
    return out


def migrate_lines(lines, path, warnings):
    """Walk `lines` and rewrite material-chunk scalar slot bindings
    that name a known IPainter.  Returns (new_lines, changed: bool)."""
    uniform = collect_uniformcolor_painters(lines)
    out = []
    current_material = None        # None or kind string
    in_brace = False
    changed = False

    for line in lines:
        stripped = line.strip()
        # Detect material chunk header.
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
                # Material kind line was followed by something other than `{`
                # — abandon parsing this chunk.
                current_material = None
                in_brace = False
                out.append(line)
                continue
            # We're inside a material chunk body.
            if stripped == '}':
                current_material = None
                in_brace = False
                out.append(line)
                continue
            params = MATERIAL_SCALAR_PARAMS[current_material]
            # Strip indentation; remember to re-apply.
            m = re.match(r'^(\s*)(\S+)\s+(.*?)\s*$', line)
            if m:
                indent, param, value = m.group(1), m.group(2), m.group(3)
                if param in params:
                    # Inline numeric? skip.
                    if value and (value[0].isdigit() or value[0] in '+-.'):
                        out.append(line)
                        continue
                    # Look up named-IPainter.
                    if value in uniform:
                        r, g, b = uniform[value]
                        if r == g == b:
                            new_val = f'{r:g}'
                        else:
                            new_val = f'{r:g} {g:g} {b:g}'
                        out.append(f'{indent}{param}\t\t\t{new_val}\n')
                        changed = True
                        continue
                    # Spectral painter or other named ref: warn and leave.
                    warnings.append(
                        f'  {path}:{stripped} '
                        f'(material `{current_material}` param `{param}` refs '
                        f'`{value}` — manual review needed)'
                    )
            out.append(line)
            continue
        out.append(line)
    return out, changed


def migrate_file(path, dry_run=False):
    """Return (changed: bool, warnings: list[str])."""
    txt = path.read_text(errors='replace')
    lines = txt.splitlines(keepends=True)
    warnings = []
    new_lines, changed = migrate_lines(lines, path, warnings)
    if changed and not dry_run:
        path.write_text(''.join(new_lines))
    return changed, warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='scenes')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    changed_count = 0
    warning_count = 0
    for path in sorted(root.rglob('*.RISEscene')):
        try:
            changed, warnings = migrate_file(path, dry_run=args.dry_run)
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
