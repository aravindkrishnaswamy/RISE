//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvectionMac.mm - Metal conservative production PPM
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "FireProductionAdvection.h"
#include "FireProductionForce.h"
#include "FireSimulationRecords.h"
#include "FireProductionTransport.h"
#include "ThreadPool.h"
#include "../Interfaces/IOptions.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace RISE
{
	// Internal behavioral seam: production and the calibration RED consume the
	// same recursion-safety decision without exposing it in the public API.
	bool FireProductionDualLayoutPackRequiresSerialOwner(
		const bool auditSerial,const bool legacyLowPriority )
	{
		return auditSerial||legacyLowPriority;
	}

	// Internal test-evidence preflight shared with FireProductionProjectionMac.mm.
	bool ValidateFireProductionRestorationCycleProbe(
		unsigned int& cycleCount,bool& enabled,std::string* error );

	namespace
	{
		std::uint64_t AvalancheAcceptedDigest(std::uint64_t word)
		{
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			return word^(word>>32u);
		}

		std::uint64_t OrderedAcceptedFloatFieldDigest(
			const std::vector<float>& values,const std::uint64_t fieldTag )
		{
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){
				std::uint32_t bits=0u;std::memcpy(&bits,&values[index],sizeof(bits));
				ordered^=static_cast<std::uint64_t>(bits)+UINT64_C(0x9e3779b97f4a7c15)+
					(static_cast<std::uint64_t>(index)<<32u);
				ordered=((ordered<<27u)|(ordered>>37u))*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			return AvalancheAcceptedDigest(ordered);
		}

		std::uint64_t OrderedAcceptedByteFieldDigest(
			const std::vector<unsigned char>& values,const std::uint64_t fieldTag )
		{
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){
				ordered^=static_cast<std::uint64_t>(values[index])+
					UINT64_C(0x9e3779b97f4a7c15)+(static_cast<std::uint64_t>(index)<<32u);
				ordered=((ordered<<27u)|(ordered>>37u))*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			return AvalancheAcceptedDigest(ordered);
		}

		std::uint64_t ParallelAcceptedManifoldPayloadDigest(
			const FireProductionResidentStepResult& value,const bool serial )
		{
			std::array<const std::vector<float>*,27> floatFields;
			std::size_t field=0u;floatFields[field++]=&value.conservativeValues;
			for(unsigned int axis=0u;axis<3u;++axis){
				floatFields[field++]=&value.transportedDual.auxiliaryFaceDensity[axis];
				floatFields[field++]=&value.transportedDual.momentum[axis];
				floatFields[field++]=&value.physicalProjection.faceDensityKGPerM3[axis];
				floatFields[field++]=&value.physicalProjection.velocityMPerS[axis];
				floatFields[field++]=&value.physicalProjection.momentumKGPerM2S[axis];
				floatFields[field++]=&value.projection.faceDensityKGPerM3[axis];
				floatFields[field++]=&value.projection.velocityMPerS[axis];
				floatFields[field++]=&value.projection.momentumKGPerM2S[axis];
			}
			floatFields[field++]=&value.physicalProjection.pressurePa;
			floatFields[field++]=&value.projection.pressurePa;
			if(field!=floatFields.size())return 0u;
			std::array<const std::vector<unsigned char>*,12> byteFields;
			field=0u;
			for(const auto& side:value.physicalProjection.pressureOpenInflow)
				byteFields[field++]=&side;
			for(const auto& side:value.projection.pressureOpenInflow)byteFields[field++]=&side;
			if(field!=byteFields.size())return 0u;
			std::array<std::uint64_t,39> ordered;
			auto hashField=[&](const std::size_t index){
				if(index<floatFields.size())ordered[index]=OrderedAcceptedFloatFieldDigest(
					*floatFields[index],static_cast<std::uint64_t>(index+1u));
				else {const std::size_t byteIndex=index-floatFields.size();
					ordered[index]=OrderedAcceptedByteFieldDigest(*byteFields[byteIndex],
						static_cast<std::uint64_t>(index+1u));}
			};
			if(serial)for(std::size_t index=0u;index<ordered.size();++index)hashField(index);
			else Implementation::GlobalThreadPool().ParallelFor(ordered.size(),hashField);
			std::uint64_t digest=UINT64_C(0xd6e8feb86659fd93);
			auto appendWord=[&](const std::uint64_t word){digest^=word+
				UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);};
			for(std::size_t index=0u;index<floatFields.size();++index){
				appendWord(index+1u);appendWord(floatFields[index]->size());appendWord(ordered[index]);}
			for(std::size_t index=0u;index<byteFields.size();++index){
				const std::size_t combined=index+floatFields.size();appendWord(combined+1u);
				appendWord(byteFields[index]->size());appendWord(ordered[combined]);}
			return AvalancheAcceptedDigest(digest^ordered.size());
		}

		std::uint64_t ParallelAcceptedStatePayloadDigestFast(
			const FireProductionProjectionShape& shape,
			const std::vector<float>& conservativeValues,
			const std::array<std::vector<float>,3>& momentum,
			const std::array<std::vector<float>,3>& velocity,const bool serial )
		{
			std::array<const std::vector<float>*,7> fields={{&conservativeValues,
				&momentum[0],&velocity[0],&momentum[1],&velocity[1],&momentum[2],&velocity[2]}};
			std::array<std::uint64_t,7> ordered;
			auto hashField=[&](const std::size_t index){ordered[index]=
				OrderedAcceptedFloatFieldDigest(*fields[index],static_cast<std::uint64_t>(index+1u));};
			if(serial)for(std::size_t index=0u;index<ordered.size();++index)hashField(index);
			else Implementation::GlobalThreadPool().ParallelFor(ordered.size(),hashField);
			std::uint64_t digest=UINT64_C(0x65f07b31c42a98de);
			auto appendWord=[&](const std::uint64_t word){digest^=word+
				UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);};
			appendWord(UINT64_C(0x7265736964656e74));appendWord(shape.nx);appendWord(shape.ny);
			appendWord(shape.nz);std::uint32_t widthBits=0u;
			std::memcpy(&widthBits,&shape.cellWidthM,sizeof(widthBits));appendWord(widthBits);
			for(std::size_t index=0u;index<fields.size();++index){appendWord(index+1u);
				appendWord(fields[index]->size());appendWord(ordered[index]);}
			return AvalancheAcceptedDigest(digest^fields.size()^UINT64_C(0xd64b291e3fa5708c));
		}

		struct MetalManifoldParameters
		{
			std::uint32_t cellCount;
			std::uint32_t affineRowCount;
			std::uint32_t affineStateDimension;
			std::uint32_t reserved;
			float temperatureMinK;
			float temperatureMaxK;
			float pressurePa;
			float feasibilityFactor;
		};

		constexpr std::size_t MetalThermochemistrySpeciesStride=32u;
		constexpr std::size_t MetalThermochemistrySpeciesValues=
			7u*MetalThermochemistrySpeciesStride;
		constexpr std::size_t MetalManifoldCertificateValues=
			MetalThermochemistrySpeciesValues+64u;
		constexpr std::size_t MetalManifoldQuantileBins=65536u;
		constexpr std::size_t MetalManifoldQuantileControlWords=6u;
		constexpr std::size_t MetalManifoldQuantileScratchWords=
			3u*MetalManifoldQuantileBins+MetalManifoldQuantileControlWords;
		constexpr std::size_t MetalManifoldQuantileControlOffset=
			3u*MetalManifoldQuantileBins*sizeof(std::uint32_t);

		bool PackMetalMethaneThermochemistry(
			std::array<float,MetalManifoldCertificateValues>& packed,
			MetalManifoldParameters& parameters,
			std::array<double,7>& lowerEnthalpy,
			std::array<double,7>& upperEnthalpy,
			std::string* error )
		{
			packed.fill(0.0f);
			const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
			if(!fuel.IsValid()||fuel.SpeciesOrder().size()!=7u||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
					lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
					upperEnthalpy.data(),upperEnthalpy.size(),error))return false;
			for(std::size_t speciesIndex=0u;speciesIndex<7u;++speciesIndex){
				const FireThermochemistrySpecies* species=fuel.FindSpecies(
					fuel.SpeciesOrder()[speciesIndex].c_str());
				if(!species||species->segments.empty()||species->segments.size()>3u)return false;
				float* destination=packed.data()+speciesIndex*MetalThermochemistrySpeciesStride;
				destination[0]=static_cast<float>(species->molecularWeightKGPerKMol);
				destination[1]=static_cast<float>(species->segments.size());
				for(std::size_t segmentIndex=0u;segmentIndex<species->segments.size();++segmentIndex){
					const FireThermochemistrySegment& segment=species->segments[segmentIndex];
					float* output=destination+2u+10u*segmentIndex;
					output[0]=static_cast<float>(segment.temperatureMinK);
					output[1]=static_cast<float>(segment.temperatureMaxK);
					for(std::size_t coefficient=0u;coefficient<7u;++coefficient)
						output[2u+coefficient]=static_cast<float>(segment.coefficients[coefficient]);
					output[9]=static_cast<float>(segment.sensibleEnthalpyOffsetJPerKG);
				}
			}
			const FireCertifiedNullspace& affine=fuel.ConservativeReconstruction();
			if(affine.stateDimension>8u||affine.constraintRows*affine.stateDimension>64u||
				affine.constraintMatrix.size()!=affine.constraintRows*affine.stateDimension)
				return false;
			parameters.affineRowCount=static_cast<std::uint32_t>(affine.constraintRows);
			parameters.affineStateDimension=static_cast<std::uint32_t>(affine.stateDimension);
			for(std::size_t coefficient=0u;coefficient<affine.constraintMatrix.size();++coefficient)
				packed[MetalThermochemistrySpeciesValues+coefficient]=
					static_cast<float>(affine.constraintMatrix[coefficient]);
			parameters.temperatureMinK=static_cast<float>(fuel.TemperatureMinK());
			parameters.temperatureMaxK=static_cast<float>(fuel.TemperatureMaxK());
			parameters.pressurePa=static_cast<float>(fuel.ThermodynamicPressurePa());
			parameters.feasibilityFactor=static_cast<float>(
				fuel.AcceptedStateFeasibilityEnvelope().kappaEpsilon32*
				static_cast<double>(std::numeric_limits<float>::epsilon()));
			return std::isfinite(parameters.temperatureMinK)&&
				std::isfinite(parameters.temperatureMaxK)&&
				std::isfinite(parameters.pressurePa)&&
				std::isfinite(parameters.feasibilityFactor)&&
				parameters.temperatureMinK>0.0f&&
				parameters.temperatureMaxK>parameters.temperatureMinK&&
				parameters.pressurePa>0.0f&&parameters.feasibilityFactor>0.0f;
		}

		bool ValidateDualMomentumStaticOwnerMetadata(
			const FireProductionDualMomentumRequest& request,
			std::string* error )
		{
			const FireProductionProjectionShape& shape=request.shape;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3))return false;
			auto faceIndex=[&](const unsigned int axis,const std::size_t x,
				const std::size_t y,const std::size_t z){
				if(axis==0u)return (z*shape.ny+y)*(shape.nx+1u)+x;
				if(axis==1u)return (z*(shape.ny+1u)+y)*shape.nx+x;
				return (z*shape.ny+y)*shape.nx+x;
			};
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				const bool periodic=lower==FireProductionProjectionPeriodic;
				const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					(periodic!=(upper==FireProductionProjectionPeriodic))||(!periodic&&extent<5u)||
					request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces)return false;
				for(const float density:request.beginningFaceDensity[axis])
					if(!(density>0.0f)||!std::isfinite(density))return false;
				for(const float momentum:request.beginningMomentum[axis])
					if(!std::isfinite(momentum))return false;
				for(const float velocity:request.frozenVelocityMPerS[axis])
					if(!std::isfinite(velocity))return false;
				if(periodic){
					const std::size_t firstExtent=axis==0u?shape.ny:shape.nx;
					const std::size_t secondExtent=axis==2u?shape.ny:shape.nz;
					for(std::size_t second=0u;second<secondExtent;++second)
						for(std::size_t first=0u;first<firstExtent;++first){
							const std::size_t lx=axis==0u?0u:first;
							const std::size_t ly=axis==0u?first:(axis==1u?0u:second);
							const std::size_t lz=axis==2u?0u:second;
							const std::size_t hx=axis==0u?extent:lx;
							const std::size_t hy=axis==1u?extent:ly;
							const std::size_t hz=axis==2u?extent:lz;
							const std::size_t low=faceIndex(axis,lx,ly,lz),high=faceIndex(axis,hx,hy,hz);
							if(request.beginningFaceDensity[axis][low]!=request.beginningFaceDensity[axis][high]||
								request.beginningMomentum[axis][low]!=request.beginningMomentum[axis][high]||
								request.frozenVelocityMPerS[axis][low]!=request.frozenVelocityMPerS[axis][high])
								return false;
						}
				}
			}
			if(error)error->clear();return true;
		}

		struct MetalParameters
		{
			std::uint32_t lineLength;
			std::uint32_t lineCount;
			std::uint32_t componentCount;
			std::uint32_t lowerBoundary;
			std::uint32_t upperBoundary;
			std::uint32_t ambientPerLine;
			float cellWidthM;
			float timeStepS;
		};

		struct MetalGridParameters
		{
			std::uint32_t nx,ny,nz,axis,componentCount;
		};

		struct MetalPeriodicDualParameters
		{
			std::uint32_t nx,ny,nz,component,sweepAxis;
		};

		struct MetalDualLineParameters
		{
			std::uint32_t nx,ny,nz,component,sweepAxis;
			std::uint32_t lineLength,lineCount,componentBeginning,componentPeriodic;
			std::uint32_t lowerComponentWall,upperComponentWall;
		};

		thread_local std::uint64_t MetalCommandCommitCount=0u;
		thread_local std::uint64_t MetalHostBufferReadCount=0u;

		id<MTLCommandBuffer> TrackedMetalCommandBuffer( id<MTLCommandQueue> queue )
		{
			return [queue commandBuffer];
		}

		void CommitTrackedMetalCommand( id<MTLCommandBuffer> command )
		{
			++MetalCommandCommitCount;[command commit];
		}

		void* ReadTrackedMetalBuffer( id<MTLBuffer> buffer )
		{
			++MetalHostBufferReadCount;return [buffer contents];
		}

		std::string MetalError( const char* prefix, NSError* error )
		{
			std::string result(prefix);
			if( error ) result += " ["+std::string([[error domain] UTF8String])+" "+
				std::to_string(static_cast<long>([error code]))+"] "+
				std::string([[error localizedDescription] UTF8String]);
			return result;
		}

		const char* RemapSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Params { uint n; uint lines; uint comps; uint lowerBoundary; uint upperBoundary; uint ambientPerLine; float dx; float dt; };
struct GridParams { uint nx; uint ny; uint nz; uint axis; uint comps; };
inline uint value_index(constant Params& p,uint c,uint l,uint i){return (c*p.lines+l)*p.n+i;}
inline uint flux_index(constant Params& p,uint c,uint l,uint f){return (c*p.lines+l)*(p.n+1u)+f;}
inline uint grid_cell(constant GridParams& g,uint x,uint y,uint z){return (z*g.ny+y)*g.nx+x;}
inline uint grid_face(constant GridParams& g,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*g.ny+y)*(g.nx+1u)+x;
 if(axis==1u)return (z*(g.ny+1u)+y)*g.nx+x;
 return (z*g.ny+y)*g.nx+x;
}
inline void axis_coordinates(constant GridParams& g,uint line,uint coordinate,
 thread uint& x,thread uint& y,thread uint& z){
 if(g.axis==0u){x=coordinate;y=line%g.ny;z=line/g.ny;return;}
 if(g.axis==1u){x=line%g.nx;y=coordinate;z=line/g.nx;return;}
 x=line%g.nx;y=line/g.nx;z=coordinate;
}
kernel void gather_grid_values(device const float* grid [[buffer(0)]],
 device float* lines [[buffer(1)]],constant GridParams& g [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=g.nx*g.ny*g.nz,total=g.comps*cells;if(gid>=total)return;
 uint c=gid/cells,cell=gid-c*cells,x=cell%g.nx,y=(cell/g.nx)%g.ny,z=cell/(g.nx*g.ny);
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint line=g.axis==0u?z*g.ny+y:(g.axis==1u?z*g.nx+x:y*g.nx+x);
 uint coordinate=g.axis==0u?x:(g.axis==1u?y:z);
 lines[(c*(cells/length)+line)*length+coordinate]=grid[gid];
}
kernel void scatter_grid_values(device const float* lines [[buffer(0)]],
 device float* grid [[buffer(1)]],constant GridParams& g [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=g.nx*g.ny*g.nz,total=g.comps*cells;if(gid>=total)return;
 uint c=gid/cells,cell=gid-c*cells,x=cell%g.nx,y=(cell/g.nx)%g.ny,z=cell/(g.nx*g.ny);
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint line=g.axis==0u?z*g.ny+y:(g.axis==1u?z*g.nx+x:y*g.nx+x);
 uint coordinate=g.axis==0u?x:(g.axis==1u?y:z);
 grid[gid]=lines[(c*(cells/length)+line)*length+coordinate];
}
kernel void gather_grid_velocity(device const float* ux [[buffer(0)]],
 device const float* uy [[buffer(1)]],device const float* uz [[buffer(2)]],
 device float* lines [[buffer(3)]],constant GridParams& g [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint lineCount=(g.nx*g.ny*g.nz)/length,total=lineCount*(length+1u);if(gid>=total)return;
 uint line=gid/(length+1u),coordinate=gid-line*(length+1u),x,y,z;
 axis_coordinates(g,line,coordinate,x,y,z);
 device const float* velocity=g.axis==0u?ux:(g.axis==1u?uy:uz);
 lines[gid]=velocity[grid_face(g,g.axis,x,y,z)];
}
inline float ambient_value(device const float* ambient,constant Params& p,uint c,uint l){
 return ambient[p.ambientPerLine!=0u?c*p.lines+l:c];
}
inline float continuous_inflow(float nearest,float ambient,float velocity,bool positive,
 constant Params& p){
 if(!(p.dt>0.0f))return nearest;
 float scale=max(0x1p-126f,max(abs(velocity),abs(p.dx/p.dt)));
 float width=0x1p-24f*scale,signedVelocity=positive?velocity:-velocity;
 if(signedVelocity<=-width)return nearest;
 if(signedVelocity>=width)return ambient;
 float weight=(signedVelocity+width)/(2.0f*width);
 return nearest+weight*(ambient-nearest);
}
inline float sample_value(device const float* q,device const float* u,
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,int i){
 if(p.lowerBoundary==0u&&p.upperBoundary==0u){int n=int(p.n);int w=i%n;if(w<0)w+=n;return q[value_index(p,c,l,uint(w))];}
 if(i<0){float nearest=q[value_index(p,c,l,0u)];if(p.lowerBoundary!=1u)return nearest;
  return continuous_inflow(nearest,ambient_value(lowerAmbient,p,c,l),
   u[l*(p.n+1u)],true,p);}
 if(i>=int(p.n)){float nearest=q[value_index(p,c,l,p.n-1u)];
  if(p.upperBoundary!=1u)return nearest;
  return continuous_inflow(nearest,ambient_value(upperAmbient,p,c,l),
   u[l*(p.n+1u)+p.n],false,p);}
 return q[value_index(p,c,l,uint(i))];
}
inline void unlimited_edges(device const float* q,device const float* u,
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,uint cell,thread float& left,thread float& right){
 int i=int(cell);float im2=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i-2);
 float im1=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i-1);
 float center=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i);
 float ip1=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i+1);
 float ip2=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i+2);
 if(im2==center&&im1==center&&ip1==center&&ip2==center){left=center;right=center;return;}
 left=(7.0f*(im1+center)-(im2+ip1))/12.0f;
 right=(7.0f*(center+ip1)-(im1+ip2))/12.0f;
}
inline void deviation_range(float dl,float dr,thread float& mn,thread float& mx){
 mn=min(dl,dr);mx=max(dl,dr);float a=3.0f*(dl+dr);float b=-4.0f*dl-2.0f*dr;
 if(a!=0.0f){float s=-b/(2.0f*a);if(s>0.0f&&s<1.0f){float v=(a*s+b)*s+dl;mn=min(mn,v);mx=max(mx,v);}}
}
inline float continuous_shared_alpha(float alpha,float headroom,float d,float center,float envelope){
 float scale=max(0x1p-126f,max(abs(center),max(abs(envelope),abs(d))));
 float width=0x1p-10f*scale;
 float numerator=headroom+max(0.0f,-d);
 float denominator=max(d,width);
 return min(alpha,min(1.0f,numerator/denominator));
}
kernel void reconstruct(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* lowerAmbient [[buffer(2)]],device const float* upperAmbient [[buffer(3)]],
 device float* left [[buffer(4)]],device float* right [[buffer(5)]],
 device float* alphaOut [[buffer(6)]],constant Params& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.n*p.lines)return;uint l=gid/p.n;uint cell=gid-l*p.n;float alpha=1.0f;
 for(uint c=0;c<p.comps;++c){uint index=value_index(p,c,l,cell);float ql,qr;
  unlimited_edges(q,u,lowerAmbient,upperAmbient,p,c,l,cell,ql,qr);left[index]=ql;right[index]=qr;
  float center=q[index],mnDev,mxDev;deviation_range(ql-center,qr-center,mnDev,mxDev);
  float qm=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,int(cell)-1);
  float qp=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,int(cell)+1);
  float mn=min(center,min(qm,qp)),mx=max(center,max(qm,qp));
  alpha=continuous_shared_alpha(alpha,mx-center,mxDev,center,mx);
  alpha=continuous_shared_alpha(alpha,center-mn,-mnDev,center,mn);
 }
 alpha=clamp(alpha,0.0f,1.0f);alphaOut[gid]=alpha;
 for(uint c=0;c<p.comps;++c){uint index=value_index(p,c,l,cell);float center=q[index];
  left[index]=center+alpha*(left[index]-center);right[index]=center+alpha*(right[index]-center);}
}
kernel void scan_lines(device const float* q [[buffer(0)]],device float* prefix [[buffer(1)]],
 constant Params& p [[buffer(2)]],threadgroup float* scratch [[threadgroup(0)]],
 uint3 threadPosition [[thread_position_in_threadgroup]],
 uint3 groupPosition [[threadgroup_position_in_grid]],uint3 threads [[threads_per_threadgroup]]){
 uint tid=threadPosition.x,group=groupPosition.x,width=threads.x;
 uint c=group/p.lines;uint l=group-c*p.lines;
 scratch[tid]=tid<p.n?q[value_index(p,c,l,tid)]:0.0f;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint offset=1u;offset<width;offset<<=1u){uint index=(tid+1u)*offset*2u-1u;
  if(index<width)scratch[index]+=scratch[index-offset];threadgroup_barrier(mem_flags::mem_threadgroup);}
 uint base=(c*p.lines+l)*(p.n+1u);if(tid==0u){prefix[base+p.n]=scratch[width-1u];scratch[width-1u]=0.0f;}
 threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint offset=width>>1u;offset>0u;offset>>=1u){uint index=(tid+1u)*offset*2u-1u;
  if(index<width){float a=scratch[index-offset];scratch[index-offset]=scratch[index];scratch[index]+=a;}
  threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(tid<p.n)prefix[base+tid]=scratch[tid];
}
inline float cell_interval(float center,float left,float right,float beginning,float end){
 if(left==center&&right==center)return (end-beginning)*center;
 float q6=6.0f*center-3.0f*(left+right),delta=end-beginning;
 return delta*(left+0.5f*(right-left+q6)*(beginning+end)-
  (q6/3.0f)*(beginning*beginning+beginning*end+end*end));
}
inline float cell_trailing(float center,float left,float right,float length){
 if(left==center&&right==center)return length*center;
 float q6=6.0f*center-3.0f*(left+right);
 return length*(right-0.5f*(right-left-q6)*length-(q6/3.0f)*length*length);
}
inline uint wrapped_cell(int cell,uint count){int n=int(count),wrapped=cell%n;
 if(wrapped<0)wrapped+=n;return uint(wrapped);}
inline float periodic_local_forward(device const float* q,device const float* left,
 device const float* right,constant Params& p,uint c,uint l,int beginningCell,
 float beginningFraction,float length){
 float remaining=length,result=0.0f,fraction=beginningFraction;int cell=beginningCell;
 while(remaining>0.0f){float span=min(remaining,1.0f-fraction);
  uint index=value_index(p,c,l,wrapped_cell(cell,p.n));
  result+=cell_interval(q[index],left[index],right[index],fraction,fraction+span);
  remaining-=span;fraction=0.0f;++cell;}
 return result;
}
inline float periodic_swept(device const float* q,device const float* left,
 device const float* right,device const float* prefix,constant Params& p,uint c,uint l,
 uint face,float courant){
 float count=float(p.n),magnitude=abs(courant),localLength=fmod(magnitude,count);
 float cycles=floor((magnitude-localLength)/count);uint base=(c*p.lines+l)*(p.n+1u);
 float result=cycles*prefix[base+p.n];
 if(courant>=0.0f){float whole=floor(localLength),fractional=localLength-whole;
  int wholeBeginning=int(face)-int(whole);
  if(fractional>0.0f){uint index=value_index(p,c,l,wrapped_cell(wholeBeginning-1,p.n));
   result+=cell_trailing(q[index],left[index],right[index],fractional);}
  result+=periodic_local_forward(q,left,right,p,c,l,wholeBeginning,0.0f,whole);return result;}
 result+=periodic_local_forward(q,left,right,p,c,l,int(face),0.0f,localLength);return -result;
}
inline float open_local_forward(device const float* q,device const float* left,
 device const float* right,constant Params& p,uint c,uint l,uint beginningCell,
 float beginningFraction,float length){
 float remaining=length,result=0.0f,fraction=beginningFraction;uint cell=beginningCell;
 while(remaining>0.0f&&cell<p.n){float span=min(remaining,1.0f-fraction);
  uint index=value_index(p,c,l,cell);
  result+=cell_interval(q[index],left[index],right[index],fraction,fraction+span);
  remaining-=span;fraction=0.0f;++cell;}
 return result;
}
inline float open_swept(device const float* q,device const float* left,device const float* right,
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,uint face,float courant,float velocity){
 float magnitude=abs(courant),leftNearest=q[value_index(p,c,l,0u)];
 float rightNearest=q[value_index(p,c,l,p.n-1u)];
 float leftExtension=p.lowerBoundary==1u?continuous_inflow(leftNearest,
  ambient_value(lowerAmbient,p,c,l),velocity,true,p):leftNearest;
 float rightExtension=p.upperBoundary==1u?continuous_inflow(rightNearest,
  ambient_value(upperAmbient,p,c,l),velocity,false,p):rightNearest;
 if(courant>=0.0f){float interiorLength=min(magnitude,float(face));
  float whole=floor(interiorLength),fractional=interiorLength-whole;
  uint wholeBeginning=face-uint(whole);float result=(magnitude-interiorLength)*leftExtension;
  if(fractional>0.0f){uint index=value_index(p,c,l,wholeBeginning-1u);
   result+=cell_trailing(q[index],left[index],right[index],fractional);}
  return result+open_local_forward(q,left,right,p,c,l,wholeBeginning,0.0f,whole);}
 float interiorLength=min(magnitude,float(p.n-face));
 return -(open_local_forward(q,left,right,p,c,l,face,0.0f,interiorLength)+
  (magnitude-interiorLength)*rightExtension);
}
kernel void face_flux(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* lowerAmbient [[buffer(2)]],device const float* upperAmbient [[buffer(3)]],
 device const float* left [[buffer(4)]],device const float* right [[buffer(5)]],
 device const float* prefix [[buffer(6)]],device float* flux [[buffer(7)]],
 constant Params& p [[buffer(8)]],
 uint gid [[thread_position_in_grid]]){
 bool periodic=p.lowerBoundary==0u&&p.upperBoundary==0u;
 uint faces=p.n+1u,activeFaces=periodic?p.n:faces,total=p.comps*p.lines*activeFaces;
 if(gid>=total)return;uint c=gid/(p.lines*activeFaces);uint rem=gid-c*p.lines*activeFaces;
 uint l=rem/activeFaces;uint f=rem-l*activeFaces;
 uint output=flux_index(p,c,l,f);
 if((f==0u&&p.lowerBoundary==2u)||(f==p.n&&p.upperBoundary==2u)){flux[output]=0.0f;return;}
 float velocity=u[l*faces+f],courant=p.dt*velocity/p.dx;
 float swept=periodic?periodic_swept(q,left,right,prefix,p,c,l,f,courant):
  open_swept(q,left,right,lowerAmbient,upperAmbient,p,c,l,f,courant,velocity);
 flux[output]=p.dx*swept;
}
kernel void update_cells(device const float* q [[buffer(0)]],device float* flux [[buffer(1)]],
 device float* updated [[buffer(2)]],constant Params& p [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){
 uint total=p.comps*p.lines*p.n;if(gid>=total)return;uint c=gid/(p.lines*p.n);
 uint rem=gid-c*p.lines*p.n;uint l=rem/p.n;uint cell=rem-l*p.n;
 uint base=flux_index(p,c,l,0u),right=cell+1u;
 if(p.lowerBoundary==0u&&p.upperBoundary==0u&&right==p.n){right=0u;flux[base+p.n]=flux[base];}
 updated[gid]=q[gid]-(flux[base+right]-flux[base+cell])/p.dx;
}
struct DualParams { uint nx; uint ny; uint nz; uint component; uint sweepAxis; };
struct DualLineParams { uint nx; uint ny; uint nz; uint component; uint sweepAxis;
 uint n; uint lines; uint componentBeginning; uint componentPeriodic;
 uint lowerComponentWall; uint upperComponentWall; };
inline uint dual_line_extent(constant DualLineParams& p,uint axis){return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint dual_line_face_index(constant DualLineParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline void dual_line_face_coordinates(constant DualLineParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline uint dual_extent(constant DualParams& p,uint axis){return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint dual_face_index(constant DualParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline void dual_face_coordinates(constant DualParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline void dual_set_coordinate(uint axis,uint value,thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u)x=value;else if(axis==1u)y=value;else z=value;
}
inline uint dual_coordinate(uint axis,uint x,uint y,uint z){return axis==0u?x:(axis==1u?y:z);}
kernel void gather_periodic_dual_values(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device float* values [[buffer(2)]],
 constant DualParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=2u*cells)return;uint channel=gid/cells,cell=gid-channel*cells;
 uint x=cell%p.nx,y=(cell/p.nx)%p.ny,z=cell/(p.nx*p.ny);
 uint face=dual_face_index(p,p.component,x,y,z);values[gid]=channel==0u?density[face]:momentum[face];
}
kernel void gather_periodic_dual_carrier(device const float* ux [[buffer(0)]],
 device const float* uy [[buffer(1)]],device const float* uz [[buffer(2)]],
 device float* carrier [[buffer(3)]],constant DualParams& p [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){
 uint faceCount=p.sweepAxis==0u?(p.nx+1u)*p.ny*p.nz:
  (p.sweepAxis==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
 if(gid>=faceCount)return;uint x,y,z;dual_face_coordinates(p,p.sweepAxis,gid,x,y,z);
 uint averageAxis=p.component==p.sweepAxis?p.sweepAxis:p.component;
 uint extent=dual_extent(p,averageAxis),coordinate=dual_coordinate(averageAxis,x,y,z);
 uint canonical=coordinate==extent?0u:coordinate,previous=canonical==0u?extent-1u:canonical-1u;
 uint lx=x,ly=y,lz=z,uxc=x,uyc=y,uzc=z;
 dual_set_coordinate(averageAxis,previous,lx,ly,lz);
 dual_set_coordinate(averageAxis,canonical,uxc,uyc,uzc);
 if(p.sweepAxis!=averageAxis){
  uint sweepExtent=dual_extent(p,p.sweepAxis);
  uint sweepCoordinate=dual_coordinate(p.sweepAxis,x,y,z);
  uint sweepCanonical=sweepCoordinate==sweepExtent?0u:sweepCoordinate;
  dual_set_coordinate(p.sweepAxis,sweepCanonical,lx,ly,lz);
  dual_set_coordinate(p.sweepAxis,sweepCanonical,uxc,uyc,uzc);
 }
 device const float* velocity=p.sweepAxis==0u?ux:(p.sweepAxis==1u?uy:uz);
 float lower=velocity[dual_face_index(p,p.sweepAxis,lx,ly,lz)];
 float upper=velocity[dual_face_index(p,p.sweepAxis,uxc,uyc,uzc)];
 carrier[gid]=0.5f*(lower+upper);
}
kernel void scatter_periodic_dual_values(device const float* values [[buffer(0)]],
 device float* density [[buffer(1)]],device float* momentum [[buffer(2)]],
 constant DualParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=2u*cells)return;uint channel=gid/cells,cell=gid-channel*cells;
 uint x=cell%p.nx,y=(cell/p.nx)%p.ny,z=cell/(p.nx*p.ny);
 uint face=dual_face_index(p,p.component,x,y,z);
 if(channel==0u)density[face]=values[gid];else momentum[face]=values[gid];
}
kernel void publish_periodic_dual_seam(device float* density [[buffer(0)]],
 device float* momentum [[buffer(1)]],constant DualParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint firstExtent=p.component==0u?p.ny:p.nx;
 uint secondExtent=p.component==2u?p.ny:p.nz;if(gid>=firstExtent*secondExtent)return;
 uint first=gid%firstExtent,second=gid/firstExtent;
 uint lx=p.component==0u?0u:first;
 uint ly=p.component==0u?first:(p.component==1u?0u:second);
 uint lz=p.component==2u?0u:second;uint hx=lx,hy=ly,hz=lz;
 dual_set_coordinate(p.component,dual_extent(p,p.component),hx,hy,hz);
 uint low=dual_face_index(p,p.component,lx,ly,lz),high=dual_face_index(p,p.component,hx,hy,hz);
 density[high]=density[low];momentum[high]=momentum[low];
}
kernel void gather_dual_line_values(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device float* values [[buffer(2)]],
 constant DualLineParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint valueCount=2u*p.lines*p.n;if(gid>=valueCount)return;
 uint channel=gid/(p.lines*p.n),rem=gid-channel*p.lines*p.n;
 uint line=rem/p.n,coordinate=rem-line*p.n,x=0u,y=0u,z=0u;
 if(p.component==p.sweepAxis){uint first=(p.component+1u)%3u,second=(p.component+2u)%3u;
  uint firstExtent=dual_line_extent(p,first);
  dual_set_coordinate(p.component,p.componentPeriodic!=0u?coordinate:coordinate+1u,x,y,z);
  dual_set_coordinate(first,line%firstExtent,x,y,z);dual_set_coordinate(second,line/firstExtent,x,y,z);
 }else{uint remainingAxis=3u-p.component-p.sweepAxis;
  uint remainingExtent=dual_line_extent(p,remainingAxis);
  dual_set_coordinate(p.component,p.componentBeginning+line/remainingExtent,x,y,z);
  dual_set_coordinate(p.sweepAxis,coordinate,x,y,z);
  dual_set_coordinate(remainingAxis,line%remainingExtent,x,y,z);
 }
 uint face=dual_line_face_index(p,p.component,x,y,z);
 values[gid]=channel==0u?density[face]:momentum[face];
}
kernel void scatter_dual_line_values(device const float* values [[buffer(0)]],
 device float* density [[buffer(1)]],device float* momentum [[buffer(2)]],
 constant DualLineParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint valueCount=2u*p.lines*p.n;if(gid>=valueCount)return;
 uint channel=gid/(p.lines*p.n),rem=gid-channel*p.lines*p.n;
 uint line=rem/p.n,coordinate=rem-line*p.n,x=0u,y=0u,z=0u;
 if(p.component==p.sweepAxis){uint first=(p.component+1u)%3u,second=(p.component+2u)%3u;
  uint firstExtent=dual_line_extent(p,first);
  dual_set_coordinate(p.component,p.componentPeriodic!=0u?coordinate:coordinate+1u,x,y,z);
  dual_set_coordinate(first,line%firstExtent,x,y,z);dual_set_coordinate(second,line/firstExtent,x,y,z);
 }else{uint remainingAxis=3u-p.component-p.sweepAxis;
  uint remainingExtent=dual_line_extent(p,remainingAxis);
  dual_set_coordinate(p.component,p.componentBeginning+line/remainingExtent,x,y,z);
  dual_set_coordinate(p.sweepAxis,coordinate,x,y,z);
  dual_set_coordinate(remainingAxis,line%remainingExtent,x,y,z);
 }
 uint face=dual_line_face_index(p,p.component,x,y,z);
 if(channel==0u)density[face]=values[gid];else momentum[face]=values[gid];
}
kernel void prescribe_dual_component_walls(device float* momentum [[buffer(0)]],
 constant DualLineParams& p [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 uint faceCount=p.component==0u?(p.nx+1u)*p.ny*p.nz:
  (p.component==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
 if(gid>=faceCount)return;uint x,y,z;
 dual_line_face_coordinates(p,p.component,gid,x,y,z);
 uint coordinate=dual_coordinate(p.component,x,y,z),extent=dual_line_extent(p,p.component);
 if((coordinate==0u&&p.lowerComponentWall!=0u)||(coordinate==extent&&p.upperComponentWall!=0u))
  momentum[gid]=0.0f;
}
kernel void add_cell_sources(device float* conservative [[buffer(0)]],
 device const float* source [[buffer(1)]],constant GridParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz,total=p.comps*cells;if(gid>=total)return;
 conservative[gid]+=source[gid];
}
kernel void extract_gas_density(device const float* conservative [[buffer(0)]],
 device float* density [[buffer(1)]],constant GridParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=cells)return;
 // The authoritative tuple is rho_tot Z, seven record-ordered constituent
 // densities, and sensible enthalpy.  Only CH4..CO (components 1..6) are gas.
 float gas=conservative[cells+gid];
 gas+=conservative[2u*cells+gid];gas+=conservative[3u*cells+gid];
 gas+=conservative[4u*cells+gid];gas+=conservative[5u*cells+gid];
 gas+=conservative[6u*cells+gid];density[gid]=gas;
}
kernel void add_face_sources(device float* momentum [[buffer(0)]],
 device const float* source [[buffer(1)]],constant uint& count [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){if(gid<count)momentum[gid]+=source[gid];}
struct ManifoldParams { uint cells; uint affineRows; uint affineDimension; uint reserved;
 float Tmin; float Tmax; float pressure; float feasibility; };
inline float methane_enthalpy(device const float* thermo,uint species,float temperature){
 device const float* record=thermo+32u*species;uint segmentCount=uint(record[1]);
 device const float* selected=record+2u;
 for(uint segment=0u;segment<segmentCount;++segment){device const float* candidate=record+2u+10u*segment;
  if(temperature>=candidate[0]&&(temperature<candidate[1]||
   (segment+1u==segmentCount&&temperature==candidate[1])))selected=candidate;}
 float inverse=1.0f/temperature,logT=log(temperature),t2=temperature*temperature;
 float t3=t2*temperature,t4=t3*temperature,t5=t4*temperature;
 float primitive=-selected[2]*inverse+selected[3]*logT+selected[4]*temperature+
  selected[5]*t2/2.0f+selected[6]*t3/3.0f+selected[7]*t4/4.0f+
  selected[8]*t5/5.0f;
 return 8314.46261815324f/record[0]*primitive+selected[9];
}
inline bool methane_state_admissible(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell){
 float values[9];for(uint component=0u;component<9u;++component){
  values[component]=state[component*p.cells+cell];if(!isfinite(values[component]))return false;}
 float totalMass=0.0f,massScale=abs(values[0]);for(uint species=0u;species<7u;++species){
  totalMass+=values[species+1u];massScale+=abs(values[species+1u]);}
 if(!(totalMass>0.0f)||!isfinite(totalMass))return false;massScale=max(1.0f,massScale);
 if(-values[0]>p.feasibility*massScale)return false;float closure=values[0];
 for(uint species=0u;species<7u;++species){closure-=values[species+1u];
  if(-values[species+1u]>p.feasibility*massScale)return false;}
 if(closure>p.feasibility*massScale)return false;
 float below=-values[8],above=values[8],energyScale=abs(values[8]);
 for(uint species=0u;species<7u;++species){float lowerH=methane_enthalpy(thermo,species,p.Tmin);
  float upperH=methane_enthalpy(thermo,species,p.Tmax),density=values[species+1u];
  below+=lowerH*density;above-=upperH*density;
  energyScale+=abs(lowerH*density)+abs(upperH*density);}
 energyScale=max(1.0f,energyScale);
 if(below>p.feasibility*energyScale||above>p.feasibility*energyScale)return false;
 device const float* matrix=thermo+224u;
 for(uint row=0u;row<p.affineRows;++row){float residual=0.0f,scale=0.0f;
  for(uint column=0u;column<p.affineDimension;++column){
   float term=matrix[row*p.affineDimension+column]*values[column];
   residual+=term;scale+=abs(term);}
  if(!isfinite(residual)||abs(residual)>p.feasibility*max(1.0f,scale))return false;}
 return true;
}
inline bool methane_temperature(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell,thread float& temperature){
 float sensible=state[8u*p.cells+cell],lowerEnergy=0.0f,upperEnergy=0.0f,scale=abs(sensible);
 for(uint species=0u;species<7u;++species){float density=state[(species+1u)*p.cells+cell];
  float lowerH=methane_enthalpy(thermo,species,p.Tmin);
  float upperH=methane_enthalpy(thermo,species,p.Tmax);
  lowerEnergy+=density*lowerH;upperEnergy+=density*upperH;
  scale+=abs(density*lowerH)+abs(density*upperH);}
 scale=max(1.0f,scale);float tolerance=p.feasibility*scale;temperature=0.0f;
 if(sensible<=lowerEnergy+tolerance)temperature=p.Tmin;
 else if(sensible>=upperEnergy-tolerance)temperature=p.Tmax;
 else {float lower=p.Tmin,upper=p.Tmax;
  for(uint iteration=0u;iteration<32u;++iteration){float midpoint=0.5f*(lower+upper),energy=0.0f;
   for(uint species=0u;species<7u;++species)
    energy+=state[(species+1u)*p.cells+cell]*methane_enthalpy(thermo,species,midpoint);
   if(energy<sensible)lower=midpoint;else upper=midpoint;}
  temperature=0.5f*(lower+upper);}
 return isfinite(temperature)&&temperature>=p.Tmin&&temperature<=p.Tmax;
}
inline bool methane_volume_ratio(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell,thread float& ratio){
 float temperature=0.0f;if(!methane_temperature(state,thermo,p,cell,temperature))return false;
 float molar=0.0f;for(uint species=0u;species<6u;++species)
  molar+=max(0.0f,state[(species+1u)*p.cells+cell])/thermo[32u*species];
 ratio=molar*8314.46261815324f*temperature/p.pressure;
 return isfinite(ratio)&&ratio>0.0f;
}
kernel void measure_methane_manifold(device const float* beginningDeviation [[buffer(0)]],
 device const float* terminal [[buffer(1)]],device const float* thermo [[buffer(2)]],
 device float2* deviationMap [[buffer(3)]],device atomic_uint* reduction [[buffer(4)]],
 constant ManifoldParams& p [[buffer(5)]],device atomic_uint* highHistogram [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;float terminalRatio=0.0f;
 if(!methane_state_admissible(terminal,thermo,p,gid)||
  !methane_volume_ratio(terminal,thermo,p,gid,terminalRatio)){
  atomic_fetch_or_explicit(reduction+2u,1u,memory_order_relaxed);return;}
 float beginning=beginningDeviation[gid],terminalDeviation=terminalRatio-1.0f;
 if(!isfinite(beginning)){atomic_fetch_or_explicit(reduction+2u,1u,memory_order_relaxed);return;}
 deviationMap[gid]=float2(beginning,terminalDeviation);
 float generation=abs(terminalDeviation-beginning),field=abs(terminalDeviation);
 atomic_fetch_max_explicit(reduction,as_type<uint>(generation),memory_order_relaxed);
 atomic_fetch_max_explicit(reduction+1u,as_type<uint>(field),memory_order_relaxed);
 atomic_fetch_add_explicit(highHistogram+(as_type<uint>(field)>>16u),1u,memory_order_relaxed);
}
kernel void select_methane_manifold_high_bins(device atomic_uint* histogram [[buffer(0)]],
 device uint* control [[buffer(1)]],constant ManifoldParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;
 uint ranks[2]={p.cells==0u?0u:(p.cells-1u)/2u,
  p.cells==0u?0u:((95u*p.cells+99u)/100u)-1u};
 for(uint quantile=0u;quantile<2u;++quantile){uint prefix=0u;
  for(uint bin=0u;bin<65536u;++bin){uint count=atomic_load_explicit(
    histogram+bin,memory_order_relaxed);
   if(ranks[quantile]<prefix+count){control[2u*quantile]=bin;
    control[2u*quantile+1u]=ranks[quantile]-prefix;break;}prefix+=count;}}
}
kernel void histogram_methane_manifold_low_bins(device const float2* deviationMap [[buffer(0)]],
 device const uint* control [[buffer(1)]],device atomic_uint* lowHistograms [[buffer(2)]],
 constant ManifoldParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;uint bits=as_type<uint>(abs(deviationMap[gid].y));uint high=bits>>16u;
 if(high==control[0])atomic_fetch_add_explicit(lowHistograms+(bits&65535u),1u,
  memory_order_relaxed);
 if(high==control[2])atomic_fetch_add_explicit(lowHistograms+65536u+(bits&65535u),1u,
  memory_order_relaxed);
}
kernel void select_methane_manifold_low_bins(device atomic_uint* lowHistograms [[buffer(0)]],
 device uint* control [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;
 for(uint quantile=0u;quantile<2u;++quantile){uint prefix=0u,target=control[2u*quantile+1u];
  for(uint bin=0u;bin<65536u;++bin){uint count=atomic_load_explicit(
    lowHistograms+quantile*65536u+bin,memory_order_relaxed);
   if(target<prefix+count){control[4u+quantile]=(control[2u*quantile]<<16u)|bin;break;}
   prefix+=count;}}
}
kernel void fold_methane_advective_anomaly_target(device const float2* deviationMap [[buffer(0)]],
 device float* restorationTarget [[buffer(1)]],constant float& inverseTimeStep [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 float2 deviation=deviationMap[gid];
 restorationTarget[gid]+=(deviation.y-deviation.x)*inverseTimeStep;
}
)METAL";
		}

		struct MetalRemapContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> reconstruct;
			id<MTLComputePipelineState> scan;
			id<MTLComputePipelineState> flux;
			id<MTLComputePipelineState> update;
			id<MTLComputePipelineState> gatherValues;
			id<MTLComputePipelineState> scatterValues;
			id<MTLComputePipelineState> gatherVelocity;
			id<MTLComputePipelineState> gatherPeriodicDualValues;
			id<MTLComputePipelineState> gatherPeriodicDualCarrier;
			id<MTLComputePipelineState> scatterPeriodicDualValues;
			id<MTLComputePipelineState> publishPeriodicDualSeam;
			id<MTLComputePipelineState> gatherDualLineValues;
			id<MTLComputePipelineState> scatterDualLineValues;
			id<MTLComputePipelineState> prescribeDualComponentWalls;
			id<MTLComputePipelineState> addCellSources;
			id<MTLComputePipelineState> extractGasDensity;
			id<MTLComputePipelineState> addFaceSources;
			id<MTLComputePipelineState> measureMethaneManifold;
			id<MTLComputePipelineState> selectMethaneManifoldHighBins;
			id<MTLComputePipelineState> histogramMethaneManifoldLowBins;
			id<MTLComputePipelineState> selectMethaneManifoldLowBins;
			id<MTLComputePipelineState> foldMethaneAdvectiveAnomalyTarget;
			std::string error;

			MetalRemapContext() : device(nil), queue(nil), reconstruct(nil), scan(nil),
				flux(nil), update(nil),gatherValues(nil),scatterValues(nil),gatherVelocity(nil),
				gatherPeriodicDualValues(nil),gatherPeriodicDualCarrier(nil),
				scatterPeriodicDualValues(nil),publishPeriodicDualSeam(nil),
				gatherDualLineValues(nil),scatterDualLineValues(nil),prescribeDualComponentWalls(nil),
				addCellSources(nil),extractGasDensity(nil),addFaceSources(nil),
				measureMethaneManifold(nil),selectMethaneManifoldHighBins(nil),
				histogramMethaneManifoldLowBins(nil),selectMethaneManifoldLowBins(nil),
				foldMethaneAdvectiveAnomalyTarget(nil)
			{
				@autoreleasepool {
					device=MTLCreateSystemDefaultDevice();
					if( !device ) { error="production fire remap has no Metal device";return; }
					MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
					if( @available(macOS 15.0,*) ) options.mathMode=MTLMathModeSafe;
					else { error="production fire remap requires Metal safe math mode";return; }
					NSError* metalError=nil;
					NSString* source=[NSString stringWithUTF8String:RemapSource()];
					id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&metalError];
					if( !library ) { error=MetalError("production fire remap library compilation failed",metalError);return; }
					auto makePipeline=[&](const char* name) -> id<MTLComputePipelineState> {
						id<MTLFunction> function=[library newFunctionWithName:
							[NSString stringWithUTF8String:name]];
						return function ? [device newComputePipelineStateWithFunction:function
							error:&metalError] : nil;
					};
					reconstruct=makePipeline("reconstruct");
					scan=makePipeline("scan_lines");
					flux=makePipeline("face_flux");
					update=makePipeline("update_cells");
					gatherValues=makePipeline("gather_grid_values");
					scatterValues=makePipeline("scatter_grid_values");
					gatherVelocity=makePipeline("gather_grid_velocity");
					gatherPeriodicDualValues=makePipeline("gather_periodic_dual_values");
					gatherPeriodicDualCarrier=makePipeline("gather_periodic_dual_carrier");
					scatterPeriodicDualValues=makePipeline("scatter_periodic_dual_values");
					publishPeriodicDualSeam=makePipeline("publish_periodic_dual_seam");
					gatherDualLineValues=makePipeline("gather_dual_line_values");
					scatterDualLineValues=makePipeline("scatter_dual_line_values");
					prescribeDualComponentWalls=makePipeline("prescribe_dual_component_walls");
					addCellSources=makePipeline("add_cell_sources");
					extractGasDensity=makePipeline("extract_gas_density");
					addFaceSources=makePipeline("add_face_sources");
					measureMethaneManifold=makePipeline("measure_methane_manifold");
					selectMethaneManifoldHighBins=makePipeline(
						"select_methane_manifold_high_bins");
					histogramMethaneManifoldLowBins=makePipeline(
						"histogram_methane_manifold_low_bins");
					selectMethaneManifoldLowBins=makePipeline(
						"select_methane_manifold_low_bins");
					foldMethaneAdvectiveAnomalyTarget=
						makePipeline("fold_methane_advective_anomaly_target");
					if( !reconstruct||!scan||!flux||!update||!gatherValues||
						!scatterValues||!gatherVelocity||!gatherPeriodicDualValues||
						!gatherPeriodicDualCarrier||!scatterPeriodicDualValues||
						!publishPeriodicDualSeam||!gatherDualLineValues||
						!scatterDualLineValues||!prescribeDualComponentWalls||
						!addCellSources||!extractGasDensity||!addFaceSources||
						!measureMethaneManifold||!selectMethaneManifoldHighBins||
						!histogramMethaneManifoldLowBins||!selectMethaneManifoldLowBins||
						!foldMethaneAdvectiveAnomalyTarget ) {
						error=MetalError("production fire remap pipeline creation failed",metalError);
						return;
					}
					queue=[device newCommandQueue];
					if( !queue ) error="production fire remap command queue allocation failed";
				}
			}

			bool Valid() const
			{
				return device&&queue&&reconstruct&&scan&&flux&&update&&gatherValues&&
					scatterValues&&gatherVelocity&&gatherPeriodicDualValues&&
					gatherPeriodicDualCarrier&&scatterPeriodicDualValues&&
					publishPeriodicDualSeam&&gatherDualLineValues&&scatterDualLineValues&&
				prescribeDualComponentWalls&&addCellSources&&extractGasDensity&&addFaceSources&&
				measureMethaneManifold&&selectMethaneManifoldHighBins&&
				histogramMethaneManifoldLowBins&&selectMethaneManifoldLowBins&&
				foldMethaneAdvectiveAnomalyTarget&&
					error.empty();
			}
		};

		MetalRemapContext& Context()
		{
			static MetalRemapContext context;
			return context;
		}

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;
			while( result<value ) result<<=1u;
			return result;
		}

		void Dispatch( id<MTLComputeCommandEncoder> encoder,
			id<MTLComputePipelineState> pipeline, std::size_t count )
		{
			const std::size_t width=std::min<std::size_t>(256u,
				static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]));
			[encoder setComputePipelineState:pipeline];
			[encoder dispatchThreads:MTLSizeMake(count,1,1)
				threadsPerThreadgroup:MTLSizeMake(width,1,1)];
		}

		bool AllFinite( const std::vector<float>& values )
		{
			return std::all_of(values.begin(),values.end(),
				[](float value){return std::isfinite(value);});
		}
	}

	bool RemapFireProductionMetal( const FireProductionRemapRequest& request,
		FireProductionRemapResult& result, std::string* structuredError )
	{
		result=FireProductionRemapResult();
		try {
		FireProductionRemapResult computed;
		if( !ValidateFireProductionRemapRequest(request,structuredError) ) return false;
		MetalRemapContext& context=Context();
		if( !context.Valid() ) {
			if( structuredError ) *structuredError=context.error;
			return false;
		}
		const std::size_t padded=NextPowerOfTwo(request.lineLength);
		if( padded>static_cast<std::size_t>([context.scan maxTotalThreadsPerThreadgroup]) ) {
			if( structuredError ) *structuredError="production fire remap line exceeds Metal scan width";
			return false;
		}

		@autoreleasepool {
			const std::size_t valueBytes=request.values.size()*sizeof(float);
			const std::size_t velocityBytes=request.faceVelocityMPerS.size()*sizeof(float);
			const std::vector<float>& lowerAmbientValues=request.lineSpecificAmbientValues?
				request.lowerAmbientValues:request.ambientValues;
			const std::vector<float>& upperAmbientValues=request.lineSpecificAmbientValues?
				request.upperAmbientValues:request.ambientValues;
			const std::size_t lowerAmbientBytes=lowerAmbientValues.size()*sizeof(float);
			const std::size_t upperAmbientBytes=upperAmbientValues.size()*sizeof(float);
			const std::size_t alphaCount=request.lineCount*request.lineLength;
			const std::size_t fluxCount=request.componentCount*request.lineCount*
				(request.lineLength+1u);
			id<MTLBuffer> values=[context.device newBufferWithBytes:request.values.data()
				length:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> velocity=[context.device newBufferWithBytes:request.faceVelocityMPerS.data()
				length:velocityBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> lowerAmbient=[context.device newBufferWithBytes:lowerAmbientValues.data()
				length:lowerAmbientBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> upperAmbient=[context.device newBufferWithBytes:upperAmbientValues.data()
				length:upperAmbientBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> left=[context.device newBufferWithLength:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> right=[context.device newBufferWithLength:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> alpha=[context.device newBufferWithLength:alphaCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> prefix=[context.device newBufferWithLength:fluxCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> flux=[context.device newBufferWithLength:fluxCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> updated=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModeShared];
			const MetalParameters parameters={static_cast<std::uint32_t>(request.lineLength),
				static_cast<std::uint32_t>(request.lineCount),
				static_cast<std::uint32_t>(request.componentCount),
				static_cast<std::uint32_t>(request.asymmetricBoundaries ? request.lowerBoundary :
					request.boundary),
				static_cast<std::uint32_t>(request.asymmetricBoundaries ? request.upperBoundary :
					request.boundary),request.lineSpecificAmbientValues?1u:0u,
				request.cellWidthM,request.timeStepS};
			id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			if( !values||!velocity||!lowerAmbient||!upperAmbient||!left||!right||!alpha||!prefix||!flux||
				!updated||!parameterBuffer ) {
				if( structuredError ) *structuredError="production fire remap buffer allocation failed";
				return false;
			}
			std::uint64_t certifiedWorkingSetBytes=0u,actualWorkingSetBytes=0u;
			if( !FireProductionRemapWorkingSetBytes(request,certifiedWorkingSetBytes) ) {
				if( structuredError ) *structuredError=
					"production fire remap working-set certificate failed";
				return false;
			}
			auto recordBuffer=[&](id<MTLBuffer> buffer) -> bool {
				const std::uint64_t allocated=static_cast<std::uint64_t>([buffer allocatedSize]);
				if( actualWorkingSetBytes>
					std::numeric_limits<std::uint64_t>::max()-allocated ) return false;
				actualWorkingSetBytes+=allocated;return true;
			};
			if( !recordBuffer(values)||!recordBuffer(velocity)||!recordBuffer(lowerAmbient)||
				!recordBuffer(upperAmbient)||!recordBuffer(left)||!recordBuffer(right)||
				!recordBuffer(alpha)||!recordBuffer(prefix)||!recordBuffer(flux)||
				!recordBuffer(updated)||!recordBuffer(parameterBuffer)||
				actualWorkingSetBytes>certifiedWorkingSetBytes||
				actualWorkingSetBytes>(std::uint64_t(2u)<<30u) ) {
				if( structuredError ) *structuredError=
					"production fire remap actual allocation exceeds certificate";
				return false;
			}
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			if( !command ) {
				if( structuredError ) *structuredError="production fire remap command allocation failed";
				return false;
			}
			id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap reconstruct encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:velocity offset:0 atIndex:1];
			[encoder setBuffer:lowerAmbient offset:0 atIndex:2];
			[encoder setBuffer:upperAmbient offset:0 atIndex:3];
			[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
			[encoder setBuffer:alpha offset:0 atIndex:6];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
			Dispatch(encoder,context.reconstruct,request.lineCount*request.lineLength);[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap scan encoder allocation failed";
				return false;
			}
			[encoder setComputePipelineState:context.scan];
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:prefix offset:0 atIndex:1];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
			[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
			[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*request.lineCount,1,1)
				threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap flux encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:velocity offset:0 atIndex:1];
			[encoder setBuffer:lowerAmbient offset:0 atIndex:2];
			[encoder setBuffer:upperAmbient offset:0 atIndex:3];
			[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
			[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
			const FireProductionRemapBoundary lower=request.asymmetricBoundaries ?
				request.lowerBoundary : request.boundary;
			const FireProductionRemapBoundary upper=request.asymmetricBoundaries ?
				request.upperBoundary : request.boundary;
			const std::size_t activeFluxCount=lower==FireProductionRemapPeriodic&&
				upper==FireProductionRemapPeriodic ?
				request.componentCount*request.lineCount*request.lineLength : fluxCount;
			Dispatch(encoder,context.flux,activeFluxCount);[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap update encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
			[encoder setBuffer:updated offset:0 atIndex:2];[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
			Dispatch(encoder,context.update,request.values.size());[encoder endEncoding];
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production fire remap command failed",[command error]);
				return false;
			}
			const float* updatedValues=static_cast<const float*>(ReadTrackedMetalBuffer(updated));
			const float* faceFluxes=static_cast<const float*>(ReadTrackedMetalBuffer(flux));
			const float* limiter=static_cast<const float*>(ReadTrackedMetalBuffer(alpha));
			computed.updatedValues.assign(updatedValues,updatedValues+request.values.size());
			computed.faceFluxes.assign(faceFluxes,faceFluxes+fluxCount);
			computed.sharedLimiterAlpha.assign(limiter,limiter+alphaCount);
			computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
		}
		if( !AllFinite(computed.updatedValues)||!AllFinite(computed.faceFluxes)||
			!AllFinite(computed.sharedLimiterAlpha)||!std::isfinite(computed.deviceElapsedMS) ) {
			if( structuredError )
				*structuredError="production fire remap produced nonfinite device output";
			return false;
		}
		if( structuredError ) structuredError->clear();
		result=std::move(computed);
		return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionRemapResult();
			if( structuredError ) try {
				*structuredError="production fire remap allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCellPalindromeMetal(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* structuredError )
	{
		result=FireProductionCellPalindromeResult();
		try {
		if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
		MetalRemapContext& context=Context();
		if( !context.Valid() ) {
			if( structuredError ) *structuredError=context.error;
			return false;
		}
		@autoreleasepool {
			const std::uint64_t beginningCommandCommitCount=MetalCommandCommitCount;
			const std::uint64_t beginningHostBufferReadCount=MetalHostBufferReadCount;
			const std::size_t cells=request.shape.CellCount();
			const std::size_t valueCount=request.componentCount*cells;
			const std::size_t valueBytes=valueCount*sizeof(float);
			const std::size_t xFaces=request.frozenVelocityMPerS[0].size();
			const std::size_t yFaces=request.frozenVelocityMPerS[1].size();
			const std::size_t zFaces=request.frozenVelocityMPerS[2].size();
			const std::size_t maximumLineFaces=std::max(xFaces,std::max(yFaces,zFaces));
			const std::size_t maximumFluxCount=request.componentCount*maximumLineFaces;
			std::uint64_t certifiedWorkingSetBytes=0u;
			if( !FireProductionCellPalindromeWorkingSetBytes(request.shape,
				request.componentCount,certifiedWorkingSetBytes) ) {
				if( structuredError ) *structuredError="production palindrome working-set certificate failed";
				return false;
			}
			id<MTLBuffer> inputStage=[context.device newBufferWithBytes:
				request.conservativeValues.data() length:valueBytes
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> xStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[0].data() length:xFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> yStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[1].data() length:yFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> zStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[2].data() length:zFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> gridA=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> gridB=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> xVelocity=[context.device newBufferWithLength:xFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> yVelocity=[context.device newBufferWithLength:yFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> zVelocity=[context.device newBufferWithLength:zFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineValues=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineUpdated=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> left=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> right=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineVelocity=[context.device newBufferWithLength:
				maximumLineFaces*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> alpha=[context.device newBufferWithLength:cells*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> prefix=[context.device newBufferWithLength:
				maximumFluxCount*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> flux=[context.device newBufferWithLength:
				maximumFluxCount*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambientValues.data()
				length:request.ambientValues.size()*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> outputStage=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModeShared];
			if( !inputStage||!xStage||!yStage||!zStage||!gridA||!gridB||!xVelocity||
				!yVelocity||!zVelocity||!lineValues||!lineUpdated||!left||!right||
				!lineVelocity||!alpha||!prefix||!flux||!ambient||!outputStage ) {
				if( structuredError ) *structuredError="production palindrome buffer allocation failed";
				return false;
			}
			std::uint64_t actualTrackedWorkingSetBytes=
				static_cast<std::uint64_t>(2u*valueBytes)+
				static_cast<std::uint64_t>(xFaces+yFaces+zFaces)*sizeof(float)+
				static_cast<std::uint64_t>(request.componentCount)*sizeof(float);
			std::uint32_t privateResidentBufferCount=0u,sharedBufferCount=0u;
			auto recordBuffer=[&](id<MTLBuffer> buffer, MTLStorageMode expected,
				std::uint32_t& count) -> bool {
				if( [buffer storageMode]!=expected ) return false;
				const std::uint64_t allocated=static_cast<std::uint64_t>([buffer allocatedSize]);
				if( actualTrackedWorkingSetBytes>
					std::numeric_limits<std::uint64_t>::max()-allocated ) return false;
				actualTrackedWorkingSetBytes+=allocated;++count;return true;
			};
			if( !recordBuffer(inputStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(xStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(yStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(zStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(ambient,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(outputStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(gridA,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(gridB,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(xVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(yVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(zVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineValues,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineUpdated,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(left,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(right,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(alpha,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(prefix,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(flux,MTLStorageModePrivate,privateResidentBufferCount) ) {
				if( structuredError ) *structuredError="production palindrome resource mode is invalid";
				return false;
			}
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			if( !command ) {
				if( structuredError ) *structuredError="production palindrome command allocation failed";
				return false;
			}
			id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
			if( !blit ) {
				if( structuredError ) *structuredError="production palindrome upload encoder failed";
				return false;
			}
			[blit copyFromBuffer:inputStage sourceOffset:0 toBuffer:gridA destinationOffset:0
				size:valueBytes];
			[blit copyFromBuffer:xStage sourceOffset:0 toBuffer:xVelocity destinationOffset:0
				size:xFaces*sizeof(float)];
			[blit copyFromBuffer:yStage sourceOffset:0 toBuffer:yVelocity destinationOffset:0
				size:yFaces*sizeof(float)];
			[blit copyFromBuffer:zStage sourceOffset:0 toBuffer:zVelocity destinationOffset:0
				size:zFaces*sizeof(float)];
			[blit endEncoding];
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
				request.timeStepS,0.5f*request.timeStepS,0.5f*request.timeStepS};
			auto boundaryValue=[](FireProductionProjectionBoundary boundary) -> std::uint32_t {
				return boundary==FireProductionProjectionPeriodic?0u:
					(boundary==FireProductionProjectionPressureOpen?1u:2u);
			};
			for( unsigned int pass=0u;pass<5u;++pass ) {
				const unsigned int axis=axes[pass];
				const std::size_t length=axis==0u?request.shape.nx:
					(axis==1u?request.shape.ny:request.shape.nz);
				const std::size_t lines=cells/length;
				const std::size_t lineFaces=lines*(length+1u);
				const std::size_t passFluxCount=request.componentCount*lineFaces;
				const MetalGridParameters gridParameters={
					static_cast<std::uint32_t>(request.shape.nx),
					static_cast<std::uint32_t>(request.shape.ny),
					static_cast<std::uint32_t>(request.shape.nz),axis,
					static_cast<std::uint32_t>(request.componentCount)};
				const MetalParameters parameters={static_cast<std::uint32_t>(length),
					static_cast<std::uint32_t>(lines),
					static_cast<std::uint32_t>(request.componentCount),
					boundaryValue(request.boundary[2u*axis]),
					boundaryValue(request.boundary[2u*axis+1u]),0u,request.shape.cellWidthM,
					steps[pass]};
				id<MTLBuffer> gridParameterBuffer=[context.device newBufferWithBytes:&gridParameters
					length:sizeof(gridParameters) options:MTLResourceStorageModeShared];
				id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
					length:sizeof(parameters) options:MTLResourceStorageModeShared];
				if( !gridParameterBuffer||!parameterBuffer ) {
					if( structuredError ) *structuredError="production palindrome parameter allocation failed";
					return false;
				}
				if( !recordBuffer(gridParameterBuffer,MTLStorageModeShared,sharedBufferCount)||
					!recordBuffer(parameterBuffer,MTLStorageModeShared,sharedBufferCount) ) {
					if( structuredError ) *structuredError="production palindrome parameter mode is invalid";
					return false;
				}
				id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome gather encoder failed";
					return false;
				}
				[encoder setBuffer:gridA offset:0 atIndex:0];[encoder setBuffer:lineValues offset:0 atIndex:1];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
				Dispatch(encoder,context.gatherValues,valueCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome velocity gather failed";
					return false;
				}
				[encoder setBuffer:xVelocity offset:0 atIndex:0];[encoder setBuffer:yVelocity offset:0 atIndex:1];
				[encoder setBuffer:zVelocity offset:0 atIndex:2];[encoder setBuffer:lineVelocity offset:0 atIndex:3];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:4];
				Dispatch(encoder,context.gatherVelocity,lineFaces);[encoder endEncoding];
				encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome reconstruct encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:lineVelocity offset:0 atIndex:1];
				[encoder setBuffer:ambient offset:0 atIndex:2];
				[encoder setBuffer:ambient offset:0 atIndex:3];
				[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
				[encoder setBuffer:alpha offset:0 atIndex:6];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
				Dispatch(encoder,context.reconstruct,cells);[encoder endEncoding];
				const std::size_t padded=NextPowerOfTwo(length);
				encoder=[command computeCommandEncoder];
				if( !encoder||padded>static_cast<std::size_t>(
					[context.scan maxTotalThreadsPerThreadgroup]) ) {
					if( structuredError ) *structuredError="production palindrome scan encoder failed";
					return false;
				}
				[encoder setComputePipelineState:context.scan];
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:prefix offset:0 atIndex:1];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
				[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
				[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*lines,1,1)
					threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome flux encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:lineVelocity offset:0 atIndex:1];
				[encoder setBuffer:ambient offset:0 atIndex:2];
				[encoder setBuffer:ambient offset:0 atIndex:3];
				[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
				[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
				const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
				Dispatch(encoder,context.flux,periodic?request.componentCount*lines*length:
					passFluxCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome update encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
				[encoder setBuffer:lineUpdated offset:0 atIndex:2];[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
				Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome scatter encoder failed";
					return false;
				}
				[encoder setBuffer:lineUpdated offset:0 atIndex:0];[encoder setBuffer:gridB offset:0 atIndex:1];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
				Dispatch(encoder,context.scatterValues,valueCount);[encoder endEncoding];
				std::swap(gridA,gridB);
			}
			blit=[command blitCommandEncoder];
			if( !blit ) {
				if( structuredError ) *structuredError="production palindrome publication encoder failed";
				return false;
			}
			[blit copyFromBuffer:gridA sourceOffset:0 toBuffer:outputStage destinationOffset:0
				size:valueBytes];[blit endEncoding];
			if( actualTrackedWorkingSetBytes>certifiedWorkingSetBytes||
				actualTrackedWorkingSetBytes>(std::uint64_t(2u)<<30u) ) {
				if( structuredError ) *structuredError="production palindrome actual allocation exceeds certificate";
				return false;
			}
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production palindrome command failed",[command error]);
				return false;
			}
			const float* output=static_cast<const float*>(ReadTrackedMetalBuffer(outputStage));
			const std::uint64_t commandCommitCount=
				MetalCommandCommitCount-beginningCommandCommitCount;
			const std::uint64_t hostBufferReadCount=
				MetalHostBufferReadCount-beginningHostBufferReadCount;
			if( commandCommitCount!=1u||hostBufferReadCount!=1u ) {
				if( structuredError ) *structuredError=
					"production palindrome command or host-read topology changed";
				return false;
			}
			result.conservativeValues.assign(output,output+valueCount);
			result.executedSubmapCount=5u;
			result.privateResidentBufferCount=privateResidentBufferCount;
			result.sharedBufferCount=sharedBufferCount;
			result.commandCommitCount=static_cast<std::uint32_t>(commandCommitCount);
			result.interstageFullGridReadbackCount=
				static_cast<std::uint32_t>(hostBufferReadCount-1u);
			result.certifiedWorkingSetBytes=certifiedWorkingSetBytes;
			result.actualTrackedWorkingSetBytes=actualTrackedWorkingSetBytes;
			result.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
		}
		if( !AllFinite(result.conservativeValues)||!std::isfinite(result.deviceElapsedMS) ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) *structuredError="production palindrome produced nonfinite output";
			return false;
		}
		if( structuredError ) structuredError->clear();
		return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) try {
				*structuredError="production palindrome allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCellPalindromeMetalResident(
		const FireProductionCellPalindromeRequest& request,
		const FireProductionMetalCellPalindromeResidentInput& input,
		FireProductionMetalCellPalindromeResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalCellPalindromeResidentResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				const std::size_t cells=request.shape.CellCount();
				const std::size_t valueCount=request.componentCount*cells;
				const std::size_t valueBytes=valueCount*sizeof(float);
				const std::size_t faceCounts[]={
					FireProductionProjectionFaceCount(request.shape,0u),
					FireProductionProjectionFaceCount(request.shape,1u),
					FireProductionProjectionFaceCount(request.shape,2u)};
				if( !input.conservativeValues||!input.ambientValues||
					[input.conservativeValues storageMode]!=MTLStorageModePrivate||
					[input.conservativeValues length]<valueBytes||
					[input.ambientValues length]<request.componentCount*sizeof(float) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome input ownership is invalid";
					return false;
				}
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!input.frozenVelocityMPerS[axis]||
					[input.frozenVelocityMPerS[axis] storageMode]!=MTLStorageModePrivate||
					[input.frozenVelocityMPerS[axis] length]<faceCounts[axis]*sizeof(float) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome carrier ownership is invalid";
					return false;
				}
				const std::size_t maximumLineFaces=std::max(faceCounts[0],
					std::max(faceCounts[1],faceCounts[2]));
				const std::size_t maximumFluxCount=request.componentCount*maximumLineFaces;
				auto privateBuffer=[&](std::size_t bytes) {
					return [context.device newBufferWithLength:bytes
						options:MTLResourceStorageModePrivate];
				};
				id<MTLBuffer> gridA=privateBuffer(valueBytes),gridB=privateBuffer(valueBytes);
				id<MTLBuffer> lineValues=privateBuffer(valueBytes),
					lineUpdated=privateBuffer(valueBytes),left=privateBuffer(valueBytes),
					right=privateBuffer(valueBytes),
					lineVelocity=privateBuffer(maximumLineFaces*sizeof(float)),
					alpha=privateBuffer(cells*sizeof(float)),
					prefix=privateBuffer(maximumFluxCount*sizeof(float)),
					flux=privateBuffer(maximumFluxCount*sizeof(float));
				const id<MTLBuffer> privateWork[]={gridA,gridB,lineValues,lineUpdated,left,right,
					lineVelocity,alpha,prefix,flux};
				std::uint64_t actualBytes=0u;
				auto record=[&](id<MTLBuffer> buffer, MTLStorageMode mode) {
					if( !buffer||[buffer storageMode]!=mode ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actualBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actualBytes+=bytes;return true;
				};
				if( !record(input.conservativeValues,MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[0],MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[1],MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[2],MTLStorageModePrivate)||
					!record(input.ambientValues,[input.ambientValues storageMode]) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome borrowed allocation is invalid";
					return false;
				}
				for( id<MTLBuffer> buffer : privateWork ) if(
					!record(buffer,MTLStorageModePrivate) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome work allocation failed";
					return false;
				}
				const std::uint64_t beginningCommandCommitCount=MetalCommandCommitCount;
				const std::uint64_t beginningHostBufferReadCount=MetalHostBufferReadCount;
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				if( !command ) {
					if( structuredError ) *structuredError=
						"production resident palindrome command allocation failed";
					return false;
				}
				id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
				if( !blit ) {
					if( structuredError ) *structuredError=
						"production resident palindrome initialization encoder failed";
					return false;
				}
				[blit copyFromBuffer:input.conservativeValues sourceOffset:0 toBuffer:gridA
					destinationOffset:0 size:valueBytes];[blit endEncoding];
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
					request.timeStepS,0.5f*request.timeStepS,0.5f*request.timeStepS};
				auto boundaryValue=[](FireProductionProjectionBoundary boundary) -> std::uint32_t {
					return boundary==FireProductionProjectionPeriodic?0u:
						(boundary==FireProductionProjectionPressureOpen?1u:2u);
				};
				for( unsigned int pass=0u;pass<5u;++pass ) {
					const unsigned int axis=axes[pass];
					const std::size_t length=axis==0u?request.shape.nx:
						(axis==1u?request.shape.ny:request.shape.nz);
					const std::size_t lines=cells/length,lineFaces=lines*(length+1u);
					const MetalGridParameters gridParameters={
						static_cast<std::uint32_t>(request.shape.nx),
						static_cast<std::uint32_t>(request.shape.ny),
						static_cast<std::uint32_t>(request.shape.nz),axis,
						static_cast<std::uint32_t>(request.componentCount)};
					const MetalParameters parameters={static_cast<std::uint32_t>(length),
						static_cast<std::uint32_t>(lines),
						static_cast<std::uint32_t>(request.componentCount),
						boundaryValue(request.boundary[2u*axis]),
						boundaryValue(request.boundary[2u*axis+1u]),0u,
						request.shape.cellWidthM,steps[pass]};
					id<MTLBuffer> gridParameterBuffer=[context.device newBufferWithBytes:&gridParameters
						length:sizeof(gridParameters) options:MTLResourceStorageModeShared];
					id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
						length:sizeof(parameters) options:MTLResourceStorageModeShared];
					if( !record(gridParameterBuffer,MTLStorageModeShared)||
						!record(parameterBuffer,MTLStorageModeShared) ) {
						if( structuredError ) *structuredError=
							"production resident palindrome parameter allocation failed";
						return false;
					}
					auto encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:gridA offset:0 atIndex:0];
					[encoder setBuffer:lineValues offset:0 atIndex:1];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.gatherValues,valueCount);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:input.frozenVelocityMPerS[0] offset:0 atIndex:0];
					[encoder setBuffer:input.frozenVelocityMPerS[1] offset:0 atIndex:1];
					[encoder setBuffer:input.frozenVelocityMPerS[2] offset:0 atIndex:2];
					[encoder setBuffer:lineVelocity offset:0 atIndex:3];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:4];
					Dispatch(encoder,context.gatherVelocity,lineFaces);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:lineVelocity offset:0 atIndex:1];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:2];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:3];
					[encoder setBuffer:left offset:0 atIndex:4];
					[encoder setBuffer:right offset:0 atIndex:5];
					[encoder setBuffer:alpha offset:0 atIndex:6];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
					Dispatch(encoder,context.reconstruct,cells);[encoder endEncoding];
					const std::size_t padded=NextPowerOfTwo(length);
					encoder=[command computeCommandEncoder];
					if( !encoder||padded>static_cast<std::size_t>(
						[context.scan maxTotalThreadsPerThreadgroup]) ) return false;
					[encoder setComputePipelineState:context.scan];
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:prefix offset:0 atIndex:1];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
					[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
					[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*lines,1,1)
						threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:lineVelocity offset:0 atIndex:1];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:2];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:3];
					[encoder setBuffer:left offset:0 atIndex:4];
					[encoder setBuffer:right offset:0 atIndex:5];
					[encoder setBuffer:prefix offset:0 atIndex:6];
					[encoder setBuffer:flux offset:0 atIndex:7];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
					const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
					Dispatch(encoder,context.flux,periodic?request.componentCount*lines*length:
						request.componentCount*lineFaces);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:flux offset:0 atIndex:1];
					[encoder setBuffer:lineUpdated offset:0 atIndex:2];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
					Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineUpdated offset:0 atIndex:0];
					[encoder setBuffer:gridB offset:0 atIndex:1];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.scatterValues,valueCount);[encoder endEncoding];
					std::swap(gridA,gridB);
				}
				if( actualBytes>(UINT64_C(1)<<31u) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome working set exceeds two GiB";
					return false;
				}
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) {
					if( structuredError ) *structuredError=MetalError(
						"production resident palindrome command failed",[command error]);
					return false;
				}
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommandCommitCount;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningHostBufferReadCount;
				if( commits!=1u||reads!=0u ) {
					if( structuredError ) *structuredError=
						"production resident palindrome transfer topology changed";
					return false;
				}
				FireProductionMetalCellPalindromeResidentResult computed;
				computed.conservativeValues=gridA;computed.executedSubmapCount=5u;
				computed.commandCommitCount=1u;computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=actualBytes;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceStartTimeS=[command GPUStartTime];
				computed.deviceEndTimeS=[command GPUEndTime];
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalCellPalindromeResidentResult();
			if( structuredError ) try {
				*structuredError="production resident palindrome allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCellPalindromeMetalResidentComparator(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* structuredError )
	{
		result=FireProductionCellPalindromeResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				const std::size_t cells=request.shape.CellCount();
				const std::size_t valueCount=request.componentCount*cells;
				const std::size_t valueBytes=valueCount*sizeof(float);
				std::array<std::size_t,3> faceCounts;
				for( unsigned int axis=0u;axis<3u;++axis )
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
				id<MTLBuffer> inputStage=[context.device newBufferWithBytes:
					request.conservativeValues.data() length:valueBytes
					options:MTLResourceStorageModeShared];
				std::array<id<MTLBuffer>,3> velocityStage,velocityPrivate;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					velocityStage[axis]=[context.device newBufferWithBytes:
						request.frozenVelocityMPerS[axis].data()
						length:faceCounts[axis]*sizeof(float)
						options:MTLResourceStorageModeShared];
					velocityPrivate[axis]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModePrivate];
				}
				id<MTLBuffer> inputPrivate=[context.device newBufferWithLength:valueBytes
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambientValues.data()
					length:request.componentCount*sizeof(float)
					options:MTLResourceStorageModeShared];
				if( !inputStage||!inputPrivate||!ambient ) return false;
				for( unsigned int axis=0u;axis<3u;++axis )
					if( !velocityStage[axis]||!velocityPrivate[axis] ) return false;
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !blit ) return false;
				[blit copyFromBuffer:inputStage sourceOffset:0 toBuffer:inputPrivate
					destinationOffset:0 size:valueBytes];
				for( unsigned int axis=0u;axis<3u;++axis )
					[blit copyFromBuffer:velocityStage[axis] sourceOffset:0
						toBuffer:velocityPrivate[axis] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalCellPalindromeResidentInput residentInput;
				residentInput.conservativeValues=inputPrivate;
				residentInput.frozenVelocityMPerS=velocityPrivate;
				residentInput.ambientValues=ambient;
				FireProductionMetalCellPalindromeResidentResult resident;
				if( !RemapFireProductionCellPalindromeMetalResident(request,residentInput,
					resident,structuredError) ) return false;
				id<MTLBuffer> outputStage=[context.device newBufferWithLength:valueBytes
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> staging=TrackedMetalCommandBuffer(context.queue);
				blit=staging?[staging blitCommandEncoder]:nil;
				if( !outputStage||!blit ) return false;
				[blit copyFromBuffer:resident.conservativeValues sourceOffset:0
					toBuffer:outputStage destinationOffset:0 size:valueBytes];
				[blit endEncoding];CommitTrackedMetalCommand(staging);[staging waitUntilCompleted];
				if( [staging status]!=MTLCommandBufferStatusCompleted ) return false;
				const float* output=static_cast<const float*>(ReadTrackedMetalBuffer(outputStage));
				FireProductionCellPalindromeResult computed;
				computed.conservativeValues.assign(output,output+valueCount);
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridReadbackCount=
					resident.interstageFullGridTransferCount;
				computed.actualTrackedWorkingSetBytes=resident.actualMetalAllocationBytes;
				if( !FireProductionCellPalindromeWorkingSetBytes(request.shape,
					request.componentCount,computed.certifiedWorkingSetBytes) ) return false;
				computed.deviceElapsedMS=resident.deviceElapsedMS;
				if( !AllFinite(computed.conservativeValues)||
					!std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) try {
				*structuredError="production resident palindrome comparator allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionPeriodicDualMomentumMetalResident(
		const FireProductionPeriodicDualMomentumRequest& request,
		const FireProductionMetalPeriodicDualMomentumResidentInput& input,
		FireProductionMetalPeriodicDualMomentumResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalPeriodicDualMomentumResidentResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionPeriodicDualMomentumResidentWorkingSetBytes(request.shape,
				certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident periodic dual working set exceeds two GiB";
				return false;
			}
			std::array<FireProductionCellPalindromeRequest,3> dualRequest;
			for( unsigned int component=0u;component<3u;++component ) if(
				!BuildFireProductionPeriodicDualCellRequest(request,component,
					dualRequest[component],structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					const id<MTLBuffer> buffers[]={input.beginningFaceDensity[axis],
						input.beginningMomentum[axis],input.frozenVelocityMPerS[axis]};
					for( id<MTLBuffer> buffer : buffers ) if( !buffer||
						[buffer storageMode]!=MTLStorageModePrivate||
						[buffer length]<faceCounts[axis]*sizeof(float) ) {
						if( structuredError ) *structuredError=
							"production resident periodic dual input ownership is invalid";
						return false;
					}
				}
				const std::size_t cells=request.shape.CellCount();
				const std::size_t dualBytes=2u*cells*sizeof(float);
				std::uint64_t borrowedBytes=0u,outputBytes=0u,maximumActual=0u;
				auto addAllocation=[](id<MTLBuffer> buffer,std::uint64_t& total) {
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( total>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					total+=bytes;return true;
				};
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!addAllocation(input.beginningFaceDensity[axis],borrowedBytes)||
					!addAllocation(input.beginningMomentum[axis],borrowedBytes)||
					!addAllocation(input.frozenVelocityMPerS[axis],borrowedBytes) ) return false;
				FireProductionMetalPeriodicDualMomentumResidentResult computed;
				for( unsigned int component=0u;component<3u;++component ) {
					computed.auxiliaryFaceDensity[component]=[context.device newBufferWithLength:
						faceCounts[component]*sizeof(float) options:MTLResourceStorageModePrivate];
					computed.momentum[component]=[context.device newBufferWithLength:
						faceCounts[component]*sizeof(float) options:MTLResourceStorageModePrivate];
					if( !computed.auxiliaryFaceDensity[component]||!computed.momentum[component]||
						!addAllocation(computed.auxiliaryFaceDensity[component],outputBytes)||
						!addAllocation(computed.momentum[component],outputBytes) ) return false;
				}
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				double deviceMS=0.0;
				for( unsigned int component=0u;component<3u;++component ) {
					id<MTLBuffer> dualInput=[context.device newBufferWithLength:dualBytes
						options:MTLResourceStorageModePrivate];
					std::array<id<MTLBuffer>,3> carrier;
					std::array<id<MTLBuffer>,3> parameter;
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						carrier[sweep]=[context.device newBufferWithLength:
							faceCounts[sweep]*sizeof(float) options:MTLResourceStorageModePrivate];
						const MetalPeriodicDualParameters values={
							static_cast<std::uint32_t>(request.shape.nx),
							static_cast<std::uint32_t>(request.shape.ny),
							static_cast<std::uint32_t>(request.shape.nz),component,sweep};
						parameter[sweep]=[context.device newBufferWithBytes:&values length:sizeof(values)
							options:MTLResourceStorageModeShared];
					}
					const float ambientValues[]={0.0f,0.0f};
					id<MTLBuffer> ambient=[context.device newBufferWithBytes:ambientValues
						length:sizeof(ambientValues) options:MTLResourceStorageModeShared];
					if( !dualInput||!ambient ) return false;
					std::uint64_t packingBytes=0u;
					if( !addAllocation(dualInput,packingBytes)||!addAllocation(ambient,packingBytes) )
						return false;
					for( unsigned int sweep=0u;sweep<3u;++sweep ) if( !carrier[sweep]||
						!parameter[sweep]||!addAllocation(carrier[sweep],packingBytes)||
						!addAllocation(parameter[sweep],packingBytes) ) return false;
					id<MTLCommandBuffer> pack=TrackedMetalCommandBuffer(context.queue);
					id<MTLBlitCommandEncoder> blit=pack?[pack blitCommandEncoder]:nil;
					if( !blit ) return false;
					[blit copyFromBuffer:input.beginningFaceDensity[component] sourceOffset:0
						toBuffer:computed.auxiliaryFaceDensity[component] destinationOffset:0
						size:faceCounts[component]*sizeof(float)];
					[blit copyFromBuffer:input.beginningMomentum[component] sourceOffset:0
						toBuffer:computed.momentum[component] destinationOffset:0
						size:faceCounts[component]*sizeof(float)];[blit endEncoding];
					id<MTLComputeCommandEncoder> encoder=[pack computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:0];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:1];
					[encoder setBuffer:dualInput offset:0 atIndex:2];
					[encoder setBuffer:parameter[0] offset:0 atIndex:3];
					Dispatch(encoder,context.gatherPeriodicDualValues,2u*cells);[encoder endEncoding];
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						encoder=[pack computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:input.frozenVelocityMPerS[0] offset:0 atIndex:0];
						[encoder setBuffer:input.frozenVelocityMPerS[1] offset:0 atIndex:1];
						[encoder setBuffer:input.frozenVelocityMPerS[2] offset:0 atIndex:2];
						[encoder setBuffer:carrier[sweep] offset:0 atIndex:3];
						[encoder setBuffer:parameter[sweep] offset:0 atIndex:4];
						Dispatch(encoder,context.gatherPeriodicDualCarrier,faceCounts[sweep]);
						[encoder endEncoding];
					}
					CommitTrackedMetalCommand(pack);[pack waitUntilCompleted];
					if( [pack status]!=MTLCommandBufferStatusCompleted ) return false;
					deviceMS+=([pack GPUEndTime]-[pack GPUStartTime])*1000.0;
					FireProductionMetalCellPalindromeResidentInput cellInput;
					cellInput.conservativeValues=dualInput;cellInput.frozenVelocityMPerS=carrier;
					cellInput.ambientValues=ambient;
					FireProductionMetalCellPalindromeResidentResult cellResult;
					if( !RemapFireProductionCellPalindromeMetalResident(dualRequest[component],
						cellInput,cellResult,structuredError) ) return false;
					deviceMS+=cellResult.deviceElapsedMS;
					id<MTLCommandBuffer> scatter=TrackedMetalCommandBuffer(context.queue);
					encoder=scatter?[scatter computeCommandEncoder]:nil;if( !encoder ) return false;
					[encoder setBuffer:cellResult.conservativeValues offset:0 atIndex:0];
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:1];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:2];
					[encoder setBuffer:parameter[0] offset:0 atIndex:3];
					Dispatch(encoder,context.scatterPeriodicDualValues,2u*cells);[encoder endEncoding];
					encoder=[scatter computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:0];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:1];
					[encoder setBuffer:parameter[0] offset:0 atIndex:2];
					const std::size_t seamCount=component==0u?request.shape.ny*request.shape.nz:
						(component==1u?request.shape.nx*request.shape.nz:
						 request.shape.nx*request.shape.ny);
					Dispatch(encoder,context.publishPeriodicDualSeam,seamCount);[encoder endEncoding];
					CommitTrackedMetalCommand(scatter);[scatter waitUntilCompleted];
					if( [scatter status]!=MTLCommandBufferStatusCompleted ) return false;
					deviceMS+=([scatter GPUEndTime]-[scatter GPUStartTime])*1000.0;
					if( borrowedBytes>std::numeric_limits<std::uint64_t>::max()-outputBytes||
						borrowedBytes+outputBytes>std::numeric_limits<std::uint64_t>::max()-
							packingBytes||borrowedBytes+outputBytes+packingBytes>
							std::numeric_limits<std::uint64_t>::max()-cellResult.actualMetalAllocationBytes )
						return false;
					maximumActual=std::max(maximumActual,borrowedBytes+outputBytes+packingBytes+
						cellResult.actualMetalAllocationBytes);
				}
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningReads;
				if( commits!=9u||reads!=0u||maximumActual>certifiedBytes||
					maximumActual>(UINT64_C(1)<<31u) ) return false;
				computed.executedSubmapCount=15u;computed.commandCommitCount=9u;
				computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=maximumActual;
				computed.deviceElapsedMS=deviceMS;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalPeriodicDualMomentumResidentResult();
			if( structuredError ) try {
				*structuredError="production resident periodic dual allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionPeriodicDualMomentumMetal(
		const FireProductionPeriodicDualMomentumRequest& request,
		FireProductionPeriodicDualMomentumResult& result, std::string* structuredError )
	{
		result=FireProductionPeriodicDualMomentumResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionPeriodicDualMomentumResidentWorkingSetBytes(request.shape,
				certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident periodic dual working set exceeds two GiB";
				return false;
			}
			FireProductionCellPalindromeRequest validated;
			if( !BuildFireProductionPeriodicDualCellRequest(request,0u,validated,
				structuredError) ) return false;
			MetalRemapContext& context=Context();if( !context.Valid() ) return false;
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts;
				FireProductionMetalPeriodicDualMomentumResidentInput residentInput;
				std::array<std::array<id<MTLBuffer>,3>,3> stage;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					const std::vector<float>* values[]={&request.beginningFaceDensity[axis],
						&request.beginningMomentum[axis],&request.frozenVelocityMPerS[axis]};
					for( unsigned int role=0u;role<3u;++role ) {
						stage[axis][role]=[context.device newBufferWithBytes:values[role]->data()
							length:faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
						id<MTLBuffer> privateValue=[context.device newBufferWithLength:
							faceCounts[axis]*sizeof(float)
							options:MTLResourceStorageModePrivate];
						if( role==0u ) residentInput.beginningFaceDensity[axis]=privateValue;
						else if( role==1u ) residentInput.beginningMomentum[axis]=privateValue;
						else residentInput.frozenVelocityMPerS[axis]=privateValue;
						if( !stage[axis][role]||!privateValue ) return false;
					}
				}
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;if( !blit ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					id<MTLBuffer> resident[]={residentInput.beginningFaceDensity[axis],
						residentInput.beginningMomentum[axis],residentInput.frozenVelocityMPerS[axis]};
					for( unsigned int role=0u;role<3u;++role )
						[blit copyFromBuffer:stage[axis][role] sourceOffset:0 toBuffer:resident[role]
							destinationOffset:0 size:faceCounts[axis]*sizeof(float)];
				}
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalPeriodicDualMomentumResidentResult resident;
				if( !RemapFireProductionPeriodicDualMomentumMetalResident(request,residentInput,
					resident,structuredError) ) return false;
				std::array<std::array<id<MTLBuffer>,2>,3> outputStage;
				id<MTLCommandBuffer> staging=TrackedMetalCommandBuffer(context.queue);
				blit=staging?[staging blitCommandEncoder]:nil;if( !blit ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					outputStage[axis][0]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
					outputStage[axis][1]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
					if( !outputStage[axis][0]||!outputStage[axis][1] ) return false;
					[blit copyFromBuffer:resident.auxiliaryFaceDensity[axis] sourceOffset:0
						toBuffer:outputStage[axis][0] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
					[blit copyFromBuffer:resident.momentum[axis] sourceOffset:0
						toBuffer:outputStage[axis][1] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
				}
				[blit endEncoding];CommitTrackedMetalCommand(staging);[staging waitUntilCompleted];
				if( [staging status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionPeriodicDualMomentumResult computed;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const float* density=static_cast<const float*>(
						ReadTrackedMetalBuffer(outputStage[axis][0]));
					const float* momentum=static_cast<const float*>(
						ReadTrackedMetalBuffer(outputStage[axis][1]));
					computed.auxiliaryFaceDensity[axis].assign(density,density+faceCounts[axis]);
					computed.momentum[axis].assign(momentum,momentum+faceCounts[axis]);
				}
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.canonicalSeamCopyCount=static_cast<std::uint32_t>(2u*(
					request.shape.ny*request.shape.nz+request.shape.nx*request.shape.nz+
					request.shape.nx*request.shape.ny));
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridTransferCount=resident.interstageFullGridTransferCount;
				computed.actualMetalAllocationBytes=resident.actualMetalAllocationBytes;
				computed.deviceElapsedMS=resident.deviceElapsedMS;
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!AllFinite(computed.auxiliaryFaceDensity[axis])||
					!AllFinite(computed.momentum[axis]) ) return false;
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionPeriodicDualMomentumResult();
			if( structuredError ) try {
				*structuredError="production periodic dual Metal allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool PrepareFireProductionDualMomentumMetalStaticState(
		const FireProductionDualMomentumRequest& request,
		FireProductionMetalDualMomentumStaticState& state,
		std::string* structuredError )
	{
		state=FireProductionMetalDualMomentumStaticState();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionDualMomentumResidentWorkingSetBytes(request.shape,
				request.boundary,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident dual working set exceeds two GiB";
				return false;
			}
			if( !ValidateDualMomentumStaticOwnerMetadata(request,structuredError) ) {
				if( structuredError ) *structuredError=
					"production resident dual static metadata is invalid";
				return false;
			}
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;return false;
			}
			@autoreleasepool {
				const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
					request.timeStepS};
				std::array<std::array<FireProductionRemapRequest,3>,3> packed;
				std::array<std::string,9> packErrors;
				std::array<bool,9> packSucceeded={{false,false,false,false,false,false,false,false,false}};
				auto packTask=[&](const unsigned int task){
					const unsigned int component=task/3u,sweep=task%3u;
					packSucceeded[task]=BuildFireProductionDualAxisRequest(request,component,sweep,
						steps[sweep],request.beginningFaceDensity[component],
						request.beginningMomentum[component],packed[component][sweep],
						&packErrors[task]);
				};
				const char* audit=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
				const char* packMode=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE");
				const bool serialPack=audit&&std::strcmp(audit,"1")==0&&packMode&&
					std::strcmp(packMode,"serial")==0;
				// Legacy low-priority workers do not steal while waiting, so nested
				// ParallelFor is not recursion-safe when a render pool is saturated.
				const bool legacyLowPriority=GlobalOptions().ReadBool(
					"force_all_threads_low_priority",false);
				if( FireProductionDualLayoutPackRequiresSerialOwner(
					serialPack,legacyLowPriority) )
					for(unsigned int task=0u;task<9u;++task)packTask(task);
				else Implementation::GlobalThreadPool().ParallelFor(9u,packTask);
				for(unsigned int task=0u;task<packSucceeded.size();++task)if(!packSucceeded[task]){
					if(structuredError)*structuredError=packErrors[task];return false;}
				std::array<std::array<id<MTLBuffer>,3>,3> velocityStage,lowerStage,upperStage;
				FireProductionMetalDualMomentumStaticState computed;
				computed.shape=request.shape;computed.timeStepS=request.timeStepS;
				computed.boundary=request.boundary;
				std::uint64_t actual=0u;
				auto add=[&](id<MTLBuffer> buffer) {
					if( !buffer ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actual>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actual+=bytes;return true;
				};
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionRemapRequest& line=packed[component][sweep];
						FireProductionMetalDualAxisStaticState& output=computed.axis[component][sweep];
						output.lineLength=static_cast<std::uint32_t>(line.lineLength);
						output.lineCount=static_cast<std::uint32_t>(line.lineCount);
						output.lowerBoundary=static_cast<std::uint32_t>(line.lowerBoundary);
						output.upperBoundary=static_cast<std::uint32_t>(line.upperBoundary);
						output.ambientPerLine=line.lineSpecificAmbientValues?1u:0u;
						const std::vector<float>& lower=line.lineSpecificAmbientValues?
							line.lowerAmbientValues:line.ambientValues;
						const std::vector<float>& upper=line.lineSpecificAmbientValues?
							line.upperAmbientValues:line.ambientValues;
						velocityStage[component][sweep]=[context.device newBufferWithBytes:
							line.faceVelocityMPerS.data() length:line.faceVelocityMPerS.size()*sizeof(float)
							options:MTLResourceStorageModeShared];
						lowerStage[component][sweep]=[context.device newBufferWithBytes:lower.data()
							length:lower.size()*sizeof(float) options:MTLResourceStorageModeShared];
						upperStage[component][sweep]=[context.device newBufferWithBytes:upper.data()
							length:upper.size()*sizeof(float) options:MTLResourceStorageModeShared];
						output.faceVelocityMPerS=[context.device newBufferWithLength:
							line.faceVelocityMPerS.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						output.lowerAmbientValues=[context.device newBufferWithLength:
							lower.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						output.upperAmbientValues=[context.device newBufferWithLength:
							upper.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						if( !velocityStage[component][sweep]||!lowerStage[component][sweep]||
							!upperStage[component][sweep]||!add(output.faceVelocityMPerS)||
							!add(output.lowerAmbientValues)||!add(output.upperAmbientValues) ) return false;
					}
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !blit ) return false;
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionRemapRequest& line=packed[component][sweep];
						const std::vector<float>& lower=line.lineSpecificAmbientValues?
							line.lowerAmbientValues:line.ambientValues;
						const std::vector<float>& upper=line.lineSpecificAmbientValues?
							line.upperAmbientValues:line.ambientValues;
						const FireProductionMetalDualAxisStaticState& output=computed.axis[component][sweep];
						[blit copyFromBuffer:velocityStage[component][sweep] sourceOffset:0
							toBuffer:output.faceVelocityMPerS destinationOffset:0
							size:line.faceVelocityMPerS.size()*sizeof(float)];
						[blit copyFromBuffer:lowerStage[component][sweep] sourceOffset:0
							toBuffer:output.lowerAmbientValues destinationOffset:0
							size:lower.size()*sizeof(float)];
						[blit copyFromBuffer:upperStage[component][sweep] sourceOffset:0
							toBuffer:output.upperAmbientValues destinationOffset:0
							size:upper.size()*sizeof(float)];
					}
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				const char* velocityAudit=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
				if( velocityAudit&&std::strcmp(velocityAudit,"1")==0 ) std::fprintf(stderr,
					"TIMESTEP_VELOCITY_DEVICE dual_static_upload=%.9g\n",
					([upload GPUEndTime]-[upload GPUStartTime])*1000.0);
				computed.uploadCommandCommitCount=1u;computed.actualMetalAllocationBytes=actual;
				state=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			state=FireProductionMetalDualMomentumStaticState();
			if( structuredError ) try { *structuredError=
				"production dual static-state allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionDualMomentumMetalResident(
		const FireProductionDualMomentumRequest& request,
		const FireProductionMetalDualMomentumStaticState& staticState,
		const FireProductionMetalDualMomentumResidentInput& input,
		FireProductionMetalDualMomentumResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalDualMomentumResidentResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionDualMomentumResidentWorkingSetBytes(request.shape,
				request.boundary,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident dual working set exceeds two GiB";
				return false;
			}
			if( staticState.shape.nx!=request.shape.nx||staticState.shape.ny!=request.shape.ny||
				staticState.shape.nz!=request.shape.nz||
				staticState.shape.cellWidthM!=request.shape.cellWidthM||
				staticState.timeStepS!=request.timeStepS||staticState.boundary!=request.boundary ) {
				if( structuredError ) *structuredError=
					"production resident dual static ownership does not match the request";
				return false;
			}
			MetalRemapContext& context=Context();
			if( !context.Valid() ) { if( structuredError ) *structuredError=context.error;return false; }
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts,canonicalOffset;
				std::size_t allFaces=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					canonicalOffset[axis]=allFaces*sizeof(float);allFaces+=faceCounts[axis];
				}
				const std::size_t packedBytes=allFaces*sizeof(float);
				if( !input.packedFaceDensity||!input.packedMomentum||
					[input.packedFaceDensity storageMode]!=MTLStorageModePrivate||
					[input.packedMomentum storageMode]!=MTLStorageModePrivate||
					[input.packedFaceDensity length]<packedBytes||[input.packedMomentum length]<packedBytes||
					input.faceByteOffset!=canonicalOffset ) return false;
				std::size_t maximumValues=0u,maximumFaces=0u,maximumCells=0u;
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionMetalDualAxisStaticState& axis=staticState.axis[component][sweep];
						const std::size_t values=2u*axis.lineLength*axis.lineCount;
						const std::size_t faces=axis.lineCount*(axis.lineLength+1u);
						const std::size_t ambient=axis.ambientPerLine?2u*axis.lineCount:2u;
						if( axis.lineLength<4u||axis.lineCount==0u||!axis.faceVelocityMPerS||
							!axis.lowerAmbientValues||!axis.upperAmbientValues||
							[axis.faceVelocityMPerS storageMode]!=MTLStorageModePrivate||
							[axis.lowerAmbientValues storageMode]!=MTLStorageModePrivate||
							[axis.upperAmbientValues storageMode]!=MTLStorageModePrivate||
							[axis.faceVelocityMPerS length]<faces*sizeof(float)||
							[axis.lowerAmbientValues length]<ambient*sizeof(float)||
							[axis.upperAmbientValues length]<ambient*sizeof(float) ) return false;
						maximumValues=std::max(maximumValues,values);
						maximumFaces=std::max(maximumFaces,faces);
						maximumCells=std::max(maximumCells,
							static_cast<std::size_t>(axis.lineLength)*axis.lineCount);
					}
				auto privateBuffer=[&](std::size_t bytes) { return [context.device
					newBufferWithLength:bytes options:MTLResourceStorageModePrivate]; };
				id<MTLBuffer> density=privateBuffer(packedBytes),momentum=privateBuffer(packedBytes);
				id<MTLBuffer> lineValues=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> lineUpdated=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> left=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> right=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> alpha=privateBuffer(maximumCells*sizeof(float));
				id<MTLBuffer> prefix=privateBuffer(2u*maximumFaces*sizeof(float));
				id<MTLBuffer> flux=privateBuffer(2u*maximumFaces*sizeof(float));
				const id<MTLBuffer> owned[]={density,momentum,lineValues,lineUpdated,left,right,
					alpha,prefix,flux};
				std::uint64_t actual=staticState.actualMetalAllocationBytes;
				auto add=[&](id<MTLBuffer> buffer,MTLStorageMode mode) {
					if( !buffer||[buffer storageMode]!=mode ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actual>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actual+=bytes;return true;
				};
				if( !add(input.packedFaceDensity,MTLStorageModePrivate)||
					!add(input.packedMomentum,MTLStorageModePrivate) ) return false;
				for( id<MTLBuffer> buffer : owned ) if( !add(buffer,MTLStorageModePrivate) ) return false;
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if( !blit ) return false;
				[blit copyFromBuffer:input.packedFaceDensity sourceOffset:0 toBuffer:density
					destinationOffset:0 size:packedBytes];
				[blit copyFromBuffer:input.packedMomentum sourceOffset:0 toBuffer:momentum
					destinationOffset:0 size:packedBytes];[blit endEncoding];
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				const float step[]={0.5f*request.timeStepS,0.5f*request.timeStepS,request.timeStepS};
				for( unsigned int component=0u;component<3u;++component ) {
					const bool periodic=request.boundary[2u*component]==FireProductionProjectionPeriodic;
					const std::size_t componentBeginning=periodic?0u:
						(request.boundary[2u*component]==FireProductionProjectionWall?1u:0u);
					const MetalDualLineParameters wallParameters={
						static_cast<std::uint32_t>(request.shape.nx),static_cast<std::uint32_t>(request.shape.ny),
						static_cast<std::uint32_t>(request.shape.nz),component,component,0u,0u,
						static_cast<std::uint32_t>(componentBeginning),periodic?1u:0u,
						request.boundary[2u*component]==FireProductionProjectionWall?1u:0u,
						request.boundary[2u*component+1u]==FireProductionProjectionWall?1u:0u};
					id<MTLBuffer> wallParameter=[context.device newBufferWithBytes:&wallParameters
						length:sizeof(wallParameters) options:MTLResourceStorageModeShared];
					if( !add(wallParameter,MTLStorageModeShared) ) return false;
					id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:0];
					[encoder setBuffer:wallParameter offset:0 atIndex:1];
					Dispatch(encoder,context.prescribeDualComponentWalls,faceCounts[component]);
					[encoder endEncoding];
					for( unsigned int pass=0u;pass<5u;++pass ) {
						const unsigned int sweep=axes[pass];
						const FireProductionMetalDualAxisStaticState& axis=staticState.axis[component][sweep];
						const MetalDualLineParameters grid={
							static_cast<std::uint32_t>(request.shape.nx),static_cast<std::uint32_t>(request.shape.ny),
							static_cast<std::uint32_t>(request.shape.nz),component,sweep,axis.lineLength,
							axis.lineCount,static_cast<std::uint32_t>(componentBeginning),periodic?1u:0u,
							request.boundary[2u*component]==FireProductionProjectionWall?1u:0u,
							request.boundary[2u*component+1u]==FireProductionProjectionWall?1u:0u};
						const MetalParameters parameters={axis.lineLength,axis.lineCount,2u,
							axis.lowerBoundary,axis.upperBoundary,axis.ambientPerLine,
							request.shape.cellWidthM,step[sweep]};
						id<MTLBuffer> gridParameter=[context.device newBufferWithBytes:&grid length:sizeof(grid)
							options:MTLResourceStorageModeShared];
						id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters length:sizeof(parameters)
							options:MTLResourceStorageModeShared];
						if( !add(gridParameter,MTLStorageModeShared)||!add(parameter,MTLStorageModeShared) ) return false;
						const std::size_t valueCount=2u*axis.lineLength*axis.lineCount;
						const std::size_t cellCount=axis.lineLength*axis.lineCount;
						const std::size_t faceCount=axis.lineCount*(axis.lineLength+1u);
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:0];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:lineValues offset:0 atIndex:2];[encoder setBuffer:gridParameter offset:0 atIndex:3];
						Dispatch(encoder,context.gatherDualLineValues,valueCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:axis.faceVelocityMPerS offset:0 atIndex:1];
						[encoder setBuffer:axis.lowerAmbientValues offset:0 atIndex:2];[encoder setBuffer:axis.upperAmbientValues offset:0 atIndex:3];
						[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
						[encoder setBuffer:alpha offset:0 atIndex:6];[encoder setBuffer:parameter offset:0 atIndex:7];
						Dispatch(encoder,context.reconstruct,cellCount);[encoder endEncoding];
						const std::size_t padded=NextPowerOfTwo(axis.lineLength);
						encoder=[command computeCommandEncoder];
						if( !encoder||padded>static_cast<std::size_t>([context.scan maxTotalThreadsPerThreadgroup]) ) return false;
						[encoder setComputePipelineState:context.scan];[encoder setBuffer:lineValues offset:0 atIndex:0];
						[encoder setBuffer:prefix offset:0 atIndex:1];[encoder setBuffer:parameter offset:0 atIndex:2];
						[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
						[encoder dispatchThreadgroups:MTLSizeMake(2u*axis.lineCount,1,1)
							threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:axis.faceVelocityMPerS offset:0 atIndex:1];
						[encoder setBuffer:axis.lowerAmbientValues offset:0 atIndex:2];[encoder setBuffer:axis.upperAmbientValues offset:0 atIndex:3];
						[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
						[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
						[encoder setBuffer:parameter offset:0 atIndex:8];
						const bool passPeriodic=axis.lowerBoundary==0u&&axis.upperBoundary==0u;
						Dispatch(encoder,context.flux,passPeriodic?2u*axis.lineCount*axis.lineLength:2u*faceCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
						[encoder setBuffer:lineUpdated offset:0 atIndex:2];[encoder setBuffer:parameter offset:0 atIndex:3];
						Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineUpdated offset:0 atIndex:0];
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:2];
						[encoder setBuffer:gridParameter offset:0 atIndex:3];
						Dispatch(encoder,context.scatterDualLineValues,valueCount);[encoder endEncoding];
					}
					if( periodic ) {
						const MetalPeriodicDualParameters seam={static_cast<std::uint32_t>(request.shape.nx),
							static_cast<std::uint32_t>(request.shape.ny),static_cast<std::uint32_t>(request.shape.nz),
							component,component};
						id<MTLBuffer> parameter=[context.device newBufferWithBytes:&seam length:sizeof(seam)
							options:MTLResourceStorageModeShared];if( !add(parameter,MTLStorageModeShared) ) return false;
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:0];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:parameter offset:0 atIndex:2];
						const std::size_t seams=component==0u?request.shape.ny*request.shape.nz:
							(component==1u?request.shape.nx*request.shape.nz:request.shape.nx*request.shape.ny);
						Dispatch(encoder,context.publishPeriodicDualSeam,seams);[encoder endEncoding];
					}
				}
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) ) return false;
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) return false;
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningReads;
				if( commits!=1u||reads!=0u ) return false;
				FireProductionMetalDualMomentumResidentResult computed;
				computed.packedAuxiliaryFaceDensity=density;computed.packedMomentum=momentum;
				computed.faceByteOffset=canonicalOffset;computed.executedSubmapCount=15u;
				computed.commandCommitCount=1u;computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=actual;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceStartTimeS=[command GPUStartTime];
				computed.deviceEndTimeS=[command GPUEndTime];
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalDualMomentumResidentResult();
			if( structuredError ) try { *structuredError=
				"production resident dual allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionDualMomentumMetalResidentComparator(
		const FireProductionDualMomentumRequest& request,
		FireProductionDualMomentumResult& result,
		std::string* structuredError )
	{
		result=FireProductionDualMomentumResult();
		try {
			FireProductionMetalDualMomentumStaticState staticState;
			if( !PrepareFireProductionDualMomentumMetalStaticState(request,staticState,
				structuredError) ) return false;
			MetalRemapContext& context=Context();
			@autoreleasepool {
				std::array<std::size_t,3> offsets,counts;std::size_t allFaces=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) { offsets[axis]=allFaces*sizeof(float);
					counts[axis]=FireProductionProjectionFaceCount(request.shape,axis);allFaces+=counts[axis]; }
				std::vector<float> density(allFaces),momentum(allFaces);
				for( unsigned int axis=0u;axis<3u;++axis ) {
					std::copy(request.beginningFaceDensity[axis].begin(),request.beginningFaceDensity[axis].end(),
						density.begin()+offsets[axis]/sizeof(float));
					std::copy(request.beginningMomentum[axis].begin(),request.beginningMomentum[axis].end(),
						momentum.begin()+offsets[axis]/sizeof(float));
				}
				id<MTLBuffer> densityStage=[context.device newBufferWithBytes:density.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> momentumStage=[context.device newBufferWithBytes:momentum.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> densityPrivate=[context.device newBufferWithLength:allFaces*sizeof(float)
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> momentumPrivate=[context.device newBufferWithLength:allFaces*sizeof(float)
					options:MTLResourceStorageModePrivate];
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !densityStage||!momentumStage||!densityPrivate||!momentumPrivate||!blit ) return false;
				[blit copyFromBuffer:densityStage sourceOffset:0 toBuffer:densityPrivate destinationOffset:0
					size:allFaces*sizeof(float)];
				[blit copyFromBuffer:momentumStage sourceOffset:0 toBuffer:momentumPrivate destinationOffset:0
					size:allFaces*sizeof(float)];[blit endEncoding];CommitTrackedMetalCommand(upload);
				[upload waitUntilCompleted];if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalDualMomentumResidentInput input;input.packedFaceDensity=densityPrivate;
				input.packedMomentum=momentumPrivate;input.faceByteOffset=offsets;
				FireProductionMetalDualMomentumResidentResult resident;
				if( !RemapFireProductionDualMomentumMetalResident(request,staticState,input,resident,
					structuredError) ) return false;
				id<MTLBuffer> output=[context.device newBufferWithLength:2u*allFaces*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> stage=TrackedMetalCommandBuffer(context.queue);
				blit=stage?[stage blitCommandEncoder]:nil;if( !output||!blit ) return false;
				[blit copyFromBuffer:resident.packedAuxiliaryFaceDensity sourceOffset:0 toBuffer:output
					destinationOffset:0 size:allFaces*sizeof(float)];
				[blit copyFromBuffer:resident.packedMomentum sourceOffset:0 toBuffer:output
					destinationOffset:allFaces*sizeof(float) size:allFaces*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(stage);[stage waitUntilCompleted];
				if( [stage status]!=MTLCommandBufferStatusCompleted ) return false;
				const float* values=static_cast<const float*>(ReadTrackedMetalBuffer(output));
				FireProductionDualMomentumResult computed;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t beginning=offsets[axis]/sizeof(float);
					computed.auxiliaryFaceDensity[axis].assign(values+beginning,values+beginning+counts[axis]);
					computed.momentum[axis].assign(values+allFaces+beginning,
						values+allFaces+beginning+counts[axis]);
				}
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridTransferCount=resident.interstageFullGridTransferCount;
				computed.actualMetalAllocationBytes=resident.actualMetalAllocationBytes;
				computed.deviceElapsedMS=resident.deviceElapsedMS;result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionDualMomentumResult();
			if( structuredError ) try { *structuredError=
				"production resident dual comparator allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool AttemptFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* structuredError )
	{
		result=FireProductionResidentStepResult();
		bool plateauRefused=false;
		try {
			auto markPhase=[&](const char* phase){if(structuredError)*structuredError=phase;};
			const char* manifoldProbeActivation=std::getenv(
				"RISE_FIRE_MANIFOLD_TIMESTEP_PROBE");
			if( manifoldProbeActivation&&std::strcmp(manifoldProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production manifold timestep probe activation is invalid";
				return false;
			}
			const char* manifoldStageBudgetActivation=std::getenv(
				"RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE");
			if( manifoldStageBudgetActivation&&
				std::strcmp(manifoldStageBudgetActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production manifold stage-budget probe activation is invalid";
				return false;
			}
			const char* timestepVelocityAuditActivation=std::getenv(
				"RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
			if( timestepVelocityAuditActivation&&
				std::strcmp(timestepVelocityAuditActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production timestep velocity audit activation is invalid";
				return false;
			}
			const bool timestepVelocityAuditEnabled=timestepVelocityAuditActivation!=0;
			const char* goldenLongShadowActivation=std::getenv(
				"RISE_FIRE_GOLDEN_LONG_SHADOW");
			if( goldenLongShadowActivation&&
				std::strcmp(goldenLongShadowActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production golden long-shadow activation is invalid";
				return false;
			}
			const char* anomalyClosureTestActivation=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST");
			if( anomalyClosureTestActivation&&(!goldenLongShadowActivation||
				(std::strcmp(anomalyClosureTestActivation,"disabled")!=0&&
				 std::strcmp(anomalyClosureTestActivation,"limited")!=0)) ) {
				if( structuredError ) *structuredError=
					"production advective anomaly closure test activation is invalid";
				return false;
			}
			const bool anomalyClosureTestDisabled=anomalyClosureTestActivation&&
				std::strcmp(anomalyClosureTestActivation,"disabled")==0;
			const char* anomalyConvergenceProbeActivation=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE");
			if( anomalyConvergenceProbeActivation&&
				std::strcmp(anomalyConvergenceProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production advective anomaly convergence probe activation is invalid";
				return false;
			}
			const char* anomalyConvergencePassValue=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES");
			unsigned long parsedAnomalyConvergencePasses=0u;
			if( anomalyConvergencePassValue ) {
				char* end=0;errno=0;
				parsedAnomalyConvergencePasses=std::strtoul(
					anomalyConvergencePassValue,&end,10);
				if( !anomalyConvergenceProbeActivation||errno!=0||!end||*end!='\0'||
					parsedAnomalyConvergencePasses<1u||parsedAnomalyConvergencePasses>8u||
					std::to_string(parsedAnomalyConvergencePasses)!=
						anomalyConvergencePassValue ) {
					if( structuredError ) *structuredError=
						"production advective anomaly convergence pass count is invalid";
					return false;
				}
			} else if( anomalyConvergenceProbeActivation ) {
				if( structuredError ) *structuredError=
					"production advective anomaly convergence pass count is missing";
				return false;
			}
			const std::uint32_t anomalyClosurePassLimit=anomalyConvergenceProbeActivation?
				static_cast<std::uint32_t>(parsedAnomalyConvergencePasses):2u;
			const char* hostResidualProbeActivation=std::getenv(
				"RISE_FIRE_HOST_RESIDUAL_PROBE");
			if( hostResidualProbeActivation&&(!goldenLongShadowActivation||
				!anomalyClosureTestActivation||
				std::strcmp(anomalyClosureTestActivation,"limited")!=0||
				std::strcmp(hostResidualProbeActivation,"1")!=0) ) {
				if( structuredError ) *structuredError=
					"production host-residual probe activation is invalid";
				return false;
			}
			const bool hostResidualProbeEnabled=hostResidualProbeActivation!=0;
			const char* timestepVelocityPackMode=std::getenv(
				"RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE");
			if( timestepVelocityPackMode&&(!(timestepVelocityAuditEnabled||hostResidualProbeEnabled)||
				(std::strcmp(timestepVelocityPackMode,"serial")!=0&&
				 std::strcmp(timestepVelocityPackMode,"parallel")!=0)) ) {
				if( structuredError ) *structuredError=
					"production timestep velocity pack mode is invalid";
				return false;
			}
			const bool timestepVelocityAuditSerial=timestepVelocityPackMode&&
				std::strcmp(timestepVelocityPackMode,"serial")==0;
			const auto timestepVelocityAuditStart=std::chrono::steady_clock::now();
			auto timestepVelocityAuditMS=[&](){return std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-timestepVelocityAuditStart).count();};
				double timestepVelocityAuditValidatedMS=0.0,timestepVelocityAuditDualStaticMS=0.0,
					timestepVelocityAuditUploadMS=0.0,timestepVelocityAuditForceMS=0.0,
					timestepVelocityAuditCellMS=0.0,timestepVelocityAuditDualMS=0.0,
					timestepVelocityAuditSourceMS=0.0,timestepVelocityAuditPhysicalMS=0.0,
					timestepVelocityAuditRestorationMS=0.0,timestepVelocityAuditTerminalMS=0.0,
					timestepVelocityAuditTerminalValidationMS=0.0,
					timestepVelocityAuditResultCopyMS=0.0,
					timestepVelocityAuditMetadataMS=0.0,
					timestepVelocityAuditAuthorityMS=0.0;
			const char* plateauEvidenceActivation=std::getenv(
				"RISE_FIRE_RESTORATION_PLATEAU_PROBE");
			if( plateauEvidenceActivation&&std::strcmp(plateauEvidenceActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production restoration plateau probe activation is invalid";
				return false;
			}
			const bool plateauEvidenceEnabled=plateauEvidenceActivation!=0;
			const char* physicalValidationProbeActivation=std::getenv(
				"RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE");
			if( physicalValidationProbeActivation&&
				std::strcmp(physicalValidationProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production physical projection validation probe activation is invalid";
				return false;
			}
			const bool physicalValidationProbeEnabled=physicalValidationProbeActivation!=0;
			unsigned int restorationProbeCycles=0u;bool restorationProbeEnabled=false;
			if( !ValidateFireProductionRestorationCycleProbe(restorationProbeCycles,
				restorationProbeEnabled,structuredError) ) return false;
			const FireProductionProjectionShape& shape=request.force.shape;
			auto sameShape=[](const FireProductionProjectionShape& a,
				const FireProductionProjectionShape& b) {
				return a.nx==b.nx&&a.ny==b.ny&&a.nz==b.nz&&a.cellWidthM==b.cellWidthM;
			};
			if( !sameShape(shape,request.cellTransport.shape)||
				!sameShape(shape,request.dualTransport.shape)||
				request.force.timeStepS!=request.cellTransport.timeStepS||
				request.force.timeStepS!=request.dualTransport.timeStepS||
				request.force.boundary!=request.cellTransport.boundary||
				request.force.boundary!=request.dualTransport.boundary||
				request.cellTransport.componentCount!=9u ) {
				if( structuredError ) *structuredError=
					"production resident step ownership metadata does not match";
				return false;
			}
			std::uint64_t certified=0u;
			if( !FireProductionResidentStepWorkingSetBytes(shape,request.force.boundary,certified)||
				certified>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident step working set exceeds two GiB";
				return false;
			}
			const std::size_t cells=shape.CellCount();
			const bool measureManifold=request.enforceManifoldPlateau&&!plateauEvidenceEnabled;
			const bool closeAdvectiveAnomaly=measureManifold&&!anomalyClosureTestDisabled&&
				!manifoldProbeActivation&&!manifoldStageBudgetActivation&&
				!timestepVelocityAuditActivation;
			if( measureManifold&&request.beginningManifoldDeviationPerCell.size()!=cells ) {
				if( structuredError ) *structuredError=
					"production resident step lacks beginning manifold metadata";
				return false;
			}
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			std::vector<float> representedBeginningDeviation;
			if( measureManifold ) {
				representedBeginningDeviation.resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell)
					representedBeginningDeviation[cell]=static_cast<float>(
						request.beginningManifoldDeviationPerCell[cell]);
			}
			std::array<double,7> lowerEnthalpy,upperEnthalpy;
			MetalManifoldParameters manifoldParameters={static_cast<std::uint32_t>(cells),
				0u,0u,0u,0.0f,0.0f,0.0f,0.0f};
			if( measureManifold&&!PackMetalMethaneThermochemistry(packedThermochemistry,
				manifoldParameters,lowerEnthalpy,upperEnthalpy,structuredError) ) return false;
			std::array<std::size_t,3> faceCounts,faceOffsets;std::size_t allFaces=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				faceOffsets[axis]=allFaces*sizeof(float);
				faceCounts[axis]=FireProductionProjectionFaceCount(shape,axis);
				allFaces+=faceCounts[axis];
			}
			auto positiveZero=[](float value) {
				std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits==0u;
			};
			std::array<bool,10> ownerValidationSucceeded;
			ownerValidationSucceeded.fill(false);
			std::array<std::string,10> ownerValidationError;
			auto validateOwnerPayload=[&](const unsigned int task) {
				bool valid=true;std::string& taskError=ownerValidationError[task];
				if( task==0u ) valid=ValidateFireProductionFrozenForceRequest(request.force,&taskError);
				else if( task==1u ) {
					valid=request.force.cellGasDensityKGPerM3.size()==cells&&
						request.cellTransport.conservativeValues.size()==9u*cells&&
						request.cellSourceIncrement.size()==9u*cells&&
						request.divergenceTargetPerS.size()==cells&&
						request.restorationDivergenceTargetPerS.size()==cells;
					if( valid ) valid=ValidateFireProductionCellPalindromeRequest(
						request.cellTransport,&taskError);
					else taskError="production resident step payload shape is invalid";
				} else if( task==2u ) {
					if( request.enforceManifoldPlateau&&!plateauEvidenceEnabled )
						for( const double value:request.beginningManifoldDeviationPerCell )
							if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step beginning manifold metadata is nonfinite";
				} else if( task==3u ) {
					for( const float value:request.divergenceTargetPerS )
						if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step physical divergence target is nonfinite";
				} else if( task==4u ) {
					for( const float value:request.restorationDivergenceTargetPerS )
						if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step restoration divergence target is nonfinite";
				} else if( task==5u ) {
					if( request.cellTransport.conservativeValues.size()==9u*cells&&
						request.force.cellGasDensityKGPerM3.size()==cells )
						for( std::size_t cell=0u;cell<cells;++cell ) {
							float gas=request.cellTransport.conservativeValues[cells+cell];
							for( std::size_t component=2u;component<=6u;++component )
								gas+=request.cellTransport.conservativeValues[component*cells+cell];
							if( gas!=request.force.cellGasDensityKGPerM3[cell] ) {valid=false;break;}
						}
					else valid=false;
					if( !valid ) taskError=
						"production resident step packed gas density does not match force input";
				} else if( task==6u ) {
					for( const float value:request.cellSourceIncrement )
						if( !positiveZero(value) ) {valid=false;break;}
					if( !valid ) taskError="production resident step cell source is not positive zero";
				} else {
					const unsigned int axis=task-7u;
					valid=request.momentumSourceIncrement[axis].size()==faceCounts[axis]&&
						request.dualTransport.beginningFaceDensity[axis]==
							request.force.faceDensityKGPerM3[axis]&&
						request.dualTransport.beginningMomentum[axis]==
							request.force.beginningMomentumKGPerM2S[axis]&&
						request.dualTransport.frozenVelocityMPerS[axis]==
							request.cellTransport.frozenVelocityMPerS[axis];
					if( !valid ) taskError=
						"production resident step dual ownership does not match force/cell input";
					else for( const float value:request.momentumSourceIncrement[axis] )
						if( !positiveZero(value) ) {valid=false;taskError=
							"production resident step momentum source is not positive zero";break;}
				}
				ownerValidationSucceeded[task]=valid;
			};
			const bool serialOwnerValidation=timestepVelocityAuditSerial||
				GlobalOptions().ReadBool("force_all_threads_low_priority",false);
			if( serialOwnerValidation )
				for( unsigned int task=0u;task<ownerValidationSucceeded.size();++task )
					validateOwnerPayload(task);
			else Implementation::GlobalThreadPool().ParallelFor(
				ownerValidationSucceeded.size(),validateOwnerPayload);
			for( unsigned int task=0u;task<ownerValidationSucceeded.size();++task )
				if( !ownerValidationSucceeded[task] ) {
					if( structuredError ) *structuredError=ownerValidationError[task];
					return false;
				}
			timestepVelocityAuditValidatedMS=timestepVelocityAuditMS();
			FireProductionMetalDualMomentumStaticState dualStatic;
			timestepVelocityAuditDualStaticMS=timestepVelocityAuditValidatedMS;
			markPhase("production resident step failed while acquiring Metal context");
			MetalRemapContext& context=Context();
			if( !context.Valid() ) return false;
			@autoreleasepool {
				markPhase("production resident step failed while allocating resident buffers");
				const std::size_t cellValueBytes=9u*cells*sizeof(float);
				const std::size_t packedFaceBytes=allFaces*sizeof(float);
				std::vector<float> packedFaceSource(allFaces,0.0f);
				for( unsigned int axis=0u;axis<3u;++axis ) std::copy(
					request.momentumSourceIncrement[axis].begin(),request.momentumSourceIncrement[axis].end(),
					packedFaceSource.begin()+faceOffsets[axis]/sizeof(float));
				auto stage=[&](const float* values,std::size_t bytes) {
					return [context.device newBufferWithBytes:values length:bytes
						options:MTLResourceStorageModeShared];
				};
				auto privateBuffer=[&](std::size_t bytes) { return [context.device
					newBufferWithLength:bytes options:MTLResourceStorageModePrivate]; };
				id<MTLBuffer> cellStage=stage(request.cellTransport.conservativeValues.data(),cellValueBytes);
				id<MTLBuffer> cellPrivate=privateBuffer(cellValueBytes);
				std::array<id<MTLBuffer>,3> velocityStage,velocityPrivate;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					velocityStage[axis]=stage(request.cellTransport.frozenVelocityMPerS[axis].data(),
						faceCounts[axis]*sizeof(float));
					velocityPrivate[axis]=privateBuffer(faceCounts[axis]*sizeof(float));
				}
				id<MTLBuffer> ambientStage=stage(request.cellTransport.ambientValues.data(),9u*sizeof(float));
				id<MTLBuffer> ambientPrivate=privateBuffer(9u*sizeof(float));
				id<MTLBuffer> cellSourceStage=stage(request.cellSourceIncrement.data(),cellValueBytes);
				id<MTLBuffer> cellSourcePrivate=privateBuffer(cellValueBytes);
				id<MTLBuffer> faceSourceStage=stage(packedFaceSource.data(),packedFaceBytes);
				id<MTLBuffer> faceSourcePrivate=privateBuffer(packedFaceBytes);
				id<MTLBuffer> targetStage=stage(request.divergenceTargetPerS.data(),cells*sizeof(float));
				id<MTLBuffer> targetPrivate=privateBuffer(cells*sizeof(float));
				id<MTLBuffer> restorationTargetStage=stage(
					request.restorationDivergenceTargetPerS.data(),cells*sizeof(float));
				id<MTLBuffer> restorationTargetPrivate=privateBuffer(cells*sizeof(float));
				id<MTLBuffer> manifoldThermochemistry=measureManifold?
					stage(packedThermochemistry.data(),packedThermochemistry.size()*sizeof(float)):nil;
				id<MTLBuffer> manifoldBeginningDeviation=measureManifold?
					stage(representedBeginningDeviation.data(),cells*sizeof(float)):nil;
				id<MTLBuffer> manifoldParametersBuffer=measureManifold?
					[context.device newBufferWithBytes:&manifoldParameters
						length:sizeof(manifoldParameters) options:MTLResourceStorageModeShared]:nil;
				id<MTLBuffer> manifoldMap=measureManifold?privateBuffer(2u*cells*sizeof(float)):nil;
				id<MTLBuffer> manifoldPredictorReduction=closeAdvectiveAnomaly?
					[context.device newBufferWithLength:3u*sizeof(std::uint32_t)
						options:MTLResourceStorageModeShared]:nil;
				id<MTLBuffer> manifoldQuantileScratch=measureManifold?
					[context.device newBufferWithLength:
						MetalManifoldQuantileScratchWords*sizeof(std::uint32_t)
						options:MTLResourceStorageModeShared]:nil;
				if( !cellStage||!cellPrivate||!ambientStage||!ambientPrivate||!cellSourceStage||
					!cellSourcePrivate||!faceSourceStage||!faceSourcePrivate||!targetStage||!targetPrivate||
					!restorationTargetStage||!restorationTargetPrivate||
					(measureManifold&&(!manifoldThermochemistry||!manifoldBeginningDeviation||
						!manifoldParametersBuffer||!manifoldMap))||
					(closeAdvectiveAnomaly&&!manifoldPredictorReduction)||
					(measureManifold&&!manifoldQuantileScratch) )
					return false;
				for( unsigned int axis=0u;axis<3u;++axis )
					if( !velocityStage[axis]||!velocityPrivate[axis] ) return false;
				std::uint64_t ownerActualMetalBytes=0u;
				auto recordOwner=[&](id<MTLBuffer> buffer) {
					if( !buffer ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( ownerActualMetalBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					ownerActualMetalBytes+=bytes;return ownerActualMetalBytes<=certified;
				};
				if( !recordOwner(cellStage)||!recordOwner(cellPrivate)||!recordOwner(ambientStage)||
					!recordOwner(ambientPrivate)||!recordOwner(cellSourceStage)||
					!recordOwner(cellSourcePrivate)||!recordOwner(faceSourceStage)||
					!recordOwner(faceSourcePrivate)||!recordOwner(targetStage)||!recordOwner(targetPrivate)||
					!recordOwner(restorationTargetStage)||!recordOwner(restorationTargetPrivate) )
					return false;
				if( measureManifold&&(!recordOwner(manifoldThermochemistry)||
					!recordOwner(manifoldBeginningDeviation)||
					!recordOwner(manifoldParametersBuffer)||!recordOwner(manifoldMap)) ) return false;
				if( closeAdvectiveAnomaly&&!recordOwner(manifoldPredictorReduction) ) return false;
				if( measureManifold&&!recordOwner(manifoldQuantileScratch) ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!recordOwner(velocityStage[axis])||!recordOwner(velocityPrivate[axis]) ) return false;
				markPhase("production resident step failed while uploading resident inputs");
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;if( !blit ) return false;
				[blit copyFromBuffer:cellStage sourceOffset:0 toBuffer:cellPrivate destinationOffset:0 size:cellValueBytes];
				for( unsigned int axis=0u;axis<3u;++axis ) [blit copyFromBuffer:velocityStage[axis]
					sourceOffset:0 toBuffer:velocityPrivate[axis] destinationOffset:0
					size:faceCounts[axis]*sizeof(float)];
				[blit copyFromBuffer:ambientStage sourceOffset:0 toBuffer:ambientPrivate destinationOffset:0 size:9u*sizeof(float)];
				[blit copyFromBuffer:cellSourceStage sourceOffset:0 toBuffer:cellSourcePrivate destinationOffset:0 size:cellValueBytes];
				[blit copyFromBuffer:faceSourceStage sourceOffset:0 toBuffer:faceSourcePrivate destinationOffset:0 size:packedFaceBytes];
				[blit copyFromBuffer:targetStage sourceOffset:0 toBuffer:targetPrivate destinationOffset:0 size:cells*sizeof(float)];
				[blit copyFromBuffer:restorationTargetStage sourceOffset:0 toBuffer:
					restorationTargetPrivate destinationOffset:0 size:cells*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				const double timestepVelocityAuditOwnerUploadDeviceMS=
					([upload GPUEndTime]-[upload GPUStartTime])*1000.0;
				timestepVelocityAuditUploadMS=timestepVelocityAuditMS();
				FireProductionMetalCellPalindromeResidentInput cellInput;cellInput.conservativeValues=cellPrivate;
				cellInput.frozenVelocityMPerS=velocityPrivate;cellInput.ambientValues=ambientPrivate;
				FireProductionMetalFrozenForceResidentState force;
				FireProductionMetalCellPalindromeResidentResult cell;
				std::array<bool,3> preparationSucceeded={{false,false,false}};
				std::array<std::string,3> preparationError;
				std::array<double,3> preparationWallMS={{0.0,0.0,0.0}};
				markPhase("production resident step failed while preparing independent resident stages");
				auto prepareIndependent=[&](const unsigned int task) {
					const auto start=std::chrono::steady_clock::now();
					if(task==0u)preparationSucceeded[task]=
						PrepareFireProductionDualMomentumMetalStaticState(request.dualTransport,
							dualStatic,&preparationError[task]);
					else if(task==1u)preparationSucceeded[task]=
						AdvanceFireProductionFrozenForceMetalResidentState(request.force,force,
							&preparationError[task]);
					else preparationSucceeded[task]=RemapFireProductionCellPalindromeMetalResident(
						request.cellTransport,cellInput,cell,&preparationError[task]);
					preparationWallMS[task]=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-start).count();
				};
				const bool serialIndependentPreparation=timestepVelocityAuditSerial||
					GlobalOptions().ReadBool("force_all_threads_low_priority",false);
				if(serialIndependentPreparation)
					for(unsigned int task=0u;task<preparationSucceeded.size();++task)
						prepareIndependent(task);
				else Implementation::GlobalThreadPool().ParallelFor(
					preparationSucceeded.size(),prepareIndependent);
				for(unsigned int task=0u;task<preparationSucceeded.size();++task)
					if(!preparationSucceeded[task]){
						if(structuredError)*structuredError=preparationError[task];return false;}
				timestepVelocityAuditForceMS=timestepVelocityAuditMS();
				const double predictorCellDeviceMS=cell.deviceElapsedMS;
				const double predictorCellDeviceStartTimeS=cell.deviceStartTimeS;
				const double predictorCellDeviceEndTimeS=cell.deviceEndTimeS;
				const std::uint64_t predictorCellActualMetalBytes=cell.actualMetalAllocationBytes;
				timestepVelocityAuditCellMS=timestepVelocityAuditForceMS;
				FireProductionMetalDualMomentumResidentInput dualInput;
				dualInput.packedFaceDensity=force.packedFaceDensityKGPerM3;
				dualInput.packedMomentum=force.packedMomentumKGPerM2S;
				dualInput.faceByteOffset=force.faceByteOffset;
				markPhase("production resident step failed during dual-momentum remap");
				FireProductionMetalDualMomentumResidentResult dual;
				if( !RemapFireProductionDualMomentumMetalResident(request.dualTransport,dualStatic,
					dualInput,dual,structuredError) ) return false;
				timestepVelocityAuditDualMS=timestepVelocityAuditMS();
				id<MTLBuffer> projectedDensity=privateBuffer(cells*sizeof(float));
				const MetalGridParameters sourceGrid={static_cast<std::uint32_t>(shape.nx),
					static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),0u,9u};
				const std::uint32_t packedFaceCount=static_cast<std::uint32_t>(allFaces);
				id<MTLBuffer> sourceGridParameter=[context.device newBufferWithBytes:&sourceGrid
					length:sizeof(sourceGrid) options:MTLResourceStorageModeShared];
				id<MTLBuffer> sourceFaceParameter=[context.device newBufferWithBytes:&packedFaceCount
					length:sizeof(packedFaceCount) options:MTLResourceStorageModeShared];
				if( !recordOwner(projectedDensity)||!recordOwner(sourceGridParameter)||
					!recordOwner(sourceFaceParameter) ) return false;
				markPhase("production resident step failed while applying resident sources");
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				id<MTLCommandBuffer> sourceCommand=TrackedMetalCommandBuffer(context.queue);
				id<MTLComputeCommandEncoder> encoder=sourceCommand?[sourceCommand computeCommandEncoder]:nil;
				if( !encoder ) return false;
				[encoder setBuffer:cell.conservativeValues offset:0 atIndex:0];
				[encoder setBuffer:cellSourcePrivate offset:0 atIndex:1];
				[encoder setBuffer:sourceGridParameter offset:0 atIndex:2];
				Dispatch(encoder,context.addCellSources,9u*cells);[encoder endEncoding];
				encoder=[sourceCommand computeCommandEncoder];if( !encoder ) return false;
				[encoder setBuffer:cell.conservativeValues offset:0 atIndex:0];
				[encoder setBuffer:projectedDensity offset:0 atIndex:1];
				[encoder setBuffer:sourceGridParameter offset:0 atIndex:2];
				Dispatch(encoder,context.extractGasDensity,cells);[encoder endEncoding];
				encoder=[sourceCommand computeCommandEncoder];if( !encoder ) return false;
				[encoder setBuffer:dual.packedMomentum offset:0 atIndex:0];
				[encoder setBuffer:faceSourcePrivate offset:0 atIndex:1];
				[encoder setBuffer:sourceFaceParameter offset:0 atIndex:2];
				Dispatch(encoder,context.addFaceSources,allFaces);[encoder endEncoding];
				CommitTrackedMetalCommand(sourceCommand);[sourceCommand waitUntilCompleted];
				std::uint64_t sourceCommits=MetalCommandCommitCount-beginningCommits;
				if( [sourceCommand status]!=MTLCommandBufferStatusCompleted||sourceCommits!=1u||
					MetalHostBufferReadCount-beginningReads!=0u ) return false;
				timestepVelocityAuditSourceMS=timestepVelocityAuditMS();
				float maximumPredictedAdvectiveAnomalyFloat=0.0f;
				std::uint32_t anomalyPredictorMeasurementCount=0u;
				double anomalyPredictorDeviceMS=0.0,anomalyCorrectorSourceDeviceMS=0.0,
					anomalyPredictorDeviceStartTimeS=0.0,anomalyPredictorDeviceEndTimeS=0.0,
					anomalyCorrectorSourceDeviceStartTimeS=0.0,
					anomalyCorrectorSourceDeviceEndTimeS=0.0;
				auto predictAdvectiveAnomaly=[&](){
					markPhase("production resident step failed during advective-anomaly prediction");
					std::memset([manifoldPredictorReduction contents],0,3u*sizeof(std::uint32_t));
					id<MTLCommandBuffer> predictorCommand=TrackedMetalCommandBuffer(context.queue);
					id<MTLBlitCommandEncoder> predictorBlit=
						predictorCommand?[predictorCommand blitCommandEncoder]:nil;
					if( !predictorBlit ) return false;
					[predictorBlit copyFromBuffer:restorationTargetStage sourceOffset:0
						toBuffer:restorationTargetPrivate destinationOffset:0 size:cells*sizeof(float)];
					[predictorBlit endEncoding];
					id<MTLComputeCommandEncoder> predictorEncoder=
						predictorCommand?[predictorCommand computeCommandEncoder]:nil;
					if( !predictorEncoder ) return false;
					[predictorEncoder setBuffer:manifoldBeginningDeviation offset:0 atIndex:0];
					[predictorEncoder setBuffer:cell.conservativeValues offset:0 atIndex:1];
					[predictorEncoder setBuffer:manifoldThermochemistry offset:0 atIndex:2];
					[predictorEncoder setBuffer:manifoldMap offset:0 atIndex:3];
					[predictorEncoder setBuffer:manifoldPredictorReduction offset:0 atIndex:4];
					[predictorEncoder setBuffer:manifoldParametersBuffer offset:0 atIndex:5];
					[predictorEncoder setBuffer:manifoldQuantileScratch offset:0 atIndex:6];
					Dispatch(predictorEncoder,context.measureMethaneManifold,cells);
					[predictorEncoder endEncoding];
					predictorEncoder=[predictorCommand computeCommandEncoder];
					if( !predictorEncoder ) return false;
					const float inverseTimeStep=1.0f/request.force.timeStepS;
					[predictorEncoder setBuffer:manifoldMap offset:0 atIndex:0];
					[predictorEncoder setBuffer:restorationTargetPrivate offset:0 atIndex:1];
					[predictorEncoder setBytes:&inverseTimeStep length:sizeof(inverseTimeStep) atIndex:2];
					Dispatch(predictorEncoder,context.foldMethaneAdvectiveAnomalyTarget,cells);
					[predictorEncoder endEncoding];
					CommitTrackedMetalCommand(predictorCommand);[predictorCommand waitUntilCompleted];
					if( [predictorCommand status]!=MTLCommandBufferStatusCompleted ) return false;
					anomalyPredictorDeviceMS+=
						([predictorCommand GPUEndTime]-[predictorCommand GPUStartTime])*1000.0;
					const double predictorStart=[predictorCommand GPUStartTime];
					const double predictorEnd=[predictorCommand GPUEndTime];
					anomalyPredictorDeviceStartTimeS=anomalyPredictorDeviceStartTimeS>0.0?
						std::min(anomalyPredictorDeviceStartTimeS,predictorStart):predictorStart;
					anomalyPredictorDeviceEndTimeS=
						std::max(anomalyPredictorDeviceEndTimeS,predictorEnd);
					const std::uint32_t* predictorReduction=static_cast<const std::uint32_t*>(
						ReadTrackedMetalBuffer(manifoldPredictorReduction));
					++anomalyPredictorMeasurementCount;
					if( !predictorReduction||predictorReduction[2u]!=0u ) return false;
					std::memcpy(&maximumPredictedAdvectiveAnomalyFloat,predictorReduction,sizeof(float));
					return std::isfinite(maximumPredictedAdvectiveAnomalyFloat);
				};
				FireProductionProjectionRequest projectionRequest;
				projectionRequest.shape=shape;projectionRequest.timeStepS=request.force.timeStepS;
				projectionRequest.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
				projectionRequest.boundary=request.force.boundary;
				projectionRequest.gasDensityKGPerM3=request.force.cellGasDensityKGPerM3;
				projectionRequest.provisionalMomentumKGPerM2S=request.dualTransport.beginningMomentum;
				projectionRequest.divergenceTargetPerS=request.divergenceTargetPerS;
				projectionRequest.residentPhysicalOpenVCycleCount=
					request.physicalOpenProjectionVCycleCount;
				FireProductionMetalProjectionResidentInput projectionInput;
				projectionInput.gasDensityKGPerM3=projectedDensity;
				projectionInput.provisionalMomentumKGPerM2S.fill(dual.packedMomentum);
				projectionInput.provisionalMomentumByteOffset=dual.faceByteOffset;
				projectionInput.divergenceTargetPerS=targetPrivate;
				const char* restorationTest=std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST");
				const bool restorationRemoved=restorationTest&&
					std::strcmp(restorationTest,"removed")==0;
				const bool restorationFullTarget=restorationTest&&
					std::strcmp(restorationTest,"full-target")==0;
				FireProductionProjectionResult physicalProjection,projection;
				std::uint32_t executedAnomalyClosurePasses=1u;
				double anomalyCorrectorCellDeviceMS=0.0,
					anomalyCorrectorCellDeviceStartTimeS=0.0,
					anomalyCorrectorCellDeviceEndTimeS=0.0,
					anomalyRestorationDeviceMS=0.0,
					anomalyRestorationDeviceStartTimeS=0.0,
					anomalyRestorationDeviceEndTimeS=0.0;
				if( restorationRemoved ) {
					if( !ProjectFireProductionMetalResident(projectionRequest,projectionInput,
						projection,structuredError) ) return false;
					timestepVelocityAuditPhysicalMS=timestepVelocityAuditMS();
					timestepVelocityAuditRestorationMS=timestepVelocityAuditPhysicalMS;
				} else {
					FireProductionMetalProjectionResidentState physicalState;
					markPhase("production resident step failed during physical projection");
					if( !ProjectFireProductionMetalResidentState(projectionRequest,projectionInput,
						physicalState,physicalProjection,structuredError) ) return false;
					timestepVelocityAuditPhysicalMS=timestepVelocityAuditMS();
					FireProductionProjectionRequest restorationRequest=projectionRequest;
					restorationRequest.divergenceTargetPerS=
						request.restorationDivergenceTargetPerS;
					FireProductionMetalProjectionResidentInput restorationInput;
					restorationInput.gasDensityKGPerM3=projectedDensity;
					restorationInput.provisionalMomentumKGPerM2S=physicalState.momentumKGPerM2S;
					restorationInput.provisionalMomentumByteOffset=physicalState.momentumByteOffset;
					restorationInput.divergenceTargetPerS=restorationFullTarget?
						targetPrivate:restorationTargetPrivate;
					for(std::uint32_t pass=1u;closeAdvectiveAnomaly&&
						pass<anomalyClosurePassLimit;++pass){
						if(!predictAdvectiveAnomaly())return false;
						if(maximumPredictedAdvectiveAnomalyFloat==0.0f)break;
						FireProductionMetalProjectionResidentState restorationState;
						if( !ProjectFireProductionMetalRestorationResidentState(restorationRequest,
							restorationInput,restorationTargetPrivate,restorationState,projection,
							structuredError) ) return false;
						anomalyRestorationDeviceMS+=projection.deviceElapsedMS;
						anomalyRestorationDeviceStartTimeS=
							anomalyRestorationDeviceStartTimeS>0.0?
							std::min(anomalyRestorationDeviceStartTimeS,
								projection.deviceStartTimeS):projection.deviceStartTimeS;
						anomalyRestorationDeviceEndTimeS=
							std::max(anomalyRestorationDeviceEndTimeS,projection.deviceEndTimeS);
						FireProductionMetalCellPalindromeResidentInput correctorInput;
						correctorInput.conservativeValues=cellPrivate;
						correctorInput.frozenVelocityMPerS=restorationState.velocityMPerS;
						correctorInput.ambientValues=ambientPrivate;
						FireProductionMetalCellPalindromeResidentResult corrector;
						std::array<bool,2> correctorSucceeded={{false,false}};
						std::array<std::string,2> correctorError;
						auto finishCorrector=[&](const std::size_t task){
							if(task==0u){
								if(!RemapFireProductionCellPalindromeMetalResident(request.cellTransport,
									correctorInput,corrector,&correctorError[task]))return;
								anomalyCorrectorCellDeviceMS+=corrector.deviceElapsedMS;
								anomalyCorrectorCellDeviceStartTimeS=
									anomalyCorrectorCellDeviceStartTimeS>0.0?
									std::min(anomalyCorrectorCellDeviceStartTimeS,
										corrector.deviceStartTimeS):corrector.deviceStartTimeS;
								anomalyCorrectorCellDeviceEndTimeS=
									std::max(anomalyCorrectorCellDeviceEndTimeS,corrector.deviceEndTimeS);
								id<MTLCommandBuffer> correctorSource=TrackedMetalCommandBuffer(context.queue);
								id<MTLComputeCommandEncoder> correctorEncoder=
									correctorSource?[correctorSource computeCommandEncoder]:nil;
								if(!correctorEncoder){correctorError[task]=
									"production anomaly corrector source encoder failed";return;}
								[correctorEncoder setBuffer:corrector.conservativeValues offset:0 atIndex:0];
								[correctorEncoder setBuffer:cellSourcePrivate offset:0 atIndex:1];
								[correctorEncoder setBuffer:sourceGridParameter offset:0 atIndex:2];
								Dispatch(correctorEncoder,context.addCellSources,9u*cells);
								[correctorEncoder endEncoding];CommitTrackedMetalCommand(correctorSource);
								[correctorSource waitUntilCompleted];
								if([correctorSource status]!=MTLCommandBufferStatusCompleted){
									correctorError[task]="production anomaly corrector source failed";return;}
								anomalyCorrectorSourceDeviceMS+=([correctorSource GPUEndTime]-
									[correctorSource GPUStartTime])*1000.0;
								const double correctorSourceStart=[correctorSource GPUStartTime];
								const double correctorSourceEnd=[correctorSource GPUEndTime];
								anomalyCorrectorSourceDeviceStartTimeS=
									anomalyCorrectorSourceDeviceStartTimeS>0.0?
									std::min(anomalyCorrectorSourceDeviceStartTimeS,
										correctorSourceStart):correctorSourceStart;
								anomalyCorrectorSourceDeviceEndTimeS=
									std::max(anomalyCorrectorSourceDeviceEndTimeS,correctorSourceEnd);
								correctorSucceeded[task]=true;
							}else correctorSucceeded[task]=
								PublishFireProductionMetalRestorationResidentState(restorationRequest,
									restorationState,projection,&correctorError[task]);
						};
						if(serialIndependentPreparation){finishCorrector(0u);finishCorrector(1u);}
						else Implementation::GlobalThreadPool().ParallelFor(
							correctorSucceeded.size(),finishCorrector);
						for(std::size_t task=0u;task<correctorSucceeded.size();++task)
							if(!correctorSucceeded[task]){
								if(structuredError)*structuredError=correctorError[task];
								return false;
							}
						++sourceCommits;cell=std::move(corrector);++executedAnomalyClosurePasses;
					}
					if(executedAnomalyClosurePasses==1u&&
						!ProjectFireProductionMetalRestorationResident(restorationRequest,
						restorationInput,restorationTargetPrivate,projection,structuredError) ) return false;
					if(executedAnomalyClosurePasses==1u){
						anomalyRestorationDeviceMS=projection.deviceElapsedMS;
						anomalyRestorationDeviceStartTimeS=projection.deviceStartTimeS;
						anomalyRestorationDeviceEndTimeS=projection.deviceEndTimeS;
					}
					timestepVelocityAuditRestorationMS=timestepVelocityAuditMS();
				}
				markPhase("production resident step failed while publishing resident terminal state");
				const std::size_t manifoldReductionOffset=cellValueBytes+2u*packedFaceBytes;
				const std::size_t manifoldReductionBytes=3u*sizeof(std::uint32_t);
				id<MTLBuffer> terminal=[context.device newBufferWithLength:
					(manifoldReductionOffset+(measureManifold?manifoldReductionBytes:0u))
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> terminalCommand=TrackedMetalCommandBuffer(context.queue);
				if( !recordOwner(terminal)||!terminalCommand ) return false;
				if( measureManifold ) {
					std::memset(static_cast<unsigned char*>([terminal contents])+
						manifoldReductionOffset,0,manifoldReductionBytes);
					std::memset([manifoldQuantileScratch contents],0,
						MetalManifoldQuantileScratchWords*sizeof(std::uint32_t));
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldBeginningDeviation offset:0 atIndex:0];
					[encoder setBuffer:cell.conservativeValues offset:0 atIndex:1];
					[encoder setBuffer:manifoldThermochemistry offset:0 atIndex:2];
					[encoder setBuffer:manifoldMap offset:0 atIndex:3];
					[encoder setBuffer:terminal offset:manifoldReductionOffset atIndex:4];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:5];
					[encoder setBuffer:manifoldQuantileScratch offset:0 atIndex:6];
					Dispatch(encoder,context.measureMethaneManifold,cells);[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldQuantileScratch offset:0 atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.selectMethaneManifoldHighBins,1u);[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldMap offset:0 atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileBins*sizeof(std::uint32_t) atIndex:2];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:3];
					Dispatch(encoder,context.histogramMethaneManifoldLowBins,cells);
					[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileBins*sizeof(std::uint32_t) atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					Dispatch(encoder,context.selectMethaneManifoldLowBins,1u);[encoder endEncoding];
				}
				blit=[terminalCommand blitCommandEncoder];if( !blit ) return false;
				[blit copyFromBuffer:cell.conservativeValues sourceOffset:0 toBuffer:terminal
					destinationOffset:0 size:cellValueBytes];
				[blit copyFromBuffer:dual.packedAuxiliaryFaceDensity sourceOffset:0 toBuffer:terminal
					destinationOffset:cellValueBytes size:packedFaceBytes];
				[blit copyFromBuffer:dual.packedMomentum sourceOffset:0 toBuffer:terminal
					destinationOffset:cellValueBytes+packedFaceBytes size:packedFaceBytes];
				[blit endEncoding];CommitTrackedMetalCommand(terminalCommand);[terminalCommand waitUntilCompleted];
				if( [terminalCommand status]!=MTLCommandBufferStatusCompleted ) return false;
				timestepVelocityAuditTerminalMS=timestepVelocityAuditMS();
				float* values=static_cast<float*>(ReadTrackedMetalBuffer(terminal));
				const std::uint32_t* manifoldReduction=measureManifold?
					reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const unsigned char*>(values)+
						manifoldReductionOffset):0;
				const char* injected=std::getenv("RISE_FIRE_PRODUCTION_STEP_FAILURE");
				if( values&&injected&&std::strcmp(injected,"terminal-nonfinite")==0 )
					values[0]=std::numeric_limits<float>::quiet_NaN();
				bool terminalValid=values!=0;
				for( std::size_t cell=0u;terminalValid&&cell<cells;++cell ) {
					float gas=values[cells+cell];
					for( std::size_t component=0u;component<9u;++component )
						terminalValid=terminalValid&&std::isfinite(values[component*cells+cell]);
					for( std::size_t component=2u;component<=6u;++component )
						gas+=values[component*cells+cell];
					terminalValid=terminalValid&&gas>0.0f&&std::isfinite(gas);
				}
				for( std::size_t face=0u;terminalValid&&face<allFaces;++face )
					terminalValid=std::isfinite(values[9u*cells+face])&&
						values[9u*cells+face]>0.0f&&
						std::isfinite(values[9u*cells+allFaces+face]);
				if( !terminalValid ) {
					if( manifoldStageBudgetActivation&&values ) {
						for(std::size_t cellIndex=0u;cellIndex<cells;++cellIndex){
							float gas=values[cells+cellIndex];bool finite=true;
							for(std::size_t component=0u;component<9u;++component)
								finite=finite&&std::isfinite(values[component*cells+cellIndex]);
							for(std::size_t component=2u;component<=6u;++component)
								gas+=values[component*cells+cellIndex];
							if(!finite||!(gas>0.0f)||!std::isfinite(gas)){std::fprintf(stderr,
								"MANIFOLD_OWNER invalid_cell=%zu gas=%.9g energy=%.9g\n",
								cellIndex,gas,values[8u*cells+cellIndex]);break;}
						}
						for(std::size_t face=0u;face<allFaces;++face)if(
							!std::isfinite(values[9u*cells+face])||
							!(values[9u*cells+face]>0.0f)||
							!std::isfinite(values[9u*cells+allFaces+face])){
							std::fprintf(stderr,"MANIFOLD_OWNER invalid_face=%zu density=%.9g momentum=%.9g\n",
								face,values[9u*cells+face],values[9u*cells+allFaces+face]);break;}
					}
					return false;
				}
				timestepVelocityAuditTerminalValidationMS=timestepVelocityAuditMS();
				markPhase("production resident step failed while validating resident terminal state");
				const bool enforcePlateau=request.enforceManifoldPlateau&&
					!restorationRemoved&&!plateauEvidenceEnabled;
				float maximumManifoldGenerationFloat=0.0f,maximumTerminalDeviationFloat=0.0f;
				float terminalDeviationP50Float=0.0f,terminalDeviationP95Float=0.0f;
				if( measureManifold&&manifoldReduction ) {
					std::memcpy(&maximumManifoldGenerationFloat,manifoldReduction,sizeof(float));
					std::memcpy(&maximumTerminalDeviationFloat,manifoldReduction+1u,sizeof(float));
					const std::uint32_t* quantileControl=reinterpret_cast<const std::uint32_t*>(
						static_cast<const unsigned char*>([manifoldQuantileScratch contents])+
						MetalManifoldQuantileControlOffset);
					std::memcpy(&terminalDeviationP50Float,quantileControl+4u,sizeof(float));
					std::memcpy(&terminalDeviationP95Float,quantileControl+5u,sizeof(float));
				}
				const double maximumManifoldGeneration=maximumManifoldGenerationFloat;
				const double maximumTerminalDeviation=maximumTerminalDeviationFloat;
				const double terminalDeviationP50=terminalDeviationP50Float;
				const double terminalDeviationP95=terminalDeviationP95Float;
				FireProductionRestorationPlateauValidation plateauValidation;
				if( enforcePlateau&&(!manifoldReduction||manifoldReduction[2u]!=0u||
					!std::isfinite(maximumManifoldGeneration)||
					!std::isfinite(maximumTerminalDeviation)||
					!std::isfinite(terminalDeviationP50)||!std::isfinite(terminalDeviationP95)||
					terminalDeviationP50<0.0||terminalDeviationP95<terminalDeviationP50||
					maximumTerminalDeviation<terminalDeviationP95||
					!FireProductionRestorationPlateauWithinBand(maximumManifoldGeneration,
						projection.maximumPreProjectionResidualPerS,
						projection.maximumPostProjectionResidualPerS,plateauValidation)) ) return false;
				constexpr double lowMachValidityCeiling=0x1p-5;
				constexpr double lowMachPlateauAllowance=(1.0-0x1p-2)*
					lowMachValidityCeiling;
				const bool plateauPassed=!enforcePlateau||
					(plateauValidation.mechanismPassed&&
					maximumTerminalDeviation<=lowMachPlateauAllowance);
				FireProductionResidentStepResult computed;
				computed.conservativeValues.assign(values,values+9u*cells);
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t beginning=faceOffsets[axis]/sizeof(float);
					computed.transportedDual.auxiliaryFaceDensity[axis].assign(
						values+9u*cells+beginning,values+9u*cells+beginning+faceCounts[axis]);
					computed.transportedDual.momentum[axis].assign(values+9u*cells+allFaces+beginning,
						values+9u*cells+allFaces+beginning+faceCounts[axis]);
				}
				timestepVelocityAuditResultCopyMS=timestepVelocityAuditMS();
				computed.physicalProjection=std::move(physicalProjection);
				computed.projection=std::move(projection);computed.forceSchedule=force.schedule;
				computed.forceDiagnostics=force.diagnostics;computed.cellSubmapCount=
					5u*executedAnomalyClosurePasses;
				computed.dualSubmapCount=dual.executedSubmapCount;computed.sourceCommandCommitCount=
					static_cast<std::uint32_t>(sourceCommits);
				computed.residentProjectionInvocationCount=
					restorationRemoved?computed.projection.residentProjectionInvocationCount:
					computed.physicalProjection.residentProjectionInvocationCount+
						std::max(1u,executedAnomalyClosurePasses-1u);
				computed.interstageFullGridTransferCount=
					force.diagnostics.substepLoopDeviceToHostTransferCount+
					cell.interstageFullGridTransferCount+dual.interstageFullGridTransferCount+
					(restorationRemoved?computed.projection.residentInterstageDeviceToHostTransferCount:
						computed.physicalProjection.residentInterstageDeviceToHostTransferCount+
							computed.projection.residentInterstageDeviceToHostTransferCount);
				computed.terminalStagingCount=
					(restorationRemoved?computed.projection.residentTerminalStagingCount:
						computed.physicalProjection.residentTerminalStagingCount+
							std::max(1u,executedAnomalyClosurePasses-1u))+1u;
				computed.combinedCertifiedWorkingSetBytes=certified;
				computed.combinedActualMetalAllocationBytes=ownerActualMetalBytes+
					force.diagnostics.actualMetalAllocationBytes+
					cell.actualMetalAllocationBytes+
					(executedAnomalyClosurePasses>1u?predictorCellActualMetalBytes:0u)+
					dual.actualMetalAllocationBytes+
					(restorationRemoved?computed.projection.residentActualMetalAllocationBytes:
						computed.physicalProjection.residentActualMetalAllocationBytes+
							computed.projection.residentActualMetalAllocationBytes);
				computed.deviceElapsedMS=force.diagnostics.advanceDeviceElapsedMS+
					predictorCellDeviceMS+anomalyCorrectorCellDeviceMS+
					dual.deviceElapsedMS+([sourceCommand GPUEndTime]-[sourceCommand GPUStartTime])*1000.0+
					anomalyPredictorDeviceMS+anomalyCorrectorSourceDeviceMS+
					(restorationRemoved?computed.projection.deviceElapsedMS:
						computed.physicalProjection.deviceElapsedMS+anomalyRestorationDeviceMS)+
					([terminalCommand GPUEndTime]-[terminalCommand GPUStartTime])*1000.0;
				double deviceStartTimeS=[upload GPUStartTime],deviceEndTimeS=[terminalCommand GPUEndTime];
				auto includeDeviceWindow=[&](const double beginning,const double end) {
					if(!(beginning>0.0)||!std::isfinite(beginning)||end<beginning||!std::isfinite(end))
						return false;
					deviceStartTimeS=std::min(deviceStartTimeS,beginning);
					deviceEndTimeS=std::max(deviceEndTimeS,end);return true;
				};
				const bool completeDeviceWindow=
					includeDeviceWindow(force.diagnostics.deviceStartTimeS,
						force.diagnostics.deviceEndTimeS)&&
					includeDeviceWindow(predictorCellDeviceStartTimeS,predictorCellDeviceEndTimeS)&&
					includeDeviceWindow(dual.deviceStartTimeS,dual.deviceEndTimeS)&&
					includeDeviceWindow([sourceCommand GPUStartTime],[sourceCommand GPUEndTime])&&
					includeDeviceWindow(restorationRemoved?computed.projection.deviceStartTimeS:
						anomalyRestorationDeviceStartTimeS,
						restorationRemoved?computed.projection.deviceEndTimeS:
						anomalyRestorationDeviceEndTimeS)&&
					(restorationRemoved||includeDeviceWindow(computed.physicalProjection.deviceStartTimeS,
						computed.physicalProjection.deviceEndTimeS))&&
					(!closeAdvectiveAnomaly||executedAnomalyClosurePasses==1u||
						(includeDeviceWindow(anomalyPredictorDeviceStartTimeS,
							anomalyPredictorDeviceEndTimeS)&&
						  (includeDeviceWindow(anomalyCorrectorCellDeviceStartTimeS,
							anomalyCorrectorCellDeviceEndTimeS)&&
						   includeDeviceWindow(anomalyCorrectorSourceDeviceStartTimeS,
							anomalyCorrectorSourceDeviceEndTimeS))));
				if(!completeDeviceWindow)return false;
				computed.deviceMakespanMS=(deviceEndTimeS-deviceStartTimeS)*1000.0;
				computed.representedTimeStepS=request.force.timeStepS;
				computed.maximumManifoldGeneration=maximumManifoldGeneration;
				computed.maximumAcceptedManifoldDeviation=maximumTerminalDeviation;
				computed.acceptedManifoldDeviationP95=terminalDeviationP95;
				computed.acceptedManifoldDeviationP50=terminalDeviationP50;
				computed.maximumPredictedAdvectiveManifoldAnomaly=
					maximumPredictedAdvectiveAnomalyFloat;
				computed.manifoldMapCellCount=measureManifold?
					static_cast<std::uint32_t>(cells):0u;
				computed.manifoldScalarDeviceToHostTransferCount=measureManifold?
					(1u+anomalyPredictorMeasurementCount):0u;
				computed.manifoldFullGridDeviceToHostTransferCount=0u;
				computed.advectiveAnomalyClosurePassCount=closeAdvectiveAnomaly?
					executedAnomalyClosurePasses:0u;
				computed.manifoldStageGeneration[0]=maximumManifoldGeneration;
				computed.manifoldStageGeneration[1]=0.0;
				computed.manifoldStageGeneration[2]=0.0;
				computed.requiredRestorationDrainFraction=
					plateauValidation.requiredDrainFraction;
				computed.deliveredRestorationDrainFraction=
					plateauValidation.deliveredDrainFraction;
				computed.restorationResidualBandPerS=
					plateauValidation.maximumPostResidualPerS;
				double suggestedManifoldTimeStepS=0.0;
				computed.manifoldNextTimeStepAvailable=enforcePlateau&&
					DeriveFireProductionManifoldTimeStep(
						static_cast<double>(request.force.timeStepS),maximumManifoldGeneration,
						plateauValidation.deliveredDrainFraction,suggestedManifoldTimeStepS,0);
				computed.suggestedManifoldTimeStepS=computed.manifoldNextTimeStepAvailable?
					suggestedManifoldTimeStepS:0.0;
				computed.manifoldPlateauPassed=enforcePlateau&&plateauPassed;
				plateauRefused=enforcePlateau&&!plateauPassed&&
					!manifoldProbeActivation&&!manifoldStageBudgetActivation;
				if( enforcePlateau )
					computed.projection.validationPassed=plateauValidation.mechanismPassed;
				if( physicalValidationProbeEnabled )
					computed.physicalProjection.validationPassed=false;
				computed.conservativeProducerPrecision=FireStateProducerPrecision::Binary32;
				computed.acceptedShape=request.force.shape;
				timestepVelocityAuditMetadataMS=timestepVelocityAuditMS();
				const bool materialGenerationAuthority=
					FireProductionEulerianGenerationHasMaterialAuthority(
						request.beginningManifoldDeviationPerCell,
						request.cellTransport.frozenVelocityMPerS);
				computed.manifoldGenerationAuthoritative=materialGenerationAuthority;
				if( enforcePlateau&&!anomalyConvergenceProbeActivation&&
					FireProductionResidentStepEligibleForAcceptedManifoldToken(computed) ) {
					std::array<std::uint64_t,2> authorityDigests={{0u,0u}};
					auto deriveAuthorityDigest=[&](const std::size_t digestIndex){
						if(digestIndex==0u)authorityDigests[digestIndex]=
							ParallelAcceptedManifoldPayloadDigest(computed,serialOwnerValidation);
						else authorityDigests[digestIndex]=ParallelAcceptedStatePayloadDigestFast(
							computed.acceptedShape,computed.conservativeValues,
							computed.projection.momentumKGPerM2S,
							computed.projection.velocityMPerS,serialOwnerValidation);
					};
					if(serialOwnerValidation){deriveAuthorityDigest(0u);deriveAuthorityDigest(1u);}
					else Implementation::GlobalThreadPool().ParallelFor(
						authorityDigests.size(),deriveAuthorityDigest);
					computed.acceptedManifoldToken_.available_=true;
					computed.acceptedManifoldToken_.representedTimeStepS_=
						static_cast<double>(request.force.timeStepS);
					computed.acceptedManifoldToken_.maximumGeneration_=maximumManifoldGeneration;
					computed.acceptedManifoldToken_.maximumAcceptedDeviation_=maximumTerminalDeviation;
					computed.acceptedManifoldToken_.acceptedDeviationP95_=terminalDeviationP95;
					computed.acceptedManifoldToken_.acceptedDeviationP50_=terminalDeviationP50;
					computed.acceptedManifoldToken_.requiredDrainFraction_=
						plateauValidation.requiredDrainFraction;
					computed.acceptedManifoldToken_.deliveredDrainFraction_=
						plateauValidation.deliveredDrainFraction;
					computed.acceptedManifoldToken_.maximumPostResidualPerS_=
						plateauValidation.maximumPostResidualPerS;
					computed.acceptedManifoldToken_.physicalMaximumPreResidualPerS_=
						computed.physicalProjection.maximumPreProjectionResidualPerS;
					computed.acceptedManifoldToken_.physicalMaximumPostResidualPerS_=
						computed.physicalProjection.maximumPostProjectionResidualPerS;
					computed.acceptedManifoldToken_.payloadDigest_=authorityDigests[0u];
					computed.acceptedManifoldToken_.acceptedStateDigest_=authorityDigests[1u];
					computed.acceptedManifoldToken_.acceptedStateDigestVersion_=2u;
					computed.acceptedManifoldToken_.generationAuthoritative_=
						materialGenerationAuthority;
				}
				timestepVelocityAuditAuthorityMS=timestepVelocityAuditMS();
				if( computed.cellSubmapCount!=5u*executedAnomalyClosurePasses||
					computed.dualSubmapCount!=15u||
					computed.sourceCommandCommitCount!=executedAnomalyClosurePasses||
					computed.residentProjectionInvocationCount!=(restorationRemoved?1u:
						1u+std::max(1u,executedAnomalyClosurePasses-1u))||
					computed.interstageFullGridTransferCount!=0u||
					MetalHostBufferReadCount-beginningReads!=
						1u+anomalyPredictorMeasurementCount||
					(measureManifold&&(computed.manifoldMapCellCount!=cells||
						computed.manifoldScalarDeviceToHostTransferCount!=
							1u+anomalyPredictorMeasurementCount||
						computed.manifoldFullGridDeviceToHostTransferCount!=0u||
						computed.manifoldStageGeneration[0]!=computed.maximumManifoldGeneration||
						computed.manifoldStageGeneration[1]!=0.0||
						computed.manifoldStageGeneration[2]!=0.0))||
					computed.combinedActualMetalAllocationBytes>certified||
					!std::isfinite(computed.deviceElapsedMS)||
					!std::isfinite(computed.deviceMakespanMS)||computed.deviceMakespanMS<0.0 ) return false;
				result=std::move(computed);
				if( timestepVelocityAuditEnabled||hostResidualProbeEnabled ) {
					const double completeMS=timestepVelocityAuditMS();
					if(hostResidualProbeEnabled)std::fprintf(stderr,
						"HOST_RESIDUAL_PREP dual_static=%.9g force=%.9g cell=%.9g "
						"parallel=%d\n",preparationWallMS[0u],preparationWallMS[1u],
						preparationWallMS[2u],serialIndependentPreparation?0:1);
					const char* wallLabel=hostResidualProbeEnabled?
						"HOST_RESIDUAL_WALL":"TIMESTEP_VELOCITY_WALL";
					std::fprintf(stderr,"%s preflight=%.9g dual_static=%.9g "
						"upload=%.9g force=%.9g cell=%.9g dual=%.9g source=%.9g "
						"physical_projection=%.9g restoration_projection=%.9g terminal=%.9g "
						"postprocess=%.9g total=%.9g\n",wallLabel,timestepVelocityAuditValidatedMS,
						timestepVelocityAuditDualStaticMS-timestepVelocityAuditValidatedMS,
						timestepVelocityAuditUploadMS-timestepVelocityAuditDualStaticMS,
						timestepVelocityAuditForceMS-timestepVelocityAuditUploadMS,
						timestepVelocityAuditCellMS-timestepVelocityAuditForceMS,
						timestepVelocityAuditDualMS-timestepVelocityAuditCellMS,
						timestepVelocityAuditSourceMS-timestepVelocityAuditDualMS,
						timestepVelocityAuditPhysicalMS-timestepVelocityAuditSourceMS,
						timestepVelocityAuditRestorationMS-timestepVelocityAuditPhysicalMS,
						timestepVelocityAuditTerminalMS-timestepVelocityAuditRestorationMS,
						completeMS-timestepVelocityAuditTerminalMS,completeMS);
					std::fprintf(stderr,"%s_POST validation=%.9g result_copy=%.9g "
						"metadata=%.9g authority=%.9g final_checks=%.9g\n",wallLabel,
						timestepVelocityAuditTerminalValidationMS-timestepVelocityAuditTerminalMS,
						timestepVelocityAuditResultCopyMS-timestepVelocityAuditTerminalValidationMS,
						timestepVelocityAuditMetadataMS-timestepVelocityAuditResultCopyMS,
						timestepVelocityAuditAuthorityMS-timestepVelocityAuditMetadataMS,
						completeMS-timestepVelocityAuditAuthorityMS);
					const double sourceDeviceMS=([sourceCommand GPUEndTime]-
						[sourceCommand GPUStartTime])*1000.0;
					const double terminalDeviceMS=([terminalCommand GPUEndTime]-
						[terminalCommand GPUStartTime])*1000.0;
					const char* deviceLabel=hostResidualProbeEnabled?
						"HOST_RESIDUAL_DEVICE":"TIMESTEP_VELOCITY_DEVICE";
					std::fprintf(stderr,"%s owner_upload=%.9g force=%.9g "
						"cell=%.9g dual=%.9g source=%.9g physical_projection=%.9g "
						"restoration_projection=%.9g terminal=%.9g work_total=%.9g makespan=%.9g\n",
						deviceLabel,timestepVelocityAuditOwnerUploadDeviceMS,
						force.diagnostics.advanceDeviceElapsedMS,cell.deviceElapsedMS,
						dual.deviceElapsedMS,sourceDeviceMS,
						restorationRemoved?projection.deviceElapsedMS:physicalProjection.deviceElapsedMS,
						restorationRemoved?0.0:projection.deviceElapsedMS,terminalDeviceMS,
						result.deviceElapsedMS,result.deviceMakespanMS);
				}
			}
			if(plateauRefused){
				if(structuredError)*structuredError=
					result.maximumAcceptedManifoldDeviation>0x1.8p-6?
						"production realized manifold deviation exceeds the low-Mach headroom allowance":
						"production restoration residual is amplified";
				return false;
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionResidentStepResult();
			if( structuredError ) try { *structuredError=
				"production resident step allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool AdvanceFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* structuredError )
	{
		result=FireProductionResidentStepResult();
		FireProductionResidentStepResult attempted;
		if(!AttemptFireProductionResidentStepMetal(request,attempted,structuredError))return false;
		if(request.enforceManifoldPlateau&&attempted.manifoldPlateauPassed&&
			!attempted.AcceptedManifoldTokenMatchesCurrentPayload()){
			result=FireProductionResidentStepResult();
			if(structuredError)*structuredError=
				"production resident step lacks material manifold generation authority";
			return false;
		}
		result=std::move(attempted);return true;
	}

	std::uint64_t FireProductionResidentStepMetalCommandCommitCount()
	{
		return MetalCommandCommitCount;
	}
}
