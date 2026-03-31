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
	pLightSampler->addref();
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

// Helper: derivative of a normalized vector.
// d/dx [v/|v|] = (I - v_hat * v_hat^T) * dv/dx / |v|
// Given h = v/|v|, dv, return the derivative of h w.r.t. the
// parameter that produced dv.
static Vector3 DeriveNormalized( const Vector3& h, const Vector3& dv, Scalar vLen )
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
		Scalar sign;
		if( v.isReflection )
		{
			h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
			sign = 1.0;
		}
		else
		{
			h_raw = Vector3(
				-(wi.x + eta_eff * wo.x),
				-(wi.y + eta_eff * wo.y),
				-(wi.z + eta_eff * wo.z) );
			sign = -1.0;
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

// Helper: invert a 2x2 block stored as [a,b,c,d] row-major
static bool Invert2x2( const Scalar* m, Scalar* inv )
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

// Helper: multiply 2x2 block A * B -> C  (all row-major)
static void Mul2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0]*B[0] + A[1]*B[2];
	C[1] = A[0]*B[1] + A[1]*B[3];
	C[2] = A[2]*B[0] + A[3]*B[2];
	C[3] = A[2]*B[1] + A[3]*B[3];
}

// Helper: multiply 2x2 block A * 2-vector v -> result 2-vector
static void Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r )
{
	r[0] = A[0]*v[0] + A[1]*v[1];
	r[1] = A[2]*v[0] + A[3]*v[1];
}

// Helper: subtract 2x2 blocks: C = A - B
static void Sub2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0] - B[0];
	C[1] = A[1] - B[1];
	C[2] = A[2] - B[2];
	C[3] = A[3] - B[3];
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

	// For small steps, skip the expensive ray-cast re-snap.
	// The linear approximation is accurate to second order and the
	// tangent-frame re-orthogonalization keeps the constraint well-posed.
	const Scalar stepSize = sqrt( du * du + dv * dv );
	if( stepSize < 0.1 )
	{
		vertex.position = newPos;
		vertex.normal = newNormal;

		// Re-orthogonalize tangent frame against perturbed normal
		Scalar d_u = Vector3Ops::Dot( vertex.dpdu, newNormal );
		vertex.dpdu = Vector3(
			vertex.dpdu.x - newNormal.x * d_u,
			vertex.dpdu.y - newNormal.y * d_u,
			vertex.dpdu.z - newNormal.z * d_u );
		Scalar d_v = Vector3Ops::Dot( vertex.dpdv, newNormal );
		vertex.dpdv = Vector3(
			vertex.dpdv.x - newNormal.x * d_v,
			vertex.dpdv.y - newNormal.y * d_v,
			vertex.dpdv.z - newNormal.z * d_v );

		vertex.valid = true;
		return true;
	}

	// For larger steps, project back onto the surface via ray intersection
	const Scalar probeOffset = 0.5;
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

	// Compute derivatives numerically via finite differences using
	// ray-surface intersection.  This is robust and works regardless
	// of whether the geometry provides analytical derivatives.
	const Scalar eps = 1e-5;

	Vector3 tangent_u, tangent_v;

	if( Vector3Ops::SquaredModulus( vertex.dpdu ) > NEARZERO &&
		Vector3Ops::SquaredModulus( vertex.dpdv ) > NEARZERO )
	{
		tangent_u = Vector3Ops::Normalize( vertex.dpdu );
		tangent_v = Vector3Ops::Normalize( vertex.dpdv );
	}
	else
	{
		// Build a tangent frame from the normal
		tangent_u = Vector3Ops::Perpendicular( vertex.normal );
		tangent_u = Vector3Ops::Normalize( tangent_u );
		tangent_v = Vector3Ops::Cross( vertex.normal, tangent_u );
		tangent_v = Vector3Ops::Normalize( tangent_v );
	}

	// Probe function: perturb position along a direction and re-intersect
	// to find the actual surface point and normal
	Point3 pos_u_plus, pos_v_plus;
	Vector3 norm_u_plus, norm_v_plus;
	bool ok_u = false, ok_v = false;

	// Helper lambda-like blocks for probing in each direction
	for( int axis = 0; axis < 2; axis++ )
	{
		const Vector3& tangent = (axis == 0) ? tangent_u : tangent_v;
		const Point3 testPos = Point3Ops::mkPoint3( vertex.position, tangent * eps );

		const Scalar probeOffset = 1e-4;

		// Probe from above
		{
			const Ray probeRay(
				Point3Ops::mkPoint3( testPos, vertex.normal * probeOffset ),
				Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z )
				);

			RayIntersection ri( probeRay, nullRasterizerState );
			vertex.pObject->IntersectRay( ri, RISE_INFINITY, true, true, false );

			if( ri.geometric.bHit )
			{
				if( axis == 0 ) { pos_u_plus = ri.geometric.ptIntersection; norm_u_plus = ri.geometric.vNormal; ok_u = true; }
				else            { pos_v_plus = ri.geometric.ptIntersection; norm_v_plus = ri.geometric.vNormal; ok_v = true; }
				continue;
			}
		}

		// Try from below
		{
			const Ray probeRay2(
				Point3Ops::mkPoint3( testPos, vertex.normal * (-probeOffset) ),
				vertex.normal
				);

			RayIntersection ri2( probeRay2, nullRasterizerState );
			vertex.pObject->IntersectRay( ri2, RISE_INFINITY, true, true, false );

			if( ri2.geometric.bHit )
			{
				if( axis == 0 ) { pos_u_plus = ri2.geometric.ptIntersection; norm_u_plus = ri2.geometric.vNormal; ok_u = true; }
				else            { pos_v_plus = ri2.geometric.ptIntersection; norm_v_plus = ri2.geometric.vNormal; ok_v = true; }
			}
		}
	}

	if( !ok_u || !ok_v )
	{
		// Set reasonable defaults
		vertex.dpdu = tangent_u;
		vertex.dpdv = tangent_v;
		vertex.dndu = Vector3( 0, 0, 0 );
		vertex.dndv = Vector3( 0, 0, 0 );
		return true;
	}

	// dpdu = d(position)/du ~ (pos_u_plus - position) / eps
	vertex.dpdu = Vector3(
		(pos_u_plus.x - vertex.position.x) / eps,
		(pos_u_plus.y - vertex.position.y) / eps,
		(pos_u_plus.z - vertex.position.z) / eps
		);

	vertex.dpdv = Vector3(
		(pos_v_plus.x - vertex.position.x) / eps,
		(pos_v_plus.y - vertex.position.y) / eps,
		(pos_v_plus.z - vertex.position.z) / eps
		);

	// dndu = d(normal)/du ~ (norm_u_plus - normal) / eps
	vertex.dndu = Vector3(
		(norm_u_plus.x - vertex.normal.x) / eps,
		(norm_u_plus.y - vertex.normal.y) / eps,
		(norm_u_plus.z - vertex.normal.z) / eps
		);

	vertex.dndv = Vector3(
		(norm_v_plus.x - vertex.normal.x) / eps,
		(norm_v_plus.y - vertex.normal.y) / eps,
		(norm_v_plus.z - vertex.normal.z) / eps
		);

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

		// Build Jacobian
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

		for( unsigned int attempt = 0; attempt < 5; attempt++ )
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

	return false;  // Did not converge within maxIterations
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

		// Compute incoming and outgoing directions at this vertex
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? endPoint : chain[i+1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		const Scalar cos_i = fabs( Vector3Ops::Dot( wi, v.normal ) );
		const Scalar r0 = ((v.eta - 1.0) * (v.eta - 1.0)) / ((v.eta + 1.0) * (v.eta + 1.0));
		const Scalar fr = r0 + (1.0 - r0) * pow( 1.0 - cos_i, 5.0 );

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

		const Scalar cos_i = fabs( Vector3Ops::Dot( wi, v.normal ) );
		const Scalar r0 = ((v.eta - 1.0) * (v.eta - 1.0)) / ((v.eta + 1.0) * (v.eta + 1.0));
		const Scalar fr = r0 + (1.0 - r0) * pow( 1.0 - cos_i, 5.0 );

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

	// Build the Jacobian to get the diagonal blocks
	std::vector<Scalar> diag, upper_blocks, lower_blocks;
	BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks );

	// Compute the determinant of the block-tridiagonal matrix via LU factorization
	// The determinant is the product of the diagonal block determinants after elimination

	std::vector<Scalar> Dp( k * 4 );

	Scalar detProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			for( int q = 0; q < 4; q++ ) {
				Dp[q] = diag[q];
			}
		}
		else
		{
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return 0.0;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower_blocks[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper_blocks[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );
		}

		// Accumulate determinant: det = product of det(Dp[i])
		const Scalar blkDet = Dp[i*4+0] * Dp[i*4+3] - Dp[i*4+1] * Dp[i*4+2];
		detProduct *= blkDet;
	}

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

	Scalar geoTerm = fabs( detProduct );
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
			return result;  // Seed too far from valid path
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
		result.valid = true;
		result.specularChain = specularChain;

		// Evaluate throughput (Fresnel * Beer's law along the chain)
		result.contribution = EvaluateChainThroughput( shadingPoint, emitterPoint, specularChain );

		// For the deterministic seed approach (straight-line trace + Newton),
		// the geometric coupling between endpoints is handled by the standard
		// geometric term G(eye, first_spec) in the integrator's contribution
		// formula.  The specular chain's internal geometric terms cancel with
		// the delta BSDF PDFs (Veach's formulation for specular vertices).
		// We only need the Fresnel throughput, not a separate manifold
		// geometric term.
		//
		// The manifold geometric term (Jacobian determinant) is needed for
		// random seed approaches where we sample seed positions on the
		// specular surfaces — it converts from seed area measure to path
		// measure.  For deterministic seeds, it is not applicable.

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

	// Sample a light
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Build seed chain
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSeedChain(
		pos, lightSample.position,
		scene, caster, seedChain );

	if( chainLen == 0 || seedChain.empty() )
		return result;

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
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
		mResult.specularChain[0].position, pos );
	Scalar distToSpec = Vector3Ops::Magnitude( dirToFirstSpec );
	if( distToSpec < 1e-8 ) return result;
	dirToFirstSpec = dirToFirstSpec * (1.0 / distToSpec);

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

	// Cosine at shading point (from rendering equation)
	const Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return result;

	// Cosine at light surface and distance from last specular vertex
	// to light (for area-to-solid-angle conversion at the light end)
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

	// SMS contribution via standard Monte Carlo estimator:
	//   f(x) / p(x)  where we sampled one light with probability pdfSelect.
	//
	// pdfPosition = 1/Area for mesh lights, so 1/pdfPosition = Area.
	// Dividing by pdfSelect is required because we sample ONE light
	// from the full set — without it the expected contribution is
	// attenuated by pdfSelect, causing systematic underestimation
	// in multi-light scenes.  (Single-light scenes are unaffected
	// since pdfSelect = 1.)  This matches the NM (spectral) SMS path
	// in BDPTIntegrator::EvaluateSMSStrategiesNM.
	//
	// No intermediate geometric terms — delta BSDFs at specular vertices
	// cancel with the geometric terms in the path integral.
	const Scalar lightGeom = cosAtLight / (distSpecToLight * distSpecToLight);

	result.contribution = fBSDF * cosAtShading
		* mResult.contribution
		* actualLe * lightGeom
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

	// Sample a light
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Build seed chain (uses RGB IOR for approximate positions)
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSeedChain(
		pos, lightSample.position,
		scene, caster, seedChain );

	if( chainLen == 0 || seedChain.empty() )
		return result;

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
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
		mResult.specularChain[0].position, pos );
	Scalar distToSpec = Vector3Ops::Magnitude( dirToFirstSpec );
	if( distToSpec < 1e-8 ) return result;
	dirToFirstSpec = dirToFirstSpec * (1.0 / distToSpec);

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

	Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return result;

	// Chain throughput (spectral — per-wavelength Fresnel)
	Scalar chainThroughput = EvaluateChainThroughputNM(
		pos, lightSample.position, mResult.specularChain, nm );

	// Cosine at light surface
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

	const Scalar lightGeom = cosAtLight / (distSpecToLight * distSpecToLight);

	result.contribution = fBSDF * cosAtShading
		* chainThroughput
		* Le * lightGeom
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
	{
		Vector3 dir = Vector3Ops::mkVector3(
			chain.front().position, shadingPoint );
		Scalar dist = Vector3Ops::NormalizeMag( dir );
		if( dist > 1e-6 )
		{
			Ray ray( shadingPoint, dir );
			ray.Advance( 1e-6 );
			if( caster.CastShadowRay( ray, dist - 2e-6 ) )
				return false;
		}
	}

	// Segment 2: last specular vertex to light
	{
		Vector3 dir = Vector3Ops::mkVector3(
			lightPoint, chain.back().position );
		Scalar dist = Vector3Ops::NormalizeMag( dir );
		if( dist > 1e-6 )
		{
			Ray ray( chain.back().position, dir );
			ray.Advance( 1e-6 );
			if( caster.CastShadowRay( ray, dist - 2e-6 ) )
				return false;
		}
	}

	return true;
}
