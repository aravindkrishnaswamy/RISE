# DL-474 corner design

Written before implementation, against master da81a949e. The previous slice
prices one certified box face exactly and refuses corners. This slice will
support two or three orthogonal faces of the SAME containing box. Other
objects/multiple independent cuts remain refused. Query radius stays fixed.

Normalize clearance by radius: each a_i is in (0,1). By octant symmetry,
the retained unit-ball volume decomposes into a positive orthant, single
negative-coordinate slabs, pair strips and (three planes) a negative brick.
For three planes the base fraction is 1/8; a slab contributes
3/16*(a-a^3/3). A pair strip contributes 3/(4*pi) times
integral_0^a integral_0^b sqrt(max(0,1-x^2-y^2)) dy dx. The brick uses
min(c,sqrt(max(0,1-x^2-y^2))). For two planes the base is 1/4,
slabs use 3/8 and the pair strip uses 3/(2*pi).

Decision: a fixed rectangular partition gives a conservative UPPER retained
fraction, using each cell's lower-left height, since height decreases in
both coordinates. The regression compares this upper fraction with an
independent high-resolution midpoint integration; that numerical reference
is not a certified lower bound. No random draws or empirical band changes. Dividing by the upper fraction leaves a bounded residual deficit
rather than overcorrecting boundary mass. Kernel/MIS factors use the SAME
fraction. Exact corner normalization remains an explicit strict residual,
not a closure claim. The origin corner tends to 1/4 or 1/8 exactly.

Validation planned: before/after the existing missing-mass corner pin;
independent geometric integration and limiting orthant values; VCMVolumeMergeTest corner witness versus PT with salted 3SE, related
VCMRecurrence and post-pass tests; four boundary-free pixel hashes identical.
Cost is fixed integration work only at certified own-box corners: the source
uses 256 cells for two planes and 1024 for three. No timing or speedup claim. Unknown/transformed-anisotropic and
multi-object cuts continue on the old kernel.

## Unit evidence and self-review

Current-master missing-mass corner pin16/1 rc1 becomes21/0 rc0. The independent
1024x1024 midpoint volume reference is .310241; the upper fraction is .310329
at clearance/radius=.25 on all three axes. The exact-normalization pin stays
20/1 rc1 at tolerance1e-6. Orthant limits are 1/4 for two faces and 1/8 for
three; the reciprocal kernel/MIS product is preserved. Focused ASan/UBSan on
the changed integrator TU and unit:20/0, rc0 (other linked objects are not
instrumented). All seven related named builds returned0 without warnings.

Self-review checked adjacent versus opposite faces, the one-cut census,
positive finite clearances/fractions, external cuts retaining their original
handling, and matching RGB/NM/HWSS kernel and MIS factors. There is no radius,
thinning, surface recurrence or random-draw change. The fixed rectangular
upper sum proves a mass bound, not exact corner normalization or a global
radiance-bias bound for arbitrary spatially varying illumination. Multiple
objects, enclosing sphere media and uncertified geometry remain residuals.

## Related gates

Actual runs: VCMRecurrence103/0, VCMLightPostPass72/0, VCMEyePostPass38/0, VCMSpectralRecurrence38/0; each rc0. Black-sphere K1/64-footprint gate, n64 ValueSalts: VCM .0370659 SD.00184256 versus PT .0368868 SD.0000760915, ratio1.00486 (per-pair ratio SD.0499029). Paired difference .000179157 SD.00184063 is within 3SE.000690235;2/0 rc0. The four global-fog boundary-free RGBA hashes match the saved baseline exactly (n4 mean.355471623683 SD.00150520762611). The separate corner witness passes2/0, rc0: n32 ValueSalts, VCM.109068 SD.0027999 versus PT.109109 SD.000227366, ratio.999621 (paired ratio SD.0258089), paired difference-.0000413368 SD.00281468 within3SE.00149271. This band does not replace the retained strict exact-volume pin.
