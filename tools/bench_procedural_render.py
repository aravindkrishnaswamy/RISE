#!/usr/bin/env python3
"""Reproducible end-to-end render baseline for design doc §8.

Compares identical 320x240 PT (128 SPP, max_diffuse/glossy_bounce=4) and
BDPT (64 SPP, max_eye/light_depth=6) renders with `render_thread_reserve_count 0`
and `--no-denoise` (3 sequential repeats each, reporting median) across:
  1. Image-texture GGX baseline (`textures/wood.png`)
  2. Verbatim `procedural_expr.py`-compiled 8-node median Phase 1 material
  3. Verbatim `procedural_expr.py`-compiled 36-node p75 Phase 1 material

Run from repo root after building `./bin/rise`:
    python3 tools/bench_procedural_render.py
"""

from __future__ import annotations

import os
import statistics
import subprocess
import sys
import tempfile
import time

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(_REPO_ROOT, "tools"))

import measure_procedural_lowering as mpl  # noqa: E402


def _format_expr_painter_chunk(compiled_expr: str) -> str:
    """Format a procedural_expr.py expression into a RISE v7 expression_painter chunk."""
    lines = [
        "expression_painter",
        "{",
        "\tname\t\t\tpnt_albedo",
    ]
    raw_lines = [ln for ln in compiled_expr.splitlines() if ln.strip()]
    for ln in raw_lines[:-1]:
        if ln.startswith("def "):
            lines.append("\tdef\t\t\t\t" + ln[4:])
    lines.append("\texpr\t\t\t" + raw_lines[-1])
    lines.append("}")
    return "\n".join(lines)


_SCENE_TEMPLATE = """RISE ASCII SCENE 7

standard_shader
{{
\tname\t\t\tglobal
\tshaderop\t\tDefaultPathTracing
}}

{rasterizer}

file_rasterizeroutput
{{
\tpattern\t\t\t{out_pattern}
\ttype\t\t\tPNG
\tbpp\t\t\t\t8
\tcolor_space\t\tsRGB
}}

film
{{
\twidth\t\t\t320
\theight\t\t\t240
}}

pinhole_camera
{{
\tlocation\t\t0 0.8 4.2
\tlookat\t\t\t0 0 0
\tup\t\t\t\t0 1 0
\tfov\t\t\t\t38.0
}}

uniformcolor_painter
{{
\tname\t\t\tpnt_white
\tcolor\t\t\t1.0 1.0 1.0
}}

uniformcolor_painter
{{
\tname\t\t\tpnt_spec
\tcolor\t\t\t0.04 0.04 0.04
}}

uniformcolor_painter
{{
\tname\t\t\tpnt_floor
\tcolor\t\t\t0.5 0.5 0.5
}}

{painter}

ggx_material
{{
\tname\t\t\tmat_hero
\trd\t\t\t\tpnt_albedo
\trs\t\t\t\tpnt_spec
\talphax\t\t\t0.25
\talphay\t\t\t0.25
\tfresnel_mode\tschlick_f0
}}

lambertian_material
{{
\tname\t\t\tmat_floor
\treflectance\t\tpnt_floor
}}

lambertian_luminaire_material
{{
\tname\t\t\tmat_light
\texitance\t\tpnt_white
\tscale\t\t\t12.0
\tmaterial\t\tnone
}}

sphere_geometry
{{
\tname\t\t\tgeom_sphere
\tradius\t\t\t1.15
}}

clippedplane_geometry
{{
\tname\t\t\tgeom_floor
\tpta\t\t\t\t-5 -1.15 -5
\tptb\t\t\t\t-5 -1.15 5
\tptc\t\t\t\t5 -1.15 5
\tptd\t\t\t\t5 -1.15 -5
}}

clippedplane_geometry
{{
\tname\t\t\tgeom_light
\tpta\t\t\t\t-1.5 3.2 -1.5
\tptb\t\t\t\t1.5 3.2 -1.5
\tptc\t\t\t\t1.5 3.2 1.5
\tptd\t\t\t\t-1.5 3.2 1.5
}}

standard_object
{{
\tname\t\t\tobj_sphere
\tgeometry\t\tgeom_sphere
\tmaterial\t\tmat_hero
}}

standard_object
{{
\tname\t\t\tobj_floor
\tgeometry\t\tgeom_floor
\tmaterial\t\tmat_floor
}}

standard_object
{{
\tname\t\t\tobj_light
\tgeometry\t\tgeom_light
\tmaterial\t\tmat_light
}}
"""


_PAINTER_IMAGE = """png_painter
{
\tname\t\t\tpnt_albedo
\tfile\t\t\ttextures/wood.png
}"""

_RASTERIZERS = {
    "PT": """pathtracing_pel_rasterizer
{
\tsamples\t\t\t\t128
\tmax_diffuse_bounce\t\t4
\tmax_glossy_bounce\t\t4
}""",
    "BDPT": """bdpt_pel_rasterizer
{
\tsamples\t\t\t\t64
\tmax_eye_depth\t\t\t6
\tmax_light_depth\t\t\t6
}""",
}


def main() -> int:
    repeats = 3
    rise_bin = os.path.join(_REPO_ROOT, "bin", "rise")
    (n8, c8), (n36, c36) = mpl.get_compiled_benchmark_graphs()
    painters = [
        ("img_ggx", _PAINTER_IMAGE),
        (f"proc_{n8}node", _format_expr_painter_chunk(c8.expression)),
        (f"proc_{n36}node", _format_expr_painter_chunk(c36.expression)),
    ]

    with tempfile.TemporaryDirectory(prefix="rise_proc_bench_") as td:
        opts_path = os.path.join(td, "options.txt")
        with open(opts_path, "w", encoding="utf-8") as f:
            f.write("render_thread_reserve_count 0\n")
        env = os.environ.copy()
        env["RISE_MEDIA_PATH"] = _REPO_ROOT + os.sep
        env["RISE_OPTIONS_FILE"] = opts_path

        results = {}
        for rname, rchunk in _RASTERIZERS.items():
            print(f"\n=== {rname} (320x240, reserve=0, --no-denoise, {repeats} sequential repeats) ===", flush=True)
            for pname, pchunk in painters:
                sc_path = os.path.join(td, f"{rname}_{pname}.RISEscene")
                out_pat = os.path.join(td, f"out_{rname}_{pname}")
                with open(sc_path, "w", encoding="utf-8") as f:
                    f.write(_SCENE_TEMPLATE.format(rasterizer=rchunk, painter=pchunk, out_pattern=out_pat))
                times = []
                for _ in range(repeats):
                    t0 = time.perf_counter()
                    proc = subprocess.run(
                        [rise_bin, "--no-denoise", sc_path],
                        input="render\nquit\n",
                        text=True,
                        capture_output=True,
                        env=env,
                        cwd=_REPO_ROOT,
                        timeout=120,
                        check=False,
                    )
                    dt = time.perf_counter() - t0
                    combined = proc.stdout + proc.stderr
                    if proc.returncode != 0 or "PARTIAL SCENE" in combined or "invalid parameter" in combined:
                        raise RuntimeError(f"{rname}/{pname} failed:\n{combined}")
                    times.append(dt)
                med = statistics.median(times)
                results[(rname, pname)] = (med, times)
                t_str = ", ".join(f"{t:.3f}s" for t in times)
                print(f"  {pname:<14s} median={med:.3f}s  runs=[{t_str}]", flush=True)

        print("\n--- Ratios vs image-textured GGX baseline (budget <= 3.0x) ---")
        p8 = f"proc_{n8}node"
        p36 = f"proc_{n36}node"
        for rname in ("PT", "BDPT"):
            base = results[(rname, "img_ggx")][0]
            m8 = results[(rname, p8)][0]
            m36 = results[(rname, p36)][0]
            print(
                f"{rname:<5s} img_ggx={base:.3f}s  {p8}={m8:.3f}s ({m8/base:.2f}x)  "
                f"{p36}={m36:.3f}s ({m36/base:.2f}x)"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
