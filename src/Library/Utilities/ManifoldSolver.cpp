//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.cpp - Specular Manifold Sampling solver
//
//    Implements the Newton iteration method from Zeltner et al. 2020
//    for finding valid specular paths connecting two non-specular
//    endpoints through a chain of specular surfaces.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ManifoldSolver.h"
#include "Optics.h"
#include "BDPTUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersection.h"
#include "../Lights/LightSampler.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Construction / Destruction
//////////////////////////////////////////////////////////////////////

ManifoldSolver::ManifoldSolver( const ManifoldSolverConfig& cfg ) :
config( cfg ),
pLightSampler( 0 )
{
	pLightSampler = new LightSampler();
}

ManifoldSolver::~ManifoldSolver()
{
	safe_release( pLightSampler );
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirection
//
//   Convention for the manifold solver:
//     wi = unit direction from vertex TOWARD the previous vertex
//          (pointing away from surface on the incoming side)
//     wo = unit direction from vertex TOWARD the next vertex
//          (pointing away from surface on the outgoing side)
//     normal = outward surface normal
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeSpecularDirection(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Vector3& wo
	) const
{
	if( isReflection )
	{
		// Reflection: wo = wi - 2*(wi.n)*n
		// Using Optics::CalculateReflectedRay expects vIn pointing toward
		// the surface.  Our wi points away from surface, so negate it.
		// reflected = vIn - 2*(vIn.n)*n
		// We want: wo = (-wi) - 2*((-wi).n)*n = -wi + 2*(wi.n)*n
		const Scalar d = Vector3Ops::Dot( wi, normal );
		wo = Vector3(
			-wi.x + 2.0 * d * normal.x,
			-wi.y + 2.0 * d * normal.y,
			-wi.z + 2.0 * d * normal.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
	else
	{
		// Refraction via Snell's law
		// wi points away from surface toward incoming side
		// We compute the refracted direction on the other side
		//
		// The standard formula uses eta_ratio = n_incoming / n_transmitted.
		// When entering glass (cos_i > 0, wi on normal side):
		//   n_incoming = 1 (air), n_transmitted = eta (glass)
		//   eta_ratio = 1/eta
		// When exiting glass (cos_i < 0, wi opposite to normal):
		//   n_incoming = eta (glass), n_transmitted = 1 (air)
		//   eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;   // Default: entering glass
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			// wi is on the opposite side of the normal — exiting glass
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;  // glass → air: n_glass / n_air
		}

		const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);

		if( sin2_t > 1.0 )
		{
			// Total internal reflection
			return false;
		}

		const Scalar cos_t = sqrt( 1.0 - sin2_t );

		// Refracted direction: wt = -eta_ratio * wi + (eta_ratio * ci - cos_t) * n
		// This gives a direction pointing away from the surface on the transmitted side
		wo = Vector3(
			-eta_ratio * wi.x + (eta_ratio * ci - cos_t) * n.x,
			-eta_ratio * wi.y + (eta_ratio * ci - cos_t) * n.y,
			-eta_ratio * wi.z + (eta_ratio * ci - cos_t) * n.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtNormal
//
//   Computes the 3x3 Jacobian d(wo)/d(n), stored row-major.
//
//   Currently unused: the half-vector Jacobian (BuildJacobian)
//   captures surface curvature via the Weingarten map (ds/du from
//   dndu/dndv) without needing this derivative.  It would be needed
//   for an analytical Jacobian of the angle-difference constraint,
//   which explicitly computes d(wo_specular)/d(n) in its chain rule.
//
//   NOTE: this derivative assumes unnormalized wo (the raw output
//   of ComputeSpecularDirection before normalization).  If used with
//   the normalized version, apply the DeriveNormalized correction.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dn[9]
	) const
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi.n)*n
		// d(wo)/d(n) = 2*(wi.n)*I + 2*outer(n, wi)
		// where outer(a,b)_ij = a_i * b_j
		const Scalar d = Vector3Ops::Dot( wi, normal );

		// Row 0
		dwo_dn[0] = 2.0 * d + 2.0 * normal.x * wi.x;		// d(wo.x)/d(n.x)
		dwo_dn[1] = 2.0 * normal.x * wi.y;				// d(wo.x)/d(n.y)
		dwo_dn[2] = 2.0 * normal.x * wi.z;				// d(wo.x)/d(n.z)
		// Row 1
		dwo_dn[3] = 2.0 * normal.y * wi.x;				// d(wo.y)/d(n.x)
		dwo_dn[4] = 2.0 * d + 2.0 * normal.y * wi.y;		// d(wo.y)/d(n.y)
		dwo_dn[5] = 2.0 * normal.y * wi.z;				// d(wo.y)/d(n.z)
		// Row 2
		dwo_dn[6] = 2.0 * normal.z * wi.x;				// d(wo.z)/d(n.x)
		dwo_dn[7] = 2.0 * normal.z * wi.y;				// d(wo.z)/d(n.y)
		dwo_dn[8] = 2.0 * d + 2.0 * normal.z * wi.z;		// d(wo.z)/d(n.z)
	}
	else
	{
		// Refraction: wo = -eta*wi + mu*n  where mu = eta*cos_i - cos_t
		// cos_i = dot(wi, n), cos_t = sqrt(1 - eta^2*(1-cos_i^2))
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );
		const Scalar sin2_t = eta * eta * (1.0 - cos_i * cos_i);

		Scalar cos_t = 0.0;
		if( sin2_t < 1.0 ) {
			cos_t = sqrt( 1.0 - sin2_t );
		}
		if( cos_t < NEARZERO ) {
			cos_t = NEARZERO;
		}

		const Scalar mu = eta * cos_i - cos_t;

		// d(mu)/d(n_j) = eta * wi_j * (1 - eta * cos_i / cos_t)
		// d(wo_i)/d(n_j) = d(mu)/d(n_j) * n_i + mu * delta_ij
		// = eta * wi_j * (1 - eta*cos_i/cos_t) * n_i + mu * delta_ij

		const Scalar factor = eta * (1.0 - eta * cos_i / cos_t);

		// Row 0
		dwo_dn[0] = factor * wi.x * normal.x + mu;
		dwo_dn[1] = factor * wi.y * normal.x;
		dwo_dn[2] = factor * wi.z * normal.x;
		// Row 1
		dwo_dn[3] = factor * wi.x * normal.y;
		dwo_dn[4] = factor * wi.y * normal.y + mu;
		dwo_dn[5] = factor * wi.z * normal.y;
		// Row 2
		dwo_dn[6] = factor * wi.x * normal.z;
		dwo_dn[7] = factor * wi.y * normal.z;
		dwo_dn[8] = factor * wi.z * normal.z + mu;
	}
}

//////////////////////////////////////////////////////////////////////
// DirectionToSpherical
//
//   Converts a direction to spherical coords (theta, phi) in the
//   local tangent frame defined by dpdu, dpdv, normal.
//////////////////////////////////////////////////////////////////////

Point2 ManifoldSolver::DirectionToSpherical(
	const Vector3& dir,
	const Vector3& dpdu,
	const Vector3& dpdv,
	const Vector3& normal
	) const
{
	const Vector3 u = Vector3Ops::Normalize( dpdu );
	const Vector3 v = Vector3Ops::Normalize( dpdv );

	const Scalar x = Vector3Ops::Dot( dir, u );
	const Scalar y = Vector3Ops::Dot( dir, v );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// Clamp z to [-1, 1] for numerical safety
	Scalar cz = z;
	if( cz > 1.0 ) cz = 1.0;
	if( cz < -1.0 ) cz = -1.0;

	const Scalar theta = acos( cz );
	const Scalar phi = atan2( y, x );

	return Point2( theta, phi );
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraint
//
//   Half-vector constraint (Zeltner et al. 2020).
//
//   For each specular vertex i, construct the generalized half-vector:
//     Refraction: h = -(wi + eta_eff * wo),  normalized
//     Reflection: h =   wi + wo,              normalized
//   where eta_eff accounts for entering vs exiting the medium.
//
//   When the specular constraint is exactly satisfied, h is parallel
//   to the surface normal.  The constraint measures the tangent-plane
//   projection of h, which must be zero:
//     C[2i]   = dot(dpdu, h)
//     C[2i+1] = dot(dpdv, h)
//
//   This avoids the angular wrapping issues of the spherical-coordinate
//   formulation and yields a well-conditioned Jacobian.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::EvaluateConstraint(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& C
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	C.resize( 2 * k, 0.0 );

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Previous and next positions
		Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// wi = direction from vertex toward previous vertex
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// wo = direction from vertex toward next vertex
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		// Construct the generalized half-vector
		Vector3 h;

		if( v.isReflection )
		{
			// Reflection: h = wi + wo
			h = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			// Refraction: determine effective eta based on which
			// side of the surface wi arrives from.
			Scalar eta_eff = v.eta;
			if( Vector3Ops::Dot( wi, v.normal ) < 0.0 )
			{
				// wi on the opposite side of the normal → exiting
				eta_eff = 1.0 / v.eta;
			}

			// h = -(wi + eta_eff * wo)
			h = Vector3(
				-(wi.x + eta_eff * wo.x),
				-(wi.y + eta_eff * wo.y),
				-(wi.z + eta_eff * wo.z)
			);
		}

		// Normalize h
		Scalar hLen = Vector3Ops::Magnitude( h );
		if( hLen < NEARZERO )
		{
			// Degenerate — set large constraint
			C[2*i]   = 1.0;
			C[2*i+1] = 1.0;
			continue;
		}
		h = h * (1.0 / hLen);

		// Tangent-plane projection: when Snell's law is satisfied,
		// h is parallel to the normal, so these projections are zero.
		// Use normalized tangent vectors for consistent scaling.
		Vector3 s = Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Normalize( v.dpdv );

		C[2*i]   = Vector3Ops::Dot( s, h );
		C[2*i+1] = Vector3Ops::Dot( t, h );
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraintAtVertex
//
//   Angle-difference constraint (Zeltner et al. 2020).
//
//   For a specular vertex, the constraint measures the angular
//   deviation between the actual outgoing direction wo and the
//   specularly scattered direction wo_spec:
//
//     C0 = theta(wo) - theta(wo_spec)
//     C1 = wrapToPi( phi(wo) - phi(wo_spec) )
//
//   where theta and phi are spherical coordinates in the local
//   tangent frame (s, t, normal).
//
//   This avoids the back-facing degeneracies of the half-vector
//   projection and provides larger convergence basins for Newton
//   iteration with distant initial guesses.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::EvaluateConstraintAtVertex(
	const Point3& vertexPos,
	const Vector3& vertexNormal,
	const Vector3& vertexDpdu,
	const Vector3& vertexDpdv,
	Scalar vertexEta,
	bool vertexIsReflection,
	const Point3& prevPos,
	const Point3& nextPos,
	Scalar& C0,
	Scalar& C1
	) const
{
	// Direction from vertex toward previous vertex (incoming)
	Vector3 wi = Vector3Ops::mkVector3( prevPos, vertexPos );
	const Scalar wiLen = Vector3Ops::NormalizeMag( wi );

	// Direction from vertex toward next vertex (actual outgoing)
	Vector3 wo_actual = Vector3Ops::mkVector3( nextPos, vertexPos );
	const Scalar woLen = Vector3Ops::NormalizeMag( wo_actual );

	if( wiLen < NEARZERO || woLen < NEARZERO )
	{
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Compute the specularly scattered direction
	Vector3 wo_specular;
	if( !ComputeSpecularDirection( wi, vertexNormal, vertexEta,
		vertexIsReflection, wo_specular ) )
	{
		// Total internal reflection for a refraction vertex — degenerate
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Convert both directions to spherical coordinates in the
	// local tangent frame
	const Point2 angles_actual = DirectionToSpherical(
		wo_actual, vertexDpdu, vertexDpdv, vertexNormal );
	const Point2 angles_specular = DirectionToSpherical(
		wo_specular, vertexDpdu, vertexDpdv, vertexNormal );

	// Theta difference (no periodicity issue)
	C0 = angles_actual.x - angles_specular.x;

	// Phi difference — must wrap to [-pi, pi] to handle periodicity
	// (Zeltner errata: without this, the solver does not realize that
	// a value near 2*pi is close to a zero and fails to converge)
	Scalar phi_diff = angles_actual.y - angles_specular.y;
	if( phi_diff > PI ) phi_diff -= 2.0 * PI;
	if( phi_diff < -PI ) phi_diff += 2.0 * PI;
	C1 = phi_diff;
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianNumerical
//
//   Builds the block-tridiagonal Jacobian dC/dx via central finite
//   differences on the constraint function.  This matches whichever
//   constraint formulation EvaluateConstraint uses (angle-difference
//   or half-vector).
//
//   For each vertex i and each surface parameter p (u or v):
//     - Diagonal block: perturb vertex i position and normal,
//       evaluate C_i at (pos ± dp*eps, normal ± dn*eps)
//     - Upper block (i+1): perturb nextPos, evaluate C_i
//     - Lower block (i-1): perturb prevPos, evaluate C_i
//
//   Uses central differences: dC/dp ≈ (C(+eps) - C(-eps)) / (2*eps)
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Numerically differentiates the SAME half-vector constraint
	// that BuildJacobian uses analytically.  For each vertex j and
	// each surface parameter p (u or v), perturb vertex j's position
	// and normal, evaluate the full chain constraint, and extract
	// the finite-difference derivative for all constraint components
	// that depend on vertex j.

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			// Build perturbed chains
			std::vector<ManifoldVertex> chainPlus( chain );
			std::vector<ManifoldVertex> chainMinus( chain );

			chainPlus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			chainPlus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );

			chainMinus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			chainMinus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			std::vector<Scalar> Cp, Cm;
			EvaluateConstraint( chainPlus, fixedStart, fixedEnd, Cp );
			EvaluateConstraint( chainMinus, fixedStart, fixedEnd, Cm );

			// Extract derivatives for constraint i w.r.t. vertex j
			// Diagonal: i == j
			diag[j*4 + 0 + p] = (Cp[2*j]   - Cm[2*j])   * inv2eps;
			diag[j*4 + 2 + p] = (Cp[2*j+1] - Cm[2*j+1]) * inv2eps;

			// Upper: vertex j affects constraint j-1 (if j > 0)
			// lower[(j-1)] maps vertex j to constraint j-1
			if( j > 0 )
			{
				unsigned int ci = j - 1;  // constraint index
				upper[ci*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				upper[ci*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}

			// Lower: vertex j affects constraint j+1 (if j < k-1)
			// lower[j] maps vertex j to constraint j+1
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;  // constraint index
				lower[j*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				lower[j*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobian
//
//   Analytical Jacobian for the half-vector constraint
//   (Zeltner et al. 2020, adapted from their reference code).
//
//   C_i = (dot(s_i, h_i),  dot(t_i, h_i))
//
//   The Jacobian is block-tridiagonal because C_i depends only on
//   vertices i-1, i, i+1.  Each block is 2x2.
//
//   For each vertex i, we compute dC_i / dX_j analytically by
//   differentiating h with respect to vertex positions and
//   accounting for the tangent-frame variation (ds, dt) due to
//   normal curvature (dndu, dndv).
//////////////////////////////////////////////////////////////////////

Vector3 ManifoldSolver::DeriveNormalized( const Vector3& h, const Vector3& dv, Scalar vLen )
{
	if( vLen < NEARZERO ) return Vector3( 0, 0, 0 );
	const Scalar invLen = 1.0 / vLen;
	// (dv - h * dot(h, dv)) / |v|
	const Scalar proj = Vector3Ops::Dot( h, dv );
	return Vector3(
		(dv.x - h.x * proj) * invLen,
		(dv.y - h.y * proj) * invLen,
		(dv.z - h.z * proj) * invLen );
}

void ManifoldSolver::BuildJacobian(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Previous and next positions
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		Vector3 s = Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Effective eta
		Scalar eta_eff = v.eta;
		if( !v.isReflection && Vector3Ops::Dot( wi, v.normal ) < 0.0 )
		{
			eta_eff = 1.0 / v.eta;
		}

		// Half-vector (unnormalized)
		Vector3 h_raw;
		if( v.isReflection )
		{
			h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			h_raw = Vector3(
				-(wi.x + eta_eff * wo.x),
				-(wi.y + eta_eff * wo.y),
				-(wi.z + eta_eff * wo.z) );
		}

		Scalar h_len = Vector3Ops::Magnitude( h_raw );
		if( h_len < NEARZERO ) continue;
		Vector3 h = h_raw * (1.0 / h_len);

		// ---- Derivative of h w.r.t. moving vertex i ----
		//
		// Moving vertex i by dp changes both wi and wo:
		//   dwi/dp = -(I - wi⊗wi) / dist_i * dp  (wi moves toward prev)
		//   dwo/dp = -(I - wo⊗wo) / dist_o * dp  (wo moves toward next)
		//
		// For h = sign * (wi + eta*wo) / |...|, with dp = dpdu:
		//   dh_raw/dp = sign * (dwi/dp + eta * dwo/dp)
		//   dh/dp = (dh_raw/dp - h * dot(h, dh_raw/dp)) / h_len
		//
		// We compute dh for dp = dpdu and dp = dpdv separately.

		// For each surface parameter (u, v):
		Vector3 dh_du, dh_dv;
		{
			// dp/du = dpdu, dp/dv = dpdv
			// dwi/du = -(dpdu - wi * dot(wi, dpdu)) / dist_i
			const Vector3 dwi_du = Vector3(
				-(v.dpdu.x - wi.x * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.y - wi.y * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.z - wi.z * Vector3Ops::Dot( wi, v.dpdu )) * inv_li );
			const Vector3 dwo_du = Vector3(
				-(v.dpdu.x - wo.x * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.y - wo.y * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.z - wo.z * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo );

			Vector3 dh_raw_du;
			if( v.isReflection )
			{
				dh_raw_du = Vector3( dwi_du.x + dwo_du.x, dwi_du.y + dwo_du.y, dwi_du.z + dwo_du.z );
			}
			else
			{
				dh_raw_du = Vector3(
					-(dwi_du.x + eta_eff * dwo_du.x),
					-(dwi_du.y + eta_eff * dwo_du.y),
					-(dwi_du.z + eta_eff * dwo_du.z) );
			}
			dh_du = DeriveNormalized( h, dh_raw_du, h_len );

			// Same for dv
			const Vector3 dwi_dv = Vector3(
				-(v.dpdv.x - wi.x * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.y - wi.y * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.z - wi.z * Vector3Ops::Dot( wi, v.dpdv )) * inv_li );
			const Vector3 dwo_dv = Vector3(
				-(v.dpdv.x - wo.x * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.y - wo.y * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.z - wo.z * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo );

			Vector3 dh_raw_dv;
			if( v.isReflection )
			{
				dh_raw_dv = Vector3( dwi_dv.x + dwo_dv.x, dwi_dv.y + dwo_dv.y, dwi_dv.z + dwo_dv.z );
			}
			else
			{
				dh_raw_dv = Vector3(
					-(dwi_dv.x + eta_eff * dwo_dv.x),
					-(dwi_dv.y + eta_eff * dwo_dv.y),
					-(dwi_dv.z + eta_eff * dwo_dv.z) );
			}
			dh_dv = DeriveNormalized( h, dh_raw_dv, h_len );
		}

		// Derivative of tangent frame due to normal curvature.
		// ds/du ≈ dndu projected onto s direction (Weingarten map).
		// For the constraint dot(s, h), the chain rule gives:
		//   d/du [dot(s, h)] = dot(ds/du, h) + dot(s, dh/du)
		//
		// ds/du = d/du [Normalize(dpdu)]
		// The dominant term comes from the normal curvature rotating s:
		//   ds/du ≈ (dndu - s * dot(s, dndu)) / |dpdu|
		// but for the constraint projection this simplifies to just
		// using the raw dndu projected onto the tangent plane.
		const Scalar inv_dpdu_len = 1.0 / fmax( Vector3Ops::Magnitude( v.dpdu ), NEARZERO );
		const Scalar inv_dpdv_len = 1.0 / fmax( Vector3Ops::Magnitude( v.dpdv ), NEARZERO );

		// ds/du and ds/dv (how s changes when moving along u or v)
		// Using the Weingarten equation: dn/du rotates the tangent frame
		const Vector3 ds_du = Vector3(
			(v.dndu.x - s.x * Vector3Ops::Dot( s, v.dndu )) * inv_dpdu_len,
			(v.dndu.y - s.y * Vector3Ops::Dot( s, v.dndu )) * inv_dpdu_len,
			(v.dndu.z - s.z * Vector3Ops::Dot( s, v.dndu )) * inv_dpdu_len );
		const Vector3 ds_dv = Vector3(
			(v.dndv.x - s.x * Vector3Ops::Dot( s, v.dndv )) * inv_dpdu_len,
			(v.dndv.y - s.y * Vector3Ops::Dot( s, v.dndv )) * inv_dpdu_len,
			(v.dndv.z - s.z * Vector3Ops::Dot( s, v.dndv )) * inv_dpdu_len );
		const Vector3 dt_du = Vector3(
			(v.dndu.x - t.x * Vector3Ops::Dot( t, v.dndu )) * inv_dpdv_len,
			(v.dndu.y - t.y * Vector3Ops::Dot( t, v.dndu )) * inv_dpdv_len,
			(v.dndu.z - t.z * Vector3Ops::Dot( t, v.dndu )) * inv_dpdv_len );
		const Vector3 dt_dv = Vector3(
			(v.dndv.x - t.x * Vector3Ops::Dot( t, v.dndv )) * inv_dpdv_len,
			(v.dndv.y - t.y * Vector3Ops::Dot( t, v.dndv )) * inv_dpdv_len,
			(v.dndv.z - t.z * Vector3Ops::Dot( t, v.dndv )) * inv_dpdv_len );

		// ---- Diagonal block: dC_i / dX_i ----
		// dC_i_s / du = dot(ds_du, h) + dot(s, dh_du)
		// dC_i_s / dv = dot(ds_dv, h) + dot(s, dh_dv)
		// dC_i_t / du = dot(dt_du, h) + dot(t, dh_du)
		// dC_i_t / dv = dot(dt_dv, h) + dot(t, dh_dv)
		diag[i*4 + 0] = Vector3Ops::Dot( ds_du, h ) + Vector3Ops::Dot( s, dh_du );  // row0, col0
		diag[i*4 + 1] = Vector3Ops::Dot( ds_dv, h ) + Vector3Ops::Dot( s, dh_dv );  // row0, col1
		diag[i*4 + 2] = Vector3Ops::Dot( dt_du, h ) + Vector3Ops::Dot( t, dh_du );  // row1, col0
		diag[i*4 + 3] = Vector3Ops::Dot( dt_dv, h ) + Vector3Ops::Dot( t, dh_dv );  // row1, col1

		// ---- Off-diagonal blocks: dC_i / dX_{i-1}  and  dC_i / dX_{i+1} ----
		//
		// Moving vertex i-1 only affects wi (not wo).
		// Moving vertex i+1 only affects wo (not wi).
		// The tangent frame (s, t) of vertex i does NOT change when
		// neighboring vertices move — only h changes.

		// Upper block: dC_i / dX_{i+1}  (if i < k-1)
		// Moving vertex i+1 by dp changes wo:
		//   dwo/dp_next = +(dp_next - wo * dot(wo, dp_next)) / dist_o
		// Note the +sign: when the next vertex moves by +dp, wo changes positively.
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;
				const Vector3 dwo = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				Vector3 dh_raw_next;
				if( v.isReflection )
					dh_raw_next = dwo;
				else
					dh_raw_next = Vector3( -eta_eff * dwo.x, -eta_eff * dwo.y, -eta_eff * dwo.z );

				const Vector3 dh_next = DeriveNormalized( h, dh_raw_next, h_len );

				// upper[i] maps vertex i+1 to constraint i
				upper[i*4 + 0 + p] = Vector3Ops::Dot( s, dh_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( t, dh_next );
			}
		}

		// Lower block: dC_i / dX_{i-1}  (if i > 0)
		// Moving vertex i-1 by dp changes wi:
		//   dwi/dp_prev = +(dp_prev - wi * dot(wi, dp_prev)) / dist_i
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;
				const Vector3 dwi = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				Vector3 dh_raw_prev;
				if( v.isReflection )
					dh_raw_prev = dwi;
				else
					dh_raw_prev = Vector3( -dwi.x, -dwi.y, -dwi.z );

				const Vector3 dh_prev = DeriveNormalized( h, dh_raw_prev, h_len );

				// lower[i-1] maps vertex i-1 to constraint i
				lower[(i-1)*4 + 0 + p] = Vector3Ops::Dot( s, dh_prev );
				lower[(i-1)*4 + 2 + p] = Vector3Ops::Dot( t, dh_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// SolveBlockTridiagonal
//
//   Solves J * delta = rhs where J is block-tridiagonal.
//   Each 2x2 block is stored as 4 scalars [a,b,c,d] in row-major.
//   diag is modified in place.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::Invert2x2( const Scalar* m, Scalar* inv )
{
	const Scalar det = m[0] * m[3] - m[1] * m[2];
	if( fabs(det) < NEARZERO )
	{
		return false;
	}
	const Scalar inv_det = 1.0 / det;
	inv[0] =  m[3] * inv_det;
	inv[1] = -m[1] * inv_det;
	inv[2] = -m[2] * inv_det;
	inv[3] =  m[0] * inv_det;
	return true;
}

void ManifoldSolver::Mul2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0]*B[0] + A[1]*B[2];
	C[1] = A[0]*B[1] + A[1]*B[3];
	C[2] = A[2]*B[0] + A[3]*B[2];
	C[3] = A[2]*B[1] + A[3]*B[3];
}

void ManifoldSolver::Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r )
{
	r[0] = A[0]*v[0] + A[1]*v[1];
	r[1] = A[2]*v[0] + A[3]*v[1];
}

void ManifoldSolver::Sub2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0] - B[0];
	C[1] = A[1] - B[1];
	C[2] = A[2] - B[2];
	C[3] = A[3] - B[3];
}

//////////////////////////////////////////////////////////////////////
// ComputeDielectricFresnel
//
//   Exact dielectric Fresnel reflectance (unpolarized average of
//   s- and p-polarized components).  Returns 1.0 for TIR.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeDielectricFresnel(
	Scalar cosI,
	Scalar eta_i,
	Scalar eta_t
	)
{
	if( cosI < 0 ) cosI = -cosI;

	const Scalar sinI2 = 1.0 - cosI * cosI;
	const Scalar sinT2 = (eta_i * eta_i) / (eta_t * eta_t) * sinI2;

	if( sinT2 >= 1.0 )
	{
		return 1.0;  // Total internal reflection
	}

	const Scalar cosT = sqrt( 1.0 - sinT2 );
	const Scalar rs = (eta_i * cosI - eta_t * cosT) / (eta_i * cosI + eta_t * cosT);
	const Scalar rp = (eta_t * cosI - eta_i * cosT) / (eta_t * cosI + eta_i * cosT);
	return (rs * rs + rp * rp) * 0.5;
}

//////////////////////////////////////////////////////////////////////
// ComputeSphericalDerivatives
//
//   World-space gradients of theta and phi from DirectionToSpherical.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSphericalDerivatives(
	const Vector3& dir,
	const Vector3& s,
	const Vector3& t,
	const Vector3& normal,
	Vector3& dTheta_dDir,
	Vector3& dPhi_dDir
	)
{
	const Scalar x = Vector3Ops::Dot( dir, s );
	const Scalar y = Vector3Ops::Dot( dir, t );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// theta = acos(z), d(theta)/d(z) = -1/sin(theta)
	const Scalar sinTheta = sqrt( fmax( 1.0 - z * z, 0.0 ) );
	if( sinTheta > NEARZERO )
	{
		const Scalar invSin = -1.0 / sinTheta;
		dTheta_dDir = Vector3( invSin * normal.x, invSin * normal.y, invSin * normal.z );
	}
	else
	{
		// dir ≈ ±normal, theta gradient is degenerate
		dTheta_dDir = Vector3( 0, 0, 0 );
	}

	// phi = atan2(y, x), d(phi)/d(x) = -y/r², d(phi)/d(y) = x/r²
	const Scalar r2 = x * x + y * y;
	if( r2 > NEARZERO )
	{
		const Scalar invR2 = 1.0 / r2;
		// dPhi/d(dir) = (-y/r²) * s + (x/r²) * t
		dPhi_dDir = Vector3(
			(-y * invR2) * s.x + (x * invR2) * t.x,
			(-y * invR2) * s.y + (x * invR2) * t.y,
			(-y * invR2) * s.z + (x * invR2) * t.z );
	}
	else
	{
		// dir ≈ ±normal, phi gradient is degenerate
		dPhi_dDir = Vector3( 0, 0, 0 );
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtWi
//
//   3x3 Jacobian d(wo)/d(wi) for reflection/refraction.
//   Assumes unnormalized wo (before normalization in
//   ComputeSpecularDirection).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtWi(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dwi[9]
	)
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi·n)*n
		// d(wo)/d(wi) = -I + 2*outer(n, n)
		dwo_dwi[0] = -1.0 + 2.0 * normal.x * normal.x;
		dwo_dwi[1] =        2.0 * normal.x * normal.y;
		dwo_dwi[2] =        2.0 * normal.x * normal.z;
		dwo_dwi[3] =        2.0 * normal.y * normal.x;
		dwo_dwi[4] = -1.0 + 2.0 * normal.y * normal.y;
		dwo_dwi[5] =        2.0 * normal.y * normal.z;
		dwo_dwi[6] =        2.0 * normal.z * normal.x;
		dwo_dwi[7] =        2.0 * normal.z * normal.y;
		dwo_dwi[8] = -1.0 + 2.0 * normal.z * normal.z;
	}
	else
	{
		// wo = -eta_ratio*wi + (eta_ratio*cos_i - cos_t)*n
		// For entering (cos_i > 0): eta_ratio = 1/eta
		// For exiting (cos_i < 0): eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;
		}

		const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);
		Scalar cos_t = 0.0;
		if( sin2_t < 1.0 )
			cos_t = sqrt( 1.0 - sin2_t );
		if( cos_t < NEARZERO )
			cos_t = NEARZERO;

		// d(wo)/d(wi) = -eta_ratio*I + eta_ratio*(1 - eta_ratio*ci/cos_t)*outer(n, n)
		const Scalar factor = eta_ratio * (1.0 - eta_ratio * ci / cos_t);

		dwo_dwi[0] = -eta_ratio + factor * n.x * n.x;
		dwo_dwi[1] =              factor * n.x * n.y;
		dwo_dwi[2] =              factor * n.x * n.z;
		dwo_dwi[3] =              factor * n.y * n.x;
		dwo_dwi[4] = -eta_ratio + factor * n.y * n.y;
		dwo_dwi[5] =              factor * n.y * n.z;
		dwo_dwi[6] =              factor * n.z * n.x;
		dwo_dwi[7] =              factor * n.z * n.y;
		dwo_dwi[8] = -eta_ratio + factor * n.z * n.z;
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiffNumerical
//
//   Numerical Jacobian for the angle-difference constraint.
//   Perturbs vertices and evaluates EvaluateConstraintAtVertex.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiffNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Helper lambda: evaluate angle-diff constraint at vertex ci
	// given modified chain positions
	auto evalAtVertex = [&]( unsigned int ci,
		const Point3& prevP, const Point3& nextP,
		const Point3& vPos, const Vector3& vNorm,
		Scalar& C0, Scalar& C1 )
	{
		const ManifoldVertex& v = chain[ci];
		EvaluateConstraintAtVertex(
			vPos, vNorm, v.dpdu, v.dpdv,
			v.eta, v.isReflection,
			prevP, nextP, C0, C1 );
	};

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			const Point3 posPlus = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			const Point3 posMinus = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			Vector3 normPlus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );
			Vector3 normMinus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			// Diagonal: constraint j w.r.t. vertex j
			{
				const Point3 prevP = (j == 0) ? fixedStart : chain[j-1].position;
				const Point3 nextP = (j == k-1) ? fixedEnd : chain[j+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( j, prevP, nextP, posPlus, normPlus, Cp0, Cp1 );
				evalAtVertex( j, prevP, nextP, posMinus, normMinus, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				diag[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				diag[j*4 + 2 + p] = dC1 * inv2eps;
			}

			// Upper: constraint j-1 w.r.t. vertex j (if j > 0)
			if( j > 0 )
			{
				unsigned int ci = j - 1;
				const Point3 prevP = (ci == 0) ? fixedStart : chain[ci-1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, prevP, posPlus, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, prevP, posMinus, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				upper[ci*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				upper[ci*4 + 2 + p] = dC1 * inv2eps;
			}

			// Lower: constraint j+1 w.r.t. vertex j (if j < k-1)
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;
				const Point3 nextP = (ci == k-1) ? fixedEnd : chain[ci+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, posPlus, nextP, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, posMinus, nextP, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				lower[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				lower[j*4 + 2 + p] = dC1 * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiff
//
//   Analytical Jacobian for the angle-difference constraint.
//   Chain rule through spherical coordinates, specular direction,
//   and surface curvature (Weingarten map).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiff(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		const Vector3 s = Vector3Ops::Normalize( v.dpdu );
		const Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Specularly scattered direction
		Vector3 wo_spec;
		if( !ComputeSpecularDirection( wi, v.normal, v.eta, v.isReflection, wo_spec ) )
		{
			// TIR — degenerate, leave zero entries
			continue;
		}

		// Spherical derivatives for wo_actual and wo_specular
		Vector3 dThetaA_dDir, dPhiA_dDir;
		ComputeSphericalDerivatives( wo, s, t, v.normal, dThetaA_dDir, dPhiA_dDir );

		Vector3 dThetaS_dDir, dPhiS_dDir;
		ComputeSphericalDerivatives( wo_spec, s, t, v.normal, dThetaS_dDir, dPhiS_dDir );

		// d(wo_spec)/d(wi) and d(wo_spec)/d(n) — 3x3 Jacobians
		Scalar dwoSpec_dwi[9], dwoSpec_dn[9];
		ComputeSpecularDirectionDerivativeWrtWi( wi, v.normal, v.eta, v.isReflection, dwoSpec_dwi );
		ComputeSpecularDirectionDerivativeWrtNormal( wi, v.normal, v.eta, v.isReflection, dwoSpec_dn );

		// ---- Diagonal block: dC_i / dX_i ----
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? v.dpdu : v.dpdv;
			const Vector3& dn = (p == 0) ? v.dndu : v.dndv;

			// d(wo_actual)/du: moving vertex changes direction to next vertex
			const Vector3 dwo_du = Vector3(
				-(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

			// d(wi)/du: moving vertex changes direction to previous vertex
			const Vector3 dwi_du = Vector3(
				-(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

			// d(wo_spec)/du = dwoSpec_dwi × dwi_du + dwoSpec_dn × dn
			Vector3 dwoSpec_du;
			dwoSpec_du.x = dwoSpec_dwi[0]*dwi_du.x + dwoSpec_dwi[1]*dwi_du.y + dwoSpec_dwi[2]*dwi_du.z
				         + dwoSpec_dn[0]*dn.x + dwoSpec_dn[1]*dn.y + dwoSpec_dn[2]*dn.z;
			dwoSpec_du.y = dwoSpec_dwi[3]*dwi_du.x + dwoSpec_dwi[4]*dwi_du.y + dwoSpec_dwi[5]*dwi_du.z
				         + dwoSpec_dn[3]*dn.x + dwoSpec_dn[4]*dn.y + dwoSpec_dn[5]*dn.z;
			dwoSpec_du.z = dwoSpec_dwi[6]*dwi_du.x + dwoSpec_dwi[7]*dwi_du.y + dwoSpec_dwi[8]*dwi_du.z
				         + dwoSpec_dn[6]*dn.x + dwoSpec_dn[7]*dn.y + dwoSpec_dn[8]*dn.z;

			// dC0/du = dTheta_actual/d(dir) · dwo_du - dTheta_spec/d(dir) · dwoSpec_du
			Scalar dC0_dp = Vector3Ops::Dot( dThetaA_dDir, dwo_du )
				          - Vector3Ops::Dot( dThetaS_dDir, dwoSpec_du );

			// dC1/du = dPhi_actual/d(dir) · dwo_du - dPhi_spec/d(dir) · dwoSpec_du
			Scalar dC1_dp = Vector3Ops::Dot( dPhiA_dDir, dwo_du )
				          - Vector3Ops::Dot( dPhiS_dDir, dwoSpec_du );

			// Frame rotation contribution: when the vertex moves, the
			// tangent frame (s, t, n) rotates, changing how the same
			// world-space direction maps to spherical coordinates.
			// For theta = acos(dir·n): d(theta)/d(n_pert) = -(1/sin(theta)) * dir
			// For phi: d(phi)/d(s_pert) and d(phi)/d(t_pert) via chain rule
			// These enter as additional terms for both wo_actual and wo_spec.
			//
			// theta_actual frame term: d(acos(wo·n))/d(n) × dn/du
			const Scalar sinThetaA = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo, v.normal) * Vector3Ops::Dot(wo, v.normal), 0.0 ) );
			if( sinThetaA > NEARZERO )
			{
				const Scalar dthetaA_dn = -Vector3Ops::Dot( wo, dn ) / sinThetaA;
				const Scalar sinThetaS = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo_spec, v.normal) * Vector3Ops::Dot(wo_spec, v.normal), 0.0 ) );
				const Scalar dthetaS_dn = (sinThetaS > NEARZERO) ?
					-Vector3Ops::Dot( wo_spec, dn ) / sinThetaS : 0.0;
				dC0_dp += dthetaA_dn - dthetaS_dn;
			}

			// phi frame terms (tangent rotation) — smaller effect, skip for now
			// to avoid excessive complexity.  The numerical Jacobian will
			// validate whether this omission matters.

			diag[i*4 + 0 + p] = dC0_dp;
			diag[i*4 + 2 + p] = dC1_dp;
		}

		// ---- Upper block: dC_i / dX_{i+1} ----
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;

				// Moving next vertex only affects wo_actual
				const Vector3 dwo_next = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				upper[i*4 + 0 + p] = Vector3Ops::Dot( dThetaA_dDir, dwo_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( dPhiA_dDir, dwo_next );
			}
		}

		// ---- Lower block: dC_i / dX_{i-1} ----
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;

				// Moving previous vertex only affects wi → wo_specular
				const Vector3 dwi_prev = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				// d(wo_spec)/d(wi_prev) = dwoSpec_dwi × dwi_prev
				Vector3 dwoSpec_prev;
				dwoSpec_prev.x = dwoSpec_dwi[0]*dwi_prev.x + dwoSpec_dwi[1]*dwi_prev.y + dwoSpec_dwi[2]*dwi_prev.z;
				dwoSpec_prev.y = dwoSpec_dwi[3]*dwi_prev.x + dwoSpec_dwi[4]*dwi_prev.y + dwoSpec_dwi[5]*dwi_prev.z;
				dwoSpec_prev.z = dwoSpec_dwi[6]*dwi_prev.x + dwoSpec_dwi[7]*dwi_prev.y + dwoSpec_dwi[8]*dwi_prev.z;

				lower[(i-1)*4 + 0 + p] = -Vector3Ops::Dot( dThetaS_dDir, dwoSpec_prev );
				lower[(i-1)*4 + 2 + p] = -Vector3Ops::Dot( dPhiS_dDir, dwoSpec_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// ValidateChainPhysics
//
//   Checks that converged specular vertices have physically
//   consistent geometry: refraction requires wi and wo on opposite
//   sides of the surface; reflection requires both on the same side.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ValidateChainPhysics(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		const Scalar wiDotN = Vector3Ops::Dot( wi, v.normal );
		const Scalar woDotN = Vector3Ops::Dot( wo, v.normal );

		if( v.isReflection )
		{
			// Reflection: both directions on the same side of surface
			if( wiDotN * woDotN < 0.0 )
			{
				return false;
			}
		}
		else
		{
			// Refraction: directions on opposite sides of surface
			if( wiDotN * woDotN > 0.0 )
			{
				return false;
			}
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// ComputeBlockTridiagonalDeterminant
//
//   Computes det(J) of a block-tridiagonal matrix via LU forward
//   elimination.  det = product of det(Dp[i]) for each 2x2 block.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeBlockTridiagonalDeterminant(
	const std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	unsigned int k
	) const
{
	if( k == 0 ) return 1.0;

	std::vector<Scalar> Dp( k * 4 );
	Scalar detProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			for( int q = 0; q < 4; q++ )
				Dp[q] = diag[q];
		}
		else
		{
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return 0.0;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );
		}

		const Scalar blkDet = Dp[i*4+0] * Dp[i*4+3] - Dp[i*4+1] * Dp[i*4+2];
		detProduct *= blkDet;
	}

	return detProduct;
}

bool ManifoldSolver::SolveBlockTridiagonal(
	std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	const std::vector<Scalar>& rhs,
	unsigned int k,
	std::vector<Scalar>& delta
	) const
{
	if( k == 0 )
	{
		return false;
	}

	delta.resize( 2 * k, 0.0 );

	// Modified diagonal blocks and modified rhs
	// We'll work with arrays of 2x2 blocks (4 scalars each) and 2-vectors
	std::vector<Scalar> Dp( k * 4 );   // modified diagonal
	std::vector<Scalar> rp( k * 2 );   // modified rhs

	// Forward sweep
	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			// Dp[0] = D[0]
			for( int q = 0; q < 4; q++ ) {
				Dp[q] = diag[q];
			}
			rp[0] = rhs[0];
			rp[1] = rhs[1];
		}
		else
		{
			// Dp[i] = D[i] - L[i] * inv(Dp[i-1]) * U[i-1]
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return false;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );

			// rp[i] = rhs[i] - L[i] * inv(Dp[i-1]) * rp[i-1]
			Scalar LiInvDpRhs[2];
			Mul2x2Vec( LiInvDp, &rp[(i-1)*2], LiInvDpRhs );

			rp[i*2]   = rhs[i*2]   - LiInvDpRhs[0];
			rp[i*2+1] = rhs[i*2+1] - LiInvDpRhs[1];
		}
	}

	// Back substitution
	for( int i = static_cast<int>(k) - 1; i >= 0; i-- )
	{
		Scalar rhs_i[2];
		rhs_i[0] = rp[i*2];
		rhs_i[1] = rp[i*2+1];

		if( i < static_cast<int>(k) - 1 )
		{
			// rhs_i -= U[i] * delta[i+1]
			Scalar Ud[2];
			Mul2x2Vec( &upper[i*4], &delta[(i+1)*2], Ud );
			rhs_i[0] -= Ud[0];
			rhs_i[1] -= Ud[1];
		}

		// delta[i] = inv(Dp[i]) * rhs_i
		Scalar invDp[4];
		if( !Invert2x2( &Dp[i*4], invDp ) )
		{
			return false;
		}
		Mul2x2Vec( invDp, rhs_i, &delta[i*2] );
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// UpdateVertexOnSurface
//
//   Steps a vertex along the surface parameterization by (du, dv)
//   and re-snaps to the surface via ray intersection.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::UpdateVertexOnSurface(
	ManifoldVertex& vertex,
	Scalar du,
	Scalar dv
	) const
{
	// Linear approximation of new position using tangent derivatives
	const Point3 newPos = Point3Ops::mkPoint3(
		vertex.position,
		vertex.dpdu * du + vertex.dpdv * dv
		);

	// Also update normal using normal derivatives (first-order)
	Vector3 newNormal = Vector3(
		vertex.normal.x + vertex.dndu.x * du + vertex.dndv.x * dv,
		vertex.normal.y + vertex.dndu.y * du + vertex.dndv.y * dv,
		vertex.normal.z + vertex.dndu.z * du + vertex.dndv.z * dv
		);
	newNormal = Vector3Ops::Normalize( newNormal );

	// For small steps, use linear approximation for position/normal
	// but always re-snap to the actual surface via intersection so
	// we get accurate derivatives for the next Newton step.
	const Scalar stepSize = sqrt( du * du + dv * dv );
	if( stepSize < 1e-8 )
	{
		// Negligible step — no change needed
		vertex.valid = true;
		return true;
	}

	// Project back onto the surface via ray intersection.
	// The probe offset should be small enough to stay near the current
	// surface but large enough to clear the local geometry.
	const Scalar probeOffset = fmin( fmax( stepSize * 2.0, 0.01 ), 0.5 );
	bool snapped = false;

	// Try from the normal side first
	{
		const Ray probeRay(
			Point3Ops::mkPoint3( newPos, newNormal * probeOffset ),
			Vector3( -newNormal.x, -newNormal.y, -newNormal.z )
			);

		RayIntersection ri( probeRay, nullRasterizerState );
		vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );

		if( ri.geometric.bHit )
		{
			vertex.position = ri.geometric.ptIntersection;
			vertex.normal = ri.geometric.vNormal;
			vertex.uv = ri.geometric.ptCoord;
			snapped = true;
		}
	}

	if( !snapped )
	{
		// Try from the other side
		const Ray probeRay2(
			Point3Ops::mkPoint3( newPos, newNormal * (-probeOffset) ),
			newNormal
			);

		RayIntersection ri2( probeRay2, nullRasterizerState );
		vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );

		if( ri2.geometric.bHit )
		{
			vertex.position = ri2.geometric.ptIntersection;
			vertex.normal = ri2.geometric.vNormal;
			vertex.uv = ri2.geometric.ptCoord;
			snapped = true;
		}
	}

	if( !snapped )
	{
		// Fall back to the linear approximation (no re-snap)
		vertex.position = newPos;
		vertex.normal = newNormal;
	}

	// Recompute surface derivatives at the new position
	vertex.valid = ComputeVertexDerivatives( vertex );
	return vertex.valid;
}

//////////////////////////////////////////////////////////////////////
// ComputeVertexDerivatives
//
//   Fills in dpdu, dpdv, dndu, dndv by querying the object's
//   geometry with proper world/object space transforms.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeVertexDerivatives(
	ManifoldVertex& vertex
	) const
{
	if( !vertex.pObject )
	{
		return false;
	}

	// Compute derivatives numerically via central finite differences
	// using ray-surface intersection.  Central differences give O(eps²)
	// accuracy vs O(eps) for forward differences, which is critical for
	// Newton convergence on curved meshes.
	const Scalar eps = 5e-4;

	Vector3 tangent_u, tangent_v;

	if( Vector3Ops::SquaredModulus( vertex.dpdu ) > NEARZERO &&
		Vector3Ops::SquaredModulus( vertex.dpdv ) > NEARZERO )
	{
		tangent_u = Vector3Ops::Normalize( vertex.dpdu );
		tangent_v = Vector3Ops::Normalize( vertex.dpdv );
	}
	else
	{
		tangent_u = Vector3Ops::Perpendicular( vertex.normal );
		tangent_u = Vector3Ops::Normalize( tangent_u );
		tangent_v = Vector3Ops::Cross( vertex.normal, tangent_u );
		tangent_v = Vector3Ops::Normalize( tangent_v );
	}

	// Probe a surface point near vertex.position along a tangent direction.
	// Returns true if successful, filling outPos and outNormal.
	// Use a small probeOffset proportional to eps to avoid punching through
	// thin geometry (the displaced slab is only ~0.15 thick).
	struct ProbeResult { Point3 pos; Vector3 normal; bool ok; };
	const Scalar derivProbeOffset = fmin( eps * 20.0, 0.02 );
	auto probeAt = [&]( const Point3& testPos ) -> ProbeResult
	{
		ProbeResult r;
		r.ok = false;
		const Scalar probeOffset = derivProbeOffset;

		// Probe from the normal side
		{
			const Ray probeRay(
				Point3Ops::mkPoint3( testPos, vertex.normal * probeOffset ),
				Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z ) );
			RayIntersection ri( probeRay, nullRasterizerState );
			vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );
			if( ri.geometric.bHit )
			{
				r.pos = ri.geometric.ptIntersection;
				r.normal = ri.geometric.vNormal;
				r.ok = true;
				return r;
			}
		}

		// Try from the other side
		{
			const Ray probeRay2(
				Point3Ops::mkPoint3( testPos, vertex.normal * (-probeOffset) ),
				vertex.normal );
			RayIntersection ri2( probeRay2, nullRasterizerState );
			vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );
			if( ri2.geometric.bHit )
			{
				r.pos = ri2.geometric.ptIntersection;
				r.normal = ri2.geometric.vNormal;
				r.ok = true;
			}
		}

		return r;
	};

	// Central differences: probe at +eps and -eps in each tangent direction
	ProbeResult u_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * eps ) );
	ProbeResult u_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * (-eps) ) );
	ProbeResult v_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * eps ) );
	ProbeResult v_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * (-eps) ) );

	if( u_plus.ok && u_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdu = Vector3(
			(u_plus.pos.x - u_minus.pos.x) * inv2eps,
			(u_plus.pos.y - u_minus.pos.y) * inv2eps,
			(u_plus.pos.z - u_minus.pos.z) * inv2eps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - u_minus.normal.x) * inv2eps,
			(u_plus.normal.y - u_minus.normal.y) * inv2eps,
			(u_plus.normal.z - u_minus.normal.z) * inv2eps );
	}
	else if( u_plus.ok )
	{
		// Fall back to forward difference
		const Scalar invEps = 1.0 / eps;
		vertex.dpdu = Vector3(
			(u_plus.pos.x - vertex.position.x) * invEps,
			(u_plus.pos.y - vertex.position.y) * invEps,
			(u_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - vertex.normal.x) * invEps,
			(u_plus.normal.y - vertex.normal.y) * invEps,
			(u_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdu = tangent_u;
		vertex.dndu = Vector3( 0, 0, 0 );
	}

	if( v_plus.ok && v_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdv = Vector3(
			(v_plus.pos.x - v_minus.pos.x) * inv2eps,
			(v_plus.pos.y - v_minus.pos.y) * inv2eps,
			(v_plus.pos.z - v_minus.pos.z) * inv2eps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - v_minus.normal.x) * inv2eps,
			(v_plus.normal.y - v_minus.normal.y) * inv2eps,
			(v_plus.normal.z - v_minus.normal.z) * inv2eps );
	}
	else if( v_plus.ok )
	{
		const Scalar invEps = 1.0 / eps;
		vertex.dpdv = Vector3(
			(v_plus.pos.x - vertex.position.x) * invEps,
			(v_plus.pos.y - vertex.position.y) * invEps,
			(v_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - vertex.normal.x) * invEps,
			(v_plus.normal.y - vertex.normal.y) * invEps,
			(v_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdv = tangent_v;
		vertex.dndv = Vector3( 0, 0, 0 );
	}

	// Project dpdu and dpdv into the tangent plane and orthogonalize.
	// Finite difference probes on curved surfaces can produce tangent
	// vectors with a normal component, which makes the Jacobian
	// inconsistent with the constraint's tangent-plane projection.
	{
		const Vector3& n = vertex.normal;

		// Project dpdu into tangent plane
		Scalar d_n = Vector3Ops::Dot( vertex.dpdu, n );
		vertex.dpdu = Vector3(
			vertex.dpdu.x - n.x * d_n,
			vertex.dpdu.y - n.y * d_n,
			vertex.dpdu.z - n.z * d_n );

		// Project dpdv into tangent plane
		d_n = Vector3Ops::Dot( vertex.dpdv, n );
		vertex.dpdv = Vector3(
			vertex.dpdv.x - n.x * d_n,
			vertex.dpdv.y - n.y * d_n,
			vertex.dpdv.z - n.z * d_n );

		// Gram-Schmidt: make dpdv orthogonal to dpdu
		const Scalar dpdu_sq = Vector3Ops::SquaredModulus( vertex.dpdu );
		if( dpdu_sq > NEARZERO )
		{
			const Scalar proj = Vector3Ops::Dot( vertex.dpdv, vertex.dpdu ) / dpdu_sq;
			vertex.dpdv = Vector3(
				vertex.dpdv.x - vertex.dpdu.x * proj,
				vertex.dpdv.y - vertex.dpdu.y * proj,
				vertex.dpdv.z - vertex.dpdu.z * proj );
		}

		// If dpdv collapsed, reconstruct from cross product
		if( Vector3Ops::SquaredModulus( vertex.dpdv ) < NEARZERO )
		{
			vertex.dpdv = Vector3Ops::Cross( n, vertex.dpdu );
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// NewtonSolve
//
//   Runs Newton iteration to solve C(x) = 0.
//   Modifies chain in-place.  Returns true if converged.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::NewtonSolve(
	std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return false;
	}

	for( unsigned int iter = 0; iter < config.maxIterations; iter++ )
	{
		// Evaluate constraint
		std::vector<Scalar> C;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C );

		// Compute norm of constraint
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
		{
			norm2 += C[i] * C[i];
		}
		const Scalar norm = sqrt( norm2 );

		if( norm < config.solverThreshold )
		{
			return true;  // Converged
		}

		// Build Jacobian (analytical half-vector, matching constraint)
		std::vector<Scalar> diag, upper_blocks, lower_blocks;
		BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks );

		// Solve for Newton step: J * delta = C  (we solve J * delta = C, then subtract)
		std::vector<Scalar> delta;
		if( !SolveBlockTridiagonal( diag, upper_blocks, lower_blocks, C, k, delta ) )
		{
			return false;  // Singular Jacobian
		}

		// Apply update with line search: try decreasing step sizes
		// until the constraint norm actually decreases.
		Scalar beta = 1.0;
		bool allValid = false;
		bool improved = false;

		// Save chain state
		std::vector<ManifoldVertex> savedChain( chain );

		for( unsigned int attempt = 0; attempt < 10; attempt++ )
		{
			// Restore chain from saved state
			chain = savedChain;

			// Apply step: x_new = x - beta * delta
			allValid = true;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar du = -beta * delta[2*i];
				const Scalar dv = -beta * delta[2*i+1];

				if( !UpdateVertexOnSurface( chain[i], du, dv ) )
				{
					allValid = false;
					break;
				}

				if( !chain[i].valid )
				{
					allValid = false;
					break;
				}
			}

			if( allValid )
			{
				// Check if the norm actually decreased
				std::vector<Scalar> C_test;
				EvaluateConstraint( chain, fixedStart, fixedEnd, C_test );
				Scalar testNorm2 = 0.0;
				for( unsigned int i = 0; i < 2*k; i++ )
				{
					testNorm2 += C_test[i] * C_test[i];
				}
				if( sqrt(testNorm2) < norm )
				{
					improved = true;
					break;
				}
			}

			beta *= 0.5;
		}

		if( !improved )
		{
			// Even the smallest step didn't improve — give up
			chain = savedChain;
			return false;
		}

		if( !allValid )
		{
			// Restore and report failure
			chain = savedChain;
			return false;
		}
	}

	// Did not converge within maxIterations.  Check if the constraint
	// norm is close enough to accept as a soft convergence.  On meshes
	// with discontinuous normals (triangle edges), Newton may oscillate
	// near the solution without reaching the strict threshold.  A
	// relaxed threshold of 10× catches these near-solutions.
	{
		std::vector<Scalar> C_final;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C_final );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
			norm2 += C_final[i] * C_final[i];
		if( sqrt(norm2) < config.solverThreshold * 10.0 )
		{
			return true;  // Soft convergence
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////
// BuildSeedChain
//
//   Traces a ray from start toward end, following refraction at each
//   specular surface to build a seed chain that naturally discovers
//   multi-object specular paths.  At each glass surface, the ray is
//   refracted using Snell's law, allowing it to follow the physical
//   light path through interlocking glass objects rather than only
//   finding objects along the straight line.
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSeedChain(
	const Point3& start,
	const Point3& end,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain
	) const
{
	chain.clear();

	Vector3 dir = Vector3Ops::mkVector3( end, start );
	const Scalar totalDist = Vector3Ops::NormalizeMag( dir );

	if( totalDist < NEARZERO )
	{
		return 0;
	}

	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr )
	{
		return 0;
	}

	Point3 currentOrigin = start;
	const Scalar offsetEps = 1e-2;

	// Track IOR of current medium (start outside in air)
	Scalar currentIOR = 1.0;


	for( unsigned int depth = 0; depth < config.maxChainDepth; depth++ )
	{
		// Offset origin along ray direction to avoid
		// re-intersecting the surface we just left
		Point3 offsetOrigin = Point3Ops::mkPoint3(
			currentOrigin, dir * offsetEps );

		Ray ray( offsetOrigin, dir );
		RayIntersection ri( ray, nullRasterizerState );

		// Scene-wide intersection via the acceleration structure
		pObjMgr->IntersectRay( ri, true, true, false );

		if( !ri.geometric.bHit )
		{
			break;
		}

		// Self-intersection guard: if we hit the same object extremely
		// close to where we just were, we are re-hitting the surface
		// we just left.  The threshold must be small enough to NOT
		// filter out the legitimate exit intersection from the far
		// side of the same object.
		if( ri.geometric.range < offsetEps * 2.0 && !chain.empty() &&
			ri.pObject == chain.back().pObject )
		{
			currentOrigin = Point3Ops::mkPoint3(
				ri.geometric.ptIntersection, dir * offsetEps );
			continue;
		}

		// Safety: don't trace forever
		if( ri.geometric.range > totalDist * 3.0 )
		{
			break;
		}

		// Check if the hit material is specular
		const IMaterial* pMat = ri.pMaterial;
		if( !pMat )
		{
			break;
		}

		SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, 0 );

		if( !specInfo.isSpecular )
		{
			// Hit a non-specular surface; stop tracing
			break;
		}

		// Create ManifoldVertex from this intersection
		ManifoldVertex mv;
		mv.position = ri.geometric.ptIntersection;
		mv.normal = ri.geometric.vNormal;
		mv.pObject = ri.pObject;
		mv.pMaterial = pMat;
		mv.eta = specInfo.ior;
		mv.attenuation = specInfo.attenuation;
		mv.isReflection = !specInfo.canRefract;
		mv.valid = false;  // Derivatives not yet computed; Solve will handle it

		chain.push_back( mv );

		// Follow refraction/reflection to determine the next ray direction.
		// This is what allows the seed chain to discover multi-object
		// paths through interlocking glass objects.
		if( specInfo.canRefract )
		{
			// Determine if entering or exiting the object based on
			// the angle between the ray direction and surface normal
			const Scalar cosI = Vector3Ops::Dot( dir, mv.normal );
			Vector3 n = mv.normal;
			Scalar etaRatio;

			if( cosI < 0 )
			{
				// Entering the object (ray and normal point in opposite directions)
				etaRatio = currentIOR / specInfo.ior;
				// Normal already points outward
			}
			else
			{
				// Exiting the object (ray and normal point same direction)
				etaRatio = specInfo.ior / currentIOR;
				n = n * (-1.0);  // Flip normal to face the incoming ray
			}

			// Compute refracted direction using Snell's law
			const Scalar cosI2 = -Vector3Ops::Dot( dir, n );
			const Scalar sin2T = etaRatio * etaRatio * (1.0 - cosI2 * cosI2);

			if( sin2T <= 1.0 )
			{
				// Refraction succeeds
				const Scalar cosT = sqrt( 1.0 - sin2T );
				dir = dir * etaRatio + n * (etaRatio * cosI2 - cosT);
				dir = Vector3Ops::Normalize( dir );

				// Update current medium IOR
				if( cosI < 0 )
				{
					currentIOR = specInfo.ior;  // Now inside the object
				}
				else
				{
					currentIOR = 1.0;  // Back in air
				}
			}
			else
			{
				// Total internal reflection
				dir = dir + n * (2.0 * cosI2);
				dir = Vector3Ops::Normalize( dir );
			}
		}
		else
		{
			// Pure reflection (mirror)
			const Scalar cosI = -Vector3Ops::Dot( dir, mv.normal );
			dir = dir + mv.normal * (2.0 * cosI);
			dir = Vector3Ops::Normalize( dir );
		}

		// Move origin past the surface along the refracted/reflected
		// direction.  We use just the new direction with the offset
		// applied at the top of the loop.
		currentOrigin = ri.geometric.ptIntersection;
	}

	return static_cast<unsigned int>( chain.size() );
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughput
//
//   Computes Fresnel-weighted transmittance/reflectance product
//   along a converged specular chain, including Beer's law.
//////////////////////////////////////////////////////////////////////
// EvaluateChainGeometry
//
//   Computes the geometric coupling factor through a specular chain
//   for the path integral.  For delta BSDFs, integrating the delta
//   function cancels the outgoing cosine at each specular vertex.
//   What remains per segment is cos(θ_incoming) / dist².
//
//   For chain  x → v_1 → ... → v_k → y  the result is:
//
//     cos(θ_x) × ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]
//              × cos(θ_y) / dist(vk,y)²
//
//   The caller supplies cosAtShading (= cos θ_x) and cosAtLight
//   (= cos θ_y) separately, so this function returns the product
//   of the segment factors only:
//
//     ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]  ×  1/dist(vk,y)²
//
//   cosAtShading and cosAtLight are multiplied by the caller.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainGeometry(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 1.0;

	Scalar geom = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 dir = Vector3Ops::mkVector3( chain[i].position, prevPos );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;
		dir = dir * (1.0 / dist);

		// Incoming cosine at this specular vertex
		const Scalar cosIn = fabs( Vector3Ops::Dot( chain[i].normal, dir ) );

		geom *= cosIn / (dist * dist);
	}

	// Last segment: chain[k-1] to endPoint (the light).
	// Only 1/dist² here — cosAtLight is supplied by the caller.
	{
		Vector3 dir = Vector3Ops::mkVector3( endPoint, chain[k-1].position );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;

		geom *= 1.0 / (dist * dist);
	}

	return geom;
}

//////////////////////////////////////////////////////////////////////

RISEPel ManifoldSolver::EvaluateChainThroughput(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	RISEPel throughput( 1.0, 1.0, 1.0 );
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Compute incoming direction at this vertex
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance
		const Scalar cosI_signed = Vector3Ops::Dot( wi, v.normal );
		const Scalar cosI = fabs( cosI_signed );
		const Scalar eta_i = (cosI_signed >= 0) ? 1.0 : v.eta;
		const Scalar eta_t = (cosI_signed >= 0) ? v.eta : 1.0;
		const Scalar fr = ComputeDielectricFresnel( cosI, eta_i, eta_t );

		if( v.isReflection )
		{
			throughput = throughput * v.attenuation * fr;
		}
		else
		{
			throughput = throughput * v.attenuation * (1.0 - fr);
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughputNM
//
//   Scalar (spectral) variant of EvaluateChainThroughput.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainThroughputNM(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain,
	const Scalar nm
	) const
{
	Scalar throughput = 1.0;
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance
		const Scalar cosI_signed = Vector3Ops::Dot( wi, v.normal );
		const Scalar cosI = fabs( cosI_signed );
		const Scalar eta_i = (cosI_signed >= 0) ? 1.0 : v.eta;
		const Scalar eta_t = (cosI_signed >= 0) ? v.eta : 1.0;
		const Scalar fr = ComputeDielectricFresnel( cosI, eta_i, eta_t );

		if( v.isReflection )
		{
			throughput *= fr;
		}
		else
		{
			throughput *= (1.0 - fr);
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// ComputeManifoldGeometricTerm
//
//   Computes the generalized geometric term (Jacobian determinant
//   ratio) for MIS weighting.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeManifoldGeometricTerm(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return 1.0;
	}

	// Build Jacobian and compute its determinant
	std::vector<Scalar> diag, upper_blocks, lower_blocks;
	BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks );

	const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
		diag, upper_blocks, lower_blocks, k );

	// Geometric distances
	Scalar distProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		const Scalar d1 = Point3Ops::Distance( prevPos, chain[i].position );
		const Scalar d2 = Point3Ops::Distance( chain[i].position, nextPos );

		if( d1 > NEARZERO && d2 > NEARZERO )
		{
			distProduct *= d1 * d1 * d2 * d2;
		}
	}

	// Apply surface metric correction: convert from parameter-space
	// Jacobian to tangent-plane world-space Jacobian
	Scalar metricProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Scalar dpduLen = Vector3Ops::Magnitude( chain[i].dpdu );
		const Scalar dpdvLen = Vector3Ops::Magnitude( chain[i].dpdv );
		if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
		{
			metricProduct *= dpduLen * dpdvLen;
		}
	}

	Scalar geoTerm = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
	if( distProduct > NEARZERO )
	{
		geoTerm /= distProduct;
	}

	// Clamp
	if( geoTerm > config.maxGeometricTerm )
	{
		geoTerm = config.maxGeometricTerm;
	}

	return geoTerm;
}

//////////////////////////////////////////////////////////////////////
// EstimatePDF
//
//   Bernoulli trial estimator for unbiased PDF estimation.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EstimatePDF(
	const ManifoldResult& solution,
	const Point3& shadingPoint,
	const Point3& emitterPoint,
	const std::vector<ManifoldVertex>& seedTemplate,
	ISampler& sampler
	) const
{
	if( !solution.valid )
	{
		return 1.0;
	}

	unsigned int count = 0;
	unsigned int trials = 0;
	const unsigned int targetCount = 1;
	const unsigned int k = static_cast<unsigned int>( solution.specularChain.size() );

	while( count < targetCount && trials < config.maxBernoulliTrials )
	{
		// Generate a random seed chain by perturbing the template
		std::vector<ManifoldVertex> testChain( seedTemplate );

		for( unsigned int i = 0; i < testChain.size(); i++ )
		{
			// Random perturbation in tangent plane
			const Scalar ru = sampler.Get1D() * 0.2 - 0.1;
			const Scalar rv = sampler.Get1D() * 0.2 - 0.1;

			testChain[i].position = Point3Ops::mkPoint3(
				testChain[i].position,
				testChain[i].dpdu * ru + testChain[i].dpdv * rv
				);

			// Re-snap to surface
			UpdateVertexOnSurface( testChain[i], 0.0, 0.0 );
		}

		// Run Newton solve on this perturbed chain
		if( NewtonSolve( testChain, shadingPoint, emitterPoint ) )
		{
			// Check if this converged to the same solution
			bool same = true;
			for( unsigned int i = 0; i < k && i < testChain.size(); i++ )
			{
				const Scalar dist = Point3Ops::Distance(
					testChain[i].position,
					solution.specularChain[i].position
					);
				if( dist > config.uniquenessThreshold )
				{
					same = false;
					break;
				}
			}

			if( same )
			{
				count++;
			}
		}

		trials++;
	}

	if( count == 0 )
	{
		return 1.0;  // Fallback
	}

	return static_cast<Scalar>(trials) / static_cast<Scalar>(count);
}

//////////////////////////////////////////////////////////////////////
// Solve (main entry point)
//////////////////////////////////////////////////////////////////////

ManifoldResult ManifoldSolver::Solve(
	const Point3& shadingPoint,
	const Vector3& shadingNormal,
	const Point3& emitterPoint,
	const Vector3& emitterNormal,
	std::vector<ManifoldVertex>& specularChain,
	ISampler& sampler
	) const
{
	ManifoldResult result;

	if( specularChain.empty() )
	{
		return result;
	}

	const unsigned int chainK = static_cast<unsigned int>( specularChain.size() );

	// Quick early-out: build minimal tangent frames from normals
	// and evaluate the initial constraint.  If the norm is too large,
	// bail before computing expensive surface derivatives.
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Evaluate constraint with minimal tangent frames.
	// Early-out if the seed is too far from any valid path.
	// Use a generous threshold — reflection seeds can be further
	// from the solution than refraction seeds because the straight-line
	// seed direction doesn't follow the reflected path.
	{
		std::vector<Scalar> C0;
		EvaluateConstraint( specularChain, shadingPoint, emitterPoint, C0 );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < C0.size(); i++ )
			norm2 += C0[i] * C0[i];
		if( sqrt(norm2) > 2.0 )
		{
					return result;  // Seed too far from valid path
		}
	}

	// Now compute full surface derivatives (the expensive part)
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( !v.valid )
		{
			if( !ComputeVertexDerivatives( v ) )
			{
					return result;
			}
		}

		// Re-ensure tangent frame after derivative computation
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Save seed template for Bernoulli trials
	const std::vector<ManifoldVertex> seedTemplate( specularChain );

	// Run Newton solver
	const bool converged = NewtonSolve( specularChain, shadingPoint, emitterPoint );

	if( converged )
	{
		// Reject physically invalid converged solutions.
		if( !ValidateChainPhysics( specularChain, shadingPoint, emitterPoint ) )
		{
			return result;
		}

		result.valid = true;
		result.specularChain = specularChain;

		// Evaluate throughput (Fresnel * Beer's law along the chain)
		result.contribution = EvaluateChainThroughput( shadingPoint, emitterPoint, specularChain );

		// Compute constraint Jacobian determinant |det(∂C/∂x_⊥)| for the
		// converged chain.  Uses the half-vector analytical Jacobian because
		// the contribution formula was derived for that formulation.
		// Newton uses angle-difference for convergence, but the measure
		// conversion factor must match the derivation.
		{
			const unsigned int k = static_cast<unsigned int>( specularChain.size() );

			std::vector<Scalar> diag, upper_blocks, lower_blocks;
			BuildJacobian( specularChain, shadingPoint, emitterPoint,
				diag, upper_blocks, lower_blocks );

			const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
				diag, upper_blocks, lower_blocks, k );

			// Convert from parameter-space to tangent-plane world coordinates
			Scalar metricProduct = 1.0;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar dpduLen = Vector3Ops::Magnitude( specularChain[i].dpdu );
				const Scalar dpdvLen = Vector3Ops::Magnitude( specularChain[i].dpdv );
				if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
				{
					metricProduct *= dpduLen * dpdvLen;
				}
			}

			result.jacobianDet = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
			if( result.jacobianDet < 1e-20 )
				result.jacobianDet = 1e-20;
		}

		// PDF estimation
		if( !config.biased )
		{
			result.pdf = EstimatePDF( result, shadingPoint, emitterPoint, seedTemplate, sampler );
		}
		else
		{
			result.pdf = 1.0;
		}
	}
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPoint
//
//   Standalone SMS evaluation at a single shading point.
//   Samples a light, builds a seed chain, solves the manifold,
//   evaluates the BSDF, and assembles the full contribution.
//
//   This is the reusable core that both BDPTIntegrator and
//   SMSShaderOp call.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContribution ManifoldSolver::EvaluateAtShadingPoint(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler
	) const
{
	SMSContribution result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler (which has the alias table
	// built during scene preparation) rather than our own uninitialized one.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;


	// Build seed chain toward the light sample.  If that ray misses the
	// specular geometry, try fallback directions.  The Newton solver only
	// needs the correct chain topology — it will reposition vertices to
	// satisfy the specular constraint for the actual light sample.
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSeedChain(
		pos, lightSample.position,
		scene, caster, seedChain );

	if( chainLen == 0 || seedChain.empty() )
	{
		// Fallback: trace along the surface normal.
		const Point3 normalTarget = Point3Ops::mkPoint3(
			pos, normal * 100.0 );
		chainLen = BuildSeedChain(
			pos, normalTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		// Fallback: try tracing toward the midpoint between pos and
		// the light.  For tilted geometry, the specular object may lie
		// between the two endpoints at a position that neither the
		// light-direction nor the normal-direction ray can reach.
		const Point3 midpoint(
			(pos.x + lightSample.position.x) * 0.5,
			(pos.y + lightSample.position.y) * 0.5,
			(pos.z + lightSample.position.z) * 0.5 );
		const Point3 midTarget = Point3Ops::mkPoint3(
			pos, Vector3Ops::Normalize( Vector3Ops::mkVector3( midpoint, pos ) ) * 100.0 );
		chainLen = BuildSeedChain(
			pos, midTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		// No specular geometry found — skip.
		return result;
	}

	// Run manifold solve
	ManifoldResult mResult = Solve(
		pos, normal,
		lightSample.position, lightSample.normal,
		seedChain, sampler );

	if( !mResult.valid )
		return result;

	// Visibility: check external segments of the specular chain
	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster ) )
		return result;

	// Direction from shading point toward first specular vertex
	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
		firstSpec.position, pos );
	Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) return result;
	dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

	const Vector3 wiAtShading = dirToFirstSpec;

	// Evaluate BSDF at shading point
	// RISE convention: ri.ray.Dir() is toward the surface (negate woOutgoing)
	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal = normal;
	rig.onb = onb;

	RISEPel fBSDF = pBSDF->value( wiAtShading, rig );
	if( ColorMath::MaxValue( fBSDF ) <= 0 )
		return result;

	// Cosine at shading point: uses the actual direction toward the
	// first specular vertex (where light arrives from after refraction).
	const Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return result;

	// Direction and distance from last specular vertex to light
	// (needed for cosine evaluation at the light surface).
	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return result;
	dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

	// For delta-position lights (point/spot), there is no surface at the
	// light — the geometric coupling has no cosine term.  The emitted
	// radiance must also be re-evaluated for the actual direction from the
	// light toward the last specular vertex, since the LightSample's Le
	// was evaluated at a random photon direction.
	Scalar cosAtLight;
	RISEPel actualLe;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		if( lightSample.pLight ) {
			actualLe = lightSample.pLight->emittedRadiance( dirSpecToLight );
		} else {
			actualLe = lightSample.Le;
		}
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return result;
		actualLe = lightSample.Le;
	}

	// SMS geometric factor (Zeltner et al. 2020).
	//
	// The path integral for a specular chain includes geometric coupling
	// at every edge (G terms) in the numerator, divided by the constraint
	// Jacobian determinant |det(∂C/∂x_⊥)| which converts the light-area
	// sampling density to the path-space density:
	//
	//   L = f_s · cos(θ_x) · T_chain · Le · cos(θ_y) · G_chain
	//       / (p(y) · |det(∂C/∂x_⊥)|)
	//
	// G_chain = ∏[cos(θ_in_vi)/dist²] · 1/dist²(vk,y)  (via EvaluateChainGeometry)
	//
	// Both G_chain and |det| blow up for thin slabs (small internal
	// distances), but their RATIO is well-behaved — the determinant
	// encodes the same 1/dist scaling from its direction derivatives.
	//
	// Our BuildJacobian + Solve compute jacobianDet = |det(∂C/∂(u,v))|.
	// With ||dpdu|| ≈ ||dpdv|| ≈ 1 (unit tangents from finite differences),
	// this equals |det(∂C/∂x_⊥)| in projected-area coordinates.
	const Scalar chainGeom = EvaluateChainGeometry(
		pos, lightSample.position, mResult.specularChain );

	const Scalar smsGeometric = cosAtLight * chainGeom
		/ fmax( mResult.jacobianDet, 1e-20 );

	// Clamp to prevent fireflies from near-singular Jacobians
	const Scalar clampedGeometric = fmin( smsGeometric, config.maxGeometricTerm );

	result.contribution = fBSDF
		* mResult.contribution
		* actualLe * cosAtShading * clampedGeometric
		/ (lightSample.pdfPosition * lightSample.pdfSelect);

	result.misWeight = 1.0 / mResult.pdf;
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointNM
//
//   Spectral variant of EvaluateAtShadingPoint.
//   Uses per-wavelength IOR for dispersion and scalar evaluation
//   throughout.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContributionNM ManifoldSolver::EvaluateAtShadingPointNM(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const Scalar nm
	) const
{
	SMSContributionNM result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;


	// Build seed chain toward light, with fallbacks (see RGB variant).
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSeedChain(
		pos, lightSample.position,
		scene, caster, seedChain );

	if( chainLen == 0 || seedChain.empty() )
	{
		const Point3 normalTarget = Point3Ops::mkPoint3(
			pos, normal * 100.0 );
		chainLen = BuildSeedChain(
			pos, normalTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		const Point3 midpoint(
			(pos.x + lightSample.position.x) * 0.5,
			(pos.y + lightSample.position.y) * 0.5,
			(pos.z + lightSample.position.z) * 0.5 );
		const Point3 midTarget = Point3Ops::mkPoint3(
			pos, Vector3Ops::Normalize( Vector3Ops::mkVector3( midpoint, pos ) ) * 100.0 );
		chainLen = BuildSeedChain(
			pos, midTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		return result;
	}

	// Override each vertex's IOR with the wavelength-dependent value.
	// This is what makes dispersion work — the Newton solver will find
	// a different position for each wavelength due to the different IOR.
	for( unsigned int i = 0; i < seedChain.size(); i++ )
	{
		if( seedChain[i].pMaterial )
		{
			// Build a minimal RayIntersectionGeometric for the query
			Ray dummyRay( seedChain[i].position, seedChain[i].normal );
			RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = seedChain[i].position;
			rig.vNormal = seedChain[i].normal;

			SpecularInfo specNM = seedChain[i].pMaterial->GetSpecularInfoNM(
				rig, 0, nm );
			seedChain[i].eta = specNM.ior;
		}
	}

	// Run manifold solve with wavelength-dependent IOR
	ManifoldResult mResult = Solve(
		pos, normal,
		lightSample.position, lightSample.normal,
		seedChain, sampler );

	if( !mResult.valid )
		return result;

	// Visibility: check external segments of the specular chain
	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster ) )
		return result;

	// Direction from shading point toward first specular vertex
	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
		firstSpec.position, pos );
	Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) return result;
	dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

	const Vector3 wiAtShading = dirToFirstSpec;

	// Evaluate BSDF at shading point (spectral)
	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal = normal;
	rig.onb = onb;

	Scalar fBSDF = pBSDF->valueNM( wiAtShading, rig, nm );
	if( fBSDF <= 0 )
		return result;

	// Cosine at shading point
	Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return result;

	// Chain throughput (spectral — per-wavelength Fresnel)
	Scalar chainThroughput = EvaluateChainThroughputNM(
		pos, lightSample.position, mResult.specularChain, nm );

	// Direction from last specular vertex to light (for cosine eval)
	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return result;
	dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

	Scalar cosAtLight;
	Scalar Le;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		if( lightSample.pLight ) {
			Le = ColorMath::Luminance(
				lightSample.pLight->emittedRadiance( dirSpecToLight ) );
		} else {
			Le = ColorMath::Luminance( lightSample.Le );
		}
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return result;
		Le = ColorMath::Luminance( lightSample.Le );
	}

	// SMS geometric factor (see RGB variant for derivation).
	const Scalar chainGeom = EvaluateChainGeometry(
		pos, lightSample.position, mResult.specularChain );

	const Scalar smsGeometric = cosAtLight * chainGeom
		/ fmax( mResult.jacobianDet, 1e-20 );
	const Scalar clampedGeometric = fmin( smsGeometric, config.maxGeometricTerm );

	result.contribution = fBSDF
		* chainThroughput
		* Le * cosAtShading * clampedGeometric
		/ (lightSample.pdfPosition * lightSample.pdfSelect);

	result.misWeight = 1.0 / mResult.pdf;
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// CheckChainVisibility
//
//   Tests whether the external segments of an SMS specular chain
//   are unoccluded.  Only the two external segments are tested:
//
//     1. Shading point  ->  first specular vertex
//     2. Last specular vertex  ->  light source
//
//   Inter-specular segments (through refractive geometry) are not
//   tested because CastShadowRay tests all objects, including the
//   glass surfaces themselves, which would always report a hit.
//   The external-segment checks catch the dominant occlusion cases
//   (opaque walls between receiver and glass, or between glass
//   and light).
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::CheckChainVisibility(
	const Point3& shadingPoint,
	const Point3& lightPoint,
	const std::vector<ManifoldVertex>& chain,
	const IRayCaster& caster
	) const
{
	if( chain.empty() ) return true;

	// Segment 1: shading point to first specular vertex
	//
	// The shading point sits on a non-specular surface.  The first
	// specular vertex is on the glass surface facing the shading
	// point.  We offset the ray endpoint by a small amount to
	// avoid hitting the glass surface at the target vertex.
	{
		const ManifoldVertex& vFirst = chain.front();

		// Determine the outward normal at the first vertex (pointing
		// toward the shading point, i.e. away from the glass interior)
		Vector3 dirToShading = Vector3Ops::mkVector3( shadingPoint, vFirst.position );
		const Scalar nDotDir = Vector3Ops::Dot( vFirst.normal, dirToShading );
		const Vector3 outwardN = (nDotDir >= 0)
			? vFirst.normal
			: Vector3( -vFirst.normal.x, -vFirst.normal.y, -vFirst.normal.z );

		// Biased endpoint: pull back from the glass surface along its
		// outward normal.  This prevents the shadow ray from hitting
		// the glass mesh at or near the target vertex.  Use a generous
		// bias to clear displacement bumps on the glass surface.
		const Scalar endBias = 5e-2;
		const Point3 biasedEnd = Point3Ops::mkPoint3(
			vFirst.position, outwardN * endBias );

		Vector3 dir = Vector3Ops::mkVector3( biasedEnd, shadingPoint );
		Scalar dist = Vector3Ops::NormalizeMag( dir );
		if( dist > 1e-4 )
		{
			Ray ray( shadingPoint, dir );
			ray.Advance( 1e-4 );
			if( caster.CastShadowRay( ray, dist - 2e-4 ) )
				return false;
		}
	}

	// Segment 2: last specular vertex to light source
	//
	// The last specular vertex sits on the glass surface facing the
	// light.  On a displaced mesh, nearby bumps can easily block a
	// ray launched with only a tiny directional bias.  We offset the
	// ray origin along the surface normal at the vertex to clear the
	// local surface geometry before testing for external occlusion.
	{
		const ManifoldVertex& vLast = chain.back();

		// Outward normal: pointing toward the light (away from glass)
		Vector3 dirToLight = Vector3Ops::mkVector3( lightPoint, vLast.position );
		const Scalar nDotDir = Vector3Ops::Dot( vLast.normal, dirToLight );
		const Vector3 outwardN = (nDotDir >= 0)
			? vLast.normal
			: Vector3( -vLast.normal.x, -vLast.normal.y, -vLast.normal.z );

		// Offset start along outward normal to clear displaced surface.
		// Use a generous bias for rough displacement.
		const Scalar normalBias = 5e-2;
		const Point3 biasedStart = Point3Ops::mkPoint3(
			vLast.position, outwardN * normalBias );

		Vector3 newDir = Vector3Ops::mkVector3( lightPoint, biasedStart );
		Scalar newDist = Vector3Ops::NormalizeMag( newDir );
		if( newDist > 1e-4 )
		{
			Ray ray( biasedStart, newDir );
			if( caster.CastShadowRay( ray, newDist - 1e-4 ) )
				return false;
		}
	}

	return true;
}
