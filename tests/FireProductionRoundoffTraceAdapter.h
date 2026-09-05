#ifndef FIRE_PRODUCTION_ROUNDOFF_TRACE_ADAPTER_H
#define FIRE_PRODUCTION_ROUNDOFF_TRACE_ADAPTER_H

#include "Utilities/FireProductionForce.h"
#include "FireProductionRoundoffWalker.h"
#include "fire_production_trace/FireProductionForce.h"
#include "fire_production_trace/SourceManifest.h"

#include <array>
#include <string>
#include <vector>

namespace FireProductionRoundoffAdapter
{
	namespace Trace=RISEFireProductionTrace;
	static constexpr std::size_t TraceSourceManifestFieldCount=16u;

	inline std::array<const char*,TraceSourceManifestFieldCount>
		TraceSourceManifestFields()
	{
		return {{Trace::SourceManifest::Generator,
			Trace::SourceManifest::FireProductionAdvectionHeader,
			Trace::SourceManifest::FireProductionAdvectionSource,
			Trace::SourceManifest::FireProductionTransportHeader,
			Trace::SourceManifest::FireProductionTransportSource,
			Trace::SourceManifest::FireProductionForceHeader,
			Trace::SourceManifest::FireProductionForceSource,
			Trace::SourceManifest::FireProductionProjectionHeader,
			Trace::SourceManifest::FireProductionProjectionSource,
			Trace::SourceManifest::FireSimulationRecordsHeader,
			Trace::SourceManifest::FireSimulationRecordsSource,
			Trace::SourceManifest::FireCaseHeader,
			Trace::SourceManifest::FireCaseSource,
			Trace::SourceManifest::TraceAdapter,
			Trace::SourceManifest::TraceCore,
			Trace::SourceManifest::IndependentWalker}};
	}

	inline Trace::FireProductionProjectionShape Shape(
		const RISE::FireProductionProjectionShape& source)
	{
		Trace::FireProductionProjectionShape result;
		result.nx=source.nx;result.ny=source.ny;result.nz=source.nz;
		result.cellWidthM=source.cellWidthM;return result;
	}

	inline Trace::FireProductionProjectionBoundary Boundary(
		const RISE::FireProductionProjectionBoundary source)
	{
		return static_cast<Trace::FireProductionProjectionBoundary>(source);
	}

	template<class Value> inline std::vector<FireProductionRoundoffTrace::TraceFloat>
		Promote(const std::vector<Value>& source)
	{
		return std::vector<FireProductionRoundoffTrace::TraceFloat>(source.begin(),source.end());
	}

	inline Trace::FireProductionFrozenForceRequest Force(
		const RISE::FireProductionFrozenForceRequest& source)
	{
		Trace::FireProductionFrozenForceRequest result;
		result.shape=Shape(source.shape);result.timeStepS=source.timeStepS;
		result.ambientDensityKGPerM3=source.ambientDensityKGPerM3;
		result.vremanCoefficient=source.vremanCoefficient;
		result.cellGasDensityKGPerM3=Promote(source.cellGasDensityKGPerM3);
		result.molecularKinematicViscosityM2PerS=Promote(
			source.molecularKinematicViscosityM2PerS);
		for(unsigned int axis=0u;axis<3u;++axis){
			result.gravityMPerS2[axis]=source.gravityMPerS2[axis];
			result.faceDensityKGPerM3[axis]=Promote(source.faceDensityKGPerM3[axis]);
			result.beginningMomentumKGPerM2S[axis]=Promote(
				source.beginningMomentumKGPerM2S[axis]);
		}
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	inline Trace::FireProductionCellPalindromeRequest Cell(
		const RISE::FireProductionCellPalindromeRequest& source)
	{
		Trace::FireProductionCellPalindromeRequest result;
		result.shape=Shape(source.shape);result.componentCount=source.componentCount;
		result.timeStepS=source.timeStepS;result.conservativeValues=Promote(
			source.conservativeValues);result.ambientValues=Promote(source.ambientValues);
		for(unsigned int axis=0u;axis<3u;++axis)
			result.frozenVelocityMPerS[axis]=Promote(source.frozenVelocityMPerS[axis]);
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	inline Trace::FireProductionDualMomentumRequest Dual(
		const RISE::FireProductionDualMomentumRequest& source)
	{
		Trace::FireProductionDualMomentumRequest result;
		result.shape=Shape(source.shape);result.timeStepS=source.timeStepS;
		result.ambientDensityKGPerM3=source.ambientDensityKGPerM3;
		for(unsigned int axis=0u;axis<3u;++axis){
			result.beginningFaceDensity[axis]=Promote(source.beginningFaceDensity[axis]);
			result.beginningMomentum[axis]=Promote(source.beginningMomentum[axis]);
			result.frozenVelocityMPerS[axis]=Promote(source.frozenVelocityMPerS[axis]);
		}
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	struct ResidentStepTraceResult
	{
		struct ProjectionStreamingEvidence
		{
			double densityLower=std::numeric_limits<double>::infinity(),densityUpper=0.0;
			double maximumResidualEvaluationRadius=0.0;
			double maximumCrossPrecisionResidualUpper=0.0;
			double streamingFaceVelocityL2PerCellUpper=0.0;
			double fp64TerminalFaceL2PerCellUpper=0.0;
			double maximumRoundedVelocity=0.0,maximumRoundedTarget=0.0;
			double maximumBeginningVelocityRoundingUpper=0.0;
			double projectionCorrectionL2PerCell=0.0;
			double firstVelocityResidual=0.0,firstVelocityRadius=0.0;
			double firstVelocityPublished=0.0,firstVelocityCenter=0.0,
				firstVelocityReconstructedRounded=0.0,firstVelocityProjectionCenter=0.0,
				firstVelocityProjectionRadius=0.0;
			float maximumRoundedResidual=0.0f,validationToleranceRounded=0.0f;
			double validationToleranceRadius=0.0;
			std::uint64_t residualCellCount=0u,velocityFaceCount=0u;
			std::size_t firstVelocityFace=std::numeric_limits<std::size_t>::max();
			unsigned int firstVelocityAxis=3u;
			std::uint64_t activeSetAmbiguousCount=0u,activeSetSeparatedMismatchCount=0u;
			bool roundedResidualMatches=false,roundedVelocityMatches=false,
				openActiveSetMatches=false,validationAccepted=false;
		};
		Trace::FireProductionFrozenForceAdvanceResult force;
		Trace::FireProductionCellPalindromeResult cell;
		Trace::FireProductionDualMomentumResult dual;
		std::vector<FireProductionRoundoffTrace::TraceFloat> conservativeValues;
		Trace::FireProductionProjectionResult physicalProjection;
		Trace::FireProductionProjectionResult projection;
		ProjectionStreamingEvidence physicalStreaming,restorationStreaming;
		std::vector<FireProductionRoundoffTrace::Observation> stages;
	};

	inline void IncludeProjectionDensityEnvelope(
		const FireProductionRoundoffTrace::TraceFloat& density,
		ResidentStepTraceResult::ProjectionStreamingEvidence& evidence)
	{
		evidence.densityLower=std::min(evidence.densityLower,std::nextafter(
			density.Center()-density.Radius(),-
			std::numeric_limits<double>::infinity()));
		evidence.densityUpper=std::max(evidence.densityUpper,std::nextafter(
			density.Center()+density.Radius(),
			std::numeric_limits<double>::infinity()));
	}

	inline bool EvaluateProjectionStreamingEvidence(
		const Trace::FireProductionProjectionRequest& request,
		const Trace::FireProductionProjectionResult& projection,const bool restoration,
		ResidentStepTraceResult::ProjectionStreamingEvidence& evidence)
	{
		evidence=ResidentStepTraceResult::ProjectionStreamingEvidence();
		const std::size_t nx=request.shape.nx,ny=request.shape.ny,nz=request.shape.nz;
		if(!nx||!ny||!nz)return false;const std::size_t cells=nx*ny*nz;
		auto faceIndex=[&](const unsigned int axis,const std::size_t x,
			const std::size_t y,const std::size_t z){return axis==0u?(z*ny+y)*(nx+1u)+x:
			(axis==1u?(z*(ny+1u)+y)*nx+x:(z*ny+y)*nx+x);};
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*ny+y)*nx+x;};
		std::array<std::vector<FireProductionRoundoffTrace::TraceFloat>,3> beginning,published;
		std::array<std::vector<FireProductionRoundoffWalker::Binary64Interval>,3> beginning64;
		float maximumVelocity=0.0f;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t count=projection.velocityMPerS[axis].size();
			if(count!=projection.faceDensityKGPerM3[axis].size()||
				count!=request.provisionalMomentumKGPerM2S[axis].size())return false;
			beginning[axis].resize(count);published[axis].resize(count);beginning64[axis].resize(count);
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz;
			for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
				for(std::size_t x=0u;x<ex;++x){const std::size_t face=faceIndex(axis,x,y,z);
					const auto densityValue=projection.faceDensityKGPerM3[axis][face];
					IncludeProjectionDensityEnvelope(densityValue,evidence);
					const FireProductionRoundoffTrace::TraceFloat densityRounded(
						densityValue.Rounded());
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					const unsigned int side=2u*axis+(coordinate==extent?1u:0u);
					const bool wall=(coordinate==0u||coordinate==extent)&&
						request.boundary[side]==Trace::FireProductionProjectionWall;
					beginning[axis][face]=wall?FireProductionRoundoffTrace::TraceFloat(0.0f):
						FireProductionRoundoffTrace::TraceFloat(
							request.provisionalMomentumKGPerM2S[axis][face].Rounded())/densityRounded;
					beginning64[axis][face]=wall?
						FireProductionRoundoffWalker::Binary64Interval::Exact(0.0):
						FireProductionRoundoffWalker::Binary64Interval::Exact(
							request.provisionalMomentumKGPerM2S[axis][face].Rounded())/
						FireProductionRoundoffWalker::Binary64Interval::Exact(densityValue.Rounded());
					evidence.maximumBeginningVelocityRoundingUpper=std::max(
						evidence.maximumBeginningVelocityRoundingUpper,
						beginning[axis][face].Radius());
					published[axis][face]=FireProductionRoundoffTrace::TraceFloat(
						projection.velocityMPerS[axis][face].Rounded());
					maximumVelocity=std::max(maximumVelocity,std::fabs(
						beginning[axis][face].Rounded()));maximumVelocity=std::max(
						maximumVelocity,std::fabs(published[axis][face].Rounded()));
				}
			evidence.velocityFaceCount+=count;
		}
		auto divergence=[&](const std::array<std::vector<FireProductionRoundoffTrace::TraceFloat>,3>&
			velocity,const std::size_t x,const std::size_t y,const std::size_t z){
			return (velocity[0][faceIndex(0u,x+1u,y,z)]-velocity[0][faceIndex(0u,x,y,z)]+
				velocity[1][faceIndex(1u,x,y+1u,z)]-velocity[1][faceIndex(1u,x,y,z)]+
				velocity[2][faceIndex(2u,x,y,z+1u)]-velocity[2][faceIndex(2u,x,y,z)])/
				FireProductionRoundoffTrace::TraceFloat(request.shape.cellWidthM.Rounded());};
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<ny;++y)
			for(std::size_t x=0u;x<nx;++x){const std::size_t cell=cellIndex(x,y,z);
				FireProductionRoundoffTrace::TraceFloat residual=divergence(published,x,y,z);
				if(restoration)residual-=divergence(beginning,x,y,z);
				residual-=FireProductionRoundoffTrace::TraceFloat(
					request.divergenceTargetPerS[cell].Rounded());
				evidence.maximumResidualEvaluationRadius=std::max(
					evidence.maximumResidualEvaluationRadius,residual.Radius());
				evidence.maximumRoundedResidual=std::max(evidence.maximumRoundedResidual,
					std::fabs(residual.Rounded()));++evidence.residualCellCount;
			}
		evidence.roundedResidualMatches=evidence.maximumRoundedResidual==
			projection.maximumPostProjectionResidualPerS.Rounded();
		evidence.maximumRoundedVelocity=maximumVelocity;
		float maximumTarget=0.0f;
		for(const auto& value:request.divergenceTargetPerS)
			maximumTarget=std::max(maximumTarget,std::fabs(value.Rounded()));
		evidence.maximumRoundedTarget=maximumTarget;
		FireProductionRoundoffTrace::TraceFloat tolerance;
		if(restoration){
			tolerance=
				FireProductionRoundoffTrace::TraceFloat(0.005f)*
				FireProductionRoundoffTrace::TraceFloat(maximumTarget);
		}else{const float longest=static_cast<float>(std::max(nx,std::max(ny,nz)));
			const FireProductionRoundoffTrace::TraceFloat length=
				FireProductionRoundoffTrace::TraceFloat(request.shape.cellWidthM.Rounded())*
				FireProductionRoundoffTrace::TraceFloat(longest);
			tolerance=FireProductionRoundoffTrace::TraceFloat(0.005f)*
				FireProductionRoundoffTrace::TraceFloat(maximumVelocity)/length;}
		evidence.validationToleranceRounded=tolerance.Rounded();
		evidence.validationToleranceRadius=tolerance.Radius();
		evidence.validationAccepted=projection.validationPassed&&
			evidence.maximumRoundedResidual<=evidence.validationToleranceRounded;
		// Re-evaluate the terminal velocity arithmetic from exact published pressure
		// and beginning bytes.  This streaming check covers every settled boundary
		// formula; the structural inverse bound remains separately scoped to the
		// frozen all-pressure-open calibration protocol.
		std::array<std::vector<FireProductionRoundoffTrace::TraceFloat>,6> boundaryPressure;
		std::array<std::vector<FireProductionRoundoffWalker::Binary64Interval>,6>
			boundaryPressure64;
		bool activeSetMatches=true;
		for(unsigned int side=0u;side<6u;++side){const unsigned int axis=side/2u;
			const std::size_t count=projection.pressureOpenInflow[side].size();
			boundaryPressure[side].assign(count,FireProductionRoundoffTrace::TraceFloat(0.0f));
			boundaryPressure64[side].assign(count,
				FireProductionRoundoffWalker::Binary64Interval::Exact(0.0));
			const bool positive=(side&1u)!=0u;const std::size_t firstCount=axis==0u?ny:nx;
			const std::size_t secondCount=axis==2u?ny:nz;
			for(std::size_t second=0u;second<secondCount;++second)
				for(std::size_t first=0u;first<firstCount;++first){std::size_t x=0u,y=0u,z=0u;
					if(axis==0u){x=positive?nx:0u;y=first;z=second;}
					if(axis==1u){x=first;y=positive?ny:0u;z=second;}
					if(axis==2u){x=first;y=second;z=positive?nz:0u;}
					const std::size_t index=second*firstCount+first;
					const auto& normalVelocity=beginning[axis][faceIndex(axis,x,y,z)];
					const double orientedCenter=(positive?1.0:-1.0)*normalVelocity.Center();
					const double orientedRadius=normalVelocity.Radius();
					const bool exactInflow=orientedCenter<0.0;
					const bool roundedInflow=projection.pressureOpenInflow[side][index]!=0u;
					if(request.openClassificationMode==
						Trace::FireProductionProjectionDeriveOpenClassification&&
						exactInflow!=roundedInflow){const bool crossesZero=
						orientedCenter-orientedRadius<=0.0&&orientedCenter+orientedRadius>=0.0;
						if(crossesZero)++evidence.activeSetAmbiguousCount;
						else{++evidence.activeSetSeparatedMismatchCount;activeSetMatches=false;}}
					if(request.openHeadMode==Trace::FireProductionProjectionUseSealedOpenHead){
						if(index>=request.sealedPressureOpenDynamicPressurePa[side].size())return false;
						const auto& sealed=request.sealedPressureOpenDynamicPressurePa[side][index];
						boundaryPressure[side][index]=FireProductionRoundoffTrace::TraceFloat(
							sealed.Rounded());
						boundaryPressure64[side][index]=
							FireProductionRoundoffWalker::Binary64Interval::Exact(sealed.Rounded());
						continue;
					}
					if(restoration||!roundedInflow)continue;
					FireProductionRoundoffTrace::TraceFloat speed2=
						beginning[axis][faceIndex(axis,x,y,z)]*
						beginning[axis][faceIndex(axis,x,y,z)];
					const std::size_t cx=axis==0u?(positive?nx-1u:0u):x;
					const std::size_t cy=axis==1u?(positive?ny-1u:0u):y;
					const std::size_t cz=axis==2u?(positive?nz-1u:0u):z;
					for(unsigned int tangent=0u;tangent<3u;++tangent)if(tangent!=axis){
						std::size_t hx=cx,hy=cy,hz=cz;if(tangent==0u)++hx;
						if(tangent==1u)++hy;if(tangent==2u)++hz;
						const auto centered=FireProductionRoundoffTrace::TraceFloat(0.5f)*(
							beginning[tangent][faceIndex(tangent,cx,cy,cz)]+
							beginning[tangent][faceIndex(tangent,hx,hy,hz)]);
						speed2+=centered*centered;}
					boundaryPressure[side][index]=FireProductionRoundoffTrace::TraceFloat(-0.5f)*
						FireProductionRoundoffTrace::TraceFloat(
							request.ambientDensityKGPerM3.Rounded())*speed2;
					auto speed264=beginning64[axis][faceIndex(axis,x,y,z)]*
						beginning64[axis][faceIndex(axis,x,y,z)];
					for(unsigned int tangent=0u;tangent<3u;++tangent)if(tangent!=axis){
						std::size_t hx=cx,hy=cy,hz=cz;if(tangent==0u)++hx;
						if(tangent==1u)++hy;if(tangent==2u)++hz;
						const auto centered64=FireProductionRoundoffWalker::Binary64Interval::Exact(0.5)*(
							beginning64[tangent][faceIndex(tangent,cx,cy,cz)]+
							beginning64[tangent][faceIndex(tangent,hx,hy,hz)]);
						speed264=speed264+centered64*centered64;}
					boundaryPressure64[side][index]=
						FireProductionRoundoffWalker::Binary64Interval::Exact(-0.5)*
						FireProductionRoundoffWalker::Binary64Interval::Exact(
							request.ambientDensityKGPerM3.Rounded())*speed264;
				}
		}
		std::array<std::vector<double>,3> faceRadius;
		std::array<std::vector<double>,3> faceRadius64;
		std::array<std::vector<FireProductionRoundoffTrace::TraceFloat>,3> exactTerminal;
		const auto dt=FireProductionRoundoffTrace::TraceFloat(request.timeStepS.Rounded());
		const auto h=FireProductionRoundoffTrace::TraceFloat(request.shape.cellWidthM.Rounded());
		bool velocityMatches=true;
		const auto dt64=FireProductionRoundoffWalker::Binary64Interval::Exact(
			request.timeStepS.Rounded());
		const auto h64=FireProductionRoundoffWalker::Binary64Interval::Exact(
			request.shape.cellWidthM.Rounded());
		for(unsigned int axis=0u;axis<3u;++axis){faceRadius[axis].resize(published[axis].size());
			faceRadius64[axis].resize(published[axis].size());
			exactTerminal[axis].resize(published[axis].size());
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz;
			for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
				for(std::size_t x=0u;x<ex;++x){const std::size_t face=faceIndex(axis,x,y,z);
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					const unsigned int side=2u*axis+(coordinate==extent?1u:0u);
					FireProductionRoundoffTrace::TraceFloat gradient;
					auto gradient64=FireProductionRoundoffWalker::Binary64Interval::Exact(0.0);
					if(coordinate>0u&&coordinate<extent){std::size_t lx=x,ly=y,lz=z;
						if(axis==0u)--lx;if(axis==1u)--ly;if(axis==2u)--lz;
						gradient=(FireProductionRoundoffTrace::TraceFloat(projection.pressurePa[
							cellIndex(x,y,z)].Rounded())-FireProductionRoundoffTrace::TraceFloat(
							projection.pressurePa[cellIndex(lx,ly,lz)].Rounded()))/h;
						gradient64=(FireProductionRoundoffWalker::Binary64Interval::Exact(
							projection.pressurePa[cellIndex(x,y,z)].Rounded())-
							FireProductionRoundoffWalker::Binary64Interval::Exact(
							projection.pressurePa[cellIndex(lx,ly,lz)].Rounded()))/h64;
					}else{const std::size_t cx=axis==0u?(coordinate?nx-1u:0u):x;
						const std::size_t cy=axis==1u?(coordinate?ny-1u:0u):y;
						const std::size_t cz=axis==2u?(coordinate?nz-1u:0u):z;
						if(request.boundary[side]==Trace::FireProductionProjectionWall){
							velocityMatches=velocityMatches&&published[axis][face].Rounded()==0.0f;
							exactTerminal[axis][face]=FireProductionRoundoffTrace::TraceFloat(0.0f);
							faceRadius[axis][face]=0.0;faceRadius64[axis][face]=0.0;continue;}
						if(request.boundary[side]==Trace::FireProductionProjectionPeriodic){
							std::size_t ox=cx,oy=cy,oz=cz,wx=cx,wy=cy,wz=cz;
							if(axis==0u){wx=nx-1u;ox=0u;}if(axis==1u){wy=ny-1u;oy=0u;}
							if(axis==2u){wz=nz-1u;oz=0u;}
							gradient=(FireProductionRoundoffTrace::TraceFloat(projection.pressurePa[
								cellIndex(ox,oy,oz)].Rounded())-FireProductionRoundoffTrace::TraceFloat(
								projection.pressurePa[cellIndex(wx,wy,wz)].Rounded()))/h;
							gradient64=(FireProductionRoundoffWalker::Binary64Interval::Exact(
								projection.pressurePa[cellIndex(ox,oy,oz)].Rounded())-
								FireProductionRoundoffWalker::Binary64Interval::Exact(
								projection.pressurePa[cellIndex(wx,wy,wz)].Rounded()))/h64;
						}else{
						const std::size_t firstCount=axis==0u?ny:nx;
						const std::size_t first=axis==0u?cy:cx;
						const std::size_t second=axis==2u?cy:cz;
						const auto pb=boundaryPressure[side][second*firstCount+first];
						const auto p=FireProductionRoundoffTrace::TraceFloat(
							projection.pressurePa[cellIndex(cx,cy,cz)].Rounded());
						gradient=coordinate?FireProductionRoundoffTrace::TraceFloat(2.0f)*(pb-p)/h:
							FireProductionRoundoffTrace::TraceFloat(2.0f)*(p-pb)/h;
						const auto pb64=boundaryPressure64[side][second*firstCount+first];
						const auto p64=FireProductionRoundoffWalker::Binary64Interval::Exact(
							projection.pressurePa[cellIndex(cx,cy,cz)].Rounded());
						gradient64=coordinate?FireProductionRoundoffWalker::Binary64Interval::Exact(2.0)*
							(pb64-p64)/h64:FireProductionRoundoffWalker::Binary64Interval::Exact(2.0)*
							(p64-pb64)/h64;}}
					const auto momentum=FireProductionRoundoffTrace::TraceFloat(
						request.provisionalMomentumKGPerM2S[axis][face].Rounded())-dt*gradient;
					const auto velocity=momentum/FireProductionRoundoffTrace::TraceFloat(
						projection.faceDensityKGPerM3[axis][face].Rounded());
					const double velocityResidual=std::fabs(static_cast<double>(
						published[axis][face].Rounded())-velocity.Center());
					if(velocityResidual>velocity.Radius()&&evidence.firstVelocityAxis==3u){
						evidence.firstVelocityAxis=axis;evidence.firstVelocityFace=face;
						evidence.firstVelocityResidual=velocityResidual;
						evidence.firstVelocityRadius=velocity.Radius();
						evidence.firstVelocityPublished=published[axis][face].Rounded();
						evidence.firstVelocityCenter=velocity.Center();
						evidence.firstVelocityReconstructedRounded=velocity.Rounded();
						evidence.firstVelocityProjectionCenter=
							projection.velocityMPerS[axis][face].Center();
						evidence.firstVelocityProjectionRadius=
							projection.velocityMPerS[axis][face].Radius();}
					velocityMatches=velocityMatches&&velocityResidual<=velocity.Radius();
					exactTerminal[axis][face]=velocity;
					faceRadius[axis][face]=velocity.Radius();
					const auto momentum64=FireProductionRoundoffWalker::Binary64Interval::Exact(
						request.provisionalMomentumKGPerM2S[axis][face].Rounded())-dt64*gradient64;
					const auto velocity64=momentum64/
						FireProductionRoundoffWalker::Binary64Interval::Exact(
							projection.faceDensityKGPerM3[axis][face].Center());
					faceRadius64[axis][face]=velocity64.Radius();
				}
		}
		double squareSum=0.0,squareSum64=0.0;
		for(unsigned int axis=0u;axis<3u;++axis)for(const double radius:faceRadius[axis])
			squareSum=std::nextafter(squareSum+radius*radius,
				std::numeric_limits<double>::infinity());
		for(unsigned int axis=0u;axis<3u;++axis)for(const double radius:faceRadius64[axis])
			squareSum64=std::nextafter(squareSum64+radius*radius,
				std::numeric_limits<double>::infinity());
		evidence.streamingFaceVelocityL2PerCellUpper=std::nextafter(std::sqrt(squareSum/
			static_cast<double>(cells)),std::numeric_limits<double>::infinity());
		evidence.fp64TerminalFaceL2PerCellUpper=std::nextafter(std::sqrt(squareSum64/
			static_cast<double>(cells)),std::numeric_limits<double>::infinity());
		double correctionSquareSum=0.0;
		for(unsigned int axis=0u;axis<3u;++axis)
			for(std::size_t face=0u;face<published[axis].size();++face){const double difference=
				static_cast<double>(published[axis][face].Rounded())-
				static_cast<double>(beginning[axis][face].Rounded());
				correctionSquareSum=std::nextafter(correctionSquareSum+difference*difference,
					std::numeric_limits<double>::infinity());}
		evidence.projectionCorrectionL2PerCell=std::nextafter(std::sqrt(
			correctionSquareSum/static_cast<double>(cells)),
			std::numeric_limits<double>::infinity());
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<ny;++y)
			for(std::size_t x=0u;x<nx;++x){const std::size_t cell=cellIndex(x,y,z);
				FireProductionRoundoffTrace::TraceFloat residual=divergence(
					exactTerminal,x,y,z);
				if(restoration)residual-=divergence(beginning,x,y,z);
				residual-=request.divergenceTargetPerS[cell];
				evidence.maximumCrossPrecisionResidualUpper=std::max(
					evidence.maximumCrossPrecisionResidualUpper,
					std::fabs(residual.Center())+residual.Radius());}
		evidence.roundedVelocityMatches=velocityMatches;
		evidence.openActiveSetMatches=activeSetMatches;
		return evidence.densityLower>0.0&&evidence.densityUpper>=evidence.densityLower&&
			evidence.roundedResidualMatches&&evidence.roundedVelocityMatches&&
			evidence.openActiveSetMatches&&evidence.validationAccepted;
	}

	inline void AppendStages(ResidentStepTraceResult& result,
		const FireProductionRoundoffTrace::Counters& counters)
	{
		result.stages.insert(result.stages.end(),counters.sealedStages.begin(),
			counters.sealedStages.end());
	}

	inline void SealProjectionOutputs(Trace::FireProductionProjectionResult& projection)
	{
		for(unsigned int axis=0u;axis<3u;++axis){
			FireProductionRoundoffTrace::ObserveAndReset(projection.faceDensityKGPerM3[axis]);
			FireProductionRoundoffTrace::ObserveMetricRangeAndReset(
				projection.velocityMPerS[axis],0u,projection.velocityMPerS[axis].size(),9u+axis);
			FireProductionRoundoffTrace::ObserveAndReset(projection.momentumKGPerM2S[axis]);
		}
		FireProductionRoundoffTrace::ObserveAndReset(projection.pressurePa);
		FireProductionRoundoffTrace::SealCurrentStage();
	}

	inline bool AdvanceResidentStepTrace(const RISE::FireProductionResidentStepRequest& request,
		const float outwardLambdaPerS,ResidentStepTraceResult& result,std::string* error)
	{
		result=ResidentStepTraceResult();ResidentStepTraceResult computed;
		Trace::FireProductionFrozenForceRequest force=Force(request.force);
		Trace::FireProductionCellPalindromeRequest cell=Cell(request.cellTransport);
		Trace::FireProductionDualMomentumRequest dual=Dual(request.dualTransport);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::AdvanceFireProductionFrozenForceCPU(force,outwardLambdaPerS,
				computed.force,error))return false;
			for(unsigned int axis=0u;axis<3u;++axis)
				FireProductionRoundoffTrace::ObserveMetricRangeAndReset(
					computed.force.momentumKGPerM2S[axis],0u,
					computed.force.momentumKGPerM2S[axis].size(),9u+axis);
			FireProductionRoundoffTrace::SealCurrentStage();
		}
		AppendStages(computed,counters);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::RemapFireProductionCellPalindromeCPU(cell,computed.cell,error))return false;
		}
		AppendStages(computed,counters);
		for(unsigned int axis=0u;axis<3u;++axis)
			dual.beginningMomentum[axis]=computed.force.momentumKGPerM2S[axis];
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::RemapFireProductionDualMomentumCPU(dual,computed.dual,error))return false;
		}
		AppendStages(computed,counters);
		computed.conservativeValues=computed.cell.conservativeValues;
		std::vector<FireProductionRoundoffTrace::TraceFloat> gas;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(computed.conservativeValues.size()!=request.cellSourceIncrement.size())return false;
			for(std::size_t value=0u;value<computed.conservativeValues.size();++value)
				computed.conservativeValues[value]+=
					FireProductionRoundoffTrace::TraceFloat(request.cellSourceIncrement[value]);
			for(unsigned int axis=0u;axis<3u;++axis){
				if(computed.dual.momentum[axis].size()!=request.momentumSourceIncrement[axis].size())
					return false;
				for(std::size_t face=0u;face<computed.dual.momentum[axis].size();++face)
					computed.dual.momentum[axis][face]+=
						FireProductionRoundoffTrace::TraceFloat(
							request.momentumSourceIncrement[axis][face]);
			}
			const std::size_t cells=force.shape.CellCount();
			if(cell.componentCount!=9u||computed.conservativeValues.size()!=9u*cells)return false;
			gas.resize(cells);
			for(std::size_t index=0u;index<cells;++index){
				gas[index]=computed.conservativeValues[cells+index];
				for(std::size_t component=2u;component<=6u;++component)
					gas[index]+=computed.conservativeValues[component*cells+index];
			}
			for(std::size_t component=0u;component<9u;++component)
				FireProductionRoundoffTrace::ObserveMetricRangeAndReset(
					computed.conservativeValues,component*cells,cells,
					static_cast<unsigned int>(component));
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[0]);
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[1]);
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[2]);
			FireProductionRoundoffTrace::ObserveAndReset(gas);
			FireProductionRoundoffTrace::SealCurrentStage();
		}
		AppendStages(computed,counters);
		Trace::FireProductionProjectionRequest projection;
		projection.shape=force.shape;projection.timeStepS=force.timeStepS;
		projection.ambientDensityKGPerM3=force.ambientDensityKGPerM3;
		projection.residentPhysicalOpenVCycleCount=
			request.physicalOpenProjectionVCycleCount;
		projection.gasDensityKGPerM3=gas;
		projection.provisionalMomentumKGPerM2S=computed.dual.momentum;
		projection.divergenceTargetPerS=Promote(request.divergenceTargetPerS);
		for(unsigned int side=0u;side<6u;++side)projection.boundary[side]=force.boundary[side];
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::ProjectFireProductionResidentPhysicalCPU(projection,
				computed.physicalProjection,error))return false;
			if(!EvaluateProjectionStreamingEvidence(projection,computed.physicalProjection,
				false,computed.physicalStreaming))return false;
			SealProjectionOutputs(computed.physicalProjection);
		}
		AppendStages(computed,counters);
		if(!request.enforceManifoldPlateau){
			computed.projection=std::move(computed.physicalProjection);
			result=std::move(computed);return true;
		}
		projection.provisionalMomentumKGPerM2S=computed.physicalProjection.momentumKGPerM2S;
		projection.divergenceTargetPerS=Promote(request.restorationDivergenceTargetPerS);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::ProjectFireProductionRestorationCPU(projection,computed.projection,error))
				return false;
			if(!EvaluateProjectionStreamingEvidence(projection,computed.projection,true,
				computed.restorationStreaming))return false;
			SealProjectionOutputs(computed.projection);
		}
		AppendStages(computed,counters);
		result=std::move(computed);return true;
	}
}

#endif
