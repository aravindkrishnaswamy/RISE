//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.cpp - Implementation of the translucent
//  SPF
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
#include "TranslucentSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;
// Shared helpers (OrientedLobeAxis, ExitValidFraction, kExitVanishThreshold)
// live in TranslucentSPF.h so TranslucentBSDF uses the exact same clipping math.
using namespace RISE::Implementation::TranslucentSPFDetail;

namespace
{
	// Re-orient shading normal `n` into the outward `geomNRaw` hemisphere
	// before clipping: double-sided meshes flip `vNormal` toward the incoming
	// ray, which points inward on an exit hit.
	inline Vector3 OrientedExitNormal( const Vector3& n, const Vector3& geomNRaw )
	{
		return ( Vector3Ops::Dot( n, geomNRaw ) < Scalar(0) ) ? -n : n;
	}

	// Exact unconditional 2-draw cosine sampler about `n`, conditioned on the
	// geometric half-space dot(wo, geomN) > 0 (used for both diffuse exit and
	// entry front reflection).  Projects onto the Malley disk (r = sqrt(1-u1),
	// matching GeometricUtilities::Perturb's cos(theta) = sqrt(u1) convention)
	// and affinely remaps each x-chord into [-cos(phi)*R, R], giving density
	//   pdf = (cos(theta) / pi) / P(valid),  P(valid) = (1 + cos(phi)) / 2.
	// Always consumes 2 draws (no rejection loop) to preserve fixed Sobol
	// dimension budgets; returns false only if P(valid) < kExitVanishThreshold.
	bool SampleValidDiffuseExit( const Vector3& n, const Vector3& geomN,
		ISampler& sampler, Vector3& outDir, Scalar& outPdf )
	{
		const Scalar cosPhi = r_max( Scalar(-1), r_min( Scalar(1), Vector3Ops::Dot(n,geomN) ) );
		const Scalar pValid = (Scalar(1)+cosPhi) * Scalar(0.5);

		// Consume 2 draws unconditionally so dimension offsets stay fixed.
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();

		if( pValid < kExitVanishThreshold ) {
			return false;
		}

		// Local frame (u, v, n) with geomN in the n-u plane.
		Vector3 u = geomN - cosPhi * n;
		const Scalar uLen2 = Vector3Ops::SquaredModulus( u );
		if( uLen2 > Scalar(1e-12) ) {
			u = u * ( Scalar(1) / sqrt(uLen2) );
		} else {
			OrthonormalBasis3D arbitrary;
			arbitrary.CreateFromW( n );
			u = arbitrary.u();
		}
		const Vector3 v = Vector3Ops::Cross( n, u );

		const Scalar r = sqrt( r_max( Scalar(0), Scalar(1) - u1 ) );
		const Scalar psi = TWO_PI * u2;
		const Scalar x = r * cos(psi);
		const Scalar y = r * sin(psi);
		const Scalar R = sqrt( r_max( Scalar(0), Scalar(1) - y*y ) );

		const Scalar xPrime = (x + R) * (Scalar(1)+cosPhi) * Scalar(0.5) - cosPhi * R;
		const Scalar zPrime = sqrt( r_max( Scalar(0), Scalar(1) - xPrime*xPrime - y*y ) );

		outDir = u*xPrime + v*y + n*zPrime;
		outPdf = ( zPrime * INV_PI ) / pValid;
		return true;
	}
}

namespace RISE { namespace Implementation { namespace TranslucentSPFDetail
{
	// Exact 2-draw Phong cos^N sampler about `axis`, clipped per theta-ring to
	// dot(w, clipN) > 0: cos(theta) = u1^(1/(N+1)), azimuth uniform on
	// [-half, half] with cos(psi) > -cot(theta)*cot(phi), giving
	//   q(w) = (N+1) * cos^N(theta) / (2 * half).
	// Renormalizes direction density only (f = kray*q/|cos|, integral(q) = 1;
	// do NOT also scale kray by half/PI).  Precondition: Dot(axis, clipN) >= 0
	// (via OrientedLobeAxis); fails loudly returning false if violated.
	// Note: GeometricUtilities::PerturbClipped carries the angle-parameterized
	// twin of this arc math — keep changes synchronized in both.
	bool SampleClippedPhong(
		const Vector3& axis, const Vector3& clipN, const Scalar N,
		const Scalar u1, const Scalar u2,
		Vector3& outDir, Scalar& outPdf )
	{
		const Scalar cosPhiRaw = Vector3Ops::Dot(axis,clipN);
		if( cosPhiRaw < Scalar(0) ) {
			GlobalLog()->PrintEasyError(
				"TranslucentSPF::SampleClippedPhong:: precondition violated -- "
				"axis is not oriented into the clip half-space (Dot(axis,clipN) < 0); "
				"refusing to sample rather than emit a non-unit direction." );
			outDir = axis;
			outPdf = 0;
			return false;
		}
		const Scalar cosPhi = r_min( Scalar(1), cosPhiRaw );
		const Scalar cosTheta = pow( u1, Scalar(1)/(N+Scalar(1)) );

		Vector3 uAxis = clipN - cosPhi*axis;
		const Scalar uLen2 = Vector3Ops::SquaredModulus( uAxis );
		if( uLen2 <= Scalar(1e-12) ) {
			// Untilted fast path: preserves exact Perturb azimuth convention.
			outDir = GeometricUtilities::Perturb( axis, acos(cosTheta), TWO_PI * u2 );
			outPdf = (N + Scalar(1)) * Scalar(0.5) * INV_PI
				* pow( fabs( Vector3Ops::Dot( outDir, axis ) ), N );
			return true;
		}
		uAxis = uAxis * ( Scalar(1) / sqrt(uLen2) );
		const Vector3 vAxis = Vector3Ops::Cross( axis, uAxis );

		const Scalar sinPhi = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );
		const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1) - cosTheta*cosTheta ) );

		// Ratio form avoids cot(theta) -> inf as theta -> 0.
		Scalar half = PI;
		const Scalar denom = sinTheta * sinPhi;
		const Scalar num = cosTheta * cosPhi;
		if( num < denom ) {
			half = acos( -num/denom );
		}

		const Scalar psi = ( Scalar(2)*u2 - Scalar(1) ) * half;
		outDir = uAxis*(sinTheta*cos(psi)) + vAxis*(sinTheta*sin(psi)) + axis*cosTheta;
		outPdf = (N + Scalar(1)) * pow( cosTheta, N ) / ( Scalar(2) * half );
		return true;
	}

	// Shared per-hit lobe evaluator for Pdf/PdfNM and TranslucentBSDF::value/valueNM
	// (f = kray * pdf / |cos|).  Cancels cosTheta analytically to avoid grazing 0/0.
	bool EvalLobe( const Lobe& lobe, const Vector3& w,
		Scalar& outPdf, Scalar& outFOverKray )
	{
		const Scalar cosTheta = Vector3Ops::Dot( w, lobe.axis );
		if( cosTheta <= 0 ) return false;
		if( Vector3Ops::Dot( w, lobe.clipN ) <= 0 ) return false;

		if( !lobe.isPhong ) {
			const Scalar pValid = ExitValidFraction( lobe.axis, lobe.clipN );
			if( pValid < kExitVanishThreshold ) return false;
			outFOverKray = INV_PI / pValid;
			outPdf       = cosTheta * outFOverKray;
			return true;
		}

		const Scalar cosPhi = r_max( Scalar(0), r_min( Scalar(1),
			Vector3Ops::Dot( lobe.axis, lobe.clipN ) ) );
		Scalar half = PI;
		{
			const Vector3 uAxis = lobe.clipN - cosPhi*lobe.axis;
			if( Vector3Ops::SquaredModulus( uAxis ) > Scalar(1e-12) ) {
				const Scalar sinPhi   = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );
				const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1) - cosTheta*cosTheta ) );
				const Scalar denom = sinTheta * sinPhi;
				const Scalar num   = cosTheta * cosPhi;
				if( num < denom ) {
					half = acos( -num/denom );
				}
			}
		}
		outFOverKray = (lobe.N + Scalar(1)) * pow( cosTheta, lobe.N - Scalar(1) ) / ( Scalar(2)*half );
		outPdf       = cosTheta * outFOverKray;
		return true;
	}

	namespace
	{
		inline void AddLobe( LobeSet& set, bool isPhong, const Vector3& axis,
			const Vector3& clipN, Scalar N, const RISEPel& kray, Scalar krayNM, bool bNM )
		{
			if( set.count >= 4 ) return;
			Lobe& L = set.lobes[set.count++];
			L.isPhong = isPhong;
			L.axis    = axis;
			L.clipN   = clipN;
			L.N       = N;
			L.kray    = kray;
			L.krayNM  = krayNM;
			// Match ScatteredRayContainer::RandomlySelect's weight reduction.
			L.selectWeight = bNM ? krayNM : ColorMath::MaxValue( kray );
			set.totalSelectWeight += L.selectWeight;
		}
	}

	void BuildLobeSet(
		const IPainter& refFront,
		const IPainter& trans,
		const IScalarPainter& extinction,
		const IScalarPainter& phongN,
		const IScalarPainter& scattering,
		const RayIntersectionGeometric& ri,
		const IORStack* pIorStack,
		const bool bNM,
		const Scalar nm,
		LobeSet& out )
	{
		out.count = 0;
		out.totalSelectWeight = 0;

		const Vector3 n = ri.onb.w();
		out.n = n;
		const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3 geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
			? trueGeomNormal : n;

		// Side test: live IOR stack is authoritative.  When stackless, use
		// `ri.bProvablyNoInterior` (NEVER `ri.bOpenSheet`, which only means
		// uncertified watertightness on meshes) or ray-vs-geomNRaw dot product.
		const bool bEntering = pIorStack
			? !pIorStack->containsCurrent()
			: ( ri.bProvablyNoInterior || Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 );

		// Entry lobes clip against ray-anchored geomN (handles back-face hits on
		// open double-sided sheets).  Exit uses ray-independent -geomNRaw so
		// BDPT light-subpath rebuilt records stay consistent.
		const Vector3 geomN = bEntering
			? ( ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw )
			: -geomNRaw;

		if( bEntering )
		{
			// Entry front reflection (clipped cosine, kray = ref)
			const RISEPel refC = bNM ? RISEPel(0,0,0) : ReflectanceColor( refFront, ri );
			const Scalar  refN = bNM ? ReflectanceColorNM( refFront, ri, nm ) : Scalar(0);
			if( bNM ? (refN > 0) : (ColorMath::MaxValue(refC) > 0) ) {
				const Vector3 nFront = OrientedLobeAxis( n, geomN );
				if( ExitValidFraction( nFront, geomN ) >= kExitVanishThreshold ) {
					AddLobe( out, false, nFront, geomN, Scalar(1), refC, refN, bNM );
				}
			}

			// Entry transmission (clipped Phong, kray = tau)
			const RISEPel tauC = bNM ? RISEPel(0,0,0) : ReflectanceColor( trans, ri );
			const Scalar  tauN = bNM ? ReflectanceColorNM( trans, ri, nm ) : Scalar(0);
			if( bNM ? (tauN > 0) : (ColorMath::MaxValue(tauC) > 0) ) {
				const Vector3 intoSolid = -geomN;
				const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );
				if( bNM ) {
					AddLobe( out, true, nEnter, intoSolid, phongN.GetValueAtNM(ri,nm),
						RISEPel(0,0,0), tauN, bNM );
				} else {
					const ScalarTriple Nt = phongN.GetValuesAt(ri);
					if( (Nt.v[0] == Nt.v[1]) && (Nt.v[1] == Nt.v[2]) ) {
						AddLobe( out, true, nEnter, intoSolid, Nt.v[0], tauC, 0, bNM );
					} else {
						for( int i = 0; i < 3; i++ ) {
							RISEPel chan( 0, 0, 0 );
							chan[i] = tauC[i];
							AddLobe( out, true, nEnter, intoSolid, Nt.v[i], chan, 0, bNM );
						}
					}
				}
			}
			return;
		}

		// Exit side: Beer extinction over interior segment (distance == 0 on rebuilt BDPT/VCM records).
		const Scalar distance = Vector3Ops::Magnitude(
			Vector3Ops::mkVector3( ri.ray.origin, ri.ptIntersection ) );

		RISEPel B( 1, 1, 1 );
		Scalar  Bnm = 1;
		if( bNM ) {
			Bnm = exp( -( extinction.GetValueAtNM(ri,nm) * distance ) );
		} else {
			const ScalarTriple abt = extinction.GetValuesAt(ri);
			const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
			B = ColorMath::exponential( -distance*ab );
		}

		RISEPel exitKray  = B;
		Scalar  exitKrayNM = Bnm;

		if( bNM ? (Bnm > 0) : (ColorMath::MaxValue(B) > 0) )
		{
			const Vector3 stayInside = geomN;
			const Vector3 nBack = OrientedLobeAxis( n, stayInside );

			if( bNM ) {
				const Scalar scat = scattering.GetValueAtNM(ri,nm);
				if( scat > 0 ) {
					AddLobe( out, true, nBack, stayInside, phongN.GetValueAtNM(ri,nm),
						RISEPel(0,0,0), Bnm*scat, bNM );
					exitKrayNM = Bnm * (Scalar(1)-scat);
				}
			} else {
				const ScalarTriple scat_t = scattering.GetValuesAt(ri);
				const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );
				if( ColorMath::MaxValue(scat) > 0 ) {
					const ScalarTriple Nt = phongN.GetValuesAt(ri);
					if( (Nt.v[0] == Nt.v[1]) && (Nt.v[1] == Nt.v[2]) ) {
						AddLobe( out, true, nBack, stayInside, Nt.v[0], B*scat, 0, bNM );
						exitKray = B * (RISEPel(1.0,1.0,1.0)-scat);
					} else {
						for( int i = 0; i < 3; i++ ) {
							RISEPel chan( 0, 0, 0 );
							chan[i] = B[i]*scat[i];
							AddLobe( out, true, nBack, stayInside, Nt.v[i], chan, 0, bNM );
							exitKray[i] = B[i] * (Scalar(1)-scat[i]);
						}
					}
				}
			}
		}

		// Diffuse exit lobe emitted unconditionally (even if zero-weight), matching Scatter().
		{
			const Vector3 nExit = OrientedExitNormal( n, geomNRaw );
			if( ExitValidFraction( nExit, geomNRaw ) >= kExitVanishThreshold ) {
				AddLobe( out, false, nExit, geomNRaw, Scalar(1), exitKray, exitKrayNM, bNM );
			}
		}
	}
} } }

using namespace RISE::Implementation::TranslucentSPFDetail;

TranslucentSPF::TranslucentSPF(
	const IPainter& rF,
	const IPainter& T,
	const IScalarPainter& ext,
	const IScalarPainter& N_,
	const IScalarPainter& scat
	) :
  pRefFront( &rF ),
  pTrans( &T ),
  pExtinction( &ext ),
  pN( &N_ ),
  pScat( &scat )
{
	pRefFront->addref();
	pTrans->addref();
	pExtinction->addref();
	pN->addref();
	pScat->addref();
}

TranslucentSPF::~TranslucentSPF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExtinction );
	safe_release( pN );
	safe_release( pScat );
}

void TranslucentSPF::SetRefFront( const IPainter& v )         { v.addref(); safe_release( pRefFront );   pRefFront   = &v; }
void TranslucentSPF::SetTrans( const IPainter& v )            { v.addref(); safe_release( pTrans );      pTrans      = &v; }
void TranslucentSPF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }
void TranslucentSPF::SetN( const IScalarPainter& v )          { v.addref(); safe_release( pN );          pN          = &v; }
void TranslucentSPF::SetScat( const IScalarPainter& v )       { v.addref(); safe_release( pScat );       pScat       = &v; }

void TranslucentSPF::Scatter( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in] Index of refraction stack
			) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Geometric-horizon references: un-flip double-sided normals (fall back to
	// `n` for ray-derived hair or degenerate normals).  `geomNRaw` is outward
	// for exit re-emission; ray-anchored `geomN` clips entry reflection (+geomN),
	// entry transmission (-geomN), and interior backscatter (+geomN).
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	// IOR stack is authoritative for inside/outside (matching DielectricSPF).
	const bool bEntering = !ior_stack.containsCurrent();

	if( bEntering )
	{
		front.kray = ReflectanceColor( *pRefFront, ri );
		front.type = ScatteredRay::eRayDiffuse;

		if( ColorMath::MaxValue(front.kray) > 0 ) {
			const Vector3 nFront = OrientedLobeAxis( n, geomN );
			Scalar frontPdf = 0;
			if( SampleValidDiffuseExit( nFront, geomN, sampler, rv, frontPdf ) ) {
				front.ray.Set( ri.ptIntersection, rv );
				front.pdf = frontPdf;
				front.isDelta = false;
				scattered.AddScatteredRay( front );
			}
		}

		trans.kray = ReflectanceColor( *pTrans, ri );
		trans.type = ScatteredRay::eRayTranslucent;

		if( ColorMath::MaxValue(trans.kray) > 0 ) {
			const Vector3 intoSolid = -geomN;
			const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );

			const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
			if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
				const Scalar u1 = sampler.Get1D();
				const Scalar u2 = sampler.Get1D();
				Scalar transPdf = 0;
				if( SampleClippedPhong( nEnter, intoSolid, Nfactor[0], u1, u2, rv, transPdf ) ) {
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = transPdf;
					trans.isDelta = false;
					trans.ior_stack = new IORStack( ior_stack );
					// No IOR parameter on translucent_material: re-push enclosing IOR (no eta jump).
					trans.ior_stack->push( ior_stack.top() );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			} else {
				RISEPel p = trans.kray;
				trans.kray = 0;
				Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
				for( int i=0; i<3; i++ ) {
					Scalar transPdf = 0;
					if( !SampleClippedPhong( nEnter, intoSolid, Nfactor[i], ptrand.x, ptrand.y, rv, transPdf ) ) {
						continue;
					}

					trans.kray = 0;
					trans.kray[i] = p[i];
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = transPdf;
					trans.isDelta = false;
					// AddScatteredRay clears delete_stack on `trans` after a successful
					// memcpy (retaining true on overflow).  Free any un-transferred stack
					// and re-arm delete_stack so each stored copy owns its own IORStack.
					if( trans.delete_stack ) {
						safe_delete( trans.ior_stack );
					}
					trans.delete_stack = true;
					trans.ior_stack = new IORStack( ior_stack );
					trans.ior_stack->push( ior_stack.top() );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			}
		}
	}
	else
	{
		const Vector3 nExit = OrientedExitNormal( n, geomNRaw );

		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		const ScalarTriple abt = pExtinction->GetValuesAt(ri);
		const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
		front.kray = ColorMath::exponential( -distance*ab );

		front.type = ScatteredRay::eRayDiffuse;

		if( ColorMath::MaxValue(front.kray) > 0 ) {
			const ScalarTriple scat_t = pScat->GetValuesAt(ri);
			const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );

			if( ColorMath::MaxValue(scat) > 0 ) {
				// Backscatter stays inside (+geomN); no IOR stack change.
				const Vector3 stayInside = geomN;
				const Vector3 nBack = OrientedLobeAxis( n, stayInside );

				trans.type = ScatteredRay::eRayTranslucent;
				trans.kray = front.kray * scat;

				const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
				if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
					const Scalar u1 = sampler.Get1D();
					const Scalar u2 = sampler.Get1D();
					Scalar backPdf = 0;
					if( SampleClippedPhong( nBack, stayInside, Nfactor[0], u1, u2, rv, backPdf ) ) {
						trans.ray.Set( ri.ptIntersection, rv );
						trans.pdf = backPdf;
						trans.isDelta = false;
						front.kray = front.kray * (RISEPel(1.0,1.0,1.0)-scat);
						scattered.AddScatteredRay( trans );
					}
				} else {
					RISEPel p = trans.kray;
					RISEPel f = front.kray;
					trans.kray = 0;
					// Zero once before loop: front.kray accumulates all 3 channels for the single exit ray below.
					front.kray = 0;
					Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
					for( int i=0; i<3; i++ ) {
						Scalar backPdf = 0;
						if( SampleClippedPhong( nBack, stayInside, Nfactor[i], ptrand.x, ptrand.y, rv, backPdf ) ) {
							trans.kray = 0;
							trans.kray[i] = p[i];
							trans.ray.Set( ri.ptIntersection, rv );
							trans.pdf = backPdf;
							trans.isDelta = false;
							front.kray[i] = f[i] * (1.0-scat[i]);
							scattered.AddScatteredRay( trans );
						} else {
							front.kray[i] = f[i];
						}
					}
				}
			}
		}

		// Diffuse exit leaves the object (clipped to outward geomNRaw) — pop IOR stack.
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( nExit, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;
			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

void TranslucentSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in] Index of refraction stack
	) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Same geometric-horizon references as Scatter() above.
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	const bool bEnteringNM = !ior_stack.containsCurrent();

	if( bEnteringNM )
	{
		front.krayNM = ReflectanceColorNM( *pRefFront, ri, nm );
		front.type = ScatteredRay::eRayDiffuse;

		if( front.krayNM > 0 ) {
			const Vector3 nFront = OrientedLobeAxis( n, geomN );
			Scalar frontPdf = 0;
			if( SampleValidDiffuseExit( nFront, geomN, sampler, rv, frontPdf ) ) {
				front.ray.Set( ri.ptIntersection, rv );
				front.pdf = frontPdf;
				front.isDelta = false;
				scattered.AddScatteredRay( front );
			}
		}

		trans.krayNM = ReflectanceColorNM( *pTrans, ri, nm );
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			const Vector3 intoSolid = -geomN;
			const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );

			const Scalar Nval = pN->GetValueAtNM(ri,nm);
			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			Scalar transPdf = 0;
			if( SampleClippedPhong( nEnter, intoSolid, Nval, u1, u2, rv, transPdf ) ) {
				trans.ray.Set( ri.ptIntersection, rv );
				trans.pdf = transPdf;
				trans.isDelta = false;
				trans.ior_stack = new IORStack( ior_stack );
				trans.ior_stack->push( ior_stack.top() );
				GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
				scattered.AddScatteredRay( trans );
			}
		}
	}
	else
	{
		const Vector3 nExit = OrientedExitNormal( n, geomNRaw );

		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		// Entry already paid pTrans; interior segment pays Beer extinction only.
		front.krayNM = exp(-(pExtinction->GetValueAtNM(ri,nm)*distance));

		front.type = ScatteredRay::eRayDiffuse;

		if( front.krayNM > 0 ) {
			const Scalar scat = pScat->GetValueAtNM(ri,nm);

			if( scat > 0 ) {
				const Vector3 stayInside = geomN;
				const Vector3 nBack = OrientedLobeAxis( n, stayInside );

				const Scalar Nval_scat = pN->GetValueAtNM(ri,nm);
				const Scalar u1 = sampler.Get1D();
				const Scalar u2 = sampler.Get1D();
				Scalar backPdf = 0;
				if( SampleClippedPhong( nBack, stayInside, Nval_scat, u1, u2, rv, backPdf ) ) {
					trans.type = ScatteredRay::eRayTranslucent;
					trans.krayNM = front.krayNM * scat;
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = backPdf;
					trans.isDelta = false;
					scattered.AddScatteredRay( trans );

					front.krayNM *= (1.0-scat);
				}
			}
		}

		// Diffuse exit (independent of Phong N) — pop IOR stack.
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( nExit, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;

			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

Scalar TranslucentSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return AggregatePdf( ri, wo, false, 0, ior_stack );
}

// Evaluates the selected lobe's spectral BSDF value f_I(wo; nm) in [1/sr].
Scalar TranslucentSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			Scalar pdf = 0, fOverKray = 0;
			if( EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) {
				return set.lobes[i].krayNM * fOverKray;
			}
			return 0;
		}
	}
	return 0;
}

Scalar TranslucentSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& /*outDir*/,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	// eRayDiffuse = cosine lobe; eRayTranslucent = Phong lobe on both sides.
	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			return set.lobes[i].krayNM;
		}
	}
	return 0;
}

Scalar TranslucentSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack,
	Scalar pdfHero
	) const
{
	if( pdfHero <= 0 ) {
		return EvaluateKrayNM( ri, outDir, rayType, nm, ior_stack );
	}

	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			Scalar pdf = 0, fOverKray = 0;
			if( EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) {
				return ( set.lobes[i].krayNM * pdf ) / pdfHero;
			}
			return 0;
		}
	}
	return 0;
}

Scalar TranslucentSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return AggregatePdf( ri, wo, true, nm, ior_stack );
}

// Exact mixture density of Scatter/ScatterNM + RandomlySelect (all lobe krays
// are direction-independent, so selection probabilities equal weight ratios).
Scalar TranslucentSPF::AggregatePdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const bool bNM,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, bNM, nm, set );

	if( set.count == 0 ) return 0;

	// Mirror RandomlySelect: single ray selects with prob 1; >=2 rays with 0 total weight select nothing.
	if( set.count == 1 ) {
		Scalar pdf = 0, fOverKray = 0;
		return EvalLobe( set.lobes[0], wo, pdf, fOverKray ) ? pdf : Scalar(0);
	}
	if( set.totalSelectWeight <= NEARZERO ) return 0;

	Scalar total = 0;
	for( int i = 0; i < set.count; i++ ) {
		Scalar pdf = 0, fOverKray = 0;
		if( !EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) continue;
		total += ( set.lobes[i].selectWeight / set.totalSelectWeight ) * pdf;
	}
	return total;
}
