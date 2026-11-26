//////////////////////////////////////////////////////////////////////
//
//  DiffuseTransmissionClosedFormTest.cpp - Closed-form physical and
//    contract tests for DiffuseTransmissionMaterial (Blender's
//    Translucent BSDF + thin two-sided diffuse reflection/transmission;
//    design doc §5.6 & Review Round 3 P1-13).
//
//  Tests:
//    1. Energy conservation (Pel): R + T <= 1 enforced per channel;
//       hemispherical + full-sphere Monte Carlo integral matches R + T.
//    2. Round 3 P1-13 spectral energy conservation (NM & HWSS):
//       saturated RGB colours whose RGB sum R + T <= (1, 1, 1) genuinely
//       uplift to raw R(nm) + T(nm) > 1.0 at overlapping wavelengths;
//       assert BOTH that raw uplift > 1.0 occurs AND that DiffuseTransmission
//       strictly enforces R(nm) + T(nm) <= 1.0 across 380..780 nm.
//    3. Helmholtz reciprocity & BDPT/VCM re-aim (Round 3 P1-13):
//       f(wi, wo) == f(wo, wi) for reflection AND transmission, from both
//       front-facing and back-facing ray orientations without flipping N.
//    4. Per-scatter-type split (DL-481 / Round 3 P1-13):
//       valueByScatterType / valueByScatterTypeNM put reflection strictly
//       in eRayDiffuse and transmission strictly in eRayTranslucent and
//       sum identically to value / valueNM.
//    5. SPF/PDF, grazing/tilted-normal MIS & HWSS companion consistency:
//       - ∫ Pdf(wo) dω == 1.0 over the full sphere (both Pel and NM)
//       - Scatter / ScatterNM emit at most 1 ray whose pdf matches Pdf / PdfNM
//         identically (including under a 35-degree tilted normal map!)
//       - Grazing incidence (cos_i ~ 0) emits 0 rays and evaluates to 0
//       - Direct DiffuseTransmissionSPF* calls to EvaluateLobeFNM, 5-arg
//         EvaluateKrayNM, and 6-arg EvaluateKrayNM (proving using-declaration)
//       - Transmitted ray does NOT modify ior_stack (`ior_stack == nullptr`)
//    6. Analytic thin-sheet back-lit transmission closed form:
//       real ObjectManager::IntersectRay hit on a ClippedPlaneGeometry at z = 0
//       with CouldLightPassThrough() == false; point light at (0, 0, -d)
//       gives exact irradiance * T / pi.
//    7. Multi-bounce white-furnace closed form (Monte Carlo Pel + NM):
//       a real unit sphere with R + T = 1.0 (both 0.4/0.6 and pure 0/1)
//       inside a unit white radiance enclosure returns exact 1.0 radiance
//       across multi-bounce interior/exterior scattering with zero energy
//       gain or loss.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Materials/DiffuseTransmissionMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/Painter.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"

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
			std::cerr << "FAIL: " << msg << "\n";
		}
	}

	bool Near( Scalar a, Scalar b, Scalar tol )
	{
		return std::fabs( a - b ) <= tol;
	}

	RayIntersectionGeometric MakeHit( const Vector3& rayDir, const Vector3& normal )
	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3Ops::Normalize( rayDir ) ), nullRasterizerState );
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3Ops::Normalize( normal );
		ri.vGeomNormal = ri.vNormal;
		ri.onb.CreateFromW( ri.vNormal );
		return ri;
	}

	void TestEnergyConservationPelAndSpectral()
	{
		// 1. Over-budget RGB input (R = 0.8, T = 0.6 -> sum 1.4 > 1.0) must normalize to R=4/7, T=3/7
		UniformColorPainter* pR = new UniformColorPainter( RISEPel( 0.8, 0.2, 0.9 ) );
		UniformColorPainter* pT = new UniformColorPainter( RISEPel( 0.6, 0.3, 0.9 ) );
		DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
		IBSDF* bsdf = mat->GetBSDF();
		Check( mat->ScattersFullSphere(), "ScattersFullSphere() is true" );
		Check( !mat->CouldLightPassThrough(), "CouldLightPassThrough() is false (diffuse transmitter, not shadow-transparent)" );
		Check( mat->HasConnectionTypeSplit(), "HasConnectionTypeSplit() is true" );
		Check( mat->ConnectionScatterTypes() == ( (1u << ScatteredRay::eRayDiffuse) | (1u << ScatteredRay::eRayTranslucent) ),
			"ConnectionScatterTypes covers eRayDiffuse | eRayTranslucent" );

		RayIntersectionGeometric ri = MakeHit( Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
		RISEPel hemi;
		Check( bsdf->hemisphericalAlbedo( ri, hemi ), "hemisphericalAlbedo returns true" );
		Check( Near( hemi[0], 1.0, 1e-12 ), "over-budget channel 0 clamps sum to 1.0" );
		Check( Near( hemi[1], 0.5, 1e-12 ), "under-budget channel 1 stays 0.5" );
		Check( Near( hemi[2], 1.0, 1e-12 ), "over-budget channel 2 clamps sum to 1.0" );

		const RISEPel fRefl = bsdf->value( Vector3( 0, 0, 1 ), ri );
		const RISEPel fTrans = bsdf->value( Vector3( 0, 0, -1 ), ri );
		Check( Near( fRefl[0], (0.8 / 1.4) * INV_PI, 1e-12 ), "scaled reflection ratio exact" );
		Check( Near( fTrans[0], (0.6 / 1.4) * INV_PI, 1e-12 ), "scaled transmission ratio exact" );

		mat->release();
		pR->release();
		pT->release();

		// 2. Round 3 P1-13 adversarial spectral uplift case:
		// RGB R = (1.0, 0.0, 0.0), T = (0.0, 1.0, 0.0) -> RGB sum is (1.0, 1.0, 0.0) <= (1,1,1),
		// yet Jakob-Hanika sigmoid spectral uplift overlaps above 1.0 near ~570-590 nm!
		UniformColorPainter* pRed = new UniformColorPainter( RISEPel( 1.0, 0.0, 0.0 ) );
		UniformColorPainter* pGrn = new UniformColorPainter( RISEPel( 0.0, 1.0, 0.0 ) );
		DiffuseTransmissionMaterial* matSpec = new DiffuseTransmissionMaterial( *pRed, *pGrn );
		IBSDF* bsdfSpec = matSpec->GetBSDF();

		bool allConserved = true;
		Scalar maxRawSum = 0.0;
		for( Scalar nm = 380.0; nm <= 780.0; nm += 2.0 ) {
			const Scalar rawSum = pRed->GetColorNM( ri, nm ) + pGrn->GetColorNM( ri, nm );
			if( rawSum > maxRawSum ) maxRawSum = rawSum;
			Scalar hNM = 0.0;
			bsdfSpec->hemisphericalAlbedoNM( ri, nm, hNM );
			const Scalar fR = bsdfSpec->valueNM( Vector3( 0, 0, 1 ), ri, nm );
			const Scalar fT = bsdfSpec->valueNM( Vector3( 0, 0, -1 ), ri, nm );
			if( hNM > 1.0 + 1e-12 || ( fR + fT ) * PI > 1.0 + 1e-12 ) {
				allConserved = false;
			}
		}
		Check( maxRawSum > 1.0 + 1e-3,
			"Round 3 P1-13 precondition: raw uplifted R(nm)+T(nm) genuinely exceeds 1.0 at >= 1 wavelength" );
		Check( allConserved, "Round 3 P1-13: spectral R(nm) + T(nm) <= 1.0 strictly across 380..780 nm" );

		matSpec->release();
		pRed->release();
		pGrn->release();
	}

	void TestReciprocityAndReAimedRays()
	{
		UniformColorPainter* pR = new UniformColorPainter( RISEPel( 0.35, 0.25, 0.15 ) );
		UniformColorPainter* pT = new UniformColorPainter( RISEPel( 0.20, 0.55, 0.45 ) );
		DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
		IBSDF* bsdf = mat->GetBSDF();
		const Vector3 n( 0, 0, 1 );

		const Vector3 dirs[] = {
			Vector3Ops::Normalize( Vector3( 0.3, 0.4,  0.86 ) ),
			Vector3Ops::Normalize( Vector3( -0.5, 0.2, 0.84 ) ),
			Vector3Ops::Normalize( Vector3( 0.2, -0.6, -0.77 ) ),
			Vector3Ops::Normalize( Vector3( -0.4, -0.3, -0.86 ) ),
		};

		for( size_t i = 0; i < 4; ++i ) {
			for( size_t j = 0; j < 4; ++j ) {
				const Vector3 wo = dirs[i];
				const Vector3 wi = dirs[j];
				RayIntersectionGeometric ri1 = MakeHit( -wo, n );
				RayIntersectionGeometric ri2 = MakeHit( -wi, n );

				const RISEPel f12 = bsdf->value( wi, ri1 );
				const RISEPel f21 = bsdf->value( wo, ri2 );
				Check( Near( f12[0], f21[0], 1e-14 ) && Near( f12[1], f21[1], 1e-14 ) && Near( f12[2], f21[2], 1e-14 ),
					"Helmholtz reciprocity holds across front and BDPT/VCM back-face re-aimed rays (Pel)" );

				const Scalar f12nm = bsdf->valueNM( wi, ri1, 550.0 );
				const Scalar f21nm = bsdf->valueNM( wo, ri2, 550.0 );
				Check( Near( f12nm, f21nm, 1e-14 ),
					"Helmholtz reciprocity holds across front and BDPT/VCM back-face re-aimed rays (NM)" );

				RISEPel split[5];
				Check( bsdf->valueByScatterType( wi, ri1, nullptr, split ), "valueByScatterType returns true" );
				const RISEPel sumSplit = split[0] + split[1] + split[2] + split[3] + split[4];
				Check( Near( sumSplit[0], f12[0], 1e-14 ) && Near( sumSplit[1], f12[1], 1e-14 ) && Near( sumSplit[2], f12[2], 1e-14 ),
					"valueByScatterType parts sum to value()" );
				if( Vector3Ops::Dot( wo, n ) * Vector3Ops::Dot( wi, n ) > 0 ) {
					Check( split[ScatteredRay::eRayTranslucent][0] == 0.0, "same-side has 0 translucent part" );
				} else {
					Check( split[ScatteredRay::eRayDiffuse][0] == 0.0, "opposite-side has 0 diffuse reflection part" );
				}

				Scalar splitNM[5];
				Check( bsdf->valueByScatterTypeNM( wi, ri1, 550.0, nullptr, splitNM ), "valueByScatterTypeNM returns true" );
				const Scalar sumNM = splitNM[0] + splitNM[1] + splitNM[2] + splitNM[3] + splitNM[4];
				Check( Near( sumNM, f12nm, 1e-14 ), "valueByScatterTypeNM parts sum to valueNM()" );
			}
		}

		mat->release();
		pR->release();
		pT->release();
	}

	void TestPdfNormalizationGrazingTiltedAndHWSS()
	{
		UniformColorPainter* pR = new UniformColorPainter( RISEPel( 0.3, 0.1, 0.2 ) );
		UniformColorPainter* pT = new UniformColorPainter( RISEPel( 0.1, 0.6, 0.4 ) );
		DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
		IBSDF* bsdf = mat->GetBSDF();
		ISPF* spf = mat->GetSPF();
		DiffuseTransmissionSPF* concreteSpf = dynamic_cast<DiffuseTransmissionSPF*>( spf );
		Check( concreteSpf != nullptr, "GetSPF() returns DiffuseTransmissionSPF" );

		// Direct tests of EvaluateLobeFNM, 5-arg EvaluateKrayNM, and 6-arg EvaluateKrayNM
		// through DiffuseTransmissionSPF* (proves `using ISPF::EvaluateKrayNM;` is intact).
		{
			RayIntersectionGeometric ri = MakeHit( Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
			IORStack ior( 1.0 );
			const Scalar nm = 540.0;
			const Vector3 woRefl = Vector3Ops::Normalize( Vector3( 0.3, 0.2, 0.93 ) );
			const Vector3 woTrans = Vector3Ops::Normalize( Vector3( -0.3, 0.2, -0.93 ) );
			const Scalar fRefl = concreteSpf->EvaluateLobeFNM( ri, woRefl, ScatteredRay::eRayDiffuse, nm, ior );
			const Scalar fTrans = concreteSpf->EvaluateLobeFNM( ri, woTrans, ScatteredRay::eRayTranslucent, nm, ior );
			Check( Near( fRefl, bsdf->valueNM( woRefl, ri, nm ), 1e-14 ), "EvaluateLobeFNM matches valueNM for reflection" );
			Check( Near( fTrans, bsdf->valueNM( woTrans, ri, nm ), 1e-14 ), "EvaluateLobeFNM matches valueNM for transmission" );
			Check( concreteSpf->EvaluateLobeFNM( ri, woRefl, ScatteredRay::eRayTranslucent, nm, ior ) == 0.0,
				"EvaluateLobeFNM returns 0 for wrong hemisphere (translucent on reflection side)" );
			Check( concreteSpf->EvaluateLobeFNM( ri, woTrans, ScatteredRay::eRayDiffuse, nm, ior ) == 0.0,
				"EvaluateLobeFNM returns 0 for wrong hemisphere (diffuse on transmission side)" );

			// 5-arg EvaluateKrayNM on concrete DiffuseTransmissionSPF*
			const Scalar k5Refl = concreteSpf->EvaluateKrayNM( ri, woRefl, ScatteredRay::eRayDiffuse, nm, ior );
			const Scalar k5Trans = concreteSpf->EvaluateKrayNM( ri, woTrans, ScatteredRay::eRayTranslucent, nm, ior );
			const Scalar cosR = Vector3Ops::Dot( woRefl, ri.onb.w() );
			const Scalar cosT = std::fabs( Vector3Ops::Dot( woTrans, ri.onb.w() ) );
			const Scalar pdfR = concreteSpf->PdfNM( ri, woRefl, nm, ior );
			const Scalar pdfT = concreteSpf->PdfNM( ri, woTrans, nm, ior );
			Check( Near( k5Refl, fRefl * cosR / pdfR, 1e-13 ), "5-arg EvaluateKrayNM exact for reflection" );
			Check( Near( k5Trans, fTrans * cosT / pdfT, 1e-13 ), "5-arg EvaluateKrayNM exact for transmission" );

			// 6-arg EvaluateKrayNM on concrete DiffuseTransmissionSPF* (proves `using ISPF::EvaluateKrayNM`)
			const Scalar k6Refl = concreteSpf->EvaluateKrayNM( ri, woRefl, ScatteredRay::eRayDiffuse, nm, ior, pdfR );
			Check( Near( k6Refl, k5Refl, 1e-13 ), "6-arg EvaluateKrayNM through concrete DiffuseTransmissionSPF* exact" );

			// Grazing incidence (ray parallel to surface -> cos_i == 0): emits 0 rays and 0 Pdf/BSDF
			RayIntersectionGeometric riGraze = MakeHit( Vector3( 1, 0, 0 ), Vector3( 0, 0, 1 ) );
			RandomNumberGenerator rngG( 77u );
			IndependentSampler sampG( rngG );
			ScatteredRayContainer scG;
			concreteSpf->Scatter( riGraze, sampG, scG, ior );
			Check( scG.Count() == 0, "grazing incidence Scatter emits 0 rays" );
			concreteSpf->ScatterNM( riGraze, sampG, nm, scG, ior );
			Check( scG.Count() == 0, "grazing incidence ScatterNM emits 0 rays" );
			Check( concreteSpf->Pdf( riGraze, woRefl, ior ) == 0.0, "grazing incidence Pdf == 0" );
			Check( concreteSpf->PdfNM( riGraze, woRefl, nm, ior ) == 0.0, "grazing incidence PdfNM == 0" );
			const RISEPel fG = bsdf->value( woRefl, riGraze );
			Check( fG[0] == 0.0 && fG[1] == 0.0 && fG[2] == 0.0, "grazing incidence BSDF == 0" );

			// Tilted normal map (shading normal tilted 35 deg from geometric normal):
			// Scatter and ScatterNM emit <= 1 ray with pdf > 0 AND sr.pdf == Pdf/PdfNM identically!
			RayIntersectionGeometric riTilted = MakeHit( Vector3( 0.0, 0.2, -0.98 ), Vector3( 0, 0, 1 ) );
			riTilted.vGeomNormal = Vector3( 0, 0, 1 );
			riTilted.vNormal = Vector3Ops::Normalize( Vector3( 0.573576, 0.0, 0.819152 ) ); // ~35 deg tilt
			riTilted.onb.CreateFromW( riTilted.vNormal );
			bool tiltedExact = true;
			bool tiltedExactNM = true;
			int tiltedCount = 0;
			int tiltedCountNM = 0;
			for( int i = 0; i < 2000; ++i ) {
				ScatteredRayContainer scT;
				concreteSpf->Scatter( riTilted, sampG, scT, ior );
				if( scT.Count() > 1 ) tiltedExact = false;
				if( scT.Count() == 1 ) {
					++tiltedCount;
					const ScatteredRay& sr = scT[0];
					const Scalar p = concreteSpf->Pdf( riTilted, sr.ray.Dir(), ior );
					if( !( sr.pdf > 0.0 ) || !Near( p, sr.pdf, 1e-11 ) ) tiltedExact = false;
				}
				ScatteredRayContainer scTNM;
				concreteSpf->ScatterNM( riTilted, sampG, nm, scTNM, ior );
				if( scTNM.Count() > 1 ) tiltedExactNM = false;
				if( scTNM.Count() == 1 ) {
					++tiltedCountNM;
					const ScatteredRay& sr = scTNM[0];
					const Scalar p = concreteSpf->PdfNM( riTilted, sr.ray.Dir(), nm, ior );
					if( !( sr.pdf > 0.0 ) || !Near( p, sr.pdf, 1e-11 ) ) tiltedExactNM = false;
				}
			}
			Check( tiltedExact && tiltedCount > 500,
				"tilted normal map: Scatter emits <=1 ray with pdf > 0 and pdf == Pdf() identically" );
			Check( tiltedExactNM && tiltedCountNM > 500,
				"tilted normal map: ScatterNM emits <=1 ray with pdf > 0 and pdf == PdfNM() identically" );
		}

		// Both front-entering and back-entering hits
		for( int side = 0; side < 2; ++side ) {
			const Vector3 rayDir = ( side == 0 ) ? Vector3( 0.2, 0.1, -0.97 ) : Vector3( 0.2, 0.1, 0.97 );
			RayIntersectionGeometric ri = MakeHit( rayDir, Vector3( 0, 0, 1 ) );
			IORStack ior( 1.0 );

			RandomNumberGenerator rng( 12345u + (unsigned)side );
			const int N = 40000;
			double pdfSumPel = 0.0, pdfSumNM = 0.0;
			double fCosPel[3] = { 0, 0, 0 };
			double fCosNM = 0.0;
			const Scalar nm = 540.0;
			for( int i = 0; i < N; ++i ) {
				const Scalar z = 2.0 * rng.CanonicalRandom() - 1.0;
				const Scalar phi = TWO_PI * rng.CanonicalRandom();
				const Scalar r = std::sqrt( std::max( 0.0, 1.0 - z * z ) );
				const Vector3 wo( r * std::cos( phi ), r * std::sin( phi ), z );
				const Scalar cosO = std::fabs( Vector3Ops::Dot( wo, ri.onb.w() ) );
				pdfSumPel += spf->Pdf( ri, wo, ior ) * FOUR_PI;
				pdfSumNM  += spf->PdfNM( ri, wo, nm, ior ) * FOUR_PI;
				const RISEPel f = bsdf->value( wo, ri );
				for( int c = 0; c < 3; ++c ) fCosPel[c] += f[c] * cosO * FOUR_PI;
				fCosNM += bsdf->valueNM( wo, ri, nm ) * cosO * FOUR_PI;
			}
			Check( Near( pdfSumPel / N, 1.0, 0.02 ), "full-sphere integral of Pdf == 1.0" );
			Check( Near( pdfSumNM / N, 1.0, 0.02 ), "full-sphere integral of PdfNM == 1.0" );
			RISEPel expAlbedo;
			bsdf->hemisphericalAlbedo( ri, expAlbedo );
			for( int c = 0; c < 3; ++c ) {
				Check( Near( fCosPel[c] / N, expAlbedo[c], 0.02 ), "full-sphere integral of f*|cos| == R+T (Pel)" );
			}
			Scalar expAlbedoNM = 0.0;
			bsdf->hemisphericalAlbedoNM( ri, nm, expAlbedoNM );
			Check( Near( fCosNM / N, expAlbedoNM, 0.02 ), "full-sphere integral of fNM*|cos| == R+T (NM)" );

			IndependentSampler sampler( rng );
			double mcPel[3] = { 0, 0, 0 };
			double mcNM = 0.0;
			double mcComp = 0.0;
			const Scalar compNM = 620.0;
			bool iorUntouched = true;
			bool pdfConsistent = true;
			const int M = 30000;
			for( int i = 0; i < M; ++i ) {
				ScatteredRayContainer scPel;
				spf->Scatter( ri, sampler, scPel, ior );
				Scalar selP = 0.0;
				ScatteredRay* chosen = scPel.RandomlySelect( sampler.Get1D(), false, &selP );
				if( chosen && selP > 0.0 ) {
					if( chosen->ior_stack != nullptr ) iorUntouched = false;
					const Scalar aggPdf = spf->Pdf( ri, chosen->ray.Dir(), ior );
					if( !Near( aggPdf, chosen->pdf * selP, 1e-11 ) ) pdfConsistent = false;
					for( int c = 0; c < 3; ++c ) mcPel[c] += chosen->kray[c] / selP;
				}

				ScatteredRayContainer scNM;
				spf->ScatterNM( ri, sampler, nm, scNM, ior );
				Scalar selNM = 0.0;
				ScatteredRay* chosenNM = scNM.RandomlySelect( sampler.Get1D(), true, &selNM );
				if( chosenNM && selNM > 0.0 ) {
					if( chosenNM->ior_stack != nullptr ) iorUntouched = false;
					const Scalar aggPdfNM = spf->PdfNM( ri, chosenNM->ray.Dir(), nm, ior );
					if( !Near( aggPdfNM, chosenNM->pdf * selNM, 1e-11 ) ) pdfConsistent = false;
					mcNM += chosenNM->krayNM / selNM;
					const Scalar kComp = spf->EvaluateKrayNM( ri, chosenNM->ray.Dir(), chosenNM->type, compNM, ior, chosenNM->pdf );
					mcComp += kComp / selNM;
				}
			}
			Check( iorUntouched, "thin diffuse transmission never mutates ior_stack" );
			Check( pdfConsistent, "Pdf / PdfNM exactly equals lobe pdf * RandomlySelect probability" );
			for( int c = 0; c < 3; ++c ) {
				Check( Near( mcPel[c] / M, expAlbedo[c], 0.02 ), "single-bounce Scatter estimator == R+T (Pel)" );
			}
			Check( Near( mcNM / M, expAlbedoNM, 0.02 ), "single-bounce ScatterNM estimator == R+T (NM)" );
			Scalar expCompNM = 0.0;
			bsdf->hemisphericalAlbedoNM( ri, compNM, expCompNM );
			Check( Near( mcComp / M, expCompNM, 0.02 ), "HWSS companion EvaluateKrayNM estimator == R(comp)+T(comp)" );
		}

		mat->release();
		pR->release();
		pT->release();
	}

	void TestAnalyticBacklitThinSheet()
	{
		// Pure Blender Translucent BSDF (R = 0, T = (0.7, 0.5, 0.3))
		// Trace a real ray through ObjectManager::IntersectRay onto a thin ClippedPlaneGeometry at z = 0.
		UniformColorPainter* pBlack = new UniformColorPainter( RISEPel( 0, 0, 0 ) );
		UniformColorPainter* pTrans = new UniformColorPainter( RISEPel( 0.7, 0.5, 0.3 ) );
		DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pBlack, *pTrans );

		const Point3 quad[4] = {
			Point3( -2, -2, 0 ), Point3( 2, -2, 0 ),
			Point3( 2,  2, 0 ),  Point3( -2, 2, 0 )
		};
		ClippedPlaneGeometry* plane = new ClippedPlaneGeometry( quad, true );
		Object* obj = new Object( plane );
		obj->AssignMaterial( *mat );
		ObjectManager* om = new ObjectManager( false, false, 4, 16 );
		om->AddItem( obj, "sheet" );

		RayIntersection ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "real ObjectManager::IntersectRay hits thin ClippedPlaneGeometry sheet" );
		Check( ri.pMaterial == mat, "hit resolves to DiffuseTransmissionMaterial" );

		// Point light of intensity I = 10 at (0, 0, -2) behind sheet at z = 0:
		// Exact transmitted radiance = I * |cos(theta_i)| / d^2 * (T / pi) = 10 * 1 / 4 * T / pi.
		const Vector3 toLight( 0, 0, -1 );
		const Scalar d = 2.0;
		const Scalar intensity = 10.0;
		const RISEPel L = ri.pMaterial->GetBSDF()->value( toLight, ri.geometric ) * ( intensity / ( d * d ) );
		Check( Near( L[0], (10.0 / 4.0) * 0.7 * INV_PI, 1e-13 ), "back-lit sheet R channel matches analytic I*T/(pi*d^2)" );
		Check( Near( L[1], (10.0 / 4.0) * 0.5 * INV_PI, 1e-13 ), "back-lit sheet G channel matches analytic I*T/(pi*d^2)" );
		Check( Near( L[2], (10.0 / 4.0) * 0.3 * INV_PI, 1e-13 ), "back-lit sheet B channel matches analytic I*T/(pi*d^2)" );

		const RISEPel Lfront = ri.pMaterial->GetBSDF()->value( Vector3( 0, 0, 1 ), ri.geometric );
		Check( Lfront[0] == 0.0 && Lfront[1] == 0.0 && Lfront[2] == 0.0,
			"pure Translucent BSDF reflects strictly 0 on front hemisphere" );

		om->release();
		obj->release();
		plane->release();
		mat->release();
		pBlack->release();
		pTrans->release();
	}

	void TestMultiBounceWhiteFurnace()
	{
		// Test 7: Real multi-bounce white-furnace on a unit sphere with R + T = 1.0
		// inside a unit-radiance environment (L_env = 1.0). Every ray that transmits
		// into the sphere bounces inside until it transmits back out to the environment!
		// Both mixed (R = 0.4, T = 0.6) and pure translucent (R = 0.0, T = 1.0) must
		// return exact 1.0 in BOTH Pel and NM!
		const struct { Scalar r; Scalar t; const char* label; } cases[2] = {
			{ 0.4, 0.6, "mixed R=0.4 T=0.6" },
			{ 0.0, 1.0, "pure translucent R=0 T=1" },
		};

		SphereGeometry* sphere = new SphereGeometry( 1.0 );

		for( int ci = 0; ci < 2; ++ci ) {
			UniformColorPainter* pR = new UniformColorPainter( RISEPel( cases[ci].r, cases[ci].r, cases[ci].r ) );
			UniformColorPainter* pT = new UniformColorPainter( RISEPel( cases[ci].t, cases[ci].t, cases[ci].t ) );
			DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
			ISPF* spf = mat->GetSPF();

			RandomNumberGenerator rng( 9001u + (unsigned)ci );
			IndependentSampler sampler( rng );
			const int NPaths = 15000;
			const int kMaxDepth = 64;
			const Scalar nm = 550.0;
			double pelSum = 0.0;
			double nmSum = 0.0;
			int multiBounceHits = 0;

			for( int i = 0; i < NPaths; ++i ) {
				// Start outside sphere at (0, 0, 3) aimed at a random disk point inside radius 0.9
				const Scalar rad = 0.9 * std::sqrt( rng.CanonicalRandom() );
				const Scalar phi = TWO_PI * rng.CanonicalRandom();
				const Point3 origin( rad * std::cos( phi ), rad * std::sin( phi ), 3.0 );
				const Vector3 dir( 0, 0, -1 );

				// Pel multi-bounce trace
				{
					Ray curRay( origin, dir );
					Scalar thru = 1.0;
					IORStack ior( 1.0 );
					for( int bounce = 0; bounce < kMaxDepth; ++bounce ) {
						RayIntersectionGeometric ri( curRay, nullRasterizerState );
						sphere->IntersectRay( ri, true, true, false );
						if( !ri.bHit ) {
							pelSum += thru; // escaped to unit white furnace!
							break;
						}
						if( bounce >= 1 ) ++multiBounceHits;
						ri.onb.CreateFromW( ri.vNormal );
						ScatteredRayContainer sc;
						spf->Scatter( ri, sampler, sc, ior );
						Scalar selP = 0.0;
						ScatteredRay* sr = sc.RandomlySelect( sampler.Get1D(), false, &selP );
						if( !sr || selP <= 0.0 ) break;
						thru *= sr->kray[0] / selP;
						const Point3 nextOrg = Point3Ops::mkPoint3( ri.ptIntersection, sr->ray.Dir() * 1e-5 );
						curRay = Ray( nextOrg, sr->ray.Dir() );
					}
				}

				// NM multi-bounce trace
				{
					Ray curRay( origin, dir );
					Scalar thru = 1.0;
					IORStack ior( 1.0 );
					for( int bounce = 0; bounce < kMaxDepth; ++bounce ) {
						RayIntersectionGeometric ri( curRay, nullRasterizerState );
						sphere->IntersectRay( ri, true, true, false );
						if( !ri.bHit ) {
							nmSum += thru;
							break;
						}
						ri.onb.CreateFromW( ri.vNormal );
						ScatteredRayContainer sc;
						spf->ScatterNM( ri, sampler, nm, sc, ior );
						Scalar selP = 0.0;
						ScatteredRay* sr = sc.RandomlySelect( sampler.Get1D(), true, &selP );
						if( !sr || selP <= 0.0 ) break;
						thru *= sr->krayNM / selP;
						const Point3 nextOrg = Point3Ops::mkPoint3( ri.ptIntersection, sr->ray.Dir() * 1e-5 );
						curRay = Ray( nextOrg, sr->ray.Dir() );
					}
				}
			}

			Check( multiBounceHits > NPaths / 2,
				"Test 7: rays genuinely enter and multi-bounce inside the translucent sphere" );
			Check( Near( pelSum / NPaths, 1.0, 0.015 ),
				"Test 7: multi-bounce white-furnace returns exact 1.0 (Pel)" );
			Check( Near( nmSum / NPaths, 1.0, 0.015 ),
				"Test 7: multi-bounce white-furnace returns exact 1.0 (NM)" );

			mat->release();
			pR->release();
			pT->release();
		}

		sphere->release();
	}
	// Simple narrowband spectral painter for Test 8 (disjoint R/T HWSS bundles).
	class BandPainter : public Painter
	{
	protected:
		RISEPel rgb;
		Scalar  nmMin;
		Scalar  nmMax;
		Scalar  val;
		virtual ~BandPainter() {}
	public:
		BandPainter( const RISEPel& rgb_, Scalar nmMin_, Scalar nmMax_, Scalar val_ ) :
			rgb( rgb_ ), nmMin( nmMin_ ), nmMax( nmMax_ ), val( val_ ) {}

		RISEPel GetColor( const RayIntersectionGeometric& ) const override { return rgb; }
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const override
		{
			return ( nm >= nmMin && nm <= nmMax ) ? val : Scalar(0);
		}
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) override {}
		void RegenerateData() override {}
	};

	void TestHWSSBundleDisjointAndComplementarySpectra()
	{
		// Test 8a: Saturated complementary RGB R = (1, 0, 0), T = (0, 0, 1).
		// Rotate hero across all 4 HWSS bundle lanes (450, 520, 590, 660 nm).
		// Broadband lobe probs eliminate sigmoid-tail 1000x companion fireflies and bias!
		{
			UniformColorPainter* pR = new UniformColorPainter( RISEPel( 1.0, 0.0, 0.0 ) );
			UniformColorPainter* pT = new UniformColorPainter( RISEPel( 0.0, 0.0, 1.0 ) );
			DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
			IBSDF* bsdf = mat->GetBSDF();
			ISPF* spf = mat->GetSPF();

			const Scalar lambdas[4] = { 450.0, 520.0, 590.0, 660.0 };
			double laneSum[4] = { 0, 0, 0, 0 };
			double maxCompWeight = 0.0;
			RayIntersectionGeometric ri = MakeHit( Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
			IORStack ior( 1.0 );
			RandomNumberGenerator rng( 20261123u );
			IndependentSampler sampler( rng );
			const int NBundles = 20000;

			for( int i = 0; i < NBundles; ++i ) {
				const int heroIdx = i & 3;
				const Scalar heroNM = lambdas[heroIdx];
				ScatteredRayContainer sc;
				spf->ScatterNM( ri, sampler, heroNM, sc, ior );
				Scalar selP = 0.0;
				ScatteredRay* sr = sc.RandomlySelect( sampler.Get1D(), true, &selP );
				if( sr && selP > 0.0 ) {
					for( int lane = 0; lane < 4; ++lane ) {
						const Scalar w = ( lane == heroIdx )
							? ( sr->krayNM / selP )
							: ( spf->EvaluateKrayNM( ri, sr->ray.Dir(), sr->type, lambdas[lane], ior, sr->pdf ) / selP );
						laneSum[lane] += w;
						if( w > maxCompWeight ) maxCompWeight = w;
					}
				}
			}

			for( int lane = 0; lane < 4; ++lane ) {
				Scalar expAlb = 0.0;
				bsdf->hemisphericalAlbedoNM( ri, lambdas[lane], expAlb );
				Check( Near( laneSum[lane] / NBundles, expAlb, 0.025 ),
					"Test 8a: HWSS bundle-averaged complementary RGB lane matches R(nm)+T(nm)" );
			}
			Check( maxCompWeight < 5.0,
				"Test 8a: HWSS companion weights stay bounded (< 5.0, no sigmoid-tail 1000x fireflies)" );

			mat->release();
			pR->release();
			pT->release();
		}

		// Test 8b: Spectrally disjoint narrowband painters with a dead hero-zero gap at 550 nm:
		//   R = 0.8 on [580, 700] nm, 0 elsewhere;  T = 0.7 on [400, 520] nm, 0 elsewhere.
		//   Bundle = {450, 550, 640, 680} nm, rotating hero across ALL 4 lanes including 550 nm!
		{
			BandPainter* pR = new BandPainter( RISEPel( 0.8, 0.0, 0.0 ), 580.0, 700.0, 0.8 );
			BandPainter* pT = new BandPainter( RISEPel( 0.0, 0.0, 0.7 ), 400.0, 520.0, 0.7 );
			DiffuseTransmissionMaterial* mat = new DiffuseTransmissionMaterial( *pR, *pT );
			ISPF* spf = mat->GetSPF();

			const Scalar lambdas[4] = { 450.0, 550.0, 640.0, 680.0 };
			const Scalar expected[4] = { 0.7, 0.0, 0.8, 0.8 };
			double laneSum[4] = { 0, 0, 0, 0 };
			int heroZeroEmitted = 0;
			RayIntersectionGeometric ri = MakeHit( Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
			IORStack ior( 1.0 );
			RandomNumberGenerator rng( 777123u );
			IndependentSampler sampler( rng );
			const int NBundles = 20000;

			for( int i = 0; i < NBundles; ++i ) {
				const int heroIdx = i & 3;
				const Scalar heroNM = lambdas[heroIdx];
				ScatteredRayContainer sc;
				spf->ScatterNM( ri, sampler, heroNM, sc, ior );
				if( heroIdx == 1 && sc.Count() == 1 && sc[0].pdf > 0.0 ) {
					++heroZeroEmitted;
				}
				Scalar selP = 0.0;
				ScatteredRay* sr = sc.RandomlySelect( sampler.Get1D(), true, &selP );
				if( sr && selP > 0.0 ) {
					for( int lane = 0; lane < 4; ++lane ) {
						const Scalar w = ( lane == heroIdx )
							? ( sr->krayNM / selP )
							: ( spf->EvaluateKrayNM( ri, sr->ray.Dir(), sr->type, lambdas[lane], ior, sr->pdf ) / selP );
						laneSum[lane] += w;
					}
				}
			}

			Check( heroZeroEmitted == NBundles / 4,
				"Test 8b: hero-zero wavelength (550 nm) still emits valid ray so live companions survive" );
			for( int lane = 0; lane < 4; ++lane ) {
				Check( Near( laneSum[lane] / NBundles, expected[lane], 0.025 ),
					"Test 8b: HWSS bundle-averaged disjoint narrowband lane converges unbiasedly (no 50% energy loss)" );
			}

			mat->release();
			pR->release();
			pT->release();
		}
	}
}

int main()
{
	TestEnergyConservationPelAndSpectral();
	TestReciprocityAndReAimedRays();
	TestPdfNormalizationGrazingTiltedAndHWSS();
	TestAnalyticBacklitThinSheet();
	TestMultiBounceWhiteFurnace();
	TestHWSSBundleDisjointAndComplementarySpectra();

	std::cout << "DiffuseTransmissionClosedFormTest: " << g_pass << " passed, " << g_fail << " failed.\n";
	return g_fail == 0 ? 0 : 1;
}
