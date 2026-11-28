#pragma once
#include "HyLIoSParticleGeometry.h"

namespace RISE::HyLIoS {
// Overlap is strict interior intersection. Touching has zero interior overlap.
// Numerical uncertainty MUST be propagated by transport, never interpreted as
// a failed placement or silently retried (that would condition the process).
enum class ParticleOverlapStatus { Overlap, Disjoint, Touching,
    Indeterminate, InvalidInput, UnsupportedRange };
struct ParticleOverlapResult {
    ParticleOverlapStatus status;
    unsigned evaluations;
};
// secondCentreRelativeToFirst is supplied directly in metres; do not subtract
// two microscopic particles' large scene-space positions inside this primitive.
// Axes are arbitrary finite nonzero vectors and mean their exact real direction.
// Bounded Perram-Wertheim contact search, with certified outward intervals.
// Generic unresolved contact returns Indeterminate; it is NOT an exact predicate
// for every binary64 input. Exact polynomial controls cover spheres and equal,
// coordinate-aligned spheroids, including contact and adjacent binary64 inputs.
// Common power-of-two scaling supports microscopic physical sizes. The supported
// exponent spread is limited explicitly: each nonzero scaled input component
// must be >= 2^-100 when the largest length/component lies in [0.5,1).
// IEEE binary64 round-to-nearest and gradual underflow are required. Compile the
// implementation with -fno-fast-math -ffp-contract=off (MSVC /fp:strict).
ParticleOverlapResult ClassifyProlateOverlap(const ProlateParticle& first,
    const ProlateParticle& second, ParticleVector secondCentreRelativeToFirst);
}
