# DL-423 redo evidence (2026-10-13)

The user-supplied reviewer files are copied unchanged with `reviewer-` prefixes.
`reviewer-Probe.cpp` retains its original absolute worktree include; use the
current CompositeEnergyConservationTest fixture to reproduce the restored probe.
The current `restored-*.txt` reports are fresh runs against the restored library.
Accuracy replicates: eight RNG-seeded batches, 20,000 draws each. AOV: eight
batches of 40,000 draws. Maximum kray: 200,000 draws per reported cell.

`operator-mode-zero-probe.cpp` is an OFFLINE albedo-only kernel extractor,
not a replacement engine model. Compile it with the library's make CPPFLAGS,
CXXFLAGS, CXXARCHFLAGS, CXXLIBSETTINGS, DEFS and LDLIBS, linking bin/librise.a,
to `/tmp/dl423-kernel-probe`. `operator-mode-zero.py` runs the extraction and
numpy solve, writing `/tmp/dl423-offline/`. It uses the substrate's actual
GGX BRDF and projected solid-angle weights. The n=512 refinement uses the
same program with cells=[(.02,.9,1.5)] and n=[128,256,512]. Phi remains 2048;
no azimuthal error bound or angular BRDF/sampler accuracy claim is made.
JSON `seconds` fields are uncontrolled observations with concurrent other
work; they are not engine performance benchmarks.

`tools/CoatedParameterContinuityProbe.cpp` is a diagnostic, outside the default
test suite. Build: `make -C build/make/rise build-tool/CoatedParameterContinuityProbe`.
Its nonzero status reports the user-requested restored-parent failures. It
does not weaken its criterion to turn this parent green. Integrated albedo
uses 256 fixed importance-proposal draws, reused across parameter changes;
it is a continuity estimate, not an albedo accuracy/conservation proof.

Look experiments isolate the rejected a8ffe5a4e CoatedBRDF object ahead of the
restored library; all other engine objects are identical (including DL-382).
Four paired Sobol salts: 2654435769, 1013904242, 3668340011, 2027808484.
The supplied reviewer scenes retain their lights, geometry and samples:
lacquer 640x360/96, tidal 800x600/32. Only output patterns and Sobol salts change.
The reviewer base_1/base_2 input scenes differed only in output pattern, so
those old pairs are not counted as independent salted trials.

`look-probe.cpp` preserves image dimensions from Film and uses
RasterConvention::PixelCentreToScreen for primary-hit masks. The first captured
binary masks used bottom-up screen coordinates; analysis flips only their
mask rows to the exact PixelCentreToScreen convention. Luminance rows are
unchanged. Corrected masks align with the rendered objects in both PNGs.
Look region masks: mat_worn_lacquer_brass, mat_stone. Block maps use 8x8
blocks with >=90% mask coverage. The CSVs also retain all background blocks.
Region SD and paired-relative SD use ddof=1 and n=4. Spatial extrema at n=4
are exploratory and may include noise; they are not variance-reduction claims.
