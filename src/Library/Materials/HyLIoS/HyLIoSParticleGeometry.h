#pragma once

#include <optional>

namespace RISE::HyLIoS {

// Geometry prerequisite for HyLIoS, not a particle-placement/transport model.
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
// positive full chord lengths/areas throw overflow_error or underflow_error. Signed
// distances, positions and
// normal components may round to zero. No RNG, retry loop, or hidden state.
double ProlateProjectedArea(const ProlateParticle& particle, ParticleVector direction);

// nullopt means the infinite line misses, or its exit lies strictly behind the
// ray origin. Tangency is a valid zero-length chord, including at distance zero.
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
