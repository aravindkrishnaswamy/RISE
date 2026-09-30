# DL334: conditioned critical-angle partition

Fresh review exposed a valid exact binary64 interface missed by the earlier
angle grid: nt=1.702, ni=1.7028514257128562, mu=0.031618823507523014 at measured
389 nm. The public SSS SPF reflected 1 while the profile transmitted
1.8850984772056378e-6. The same phenomenon occurred at 659 nm and with raw and
green slot views. This is an arithmetic inconsistency, not a wavelength query
or normal mismatch. The earlier gate is historical evidence, not acceptance of
this newly discovered input.

Pointwise transmission now carries both absolute indices into the same scalar
Fresnel law used by reflection. Forming the relative ratio first discards a
quotient residual: mathematically scale invariance does not imply that independently
rounded representations of the pair are interchangeable at critical incidence.
The relative ratio still parameterizes hemisphere normalization, whose integrated
law is well conditioned and whose existing approximation/tolerance is unchanged.
Burley/skin RGB and NM boundaries, RW entry adapters/continuations, shared vertex
evaluation, and PT/BDPT RW coins all retain the absolute pair for pointwise Ft.

For denser-to-rarer Snell refraction define q=nt/ni. The transmitted cosine obeys
cosT^2=(mu^2+q^2-1)/q^2. Near critical, the small discriminant must not be formed
from an already rounded sinT. The implementation retains the quotient residual
fma(-q,ni,nt)/ni, both square residuals, and error-free TwoSum residuals when
adding q^2, mu^2 and -1. All terms are scaled to at most one, avoiding squaring
large absolute indices. It rejects a negative computed discriminant and takes
the square root of a nonnegative one. No expanded TIR epsilon, transmission
clamp, changed acceptance tolerance, or energy correction is added.

The vector Fresnel API derives cosT from the same incident cosine and absolute
pair. Recovering cosT by dotting a nearly tangent transmitted vector introduces
another cancellation path; the transmitted vector remains part of the existing
API and its degeneracy/norm diagnostics are retained, but it is not a separate
Fresnel-angle authority. Every production vector-law caller was audited: dielectric,
perfect refractor and SSS Pel/NM entry/exit/geometric gates, BioSpec layer refraction,
random-walk internal reflection, and ray-caster straight shadow transmission all
supply the ordered pair used by their Snell call. SSS rough SPF and BSDF already
use the scalar absolute-pair law; GGX microfacet incidence is different from the
macro diffusion boundary and is not asserted to partition at one shared cosine.
Scalar manifold/fibre callers inherit the conditioned cosine kernel. HWSS delegates
these material/NM primitives; containment behavior is unchanged by this repair.

Error-free residuals require ordered binary operations. Clang locally disables
reassociation and implicit contraction in CalculateRefractedCosine; explicit fma
remains intentional. GCC has a function-local no-fast-math attribute and MSVC a
scoped precise pragma. The macOS shipping O3/LTO/fast-math LLVM IR retains all 36
residual binary/FMA operations without reassoc/contract flags and three explicit
FMAs. Unary fabs/fneg/sqrt retain the normal shipping flags. Actual shipping Opto
object execution is verified separately; GCC/MSVC execution is not claimed.

SSSCriticalPartitionTest embeds independent Decimal90 Fresnel/Snell reference
values for exact binary64 inputs: the two reviewer critical cases and ULP neighbors,
near-unity ratios, matched/grazing/TIR/immersed cases, constant indices, three
coordinate axes, and public diffusion/RW/skin raw and green views. It checks the
physical nonnegative boundary transmission, the fixed partition limit 1e-6,
independent reflection/transmission, and scalar/vector critical-side decisions.
Constant-index Pel controls cover the repaired RGB path. Adapters have open
hemisphere support, so their exact tangent value is tested separately through
profile/Optics laws rather than assigned a nonzero tangent BSDF value. The prior
two regression sources stay unchanged, retaining their exact original RED proof.

This is compensated binary64 arithmetic for representable input indices and
cosines, not arbitrary precision or a promise to recover an unrounded geometric
angle. The Decimal90 fixtures independently test the rounded physical tuple.
The earlier topology, snapshot and bounded furnace limitations still apply.
No transport eta-square, density, strategy or MIS arithmetic changes.
