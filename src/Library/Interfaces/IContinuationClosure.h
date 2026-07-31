//////////////////////////////////////////////////////////////////////
//
//  IContinuationClosure.h - Immutable, pre-NEE surface continuation
//  closures used by the fire/smoke volume-emission MIS partition.
//
//////////////////////////////////////////////////////////////////////

#ifndef ICONTINUATION_CLOSURE_
#define ICONTINUATION_CLOSURE_

#include "IReference.h"
#include "ISPF.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Ray.h"

namespace RISE
{
	class RayIntersectionGeometric;
	struct StabilityConfig;
	class IMaterial;
	class IORStack;

	enum ContinuationLobeMask
	{
		eContinuationLobeNone = 0,
		eContinuationLobeDiffuse = 1u << 0,
		eContinuationLobeGlossy = 1u << 1
	};

	struct ContinuationAvailability
	{
		unsigned int vertexMask;
		unsigned int marchMask;

		ContinuationAvailability() : vertexMask(0), marchMask(0) {}
	};

	struct MediumContinuationAvailability
	{
		bool vertexAllowed;
		bool marchAllowed;

		MediumContinuationAvailability() :
		  vertexAllowed(false), marchAllowed(false) {}
	};

	/// Path-local state captured before volume NEE consumes random numbers.
	struct ContinuationPathState
	{
		unsigned int pathDepth;
		unsigned int rrMinDepth;
		Scalar rrThreshold;
		Scalar importance;

		ContinuationPathState() :
		  pathDepth(0), rrMinDepth(0), rrThreshold(0), importance(1) {}
	};

	struct ContinuationSamplePel
	{
		Ray ray;
		RISEPel response;
		RISEPel throughput;
		Scalar pdf;
		Scalar reachPdf;
		Scalar survivalProbability;
		unsigned int lobe;
		ScatteredRay::ScatRayType type;
		bool singular;
		bool horizonPassed;
		bool rouletteSurvived;

		ContinuationSamplePel() : response(0.0), throughput(0.0), pdf(0),
		  reachPdf(0), survivalProbability(0), lobe(0),
		  type(ScatteredRay::eRayDiffuse), singular(false),
		  horizonPassed(false), rouletteSurvived(false) {}
	};

	struct ContinuationSampleNM
	{
		Ray ray;
		Scalar response;
		Scalar throughput;
		Scalar pdf;
		Scalar reachPdf;
		Scalar survivalProbability;
		unsigned int lobe;
		ScatteredRay::ScatRayType type;
		bool singular;
		bool horizonPassed;
		bool rouletteSurvived;

		ContinuationSampleNM() : response(0), throughput(0), pdf(0),
		  reachPdf(0), survivalProbability(0), lobe(0),
		  type(ScatteredRay::eRayDiffuse), singular(false),
		  horizonPassed(false), rouletteSurvived(false) {}
	};

	class IContinuationClosurePel : public virtual IReference
	{
	protected:
		IContinuationClosurePel() {}
		virtual ~IContinuationClosurePel() {}
	public:
		virtual unsigned int GetLobeMask() const = 0;
		virtual Scalar GetSelectionMass( unsigned int lobe ) const = 0;
		virtual Scalar BasePdf(
			unsigned int lobe, const Vector3& direction ) const = 0;
		virtual bool PassesHorizon(
			unsigned int lobe, const Vector3& direction ) const = 0;
		virtual RISEPel EvaluateSubset(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual Scalar PdfMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual Scalar PdfReachMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual bool SampleSubset(
			unsigned int mask, Scalar xiLobe, const Point2& xiDirection,
			Scalar xiRoulette, bool applyRoulette,
			ContinuationSamplePel& sample ) const = 0;
		/// Terminal source-only directional density: deterministic continuation
		/// gates are applied, but ordinary Russian roulette is not.
		virtual Scalar PdfMarchMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
	};

	class IContinuationClosureNM : public virtual IReference
	{
	protected:
		IContinuationClosureNM() {}
		virtual ~IContinuationClosureNM() {}
	public:
		virtual unsigned int GetLobeMask() const = 0;
		virtual Scalar GetSelectionMass( unsigned int lobe ) const = 0;
		virtual Scalar BasePdf(
			unsigned int lobe, const Vector3& direction ) const = 0;
		virtual bool PassesHorizon(
			unsigned int lobe, const Vector3& direction ) const = 0;
		virtual Scalar EvaluateSubset(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual Scalar PdfMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual Scalar PdfReachMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
		virtual bool SampleSubset(
			unsigned int mask, Scalar xiLobe, const Point2& xiDirection,
			Scalar xiRoulette, bool applyRoulette,
			ContinuationSampleNM& sample ) const = 0;
		/// Terminal source-only directional density: deterministic continuation
		/// gates are applied, but ordinary Russian roulette is not.
		virtual Scalar PdfMarchMarginal(
			unsigned int mask, const Vector3& direction ) const = 0;
	};

	namespace Implementation
	{
		ContinuationAvailability ResolveContinuationAvailability(
			unsigned int closureMask,
			bool downstreamVertexAllowedByTotalDepth,
			unsigned int diffuseBounces,
			unsigned int glossyBounces,
			const StabilityConfig& config );

		MediumContinuationAvailability ResolveMediumContinuationAvailability(
			bool downstreamVertexAllowedByTotalDepth,
			unsigned int volumeBounces,
			unsigned int maxVolumeBounces );

		MediumContinuationAvailability ResolveMediumContinuationAvailability(
			bool downstreamVertexAllowedByTotalDepth,
			unsigned int volumeBounces,
			const StabilityConfig& config );

		bool IsExactSupportedContinuationMaterial( const IMaterial* material );

		const IContinuationClosurePel* CreateLambertianContinuationClosurePel(
			const RayIntersectionGeometric& ri, const RISEPel& reflectance,
			const ContinuationPathState& pathState );
		const IContinuationClosureNM* CreateLambertianContinuationClosureNM(
			const RayIntersectionGeometric& ri, Scalar reflectance,
			const ContinuationPathState& pathState );
		const IContinuationClosurePel* CreateOrenNayarContinuationClosurePel(
			const RayIntersectionGeometric& ri, const RISEPel& reflectance,
			const RISEPel& roughness,
			const ContinuationPathState& pathState );
		const IContinuationClosureNM* CreateOrenNayarContinuationClosureNM(
			const RayIntersectionGeometric& ri, Scalar reflectance,
			Scalar roughness,
			const ContinuationPathState& pathState );
		const IContinuationClosurePel* CreateIsotropicPhongContinuationClosurePel(
			const RayIntersectionGeometric& ri, const RISEPel& rd,
			const RISEPel& rs, Scalar exponent,
			const ContinuationPathState& pathState );
		const IContinuationClosureNM* CreateIsotropicPhongContinuationClosureNM(
			const RayIntersectionGeometric& ri, Scalar rd, Scalar rs,
			Scalar exponent,
			const ContinuationPathState& pathState );
	}
}

#endif
