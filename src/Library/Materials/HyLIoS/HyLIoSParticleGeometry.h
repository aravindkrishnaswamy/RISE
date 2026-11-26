#pragma once

#include <optional>

namespace RISE::HyLIoS {

// Geometry prerequisite for HyLIoS, not a particle-placement/transport model.
// The implementation TU requires strict FP (no fast-math/reassociation); its
// compensated products and sums rely on IEEE binary64 round-to-nearest steps.
// Calls require FE_TONEAREST and gradual underflow; other environments throw
// runtime_error before geometry arithmetic.
// All positions are relative to the particle centre, in metres. Keep scene
// translation outside these primitives so microscopic chords retain precision.
struct ParticleVector { double x, y, z; };
struct ProlateParticle {
	double semiMinorMetres;
	double semiMajorMetres;
	ParticleVector axis;
};
struct ParticleChord {
	// Physical distances along the NORMALIZED input direction. A ray starting
	// inside retains a negative entry; the next boundary is exitDistanceMetres.
	double entryDistanceMetres;
	double exitDistanceMetres;
	// Full line chord, including the part behind an inside ray's origin. This
	// is evaluated independently of subtracting two possibly distant roots.
	double chordLengthMetres;
	ParticleVector entryPointMetres;
	ParticleVector exitPointMetres;
	ParticleVector entryOutwardNormal;
	ParticleVector exitOutwardNormal;
};
struct ProjectedParticleHit {
	// In the plane through the particle centre perpendicular to the direction.
	ParticleVector projectedPointMetres;
	ParticleChord chord;
};

// Finite positive axes with major >= minor, and finite nonzero axis/direction
// vectors are required. Vectors are normalized internally without squaring their
// unscaled components. Invalid inputs throw invalid_argument; unrepresentable
// full chord lengths/areas and nonzero signed distances that cannot be represented
// throw overflow_error or underflow_error. Exact surface/tangent zero distances
// are retained; position and normal components may round to zero. A nonzero
// projected displacement or normalized disk component that rounds to zero
// raises an explicit range exception, never a physical miss or altered normal.
// This is binary64 geometry; it does not promise arbitrary exponent ranges.
// No RNG, retry loop, or hidden state.
double ProlateProjectedArea(const ProlateParticle& particle, ParticleVector direction);

// nullopt means the infinite line misses, or its exit lies strictly behind the
// ray origin. Tangency is a valid zero-length chord, including at distance zero.
// On/inside origins use an implicit quadric, with known surface roots deflated.
// Its raw-direction parameter roots must be representable. Finite outward
// coefficient/root/gradient enclosures are also required: range_error is raised
// when an enclosure cannot be formed or its gradient uncertainty radius reaches
// the nominal gradient magnitude. This explicitly excludes poorly resolved
// grazing normals and extreme exponent combinations. Known surface zeros are
// deflated only when certified; the projected sampler has no such root enclosure.
// No ray-origin epsilon or self-intersection policy is imposed by this math.
std::optional<ParticleChord> IntersectProlateRay(const ProlateParticle& particle,
	ParticleVector originRelativeMetres, ParticleVector direction);

// Conditional on the supplied orientation: density 1/ProlateProjectedArea with
// respect to projected AREA, not surface area. Both uniforms are in [0,1).
// Radius squared is uniform, azimuth is 2*pi*u. The chord uses the known disk
// radius directly, avoiding cancellation/rejection after reconstructing a point
// near the silhouette. Returned positions have ordinary binary64 rounding.
ProjectedParticleHit SampleProlateProjectedHit(const ProlateParticle& particle,
	ParticleVector direction, double uniformRadiusSquared, double uniformAzimuth);

} // namespace RISE::HyLIoS
