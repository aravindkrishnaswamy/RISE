//////////////////////////////////////////////////////////////////////
//
//  ContinuationAvailabilityTest.cpp - Phase-B gate-3 immutable
//  continuation-closure and r42 two-availability regressions.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../src/Library/Interfaces/IContinuationClosure.h"
#include "../src/Library/Materials/IsotropicPhongMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/StabilityConfig.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int passed = 0;
	int failed = 0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) ++passed;
		else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	void CheckNear(
		const Scalar actual, const Scalar expected,
		const Scalar tolerance, const char* label )
	{
		const bool ok = std::fabs(actual-expected) <= tolerance;
		Check(ok,label);
		if( !ok ) {
			std::cout << "  got " << actual << ", expected " << expected << std::endl;
		}
	}

	RayIntersectionGeometric MakeIntersection(
		const Vector3& shadingNormal = Vector3(0,0,1),
		const Vector3& rayDirection = Vector3(0,0,-1) )
	{
		const RasterizerState rast = {0,0};
		RayIntersectionGeometric ri(
			Ray(Point3(0,0,1),rayDirection),rast);
		ri.bHit = true;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = shadingNormal;
		ri.vGeomNormal = Vector3(0,0,1);
		ri.onb.CreateFromW(shadingNormal);
		return ri;
	}

	ContinuationPathState NoRouletteState()
	{
		ContinuationPathState state;
		state.pathDepth = 0;
		state.rrMinDepth = 3;
		state.rrThreshold = 0.05;
		state.importance = 1.0;
		return state;
	}

	Scalar BitsToScalar( const std::uint64_t bits )
	{
		volatile std::uint64_t runtimeBits = bits;
		const std::uint64_t copy = runtimeBits;
		Scalar value;
		std::memcpy(&value,&copy,sizeof(value));
		return value;
	}

	void CheckPelNear(
		const RISEPel& actual, const RISEPel& expected,
		const Scalar tolerance, const char* label )
	{
		CheckNear(actual[0],expected[0],tolerance,label);
		CheckNear(actual[1],expected[1],tolerance,label);
		CheckNear(actual[2],expected[2],tolerance,label);
	}

	class TestScalarPainter final :
		public virtual IScalarPainter,
		public virtual Reference
	{
		const ScalarTriple values;
		const bool variationHint;
	protected:
		~TestScalarPainter() override {}
	public:
		TestScalarPainter( const ScalarTriple& values_, const bool variationHint_ ) :
		  values(values_), variationHint(variationHint_) {}
		ScalarTriple GetValuesAt(
			const RayIntersectionGeometric& ) const override { return values; }
		Scalar GetValueAtNM(
			const RayIntersectionGeometric&, Scalar ) const override
		{
			return values.v[0];
		}
		bool HasPerChannelVariation() const override { return variationHint; }
	};

	void TestR42AvailabilityReductionAndCaps()
	{
		const unsigned int all = eContinuationLobeDiffuse |
			eContinuationLobeGlossy;
		StabilityConfig config;
		ContinuationAvailability availability = ResolveContinuationAvailability(
			all,true,0,0,config);
		Check( availability.vertexMask==all && availability.marchMask==all,
			"non-terminal A_vertex and A_march reduce to the identical full mask" );

		availability = ResolveContinuationAvailability(all,false,0,0,config);
		Check( availability.vertexMask==eContinuationLobeNone &&
			availability.marchMask==all,
			"total-depth terminal suppresses A_vertex but preserves A_march" );

		config.maxDiffuseBounce = 0;
		availability = ResolveContinuationAvailability(all,true,0,0,config);
		Check( availability.vertexMask==eContinuationLobeGlossy &&
			availability.marchMask==eContinuationLobeGlossy,
			"diffuse per-type cap removes diffuse from both availability sets" );

		availability = ResolveContinuationAvailability(all,false,0,0,config);
		Check( availability.vertexMask==eContinuationLobeNone &&
			availability.marchMask==eContinuationLobeGlossy,
			"mixed total-depth and diffuse cap leaves only glossy source march" );

		config.maxGlossyBounce = 0;
		availability = ResolveContinuationAvailability(all,false,0,0,config);
		Check( availability.vertexMask==eContinuationLobeNone &&
			availability.marchMask==eContinuationLobeNone,
			"per-type caps can make A_march empty" );
	}

	template<class Closure>
	void TestPhongClosureCommon(
		const Closure& closure, const Scalar diffuseMass,
		const Scalar glossyMass, const char* prefix )
	{
		const unsigned int all = eContinuationLobeDiffuse |
			eContinuationLobeGlossy;
		Check( closure.GetLobeMask()==all, prefix );
		CheckNear(closure.GetSelectionMass(eContinuationLobeDiffuse),
			diffuseMass,1e-15,"Phong diffuse selection mass is captured");
		CheckNear(closure.GetSelectionMass(eContinuationLobeGlossy),
			glossyMass,1e-15,"Phong glossy selection mass is captured");
		const Vector3 direction(0,0,1);
		const Scalar totalMass = diffuseMass+glossyMass;
		const Scalar expectedPdf = diffuseMass/totalMass*INV_PI +
			glossyMass/totalMass*(10.0*INV_PI*0.5);
		CheckNear(closure.PdfMarginal(all,direction),expectedPdf,1e-14,
			"Phong marginal is the normalized full-lobe mixture");
		CheckNear(closure.PdfMarginal(eContinuationLobeDiffuse,direction),
			INV_PI,1e-15,"diffuse-only marginal renormalizes to cosine pdf");
		CheckNear(closure.PdfMarginal(eContinuationLobeGlossy,direction),
			10.0*INV_PI*0.5,1e-14,"glossy-only marginal renormalizes to Phong pdf");
	}

	void TestIsotropicPhongMixedSplitPelAndNM()
	{
		const RayIntersectionGeometric ri = MakeIntersection();
		const ContinuationPathState state = NoRouletteState();
		const IContinuationClosurePel* pel =
			CreateIsotropicPhongContinuationClosurePel(
				ri,RISEPel(0.2,0.4,0.1),RISEPel(0.6,0.1,0.2),9.0,state);
		const IContinuationClosureNM* nm =
			CreateIsotropicPhongContinuationClosureNM(ri,0.25,0.75,9.0,state);
		Check(pel!=0,"valid Pel IsotropicPhong closure is constructed");
		Check(nm!=0,"valid NM IsotropicPhong closure is constructed");
		if( pel ) TestPhongClosureCommon(*pel,0.4,0.6,
			"Pel Phong closure enumerates diffuse and glossy lobes");
		if( nm ) TestPhongClosureCommon(*nm,0.25,0.75,
			"NM Phong closure enumerates diffuse and glossy lobes");

		const Vector3 direction(0,0,1);
		if( pel ) {
			const RISEPel diffuse = pel->EvaluateSubset(
				eContinuationLobeDiffuse,direction);
			const RISEPel glossy = pel->EvaluateSubset(
				eContinuationLobeGlossy,direction);
			const RISEPel all = pel->EvaluateSubset(
				eContinuationLobeDiffuse|eContinuationLobeGlossy,direction);
			CheckPelNear(all,diffuse+glossy,1e-15,
				"Pel mixed-lobe f_A plus f_D split is exactly additive");

			StabilityConfig cap;
			cap.maxDiffuseBounce = 0;
			const ContinuationAvailability availability =
				ResolveContinuationAvailability(pel->GetLobeMask(),true,0,0,cap);
			CheckPelNear(pel->EvaluateSubset(availability.marchMask,direction),
				glossy,1e-15,"Pel allowed f_A contains only glossy response");
			CheckPelNear(pel->EvaluateSubset(
				pel->GetLobeMask()&~availability.marchMask,direction),
				diffuse,1e-15,"Pel capped diffuse response is NEE-only f_D");

			unsigned int diffuseSamples = 0;
			unsigned int glossySamples = 0;
			for( unsigned int i=0; i<1000; ++i ) {
				ContinuationSamplePel sample;
				const bool selected = pel->SampleSubset(
					pel->GetLobeMask(),(Scalar(i)+0.5)/1000.0,
					Point2(0.37,0.61),0.0,false,sample);
				Check(selected,"Pel mixed closure selects one lobe");
				if( selected && sample.horizonPassed ) {
					CheckNear(sample.pdf,pel->PdfMarginal(
						pel->GetLobeMask(),sample.ray.Dir()),1e-15,
						"Pel sampled PDF is the full allowed-lobe marginal");
					CheckNear(sample.reachPdf,sample.pdf,1e-15,
						"Pel no-RR sampled reach density equals marginal PDF");
				}
				if( sample.lobe==eContinuationLobeDiffuse ) ++diffuseSamples;
				if( sample.lobe==eContinuationLobeGlossy ) ++glossySamples;
			}
			Check(diffuseSamples==400 && glossySamples==600,
				"Pel lobe frequencies equal direction-independent 40/60 masses");
		}
		if( nm ) {
			const Scalar diffuse = nm->EvaluateSubset(
				eContinuationLobeDiffuse,direction);
			const Scalar glossy = nm->EvaluateSubset(
				eContinuationLobeGlossy,direction);
			const Scalar all = nm->EvaluateSubset(
				eContinuationLobeDiffuse|eContinuationLobeGlossy,direction);
			CheckNear(all,diffuse+glossy,1e-15,
				"mixed-lobe f_A plus f_D split is exactly additive");

			StabilityConfig cap;
			cap.maxDiffuseBounce = 0;
			const ContinuationAvailability availability =
				ResolveContinuationAvailability(nm->GetLobeMask(),true,0,0,cap);
			CheckNear(nm->EvaluateSubset(availability.marchMask,direction),
				glossy,1e-15,"allowed f_A contains only glossy response");
			CheckNear(nm->EvaluateSubset(
				nm->GetLobeMask()&~availability.marchMask,direction),
				diffuse,1e-15,"capped diffuse response is confined to NEE-only f_D");

			unsigned int diffuseSamples = 0;
			unsigned int glossySamples = 0;
			for( unsigned int i=0; i<1000; ++i ) {
				ContinuationSampleNM sample;
				const bool selected = nm->SampleSubset(
					nm->GetLobeMask(),(Scalar(i)+0.5)/1000.0,
					Point2(0.37,0.61),0.0,false,sample);
				Check(selected,"mixed closure selects one lobe");
				if( selected && sample.horizonPassed ) {
					CheckNear(sample.pdf,nm->PdfMarginal(
						nm->GetLobeMask(),sample.ray.Dir()),1e-15,
						"NM sampled PDF is the full allowed-lobe marginal");
					CheckNear(sample.reachPdf,sample.pdf,1e-15,
						"NM no-RR sampled reach density equals marginal PDF");
				}
				if( sample.lobe==eContinuationLobeDiffuse ) ++diffuseSamples;
				if( sample.lobe==eContinuationLobeGlossy ) ++glossySamples;
			}
			Check(diffuseSamples==250 && glossySamples==750,
				"NM lobe frequencies equal direction-independent 25/75 masses");
		}
		safe_release(pel);
		safe_release(nm);
	}

	template<class Closure>
	Scalar IntegrateGatedPdf(
		const Closure& closure, const unsigned int mask )
	{
		const unsigned int zSteps = 160;
		const unsigned int phiSteps = 320;
		Scalar sum = 0.0;
		for( unsigned int iz=0; iz<zSteps; ++iz ) {
			const Scalar z = -1.0+(Scalar(iz)+0.5)*(2.0/Scalar(zSteps));
			const Scalar radius = std::sqrt(r_max(Scalar(0),1.0-z*z));
			for( unsigned int ip=0; ip<phiSteps; ++ip ) {
				const Scalar phi = TWO_PI*(Scalar(ip)+0.5)/Scalar(phiSteps);
				const Vector3 direction(radius*std::cos(phi),radius*std::sin(phi),z);
				sum += closure.PdfMarginal(mask,direction);
			}
		}
		return sum*(4.0*PI/Scalar(zSteps*phiSteps));
	}

	template<class Closure, class Sample>
	Scalar MeasureHorizonNull(
		const Closure& closure, const unsigned int mask )
	{
		const unsigned int sampleCount = 50000;
		unsigned int nullCount = 0;
		for( unsigned int i=0; i<sampleCount; ++i ) {
			Sample sample;
			const Scalar u = (Scalar(i)+0.5)/Scalar(sampleCount);
			const Scalar v = std::fmod((Scalar(i)+0.5)*0.6180339887498948,1.0);
			const bool selected = closure.SampleSubset(
				mask,0.5,Point2(u,v),0.0,false,sample);
			Check(selected,"horizon fixture always selects its supported lobe");
			if( !sample.horizonPassed ) ++nullCount;
		}
		return Scalar(nullCount)/Scalar(sampleCount);
	}

	template<class Closure, class Sample>
	void GateHorizonNormalization(
		const Closure& closure, const char* label )
	{
		const unsigned int mask = closure.GetLobeMask();
		const Scalar continuous = IntegrateGatedPdf(closure,mask);
		const Scalar nullMass = MeasureHorizonNull<Closure,Sample>(closure,mask);
		CheckNear(continuous+nullMass,1.0,0.012,label);
	}

	void TestHorizonNormalizationPelAndNM()
	{
		const Vector3 tilted = Vector3Ops::Normalize(Vector3(0.8,0,0.6));
		const RayIntersectionGeometric ri = MakeIntersection(tilted);
		const ContinuationPathState state = NoRouletteState();
		const IContinuationClosurePel* lambertPel =
			CreateLambertianContinuationClosurePel(ri,RISEPel(0.5),state);
		const IContinuationClosureNM* lambertNM =
			CreateLambertianContinuationClosureNM(ri,0.5,state);
		const IContinuationClosurePel* orenPel =
			CreateOrenNayarContinuationClosurePel(
				ri,RISEPel(0.5),RISEPel(0.35),state);
		const IContinuationClosureNM* orenNM =
			CreateOrenNayarContinuationClosureNM(ri,0.5,0.35,state);
		const IContinuationClosurePel* phongPel =
			CreateIsotropicPhongContinuationClosurePel(
				ri,RISEPel(0.0),RISEPel(0.7),40.0,state);
		const IContinuationClosureNM* phongNM =
			CreateIsotropicPhongContinuationClosureNM(ri,0,0.7,40.0,state);
		Check(lambertPel && lambertNM && orenPel && orenNM && phongPel && phongNM,
			"all Pel/NM grazing horizon fixtures construct");
		if( lambertPel ) GateHorizonNormalization<
			IContinuationClosurePel,ContinuationSamplePel>(*lambertPel,
			"Pel Lambertian gated density plus sampled null mass normalizes");
		if( lambertNM ) GateHorizonNormalization<
			IContinuationClosureNM,ContinuationSampleNM>(*lambertNM,
			"NM Lambertian gated density plus sampled null mass normalizes");
		if( orenPel ) GateHorizonNormalization<
			IContinuationClosurePel,ContinuationSamplePel>(*orenPel,
			"Pel Oren-Nayar gated density plus sampled null mass normalizes");
		if( orenNM ) GateHorizonNormalization<
			IContinuationClosureNM,ContinuationSampleNM>(*orenNM,
			"NM Oren-Nayar gated density plus sampled null mass normalizes");
		if( phongPel ) GateHorizonNormalization<
			IContinuationClosurePel,ContinuationSamplePel>(*phongPel,
			"Pel Phong gated density plus sampled null mass normalizes");
		if( phongNM ) GateHorizonNormalization<
			IContinuationClosureNM,ContinuationSampleNM>(*phongNM,
			"NM Phong gated density plus sampled null mass normalizes");
		safe_release(lambertPel); safe_release(lambertNM);
		safe_release(orenPel); safe_release(orenNM);
		safe_release(phongPel); safe_release(phongNM);
	}

	void TestHorizonNullAndRouletteMass()
	{
		const Vector3 tilted = Vector3Ops::Normalize(Vector3(0.8,0,0.6));
		const RayIntersectionGeometric tiltedRi = MakeIntersection(tilted);
		const ContinuationPathState noRR = NoRouletteState();
		const IContinuationClosureNM* tiltedClosure =
			CreateLambertianContinuationClosureNM(tiltedRi,0.5,noRR);
		const Vector3 belowGeometric = Vector3Ops::Normalize(Vector3(1,0,-0.1));
		Check( tiltedClosure &&
			tiltedClosure->BasePdf(eContinuationLobeDiffuse,belowGeometric)>0.0,
			"tilted Lambertian base law has support before geometric gate" );
		Check( tiltedClosure &&
			!tiltedClosure->PassesHorizon(eContinuationLobeDiffuse,belowGeometric) &&
			tiltedClosure->PdfMarginal(eContinuationLobeDiffuse,belowGeometric)==0.0 &&
			tiltedClosure->EvaluateSubset(eContinuationLobeDiffuse,belowGeometric)==0.0,
			"geometric horizon produces an explicit zero-density null outcome" );
		safe_release(tiltedClosure);

		ContinuationPathState rr;
		rr.pathDepth = 3;
		rr.rrMinDepth = 3;
		rr.rrThreshold = 2.0;
		rr.importance = 1.0;
		const RayIntersectionGeometric ri = MakeIntersection();
		const IContinuationClosureNM* closure =
			CreateLambertianContinuationClosureNM(ri,0.5,rr);
		const IContinuationClosurePel* pelClosure =
			CreateLambertianContinuationClosurePel(ri,RISEPel(0.5),rr);
		if( closure ) {
			const Scalar pdf = INV_PI;
			CheckNear(closure->PdfReachMarginal(
				eContinuationLobeDiffuse,Vector3(0,0,1)),pdf*0.25,1e-15,
				"reach density includes the exact RR survival mass");
			ContinuationSampleNM survive;
			closure->SampleSubset(eContinuationLobeDiffuse,0.4,
				Point2(0,0),0.2,true,survive);
			Check(survive.horizonPassed && survive.rouletteSurvived,
				"RR sample below survival atom continues");
			CheckNear(survive.survivalProbability,0.25,1e-15,
				"sample records RR survival probability");
			CheckNear(survive.throughput,2.0,1e-15,
				"surviving continuation applies one reciprocal-RR compensation");

			ContinuationSampleNM terminate;
			closure->SampleSubset(eContinuationLobeDiffuse,0.4,
				Point2(0,0),0.3,true,terminate);
			Check(terminate.horizonPassed && !terminate.rouletteSurvived,
				"RR sample above survival atom terminates");

			ContinuationSampleNM terminalMarch;
			closure->SampleSubset(eContinuationLobeDiffuse,0.4,
				Point2(0,0),0.99,false,terminalMarch);
			Check(terminalMarch.rouletteSurvived &&
				terminalMarch.survivalProbability==1.0 &&
				terminalMarch.reachPdf==terminalMarch.pdf,
				"total-depth A_march sample omits roulette survival exactly");
			CheckNear(terminalMarch.throughput,0.5,1e-15,
				"source-only terminal segment keeps unamplified throughput");
		}
		if( pelClosure ) {
			const unsigned int diffuse = eContinuationLobeDiffuse;
			const Vector3 direction(0,0,1);
			CheckNear(pelClosure->PdfReachMarginal(diffuse,direction),
				INV_PI*0.25,1e-15,
				"Pel reach density includes the exact RR survival mass");
			ContinuationSamplePel survive;
			pelClosure->SampleSubset(diffuse,0.4,Point2(0,0),0.2,true,survive);
			Check(survive.horizonPassed && survive.rouletteSurvived,
				"Pel RR sample below survival atom continues");
			CheckNear(survive.pdf,pelClosure->PdfMarginal(
				diffuse,survive.ray.Dir()),1e-15,
				"Pel RR sample records the full marginal PDF");
			CheckNear(survive.reachPdf,survive.pdf*0.25,1e-15,
				"Pel sampled reach PDF contains exactly one survival factor");
			CheckPelNear(survive.throughput,RISEPel(2.0),1e-15,
				"Pel surviving continuation applies reciprocal-RR compensation");
			ContinuationSamplePel terminate;
			pelClosure->SampleSubset(diffuse,0.4,Point2(0,0),0.3,true,terminate);
			Check(terminate.horizonPassed && !terminate.rouletteSurvived,
				"Pel RR sample above survival atom terminates");
			ContinuationSamplePel terminalMarch;
			pelClosure->SampleSubset(diffuse,0.4,Point2(0,0),0.99,false,
				terminalMarch);
			Check(terminalMarch.rouletteSurvived &&
				terminalMarch.survivalProbability==1.0 &&
				terminalMarch.reachPdf==terminalMarch.pdf,
				"Pel total-depth A_march sample omits roulette survival exactly");
			CheckPelNear(terminalMarch.throughput,RISEPel(0.5),1e-15,
				"Pel source-only terminal segment keeps unamplified throughput");
		}
		safe_release(closure);
		safe_release(pelClosure);
	}

	void TestSingleDiffuseMassAndBRDFGrazingPredicates()
	{
		const ContinuationPathState state = NoRouletteState();
		const RayIntersectionGeometric ri = MakeIntersection();
		const IContinuationClosurePel* lambertPel =
			CreateLambertianContinuationClosurePel(ri,RISEPel(0.2,0.7,0.4),state);
		const IContinuationClosureNM* lambertNM =
			CreateLambertianContinuationClosureNM(ri,0.2,state);
		const IContinuationClosurePel* orenPel =
			CreateOrenNayarContinuationClosurePel(
				ri,RISEPel(0.3),RISEPel(0.25),state);
		const IContinuationClosureNM* orenNM =
			CreateOrenNayarContinuationClosureNM(ri,0.3,0.25,state);
		Check(lambertPel && lambertPel->GetSelectionMass(
			eContinuationLobeDiffuse)==1.0,
			"Pel Lambertian single diffuse class has pinned mass one");
		Check(lambertNM && lambertNM->GetSelectionMass(
			eContinuationLobeDiffuse)==1.0,
			"NM Lambertian single diffuse class has pinned mass one");
		Check(orenPel && orenPel->GetSelectionMass(
			eContinuationLobeDiffuse)==1.0,
			"Pel Oren-Nayar single diffuse class has pinned mass one");
		Check(orenNM && orenNM->GetSelectionMass(
			eContinuationLobeDiffuse)==1.0,
			"NM Oren-Nayar single diffuse class has pinned mass one");

		const Vector3 grazing = Vector3Ops::Normalize(
			Vector3(1,0,NEARZERO*0.5));
		Check(lambertPel && lambertPel->BasePdf(
			eContinuationLobeDiffuse,grazing)>0.0 &&
			ColorMath::MaxValue(lambertPel->EvaluateSubset(
				eContinuationLobeDiffuse,grazing))==0.0,
			"Pel Lambertian keeps sampler support but matches BRDF response at grazing");
		Check(lambertNM && lambertNM->BasePdf(
			eContinuationLobeDiffuse,grazing)>0.0 &&
			lambertNM->EvaluateSubset(eContinuationLobeDiffuse,grazing)==0.0,
			"NM Lambertian keeps sampler support but matches BRDF response at grazing");
		const IContinuationClosurePel* phongPel =
			CreateIsotropicPhongContinuationClosurePel(
				ri,RISEPel(0.3),RISEPel(0.7),9.0,state);
		const IContinuationClosureNM* phongNM =
			CreateIsotropicPhongContinuationClosureNM(ri,0.3,0.7,9.0,state);
		Check(phongPel && ColorMath::MaxValue(phongPel->EvaluateSubset(
			eContinuationLobeDiffuse|eContinuationLobeGlossy,grazing))==0.0,
			"Pel Phong response matches the BRDF outgoing grazing predicate");
		Check(phongNM && phongNM->EvaluateSubset(
			eContinuationLobeDiffuse|eContinuationLobeGlossy,grazing)==0.0,
			"NM Phong response matches the BRDF outgoing grazing predicate");

		const RayIntersectionGeometric viewGrazing = MakeIntersection(
			Vector3(0,0,1),Vector3Ops::Normalize(Vector3(1,0,-NEARZERO*0.5)));
		const IContinuationClosureNM* viewLambert =
			CreateLambertianContinuationClosureNM(viewGrazing,0.5,state);
		const IContinuationClosureNM* viewPhong =
			CreateIsotropicPhongContinuationClosureNM(viewGrazing,0.3,0.7,9.0,state);
		Check(viewLambert && viewLambert->EvaluateSubset(
			eContinuationLobeDiffuse,Vector3(0,0,1))==0.0,
			"Lambertian closure matches the BRDF invalid grazing-view branch");
		Check(viewPhong && viewPhong->EvaluateSubset(
			eContinuationLobeDiffuse|eContinuationLobeGlossy,Vector3(0,0,1))==0.0,
			"Phong closure matches the BRDF invalid grazing-view branch");

		safe_release(lambertPel); safe_release(lambertNM);
		safe_release(orenPel); safe_release(orenNM);
		safe_release(phongPel); safe_release(phongNM);
		safe_release(viewLambert); safe_release(viewPhong);
	}

	void TestInvalidParametersFailClosed()
	{
		const RayIntersectionGeometric ri = MakeIntersection();
		const ContinuationPathState state = NoRouletteState();
		const Scalar nan = BitsToScalar(0x7FF8000000000000ULL);
		const Scalar inf = BitsToScalar(0x7FF0000000000000ULL);
		Check(CreateLambertianContinuationClosureNM(ri,-0.1,state)==0,
			"negative NM reflectance is rejected");
		Check(CreateLambertianContinuationClosureNM(ri,nan,state)==0,
			"NaN NM reflectance is rejected");
		Check(CreateIsotropicPhongContinuationClosureNM(ri,0.2,0.3,inf,state)==0,
			"infinite Phong exponent is rejected");
		Check(CreateOrenNayarContinuationClosurePel(
			ri,RISEPel(0.5),RISEPel(0.2,-0.1,0.2),state)==0,
			"negative Pel Oren-Nayar roughness is rejected");

		UniformColorPainter* rd = new UniformColorPainter(RISEPel(0.2));
		UniformColorPainter* rs = new UniformColorPainter(RISEPel(0.3));
		rd->addref(); rs->addref();
		const Scalar invalidValues[] = { -1.0, nan, inf };
		const char* labels[] = {
			"negative hidden Pel exponent channel is rejected",
			"NaN hidden Pel exponent channel is rejected",
			"infinite hidden Pel exponent channel is rejected"
		};
		for( unsigned int i=0; i<3; ++i ) {
			TestScalarPainter* exponent = new TestScalarPainter(
				ScalarTriple(9.0,invalidValues[i],9.0),false);
			exponent->addref();
			IsotropicPhongMaterial* material =
				new IsotropicPhongMaterial(*rd,*rs,*exponent);
			material->addref();
			const IORStack ior(1.0);
			const IContinuationClosurePel* pel =
				material->MakeContinuationClosurePel(ri,ior,state);
			Check(pel==0,labels[i]);
			safe_release(pel);
			safe_release(material);
			safe_release(exponent);
		}
		safe_release(rd); safe_release(rs);
	}
}

int main()
{
	std::cout << "Continuation Closure Phase-B Gate 3" << std::endl;
	TestR42AvailabilityReductionAndCaps();
	TestIsotropicPhongMixedSplitPelAndNM();
	TestHorizonNormalizationPelAndNM();
	TestHorizonNullAndRouletteMass();
	TestSingleDiffuseMassAndBRDFGrazingPredicates();
	TestInvalidParametersFailClosed();
	std::cout << "Passed: " << passed << " Failed: " << failed << std::endl;
	return failed==0 ? 0 : 1;
}
