//////////////////////////////////////////////////////////////////////
//
//  ContinuationClosure.cpp - Audited immutable surface closures for
//  Phase-B volume-emission MIS.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/IContinuationClosure.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/Optics.h"
#include "../Utilities/Reference.h"
#include "../Utilities/StabilityConfig.h"
#include "LambertianMaterial.h"
#include "OrenNayarMaterial.h"
#include "IsotropicPhongMaterial.h"
#include "LambertianLuminaireMaterial.h"
#include "PhongLuminaireMaterial.h"
#include <typeinfo>
#include <type_traits>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	inline bool FiniteNonnegative( const Scalar v )
	{
		return RISE::IsFiniteDouble(v) && v >= 0.0;
	}

	inline bool FiniteNonnegative( const RISEPel& v )
	{
		return FiniteNonnegative(v[0]) && FiniteNonnegative(v[1]) &&
			FiniteNonnegative(v[2]);
	}

	inline Scalar MaxComponent( const RISEPel& v )
	{
		return r_max(v[0],r_max(v[1],v[2]));
	}

	struct ClosureFrame
	{
		RayIntersectionGeometric ri;
		Vector3 normal;
		Vector3 geometricNormal;
		Vector3 reflected;
		Vector3 responseNormal;
		Vector3 responseReflected;
		bool viewBranchValid;

		explicit ClosureFrame(
			const RayIntersectionGeometric& source,
			const bool isotropicPhong ) : ri(source)
		{
			const Scalar rayNormal = Vector3Ops::Dot(ri.ray.Dir(),ri.onb.w());
			viewBranchValid = std::fabs(rayNormal) >= NEARZERO;
			responseNormal = rayNormal > NEARZERO ? -ri.onb.w() : ri.onb.w();
			responseReflected = Optics::CalculateReflectedRay(
				ri.ray.Dir(),responseNormal);
			const Scalar samplingSide = isotropicPhong ?
				Vector3Ops::Dot(ri.ray.Dir(),ri.vGeomNormal) : rayNormal;
			const bool flipSamplingNormal = isotropicPhong ?
				samplingSide > 0.0 : samplingSide > NEARZERO;
			normal = flipSamplingNormal ? -ri.onb.w() : ri.onb.w();
			const Vector3& rawGeom =
				Vector3Ops::SquaredModulus(ri.vGeomNormal) > Scalar(1e-12) ?
				ri.vGeomNormal : normal;
			geometricNormal = Vector3Ops::Dot(rawGeom,ri.ray.Dir()) < 0 ?
				rawGeom : -rawGeom;
			reflected = Optics::CalculateReflectedRay(ri.ray.Dir(),normal);
		}

		bool AboveHorizon( const Vector3& direction ) const
		{
			return Vector3Ops::Dot(direction,geometricNormal) > 0;
		}
	};

	inline Scalar DiffuseBasePdf( const ClosureFrame& frame, const Vector3& direction )
	{
		const Scalar cosine = Vector3Ops::Dot(
			Vector3Ops::Normalize(direction),frame.normal);
		return cosine > 0.0 ? cosine*INV_PI : 0.0;
	}

	inline Scalar PhongBasePdf(
		const ClosureFrame& frame, const Vector3& direction,
		const Scalar exponent )
	{
		const Scalar cosine = Vector3Ops::Dot(
			Vector3Ops::Normalize(direction),
			Vector3Ops::Normalize(frame.reflected));
		return cosine > 0.0 ?
			(exponent+1.0)*INV_PI*0.5*pow(cosine,exponent) : 0.0;
	}

	inline Vector3 SampleDiffuse(
		const ClosureFrame& frame, const Point2& xi )
	{
		OrthonormalBasis3D basis = frame.ri.onb;
		if( Vector3Ops::Dot(frame.ri.ray.Dir(),frame.ri.onb.w()) > NEARZERO ) {
			basis.FlipW();
		}
		return GeometricUtilities::CreateDiffuseVector(basis,xi);
	}

	inline Vector3 SamplePhong(
		const ClosureFrame& frame, const Point2& xi,
		const Scalar exponent )
	{
		return GeometricUtilities::Perturb(
			frame.reflected,
			acos(pow(xi.x,1.0/(exponent+1.0))),
			TWO_PI*xi.y);
	}

	template<class Value>
	inline Value ZeroValue();
	template<> inline Scalar ZeroValue<Scalar>() { return 0.0; }
	template<> inline RISEPel ZeroValue<RISEPel>() { return RISEPel(0.0); }

	template<class Value>
	inline Scalar SelectionMass( const Value& value )
	{
		if constexpr ( std::is_same<Value,Scalar>::value ) return value;
		return MaxComponent(value);
	}

	inline Scalar ThroughputMagnitude( const Scalar value )
	{
		return std::fabs(value);
	}

	inline Scalar ThroughputMagnitude( const RISEPel& value )
	{
		return MaxComponent(value);
	}

	inline Scalar DivideValue( const Scalar value, const Scalar divisor )
	{
		return value/divisor;
	}

	inline RISEPel DivideValue( const RISEPel& value, const Scalar divisor )
	{
		return value*(1.0/divisor);
	}

	template<class Value>
	class ContinuationClosureBase
	{
	protected:
		const ClosureFrame frame;
		const Value diffuse;
		const Value glossy;
		const Scalar exponent;
		const bool orenNayar;
		const bool isotropicPhong;
		const bool unitDiffuseMass;
		const Value roughness;
		const ContinuationPathState pathState;

		ContinuationClosureBase(
			const RayIntersectionGeometric& ri,
			const Value& diffuse_, const Value& glossy_,
			const Scalar exponent_, const bool orenNayar_,
			const bool isotropicPhong_,
			const bool unitDiffuseMass_,
			const Value& roughness_,
			const ContinuationPathState& pathState_ ) :
		  frame(ri,isotropicPhong_), diffuse(diffuse_), glossy(glossy_),
		  exponent(exponent_), orenNayar(orenNayar_),
		  isotropicPhong(isotropicPhong_), unitDiffuseMass(unitDiffuseMass_),
		  roughness(roughness_),
		  pathState(pathState_) {}

		unsigned int LobeMask() const
		{
			unsigned int mask = 0;
			if( SelectionMass(diffuse) > 0.0 ) mask |= eContinuationLobeDiffuse;
			if( SelectionMass(glossy) > 0.0 ) mask |= eContinuationLobeGlossy;
			return mask;
		}

		Scalar LobeMass( const unsigned int lobe ) const
		{
			if( lobe==eContinuationLobeDiffuse ) {
				const Scalar coefficient = SelectionMass(diffuse);
				return unitDiffuseMass && coefficient > 0.0 ? 1.0 : coefficient;
			}
			if( lobe==eContinuationLobeGlossy ) return SelectionMass(glossy);
			return 0.0;
		}

		Scalar LobeBasePdf(
			const unsigned int lobe, const Vector3& direction ) const
		{
			if( lobe==eContinuationLobeDiffuse ) {
				return DiffuseBasePdf(frame,direction);
			}
			if( lobe==eContinuationLobeGlossy ) {
				return PhongBasePdf(frame,direction,exponent);
			}
			return 0.0;
		}

		Value Evaluate( const unsigned int mask, const Vector3& direction ) const
		{
			if( !frame.AboveHorizon(direction) ) return ZeroValue<Value>();
			const Vector3 normalized = Vector3Ops::Normalize(direction);
			Value result = ZeroValue<Value>();
			if( isotropicPhong ) {
				const Scalar cosine = Vector3Ops::Dot(normalized,frame.responseNormal);
				if( !frame.viewBranchValid || cosine < NEARZERO ) return result;
				if( mask&eContinuationLobeDiffuse ) {
					result = result + diffuse*INV_PI;
				}
				if( mask&eContinuationLobeGlossy ) {
					const Scalar alpha = Vector3Ops::Dot(
						normalized,Vector3Ops::Normalize(frame.responseReflected));
					if( alpha > 0.0 ) {
						result = result + glossy *
							((exponent+2.0)*INV_PI*0.5*pow(alpha,exponent));
					}
				}
				return result;
			}
			const Scalar cosine = Vector3Ops::Dot(normalized,frame.responseNormal);
			if( !orenNayar && (!frame.viewBranchValid || cosine < NEARZERO) ) {
				return result;
			}
			if( cosine <= 0.0 ) return result;
			if( mask&eContinuationLobeDiffuse ) {
				if( orenNayar ) {
					Value l1 = ZeroValue<Value>();
					Value l2 = ZeroValue<Value>();
					OrenNayarBRDF::ComputeFactor<Value>(
						l1,l2,direction,frame.ri,frame.responseNormal,roughness);
					result = result + l1*INV_PI*diffuse +
						l2*INV_PI*(diffuse*diffuse);
				} else {
					result = result + diffuse*INV_PI;
				}
			}
			return result;
		}

		Scalar Pdf( const unsigned int mask, const Vector3& direction ) const
		{
			if( !frame.AboveHorizon(direction) ) return 0.0;
			const unsigned int active = mask&LobeMask();
			Scalar total = 0.0;
			if( active&eContinuationLobeDiffuse ) total += LobeMass(eContinuationLobeDiffuse);
			if( active&eContinuationLobeGlossy ) total += LobeMass(eContinuationLobeGlossy);
			if( total <= 0.0 ) return 0.0;
			Scalar pdf = 0.0;
			if( active&eContinuationLobeDiffuse ) {
				pdf += LobeMass(eContinuationLobeDiffuse)/total * DiffuseBasePdf(frame,direction);
			}
			if( active&eContinuationLobeGlossy ) {
				pdf += LobeMass(eContinuationLobeGlossy)/total * PhongBasePdf(frame,direction,exponent);
			}
			return pdf;
		}

		Value ScatterThroughput(
			const unsigned int mask, const Vector3& direction,
			const Scalar pdf ) const
		{
			if( pdf <= 0.0 ) return ZeroValue<Value>();
			const Scalar cosine = Vector3Ops::Dot(
				Vector3Ops::Normalize(direction),frame.normal);
			if( cosine <= 0.0 ) return ZeroValue<Value>();
			return DivideValue(Evaluate(mask,direction)*cosine,pdf);
		}

		Scalar SurvivalProbability(
			const unsigned int mask, const Vector3& direction ) const
		{
			const Scalar pdf = Pdf(mask,direction);
			if( pdf <= 0.0 ) return 0.0;
			const Scalar magnitude = ThroughputMagnitude(
				ScatterThroughput(mask,direction,pdf));
			if( !FiniteNonnegative(magnitude) || magnitude <= NEARZERO ) return 0.0;
			if( pathState.pathDepth < pathState.rrMinDepth ) return 1.0;
			const Scalar denominator = r_max(pathState.importance,pathState.rrThreshold);
			if( !RISE::IsFiniteDouble(denominator) || denominator <= 0.0 ) return 0.0;
			return r_min(Scalar(1.0),pathState.importance*magnitude/denominator);
		}

		Scalar ReachPdf( const unsigned int mask, const Vector3& direction ) const
		{
			return Pdf(mask,direction)*SurvivalProbability(mask,direction);
		}

		bool SelectAndSample(
			const unsigned int mask, Scalar xiLobe,
			const Point2& xiDirection, unsigned int& lobe,
			Vector3& direction ) const
		{
			const unsigned int active = mask&LobeMask();
			Scalar total = 0.0;
			if( active&eContinuationLobeDiffuse ) total += LobeMass(eContinuationLobeDiffuse);
			if( active&eContinuationLobeGlossy ) total += LobeMass(eContinuationLobeGlossy);
			if( total <= 0.0 || !RISE::IsFiniteDouble(total) ) return false;
			xiLobe *= total;
			if( active&eContinuationLobeDiffuse ) {
				const Scalar mass = LobeMass(eContinuationLobeDiffuse);
				if( xiLobe < mass || !(active&eContinuationLobeGlossy) ) {
					lobe = eContinuationLobeDiffuse;
					direction = SampleDiffuse(frame,xiDirection);
					return true;
				}
				xiLobe -= mass;
			}
			if( active&eContinuationLobeGlossy ) {
				lobe = eContinuationLobeGlossy;
				direction = SamplePhong(frame,xiDirection,exponent);
				return true;
			}
			return false;
		}
	};

	class PelClosure final :
		public virtual IContinuationClosurePel,
		public virtual Reference,
		private ContinuationClosureBase<RISEPel>
	{
	public:
		PelClosure(
			const RayIntersectionGeometric& ri,
			const RISEPel& diffuse, const RISEPel& glossy,
			Scalar exponent, bool orenNayar, bool isotropicPhong,
			bool unitDiffuseMass,
			const RISEPel& roughness,
			const ContinuationPathState& pathState ) :
		  ContinuationClosureBase<RISEPel>(
			ri,diffuse,glossy,exponent,orenNayar,isotropicPhong,unitDiffuseMass,
			roughness,pathState) {}
		unsigned int GetLobeMask() const override { return LobeMask(); }
		Scalar GetSelectionMass( unsigned int lobe ) const override { return LobeMass(lobe); }
		Scalar BasePdf( unsigned int lobe, const Vector3& direction ) const override
		{ return LobeBasePdf(lobe,direction); }
		bool PassesHorizon( unsigned int lobe, const Vector3& direction ) const override
		{ return (lobe&LobeMask())!=0 && frame.AboveHorizon(direction); }
		RISEPel EvaluateSubset( unsigned int mask, const Vector3& direction ) const override
		{ return Evaluate(mask,direction); }
		Scalar PdfMarginal( unsigned int mask, const Vector3& direction ) const override
		{ return Pdf(mask,direction); }
		Scalar PdfReachMarginal( unsigned int mask, const Vector3& direction ) const override
		{ return ReachPdf(mask,direction); }
		bool SampleSubset(
			unsigned int mask, Scalar xiLobe, const Point2& xiDirection,
			Scalar xiRoulette, bool applyRoulette,
			ContinuationSamplePel& sample ) const override
		{
			sample = ContinuationSamplePel();
			Vector3 direction;
			if( !SelectAndSample(mask,xiLobe,xiDirection,sample.lobe,direction) ) return false;
			sample.ray = Ray(frame.ri.ptIntersection,direction);
			sample.horizonPassed = frame.AboveHorizon(direction);
			if( !sample.horizonPassed ) return true;
			sample.response = Evaluate(mask,direction);
			sample.pdf = Pdf(mask,direction);
			sample.throughput = ScatterThroughput(mask,direction,sample.pdf);
			if( ThroughputMagnitude(sample.throughput) <= NEARZERO ) {
				sample.survivalProbability = 0.0;
				sample.reachPdf = 0.0;
				return true;
			}
			sample.survivalProbability = applyRoulette ?
				SurvivalProbability(mask,direction) : 1.0;
			sample.reachPdf = sample.pdf*sample.survivalProbability;
			sample.rouletteSurvived = sample.survivalProbability > 0.0 &&
				(!applyRoulette || xiRoulette < sample.survivalProbability);
			if( sample.rouletteSurvived && applyRoulette &&
				sample.survivalProbability < 1.0 ) {
				sample.throughput = DivideValue(
					sample.throughput,sample.survivalProbability);
			}
			sample.type = sample.lobe==eContinuationLobeDiffuse ?
				ScatteredRay::eRayDiffuse : ScatteredRay::eRayReflection;
			return true;
		}
	};

	class NMClosure final :
		public virtual IContinuationClosureNM,
		public virtual Reference,
		private ContinuationClosureBase<Scalar>
	{
	public:
		NMClosure(
			const RayIntersectionGeometric& ri,
			Scalar diffuse, Scalar glossy, Scalar exponent,
			bool orenNayar, bool isotropicPhong, bool unitDiffuseMass,
			Scalar roughness,
			const ContinuationPathState& pathState ) :
		  ContinuationClosureBase<Scalar>(
			ri,diffuse,glossy,exponent,orenNayar,isotropicPhong,unitDiffuseMass,
			roughness,pathState) {}
		unsigned int GetLobeMask() const override { return LobeMask(); }
		Scalar GetSelectionMass( unsigned int lobe ) const override { return LobeMass(lobe); }
		Scalar BasePdf( unsigned int lobe, const Vector3& direction ) const override
		{ return LobeBasePdf(lobe,direction); }
		bool PassesHorizon( unsigned int lobe, const Vector3& direction ) const override
		{ return (lobe&LobeMask())!=0 && frame.AboveHorizon(direction); }
		Scalar EvaluateSubset( unsigned int mask, const Vector3& direction ) const override
		{ return Evaluate(mask,direction); }
		Scalar PdfMarginal( unsigned int mask, const Vector3& direction ) const override
		{ return Pdf(mask,direction); }
		Scalar PdfReachMarginal( unsigned int mask, const Vector3& direction ) const override
		{ return ReachPdf(mask,direction); }
		bool SampleSubset(
			unsigned int mask, Scalar xiLobe, const Point2& xiDirection,
			Scalar xiRoulette, bool applyRoulette,
			ContinuationSampleNM& sample ) const override
		{
			sample = ContinuationSampleNM();
			Vector3 direction;
			if( !SelectAndSample(mask,xiLobe,xiDirection,sample.lobe,direction) ) return false;
			sample.ray = Ray(frame.ri.ptIntersection,direction);
			sample.horizonPassed = frame.AboveHorizon(direction);
			if( !sample.horizonPassed ) return true;
			sample.response = Evaluate(mask,direction);
			sample.pdf = Pdf(mask,direction);
			sample.throughput = ScatterThroughput(mask,direction,sample.pdf);
			if( ThroughputMagnitude(sample.throughput) <= NEARZERO ) {
				sample.survivalProbability = 0.0;
				sample.reachPdf = 0.0;
				return true;
			}
			sample.survivalProbability = applyRoulette ?
				SurvivalProbability(mask,direction) : 1.0;
			sample.reachPdf = sample.pdf*sample.survivalProbability;
			sample.rouletteSurvived = sample.survivalProbability > 0.0 &&
				(!applyRoulette || xiRoulette < sample.survivalProbability);
			if( sample.rouletteSurvived && applyRoulette &&
				sample.survivalProbability < 1.0 ) {
				sample.throughput = DivideValue(
					sample.throughput,sample.survivalProbability);
			}
			sample.type = sample.lobe==eContinuationLobeDiffuse ?
				ScatteredRay::eRayDiffuse : ScatteredRay::eRayReflection;
			return true;
		}
	};
}

ContinuationAvailability RISE::Implementation::ResolveContinuationAvailability(
	const unsigned int closureMask,
	const bool downstreamVertexAllowedByTotalDepth,
	const unsigned int diffuseBounces,
	const unsigned int glossyBounces,
	const StabilityConfig& config )
{
	ContinuationAvailability result;
	result.marchMask = closureMask;
	if( diffuseBounces >= config.maxDiffuseBounce ) {
		result.marchMask &= ~static_cast<unsigned int>(eContinuationLobeDiffuse);
	}
	if( glossyBounces >= config.maxGlossyBounce ) {
		result.marchMask &= ~static_cast<unsigned int>(eContinuationLobeGlossy);
	}
	result.vertexMask = downstreamVertexAllowedByTotalDepth ? result.marchMask : 0;
	return result;
}

bool RISE::Implementation::IsExactSupportedContinuationMaterial(
	const IMaterial* material )
{
	if( !material ) return false;
	const std::type_info& type = typeid(*material);
	if( type==typeid(LambertianMaterial) || type==typeid(OrenNayarMaterial) ||
		type==typeid(IsotropicPhongMaterial) ) return true;
	if( type==typeid(LambertianLuminaireMaterial) ) {
		const LambertianLuminaireMaterial* luminaire =
			dynamic_cast<const LambertianLuminaireMaterial*>(material);
		return IsExactSupportedContinuationMaterial(
			luminaire ? &luminaire->GetBaseMaterial() : 0);
	}
	if( type==typeid(PhongLuminaireMaterial) ) {
		const PhongLuminaireMaterial* luminaire =
			dynamic_cast<const PhongLuminaireMaterial*>(material);
		return IsExactSupportedContinuationMaterial(
			luminaire ? &luminaire->GetBaseMaterial() : 0);
	}
	return false;
}

const IContinuationClosurePel* RISE::Implementation::CreateLambertianContinuationClosurePel(
	const RayIntersectionGeometric& ri, const RISEPel& reflectance,
	const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(reflectance) ) return 0;
	return new PelClosure(
		ri,reflectance,RISEPel(0.0),0.0,false,false,true,
		RISEPel(0.0),pathState);
}

const IContinuationClosureNM* RISE::Implementation::CreateLambertianContinuationClosureNM(
	const RayIntersectionGeometric& ri, const Scalar reflectance,
	const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(reflectance) ) return 0;
	return new NMClosure(ri,reflectance,0.0,0.0,false,false,true,0.0,pathState);
}

const IContinuationClosurePel* RISE::Implementation::CreateOrenNayarContinuationClosurePel(
	const RayIntersectionGeometric& ri, const RISEPel& reflectance,
	const RISEPel& roughness, const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(reflectance) || !FiniteNonnegative(roughness) ) return 0;
	return new PelClosure(
		ri,reflectance,RISEPel(0.0),0.0,true,false,true,roughness,pathState);
}

const IContinuationClosureNM* RISE::Implementation::CreateOrenNayarContinuationClosureNM(
	const RayIntersectionGeometric& ri, const Scalar reflectance,
	const Scalar roughness, const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(reflectance) || !FiniteNonnegative(roughness) ) return 0;
	return new NMClosure(ri,reflectance,0.0,0.0,true,false,true,roughness,pathState);
}

const IContinuationClosurePel* RISE::Implementation::CreateIsotropicPhongContinuationClosurePel(
	const RayIntersectionGeometric& ri, const RISEPel& rd,
	const RISEPel& rs, const Scalar exponent,
	const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(rd) || !FiniteNonnegative(rs) ||
		!FiniteNonnegative(exponent) ) return 0;
	return new PelClosure(
		ri,rd,rs,exponent,false,true,false,RISEPel(0.0),pathState);
}

const IContinuationClosureNM* RISE::Implementation::CreateIsotropicPhongContinuationClosureNM(
	const RayIntersectionGeometric& ri, const Scalar rd,
	const Scalar rs, const Scalar exponent,
	const ContinuationPathState& pathState )
{
	if( !FiniteNonnegative(rd) || !FiniteNonnegative(rs) ||
		!FiniteNonnegative(exponent) ) return 0;
	return new NMClosure(ri,rd,rs,exponent,false,true,false,0.0,pathState);
}
