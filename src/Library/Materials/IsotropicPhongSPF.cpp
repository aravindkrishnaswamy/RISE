//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongSPF.cpp - Implementation of the phong SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "IsotropicPhongSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Stratified-quadrature resolution PER AXIS for the diffuse lobe's
	//! selection coefficient C_D (see PhongDiffuseSelectCoefficient below).
	//! Same role and tuning rationale as SchlickSPF.cpp's kSpecQuadN --
	//! Scatter's accept boundary (the geomN gate) is the same kind of
	//! bounded-but-non-smooth integrand.  See
	//! docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 3.
	const int kPhongQuadN = 16;

	//! The specular lanes Scatter()/ScatterNM() would emit at this shading
	//! point: ONE in the ordinary case (and always in the spectral path),
	//! THREE when a per-channel exponent painter drives Scatter()'s
	//! per-channel branch.  `rs[i]` is lane i's specular reflectance
	//! (direction-INDEPENDENT); the direction-dependent part of the
	//! realized kray, `(N+2)/(N+1)*max(cos_o,0)`, is applied at the query
	//! direction by the functions below.
	struct PhongLobeSet
	{
		int    count;
		Scalar N[3];
		Scalar rs[3];
	};
}

//! Probability Scatter's diffuse ray survives its geometric-horizon gate:
//! the exact fraction of a cosine-weighted hemisphere about `n` that lies
//! above the plane of `geomN`.  Identical Malley-disk-projection closed
//! form to SchlickSPF.cpp's SchlickDiffuseAcceptFraction / DL-45's
//! TranslucentSPF derivation: (1 + dot(n,geomN))/2.
static inline Scalar PhongDiffuseAcceptFraction( const Vector3& n, const Vector3& geomN )
{
	const Scalar c = Vector3Ops::Dot( n, geomN );
	return r_max( Scalar(0), r_min( Scalar(1), 0.5 * (1.0 + c) ) );
}

//! C_D -- the coefficient the diffuse sampling density carries in the
//! aggregate density (see docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 2 for
//! the derivation, which mirrors DL-67 Slice 0's SchlickDiffuseSelectCoefficient).
//! `wD` is the query-direction-independent diffuse selection weight
//! (MaxValue(Rd) for RGB, the NM reflectance for spectral).
//!
//! kPhongQuadN^2 deterministic stratified replays of GenerateSpecularRay's
//! own (xi,b) unit square.  Unlike Schlick, Phong's azimuth warp
//! (around = TWO_PI*b) does not depend on the lobe's exponent N at all --
//! only the polar warp (down = acos(pow(xi,1/(N+1)))) does -- so the
//! (cosAround,sinAround) row is shared across every lane and only the
//! (cosDown,sinDown) rows are per-lane, computed once per call rather than
//! once per grid cell.
//!
//! `nu,nv,nw` / `gu,gv,gw` are `n` / `geomN` projected into the frame
//! GenerateSpecularRay's own Perturb() call builds internally around
//! `reflected` (U = normalize(reflected), V,W its perpendiculars) -- see
//! docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 1 for the closed-form
//! derivation of GeometricUtilities::Perturb's frame, which lets a lane's
//! emitted direction be written `cos(down)*U + sin(down)*sin(around)*V -
//! sin(down)*cos(around)*W` without replaying the rotation matrices.
static Scalar PhongDiffuseSelectCoefficient(
	const Scalar wD,
	const PhongLobeSet& lobes,
	const Scalar nu, const Scalar nv, const Scalar nw,
	const Scalar gu, const Scalar gv, const Scalar gw
	)
{
	Scalar cosDown[3][kPhongQuadN];
	Scalar sinDown[3][kPhongQuadN];
	Scalar cosAround[kPhongQuadN];
	Scalar sinAround[kPhongQuadN];

	const Scalar inv = 1.0 / Scalar(kPhongQuadN);
	for( int j = 0; j < lobes.count; j++ ) {
		const Scalar invExp = 1.0 / (lobes.N[j] + 1.0);
		for( int a = 0; a < kPhongQuadN; a++ ) {
			const Scalar xi = (Scalar(a) + 0.5) * inv;
			const Scalar c = r_min( Scalar(1), pow( xi, invExp ) );
			cosDown[j][a] = c;
			sinDown[j][a] = sqrt( r_max( Scalar(0), 1.0 - c*c ) );
		}
	}
	for( int b = 0; b < kPhongQuadN; b++ ) {
		const Scalar around = TWO_PI * (Scalar(b) + 0.5) * inv;
		cosAround[b] = cos(around);
		sinAround[b] = sin(around);
	}

	Scalar accum = 0;
	for( int a = 0; a < kPhongQuadN; a++ ) {
		for( int b = 0; b < kPhongQuadN; b++ ) {
			Scalar wS = 0;
			int nAccepted = 0;
			for( int j = 0; j < lobes.count; j++ ) {
				const Scalar ct = cosDown[j][a];
				const Scalar st = sinDown[j][a];
				const Scalar cosO   = ct*nu + st*sinAround[b]*nv - st*cosAround[b]*nw;
				const Scalar geomOk = ct*gu + st*sinAround[b]*gv - st*cosAround[b]*gw;
				if( geomOk <= 0 ) {
					// Scatter's own accept-check (dot(specular.ray.Dir(),geomN)>0)
					// would drop this lane from the container entirely.
					continue;
				}
				nAccepted++;
				wS += lobes.rs[j] * ((lobes.N[j]+2.0)/(lobes.N[j]+1.0)) * r_max(cosO, Scalar(0));
			}

			if( nAccepted == 0 ) {
				// RandomlySelect's freeidx==1 short-circuit: the lone
				// diffuse ray is returned whatever its weight.
				accum += 1.0;
				continue;
			}

			const Scalar total = wD + wS;
			if( total > NEARZERO ) {
				accum += wD / total;
			}
			// else RandomlySelect returns nothing at all -- contributes 0.
		}
	}

	return accum / Scalar(kPhongQuadN*kPhongQuadN);
}

//! sum_i q_i(wo) * p_i(wo) -- the specular half of the aggregate.  `aD` is
//! the probability Scatter's diffuse ray survives its own geometric-horizon
//! gate (1 whenever the shading and geometric normals agree).
//!
//! For the per-channel branch, lanes j != i share ONE random pair with lane
//! i, so a query wo hypothesized as lane i's own draw determines what the
//! OTHER lanes drew too.  Because the azimuth warp is exponent-independent
//! (see PhongDiffuseSelectCoefficient's comment), that shared azimuth is
//! recovered exactly from wo's own (V,W) projection with no inversion at
//! all, and the polar angle scales as a pure power law in cos(down) across
//! lanes (cos(down_j) = cos(down_i)^((N_i+1)/(N_j+1))) -- see
//! docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 1.
static Scalar PhongSpecularDensity(
	const Vector3& woNorm,
	const Vector3& U, const Vector3& V, const Vector3& W,
	const Scalar nu, const Scalar nv, const Scalar nw,
	const Scalar gu, const Scalar gv, const Scalar gw,
	const Scalar wD, const Scalar aD,
	const PhongLobeSet& lobes
	)
{
	const Scalar ctI = Vector3Ops::Dot( woNorm, U );
	if( ctI <= 0 ) {
		// wo is more than 90 degrees from `reflected` -- Perturb's polar
		// warp (down in [0, pi/2]) can never emit it from ANY lane.
		return 0;
	}
	const Scalar sy = Vector3Ops::Dot( woNorm, V );
	const Scalar sz = Vector3Ops::Dot( woNorm, W );
	const Scalar sinDownI = sqrt( r_max( Scalar(0), 1.0 - ctI*ctI ) );

	// cos_o = dot(wo, n) -- the shading-normal cosine Scatter's kray
	// actually uses (`ctI` above is cosAlpha = dot(wo, reflected), the
	// DIFFERENT quantity the phong lobe's density is built from).  Written
	// via the same U/V/W projection identity PhongDiffuseSelectCoefficient
	// and the sibling-reconstruction loop below use for cos_o_j, at
	// ctJ=ctI, scale=1 -- i.e. lane i is its own j==i case.
	const Scalar cosO_i = ctI*nu + sy*nv + sz*nw;

	Scalar sum = 0;

	for( int i = 0; i < lobes.count; i++ ) {
		// p_i(wo): the phong lobe density lane i's own sampler reports for
		// wo -- exactly IsotropicPhongSPF::Scatter's own specular.pdf
		// formula, evaluated at this direction instead of the sampled one.
		const Scalar pdf_i = (lobes.N[i] + 1.0) * INV_PI * 0.5 * pow( ctI, lobes.N[i] );
		if( pdf_i <= 0 ) {
			continue;
		}

		// Lane i's own realized weight, exact at wo: Pdf()'s caller has
		// already checked wo passes the geomN gate, so lane i is
		// unconditionally in the container.
		const Scalar w_i = lobes.rs[i] * ((lobes.N[i]+2.0)/(lobes.N[i]+1.0)) * r_max(cosO_i, Scalar(0));

		Scalar wOther = 0;
		int nAccepted = 1; // lane i itself

		if( lobes.count > 1 && sinDownI > NEARZERO ) {
			for( int j = 0; j < lobes.count; j++ ) {
				if( j == i ) {
					continue;
				}
				const Scalar ratio = (lobes.N[i] + 1.0) / (lobes.N[j] + 1.0);
				const Scalar ctJ = pow( ctI, ratio );
				const Scalar sinDownJ = sqrt( r_max( Scalar(0), 1.0 - ctJ*ctJ ) );
				const Scalar scale = sinDownJ / sinDownI;

				const Scalar cosO_j   = ctJ*nu + scale*(sy*nv + sz*nw);
				const Scalar geomOk_j = ctJ*gu + scale*(sy*gv + sz*gw);
				if( geomOk_j <= 0 ) {
					continue;
				}
				nAccepted++;
				wOther += lobes.rs[j] * ((lobes.N[j]+2.0)/(lobes.N[j]+1.0)) * r_max(cosO_j, Scalar(0));
			}
		}
		// sinDownI <= NEARZERO means wo sits essentially exactly at
		// `reflected` -- a measure-zero direction where the azimuth is
		// undefined; treat lane i as the sole occupant there rather than
		// dividing by a near-zero scale factor.

		const Scalar wS = w_i + wOther;

		Scalar q = 0;
		// ...with the diffuse ray present (probability aD).
		const Scalar totalWith = wD + wS;
		if( totalWith > NEARZERO ) {
			q += aD * (w_i / totalWith);
		}
		// ...and without it (probability 1-aD), where a lone specular ray
		// wins outright through RandomlySelect's freeidx==1 short-circuit.
		if( aD < 1.0 ) {
			if( nAccepted == 1 ) {
				q += (1.0 - aD);
			} else if( wS > NEARZERO ) {
				q += (1.0 - aD) * (w_i / wS);
			}
		}

		sum += q * pdf_i;
	}

	return sum;
}

IsotropicPhongSPF::IsotropicPhongSPF( const IPainter& Rd_, const IPainter& Rs_, const IScalarPainter& exp ) :
  pRd( &Rd_ ), pRs( &Rs_ ), pExponent( &exp )
{
	pRd->addref();
	pRs->addref();
	pExponent->addref();
}

IsotropicPhongSPF::~IsotropicPhongSPF( )
{
	safe_release( pRd );
	safe_release( pRs );
	safe_release( pExponent );
}

void IsotropicPhongSPF::SetRd( const IPainter& v )
{
	v.addref();
	safe_release( pRd );
	pRd = &v;
}

void IsotropicPhongSPF::SetRs( const IPainter& v )
{
	v.addref();
	safe_release( pRs );
	pRs = &v;
}

void IsotropicPhongSPF::SetExponent( const IScalarPainter& v )
{
	v.addref();
	safe_release( pExponent );
	pExponent = &v;
}

static void GenerateDiffuseRay(
	ScatteredRay& diffuse,
	const Scalar rdotn,											///< [in] Angle between view ray and normal
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand										///< [in] Random numbers
	)
{
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution.
	//
	// The predicate is `> 0`, matching EXACTLY how Scatter/ScatterNM/Pdf
	// build the lobe normal `n = rdotn > 0 ? -ri.onb.w() : ri.onb.w()`.
	// It used to be `> NEARZERO`, leaving a window rdotn in (0, 1e-12]
	// where `n` was flipped but the diffuse lobe was still sampled around
	// the UNflipped frame: the lobe then sat in the opposite hemisphere
	// from the one `n` names, its stored pdf (cos(dir,n)/pi, gated on
	// cos > 0) came out 0, and Pdf() -- which uses the same `n` -- could
	// not price it either, while its kray (a direction-independent
	// GetColor) stayed at full strength.
	if( rdotn > 0 )
	{
		OrthonormalBasis3D	myonb = ri.onb;
		myonb.FlipW();
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	} else {
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( ri.onb, ptrand ) );
	}
}

static void GenerateSpecularRay(
	ScatteredRay& specular,
	const Vector3& normal,										///< [in] Adjusted normal at surface
	const Vector3& reflected,									///< [in] Reflected ray at surface
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand,										///< [in] Random numbers
	const Scalar exponent
	)
{
	specular.type = ScatteredRay::eRayReflection;

	Vector3	rv = reflected;

	// Use the warping function to perturb the reflected ray using phong
	rv = GeometricUtilities::Perturb(rv,
        acos( pow(ptrand.x, 1.0 / (exponent+1.0)) ),
                 TWO_PI * ptrand.y);

	specular.ray.Set( ri.ptIntersection, rv );
}

void IsotropicPhongSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped lobe normal `n`
	// (built from `ri.onb.w()` below) stays in the shading frame so the
	// Phong lobe and cosine PDF remain BSDF-coupled.
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vGeomNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a direction that validates against
	// the (tilted) shading normal can still point below the geometric
	// surface -- the continuation ray then tunnels into the solid.  The
	// vGeomNormal use above only PICKS the face side; this gate REJECTS
	// below-horizon lobes.  Degenerate vGeomNormal (SquaredModulus guard,
	// matches GlintModifier.cpp) falls back to the shading normal, making
	// the gate a no-op.
	// (ray-anchor sweep: geomN's orientation is anchored to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot flip the gate to the wrong side.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const ScalarTriple Nt = pExponent->GetValuesAt(ri);
	const Scalar N[3] = { Nt.v[0], Nt.v[1], Nt.v[2] };

	ScatteredRay diffuse, specular;
	GenerateDiffuseRay( diffuse, rdotn, ri,  Point2( sampler.Get1D(), sampler.Get1D() ) );

	// Set PDF for diffuse ray: cosine-weighted hemisphere sampling
	{
		const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), n );
		diffuse.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		diffuse.isDelta = false;
	}

	if( !pExponent->HasPerChannelVariation() ) {
		GenerateSpecularRay( specular, n, reflected, ri,  Point2( sampler.Get1D(), sampler.Get1D() ), N[0] );

		// kray = BRDF * cos_o / pdf = Rs * (N+2)/(2*pi) * cos^N(alpha) * cos_o
		//        / [(N+1)/(2*pi) * cos^N(alpha)]
		//      = Rs * (N+2)/(N+1) * cos_o
		// This is bounded since cos_o ∈ [0,1] and (N+2)/(N+1) ∈ (1,2]
		const Scalar cos_o = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), n );
		specular.kray = pRs->GetColor(ri) * ((N[0]+2.0)/(N[0]+1.0)) * r_max(cos_o, 0.0);

		// PDF for phong lobe: (N+1)/(2*pi) * cos^N(alpha), alpha = angle from reflection direction
		const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
		if( cosAlpha > 0 ) {
			specular.pdf = (N[0] + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N[0] );
		} else {
			specular.pdf = 0;
		}
		specular.isDelta = false;

		if( Vector3Ops::Dot( specular.ray.Dir(), geomN ) > 0 ) {
			scattered.AddScatteredRay( specular );
		}
	} else {
		const RISEPel spec = pRs->GetColor(ri);
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		for( int i=0; i<3; i++ ) {
			GenerateSpecularRay( specular, n, reflected, ri,  ptrand, N[i] );

			const Scalar cos_o = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), n );
			specular.kray = 0.0;
			specular.kray[i] = spec[i] * ((N[i]+2.0)/(N[i]+1.0)) * r_max(cos_o, 0.0);

			// PDF for phong lobe per channel
			const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
			if( cosAlpha > 0 ) {
				specular.pdf = (N[i] + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N[i] );
			} else {
				specular.pdf = 0;
			}
			specular.isDelta = false;

			if( Vector3Ops::Dot( specular.ray.Dir(), geomN ) > 0 ) {
				scattered.AddScatteredRay( specular );
			}
		}
	}

	diffuse.kray = pRd->GetColor(ri);
	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( diffuse );
	}
}


void IsotropicPhongSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped lobe normal `n`
	// (built from `ri.onb.w()` below) stays in the shading frame so the
	// Phong lobe and cosine PDF remain BSDF-coupled.
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vGeomNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a direction that validates against
	// the (tilted) shading normal can still point below the geometric
	// surface -- the continuation ray then tunnels into the solid.  The
	// vGeomNormal use above only PICKS the face side; this gate REJECTS
	// below-horizon lobes.  Degenerate vGeomNormal (SquaredModulus guard,
	// matches GlintModifier.cpp) falls back to the shading normal, making
	// the gate a no-op.
	// (ray-anchor sweep: geomN's orientation is anchored to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot flip the gate to the wrong side.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	ScatteredRay diffuse, specular;
	const Scalar N = pExponent->GetValueAtNM(ri,nm);
	GenerateDiffuseRay( diffuse, rdotn, ri,  Point2( sampler.Get1D(), sampler.Get1D() ) );
	GenerateSpecularRay( specular, n, reflected, ri,  Point2( sampler.Get1D(), sampler.Get1D() ),  N );

	diffuse.krayNM = GuardedGetColorNM( *pRd, ri, nm );
	{
		const Scalar cos_o = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), n );
		specular.krayNM = GuardedGetColorNM( *pRs, ri, nm ) * ((N+2.0)/(N+1.0)) * r_max(cos_o, 0.0);
	}

	// Set PDF for diffuse ray
	{
		const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), n );
		diffuse.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		diffuse.isDelta = false;
	}

	// Set PDF for specular ray
	{
		const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
		if( cosAlpha > 0 ) {
			specular.pdf = (N + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N );
		} else {
			specular.pdf = 0;
		}
		specular.isDelta = false;
	}

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( diffuse );
	}
	if( Vector3Ops::Dot( specular.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( specular );
	}
}

Scalar IsotropicPhongSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped lobe normal `n`
	// (built from `ri.onb.w()` below) stays in the shading frame so the
	// Phong lobe and cosine PDF remain BSDF-coupled.
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vGeomNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Geometric-horizon gate (MIS consistency with Scatter's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}

	// Diffuse component: cosine-weighted hemisphere about `n`.  Direction-
	// independent kray (pRd->GetColor(ri)), exactly as SchlickSPF's diffuse
	// lobe -- no averaging approximation needed on this side (DL-67 Slice 0
	// section 1).
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	const Scalar diffusePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	const RISEPel rd = pRd->GetColor(ri);
	const RISEPel rs = pRs->GetColor(ri);
	const Scalar wD = ColorMath::MaxValue(rd);

	// The lanes Scatter() would have emitted.  Branch on the SAME predicate
	// Scatter() branches on, and use each lane's own N -- a lane's sampling
	// density and realized weight have to be those of what that lane
	// actually draws, not a channel average (DL-98).
	const ScalarTriple Nt = pExponent->GetValuesAt(ri);
	PhongLobeSet lobes;
	if( !pExponent->HasPerChannelVariation() ) {
		lobes.count = 1;
		lobes.N[0]  = Nt.v[0];
		lobes.rs[0] = ColorMath::MaxValue(rs);
	} else {
		lobes.count = 3;
		for( int i = 0; i < 3; i++ ) {
			lobes.N[i]  = Nt.v[i];
			lobes.rs[i] = rs[i];
		}
	}

	// Frame GenerateSpecularRay's Perturb() call builds internally around
	// `reflected` -- see docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 1.
	OrthonormalBasis3D uvw;
	uvw.CreateFromU( reflected );
	const Vector3& U = uvw.u();
	const Vector3& V = uvw.v();
	const Vector3& W = uvw.w();
	const Scalar nu = Vector3Ops::Dot(n,U),      nv = Vector3Ops::Dot(n,V),      nw = Vector3Ops::Dot(n,W);
	const Scalar gu = Vector3Ops::Dot(geomN,U),  gv = Vector3Ops::Dot(geomN,V),  gw = Vector3Ops::Dot(geomN,W);

	const Scalar aD = PhongDiffuseAcceptFraction( n, geomN );
	const Scalar cD = PhongDiffuseSelectCoefficient( wD, lobes, nu,nv,nw, gu,gv,gw );

	return cD * diffusePdf
	     + PhongSpecularDensity( woNorm, U,V,W, nu,nv,nw, gu,gv,gw, wD, aD, lobes );
}

Scalar IsotropicPhongSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped lobe normal `n`
	// (built from `ri.onb.w()` below) stays in the shading frame so the
	// Phong lobe and cosine PDF remain BSDF-coupled.
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vGeomNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Geometric-horizon gate (MIS consistency with ScatterNM's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}

	// Diffuse component
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	const Scalar diffusePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	const Scalar wD = GuardedGetColorNM( *pRd, ri, nm );

	// ScatterNM has no per-channel branch, so there is always exactly one
	// specular lane.
	PhongLobeSet lobes;
	lobes.count  = 1;
	lobes.N[0]   = pExponent->GetValueAtNM(ri,nm);
	lobes.rs[0]  = GuardedGetColorNM( *pRs, ri, nm );

	OrthonormalBasis3D uvw;
	uvw.CreateFromU( reflected );
	const Vector3& U = uvw.u();
	const Vector3& V = uvw.v();
	const Vector3& W = uvw.w();
	const Scalar nu = Vector3Ops::Dot(n,U),      nv = Vector3Ops::Dot(n,V),      nw = Vector3Ops::Dot(n,W);
	const Scalar gu = Vector3Ops::Dot(geomN,U),  gv = Vector3Ops::Dot(geomN,V),  gw = Vector3Ops::Dot(geomN,W);

	const Scalar aD = PhongDiffuseAcceptFraction( n, geomN );
	const Scalar cD = PhongDiffuseSelectCoefficient( wD, lobes, nu,nv,nw, gu,gv,gw );

	return cD * diffusePdf
	     + PhongSpecularDensity( woNorm, U,V,W, nu,nv,nw, gu,gv,gw, wD, aD, lobes );
}
