//////////////////////////////////////////////////////////////////////
//
//  MixMaterialTest.cpp - Comprehensive closed-form, integrator, parser,
//  and Review Round 3 P1 contract tests for `MixMaterial` (`mix_material`).
//
//  Tests:
//    1. Construction validation & refusals (MIX §I.3):
//       - empty/null weight, nested mix, dielectric, rough dielectric,
//         perfect refractor, translucent, composite, null boundary,
//         mix wrapped inside coated/fabric/composite/luminaire,
//         interior_medium on mix object
//       - accepts Lambertian, GGX, OrenNayar, DiffuseTransmission,
//         SSS, RandomWalkSSS, none (absorber)
//    2. Bounded response-coefficient semantics (MIX §I.1):
//       - 0.25 Red + 0 shortfall -> 0.25 Red / 0.75 absorber (NOT 1.0)
//       - S > 1 (0.6 + 0.6 = 1.2) -> clamped to 0.5 / 0.5
//       - negative / NaN / +Inf weights clamped to 0
//    3. Round 3 §3 independent per-hit sampler draw on coincident sheets
//    4. Round 3 P1-2 pass-through alpha registration, LightSampler census,
//       and unified RayIntersection::HitCoverage()
//    5. Per-leaf optional modifier replacement (`worn_wooden_box` motif)
//    6. Round 3 P1-6 & P1-1 noise-free expected-value denoiser guides
//       (including per-leaf modifiers & black absorber albedo)
//    7. Post-traversal `osl_ao`-weighted mix (box center vs convex edge)
//    8. End-to-end CST v7 `mix_material` parsing + real PT / BDPT / VCM
//       renders verifying Round 3 P1-1 (`0.25 White + 0.75 none` VCM merge
//       converges to 0.25, NOT 4x over-bright 1.0), color blend, and
//       pass-through layering over a background quad.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IRayIntersectionModifier.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Materials/MixMaterial.h"
#include "../src/Library/Materials/DiffuseTransmissionMaterial.h"
#include "../src/Library/Materials/NullBoundaryMaterial.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Cameras/OrthographicCamera.h"
#include "../src/Library/Cameras/CameraUtilities.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/SceneEditor/MaterialPreview.h"
#include "../src/Library/SceneEditor/PainterPreview.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/AOVBuffers.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Modifiers/ModifierFrame.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/ExpressionMemo.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check( bool cond, const char* msg )
	{
		if( cond ) {
			++g_pass;
		} else {
			++g_fail;
			std::fprintf( stderr, "FAIL: %s\n", msg );
		}
	}

	void CheckClose( double a, double b, double tol, const char* msg )
	{
		if( std::isfinite( a ) && std::fabs( a - b ) <= tol ) {
			++g_pass;
		} else {
			++g_fail;
			std::fprintf( stderr, "FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n", msg, a, b, tol );
		}
	}

	//! Simple deterministic normal-tilting modifier for testing per-leaf
	//! modifier replacement and expected-value normal guides.
	class FixedTiltModifier :
		public virtual IRayIntersectionModifier,
		public virtual Reference
	{
		Vector3 targetNormal;
	public:
		explicit FixedTiltModifier( const Vector3& n )
			: targetNormal( Vector3Ops::Normalize( n ) ) {}

		void Modify( RayIntersectionGeometric& ri ) const override
		{
			ModifierFrame::RecordPreModifierFrame( ri );
			ri.vNormal = targetNormal;
			ri.onb.CreateFromW( targetNormal );
		}
	};

	//! Captures rendered pixels from a real IJob rasterization run.
	class BufferRasterizerOutput :
		public virtual IJobRasterizerOutput,
		public virtual Reference
	{
	public:
		unsigned int width = 0;
		unsigned int height = 0;
		std::vector<RISEColor> pixels;

		bool PremultipliedAlpha() override { return false; }
		int GetColorSpace() override { return 0; }
		bool WantsFloat32() override { return true; }

		void OutputImageRGBA16(
			const unsigned short*,
			const unsigned int,
			const unsigned int,
			const unsigned int,
			const unsigned int,
			const unsigned int,
			const unsigned int
			) override
		{
		}

		void OutputImageRGBA32F(
			const float* pImageData,
			const unsigned int w,
			const unsigned int h,
			const unsigned int /*rc_top*/,
			const unsigned int /*rc_left*/,
			const unsigned int /*rc_bottom*/,
			const unsigned int /*rc_right*/
			) override
		{
			width = w;
			height = h;
			pixels.resize( static_cast<size_t>( w ) * h );
			for( size_t i = 0; i < pixels.size(); ++i ) {
				pixels[i] = RISEColor(
					RISEPel( pImageData[i * 4 + 0], pImageData[i * 4 + 1], pImageData[i * 4 + 2] ),
					pImageData[i * 4 + 3] );
			}
		}

		RISEPel CenterMean( unsigned int margin = 2 ) const
		{
			RISEPel sum( 0, 0, 0 );
			unsigned int count = 0;
			const unsigned int y0 = ( height > 2 * margin ) ? margin : 0;
			const unsigned int y1 = ( height > 2 * margin ) ? ( height - margin ) : height;
			const unsigned int x0 = ( width > 2 * margin ) ? margin : 0;
			const unsigned int x1 = ( width > 2 * margin ) ? ( width - margin ) : width;
			for( unsigned int y = y0; y < y1; ++y ) {
				for( unsigned int x = x0; x < x1; ++x ) {
					sum = sum + pixels[static_cast<size_t>( y ) * width + x].base;
					++count;
				}
			}
			return count > 0 ? ( sum * ( Scalar( 1 ) / static_cast<Scalar>( count ) ) ) : RISEPel( 0, 0, 0 );
		}
	};

	TriangleMeshGeometryIndexed* MakeQuadXY( const Scalar z, const Scalar halfExtent = 2.0 )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
		mesh->BeginIndexedTriangles();
		mesh->AddVertex( Point3( -halfExtent, -halfExtent, z ) );
		mesh->AddVertex( Point3(  halfExtent, -halfExtent, z ) );
		mesh->AddVertex( Point3(  halfExtent,  halfExtent, z ) );
		mesh->AddVertex( Point3( -halfExtent,  halfExtent, z ) );
		mesh->AddNormal( Vector3( 0, 0, 1 ) );
		mesh->AddTexCoord( Point2( 0, 0 ) );
		mesh->AddTexCoord( Point2( 1, 0 ) );
		mesh->AddTexCoord( Point2( 1, 1 ) );
		mesh->AddTexCoord( Point2( 0, 1 ) );
		IndexedTriangle t0, t1;
		t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
		t0.iNormals[0]  = 0; t0.iNormals[1]  = 0; t0.iNormals[2]  = 0;
		t0.iCoords[0]   = 0; t0.iCoords[1]   = 1; t0.iCoords[2]   = 2;
		t1.iVertices[0] = 0; t1.iVertices[1] = 2; t1.iVertices[2] = 3;
		t1.iNormals[0]  = 0; t1.iNormals[1]  = 0; t1.iNormals[2]  = 0;
		t1.iCoords[0]   = 0; t1.iCoords[1]   = 2; t1.iCoords[2]   = 3;
		mesh->AddIndexedTriangle( t0 );
		mesh->AddIndexedTriangle( t1 );
		mesh->DoneIndexedTriangles();
		return mesh;
	}

	void TestValidationAndRefusals()
	{
		UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		UniformScalarPainter* pHalf = new UniformScalarPainter( 0.5 );
		UniformScalarPainter* pOne  = new UniformScalarPainter( 1.0 );
		UniformScalarPainter* pIOR  = new UniformScalarPainter( 1.5 );
		UniformScalarPainter* pScat = new UniformScalarPainter( 100.0 );
		UniformScalarPainter* pRough = new UniformScalarPainter( 0.2 );

		IMaterial* mLamb = 0;
		IMaterial* mDiel = 0;
		IMaterial* mRoughDiel = 0;
		IMaterial* mRefr = 0;
		IMaterial* mTrans = 0;
		IMaterial* mDiffTrans = 0;
		IMaterial* mNullBound = new NullBoundaryMaterial();

		Check( RISE_API_CreateLambertianMaterial( &mLamb, *pWhite ), "create lambertian" );
		Check( RISE_API_CreateDielectricMaterial( &mDiel, *pOne, *pIOR, *pScat, false ), "create dielectric" );
		Check( RISE_API_CreateRoughDielectricMaterial( &mRoughDiel, *pOne, *pIOR, *pRough ), "create rough dielectric" );
		Check( RISE_API_CreatePerfectRefractorMaterial( &mRefr, *pWhite, *pIOR ), "create perfect refractor" );
		Check( RISE_API_CreateTranslucentMaterial( &mTrans, *pWhite, *pWhite, *pOne, *pOne, *pScat ), "create translucent" );
		mDiffTrans = new DiffuseTransmissionMaterial( *pWhite, *pWhite );
		Check( mDiffTrans != 0, "create diffuse_transmission" );

		// Valid mix: Lambertian + DiffuseTransmission + none
		{
			const IMaterial* kids[3] = { mLamb, mDiffTrans, 0 };
			const IScalarPainter* wts[3] = { pHalf, pHalf, pHalf };
			IMaterial* mValid = 0;
			Check( RISE_API_CreateMixMaterial( &mValid, kids, wts, 0, 3, 0 ) && mValid != 0,
				"valid mix accepts Lambertian + DiffuseTransmission + none" );
			Check( mValid->IsUnresolvedMix(), "MixMaterial::IsUnresolvedMix() == true" );
			Check( mValid->GetBSDF() == 0 && mValid->GetSPF() == 0 && mValid->GetEmitter() == 0,
				"unresolved MixMaterial returns null BSDF/SPF/Emitter" );

			// Refuse nested mix
			const IMaterial* nestedKids[1] = { mValid };
			const IScalarPainter* nestedWts[1] = { pOne };
			IMaterial* mNested = 0;
			Check( !RISE_API_CreateMixMaterial( &mNested, nestedKids, nestedWts, 0, 1, 0 ) && mNested == 0,
				"refuse nested MixMaterial" );
			mValid->release();
		}

		// Refuse empty / null weight
		{
			IMaterial* mBad = 0;
			const IMaterial* kids[1] = { mLamb };
			const IScalarPainter* nullWts[1] = { 0 };
			Check( !RISE_API_CreateMixMaterial( &mBad, kids, nullWts, 0, 0, 0 ), "refuse 0 children" );
			Check( !RISE_API_CreateMixMaterial( &mBad, kids, nullWts, 0, 1, 0 ), "refuse null weight" );
		}

		// Refuse containment-tracking leaves
		const IMaterial* forbidden[5] = { mDiel, mRoughDiel, mRefr, mTrans, mNullBound };
		const char* names[5] = { "dielectric", "rough_dielectric", "perfect_refractor", "translucent", "null_boundary" };
		for( int i = 0; i < 5; ++i ) {
			const IMaterial* kids[1] = { forbidden[i] };
			const IScalarPainter* wts[1] = { pOne };
			IMaterial* mBad = 0;
			Check( !RISE_API_CreateMixMaterial( &mBad, kids, wts, 0, 1, 0 ) && mBad == 0, names[i] );
		}

		// Refuse MixMaterial wrapped in luminaire / composite / coated / fabric or hosting interior_medium
		{
			IJob* pJob = 0;
			Check( RISE_CreateJob( &pJob ) && pJob != 0, "create job for refusal tests" );
			double cW[3] = { 0.8, 0.8, 0.8 };
			Check( pJob->AddUniformColorPainter( "p_w", cW, "sRGB" ), "add painter" );
			Check( pJob->AddLambertianMaterial( "m_l", "p_w" ), "add lambertian" );
			const char* cKids[1] = { "m_l" };
			const char* cWts[1]  = { "0.5" };
			Check( pJob->AddMixMaterial( "m_mix", cKids, cWts, 0, 1, 0 ), "add mix via Job" );
			Check( !pJob->AddLambertianLuminaireMaterial( "m_lum_bad", "p_w", "m_mix", 1.0 ),
				"Job refuses MixMaterial inside LambertianLuminaire" );
			Check( !pJob->AddPhongLuminaireMaterial( "m_plum_bad", "p_w", "m_mix", "10", 1.0 ),
				"Job refuses MixMaterial inside PhongLuminaire" );
			Check( !pJob->AddCompositeMaterial( "m_comp_bad", "m_mix", "m_l", 4, 2, 2, 2, 2, 1.0, "0.1" ),
				"Job refuses MixMaterial inside Composite" );
			Check( !pJob->AddCoatedMaterial( "m_coat_bad", "m_mix", "1.0", "1.5", "0.1", "0.0", "0.0", "p_w" ),
				"Job refuses MixMaterial inside Coated" );
			Check( !pJob->AddFabricMaterial( "m_fab_bad", "silk", "m_mix", "p_w", "0.3", "0.0" ),
				"Job refuses MixMaterial inside Fabric" );

			double pos[3] = { 0, 0, 0 }, ori[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 };
			double sa[3] = { 0.1, 0.1, 0.1 }, ss[3] = { 0.1, 0.1, 0.1 };
			Check( pJob->AddBoxGeometry( "g_b", 1, 1, 1 ), "add box" );
			Check( pJob->AddHomogeneousMedium( "med_a", sa, ss, "isotropic", 0.0 ), "add medium" );
			RadianceMapConfig rmCfg;
			Check( pJob->AddObject( "o_mix", "g_b", "m_mix", 0, 0, rmCfg, pos, ori, sc, true, true ), "add mix obj" );
			Check( !pJob->SetObjectInteriorMedium( "o_mix", "med_a" ),
				"Job refuses interior_medium on object with MixMaterial" );
			pJob->release();
		}

		mLamb->release(); mDiel->release(); mRoughDiel->release();
		mRefr->release(); mTrans->release(); mDiffTrans->release(); mNullBound->release();
		pWhite->release(); pHalf->release(); pOne->release(); pIOR->release(); pScat->release(); pRough->release();
	}

	void TestResponseCoefficientsAndIndependentDraws()
	{
		UniformColorPainter* pRed   = new UniformColorPainter( RISEPel( 1, 0, 0 ) );
		UniformColorPainter* pBlue  = new UniformColorPainter( RISEPel( 0, 0, 1 ) );
		UniformScalarPainter* p025 = new UniformScalarPainter( 0.25 );
		UniformScalarPainter* p060 = new UniformScalarPainter( 0.60 );
		UniformScalarPainter* p050 = new UniformScalarPainter( 0.50 );
		UniformScalarPainter* p040 = new UniformScalarPainter( 0.40 );
		UniformScalarPainter* pNeg = new UniformScalarPainter( -0.5 );
		UniformScalarPainter* pNaN = new UniformScalarPainter( std::numeric_limits<Scalar>::quiet_NaN() );

		IMaterial* mRed = 0;
		IMaterial* mBlue = 0;
		RISE_API_CreateLambertianMaterial( &mRed, *pRed );
		RISE_API_CreateLambertianMaterial( &mBlue, *pBlue );

		RandomNumberGenerator rng( 1337 );
		IndependentSampler sampler( rng );

		// 1) Bounded shortfall: 0.25 Red + 0 -> 0.25 Red, 0.75 absorber
		{
			const IMaterial* kids[1] = { mRed };
			const IScalarPainter* wts[1] = { p025 };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, 0, 1, 0 ), "create 0.25 mix" );
			int redCount = 0;
			const int N = 40000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				ri.geometric.bHit = true;
				ri.pMaterial = mMix;
				mMix->ResolveMixHit( ri, &sampler );
				Check( ri.pUnresolvedMix == mMix && ri.bMaterialFromMix, "mix stamps provenance" );
				CheckClose( ri.mixConnectible, 0.25, 1e-12, "q == 0.25 for 0.25 Red + 0.75 shortfall" );
				if( ri.pMaterial == mRed ) ++redCount;
				else Check( ri.pMaterial == 0, "shortfall resolves to nullptr absorber" );
			}
			CheckClose( static_cast<double>( redCount ) / N, 0.25, 0.015,
				"MIX §I.1: 0.25 Red reflects 0.25 (NOT normalized to 1.0)" );
			mMix->release();
		}

		// 2) S > 1 clamping: 0.6 Red + 0.6 Blue -> 0.5 / 0.5
		{
			const IMaterial* kids[2] = { mRed, mBlue };
			const IScalarPainter* wts[2] = { p060, p060 };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, 0, 2, 0 ), "create S>1 mix" );
			int redCount = 0, blueCount = 0;
			const int N = 40000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				ri.geometric.bHit = true;
				ri.pMaterial = mMix;
				mMix->ResolveMixHit( ri, &sampler );
				if( ri.pMaterial == mRed ) ++redCount;
				else if( ri.pMaterial == mBlue ) ++blueCount;
			}
			CheckClose( static_cast<double>( redCount ) / N, 0.50, 0.015, "S=1.2 Red clamps to 0.5" );
			CheckClose( static_cast<double>( blueCount ) / N, 0.50, 0.015, "S=1.2 Blue clamps to 0.5" );
			mMix->release();
		}

		// 3) Negative / NaN weights clamped to 0
		{
			const IMaterial* kids[3] = { mRed, mBlue, mBlue };
			const IScalarPainter* wts[3] = { p040, pNeg, pNaN };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, 0, 3, 0 ), "create bad-weight mix" );
			int redCount = 0, blueCount = 0;
			const int N = 20000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				ri.geometric.bHit = true;
				ri.pMaterial = mMix;
				mMix->ResolveMixHit( ri, &sampler );
				if( ri.pMaterial == mRed ) ++redCount;
				else if( ri.pMaterial == mBlue ) ++blueCount;
			}
			Check( blueCount == 0, "negative/NaN weights never selected" );
			CheckClose( static_cast<double>( redCount ) / N, 0.40, 0.02, "0.40 valid + neg/NaN -> 0.40" );
			mMix->release();
		}

		// 4) Round 3 §3: Independent per-hit draws on two coincident sheets at identical point
		{
			const IMaterial* kids[2] = { mRed, mBlue };
			const IScalarPainter* wts[2] = { p050, p050 };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, 0, 2, 0 ), "create 50/50 mix" );

			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* qGeo = MakeQuadXY( 0.0 );
			Object* qObj = new Object( qGeo );
			qObj->AssignMaterial( *mMix );
			om->AddItem( qObj, "sheet" );
			om->PrepareForRendering();

			int bothRed = 0;
			const int N = 20000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri1( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				RayIntersection ri2( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				om->IntersectRaySampled( ri1, sampler );
				om->IntersectRaySampled( ri2, sampler );
				if( ri1.pMaterial == mRed && ri2.pMaterial == mRed ) ++bothRed;
			}
			// Under independent draws: 0.25. Under withdrawn patchwork hash: 0.50!
			CheckClose( static_cast<double>( bothRed ) / N, 0.25, 0.02,
				"Round 3 §3: two hits at identical point draw independently (joint 0.25, not 0.50)" );

			qObj->release(); qGeo->release(); om->release(); mMix->release();
		}

		mRed->release(); mBlue->release();
		pRed->release(); pBlue->release(); p025->release(); p060->release();
		p050->release(); p040->release(); pNeg->release(); pNaN->release();
	}

	void TestPassthroughAlphaRegistrationAndHitCoverage()
	{
		UniformColorPainter* pRed   = new UniformColorPainter( RISEPel( 1, 0, 0 ) );
		UniformColorPainter* pGreen = new UniformColorPainter( RISEPel( 0, 1, 0 ) );
		UniformColorPainter* pBlue  = new UniformColorPainter( RISEPel( 0, 0, 1 ) );
		UniformScalarPainter* p030 = new UniformScalarPainter( 0.30 );
		UniformScalarPainter* p040 = new UniformScalarPainter( 0.40 );
		UniformScalarPainter* p050 = new UniformScalarPainter( 0.50 );

		IMaterial* mRedHalfAlpha = 0;
		IMaterial* mBlue = 0;
		IMaterial* mGreen = 0;
		RISE_API_CreateLambertianMaterial( &mRedHalfAlpha, *pRed );
		RISE_API_CreateLambertianMaterial( &mBlue, *pBlue );
		RISE_API_CreateLambertianMaterial( &mGreen, *pGreen );

		// First test pass-through mix WITHOUT any SetAlpha on any material:
		// Round 3 P1-2: AnyAlphaMaterials() and LightSampler::SceneHasAlphaCoverage() MUST become true!
		const bool alphaBefore = IMaterial::AnyAlphaMaterials();
		{
			const IMaterial* kids[1] = { mBlue };
			const IScalarPainter* wts[1] = { p030 };
			IMaterial* mPass = 0;
			Check( RISE_API_CreateMixMaterial( &mPass, kids, wts, 0, 1, p040 ), "create passthrough mix" );
			Check( IMaterial::AnyAlphaMaterials(), "Round 3 P1-2: passthrough mix sets AnyAlphaMaterials()==true" );
			Check( mPass->HasAlphaCoverage(), "Round 3 P1-2: passthrough mix HasAlphaCoverage()==true" );

			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* frontGeo = MakeQuadXY( 0.0 );
			TriangleMeshGeometryIndexed* backGeo  = MakeQuadXY( -1.0 );
			Object* frontObj = new Object( frontGeo );
			Object* backObj  = new Object( backGeo );
			frontObj->AssignMaterial( *mPass );
			backObj->AssignMaterial( *mGreen );
			om->AddItem( frontObj, "front_mix" );
			om->AddItem( backObj, "back_green" );
			om->PrepareForRendering();

			Scene* sc = new Scene();
			sc->SetObjectManager( om );
			LightSampler* ls = new LightSampler();
			LuminaryManager::LuminariesList emptyLums;
			ls->Prepare( *sc, emptyLums );
			Check( ls->SceneHasAlphaCoverage(),
				"Round 3 P1-2: LightSampler::SceneHasAlphaCoverage()==true for passthrough-only mix" );
			ls->release();
			sc->release();

			// Cast 20,000 rays: 40% pass through to back_green, 30% hit mBlue, 30% hit absorber on front_mix!
			RandomNumberGenerator rng( 4242 );
			IndependentSampler sampler( rng );
			int hitBack = 0, hitBlue = 0, hitAbs = 0;
			const int N = 20000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				om->IntersectRaySampled( ri, sampler );
				Check( ri.geometric.bHit, "ray hits front or back" );
				if( ri.pObject == backObj ) ++hitBack;
				else if( ri.pMaterial == mBlue ) {
					++hitBlue;
					CheckClose( ri.acceptedAlphaCoverage, 0.60, 1e-12, "acceptedAlphaCoverage == 1 - 0.4 = 0.6" );
					CheckClose( ri.mixConnectible, 0.50, 1e-12, "mixConnectible == 0.3 / 0.6 = 0.5" );
				} else {
					++hitAbs;
					Check( ri.pMaterial == 0, "absorber shortfall on front_mix" );
				}
			}
			CheckClose( static_cast<double>( hitBack ) / N, 0.40, 0.02, "passthrough rate == 0.40" );
			CheckClose( static_cast<double>( hitBlue ) / N, 0.30, 0.02, "blue leaf rate == 0.30" );
			CheckClose( static_cast<double>( hitAbs  ) / N, 0.30, 0.02, "absorber rate == 0.30" );

			frontObj->release(); backObj->release(); frontGeo->release(); backGeo->release();
			om->release(); mPass->release();
		}
		Check( IMaterial::AnyAlphaMaterials() == alphaBefore,
			"releasing passthrough mix decrements global alphaMaterialCount cleanly" );

		// Now compose mix pass-through (0.4 -> mixCoverage=0.6) with leaf SetAlpha (0.5):
		// HitCoverage() for Red leaf MUST be 0.6 * 0.5 = 0.30!
		mRedHalfAlpha->SetAlpha( p050, eAlphaBlend, 0.5 );
		{
			const IMaterial* kids[2] = { mRedHalfAlpha, mBlue };
			const IScalarPainter* wts[2] = { p030, p030 };
			IMaterial* mComp = 0;
			Check( RISE_API_CreateMixMaterial( &mComp, kids, wts, 0, 2, p040 ), "create composed alpha mix" );

			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* frontGeo = MakeQuadXY( 0.0 );
			TriangleMeshGeometryIndexed* backGeo  = MakeQuadXY( -1.0 );
			Object* frontObj = new Object( frontGeo );
			Object* backObj  = new Object( backGeo );
			frontObj->AssignMaterial( *mComp );
			backObj->AssignMaterial( *mGreen );
			om->AddItem( frontObj, "front_comp" );
			om->AddItem( backObj, "back_green" );
			om->PrepareForRendering();

			RandomNumberGenerator rng( 9999 );
			IndependentSampler sampler( rng );
			int hitRed = 0, hitBlue = 0, hitBack = 0;
			const int N = 30000;
			for( int i = 0; i < N; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				om->IntersectRaySampled( ri, sampler );
				if( ri.pObject == backObj ) ++hitBack;
				else if( ri.pMaterial == mRedHalfAlpha ) {
					++hitRed;
					CheckClose( ri.acceptedAlphaCoverage, 0.30, 1e-12, "HitCoverage(Red) == 0.6 * 0.5 = 0.30" );
				} else if( ri.pMaterial == mBlue ) {
					++hitBlue;
					CheckClose( ri.acceptedAlphaCoverage, 0.60, 1e-12, "HitCoverage(Blue) == 0.6 * 1.0 = 0.60" );
				}
			}
			// P(Red & accepted) = 0.5 * 0.3 = 0.15; P(Blue & accepted) = 0.5 * 0.6 = 0.30; P(back) = 0.55
			CheckClose( static_cast<double>( hitRed  ) / N, 0.15, 0.015, "composed Red hit rate == 0.15" );
			CheckClose( static_cast<double>( hitBlue ) / N, 0.30, 0.015, "composed Blue hit rate == 0.30" );
			CheckClose( static_cast<double>( hitBack ) / N, 0.55, 0.015, "composed passthrough rate == 0.55" );

			frontObj->release(); backObj->release(); frontGeo->release(); backGeo->release();
			om->release(); mComp->release();
		}

		mRedHalfAlpha->release(); mBlue->release(); mGreen->release();
		pRed->release(); pGreen->release(); pBlue->release();
		p030->release(); p040->release(); p050->release();
	}

	void TestPerLeafModifierAndExpectedGuides()
	{
		UniformColorPainter* pRed   = new UniformColorPainter( RISEPel( 1, 0, 0 ) );
		UniformColorPainter* pBlue  = new UniformColorPainter( RISEPel( 0, 0, 1 ) );
		UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		UniformScalarPainter* p050 = new UniformScalarPainter( 0.50 );
		UniformScalarPainter* p025 = new UniformScalarPainter( 0.25 );

		IMaterial* mRed = 0;
		IMaterial* mBlue = 0;
		IMaterial* mWhite = 0;
		RISE_API_CreateLambertianMaterial( &mRed, *pRed );
		RISE_API_CreateLambertianMaterial( &mBlue, *pBlue );
		RISE_API_CreateLambertianMaterial( &mWhite, *pWhite );

		const Vector3 nTilted = Vector3Ops::Normalize( Vector3( 0.6, 0.0, 0.8 ) );
		FixedTiltModifier* pModTilt = new FixedTiltModifier( nTilted );

		// 1) 0.5 Red (no modifier) + 0.5 Blue (FixedTiltModifier)
		{
			const IMaterial* kids[2] = { mRed, mBlue };
			const IScalarPainter* wts[2] = { p050, p050 };
			const IRayIntersectionModifier* mods[2] = { 0, pModTilt };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, mods, 2, 0 ), "create leaf-mod mix" );

			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* qGeo = MakeQuadXY( 0.0 );
			Object* qObj = new Object( qGeo );
			qObj->AssignMaterial( *mMix );
			om->AddItem( qObj, "quad_mod" );
			om->PrepareForRendering();

			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );
			const Vector3 expectedN = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) * 0.5 + nTilted * 0.5 );
			bool sawRed = false, sawBlue = false;

			for( int i = 0; i < 64; ++i ) {
				RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				om->IntersectRaySampled( ri, sampler );
				Check( ri.geometric.bHit, "hit quad_mod" );
				if( ri.pMaterial == mRed ) {
					sawRed = true;
					Check( ri.pModifier == 0, "Leaf 0 keeps null modifier" );
				} else if( ri.pMaterial == mBlue ) {
					sawBlue = true;
					Check( ri.pModifier == pModTilt, "Leaf 1 receives per-leaf modifier" );
				}
				// Apply chosen leaf's modifier (as Accurate AOV does), THEN ask for expected guides:
				// Round 3 P1-6: guides MUST stay EXACT expected value regardless of which leaf was drawn!
				if( ri.pModifier ) ri.pModifier->Modify( ri.geometric );
				RISEPel guideAlbedo( 0, 0, 0 );
				Vector3 guideNormal( 0, 0, 0 );
				EvaluateMaterialAOVGuides( ri, ri.geometric, false, 0, guideAlbedo, guideNormal );
				CheckClose( guideAlbedo[0], 0.5, 1e-9, "expected guide albedo R == 0.5" );
				CheckClose( guideAlbedo[1], 0.0, 1e-9, "expected guide albedo G == 0.0" );
				CheckClose( guideAlbedo[2], 0.5, 1e-9, "expected guide albedo B == 0.5" );
				CheckClose( guideNormal.x, expectedN.x, 1e-9, "expected guide normal X" );
				CheckClose( guideNormal.y, expectedN.y, 1e-9, "expected guide normal Y" );
				CheckClose( guideNormal.z, expectedN.z, 1e-9, "expected guide normal Z" );
			}
			Check( sawRed && sawBlue, "both leaves observed in 64 draws" );

			qObj->release(); qGeo->release(); om->release(); mMix->release();
		}

		// 2) Round 3 P1-1: Absorber albedo guide MUST be black (0,0,0), NOT white (1,1,1)!
		{
			const IMaterial* kids[1] = { mWhite };
			const IScalarPainter* wts[1] = { p025 };
			IMaterial* mAbsMix = 0;
			Check( RISE_API_CreateMixMaterial( &mAbsMix, kids, wts, 0, 1, 0 ), "create absorber mix" );

			RayIntersection ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			ri.geometric.bHit = true;
			ri.geometric.vNormal = Vector3( 0, 0, 1 );
			ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
			ri.pMaterial = mAbsMix;
			mAbsMix->ResolveMixHit( ri, 0 );
			RISEPel guideAlbedo( 0, 0, 0 );
			Vector3 guideNormal( 0, 0, 0 );
			EvaluateMaterialAOVGuides( ri, ri.geometric, false, 0, guideAlbedo, guideNormal );
			CheckClose( guideAlbedo[0], 0.25, 1e-9, "Round 3 P1-1: 0.25 White + 0.75 none guide albedo == 0.25 (NOT 1.0)" );
			CheckClose( guideAlbedo[1], 0.25, 1e-9, "guide albedo G == 0.25" );
			CheckClose( guideAlbedo[2], 0.25, 1e-9, "guide albedo B == 0.25" );
			mAbsMix->release();

			// Standalone nullptr absorber -> black (0,0,0)
			RayIntersection riNull( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			riNull.geometric.bHit = true;
			riNull.geometric.vNormal = Vector3( 0, 0, 1 );
			riNull.pMaterial = 0;
			EvaluateMaterialAOVGuides( riNull, riNull.geometric, false, 0, guideAlbedo, guideNormal );
			CheckClose( guideAlbedo[0], 0.0, 1e-9, "standalone absorber guide albedo == 0.0" );
		}

		pModTilt->release();
		mRed->release(); mBlue->release(); mWhite->release();
		pRed->release(); pBlue->release(); pWhite->release(); p050->release(); p025->release();
	}

	void TestOslAOWeightedMixWornWoodenBoxMotif()
	{
		// Exact `worn_wooden_box` motif:
		//   Leaf 0 (flat): weight = osl_ao(0.07, 16, vec3(0,0,0), 3)  (inside + only_local)
		//   Leaf 1 (bumped): weight = 1 - osl_ao(0.07, 16, vec3(0,0,0), 3) with modifier
		UniformColorPainter* pFlatColor = new UniformColorPainter( RISEPel( 0.8, 0.5, 0.2 ) );
		UniformColorPainter* pWornColor = new UniformColorPainter( RISEPel( 0.3, 0.2, 0.1 ) );
		IMaterial* mFlat = 0;
		IMaterial* mWorn = 0;
		RISE_API_CreateLambertianMaterial( &mFlat, *pFlatColor );
		RISE_API_CreateLambertianMaterial( &mWorn, *pWornColor );

		FixedTiltModifier* pWornMod = new FixedTiltModifier( Vector3( 0.3, 0.0, 0.95 ) );

		std::vector<ParamSpec> specs;
		auto compileScalar = []( const char* expr, ExpressionProgram& out ) -> bool {
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			return b.Finalize( expr, out ) && out.IsValid();
		};
		ExpressionProgram progFlat = ExpressionProgram::Invalid();
		ExpressionProgram progWorn = ExpressionProgram::Invalid();
		Check( compileScalar( "osl_ao(0.07, 16, vec3(0,0,0), 3)", progFlat ),
			"compile worn_box flat AO weight" );
		Check( compileScalar( "1.0 - osl_ao(0.07, 16, vec3(0,0,0), 3)", progWorn ),
			"compile worn_box worn AO weight" );
		ExpressionScalarPainter* wFlat = new ExpressionScalarPainter( progFlat, specs );
		ExpressionScalarPainter* wWorn = new ExpressionScalarPainter( progWorn, specs );

		const IMaterial* kids[2] = { mFlat, mWorn };
		const IScalarPainter* wts[2] = { wFlat, wWorn };
		const IRayIntersectionModifier* mods[2] = { 0, pWornMod };
		IMaterial* mBoxMix = 0;
		Check( RISE_API_CreateMixMaterial( &mBoxMix, kids, wts, mods, 2, 0 ), "create AO-weighted mix" );

		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		BoxGeometry* boxGeo = new BoxGeometry( 1.0, 1.0, 1.0 );
		Object* boxObj = new Object( boxGeo );
		boxObj->SetPosition( Point3( 0.5, 0.5, 0.5 ) );
		boxObj->FinalizeTransformations();
		boxObj->AssignMaterial( *mBoxMix );
		om->AddItem( boxObj, "worn_box" );
		om->PrepareForRendering();

		RandomNumberGenerator rng( 2026 );
		IndependentSampler sampler( rng );

		// Center of top face (0.5, 0.5, 1.0): inside AO with R=0.07 is 1.0 -> 100% mFlat, no modifier!
		int centerFlat = 0;
		for( int i = 0; i < 64; ++i ) {
			RayIntersection ri( Ray( Point3( 0.5, 0.5, 1.5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRaySampled( ri, sampler );
			if( ri.pMaterial == mFlat && ri.pModifier == 0 ) ++centerFlat;
		}
		Check( centerFlat == 64, "worn_wooden_box motif: box center selects 100% flat leaf (no bump)" );

		// Near convex edge (0.01, 0.5, 1.0): inside AO with R=0.07 < 0.7 -> mixes worn leaf WITH modifier!
		int edgeWorn = 0;
		for( int i = 0; i < 200; ++i ) {
			RayIntersection ri( Ray( Point3( 0.01, 0.5, 1.5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRaySampled( ri, sampler );
			if( ri.pMaterial == mWorn && ri.pModifier == pWornMod ) ++edgeWorn;
		}
		Check( edgeWorn > 30 && edgeWorn < 180,
			"worn_wooden_box motif: convex edge stochastically selects worn leaf with Relief/Tilt modifier" );

		boxObj->release(); boxGeo->release(); om->release();
		mBoxMix->release(); wFlat->release(); wWorn->release();
		pWornMod->release(); mFlat->release(); mWorn->release();
		pFlatColor->release(); pWornColor->release();
	}

	RISEPel RenderCstScene( const std::string& sceneBody, const std::string& rasterizerBlock, const char* tag )
	{
		const std::filesystem::path tmpPath =
			std::filesystem::temp_directory_path() / ( std::string( "rise_mix_test_" ) + tag + ".RISEscene" );
		{
			std::ofstream ofs( tmpPath.string() );
			ofs << "RISE ASCII SCENE 7\n"
				<< "film\n{\n\twidth 16\n\theight 16\n}\n"
				<< "pinhole_camera\n{\n\tlocation 0 0 2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40\n}\n"
				<< "uniformcolor_painter\n{\n\tname p_env\n\tcolor 1.0 1.0 1.0\n\tcolorspace Rec709RGB_Linear\n}\n"
				<< "standard_shader\n{\n\tname s_def\n\tshaderop DefaultDirectLighting\n}\n"
				<< rasterizerBlock << "\n"
				<< sceneBody << "\n";
		}

		IJob* pJob = 0;
		Check( RISE_CreateJob( &pJob ) && pJob != 0, "create job for render" );
		const bool loaded = pJob->LoadAsciiSceneAuto( tmpPath.string().c_str() );
		std::error_code ec;
		std::filesystem::remove( tmpPath, ec );
		Check( loaded, tag );
		if( !loaded ) {
			pJob->release();
			return RISEPel( -1, -1, -1 );
		}

		BufferRasterizerOutput* pOut = new BufferRasterizerOutput();
		Check( pJob->AddCallbackRasterizerOutput( pOut ), "attach callback output" );
		Check( pJob->Rasterize(), "rasterize mix scene" );
		const RISEPel mean = pOut->CenterMean( 3 );
		pOut->release();
		pJob->release();
		return mean;
	}

	void TestEndToEndPTBDPTVCMFurnaceAndAbsorber()
	{
		// White furnace (uniform env = 1.0):
		//   A Lambertian quad with reflectance R facing a uniform L=1 hemisphere
		//   reflects exact radiance L_out = R!
		//
		// Case A (Round 3 P1-1 RED-PROOF):
		//   `0.25 White(0.8) + 0.75 none` (absorber!)
		//   Expected radiance = 0.25 * 0.8 = 0.20 in PT, BDPT, AND VCM!
		//   (Before Round 3 P1-1 fix, VCM stored photons on the `none` leaf and
		//    divided by q=0.25 -> 4x over-bright 0.80!)
		const std::string absorberScene =
			"uniformcolor_painter\n{\n\tname p_w8\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_w8\n\treflectance p_w8\n}\n"
			"mix_material\n{\n\tname m_abs\n\tchild m_w8 weight 0.25\n\tchild none weight 0.75\n}\n"
			"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_abs\n\tshader s_def\n}\n";

		const std::string ptBlock =
			"pathtracing_pel_rasterizer\n{\n\tsamples 64\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";
		const std::string bdptBlock =
			"bdpt_pel_rasterizer\n{\n\tsamples 64\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";
		const std::string vcmBlock =
			"vcm_pel_rasterizer\n{\n\tsamples 64\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tmerge_radius 0.5\n\tvc_enabled TRUE\n\tvm_enabled TRUE\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";

		const RISEPel ptAbs   = RenderCstScene( absorberScene, ptBlock, "pt_absorber" );
		const RISEPel bdptAbs = RenderCstScene( absorberScene, bdptBlock, "bdpt_absorber" );
		const RISEPel vcmAbs  = RenderCstScene( absorberScene, vcmBlock, "vcm_absorber" );
		CheckClose( ptAbs[0],   0.20, 0.03, "PT 0.25*0.8 + 0.75 none == 0.20" );
		CheckClose( bdptAbs[0], 0.20, 0.03, "BDPT 0.25*0.8 + 0.75 none == 0.20" );
		CheckClose( vcmAbs[0],  0.20, 0.03, "Round 3 P1-1: VCM (VC+VM, R=0.5) 0.25*0.8 + 0.75 none == 0.20 (NOT 4x over-bright)" );

		// Pure VM-only test (point light + NO env map -> s=0 is zero, VC is off,
		// so 100% of radiance comes strictly from VCM photon merges!):
		// Compare plain 0.20 Lambertian vs `0.25 * 0.8 + 0.75 none` mix.
		const std::string vmOnlyPointBlock =
			"vcm_pel_rasterizer\n{\n\tsamples 64\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tmerge_radius 0.5\n\tvc_enabled FALSE\n\tvm_enabled TRUE\n\tdefaultshader s_def\n\toidn_denoise FALSE\n}\n";
		const std::string pointLightChunk =
			"omni_light\n{\n\tname l_pt\n\tposition 0 0 2\n\tpower 50.26548245743669\n\tcolor 1 1 1\n}\n";
		const std::string plain020Scene = pointLightChunk +
			"uniformcolor_painter\n{\n\tname p_w2\n\tcolor 0.2 0.2 0.2\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_w2\n\treflectance p_w2\n}\n"
			"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_w2\n\tshader s_def\n}\n";
		const RISEPel vmPlain = RenderCstScene( plain020Scene, vmOnlyPointBlock, "vmonly_plain" );
		const RISEPel vmMix   = RenderCstScene( pointLightChunk + absorberScene, vmOnlyPointBlock, "vmonly_mix" );
		Check( vmPlain[0] > 0.03, "VM-only point-light baseline produces non-zero merge radiance" );
		CheckClose( vmMix[0], vmPlain[0], vmPlain[0] * 0.15,
			"Round 3 P1-1: pure VM-only photon merge on 0.25*0.8 + 0.75 none matches plain 0.20 (NOT 4x over-bright!)" );

		// Case B: 50/50 Red(0.8,0,0) + Blue(0,0,0.8) -> (0.40, 0, 0.40) in PT, BDPT, VCM
		const std::string blendScene =
			"uniformcolor_painter\n{\n\tname p_r\n\tcolor 0.8 0.0 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
			"uniformcolor_painter\n{\n\tname p_b\n\tcolor 0.0 0.0 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_r\n\treflectance p_r\n}\n"
			"lambertian_material\n{\n\tname m_b\n\treflectance p_b\n}\n"
			"mix_material\n{\n\tname m_rb\n\tchild m_r weight 0.5\n\tchild m_b weight 0.5\n}\n"
			"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_rb\n\tshader s_def\n}\n";

		const RISEPel ptBlend  = RenderCstScene( blendScene, ptBlock, "pt_blend" );
		const RISEPel vcmBlend = RenderCstScene( blendScene, vcmBlock, "vcm_blend" );
		CheckClose( ptBlend[0],  0.40, 0.03, "PT 50/50 Red+Blue R == 0.40" );
		CheckClose( ptBlend[2],  0.40, 0.03, "PT 50/50 Red+Blue B == 0.40" );
		CheckClose( vcmBlend[0], 0.40, 0.03, "VCM 50/50 Red+Blue R == 0.40" );
		CheckClose( vcmBlend[2], 0.40, 0.03, "VCM 50/50 Red+Blue B == 0.40" );

		// Case C: Pass-through mix `0.5 Red(0.8,0,0) + 0.5 passthrough` in front of Green(0,0.8,0)
		// Camera sees 50% front Red (0.8,0,0) + 50% back Green (illuminated through 50%-transparent front:
		// front blocks 50% of upper hemisphere above back quad, wait: use a small front quad or check
		// with uniform env + self-emissive green background for an exact closed form (0.4, 0.4, 0)!)
		const std::string passthroughScene =
			"uniformcolor_painter\n{\n\tname p_r\n\tcolor 0.8 0.0 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
			"uniformcolor_painter\n{\n\tname p_g\n\tcolor 0.0 0.8 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
			"uniformcolor_painter\n{\n\tname p_k\n\tcolor 0.0 0.0 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_r\n\treflectance p_r\n}\n"
			"lambertian_material\n{\n\tname m_k\n\treflectance p_k\n}\n"
			"lambertian_luminaire_material\n{\n\tname m_emit_g\n\tmaterial m_k\n\texitance p_g\n\tscale 3.141592653589793\n}\n"
			"mix_material\n{\n\tname m_pt\n\tchild m_r weight 0.5\n\tpassthrough weight 0.5\n}\n"
			"clippedplane_geometry\n{\n\tname g_front\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"clippedplane_geometry\n{\n\tname g_back\n\tpta -3 -3 -0.5\n\tptb 3 -3 -0.5\n\tptc 3 3 -0.5\n\tptd -3 3 -0.5\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_front\n\tgeometry g_front\n\tmaterial m_pt\n\tshader s_def\n}\n"
			"standard_object\n{\n\tname o_back\n\tgeometry g_back\n\tmaterial m_emit_g\n\tshader s_def\n}\n";

		const RISEPel ptPass = RenderCstScene( passthroughScene, ptBlock, "pt_passthrough" );
		// Front reflects 0.5 * 0.8 = 0.40 R; back emits radiance 0.8 G, seen 50% of time -> 0.40 G!
		CheckClose( ptPass[0], 0.40, 0.04, "PT passthrough mix front Red == 0.40" );
		CheckClose( ptPass[1], 0.40, 0.04, "PT passthrough mix background Green == 0.40" );

		// Case D (Slice 3 R1 A-P1-1 RED-PROOF): VCM VM-only with a zero-alpha
		// cutout connectible child (0.5 White(alpha=0) + 0.5 White(opaque)):
		// `qSum` MUST exclude the zero-alpha child so VM matches plain 0.40, NOT 2x dark 0.20!
		const std::string zeroAlphaCutoutScene = pointLightChunk +
			"uniformcolor_painter\n{\n\tname p_w8\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_cutout0\n\treflectance p_w8\n\talpha_coverage 0.0\n\talpha_mode mask\n\talpha_cutoff 0.5\n}\n"
			"lambertian_material\n{\n\tname m_opaque\n\treflectance p_w8\n}\n"
			"mix_material\n{\n\tname m_mix_cut\n\tchild m_cutout0 weight 0.5\n\tchild m_opaque weight 0.5\n}\n"
			"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_mix_cut\n\tshader s_def\n}\n";
		const std::string plain040Scene = pointLightChunk +
			"uniformcolor_painter\n{\n\tname p_w4\n\tcolor 0.4 0.4 0.4\n\tcolorspace Rec709RGB_Linear\n}\n"
			"lambertian_material\n{\n\tname m_w4\n\treflectance p_w4\n}\n"
			"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
			"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_w4\n\tshader s_def\n}\n";
		const RISEPel vmPlain40 = RenderCstScene( plain040Scene, vmOnlyPointBlock, "vmonly_plain40" );
		const RISEPel vmCutout  = RenderCstScene( zeroAlphaCutoutScene, vmOnlyPointBlock, "vmonly_cutout" );
		CheckClose( vmCutout[0], vmPlain40[0], vmPlain40[0] * 0.15,
			"R1 A-P1-1: VCM VM-only with zero-alpha cutout child matches plain 0.40 (NOT 2x dark 0.20)" );

		// Case E (Spectral PT + Spectral VCM parity on 0.25*0.8 + 0.75 none):
		const std::string ptSpecBlock =
			"pathtracing_spectral_rasterizer\n{\n\tsamples 32\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";
		const std::string vcmSpecBlock =
			"vcm_spectral_rasterizer\n{\n\tsamples 32\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tmerge_radius 0.5\n\tvc_enabled TRUE\n\tvm_enabled TRUE\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";
		const RISEPel ptSpecAbs  = RenderCstScene( absorberScene, ptSpecBlock, "pt_spec_abs" );
		const RISEPel vcmSpecAbs = RenderCstScene( absorberScene, vcmSpecBlock, "vcm_spec_abs" );
		CheckClose( ptSpecAbs[0],  0.20, 0.04, "Spectral PT (HWSS) 0.25*0.8 + 0.75 none == 0.20" );
		CheckClose( vcmSpecAbs[0], 0.20, 0.04, "Spectral VCM 0.25*0.8 + 0.75 none == 0.20" );
	}

	void TestRound1RedProofs()
	{
		UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		UniformScalarPainter* pHalf = new UniformScalarPainter( 0.5 );
		UniformScalarPainter* pOne  = new UniformScalarPainter( 1.0 );
		IMaterial* mLamb = 0;
		RISE_API_CreateLambertianMaterial( &mLamb, *pWhite );
		IMaterial* mDiffTrans = new DiffuseTransmissionMaterial( *pWhite, *pWhite );

		// 1) R1 B-P1-2: Direct RISE_API wrapper factories MUST refuse MixMaterial and null *ppi
		{
			const IMaterial* kids[2] = { mLamb, mDiffTrans };
			const IScalarPainter* wts[2] = { pHalf, pHalf };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, 0, 2, 0 ) && mMix != 0, "create mix for API refusals" );

			IMaterial* out = mLamb; // non-null sentinel to prove *ppi is nulled on failure
			Check( !RISE_API_CreateCompositeMaterial( &out, *mMix, *mLamb, 4, 2, 2, 2, 2, 1.0, *pOne ) && out == 0,
				"R1 B-P1-2: RISE_API_CreateCompositeMaterial refuses MixMaterial and nulls *ppi" );
			out = mLamb;
			Check( !RISE_API_CreateLambertianLuminaireMaterial( &out, *pWhite, *mMix, 1.0 ) && out == 0,
				"R1 B-P1-2: RISE_API_CreateLambertianLuminaireMaterial refuses MixMaterial and nulls *ppi" );
			out = mLamb;
			Check( !RISE_API_CreatePhongLuminaireMaterial( &out, *pWhite, *mMix, *pOne, 1.0 ) && out == 0,
				"R1 B-P1-2: RISE_API_CreatePhongLuminaireMaterial refuses MixMaterial and nulls *ppi" );

			// 2) R1 A-P1-4 / B-P1-4: AuditedMaterialGraph must accept MixMaterial + DiffuseTransmission
			//    so Extended SMS is NOT globally disabled!
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			BoxGeometry* boxGeo = new BoxGeometry( 1.0, 1.0, 1.0 );
			Object* obj = new Object( boxGeo );
			obj->AssignMaterial( *mMix );
			om->AddItem( obj, "mix_audited" );
			om->PrepareForRendering();
			Check( !om->HasRejectedTransmissiveCaster(),
				"R1 A-P1-4: MixMaterial(Lambertian + DiffuseTransmission) is audited (Extended SMS stays enabled)" );

			// 3) R1 B-P1-5: CloneMaterialForSnapshot produces an independent MixMaterial + cloned children
			const IMaterial* cloned = CloneMaterialForSnapshot( mMix );
			const MixMaterial* clonedMix = dynamic_cast<const MixMaterial*>( cloned );
			Check( cloned != 0 && cloned != mMix && clonedMix != 0,
				"R1 B-P1-5: CloneMaterialForSnapshot reconstructs a distinct MixMaterial" );
			if( clonedMix ) {
				Check( clonedMix->GetChildCount() == 2 && clonedMix->GetChild( 0 ).pMaterial != mLamb,
					"R1 B-P1-5: CloneMaterialForSnapshot clones child LambertianMaterial independently" );
			}
			if( cloned ) cloned->release();

			obj->release(); boxGeo->release(); om->release();
			mMix->release();
		}

		// 4) R1 B-P1-1: Reverse-order interior_medium -> SetObjectMaterial(mix) refused + DL-409 sheet refused
		{
			IJob* pJob = 0;
			Check( RISE_CreateJob( &pJob ) && pJob != 0, "create job for reverse-order refusal" );
			double cW[3] = { 0.8, 0.8, 0.8 };
			double pos[3] = { 0, 0, 0 }, ori[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 };
			double sa[3] = { 0.1, 0.1, 0.1 }, ss[3] = { 0.1, 0.1, 0.1 };
			RadianceMapConfig rmCfg;
			Check( pJob->AddUniformColorPainter( "p_w", cW, "sRGB" ), "add p_w" );
			Check( pJob->AddLambertianMaterial( "m_l", "p_w" ), "add m_l" );
			Check( pJob->AddRandomWalkSSSMaterial( "m_rw", "1.4", "0.1 0.1 0.1", "1.0 1.0 1.0", "0.0", "0.1", "32" ), "add m_rw" );
			const char* cKids[1] = { "m_l" };
			const char* cWts[1]  = { "1.0" };
			Check( pJob->AddMixMaterial( "m_mix", cKids, cWts, 0, 1, 0 ), "add m_mix" );
			const char* rwKids[1] = { "m_rw" };
			Check( pJob->AddMixMaterial( "m_mix_rw", rwKids, cWts, 0, 1, 0 ), "add m_mix_rw" );
			Check( pJob->AddBoxGeometry( "g_b", 1, 1, 1 ), "add box" );
			const float verts[12] = { -1, -1, 0,  1, -1, 0,  1, 1, 0,  -1, 1, 0 };
			const float norms[3]  = { 0, 0, 1 };
			const float uvs[8]    = { 0, 0,  1, 0,  1, 1,  0, 1 };
			const unsigned int vf[6] = { 0, 1, 2,  0, 2, 3 };
			const unsigned int nf[6] = { 0, 0, 0,  0, 0, 0 };
			Check( pJob->AddIndexedTriangleMeshGeometry( "g_sheet", verts, norms, uvs, vf, vf, nf, 4, 1, 4, 2, true, true ), "add flat mesh sheet" );
			Check( pJob->AddHomogeneousMedium( "med_a", sa, ss, "isotropic", 0.0 ), "add med" );
			Check( pJob->AddObject( "o_med", "g_b", "m_l", 0, 0, rmCfg, pos, ori, sc, true, true ), "add o_med" );
			Check( pJob->SetObjectInteriorMedium( "o_med", "med_a" ), "attach med first" );
			Check( !pJob->SetObjectMaterial( "o_med", "m_mix" ),
				"R1 B-P1-1: reverse-order SetObjectMaterial(mix) on object with interior_medium refused" );
			Check( !pJob->AddObject( "o_sheet_rw_direct", "g_sheet", "m_rw", 0, 0, rmCfg, pos, ori, sc, true, true ),
				"DL-409 refuses bare RandomWalkSSS on zero-volume mesh sheet" );
			Check( !pJob->AddObject( "o_sheet_rw", "g_sheet", "m_mix_rw", 0, 0, rmCfg, pos, ori, sc, true, true ),
				"R1 B-P1-7: DL-409 refuses RandomWalkSSS leaf inside MixMaterial on zero-volume mesh sheet" );
			pJob->release();
		}

		// 5) R1 A-P1-3: EvaluateExpectedGuides preserves coherent UV tangent frame (onb) for UV-domain ReliefModifier
		{
			std::vector<ParamSpec> specs;
			ExpressionProgram progU = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( b.Finalize( "u", progU ) && progU.IsValid(), "compile h(u,v)=u" );
			ExpressionScalarPainter* spU = new ExpressionScalarPainter( progU, specs );
			ReliefModifier* pUvRelief = new ReliefModifier( *spU, 0.5, ReliefDomain::UV, 0.01, 0.0 );

			// Tilted ClippedPlaneGeometry whose UV tangent U points along +Y (NOT canonical CreateFromW X!)
			Point3 pts[4] = { Point3(0,0,0), Point3(0,2,0), Point3(2,2,0), Point3(2,0,0) };
			ClippedPlaneGeometry* plane = new ClippedPlaneGeometry( pts, true );
			Object* obj = new Object( plane );
			const IMaterial* kids[2] = { mLamb, mLamb };
			const IScalarPainter* wts[2] = { pHalf, pHalf };
			const IRayIntersectionModifier* mods[2] = { 0, pUvRelief };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, kids, wts, mods, 2, 0 ), "create UV relief mix" );
			obj->AssignMaterial( *mMix );
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			om->AddItem( obj, "uv_plane" );
			om->PrepareForRendering();

			RayIntersection ri( Ray( Point3( 1.0, 1.0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit, "hit uv_plane" );
			// Ground truth: apply pUvRelief directly to unmodified ri.geometric
			RayIntersectionGeometric gtGeom( ri.geometric );
			const Vector3 nFlat = gtGeom.vNormal;
			pUvRelief->Modify( gtGeom );
			const Vector3 nRef = Vector3Ops::Normalize( nFlat * 0.5 + gtGeom.vNormal * 0.5 );

			// Now simulate Accurate AOV: modify ri.geometric first, then evaluate expected guides!
			if( ri.pModifier ) ri.pModifier->Modify( ri.geometric );
			RISEPel gAlb( 0, 0, 0 );
			Vector3 gNorm( 0, 0, 0 );
			EvaluateMaterialAOVGuides( ri, ri.geometric, false, 0, gAlb, gNorm );
			CheckClose( gNorm.x, nRef.x, 1e-6, "R1 A-P1-3: UV relief expected guide normal X preserves coherent ONB" );
			CheckClose( gNorm.y, nRef.y, 1e-6, "R1 A-P1-3: UV relief expected guide normal Y preserves coherent ONB" );
			CheckClose( gNorm.z, nRef.z, 1e-6, "R1 A-P1-3: UV relief expected guide normal Z preserves coherent ONB" );

			om->release(); obj->release(); plane->release(); mMix->release();
			pUvRelief->release(); spU->release();
		}

		// 6) R1 B-P1-3 + Wave D: CST ReferenceGraph traces mix_material child & passthrough edges
		//    and DocRename rewrites material/weight/modifier/passthrough tokens cleanly with zero dangling refs.
		{
			const std::string src =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n{\n\tname p_a\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n"
				"lambertian_material\n{\n\tname m_a\n\treflectance p_a\n}\n"
				"scalar_painter\n{\n\tname w_a\n\tvalue 0.5\n}\n"
				"scalar_painter\n{\n\tname w_p\n\tvalue 0.25\n}\n"
				"relief_modifier\n{\n\tname mod_a\n\theight w_a\n\tscale 0.1\n}\n"
				"mix_material\n{\n\tname m_mix\n\tchild m_a weight w_a modifier mod_a\n\tpassthrough weight w_p\n}\n";
			std::vector<std::string> diags;
			Cst::Document doc = Cst::ParseToCst( src );
			Cst::ReferenceGraph g = Cst::BuildReferenceGraph( doc, &diags );
			Check( g.edges.size() >= 6, "R1 B-P1-3: CST BuildReferenceGraph traces mix_material child mat/weight/mod + passthrough edges" );
			std::vector<std::string> renameDiags;
			Cst::NodeId maId = Cst::DocFindByName( doc, "lambertian_material/m_a" );
			Check( maId != 0, "find m_a chunk in CST" );
			Cst::Document renamed = Cst::DocRename( doc, maId, "m_a_renamed", &renameDiags );
			Check( renameDiags.empty(), "Wave D: CST DocRename succeeds on document with mix_material" );
			Cst::NodeId waId = Cst::DocFindByName( renamed, "scalar_painter/w_a" );
			renamed = Cst::DocRename( renamed, waId, "w_a_renamed", &renameDiags );
			Cst::NodeId modId = Cst::DocFindByName( renamed, "relief_modifier/mod_a" );
			renamed = Cst::DocRename( renamed, modId, "mod_a_renamed", &renameDiags );
			Cst::NodeId wpId = Cst::DocFindByName( renamed, "scalar_painter/w_p" );
			renamed = Cst::DocRename( renamed, wpId, "w_p_renamed", &renameDiags );
			Check( renameDiags.empty(), "Wave D: CST DocRename rewrites mat/weight/modifier/passthrough in mix_material with 0 diagnostics" );
			const std::string outText = Cst::SerializeCst( renamed );
			Check( outText.find( "child m_a_renamed weight w_a_renamed modifier mod_a_renamed" ) != std::string::npos,
				"Wave D: DocRename rewrote child mat, weight, and modifier tokens in mix_material" );
			Check( outText.find( "passthrough weight w_p_renamed" ) != std::string::npos,
				"Wave D: DocRename rewrote passthrough weight token in mix_material" );
			std::vector<std::string> postDiags;
			std::vector<Cst::UnresolvedReference> postUnres;
			Cst::BuildReferenceGraph( renamed, &postDiags, &postUnres );
			Check( postDiags.empty() && postUnres.empty(),
				"Wave D: renamed mix_material document has zero unresolved references" );
		}

		mDiffTrans->release(); mLamb->release();
		pWhite->release(); pHalf->release(); pOne->release();
	}

	void TestRound2RedProofs()
	{
		// 1. R2 P1-1: SSS contact discovery (`FindExitContact`) on a touching
		//    MixMaterial with a zero-alpha cutout leaf (0.5) + opaque leaf (0.5).
		//    Every probe point along the touching plane must discover the neighbor
		//    instead of dropping ~50% of points where the deterministic fallback
		//    draw hashes onto the zero-alpha leaf.
		{
			BoxGeometry* pBoxSss = new BoxGeometry( 2.0, 2.0, 2.0 );
			BoxGeometry* pBoxMix = new BoxGeometry( 2.0, 2.0, 2.0 );
			UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );
			UniformScalarPainter* pHalf = new UniformScalarPainter( 0.5 );
			UniformScalarPainter* pZero = new UniformScalarPainter( 0.0 );
			UniformScalarPainter* pIor = new UniformScalarPainter( 1.3 );
			UniformScalarPainter* pAbs = new UniformScalarPainter( 0.1 );
			UniformScalarPainter* pScat = new UniformScalarPainter( 1.0 );

			IMaterial* mSss = 0;
			RISE_API_CreateRandomWalkSSSMaterial( &mSss, *pIor, *pAbs, *pScat, Scalar( 0.0 ), Scalar( 0.0 ), 16 );
			IMaterial* mOpaque = 0;
			RISE_API_CreateLambertianMaterial( &mOpaque, *pWhite );
			IMaterial* mZeroAlpha = 0;
			RISE_API_CreateLambertianMaterial( &mZeroAlpha, *pWhite );
			mZeroAlpha->SetAlpha( pZero, eAlphaMask, 0.5 );

			const IMaterial* mats[2] = { mOpaque, mZeroAlpha };
			const IScalarPainter* wts[2] = { pHalf, pHalf };
			IMaterial* mMix = 0;
			Check( RISE_API_CreateMixMaterial( &mMix, mats, wts, 0, 2, 0 ), "R2 P1-1: create opaque+zero-alpha mix" );

			Object* oSss = new Object( pBoxSss );
			oSss->SetPosition( Point3( 0, 0, -1 ) );
			oSss->FinalizeTransformations();
			oSss->AssignMaterial( *mSss );

			Object* oMix = new Object( pBoxMix );
			oMix->SetPosition( Point3( 0, 0, 1 ) );
			oMix->FinalizeTransformations();
			oMix->AssignMaterial( *mMix );

			ObjectManager* om = new ObjectManager( false, false, 4, 32 );
			om->AddItem( oSss, "o_sss" );
			om->AddItem( oMix, "o_mix" );
			om->PrepareForRendering();

			int foundCount = 0;
			const int kProbes = 32;
			for( int i = 0; i < kProbes; ++i ) {
				const Scalar x = -0.8 + 1.6 * ( Scalar( i ) + 0.5 ) / Scalar( kProbes );
				const Scalar y = -0.5 + 1.0 * ( Scalar( ( i * 7 ) % kProbes ) + 0.5 ) / Scalar( kProbes );
				const IObject* partner = BSSRDFSampling::FindSurfaceContact(
					*om, Point3( x, y, 0.0 ), Vector3( 0, 0, 1 ), oSss, Vector3( 0, 0, 1 ) );
				if( partner == oMix ) {
					++foundCount;
				}
			}
			Check( foundCount == kProbes,
				"R2 P1-1: FindSurfaceContact discovers opaque+zero-alpha MixMaterial at 100% of touching points" );

			om->release();
			oSss->release(); oMix->release();
			mMix->release(); mOpaque->release(); mZeroAlpha->release(); mSss->release();
			pBoxSss->release(); pBoxMix->release();
			pWhite->release(); pHalf->release(); pZero->release();
			pIor->release(); pAbs->release(); pScat->release();
		}

		// 2. R2 P1-2: IntersectShadowRaySampled hitting an opaque shadow-casting SSS
		//    box covered flush by a coincident non-shadow-casting decal MUST shadow!
		{
			BoxGeometry* pBoxSss = new BoxGeometry( 2.0, 2.0, 2.0 );
			const Point3 decalPts[4] = {
				Point3( -1, -1, 0 ), Point3( 1, -1, 0 ), Point3( 1, 1, 0 ), Point3( -1, 1, 0 )
			};
			ClippedPlaneGeometry* pDecal = new ClippedPlaneGeometry( decalPts, true );
			UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );
			UniformScalarPainter* pIor = new UniformScalarPainter( 1.3 );
			UniformScalarPainter* pAbs = new UniformScalarPainter( 0.1 );
			UniformScalarPainter* pScat = new UniformScalarPainter( 1.0 );
			UniformScalarPainter* pHalf = new UniformScalarPainter( 0.5 );

			IMaterial* mSss = 0;
			RISE_API_CreateRandomWalkSSSMaterial( &mSss, *pIor, *pAbs, *pScat, Scalar( 0.0 ), Scalar( 0.0 ), 16 );
			IMaterial* mDecal = 0;
			RISE_API_CreateLambertianMaterial( &mDecal, *pWhite );
			// Register a dummy alpha material so IntersectShadowRaySampled enters the sampled loop.
			IMaterial* mAlpha = 0;
			RISE_API_CreateLambertianMaterial( &mAlpha, *pWhite );
			mAlpha->SetAlpha( pHalf, eAlphaBlend, 0.5 );

			Object* oSss = new Object( pBoxSss );
			oSss->SetPosition( Point3( 0, 0, -1 ) );
			oSss->FinalizeTransformations();
			oSss->SetShadowParams( true, true );
			oSss->AssignMaterial( *mSss );

			Object* oDecal = new Object( pDecal );
			oDecal->SetPosition( Point3( 0, 0, 0 ) );
			oDecal->FinalizeTransformations();
			oDecal->SetShadowParams( false, true );
			oDecal->AssignMaterial( *mDecal );

			ObjectManager* om = new ObjectManager( false, false, 4, 32 );
			// Name `o_a_sss` before `o_z_decal` so GenericManager's name-ordered
			// linear traversal hits the SSS body first and enters
			// ResolveCoincidentEntry -> hasCoincidentFallback.
			om->AddItem( oSss, "o_a_sss" );
			om->AddItem( oDecal, "o_z_decal" );
			om->PrepareForRendering();

			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );
			Ray shadowRay( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) );
			// Distance 3.0 terminates at z = -1 (strictly before oSss's back face at z = -2, t = 4.0)
			// so this genuinely fails if the coincident z = 0 fallback to oSss is skipped!
			const bool shadowed = om->IntersectShadowRaySampled( shadowRay, 3.0, sampler );
			Check( shadowed,
				"R2 P1-2: IntersectShadowRaySampled shadows on opaque SSS body covered by non-shadow-casting coincident decal" );

			om->release();
			oSss->release(); oDecal->release();
			mAlpha->release(); mDecal->release(); mSss->release();
			pBoxSss->release(); pDecal->release();
			pWhite->release(); pIor->release(); pAbs->release(); pScat->release(); pHalf->release();
		}

		// 3. R2 P1-3: Refused incremental repoint in Job::AddObject and Job::AddObjectMatrix
		//    must NOT mutate the existing object's geometry before refusing!
		{
			IJob* pJob = 0;
			RISE_CreateJob( &pJob );
			IJobPriv* pJobPriv = dynamic_cast<IJobPriv*>( pJob );
			Check( pJobPriv != 0, "R2 P1-3: IJobPriv available" );
			if( pJobPriv ) {
				const double white[3] = { 0.8, 0.8, 0.8 };
				const double zero[3] = { 0, 0, 0 };
				const double one[3] = { 1, 1, 1 };
				const double sigma[3] = { 0.1, 0.1, 0.1 };
				const double eye[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
				pJob->AddUniformColorPainter( "p_w", white, "Rec709RGB_Linear" );
				pJob->AddLambertianMaterial( "m_l", "p_w" );
				const char* mats[1] = { "m_l" };
				const char* wts[1] = { "1.0" };
				pJob->AddMixMaterial( "m_mix", mats, wts, 0, 1, 0 );
				pJob->AddBoxGeometry( "g_orig", 1.0, 1.0, 1.0 );
				pJob->AddBoxGeometry( "g_other", 2.0, 2.0, 2.0 );
				pJob->AddHomogeneousMedium( "med", sigma, sigma, "isotropic", 0.0 );
				RadianceMapConfig rmc;
				Check( pJob->AddObject( "o_test", "g_orig", "m_l", 0, 0, rmc, zero, zero, one, true, true ),
					"R2 P1-3: create initial object" );
				Check( pJob->SetObjectInteriorMedium( "o_test", "med" ), "R2 P1-3: attach interior_medium" );
				const IGeometry* origGeom = pJobPriv->GetObjects()->GetItem( "o_test" )->GetGeometry();
				Check( origGeom != 0, "R2 P1-3: original geometry non-null" );

				pJob->SetIncrementalRepointMode( true );
				Check( !pJob->AddObject( "o_test", "g_other", "m_mix", 0, 0, rmc, zero, zero, one, true, true ),
					"R2 P1-3: AddObject repoint refuses mix_material onto object with interior_medium" );
				Check( pJobPriv->GetObjects()->GetItem( "o_test" )->GetGeometry() == origGeom,
					"R2 P1-3: refused AddObject repoint leaves existing geometry untouched" );

				Check( !pJob->AddObjectMatrix( "o_test", "g_other", "m_mix", 0, 0, rmc, eye, true, true ),
					"R2 P1-3: AddObjectMatrix repoint refuses mix_material onto object with interior_medium" );
				Check( pJobPriv->GetObjects()->GetItem( "o_test" )->GetGeometry() == origGeom,
					"R2 P1-3: refused AddObjectMatrix repoint leaves existing geometry untouched" );
				pJob->SetIncrementalRepointMode( false );
			}
			pJob->release();
		}

		// 4. R2 P2-1: CSGObject::AssignObjects refuses attaching a MixMaterial operand
		//    when the CSGObject already owns an interior_medium and no direct material.
		{
			BoxGeometry* pBox = new BoxGeometry( 1.0, 1.0, 1.0 );
			UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );
			UniformScalarPainter* pOne = new UniformScalarPainter( 1.0 );
			IMaterial* mLamb = 0;
			RISE_API_CreateLambertianMaterial( &mLamb, *pWhite );
			const IMaterial* mats[1] = { mLamb };
			const IScalarPainter* wts[1] = { pOne };
			IMaterial* mMix = 0;
			RISE_API_CreateMixMaterial( &mMix, mats, wts, 0, 1, 0 );

			Object* oMix = new Object( pBox );
			oMix->AssignMaterial( *mMix );
			Object* oLamb = new Object( pBox );
			oLamb->AssignMaterial( *mLamb );

			IPhaseFunction* pPhase = 0;
			RISE_API_CreateIsotropicPhaseFunction( &pPhase );
			IMedium* pMed = 0;
			RISE_API_CreateHomogeneousMedium( &pMed, RISEPel( 0.1, 0.1, 0.1 ), RISEPel( 0.1, 0.1, 0.1 ), *pPhase );

			CSGObject* pCsg = new CSGObject( CSG_UNION );
			Check( pCsg->AssignInteriorMedium( *pMed ), "R2 P2-1: empty CSGObject accepts interior_medium" );
			Check( !pCsg->AssignObjects( oMix, oLamb ),
				"R2 P2-1: CSGObject::AssignObjects refuses MixMaterial operand when interior_medium is set" );
			Check( pCsg->AssignObjects( oLamb, oLamb ),
				"R2 P2-1: CSGObject::AssignObjects accepts non-Mix operands" );

			pCsg->release();
			pMed->release(); pPhase->release();
			oMix->release(); oLamb->release();
			mMix->release(); mLamb->release();
			pWhite->release(); pOne->release(); pBox->release();
		}

		// 5. R2 P2-5: Pure VM-only VCM render on MixMaterial with mixed partial-alpha
		//    connectible leaves (0.5 * alpha=0.2 + 0.5 * alpha=0.8 -> expected 0.5 * 0.8 = 0.40).
		{
			const std::string vmOnlyPointBlock =
				"vcm_pel_rasterizer\n{\n\tsamples 64\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tmerge_radius 0.5\n\tvc_enabled FALSE\n\tvm_enabled TRUE\n\tdefaultshader s_def\n\toidn_denoise FALSE\n}\n";
			const std::string pointLightChunk =
				"omni_light\n{\n\tname l_pt\n\tposition 0 0 2\n\tpower 50.26548245743669\n\tcolor 1 1 1\n}\n";
			const std::string plain040Scene = pointLightChunk +
				"uniformcolor_painter\n{\n\tname p_w4\n\tcolor 0.4 0.4 0.4\n\tcolorspace Rec709RGB_Linear\n}\n"
				"lambertian_material\n{\n\tname m_w4\n\treflectance p_w4\n}\n"
				"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
				"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_w4\n\tshader s_def\n}\n";
			const std::string partialAlphaMixScene = pointLightChunk +
				"uniformcolor_painter\n{\n\tname p_w8\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
				"lambertian_material\n{\n\tname m_a2\n\treflectance p_w8\n\talpha_coverage 0.2\n\talpha_mode blend\n}\n"
				"lambertian_material\n{\n\tname m_a8\n\treflectance p_w8\n\talpha_coverage 0.8\n\talpha_mode blend\n}\n"
				"mix_material\n{\n\tname m_pa\n\tchild m_a2 weight 0.5\n\tchild m_a8 weight 0.5\n}\n"
				"clippedplane_geometry\n{\n\tname g_q\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n\tdoublesided TRUE\n}\n"
				"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_pa\n\tshader s_def\n}\n";
			const RISEPel vmPlain = RenderCstScene( plain040Scene, vmOnlyPointBlock, "r2_vmonly_plain040" );
			const RISEPel vmPartial = RenderCstScene( partialAlphaMixScene, vmOnlyPointBlock, "r2_vmonly_partial_alpha" );
			Check( vmPlain[0] > 0.05, "R2 P2-5: VM-only plain 0.40 baseline positive" );
			CheckClose( vmPartial[0], vmPlain[0], vmPlain[0] * 0.18,
				"R2 P2-5: VM-only partial-alpha MixMaterial (0.5*0.2 + 0.5*0.8)*0.8 matches plain 0.40" );
		}
	}

	void TestWaveBCDContainmentNearClipAndGUI()
	{
		// 1) Wave B Part 0: PathDependence bit propagation & BDPT/VCM/MLT refusal
		{
			const std::string lwScene =
				"RISE ASCII SCENE 7\n"
				"film\n{\n\twidth 8\n\theight 8\n}\n"
				"pinhole_camera\n{\n\tlocation 0 0 2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45\n}\n"
				"uniformcolor_painter\n{\n\tname p_env\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n"
				"expression_painter\n{\n\tname p_lw\n\texpr vec3(osl_layer_weight(0.5, vec3(0,0,0), 0), 0.2, 0.2)\n}\n"
				"expression_painter\n{\n\tname p_ao\n\texpr vec3(osl_ao(0.5, 4, vec3(0,0,0), 0), 0.5, 0.5)\n}\n"
				"lambertian_material\n{\n\tname m_lw\n\treflectance p_lw\n}\n"
				"lambertian_material\n{\n\tname m_ao\n\treflectance p_ao\n}\n"
				"standard_shader\n{\n\tname s_def\n\tshaderop DefaultPathTracing\n}\n"
				"clippedplane_geometry\n{\n\tname g_q\n\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n\tdoublesided TRUE\n}\n"
				"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_lw\n\tshader s_def\n}\n"
				"bdpt_pel_rasterizer\n{\n\tsamples 4\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tdefaultshader s_def\n\tradiance_map p_env\n\toidn_denoise FALSE\n}\n";

			const std::string tmpPath = ( std::filesystem::temp_directory_path() / "rise_waveb_pathdep_bdpt.RISEscene" ).string();
			{ std::ofstream f( tmpPath ); f << lwScene; }
			IJob* pJob = 0;
			RISE_CreateJob( &pJob );
			Check( pJob->LoadAsciiSceneAuto( tmpPath.c_str() ), "Wave B: load osl_layer_weight BDPT scene" );
			IJobPriv* pPriv = dynamic_cast<IJobPriv*>( pJob );
			IMaterial* mLw = pPriv->GetMaterials()->GetItem( "m_lw" );
			IMaterial* mAo = pPriv->GetMaterials()->GetItem( "m_ao" );
			Check( mLw && ( mLw->GetPathDependence() & PathDependence::kViewDirection ) != 0u,
				"Wave B: osl_layer_weight propagates PathDependence::kViewDirection to LambertianMaterial" );
			Check( mAo && mAo->GetPathDependence() == PathDependence::kNone,
				"Wave B: osl_ao is physically based (PathDependence::kNone per Part 0)" );
			const ObjectManager* om = dynamic_cast<const ObjectManager*>( pPriv->GetScene()->GetObjects() );
			Check( om && ( om->ComputeScenePathDependence() & PathDependence::kViewDirection ) != 0u,
				"Wave B: ObjectManager::ComputeScenePathDependence detects kViewDirection" );
			const PixelBasedRasterizerHelper* bdptHelper =
				dynamic_cast<const PixelBasedRasterizerHelper*>( pPriv->GetRasterizer() );
			Check( bdptHelper && !bdptHelper->ValidateSceneForRasterizer( *pPriv->GetScene() ),
				"Wave B: BDPT ValidateSceneForRasterizer refuses osl_layer_weight scene" );
			pJob->release();
			std::filesystem::remove( tmpPath );
		}

		// 2) Wave B Part 0: Spatially varying SSS MixMaterial refused in VCM, allowed when constant
		{
			const std::string sssMixBody =
				"RISE ASCII SCENE 7\n"
				"film\n{\n\twidth 8\n\theight 8\n}\n"
				"pinhole_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45\n}\n"
				"uniformcolor_painter\n{\n\tname p_w\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
				"lambertian_material\n{\n\tname m_l\n\treflectance p_w\n}\n"
				"randomwalk_sss_material\n{\n\tname m_sss\n\tior 1.3\n\tabsorption 0.2\n\tscattering 1.0\n\tg 0.0\n\troughness 0.1\n\tmax_bounces 8\n}\n"
				"scalar_painter\n{\n\tname sp_var\n\texpression clamp(u, 0.0, 1.0)\n}\n"
				"scalar_painter\n{\n\tname sp_const\n\tvalue 0.5\n}\n"
				"mix_material\n{\n\tname m_sss_var\n\tchild m_sss weight sp_var\n\tchild m_l weight 0.2\n}\n"
				"mix_material\n{\n\tname m_sss_const\n\tchild m_sss weight sp_const\n\tchild m_l weight 0.5\n}\n"
				"standard_shader\n{\n\tname s_def\n\tshaderop DefaultPathTracing\n}\n"
				"box_geometry\n{\n\tname g_b\n\twidth 1\n\theight 1\n\tdepth 1\n}\n"
				"standard_object\n{\n\tname o_b\n\tgeometry g_b\n\tmaterial m_sss_var\n\tshader s_def\n}\n"
				"vcm_pel_rasterizer\n{\n\tsamples 4\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tmerge_radius 0.2\n\tdefaultshader s_def\n\toidn_denoise FALSE\n}\n";

			const std::string tmpPath = ( std::filesystem::temp_directory_path() / "rise_waveb_sss_mix_vcm.RISEscene" ).string();
			{ std::ofstream f( tmpPath ); f << sssMixBody; }
			IJob* pJob = 0;
			RISE_CreateJob( &pJob );
			Check( pJob->LoadAsciiSceneAuto( tmpPath.c_str() ), "Wave B: load spatially varying SSS mix_material scene" );
			IJobPriv* pPriv = dynamic_cast<IJobPriv*>( pJob );
			IMaterial* mVar = pPriv->GetMaterials()->GetItem( "m_sss_var" );
			IMaterial* mCon = pPriv->GetMaterials()->GetItem( "m_sss_const" );
			Check( mVar && ( mVar->GetPathDependence() & PathDependence::kVaryingSSSMix ) != 0u,
				"Wave B: spatially varying SSS MixMaterial sets PathDependence::kVaryingSSSMix" );
			Check( mCon && ( mCon->GetPathDependence() & PathDependence::kVaryingSSSMix ) == 0u,
				"Wave B: constant-weight SSS MixMaterial does not set kVaryingSSSMix" );
			const PixelBasedRasterizerHelper* vcmHelper =
				dynamic_cast<const PixelBasedRasterizerHelper*>( pPriv->GetRasterizer() );
			Check( vcmHelper && !vcmHelper->ValidateSceneForRasterizer( *pPriv->GetScene() ),
				"Wave B: VCM ValidateSceneForRasterizer refuses spatially varying SSS MixMaterial" );
			pJob->release();
			std::filesystem::remove( tmpPath );
		}

		// 3) Wave B Option A: Camera near_clip parsing, CameraUtilities, CameraIntrospection & AOV depth
		{
			const std::string ncScene =
				"RISE ASCII SCENE 7\n"
				"film\n{\n\twidth 8\n\theight 8\n}\n"
				"pinhole_camera\n{\n\tname cam_nc\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45\n\tnear_clip 1.5\n}\n"
				"uniformcolor_painter\n{\n\tname p_w\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n"
				"lambertian_material\n{\n\tname m_w\n\treflectance p_w\n}\n"
				"standard_shader\n{\n\tname s_def\n\tshaderop DefaultPathTracing\n}\n"
				"clippedplane_geometry\n{\n\tname g_q\n\tpta -4 -4 0\n\tptb 4 -4 0\n\tptc 4 4 0\n\tptd -4 4 0\n\tdoublesided TRUE\n}\n"
				"standard_object\n{\n\tname o_q\n\tgeometry g_q\n\tmaterial m_w\n\tshader s_def\n}\n";
			const std::string tmpPath = ( std::filesystem::temp_directory_path() / "rise_waveb_nearclip.RISEscene" ).string();
			{ std::ofstream f( tmpPath ); f << ncScene; }
			IJob* pJob = 0;
			RISE_CreateJob( &pJob );
			Check( pJob->LoadAsciiSceneAuto( tmpPath.c_str() ), "Wave B/D: parse pinhole_camera with near_clip 1.5" );
			IJobPriv* pPriv = dynamic_cast<IJobPriv*>( pJob );
			ICamera* cam = pPriv->GetCameras()->GetItem( "cam_nc" );
			const CameraCommon* cc = dynamic_cast<const CameraCommon*>( cam );
			Check( cc && std::fabs( cc->GetNearClip() - 1.5 ) < 1e-9, "Wave D: near_clip parsed onto CameraCommon" );
			Check( !BDPTCameraUtilities::IsPointPastNearClip( *cam, Point3( 0, 0, 3.0 ) ),
				"Wave B Option A: point at z=3 (dist 1.0 < 1.5) rejected by IsPointPastNearClip" );
			Check( BDPTCameraUtilities::IsPointPastNearClip( *cam, Point3( 0, 0, 2.0 ) ),
				"Wave B Option A: point at z=2 (dist 2.0 > 1.5) accepted by IsPointPastNearClip" );
			const Vector3 centerDir( 0, 0, -1 );
			CheckClose( BDPTCameraUtilities::ComputeNearClipDistance( *cam, centerDir ), 1.5, 1e-9,
				"Wave B Option A: ComputeNearClipDistance along optical axis == 1.5" );
			const Point3 visEnd = BDPTCameraUtilities::ApplyNearClipToConnectionEndpoint( *cam, Point3( 0, 0, 4 ), centerDir );
			CheckClose( visEnd.z, 2.5, 1e-9, "Wave B Option A: visibility endpoint sits at z = 4 - 1.5 = 2.5" );
			// CameraIntrospection read/write & snapshot round-trip
			Check( CameraIntrospection::GetPropertyValue( *cam, String( "near_clip" ) ) == String( "1.5" ),
				"Wave D: CameraIntrospection reads near_clip" );
			Check( !CameraIntrospection::SetProperty( *cam, String( "near_clip" ), String( "-1.0" ) ),
				"Wave D: CameraIntrospection rejects negative near_clip" );
			Check( CameraIntrospection::SetProperty( *cam, String( "near_clip" ), String( "0.75" ) ),
				"Wave D: CameraIntrospection updates near_clip" );
			CheckClose( cc->GetNearClip(), 0.75, 1e-9, "Wave D: near_clip updated to 0.75" );
			CameraSnapshot snap;
			Check( CameraIntrospection::CaptureCameraSnapshot( *cam, snap ) && std::fabs( snap.nearClip - 0.75 ) < 1e-9,
				"Wave D: CameraSnapshot captures nearClip" );
			Check( CameraIntrospection::AddCameraFromSnapshot( *pJob, String( "cam_nc_clone" ), snap ),
				"Wave D: AddCameraFromSnapshot clones camera with nearClip" );
			const CameraCommon* cloneCC = dynamic_cast<const CameraCommon*>( pPriv->GetCameras()->GetItem( "cam_nc_clone" ) );
			Check( cloneCC && std::fabs( cloneCC->GetNearClip() - 0.75 ) < 1e-9,
				"Wave D: cloned camera preserves nearClip == 0.75" );
			pJob->release();
			std::filesystem::remove( tmpPath );
		}

		// 4) Wave D: diffuse_transmission_material + scalar_painter { channel RGB } + MaterialIntrospection
		//    + ConnectionLegality + MaterialPreview + PainterPreview
		{
			const std::string guiScene =
				"RISE ASCII SCENE 7\n"
				"film\n{\n\twidth 16\n\theight 16\n}\n"
				"pinhole_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45\n}\n"
				"uniformcolor_painter\n{\n\tname p_red\n\tcolor 0.9 0.0 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
				"uniformcolor_painter\n{\n\tname p_blue\n\tcolor 0.0 0.0 0.9\n\tcolorspace Rec709RGB_Linear\n}\n"
				"uniformcolor_painter\n{\n\tname p_green\n\tcolor 0.0 0.8 0.0\n\tcolorspace Rec709RGB_Linear\n}\n"
				"scalar_painter\n{\n\tname sp_rgb_bridge\n\tpainter p_red\n\tchannel RGB\n}\n"
				"expression_painter\n{\n\tname p_camray\n\texpr vec3(osl_is_camera_ray(), osl_is_camera_ray(), osl_is_camera_ray())\n}\n"
				"expression_painter\n{\n\tname p_lw_prev\n\texpr vec3(osl_layer_weight(0.5, vec3(0,0,0), 0), 0.0, 0.0)\n}\n"
				"lambertian_material\n{\n\tname m_red\n\treflectance p_red\n}\n"
				"lambertian_material\n{\n\tname m_blue\n\treflectance p_blue\n}\n"
				"diffuse_transmission_material\n{\n\tname m_dt\n\treflectance p_red\n\ttransmittance p_green\n}\n"
				"mix_material\n{\n\tname m_rb\n\tchild m_red weight 0.5\n\tchild m_blue weight 0.5\n}\n";
			const std::string tmpPath = ( std::filesystem::temp_directory_path() / "rise_waved_gui.RISEscene" ).string();
			{ std::ofstream f( tmpPath ); f << guiScene; }
			IJob* pJob = 0;
			RISE_CreateJob( &pJob );
			Check( pJob->LoadAsciiSceneAuto( tmpPath.c_str() ),
				"Wave D: parse diffuse_transmission_material, mix_material, and scalar_painter { channel RGB }" );
			IJobPriv* pPriv = dynamic_cast<IJobPriv*>( pJob );
			IScalarPainter* spRgb = pPriv->GetScalarPainters()->GetItem( "sp_rgb_bridge" );
			Check( spRgb && spRgb->HasPerChannelVariation(),
				"Wave D: scalar_painter { channel RGB } constructs per-channel PainterRGBScalarPainter" );
			IMaterial* mDt = pPriv->GetMaterials()->GetItem( "m_dt" );
			IMaterial* mRb = pPriv->GetMaterials()->GetItem( "m_rb" );
			Check( mDt && MaterialIntrospection::GetTypeName( *mDt ) == String( "Diffuse Transmission" ),
				"Wave D: MaterialIntrospection::GetTypeName recognizes DiffuseTransmissionMaterial" );
			const auto dtRows = MaterialIntrospection::Inspect(
				String( "m_dt" ), *mDt, pPriv->GetPainters(), pPriv->GetScalarPainters(), pJob );
			bool foundRefl = false, foundTrans = false;
			for( const auto& r : dtRows ) {
				if( r.name == String( "reflectance" ) && r.value == String( "p_red" ) && r.editable ) foundRefl = true;
				if( r.name == String( "transmittance" ) && r.value == String( "p_green" ) && r.editable ) foundTrans = true;
			}
			Check( foundRefl && foundTrans,
				"Wave D: MaterialIntrospection::Inspect exposes editable reflectance & transmittance on DiffuseTransmissionMaterial" );
			IPainter* pBlue = pPriv->GetPainters()->GetItem( "p_blue" );
			Check( MaterialIntrospection::SetSlot( *mDt, String( "transmittance" ), pBlue, nullptr ),
				"Wave D: MaterialIntrospection::SetSlot live-rebinds DiffuseTransmissionMaterial::transmittance" );
			Check( MaterialIntrospection::GetSlot( *mDt, String( "transmittance" ) ).painter == pBlue,
				"Wave D: GetSlot reflects live-rebound transmittance painter" );

			const auto mixRows = MaterialIntrospection::Inspect(
				String( "m_rb" ), *mRb, pPriv->GetPainters(), pPriv->GetScalarPainters(), pJob );
			bool foundChild0 = false;
			for( const auto& r : mixRows ) {
				if( r.name == String( "child[0]" ) && r.value == String( "Lambertian" ) ) foundChild0 = true;
			}
			Check( foundChild0, "Wave D: MaterialIntrospection::Inspect surfaces MixMaterial child rows" );

			// ConnectionLegality refuses wrapping mix_material inside composite / luminaire / coated / fabric
			Check( !ConnectionLegality::CheckConnectionByKeyword( "composite_material", "top", "mix_material", ChunkCategory::Material ).legal,
				"Wave D: ConnectionLegality refuses mix_material inside composite_material" );
			Check( !ConnectionLegality::CheckConnectionByKeyword( "lambertian_luminaire_material", "material", "mix_material", ChunkCategory::Material ).legal,
				"Wave D: ConnectionLegality refuses mix_material inside lambertian_luminaire_material" );
			Check( !ConnectionLegality::CheckConnectionByKeyword( "coated_material", "base", "mix_material", ChunkCategory::Material ).legal,
				"Wave D: ConnectionLegality refuses mix_material inside coated_material" );

			// MaterialPreview renders MixMaterial as weighted red+blue (purple, R & B >> G), NOT fallback grey!
			const MaterialPreview::Result mixPrev = MaterialPreview::RenderMaterialPreview( *pPriv, String( "m_rb" ), 32, 32 );
			Check( mixPrev.status == MaterialPreview::Status::Ok && mixPrev.rgba.size() == 32 * 32 * 4,
				"Wave D: MaterialPreview renders MixMaterial" );
			const size_t midIdx = ( 16 * 32 + 16 ) * 4;
			Check( mixPrev.rgba[midIdx + 0] > 40 && mixPrev.rgba[midIdx + 2] > 40 &&
				mixPrev.rgba[midIdx + 0] > mixPrev.rgba[midIdx + 1] + 25 &&
				mixPrev.rgba[midIdx + 2] > mixPrev.rgba[midIdx + 1] + 25,
				"Wave D: MaterialPreview shades 50/50 Red+Blue MixMaterial as purple, not grey fallback" );

			// PainterPreview sets camera ray so osl_is_camera_ray() renders white (255) instead of black (0)
			const PainterPreview::Result camPrev = PainterPreview::RenderPainterPreview( *pPriv, String( "p_camray" ), 16, 16 );
			Check( camPrev.status == PainterPreview::Status::Ok && camPrev.rgba[ ( 8 * 16 + 8 ) * 4 ] >= 250,
				"Wave D: PainterPreview sets camera ray so osl_is_camera_ray() previews white" );
			const PainterPreview::Result lwPrev = PainterPreview::RenderPainterPreview( *pPriv, String( "p_lw_prev" ), 16, 16 );
			Check( lwPrev.status == PainterPreview::Status::Ok && lwPrev.rgba[ ( 8 * 16 + 8 ) * 4 ] > 0,
				"Wave D: PainterPreview supplies valid non-zero incident ray for osl_layer_weight()" );

			pJob->release();
			std::filesystem::remove( tmpPath );
		}
	}
}

int main()
{
	TestValidationAndRefusals();
	TestResponseCoefficientsAndIndependentDraws();
	TestPassthroughAlphaRegistrationAndHitCoverage();
	TestPerLeafModifierAndExpectedGuides();
	TestOslAOWeightedMixWornWoodenBoxMotif();
	TestEndToEndPTBDPTVCMFurnaceAndAbsorber();
	TestRound1RedProofs();
	TestRound2RedProofs();
	TestWaveBCDContainmentNearClipAndGUI();

	std::printf( "MixMaterialTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
