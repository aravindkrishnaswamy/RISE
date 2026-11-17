//////////////////////////////////////////////////////////////////////
//
//  AreaLightShaderOpScalarNTest.cpp - Regression for the AreaLightShaderOp
//    `N` (Phong/directionality exponent) IPainter -> IScalarPainter
//    migration.
//
//    THE BUG: AreaLightShaderOp::N used to be `const IPainter&`.  The
//    spectral path (PerformOperationNM) read it via `N.GetColorNM(...)`,
//    which routes an inline-numeric exponent through
//    UniformColorPainter -> eSpectrumKind_Albedo's [0,1] clamp before the
//    Jakob-Hanika RGB->spectrum LUT (see RGBSpectra.h ~line 39 and
//    docs/ISCALARPAINTER_REFACTOR.md).  An authored `N 5` therefore
//    silently became N =~ 1 in every spectral rasterizer, while the RGB
//    path (PerformOperation, N.GetColor -- no uplift) used the true
//    value 5.  `N` is a PHYSICAL SCALAR (an exponent in
//    `(N+1)*pow(fDot,N)`), not a colour, so it now rides the
//    `IScalarPainter` pipe (GetValuesAt(ri).v[0] / GetValueAtNM(ri,nm)),
//    which carries no colorspace and no JH uplift.
//
//    THIS TEST constructs an AreaLightShaderOp directly (bypassing scene
//    parsing) with N bound to a UniformScalarPainter(5.0), calls
//    PerformOperation (RGB) and PerformOperationNM (spectral) at the
//    SAME fixed geometry, and checks that both derive the identical
//    light-shaping factor -- i.e. the spectral path is NOT silently
//    using N=~1.  The light rectangle is sized near-zero (1e-6 x 1e-6)
//    so Monte-Carlo sample jitter perturbs the sampled point by a
//    negligible amount, making the two independent PerformOperation /
//    PerformOperationNM calls (each of which internally draws its own
//    jittered sample) agree to a tight numeric tolerance without needing
//    to reproduce MultiJitteredSampling2D's internals bit-for-bit.
//
//    Geometry is chosen so fDot = 0.3 (steeply off-normal), which makes
//    N=5 and N=1 give a DRAMATICALLY different light-shaping factor
//    (pow(0.3,5)/pow(0.3,1) =~ 1/41) -- so a regression back to the
//    pre-fix clamped-to-~1 behaviour fails loudly rather than by a
//    marginal amount.
//
//  Author: Generated for the AreaLightShaderOp IScalarPainter migration
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Shaders/AreaLightShaderOp.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
			std::cout << "  pass: " << what << "\n";
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	//! Minimal IRayCaster stub.  AreaLightShaderOp calls CastShadowRay
	//! only when `ri.pObject->DoesReceiveShadows()` is true (see
	//! LitStubObject below, which reports true so BOTH PerformOperation
	//! and PerformOperationNM actually accumulate a contribution --
	//! PerformOperationNM only adds its term inside that branch).  This
	//! stub reports "no occlusion" (returns false = not blocked), i.e. an
	//! unoccluded shading point.  Every other member exists solely to
	//! satisfy the pure-virtual interface; none of them are exercised.
	class NullRayCaster :
		public virtual IRayCaster,
		public virtual Reference
	{
	protected:
		virtual ~NullRayCaster() {}

	public:
		bool CastRay( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel&,
			const RAY_STATE&, Scalar*, const IRadianceMap* ) const override { return false; }

		bool CastRayNM( const RuntimeContext&, const RasterizerState&, const Ray&, Scalar&,
			const RAY_STATE&, const Scalar, Scalar*, const IRadianceMap* ) const override { return false; }

		bool CastRay( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel&,
			const RAY_STATE&, Scalar*, const IRadianceMap*, const IORStack& ) const override { return false; }

		bool CastRayNM( const RuntimeContext&, const RasterizerState&, const Ray&, Scalar&,
			const RAY_STATE&, const Scalar, Scalar*, const IRadianceMap*, const IORStack& ) const override { return false; }

		bool CastShadowRay( const Ray&, const Scalar ) const override
		{
			// Unoccluded -- see class comment.
			return false;
		}

		bool CastOcclusionRay( const Ray&, const Scalar ) const override
		{
			// Not exercised by this test (AreaLightShaderOp only calls
			// CastShadowRay); stub to satisfy the pure-virtual interface.
			return false;
		}

		void AttachScene( const IScene* ) override {}
		const IScene* GetAttachedScene() const override { return nullptr; }
		void SetLuminaireSampling( ISampling2D* ) override {}
		const ILuminaryManager* GetLuminaries() const override { return nullptr; }
		const Implementation::LightSampler* GetLightSampler() const override { return nullptr; }
		void SetRISCandidates( const unsigned int ) override {}
		void SetLightSampleRRThreshold( const Scalar ) override {}
		void SetUseLightBVH( const bool ) override {}
		bool IsRadianceMapVisibleAsBackground() const override { return false; }
	};

	//! Minimal IObject stub whose `DoesReceiveShadows()` returns TRUE.
	//! AreaLightShaderOp::PerformOperationNM only accumulates a
	//! contribution when `ri.pObject->DoesReceiveShadows()` is true (a
	//! pre-existing RGB/NM asymmetry, out of scope for this migration --
	//! the RGB path contributes unconditionally and only skips via the
	//! shadow test) -- so unlike TestStubObject.h's StubObject (which
	//! hardcodes false), this object must answer true.  CastShadowRay is
	//! then genuinely invoked on `NullRayCaster` above, which reports "no
	//! occlusion" (returns false), matching an unoccluded shading point.
	class LitStubObject :
		public virtual IObject,
		public virtual Reference
	{
	public:
		LitStubObject() {}

		void IntersectRay( RayIntersection&, const Scalar, const bool, const bool, const bool ) const override {}
		bool IntersectRay_IntersectionOnly( const Ray&, const Scalar, const bool, const bool ) const override { return false; }
		bool IsWorldVisible() const override { return true; }
		bool DoesCastShadows() const override { return false; }
		bool DoesReceiveShadows() const override { return true; }
		const IMaterial* GetMaterial() const override { return 0; }
		const IMedium* GetInteriorMedium() const override { return 0; }
		void UniformRandomPoint( Point3*, Vector3*, Point2*, const Point3& ) const override {}
		Scalar GetArea() const override { return 0; }
		const BoundingBox getBoundingBox() const override { return BoundingBox(); }

		void ClearAllTransforms() override {}
		void FinalizeTransformations() override {}
		Matrix4 const GetFinalTransformMatrix() const override { return Matrix4(); }
		Matrix4 const GetFinalInverseTransformMatrix() const override { return Matrix4(); }
		void FinalizeTransformations( const Matrix4& ) override {}
		Matrix4 const GetLocalTransformMatrix() const override { return Matrix4(); }
		Matrix4 const GetParentWorldTransformMatrix() const override { return Matrix4Ops::Identity(); }
		bool IsParentWorldInvertible() const override { return true; }
		Matrix4 const WorldToLocal( const Matrix4& m ) const override { return m; }

	protected:
		~LitStubObject() {}
	};
}

int main()
{
	std::cout << "AreaLightShaderOpScalarNTest -- N: IPainter -> IScalarPainter\n";

	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	// ------------------------------------------------------------------
	// Fixed geometry: intersection at the origin, unit +Y shading normal.
	// The light is placed so the vector-to-light dotted with the normal
	// (`fDot` in AreaLightShaderOp.cpp) is EXACTLY 0.3 -- steeply
	// off-normal, so N=5 vs the pre-fix ~clamped-to-1 behaviour diverge
	// by ~41x (see file header).  The light directly faces the
	// intersection point (fDotLight = 1, well inside the hotspot).
	// ------------------------------------------------------------------
	const Scalar fDotTarget = Scalar(0.3);
	const Vector3 vNormal( 0, 1, 0 );
	// Unit vector from the surface toward the light: (sin, cos, 0) so
	// Dot(u, vNormal) == cos == fDotTarget.
	const Scalar sinPart = std::sqrt( Scalar(1) - fDotTarget * fDotTarget );
	const Vector3 uToLight( sinPart, fDotTarget, 0 );

	const Scalar distToLight = Scalar(2.0);
	const Point3 ptIntersection( 0, 0, 0 );
	const Point3 lightLocation(
		ptIntersection.x + uToLight.x * distToLight,
		ptIntersection.y + uToLight.y * distToLight,
		ptIntersection.z + uToLight.z * distToLight
		);
	// Light faces directly back at the intersection point.
	const Vector3 lightDir( -uToLight.x, -uToLight.y, -uToLight.z );

	// Near-zero rectangle: MC sample jitter perturbs the sampled point by
	// at most ~1e-6 world units, negligible against distToLight = 2.0.
	const Scalar lightWidth  = Scalar(1e-6);
	const Scalar lightHeight = Scalar(1e-6);

	// ------------------------------------------------------------------
	// Painters: white (untinted) emission, N = UniformScalarPainter(5).
	// ------------------------------------------------------------------
	IPainter* pEmm = nullptr;
	RISE_API_CreateUniformColorPainter( &pEmm, RISEPel( 1, 1, 1 ) );

	IScalarPainter* pN = nullptr;
	RISE_API_CreateUniformScalarPainter( &pN, Scalar(5.0) );

	Check( pEmm != nullptr, "emission painter created" );
	Check( pN != nullptr, "N scalar painter created" );

	// ------------------------------------------------------------------
	// Sanity: the scalar painter reports N=5 unclamped on BOTH pipes --
	// this is the direct fix (contrast with the old IPainter path, whose
	// GetColorNM would have clamped an inline `5` toward ~1 above the
	// eSpectrumKind_Albedo [0,1] ceiling).
	// ------------------------------------------------------------------
	{
		const Ray dummyRay( ptIntersection, vNormal );
		const RasterizerState dummyRs = { 0, 0 };
		RayIntersectionGeometric dummyRi( dummyRay, dummyRs );
		dummyRi.bHit = true;

		const Scalar nRgb = pN->GetValuesAt( dummyRi ).v[0];
		const Scalar nNm550 = pN->GetValueAtNM( dummyRi, Scalar(550) );
		const Scalar nNm660 = pN->GetValueAtNM( dummyRi, Scalar(660) );
		std::printf( "  N via GetValuesAt().v[0] = %.6f, GetValueAtNM(550) = %.6f, GetValueAtNM(660) = %.6f\n",
			double(nRgb), double(nNm550), double(nNm660) );
		Check( nRgb == Scalar(5), "N.GetValuesAt().v[0] == 5 exactly (RGB pipe)" );
		Check( nNm550 == Scalar(5), "N.GetValueAtNM(ri,550) == 5 exactly (spectral pipe, unclamped)" );
		Check( nNm660 == Scalar(5), "N.GetValueAtNM(ri,660) == 5 exactly (spectral pipe, unclamped)" );
	}

	// ------------------------------------------------------------------
	// Construct the real shader op and drive both PerformOperation
	// (RGB) and PerformOperationNM (spectral) at the fixed geometry.
	// ------------------------------------------------------------------
	IShaderOp* pShaderOpBase = nullptr;
	RISE_API_CreateAreaLightShaderOp(
		&pShaderOpBase,
		lightWidth, lightHeight,
		lightLocation,
		Vector3Ops::Normalize( lightDir ),
		/*samples*/ 1,
		*pEmm,
		/*power*/ Scalar(1.0),
		*pN,
		/*hotSpot*/ PI,			// 180 degrees -- half-angle 90deg comfortably admits our ~72.5deg angle of incidence
		/*cache*/ false
		);
	Check( pShaderOpBase != nullptr, "AreaLightShaderOp constructed" );

	if( pShaderOpBase )
	{
		RandomNumberGenerator rng( 1u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

		LitStubObject* pStubObj = new LitStubObject();
		GlobalLog()->PrintNew( pStubObj, __FILE__, __LINE__, "test stub object" );
		NullRayCaster* pCaster = new NullRayCaster();
		GlobalLog()->PrintNew( pCaster, __FILE__, __LINE__, "test null ray caster" );

		IRayCaster::RAY_STATE rs;
		rs.type = IRayCaster::RAY_STATE::eRayView;

		IORStack iorStack( Scalar(1.0) );

		const Ray viewRay( Point3( 0, 0, 4 ), Vector3( 0, 0, -1 ) );
		const RasterizerState rast = { 0, 0 };

		RayIntersection ri( viewRay, rast );
		ri.geometric.bHit = true;
		ri.geometric.ptIntersection = ptIntersection;
		ri.geometric.vNormal = vNormal;
		ri.geometric.vGeomNormal = vNormal;
		ri.pMaterial = nullptr;		// no BSDF -> RGB path multiplies by RISEPel(1,1,1), NM path by 1
		ri.pObject = pStubObj;			// DoesReceiveShadows() == false -> CastShadowRay never called

		RISEPel cRgb( 0, 0, 0 );
		pShaderOpBase->PerformOperation( rc, ri, *pCaster, rs, cRgb, iorStack, nullptr );

		const Scalar nmProbe = Scalar( 550.0 );
		const Scalar cNm = pShaderOpBase->PerformOperationNM( rc, ri, *pCaster, rs, Scalar(0), nmProbe, iorStack, nullptr );

		std::printf( "  PerformOperation (RGB)   c = (%.9e, %.9e, %.9e)\n", double(cRgb[0]), double(cRgb[1]), double(cRgb[2]) );
		std::printf( "  PerformOperationNM(550nm) c = %.9e\n", double(cNm) );

		Check( cRgb[0] > 0 && cRgb[1] > 0 && cRgb[2] > 0, "RGB path is lit (nonzero)" );
		Check( cNm > 0, "spectral path is lit (nonzero)" );

		// White emission -> all 3 RGB channels equal (up to FP noise from
		// the independently-jittered sample).
		if( cRgb[0] > 0 ) {
			Check( std::fabs( cRgb[0] - cRgb[1] ) < cRgb[0] * Scalar(1e-3) &&
			       std::fabs( cRgb[0] - cRgb[2] ) < cRgb[0] * Scalar(1e-3),
				"RGB channels agree (white emission, uniform N)" );
		}

		// Query the emission painter's OWN spectral factor at nmProbe
		// directly.  Dividing it back out isolates the light-SHAPING
		// factor -- (N+1)*pow(fDot,N)/(2*PI) * fDotLight * attenuation --
		// which is EXACTLY where `N` participates, and is the thing this
		// migration must keep IDENTICAL between the RGB and spectral pipes.
		//
		// It MUST be read through `GetRadianceNM`, not `GetColorNM`: since
		// Stage C slice 2 (docs/SPECTRAL_ILLUMINANT_CONVENTION.md)
		// `AreaLightShaderOp::PerformOperationNM` samples its `emm`
		// EMISSION slot as a radiance source, so `emm`'s contribution to
		// `cNm` is its illuminant-shaped spectrum (sigmoid x D65norm),
		// not its reflectance sigmoid.  Normalising by `GetColorNM` here
		// left D65norm(550)/kD65YNorm ~= 1.052 in the quotient and made
		// this check fail by exactly 5.2 % -- a stale test, not a
		// light-shaping regression.
		const RayIntersectionGeometric& rig = ri.geometric;
		const Scalar emmFactorNm = pEmm->GetRadianceNM( rig, nmProbe );
		std::printf( "  emm->GetRadianceNM(550nm) = %.9f\n", double(emmFactorNm) );

		if( cRgb[0] > 0 && emmFactorNm > 0 )
		{
			const Scalar lightFactorRgb = cRgb[0];			// emm RGB factor is exactly 1 (white, GetColor no uplift)
			const Scalar lightFactorNm  = cNm / emmFactorNm;	// back out emm's own (unguarded, untouched) NM factor

			const Scalar relDiff = std::fabs( lightFactorNm - lightFactorRgb ) /
				r_max( lightFactorRgb, Scalar(1e-300) );
			std::printf( "  light-shaping factor: RGB = %.9e, NM(/emm) = %.9e, relDiff = %.6e\n",
				double(lightFactorRgb), double(lightFactorNm), double(relDiff) );

			// THE FIX: NM path's light-shaping factor (driven by N=5 via
			// IScalarPainter, unclamped) matches the RGB path's to a tight
			// tolerance -- the only difference between the two calls is
			// independent MC sample jitter over a near-zero light rect.
			Check( relDiff < Scalar(1e-2),
				"NM light-shaping factor matches RGB's (N carried identically on both pipes)" );

			// THE BUG, if it were still present: pre-fix, the spectral
			// path's N would have been clamped toward ~1 by IPainter's
			// eSpectrumKind_Albedo GetColorNM, giving light-shaping factor
			// proportional to pow(fDot,1) instead of pow(fDot,5) -- at
			// fDot=0.3 that is ~41x LARGER than the correct N=5 value.
			// Assert the observed factor is NOT anywhere near that
			// clamped-N=1 magnitude, so a regression back to IPainter
			// fails loudly rather than marginally.
			const Scalar pN_val = Scalar(5);
			const Scalar k5 = ( pN_val + 1 ) * std::pow( fDotTarget, pN_val ) * ( Scalar(1) / TWO_PI );
			const Scalar k1 = ( Scalar(1) + 1 ) * std::pow( fDotTarget, Scalar(1) ) * ( Scalar(1) / TWO_PI );
			// Compare shapes via the k5/k1 ratio directly instead of
			// re-deriving attenuation/fDotLight (which cancel identically
			// in both paths): lightFactorRgb / k5 == attenuation*fDotLight,
			// a positive constant shared by both formulas.
			const Scalar impliedAtten = lightFactorRgb / k5;
			const Scalar wouldBeK1Factor = k1 * impliedAtten;
			std::printf( "  analytic k5=%.9e k1=%.9e; observed NM factor vs k1-based (clamped-N) prediction = %.9e vs %.9e\n",
				double(k5), double(k1), double(lightFactorNm), double(wouldBeK1Factor) );
			Check( lightFactorNm < wouldBeK1Factor * Scalar(0.5),
				"NM light-shaping factor is pow(fDot,5)-shaped, NOT clamped toward N=1 (would be ~41x larger)" );
		}

		safe_release( pCaster );
		safe_release( pStubObj );
		pShaderOpBase->release();
	}

	safe_release( pEmm );
	safe_release( pN );

	std::cout << "================================\n";
	std::cout << "Passed: " << s_pass << "  Failed: " << s_fail << std::endl;
	return ( s_fail == 0 ) ? 0 : 1;
}
