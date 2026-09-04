//////////////////////////////////////////////////////////////////////
//
//  FireProductionTransport.cpp - strict-binary32 production transport oracle
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionTransport.h"
#include "FireCase.h"

#include "FireProductionAdvection.h"
#include "FireSimulationRecords.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace RISE
{
	namespace
	{
		const std::uint64_t MetalAllocationQuantumBytes=UINT64_C(16384);

		bool Fail( std::string* error, const char* message ) noexcept
		{
			if( error ) try { *error=message; } catch( const std::bad_alloc& ) {}
			return false;
		}

		bool AddBytes( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( bytesPerValue&&count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue )
				return false;
			const std::uint64_t bytes=count*bytesPerValue;
			if( total>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
			total+=bytes;return true;
		}

		bool AddMetalBufferBytes( std::uint64_t requestedBytes, std::uint64_t& total )
		{
			if( requestedBytes==0u ) return false;
			const std::uint64_t remainder=requestedBytes%MetalAllocationQuantumBytes;
			if( remainder ) {
				const std::uint64_t increment=MetalAllocationQuantumBytes-remainder;
				if( requestedBytes>std::numeric_limits<std::uint64_t>::max()-increment )
					return false;
				requestedBytes+=increment;
			}
			if( total>std::numeric_limits<std::uint64_t>::max()-requestedBytes ) return false;
			total+=requestedBytes;return true;
		}

		bool AddMetalValueBuffer( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( bytesPerValue&&count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue )
				return false;
			return AddMetalBufferBytes(count*bytesPerValue,total);
		}

		void HashEOSByte( std::uint64_t& hash, const unsigned char value )
		{
			hash^=value;hash*=UINT64_C(1099511628211);
		}

		void HashEOSUInt64( std::uint64_t& hash, const std::uint64_t value )
		{
			for(unsigned int byte=0u;byte<8u;++byte)
				HashEOSByte(hash,static_cast<unsigned char>(value>>(8u*byte)));
		}

		std::uint64_t EOSDoubleBits( const double value )
		{
			std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits;
		}

		void HashEOSDouble( std::uint64_t& hash, const double value )
		{
			HashEOSUInt64(hash,EOSDoubleBits(value));
		}

		void HashEOSFloat( std::uint64_t& hash, const float value )
		{
			// Conversion is injective for every finite binary32 value and survives
			// the mechanically generated fp64/roundoff mirrors.
			HashEOSDouble(hash,static_cast<double>(value));
		}

		void HashEOSString( std::uint64_t& hash, const std::string& value )
		{
			HashEOSUInt64(hash,value.size());
			for(const unsigned char byte:value)HashEOSByte(hash,byte);
		}

		std::uint64_t EOSValueDigest( const std::vector<float>& values )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			HashEOSUInt64(hash,values.size());
			for(const float value:values)HashEOSFloat(hash,value);
			return hash;
		}

		bool ValidEOSStage( const FireProductionScalarEOSStage stage )
		{
			return stage==FireProductionScalarEOSStage::QStar||
				stage==FireProductionScalarEOSStage::QNPlus1;
		}

		bool CanonicalEOSRecordId( const std::string& value )
		{
			return value.size()==64u&&std::all_of(value.begin(),value.end(),[](const char digit){
				return (digit>='0'&&digit<='9')||(digit>='a'&&digit<='f');
			});
		}

		std::uint64_t EOSAcceptanceIdentity(
			const FireProductionScalarEOSAcceptanceRequest& request,
			const std::string& caseRecordId,const double lowerTemperatureK,
			const double upperTemperatureK,
			const std::uint64_t stateDigest,const std::uint64_t temperatureDigest,
			const double maximumEOSResidual )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar accepted-state EOS CPU v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,request.shape.nx);HashEOSUInt64(hash,request.shape.ny);
			HashEOSUInt64(hash,request.shape.nz);HashEOSFloat(hash,request.shape.cellWidthM);
			HashEOSFloat(hash,request.timeStepS);HashEOSUInt64(hash,request.attemptIdentity);
			HashEOSUInt64(hash,static_cast<std::uint8_t>(request.stage));
			HashEOSUInt64(hash,static_cast<std::uint8_t>(request.producerPrecision));
			HashEOSString(hash,request.methaneRecordId);HashEOSString(hash,caseRecordId);
			HashEOSDouble(hash,lowerTemperatureK);
			HashEOSDouble(hash,upperTemperatureK);
			HashEOSUInt64(hash,stateDigest);HashEOSUInt64(hash,temperatureDigest);
			HashEOSDouble(hash,maximumEOSResidual);
			return hash;
		}

		void HashFluxValues( std::uint64_t& hash, const std::vector<float>& values )
		{
			HashEOSUInt64(hash,values.size());
			for(const float value:values)HashEOSFloat(hash,value);
		}

		void HashFluxBytes( std::uint64_t& hash,
			const std::vector<unsigned char>& values )
		{
			HashEOSUInt64(hash,values.size());
			for(const unsigned char value:values)HashEOSByte(hash,value);
		}

		void HashSourceDoubleValues( std::uint64_t& hash,
			const std::vector<double>& values )
		{
			HashEOSUInt64(hash,values.size());
			for(const double value:values)HashEOSDouble(hash,value);
		}

		std::uint64_t SourcePacketContentIdentity(
			const FireProductionFrozenSourcePacketSeal& seal )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE canonical frozen source packet content v2";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashFluxValues(hash,seal.SourceDelta());
			HashFluxValues(hash,seal.DivergenceTargetPerS());
			HashSourceDoubleValues(hash,seal.ReactedFuelKGPerM3());
			HashSourceDoubleValues(hash,seal.OxidizedCarbonKGPerM3());
			HashSourceDoubleValues(hash,seal.GrossCarbonFormedKGPerM3());
			HashSourceDoubleValues(hash,seal.GasHeatReleaseWPerM3());
			HashSourceDoubleValues(hash,seal.SootHeatReleaseWPerM3());
			HashSourceDoubleValues(hash,seal.PilotEnergyDeltaJPerM3());
			HashSourceDoubleValues(hash,seal.PilotExpansionIntegral());
			HashSourceDoubleValues(hash,seal.RadiativeCoolingWPerM3());
			return hash;
		}

		std::uint64_t SourceGlobalRadiationIdentity(
			const FireProductionFrozenSourcePacketSeal& seal )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE canonical frozen source global radiation v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,seal.SourceInputIdentity());
			HashEOSDouble(hash,seal.RadiationBeta());
			HashEOSDouble(hash,seal.RadiationGamma());
			HashEOSDouble(hash,seal.RadiationEscapeFactor());
			HashSourceDoubleValues(hash,seal.GasHeatReleaseWPerM3());
			HashSourceDoubleValues(hash,seal.SootHeatReleaseWPerM3());
			HashSourceDoubleValues(hash,seal.RadiativeCoolingWPerM3());
			return hash;
		}

		std::uint64_t SourcePacketIdentity(
			const FireProductionFrozenSourcePacketSeal& seal )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE canonical frozen source packet seal v2";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,seal.Shape().nx);HashEOSUInt64(hash,seal.Shape().ny);
			HashEOSUInt64(hash,seal.Shape().nz);HashEOSFloat(hash,seal.Shape().cellWidthM);
			HashEOSFloat(hash,seal.TimeStepS());HashEOSDouble(hash,seal.BeginningTimeS());
			HashEOSUInt64(hash,seal.AttemptIdentity());
			HashEOSString(hash,seal.MethaneRecordId());
			HashEOSString(hash,seal.TransportRecordId());
			HashEOSString(hash,seal.OpacityRecordId());HashEOSString(hash,seal.CaseRecordId());
			HashEOSUInt64(hash,seal.BeginningStateIdentity());
			HashEOSUInt64(hash,seal.ReactionControlIdentity());
			HashEOSUInt64(hash,seal.SourceInputIdentity());
			HashEOSUInt64(hash,seal.GlobalRadiationIdentity());
			HashEOSUInt64(hash,seal.PacketContentIdentity());
			HashEOSDouble(hash,seal.MaximumScaledExpansion());
			return hash;
		}

		bool ValidDivergenceTargetRole(
			const FireProductionScalarDivergenceTargetRole role )
		{
			return role==FireProductionScalarDivergenceTargetRole::R0Base||
				role==FireProductionScalarDivergenceTargetRole::R1Base;
		}

		std::uint64_t BaseDivergenceTargetIdentity(
			const FireProductionScalarDivergenceTargetSeal& seal )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar base divergence target CPU v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,seal.Shape().nx);HashEOSUInt64(hash,seal.Shape().ny);
			HashEOSUInt64(hash,seal.Shape().nz);HashEOSFloat(hash,seal.Shape().cellWidthM);
			HashEOSFloat(hash,seal.TimeStepS());HashEOSUInt64(hash,seal.AttemptIdentity());
			HashEOSUInt64(hash,static_cast<std::uint8_t>(seal.Role()));
			for(const FireProductionProjectionBoundary value:seal.Boundary())
				HashEOSUInt64(hash,static_cast<std::uint8_t>(value));
			HashEOSString(hash,seal.MethaneRecordId());
			HashEOSUInt64(hash,seal.SourcePacketIdentity());
			HashEOSUInt64(hash,seal.FluxCompositionIdentity());
			HashFluxValues(hash,seal.TargetPerS());
			HashEOSDouble(hash,seal.MaximumScaledExpansion());
			return hash;
		}

		bool SameProjectionShape( const FireProductionProjectionShape& a,
			const FireProductionProjectionShape& b )
		{
			return a.nx==b.nx&&a.ny==b.ny&&a.nz==b.nz&&
				a.cellWidthM==b.cellWidthM;
		}

		bool ValidProjectionBoundaryTopology(
			const std::array<FireProductionProjectionBoundary,6>& boundary )
		{
			for(const FireProductionProjectionBoundary value:boundary)
				if(value!=FireProductionProjectionPeriodic&&
					value!=FireProductionProjectionPressureOpen&&
					value!=FireProductionProjectionWall)return false;
			for(unsigned int axis=0u;axis<3u;++axis)
				if((boundary[2u*axis]==FireProductionProjectionPeriodic)!=
					(boundary[2u*axis+1u]==FireProductionProjectionPeriodic))return false;
			return true;
		}

		std::uint64_t ScalarProjectionTargetIdentity(
			const FireProductionScalarProjectionTargetSeal& seal )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar authenticated projection target CPU v2";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,seal.Shape().nx);HashEOSUInt64(hash,seal.Shape().ny);
			HashEOSUInt64(hash,seal.Shape().nz);HashEOSFloat(hash,seal.Shape().cellWidthM);
			HashEOSFloat(hash,seal.TimeStepS());HashEOSUInt64(hash,seal.AttemptIdentity());
			HashEOSUInt64(hash,static_cast<std::uint8_t>(seal.Role()));
			for(const FireProductionProjectionBoundary value:seal.Boundary())
				HashEOSUInt64(hash,static_cast<std::uint8_t>(value));
			HashEOSUInt64(hash,seal.BaseTargetIdentity());
			HashEOSUInt64(hash,seal.ParentTargetIdentity());
			HashEOSUInt64(hash,seal.AcceptedCandidateIdentity());
			HashEOSUInt64(hash,seal.CorrectionIteration());
			HashFluxValues(hash,seal.TargetPerS());
			return hash;
		}

		std::uint64_t CanonicalSourceBeginningStateIdentity(
			const FireProductionProjectionShape& shape,
			const std::vector<float>& conservativeValues,
			const std::vector<float>& temperatureK )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE canonical frozen source beginning v1";
			for(const unsigned char* byte=reinterpret_cast<const unsigned char*>(domain);
				*byte;++byte)HashEOSByte(hash,*byte);
			HashEOSUInt64(hash,shape.nx);HashEOSUInt64(hash,shape.ny);
			HashEOSUInt64(hash,shape.nz);HashEOSFloat(hash,shape.cellWidthM);
			HashFluxValues(hash,conservativeValues);HashFluxValues(hash,temperatureK);
			return hash;
		}

		bool ValidHeunFluxRole( const FireProductionScalarHeunFluxRole role )
		{
			return role==FireProductionScalarHeunFluxRole::R0||
				role==FireProductionScalarHeunFluxRole::R1||
				role==FireProductionScalarHeunFluxRole::HeunAverage;
		}

		std::uint64_t HeunVelocityIdentity(
			const std::array<std::vector<float>,3>& velocity )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun frozen velocity v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			for(const std::vector<float>& axis:velocity)HashFluxValues(hash,axis);
			return hash;
		}

		bool SameFloatBits( const float first,const float second )
		{
			return std::memcmp(&first,&second,sizeof(float))==0;
		}

		bool SameFloatVectorBits( const std::vector<float>& first,
			const std::vector<float>& second )
		{
			if(first.size()!=second.size())return false;
			for(std::size_t value=0u;value<first.size();++value)
				if(!SameFloatBits(first[value],second[value]))return false;
			return true;
		}

		std::uint64_t HeunStageInputIdentity(
			const std::uint64_t attemptIdentity,const FireProductionScalarHeunFluxRole role,
			const FireProductionScalarFCTRequest& advective,
			const FireProductionScalarPhysicalFluxPrerequisiteRequest& physical )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun raw stage operands v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,attemptIdentity);
			HashEOSUInt64(hash,static_cast<std::uint8_t>(role));
			HashEOSUInt64(hash,advective.shape.nx);HashEOSUInt64(hash,advective.shape.ny);
			HashEOSUInt64(hash,advective.shape.nz);HashEOSFloat(hash,advective.shape.cellWidthM);
			HashEOSFloat(hash,advective.timeStepS);
			for(const FireProductionProjectionBoundary value:advective.boundary)
				HashEOSUInt64(hash,static_cast<std::uint8_t>(value));
			HashFluxValues(hash,advective.beginning);HashFluxValues(hash,advective.sourceDelta);
			for(const std::vector<float>& axis:advective.frozenVelocityMPerS)
				HashFluxValues(hash,axis);
			for(const float value:advective.ambient)HashEOSFloat(hash,value);
			for(const std::vector<unsigned char>& side:advective.pressureOpenInflow)
				HashFluxBytes(hash,side);
			HashEOSUInt64(hash,advective.nullity);
			HashFluxValues(hash,advective.nullspaceBasis);
			HashFluxValues(hash,advective.coordinateProjector);
			for(const float value:advective.enthalpyBoundsJPerKG)HashEOSFloat(hash,value);
			HashEOSFloat(hash,advective.feasibilityFactor);
			HashEOSFloat(hash,advective.assemblyReserveFactor);
			HashFluxValues(hash,physical.temperatureK);
			HashFluxValues(hash,physical.diffusivityM2PerS);
			HashFluxValues(hash,physical.conductivityWPerMK);
			HashEOSFloat(hash,physical.ambientTemperatureK);
			return hash;
		}

		std::uint64_t HeunSharedFCTContractIdentity(
			const FireProductionScalarFCTRequest& request )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun shared FCT contract v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,request.shape.nx);HashEOSUInt64(hash,request.shape.ny);
			HashEOSUInt64(hash,request.shape.nz);HashEOSFloat(hash,request.shape.cellWidthM);
			HashEOSFloat(hash,request.timeStepS);
			for(const FireProductionProjectionBoundary value:request.boundary)
				HashEOSUInt64(hash,static_cast<std::uint8_t>(value));
			HashFluxValues(hash,request.sourceDelta);
			for(const float value:request.ambient)HashEOSFloat(hash,value);
			HashEOSUInt64(hash,request.nullity);
			HashFluxValues(hash,request.nullspaceBasis);
			HashFluxValues(hash,request.coordinateProjector);
			for(const float value:request.enthalpyBoundsJPerKG)HashEOSFloat(hash,value);
			HashEOSFloat(hash,request.feasibilityFactor);
			HashEOSFloat(hash,request.assemblyReserveFactor);
			return hash;
		}

		std::uint64_t HeunFCTRequestIdentity(
			const FireProductionScalarFCTRequest& request )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun FCT request v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,HeunSharedFCTContractIdentity(request));
			HashFluxValues(hash,request.beginning);
			for(const std::vector<float>& axis:request.frozenVelocityMPerS)
				HashFluxValues(hash,axis);
			for(const std::vector<unsigned char>& side:request.pressureOpenInflow)
				HashFluxBytes(hash,side);
			return hash;
		}

		std::uint64_t HeunAlphaIdentity(
			const std::uint64_t attemptIdentity,const std::uint64_t averageIdentity,
			const std::array<std::uint64_t,2>& parents,
			const std::array<std::vector<float>,3>& alpha )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun fresh alpha v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,attemptIdentity);HashEOSUInt64(hash,averageIdentity);
			HashEOSUInt64(hash,parents[0]);HashEOSUInt64(hash,parents[1]);
			for(const std::vector<float>& axis:alpha)HashFluxValues(hash,axis);
			return hash;
		}

		std::uint64_t HeunAverageInputIdentity(
			const FireProductionScalarHeunFluxStage& first,
			const FireProductionScalarHeunFluxStage& second )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun averaged stage operands v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			HashEOSUInt64(hash,first.attemptIdentity);
			HashEOSUInt64(hash,first.stageInputIdentity);
			HashEOSUInt64(hash,second.stageInputIdentity);
			HashEOSUInt64(hash,first.sharedFCTContractIdentity);
			HashEOSUInt64(hash,first.compositionIdentity);
			HashEOSUInt64(hash,second.compositionIdentity);
			return hash;
		}

		std::uint64_t HeunFluxStageIdentity(
			const FireProductionScalarHeunFluxStage& stage )
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE scalar Heun flux stage CPU v1";
			for(const unsigned char byte:domain)HashEOSByte(hash,byte);
			const FireProductionScalarFCTFluxPair& pair=stage.compositeFluxPair;
			HashEOSUInt64(hash,pair.shape.nx);HashEOSUInt64(hash,pair.shape.ny);
			HashEOSUInt64(hash,pair.shape.nz);HashEOSFloat(hash,pair.shape.cellWidthM);
			HashEOSFloat(hash,pair.timeStepS);
			for(const FireProductionProjectionBoundary value:pair.boundary)
				HashEOSUInt64(hash,static_cast<std::uint8_t>(value));
			for(const std::size_t value:pair.packedFaceOffset)HashEOSUInt64(hash,value);
			HashFluxValues(hash,pair.lowFlux);HashFluxValues(hash,pair.fluxDelta);
			HashFluxValues(hash,stage.physicalMassFluxKGPerM2S);
			HashFluxValues(hash,stage.physicalEnergyFluxWPerM2);
			for(unsigned int axis=0u;axis<3u;++axis){
				HashFluxValues(hash,stage.physicalGasFluxKGPerM2S[axis]);
				HashFluxValues(hash,stage.advectiveGasLowFluxKGPerM2S[axis]);
				HashFluxValues(hash,stage.advectiveGasFluxDeltaKGPerM2S[axis]);
			}
			HashEOSString(hash,stage.methaneRecordId);
			HashEOSUInt64(hash,stage.attemptIdentity);
			HashEOSUInt64(hash,static_cast<std::uint8_t>(stage.role));
			HashEOSUInt64(hash,stage.stageInputIdentity);
			HashEOSUInt64(hash,stage.sharedFCTContractIdentity);
			HashEOSUInt64(hash,stage.fctRequestIdentity);
			HashEOSUInt64(hash,stage.frozenVelocityIdentity);
			HashEOSUInt64(hash,stage.parentCompositionIdentity[0]);
			HashEOSUInt64(hash,stage.parentCompositionIdentity[1]);
			HashEOSDouble(hash,stage.physicalConstraintForwardErrorBoundKGPerM2S);
			HashEOSDouble(hash,stage.physicalGasAveragingForwardErrorBoundKGPerM2S);
			HashEOSUInt64(hash,stage.fp64ReferenceIdentityVerified?1u:0u);
			return hash;
		}

		void FailWithoutThrow( std::string* error, const char* message ) noexcept
		{
			if( !error ) return;
			try { *error=message; } catch( const std::bad_alloc& ) {}
		}

		std::size_t CellIndex( const FireProductionProjectionShape& shape,
			std::size_t x, std::size_t y, std::size_t z )
		{
			return (z*shape.ny+y)*shape.nx+x;
		}

		std::size_t FaceIndex( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t x, std::size_t y, std::size_t z )
		{
			if( axis==0u ) return (z*shape.ny+y)*(shape.nx+1u)+x;
			if( axis==1u ) return (z*(shape.ny+1u)+y)*shape.nx+x;
			return (z*shape.ny+y)*shape.nx+x;
		}

		std::size_t AxisCoordinateExtent( const FireProductionProjectionShape& shape,
			unsigned int axis )
		{
			return axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		}

		float CanonicalPeriodicFaceValue( const FireProductionProjectionShape& shape,
			const std::vector<float>& values, unsigned int axis,
			std::size_t x, std::size_t y, std::size_t z )
		{
			if( axis==0u&&x==shape.nx ) x=0u;
			if( axis==1u&&y==shape.ny ) y=0u;
			if( axis==2u&&z==shape.nz ) z=0u;
			return values[FaceIndex(shape,axis,x,y,z)];
		}

		void SetAxisCoordinate( unsigned int axis, std::size_t coordinate,
			std::size_t& x, std::size_t& y, std::size_t& z )
		{
			if( axis==0u ) x=coordinate;
			else if( axis==1u ) y=coordinate;
			else z=coordinate;
		}

		std::size_t AxisCoordinate( unsigned int axis, std::size_t x,
			std::size_t y, std::size_t z )
		{
			return axis==0u?x:(axis==1u?y:z);
		}

		std::vector<float> PeriodicDualCarrier(
			const FireProductionPeriodicDualMomentumRequest& request,
			unsigned int component, unsigned int sweepAxis )
		{
			const FireProductionProjectionShape& shape=request.shape;
			std::vector<float> carrier(FireProductionProjectionFaceCount(shape,sweepAxis));
			const std::size_t xEnd=shape.nx+(sweepAxis==0u?1u:0u);
			const std::size_t yEnd=shape.ny+(sweepAxis==1u?1u:0u);
			const std::size_t zEnd=shape.nz+(sweepAxis==2u?1u:0u);
			for( std::size_t z=0u;z<zEnd;++z ) for( std::size_t y=0u;y<yEnd;++y )
				for( std::size_t x=0u;x<xEnd;++x ) {
					std::size_t lowerX=x,lowerY=y,lowerZ=z;
					std::size_t upperX=x,upperY=y,upperZ=z;
					const unsigned int averageAxis=component==sweepAxis?sweepAxis:component;
					const std::size_t extent=AxisCoordinateExtent(shape,averageAxis);
					const std::size_t coordinate=AxisCoordinate(averageAxis,x,y,z);
					const std::size_t canonical=coordinate==extent?0u:coordinate;
					const std::size_t previous=canonical==0u?extent-1u:canonical-1u;
					SetAxisCoordinate(averageAxis,previous,lowerX,lowerY,lowerZ);
					SetAxisCoordinate(averageAxis,canonical,upperX,upperY,upperZ);
					if( sweepAxis!=averageAxis ) {
						const std::size_t sweepExtent=AxisCoordinateExtent(shape,sweepAxis);
						const std::size_t sweepCoordinate=AxisCoordinate(sweepAxis,x,y,z);
						const std::size_t sweepCanonical=sweepCoordinate==sweepExtent?
							0u:sweepCoordinate;
						SetAxisCoordinate(sweepAxis,sweepCanonical,
							lowerX,lowerY,lowerZ);
						SetAxisCoordinate(sweepAxis,sweepCanonical,
							upperX,upperY,upperZ);
					}
					const float lower=CanonicalPeriodicFaceValue(shape,
						request.frozenVelocityMPerS[sweepAxis],sweepAxis,
						lowerX,lowerY,lowerZ);
					const float upper=CanonicalPeriodicFaceValue(shape,
						request.frozenVelocityMPerS[sweepAxis],sweepAxis,
						upperX,upperY,upperZ);
					carrier[FaceIndex(shape,sweepAxis,x,y,z)]=0.5f*(lower+upper);
				}
			return carrier;
		}

		bool AxisIsPeriodic( const FireProductionDualMomentumRequest& request,
			unsigned int axis )
		{
			return request.boundary[2u*axis]==FireProductionProjectionPeriodic&&
				request.boundary[2u*axis+1u]==FireProductionProjectionPeriodic;
		}

		FireProductionRemapBoundary RemapBoundary(
			FireProductionProjectionBoundary boundary );

		bool DualLineDimensions( const FireProductionDualMomentumRequest& request,
			unsigned int component, unsigned int sweepAxis,
			std::size_t& length, std::size_t& lines )
		{
			const FireProductionProjectionShape& shape=request.shape;
			const std::size_t componentExtent=AxisCoordinateExtent(shape,component);
			if( component==sweepAxis ) {
				length=AxisIsPeriodic(request,component)?componentExtent:componentExtent-1u;
				const unsigned int first=(component+1u)%3u,second=(component+2u)%3u;
				lines=AxisCoordinateExtent(shape,first)*AxisCoordinateExtent(shape,second);
				return length>=4u;
			}
			const bool componentPeriodic=AxisIsPeriodic(request,component);
			const std::size_t beginning=componentPeriodic?0u:
				(request.boundary[2u*component]==FireProductionProjectionWall?1u:0u);
			const std::size_t end=componentPeriodic?componentExtent:
				componentExtent+1u-
				(request.boundary[2u*component+1u]==FireProductionProjectionWall?1u:0u);
			const unsigned int remaining=3u-component-sweepAxis;
			length=AxisCoordinateExtent(shape,sweepAxis);
			lines=(end-beginning)*AxisCoordinateExtent(shape,remaining);
			return length>=4u&&lines>0u;
		}

		float CrossCarrierAt( const FireProductionDualMomentumRequest& request,
			unsigned int component, unsigned int sweepAxis,
			std::size_t sweepFace, std::size_t componentFace,
			std::size_t remainingCoordinate )
		{
			const FireProductionProjectionShape& shape=request.shape;
			if( (sweepFace==0u&&request.boundary[2u*sweepAxis]==
				FireProductionProjectionWall)||
			(sweepFace==AxisCoordinateExtent(shape,sweepAxis)&&
			 request.boundary[2u*sweepAxis+1u]==FireProductionProjectionWall) ) return 0.0f;
			const unsigned int remaining=3u-component-sweepAxis;
			const std::size_t componentExtent=AxisCoordinateExtent(shape,component);
			auto velocityAt=[&](std::size_t componentCell) {
				std::size_t x=0u,y=0u,z=0u;
				SetAxisCoordinate(sweepAxis,sweepFace,x,y,z);
				SetAxisCoordinate(component,componentCell,x,y,z);
				SetAxisCoordinate(remaining,remainingCoordinate,x,y,z);
				return request.frozenVelocityMPerS[sweepAxis][
					FaceIndex(shape,sweepAxis,x,y,z)];
			};
			float lower=0.0f,upper=0.0f;
			if( componentFace>0u&&componentFace<componentExtent ) {
				lower=velocityAt(componentFace-1u);upper=velocityAt(componentFace);
			} else if( componentFace==0u ) {
				upper=velocityAt(0u);
				const FireProductionProjectionBoundary boundary=request.boundary[2u*component];
				lower=boundary==FireProductionProjectionPeriodic?
					velocityAt(componentExtent-1u):
					(boundary==FireProductionProjectionWall?-upper:upper);
			} else {
				lower=velocityAt(componentExtent-1u);
				const FireProductionProjectionBoundary boundary=
					request.boundary[2u*component+1u];
				upper=boundary==FireProductionProjectionPeriodic?velocityAt(0u):
					(boundary==FireProductionProjectionWall?-lower:lower);
			}
			return 0.5f*(lower+upper);
		}

		bool BuildDualAxisRequest( const FireProductionDualMomentumRequest& request,
			unsigned int component, unsigned int sweepAxis, float timeStepS,
			const std::vector<float>& density, const std::vector<float>& momentum,
			FireProductionRemapRequest& lineRequest, std::string* error )
		{
			const FireProductionProjectionShape& shape=request.shape;
			std::size_t length=0u,lines=0u;
			if( !DualLineDimensions(request,component,sweepAxis,length,lines) )
				return Fail(error,"dual momentum owned line is too short");
			lineRequest=FireProductionRemapRequest();
			lineRequest.lineLength=length;lineRequest.lineCount=lines;
			lineRequest.componentCount=2u;lineRequest.cellWidthM=shape.cellWidthM;
			lineRequest.timeStepS=timeStepS;lineRequest.asymmetricBoundaries=true;
			lineRequest.lowerBoundary=RemapBoundary(request.boundary[2u*sweepAxis]);
			lineRequest.upperBoundary=RemapBoundary(request.boundary[2u*sweepAxis+1u]);
			lineRequest.lineSpecificAmbientValues=!AxisIsPeriodic(request,sweepAxis);
			lineRequest.ambientValues.assign(2u,0.0f);
			if( lineRequest.lineSpecificAmbientValues ) {
				lineRequest.lowerAmbientValues.resize(2u*lines);
				lineRequest.upperAmbientValues.resize(2u*lines);
			}
			lineRequest.values.resize(2u*lines*length);
			lineRequest.faceVelocityMPerS.resize(lines*(length+1u));
			const std::size_t componentExtent=AxisCoordinateExtent(shape,component);
			if( component==sweepAxis ) {
				const unsigned int first=(component+1u)%3u,second=(component+2u)%3u;
				const std::size_t firstExtent=AxisCoordinateExtent(shape,first);
				for( std::size_t line=0u;line<lines;++line ) {
					const std::size_t firstCoordinate=line%firstExtent;
					const std::size_t secondCoordinate=line/firstExtent;
					for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
						std::size_t x=0u,y=0u,z=0u;
						SetAxisCoordinate(component,AxisIsPeriodic(request,component)?
							coordinate:coordinate+1u,x,y,z);
						SetAxisCoordinate(first,firstCoordinate,x,y,z);
						SetAxisCoordinate(second,secondCoordinate,x,y,z);
						const std::size_t face=FaceIndex(shape,component,x,y,z);
						lineRequest.values[line*length+coordinate]=density[face];
						lineRequest.values[(lines+line)*length+coordinate]=momentum[face];
					}
					for( std::size_t face=0u;face<=length;++face ) {
						std::size_t lowerCoordinate=0u,upperCoordinate=0u;
						if( AxisIsPeriodic(request,component) ) {
							upperCoordinate=face==length?0u:face;
							lowerCoordinate=upperCoordinate?upperCoordinate-1u:componentExtent-1u;
						} else {lowerCoordinate=face;upperCoordinate=face+1u;}
						std::size_t lx=0u,ly=0u,lz=0u,ux=0u,uy=0u,uz=0u;
						SetAxisCoordinate(component,lowerCoordinate,lx,ly,lz);
						SetAxisCoordinate(component,upperCoordinate,ux,uy,uz);
						SetAxisCoordinate(first,firstCoordinate,lx,ly,lz);
						SetAxisCoordinate(first,firstCoordinate,ux,uy,uz);
						SetAxisCoordinate(second,secondCoordinate,lx,ly,lz);
						SetAxisCoordinate(second,secondCoordinate,ux,uy,uz);
						const float lower=request.frozenVelocityMPerS[component][
							FaceIndex(shape,component,lx,ly,lz)];
						const float upper=request.frozenVelocityMPerS[component][
							FaceIndex(shape,component,ux,uy,uz)];
						lineRequest.faceVelocityMPerS[line*(length+1u)+face]=0.5f*(lower+upper);
					}
				}
			} else {
				const bool componentPeriodic=AxisIsPeriodic(request,component);
				const std::size_t componentBeginning=componentPeriodic?0u:
					(request.boundary[2u*component]==FireProductionProjectionWall?1u:0u);
				const unsigned int remaining=3u-component-sweepAxis;
				const std::size_t remainingExtent=AxisCoordinateExtent(shape,remaining);
				for( std::size_t line=0u;line<lines;++line ) {
					const std::size_t componentCoordinate=componentBeginning+
						line/remainingExtent;
					const std::size_t remainingCoordinate=line%remainingExtent;
					for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
						std::size_t x=0u,y=0u,z=0u;
						SetAxisCoordinate(component,componentCoordinate,x,y,z);
						SetAxisCoordinate(sweepAxis,coordinate,x,y,z);
						SetAxisCoordinate(remaining,remainingCoordinate,x,y,z);
						const std::size_t face=FaceIndex(shape,component,x,y,z);
						lineRequest.values[line*length+coordinate]=density[face];
						lineRequest.values[(lines+line)*length+coordinate]=momentum[face];
					}
					for( std::size_t face=0u;face<=length;++face )
						lineRequest.faceVelocityMPerS[line*(length+1u)+face]=CrossCarrierAt(
							request,component,sweepAxis,face,componentCoordinate,
							remainingCoordinate);
				}
			}
			if( AxisIsPeriodic(request,sweepAxis) )
				for( std::size_t line=0u;line<lines;++line )
					lineRequest.faceVelocityMPerS[line*(length+1u)+length]=
						lineRequest.faceVelocityMPerS[line*(length+1u)];
			if( lineRequest.lineSpecificAmbientValues ) for( std::size_t line=0u;line<lines;++line ) {
				lineRequest.lowerAmbientValues[line]=request.ambientDensityKGPerM3;
				lineRequest.upperAmbientValues[line]=request.ambientDensityKGPerM3;
				lineRequest.lowerAmbientValues[lines+line]=component==sweepAxis?
					request.ambientDensityKGPerM3*lineRequest.faceVelocityMPerS[
						line*(length+1u)]:0.0f;
				lineRequest.upperAmbientValues[lines+line]=component==sweepAxis?
					request.ambientDensityKGPerM3*lineRequest.faceVelocityMPerS[
						line*(length+1u)+length]:0.0f;
			}
			return true;
		}

		bool ApplyDualAxis( const FireProductionDualMomentumRequest& request,
			unsigned int component, unsigned int sweepAxis, float timeStepS,
			std::vector<float>& density, std::vector<float>& momentum,
			std::string* error )
		{
			FireProductionRemapRequest lineRequest;
			if( !BuildDualAxisRequest(request,component,sweepAxis,timeStepS,
				density,momentum,lineRequest,error) ) return false;
			FireProductionRemapResult remapped;
			if( !RemapFireProductionCPU(lineRequest,remapped,error) ) return false;
			const FireProductionProjectionShape& shape=request.shape;
			const std::size_t length=lineRequest.lineLength,lines=lineRequest.lineCount;
			if( component==sweepAxis ) {
				const unsigned int first=(component+1u)%3u,second=(component+2u)%3u;
				const std::size_t firstExtent=AxisCoordinateExtent(shape,first);
				for( std::size_t line=0u;line<lines;++line )
					for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
						std::size_t x=0u,y=0u,z=0u;
						SetAxisCoordinate(component,AxisIsPeriodic(request,component)?
							coordinate:coordinate+1u,x,y,z);
						SetAxisCoordinate(first,line%firstExtent,x,y,z);
						SetAxisCoordinate(second,line/firstExtent,x,y,z);
						const std::size_t face=FaceIndex(shape,component,x,y,z);
						density[face]=remapped.updatedValues[line*length+coordinate];
						momentum[face]=remapped.updatedValues[(lines+line)*length+coordinate];
					}
			} else {
				const bool componentPeriodic=AxisIsPeriodic(request,component);
				const std::size_t componentBeginning=componentPeriodic?0u:
					(request.boundary[2u*component]==FireProductionProjectionWall?1u:0u);
				const unsigned int remaining=3u-component-sweepAxis;
				const std::size_t remainingExtent=AxisCoordinateExtent(shape,remaining);
				for( std::size_t line=0u;line<lines;++line )
					for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
						std::size_t x=0u,y=0u,z=0u;
						SetAxisCoordinate(component,componentBeginning+
							line/remainingExtent,x,y,z);
						SetAxisCoordinate(sweepAxis,coordinate,x,y,z);
						SetAxisCoordinate(remaining,line%remainingExtent,x,y,z);
						const std::size_t face=FaceIndex(shape,component,x,y,z);
						density[face]=remapped.updatedValues[line*length+coordinate];
						momentum[face]=remapped.updatedValues[(lines+line)*length+coordinate];
					}
			}
			return true;
		}

		bool SamePeriodicFaceValue( const float first, const float second )
		{
			return first==second;
		}

		bool PeriodicFaceSeamEqual( const FireProductionProjectionShape& shape,
			const std::vector<float>& values, unsigned int axis )
		{
			if( axis==0u ) for( std::size_t z=0u;z<shape.nz;++z )
				for( std::size_t y=0u;y<shape.ny;++y )
					if( !SamePeriodicFaceValue(values[FaceIndex(shape,axis,0u,y,z)],
						values[FaceIndex(shape,axis,shape.nx,y,z)]) ) return false;
			if( axis==1u ) for( std::size_t z=0u;z<shape.nz;++z )
				for( std::size_t x=0u;x<shape.nx;++x )
					if( !SamePeriodicFaceValue(values[FaceIndex(shape,axis,x,0u,z)],
						values[FaceIndex(shape,axis,x,shape.ny,z)]) ) return false;
			if( axis==2u ) for( std::size_t y=0u;y<shape.ny;++y )
				for( std::size_t x=0u;x<shape.nx;++x )
					if( !SamePeriodicFaceValue(values[FaceIndex(shape,axis,x,y,0u)],
						values[FaceIndex(shape,axis,x,y,shape.nz)]) ) return false;
			return true;
		}

		bool PeriodicFaceSeamBitEqual( const FireProductionProjectionShape& shape,
			const std::vector<float>& values, unsigned int axis )
		{
			auto equal=[]( const float first, const float second ) {
				std::uint32_t firstBits=0u,secondBits=0u;
				std::memcpy(&firstBits,&first,sizeof(firstBits));
				std::memcpy(&secondBits,&second,sizeof(secondBits));
				return firstBits==secondBits;
			};
			if( axis==0u ) for( std::size_t z=0u;z<shape.nz;++z )
				for( std::size_t y=0u;y<shape.ny;++y )
					if( !equal(values[FaceIndex(shape,axis,0u,y,z)],
						values[FaceIndex(shape,axis,shape.nx,y,z)]) ) return false;
			if( axis==1u ) for( std::size_t z=0u;z<shape.nz;++z )
				for( std::size_t x=0u;x<shape.nx;++x )
					if( !equal(values[FaceIndex(shape,axis,x,0u,z)],
						values[FaceIndex(shape,axis,x,shape.ny,z)]) ) return false;
			if( axis==2u ) for( std::size_t y=0u;y<shape.ny;++y )
				for( std::size_t x=0u;x<shape.nx;++x )
					if( !equal(values[FaceIndex(shape,axis,x,y,0u)],
						values[FaceIndex(shape,axis,x,y,shape.nz)]) ) return false;
			return true;
		}

		void PublishPeriodicDualSeam( const FireProductionProjectionShape& shape,
			unsigned int component, std::vector<float>& density,
			std::vector<float>& momentum, std::uint32_t& copyCount )
		{
			const std::size_t firstEnd=component==0u?shape.ny:shape.nx;
			const std::size_t secondEnd=component==2u?shape.ny:shape.nz;
			for( std::size_t second=0u;second<secondEnd;++second )
				for( std::size_t first=0u;first<firstEnd;++first ) {
					std::size_t lowX=component==0u?0u:first;
					std::size_t lowY=component==0u?first:(component==1u?0u:second);
					std::size_t lowZ=component==2u?0u:second;
					std::size_t highX=lowX,highY=lowY,highZ=lowZ;
					SetAxisCoordinate(component,AxisCoordinateExtent(shape,component),
						highX,highY,highZ);
					const std::size_t low=FaceIndex(shape,component,lowX,lowY,lowZ);
					const std::size_t high=FaceIndex(shape,component,highX,highY,highZ);
					density[high]=density[low];momentum[high]=momentum[low];copyCount+=2u;
				}
		}

		FireProductionRemapBoundary RemapBoundary(
			FireProductionProjectionBoundary boundary )
		{
			if( boundary==FireProductionProjectionPeriodic )
				return FireProductionRemapPeriodic;
			if( boundary==FireProductionProjectionPressureOpen )
				return FireProductionRemapPressureOpen;
			return FireProductionRemapWall;
		}

		std::size_t AxisExtent( const FireProductionProjectionShape& shape,
			unsigned int axis )
		{
			return axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		}

		std::size_t AxisLineCount( const FireProductionProjectionShape& shape,
			unsigned int axis )
		{
			return axis==0u?shape.ny*shape.nz:
				(axis==1u?shape.nx*shape.nz:shape.nx*shape.ny);
		}

		void AxisCoordinates( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t line, std::size_t coordinate,
			std::size_t& x, std::size_t& y, std::size_t& z );

		bool ValidateAxisSchedule( const FireProductionCellPalindromeRequest& request,
			unsigned int axis, float timeStepS, std::string* error )
		{
			const std::size_t length=AxisExtent(request.shape,axis);
			const std::size_t lines=AxisLineCount(request.shape,axis);
			const std::size_t valueCount=request.componentCount*request.shape.CellCount();
			const std::size_t maximum=std::numeric_limits<std::size_t>::max();
			if( lines>maximum/(length+1u)||
				request.componentCount>maximum/(lines*(length+1u)) )
				return Fail(error,"production palindrome axis dimensions overflow");
			const std::size_t fluxCount=request.componentCount*lines*(length+1u);
			if( valueCount>std::numeric_limits<std::uint32_t>::max()||
				fluxCount>std::numeric_limits<std::uint32_t>::max()||
				lines>std::numeric_limits<std::uint32_t>::max()||
				request.componentCount>std::numeric_limits<std::uint32_t>::max() )
				return Fail(error,"production palindrome axis exceeds kernel indexing");
			FireProductionRemapRequest axisResource;
			axisResource.lineLength=length;axisResource.lineCount=lines;
			axisResource.componentCount=request.componentCount;
			std::uint64_t axisWorkingBytes=0u;
			if( !FireProductionRemapWorkingSetBytes(axisResource,axisWorkingBytes)||
				axisWorkingBytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"production palindrome axis exceeds two GiB");
			const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
			for( std::size_t line=0;line<lines;++line ) {
				double previous=0.0;
				float seamVelocity=0.0f;
				for( std::size_t face=0;face<=length;++face ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,face,x,y,z);
					const float velocity=request.frozenVelocityMPerS[axis][
						FaceIndex(request.shape,axis,x,y,z)];
					if( face==0u ) seamVelocity=velocity;
					if( periodic&&face==length&&velocity!=seamVelocity )
						return Fail(error,"production palindrome periodic seam is not single-valued");
					const float courant=timeStepS*velocity/request.shape.cellWidthM;
					if( !std::isfinite(courant) )
						return Fail(error,"production palindrome Courant number is nonfinite");
					const double departure=static_cast<double>(face)-
						static_cast<double>(courant);
					if( !std::isfinite(departure)||(face>0u&&departure<previous) )
						return Fail(error,"production palindrome backtraced map is folded");
					previous=departure;
				}
			}
			return true;
		}

		void AxisCoordinates( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t line, std::size_t coordinate,
			std::size_t& x, std::size_t& y, std::size_t& z )
		{
			if( axis==0u ) {x=coordinate;y=line%shape.ny;z=line/shape.ny;return;}
			if( axis==1u ) {x=line%shape.nx;y=coordinate;z=line/shape.nx;return;}
			x=line%shape.nx;y=line/shape.nx;z=coordinate;
		}

		bool ApplyAxis( const FireProductionCellPalindromeRequest& request,
			unsigned int axis, float timeStepS, std::vector<float>& values,
			std::vector<float>* acceptedGasMassDoseKGPerM2, std::string* error )
		{
			FireProductionRemapRequest lineRequest;
			lineRequest.lineLength=AxisExtent(request.shape,axis);
			lineRequest.lineCount=AxisLineCount(request.shape,axis);
			lineRequest.componentCount=request.componentCount;
			lineRequest.cellWidthM=request.shape.cellWidthM;
			lineRequest.timeStepS=timeStepS;
			lineRequest.asymmetricBoundaries=true;
			lineRequest.lowerBoundary=RemapBoundary(request.boundary[2u*axis]);
			lineRequest.upperBoundary=RemapBoundary(request.boundary[2u*axis+1u]);
			lineRequest.ambientValues=request.ambientValues;
			lineRequest.values.resize(values.size());
			lineRequest.faceVelocityMPerS.resize(lineRequest.lineCount*
				(lineRequest.lineLength+1u));
			for( std::size_t line=0;line<lineRequest.lineCount;++line ) {
				for( std::size_t face=0;face<=lineRequest.lineLength;++face ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,face,x,y,z);
					lineRequest.faceVelocityMPerS[line*(lineRequest.lineLength+1u)+face]=
						request.frozenVelocityMPerS[axis][FaceIndex(request.shape,axis,x,y,z)];
				}
				for( std::size_t coordinate=0;coordinate<lineRequest.lineLength;++coordinate ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,coordinate,x,y,z);
					const std::size_t cell=CellIndex(request.shape,x,y,z);
					for( std::size_t component=0;component<request.componentCount;++component )
						lineRequest.values[(component*lineRequest.lineCount+line)*
							lineRequest.lineLength+coordinate]=
							values[component*request.shape.CellCount()+cell];
				}
			}
			FireProductionRemapResult lineResult;
			if( !RemapFireProductionCPU(lineRequest,lineResult,error) ) return false;
			if( acceptedGasMassDoseKGPerM2 ) {
				acceptedGasMassDoseKGPerM2->assign(
					FireProductionProjectionFaceCount(request.shape,axis),0.0f);
				for( std::size_t line=0;line<lineRequest.lineCount;++line )
					for( std::size_t face=0;face<=lineRequest.lineLength;++face ) {
						float gasMassDose=0.0f;
						for( std::size_t component=1u;component<=6u;++component )
							gasMassDose+=lineResult.faceFluxes[
								(component*lineRequest.lineCount+line)*
								(lineRequest.lineLength+1u)+face];
						std::size_t x=0u,y=0u,z=0u;
						AxisCoordinates(request.shape,axis,line,face,x,y,z);
						(*acceptedGasMassDoseKGPerM2)[FaceIndex(
							request.shape,axis,x,y,z)]=gasMassDose;
					}
			}
			for( std::size_t line=0;line<lineRequest.lineCount;++line )
				for( std::size_t coordinate=0;coordinate<lineRequest.lineLength;++coordinate ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,coordinate,x,y,z);
					const std::size_t cell=CellIndex(request.shape,x,y,z);
					for( std::size_t component=0;component<request.componentCount;++component )
						values[component*request.shape.CellCount()+cell]=
							lineResult.updatedValues[(component*lineRequest.lineCount+line)*
								lineRequest.lineLength+coordinate];
				}
			return true;
		}
	}

	void FireProductionFrozenSourcePacketSeal::FinalizeIdentities()
	{
		packetContentIdentity_=SourcePacketContentIdentity(*this);
		globalRadiationIdentity_=SourceGlobalRadiationIdentity(*this);
		packetIdentity_=SourcePacketIdentity(*this);
		sealed_=true;
	}

	bool FireProductionFrozenSourcePacketSeal::Matches( std::string* error ) const
	{
		const std::size_t cells=shape_.CellCount();
		if(!sealed_||shape_.nx<4u||shape_.nx>1024u||shape_.ny<4u||shape_.ny>1024u||
			shape_.nz<4u||shape_.nz>1024u||!(shape_.cellWidthM>0.0f)||
			!std::isfinite(shape_.cellWidthM)||!(timeStepS_>0.0f)||
			!std::isfinite(timeStepS_)||!std::isfinite(beginningTimeS_)||
			beginningTimeS_<0.0||attemptIdentity_==0u||
			!CanonicalEOSRecordId(methaneRecordId_)||
			!CanonicalEOSRecordId(transportRecordId_)||
			!CanonicalEOSRecordId(opacityRecordId_)||!CanonicalEOSRecordId(caseRecordId_)||
			beginningTemperatureK_.size()!=cells||sourceDelta_.size()!=9u*cells||
			divergenceTargetPerS_.size()!=cells||
			reactedFuelKGPerM3_.size()!=cells||
			oxidizedCarbonKGPerM3_.size()!=cells||grossCarbonFormedKGPerM3_.size()!=cells||
			gasHeatReleaseWPerM3_.size()!=cells||sootHeatReleaseWPerM3_.size()!=cells||
			pilotEnergyDeltaJPerM3_.size()!=cells||pilotExpansionIntegral_.size()!=cells||
			radiativeCoolingWPerM3_.size()!=cells||
			!std::isfinite(radiationBeta_)||radiationBeta_<0.0||
			!std::isfinite(radiationGamma_)||radiationGamma_<0.0||radiationGamma_>1.0||
			!std::isfinite(radiationEscapeFactor_)||radiationEscapeFactor_<0.0||
			!std::isfinite(maximumScaledExpansion_)||maximumScaledExpansion_>0.5||
			beginningStateIdentity_==0u||reactionControlIdentity_==0u||
			sourceInputIdentity_==0u||globalRadiationIdentity_==0u||
			packetContentIdentity_==0u||packetIdentity_==0u)
			return Fail(error,"canonical frozen source-packet seal metadata is invalid");
		for(const float value:beginningTemperatureK_)if(!std::isfinite(value)||
			value<RISE::FireSimulationMethaneRecord::PhysicalV1().TemperatureMinK()||
			value>RISE::FireSimulationMethaneRecord::PhysicalV1().TemperatureMaxK())return Fail(error,
				"production canonical source beginning temperature is invalid");
		for(const float value:sourceDelta_)if(!std::isfinite(value)||
			(value==0.0f&&std::signbit(static_cast<double>(value))))return Fail(error,
				"canonical frozen source-packet resident dose is noncanonical");
		for(const float value:divergenceTargetPerS_)if(!std::isfinite(value)||
			(value==0.0f&&std::signbit(static_cast<double>(value))))return Fail(error,
				"canonical frozen source-packet divergence target is noncanonical");
		const std::vector<double>* diagnostic[]={&reactedFuelKGPerM3_,
			&oxidizedCarbonKGPerM3_,&grossCarbonFormedKGPerM3_,&gasHeatReleaseWPerM3_,
			&sootHeatReleaseWPerM3_,&pilotEnergyDeltaJPerM3_,&pilotExpansionIntegral_,
			&radiativeCoolingWPerM3_};
		for(const std::vector<double>* values:diagnostic)for(const double value:*values)
			if(!std::isfinite(value)||(value==0.0&&std::signbit(value)))return Fail(error,
				"canonical frozen source-packet diagnostic is noncanonical");
		if(packetContentIdentity_!=SourcePacketContentIdentity(*this)||
			globalRadiationIdentity_!=SourceGlobalRadiationIdentity(*this)||
			packetIdentity_!=SourcePacketIdentity(*this))return Fail(error,
				"canonical frozen source-packet seal identity does not match its content");
		if(error)error->clear();return true;
	}

	bool FireProductionFrozenSourcePacketSealMatches(
		const FireProductionFrozenSourcePacketSeal& seal,std::string* error )
	{
		return seal.Matches(error);
	}

	bool FireProductionFrozenSourcePacketSealMatchesBeginningState(
		const FireProductionFrozenSourcePacketSeal& seal,
		const FireProductionProjectionShape& shape,
		const std::vector<float>& conservativeValues,
		const std::vector<float>& temperatureK,std::string* error )
	{
		if(!FireProductionFrozenSourcePacketSealMatches(seal,error))return false;
		const FireProductionProjectionShape& sourceShape=seal.Shape();
		const std::size_t cells=shape.CellCount();
		if(sourceShape.nx!=shape.nx||sourceShape.ny!=shape.ny||sourceShape.nz!=shape.nz||
			sourceShape.cellWidthM!=shape.cellWidthM||
			conservativeValues.size()!=9u*cells||temperatureK.size()!=cells||
			seal.BeginningTemperatureK().size()!=cells||
			!SameFloatVectorBits(seal.BeginningTemperatureK(),temperatureK)||
			seal.BeginningStateIdentity()!=CanonicalSourceBeginningStateIdentity(
				shape,conservativeValues,temperatureK))return Fail(error,
				"canonical frozen source beginning-state parent differs");
		if(error)error->clear();return true;
	}

	bool FireProductionResidentTransportLiveIncrementWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!std::isfinite(shape.cellWidthM)||
			!(shape.cellWidthM>0.0f))return false;
		const std::uint64_t cells=static_cast<std::uint64_t>(shape.nx)*shape.ny*shape.nz;
		const std::uint64_t boundaryFaces=2u*(static_cast<std::uint64_t>(shape.ny)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.ny);
		// Device-private coefficient publication, identity, immutable record
		// tables, fixed fuel-inlet classes, parameters, and two refusal words.
		// State/T/projected velocity are already resident owner surfaces.
		return AddMetalValueBuffer(3u*cells,sizeof(float),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint64_t),bytes)&&
			AddMetalValueBuffer(7u*32u+64u,sizeof(float),bytes)&&
			AddMetalValueBuffer(6u*(1u+5u*128u),sizeof(float),bytes)&&
			AddMetalValueBuffer(boundaryFaces,sizeof(unsigned char),bytes)&&
			AddMetalValueBuffer(256u,sizeof(unsigned char),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint32_t),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint32_t),bytes);
	}

	bool FireProductionResidentPhysicalFluxLiveIncrementWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return false;
		const std::uint64_t faces=FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		const std::uint64_t boundaryFaces=2u*(static_cast<std::uint64_t>(shape.ny)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.ny);
		// Private immutable surfaces not already owned by transport, followed by
		// donor, retained canonical FCT delta, MC-MUSCL, f_N, energy, J_g,
		// staged log/enthalpy certification, both composites, the physical
		// obligation word, and publication.
		return AddMetalValueBuffer(boundaryFaces,sizeof(unsigned char),bytes)&&
			AddMetalValueBuffer(9u,sizeof(float),bytes)&&
			AddMetalValueBuffer(64u,sizeof(float),bytes)&&
			AddMetalValueBuffer(64u,sizeof(float),bytes)&&
			AddMetalValueBuffer(64u,sizeof(float),bytes)&&
			AddMetalValueBuffer(16u,sizeof(unsigned char),bytes)&&
			AddMetalValueBuffer(9u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(9u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(9u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(8u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(7u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(9u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(9u*faces,sizeof(float),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint32_t),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint64_t),bytes)&&bytes<=(UINT64_C(1)<<31u);
	}

	bool FireProductionResidentEOSCandidateLiveIncrementWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return false;
		const std::uint64_t cells=static_cast<std::uint64_t>(shape.nx)*shape.ny*shape.nz;
		const std::uint64_t faces=FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		// Complete source-inclusive FCT Q* producer over the retained r198 delta:
		// low state, limiter ratios, shared face alpha, accepted candidate,
		// thermochemistry/certificate inputs, EOS fields, and identities.
		if(!AddMetalValueBuffer(9u*cells,sizeof(float),bytes)||
			!AddMetalValueBuffer(11u*cells,sizeof(float),bytes)||
			!AddMetalValueBuffer(faces,sizeof(float),bytes)||
			!AddMetalValueBuffer(14u,sizeof(float),bytes)||
			!AddMetalValueBuffer(64u,sizeof(float),bytes)||
			!AddMetalValueBuffer(1u,128u,bytes))return false;
		return AddMetalValueBuffer(9u*cells,sizeof(float),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint64_t),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint64_t),bytes)&&
			// Device-private compensated copy of the sealed binary64 EOS record.
			AddMetalValueBuffer(870u,sizeof(float),bytes)&&
			AddMetalValueBuffer(cells,sizeof(float),bytes)&&
			AddMetalValueBuffer(cells,sizeof(float),bytes)&&
			AddMetalValueBuffer(cells,sizeof(float),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint64_t),bytes)&&
			// Device-side first-failure witness: minimum cell plus per-cell EOS subterm map.
			AddMetalValueBuffer(1u,sizeof(std::uint32_t),bytes)&&
			AddMetalValueBuffer(cells,sizeof(std::uint32_t),bytes)&&
			AddMetalValueBuffer(256u,sizeof(unsigned char),bytes)&&
			AddMetalValueBuffer(1u,sizeof(std::uint32_t),bytes)&&bytes<=(UINT64_C(1)<<31u);
	}

	bool FireProductionCellPalindromeWorkingSetBytes(
		const FireProductionProjectionShape& shape, std::size_t componentCount,
		std::uint64_t& bytes, bool retainAcceptedGasMassDose )
	{
		bytes=0u;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||componentCount==0u ) return false;
		const std::uint64_t cells=static_cast<std::uint64_t>(shape.nx)*shape.ny*shape.nz;
		if( componentCount>std::numeric_limits<std::uint64_t>::max()/cells ) return false;
		const std::uint64_t values=cells*componentCount;
		const std::uint64_t xFaces=static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz;
		const std::uint64_t yFaces=static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz;
		const std::uint64_t zFaces=static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u);
		const std::uint64_t allFaces=xFaces+yFaces+zFaces;
		const std::uint64_t retainedGasFaces=componentCount==9u&&retainAcceptedGasMassDose?
			2u*xFaces+2u*yFaces+zFaces:0u;
		const std::uint64_t maximumLineFaces=std::max(xFaces,std::max(yFaces,zFaces));
		if( componentCount>std::numeric_limits<std::uint64_t>::max()/maximumLineFaces )
			return false;
		const std::uint64_t componentLineFaces=componentCount*maximumLineFaces;
		std::uint64_t total=0u;
		// Caller-owned request/result payloads remain live through publication.
		if( !AddBytes(values,2u*sizeof(float),total)||
			!AddBytes(allFaces,sizeof(float),total)||
			!AddBytes(componentCount,sizeof(float),total)||
			!AddBytes(retainedGasFaces,sizeof(float),total) ) return false;
		// Eight value-sized Metal buffers: input/output staging, two resident grids,
		// and four resident line/reconstruction fields.
		for( unsigned int buffer=0u;buffer<8u;++buffer )
			if( !AddMetalValueBuffer(values,sizeof(float),total) ) return false;
		// Each carrier is present once in Shared staging and once in Private storage.
		const std::uint64_t faceCounts[]={xFaces,yFaces,zFaces};
		for( const std::uint64_t faceCount : faceCounts )
			for( unsigned int buffer=0u;buffer<2u;++buffer )
				if( !AddMetalValueBuffer(faceCount,sizeof(float),total) ) return false;
		if( !AddMetalValueBuffer(maximumLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(cells,sizeof(float),total)||
			!AddMetalValueBuffer(componentLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(componentLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(componentCount,sizeof(float),total) ) return false;
		if( retainedGasFaces&&!AddMetalValueBuffer(retainedGasFaces,sizeof(float),total) )
			return false;
		// Five grid-parameter and five line-parameter resources remain retained by
		// the one command until its terminal publication completes.
		for( unsigned int pass=0u;pass<5u;++pass )
			if( !AddMetalBufferBytes(sizeof(std::uint32_t)*5u,total)||
				!AddMetalBufferBytes(sizeof(std::uint32_t)*5u+sizeof(float)*2u,total) )
				return false;
		bytes=total;return true;
	}

	bool ValidateFireProductionCellPalindromeRequest(
		const FireProductionCellPalindromeRequest& request, std::string* error )
	{
		const FireProductionProjectionShape& shape=request.shape;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||request.componentCount==0u||
			!(shape.cellWidthM>0.0f)||request.timeStepS<0.0f||
			!std::isfinite(shape.cellWidthM)||!std::isfinite(request.timeStepS) )
			return Fail(error,"production palindrome shape or schedule is invalid");
		const std::size_t maximum=std::numeric_limits<std::size_t>::max();
		if( shape.nx>maximum/shape.ny||shape.nx*shape.ny>maximum/shape.nz )
			return Fail(error,"production palindrome shape overflows");
		const std::size_t cells=shape.CellCount();
		if( request.componentCount>maximum/cells )
			return Fail(error,"production palindrome tuple shape is invalid");
		std::uint64_t workingBytes=0u;
		if( !FireProductionCellPalindromeWorkingSetBytes(shape,request.componentCount,
			workingBytes,request.retainAcceptedGasMassDose)||
			workingBytes>(std::uint64_t(2u)<<30u) )
			return Fail(error,"production palindrome working set exceeds two GiB");
		if( request.conservativeValues.size()!=request.componentCount*cells||
			request.ambientValues.size()!=request.componentCount )
			return Fail(error,"production palindrome tuple shape is invalid");
		for( unsigned int axis=0u;axis<3u;++axis ) {
			if( request.frozenVelocityMPerS[axis].size()!=
				FireProductionProjectionFaceCount(shape,axis) )
				return Fail(error,"production palindrome velocity shape is invalid");
			const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
			const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
			if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
				upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
				((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
				return Fail(error,"production palindrome boundary pairing is invalid");
		}
		for( const float value : request.conservativeValues ) if( !std::isfinite(value) )
			return Fail(error,"production palindrome state is nonfinite");
		for( const std::vector<float>& velocity : request.frozenVelocityMPerS )
			for( const float value : velocity ) if( !std::isfinite(value) )
				return Fail(error,"production palindrome velocity is nonfinite");
		for( const float value : request.ambientValues ) if( !std::isfinite(value) )
			return Fail(error,"production palindrome ambient tuple is nonfinite");
		const float halfStep=0.5f*request.timeStepS;
		if( !ValidateAxisSchedule(request,0u,halfStep,error)||
			!ValidateAxisSchedule(request,1u,halfStep,error)||
			!ValidateAxisSchedule(request,2u,request.timeStepS,error) ) return false;
		return true;
	}

	bool RemapFireProductionCellPalindromeCPU(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* error )
	{
		result=FireProductionCellPalindromeResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,error) ) return false;
			std::vector<float> values=request.conservativeValues;
			const float halfStep=0.5f*request.timeStepS;
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			const float steps[]={halfStep,halfStep,request.timeStepS,halfStep,halfStep};
			for( unsigned int pass=0u;pass<5u;++pass ) {
				std::vector<float>* accepted=request.componentCount==9u&&
					request.retainAcceptedGasMassDose?
					&result.acceptedGasMassDoseKGPerM2[pass]:0;
				if( !ApplyAxis(request,axes[pass],steps[pass],values,accepted,error) )
					return false;
			}
			result.conservativeValues=std::move(values);
			result.executedSubmapCount=5u;
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			FailWithoutThrow(error,"production palindrome allocation failed");
			return false;
		}
	}

	bool BuildFireProductionPeriodicDualCellRequest(
		const FireProductionPeriodicDualMomentumRequest& request,
		unsigned int transportedComponent,
		FireProductionCellPalindromeRequest& dualRequest, std::string* error )
	{
		dualRequest=FireProductionCellPalindromeRequest();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( transportedComponent>2u||shape.nx<4u||shape.nx>1024u||
				shape.ny<4u||shape.ny>1024u||shape.nz<4u||shape.nz>1024u||
				!(shape.cellWidthM>0.0f)||request.timeStepS<0.0f||
				!std::isfinite(shape.cellWidthM)||!std::isfinite(request.timeStepS) )
				return Fail(error,"periodic dual cell request shape is invalid");
			for( const FireProductionProjectionBoundary boundary : request.boundary )
				if( boundary!=FireProductionProjectionPeriodic )
					return Fail(error,"periodic dual cell request requires periodic boundaries");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces||
					!PeriodicFaceSeamEqual(shape,request.beginningFaceDensity[axis],axis)||
					!PeriodicFaceSeamEqual(shape,request.beginningMomentum[axis],axis)||
					!PeriodicFaceSeamEqual(shape,request.frozenVelocityMPerS[axis],axis) )
					return Fail(error,"periodic dual cell request face shape or seam is invalid");
				for( const float density : request.beginningFaceDensity[axis] )
					if( !(density>0.0f)||!std::isfinite(density) )
						return Fail(error,"periodic dual cell request density is invalid");
				for( const float value : request.beginningMomentum[axis] )
					if( !std::isfinite(value) )
						return Fail(error,"periodic dual cell request momentum is nonfinite");
				for( const float value : request.frozenVelocityMPerS[axis] )
					if( !std::isfinite(value) )
						return Fail(error,"periodic dual cell request carrier is nonfinite");
			}
			const std::size_t cells=shape.CellCount();
			dualRequest.shape=shape;dualRequest.componentCount=2u;
			dualRequest.timeStepS=request.timeStepS;
			dualRequest.boundary.fill(FireProductionProjectionPeriodic);
			dualRequest.conservativeValues.resize(2u*cells);
			dualRequest.ambientValues.assign(2u,0.0f);
			for( std::size_t z=0u;z<shape.nz;++z )
				for( std::size_t y=0u;y<shape.ny;++y )
					for( std::size_t x=0u;x<shape.nx;++x ) {
						const std::size_t cell=CellIndex(shape,x,y,z);
						const std::size_t face=FaceIndex(shape,transportedComponent,x,y,z);
						dualRequest.conservativeValues[cell]=
							request.beginningFaceDensity[transportedComponent][face];
						dualRequest.conservativeValues[cells+cell]=
							request.beginningMomentum[transportedComponent][face];
					}
			for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
				dualRequest.frozenVelocityMPerS[sweepAxis]=
					PeriodicDualCarrier(request,transportedComponent,sweepAxis);
			if( !ValidateFireProductionCellPalindromeRequest(dualRequest,error) ) {
				dualRequest=FireProductionCellPalindromeRequest();return false;
			}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			dualRequest=FireProductionCellPalindromeRequest();
			FailWithoutThrow(error,"periodic dual cell request allocation failed");
			return false;
		}
	}

	bool FireProductionPeriodicDualMomentumResidentWorkingSetBytes(
		const FireProductionProjectionShape& shape, std::uint64_t& bytes )
	{
		bytes=0u;std::uint64_t nested=0u;
		if( !FireProductionCellPalindromeWorkingSetBytes(shape,2u,nested) ) return false;
		const std::uint64_t allFaces=
			static_cast<std::uint64_t>(FireProductionProjectionFaceCount(shape,0u))+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		if( !AddBytes(allFaces,15u*sizeof(float),nested) ) return false;
		bytes=nested;return true;
	}

	bool FireProductionDualMomentumResidentWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		const std::array<FireProductionProjectionBoundary,6>& boundary,
		std::uint64_t& bytes )
	{
		bytes=0u;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u ) return false;
		FireProductionDualMomentumRequest dimensions;dimensions.shape=shape;
		dimensions.boundary=boundary;
		for( unsigned int axis=0u;axis<3u;++axis ) {
			const FireProductionProjectionBoundary lower=boundary[2u*axis];
			const FireProductionProjectionBoundary upper=boundary[2u*axis+1u];
			if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
				upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
				((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
				return false;
		}
		const std::uint64_t xFaces=static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz;
		const std::uint64_t yFaces=static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz;
		const std::uint64_t zFaces=static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u);
		const std::uint64_t allFaces=xFaces+yFaces+zFaces;
		std::uint64_t maximumValues=0u,maximumFaces=0u,maximumCells=0u,total=0u;
		for( unsigned int component=0u;component<3u;++component )
			for( unsigned int sweep=0u;sweep<3u;++sweep ) {
				std::size_t length=0u,lines=0u;
				if( !DualLineDimensions(dimensions,component,sweep,length,lines) ) return false;
				const std::uint64_t lineLength=static_cast<std::uint64_t>(length);
				const std::uint64_t lineCount=static_cast<std::uint64_t>(lines);
				const std::uint64_t lineFaces=lineCount*(lineLength+1u);
				const std::uint64_t lineCells=lineCount*lineLength;
				const std::uint64_t ambient=AxisIsPeriodic(dimensions,sweep)?2u:2u*lineCount;
				if( !AddMetalValueBuffer(lineFaces,sizeof(float),total)||
					!AddMetalValueBuffer(ambient,sizeof(float),total)||
					!AddMetalValueBuffer(ambient,sizeof(float),total) ) return false;
				maximumValues=std::max(maximumValues,2u*lineCells);
				maximumFaces=std::max(maximumFaces,lineFaces);
				maximumCells=std::max(maximumCells,lineCells);
			}
		// Borrowed packed density/momentum and their atomically published successors.
		for( unsigned int buffer=0u;buffer<4u;++buffer )
			if( !AddMetalValueBuffer(allFaces,sizeof(float),total) ) return false;
		// Four tuple fields, one limiter, and two two-component face arrays.
		for( unsigned int buffer=0u;buffer<4u;++buffer )
			if( !AddMetalValueBuffer(maximumValues,sizeof(float),total) ) return false;
		if( !AddMetalValueBuffer(maximumCells,sizeof(float),total)||
			!AddMetalValueBuffer(2u*maximumFaces,sizeof(float),total)||
			!AddMetalValueBuffer(2u*maximumFaces,sizeof(float),total) ) return false;
		// Three wall-prescription parameters, two parameters for each of fifteen
		// submaps, and one seam parameter for every periodic component axis.
		for( unsigned int component=0u;component<3u;++component ) {
			if( !AddMetalBufferBytes(sizeof(std::uint32_t)*11u,total) ) return false;
			if( boundary[2u*component]==FireProductionProjectionPeriodic&&
				!AddMetalBufferBytes(sizeof(std::uint32_t)*5u,total) ) return false;
		}
		for( unsigned int pass=0u;pass<15u;++pass )
			if( !AddMetalBufferBytes(sizeof(std::uint32_t)*11u,total)||
				!AddMetalBufferBytes(sizeof(std::uint32_t)*6u+sizeof(float)*2u,total) )
				return false;
		bytes=total;return true;
	}

	bool FireProductionCompatibleDualMomentumResidentWorkingSetBytes(
		const FireProductionProjectionShape& shape, std::uint64_t& bytes )
	{
		bytes=0u;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u ) return false;
		const std::uint64_t xFaces=static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz;
		const std::uint64_t yFaces=static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz;
		const std::uint64_t zFaces=static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u);
		const std::uint64_t allFaces=xFaces+yFaces+zFaces;
		const std::uint64_t retainedMassDose=2u*xFaces+2u*yFaces+zFaces;
		std::uint64_t total=0u;
		// Borrowed beginning density/momentum, two ping-pong output pairs, and
		// the five accepted primal gas-mass-dose fields.
		for( unsigned int buffer=0u;buffer<6u;++buffer )
			if( !AddMetalValueBuffer(allFaces,sizeof(float),total) ) return false;
		if( !AddMetalValueBuffer(retainedMassDose,sizeof(float),total) ) return false;
		for( unsigned int pass=0u;pass<5u;++pass )
			if( !AddMetalBufferBytes(sizeof(std::uint32_t)*10u+sizeof(float),total) )
				return false;
		bytes=total;return true;
	}

	bool RemapFireProductionPeriodicDualMomentumCPU(
		const FireProductionPeriodicDualMomentumRequest& request,
		FireProductionPeriodicDualMomentumResult& result, std::string* error )
	{
		result=FireProductionPeriodicDualMomentumResult();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			for( const FireProductionProjectionBoundary boundary : request.boundary )
				if( boundary!=FireProductionProjectionPeriodic )
					return Fail(error,"periodic dual momentum requires periodic boundaries");
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS) )
				return Fail(error,"periodic dual momentum shape or schedule is invalid");
			const std::size_t cells=shape.CellCount();
			std::uint64_t nestedWorkingSetBytes=0u;
			if( !FireProductionCellPalindromeWorkingSetBytes(shape,2u,nestedWorkingSetBytes)||
				nestedWorkingSetBytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"periodic dual momentum working set exceeds two GiB");
			std::uint64_t combinedWorkingSetBytes=nestedWorkingSetBytes;
			const std::uint64_t allFaces=
				static_cast<std::uint64_t>(FireProductionProjectionFaceCount(shape,0u))+
				FireProductionProjectionFaceCount(shape,1u)+
				FireProductionProjectionFaceCount(shape,2u);
			// Nine caller arrays and six atomic-publication arrays coexist with the
			// largest nested two-channel palindrome.
			if( !AddBytes(allFaces,15u*sizeof(float),combinedWorkingSetBytes)||
				combinedWorkingSetBytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"periodic dual momentum combined working set exceeds two GiB");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces||
					!PeriodicFaceSeamEqual(shape,request.beginningFaceDensity[axis],axis)||
					!PeriodicFaceSeamEqual(shape,request.beginningMomentum[axis],axis)||
					!PeriodicFaceSeamEqual(shape,request.frozenVelocityMPerS[axis],axis) )
					return Fail(error,"periodic dual momentum face shape or seam is invalid");
				for( const float density : request.beginningFaceDensity[axis] )
					if( !(density>0.0f)||!std::isfinite(density) )
						return Fail(error,"periodic dual momentum density is invalid");
				for( const float momentum : request.beginningMomentum[axis] )
					if( !std::isfinite(momentum) )
						return Fail(error,"periodic dual momentum is nonfinite");
				for( const float velocity : request.frozenVelocityMPerS[axis] )
					if( !std::isfinite(velocity) )
						return Fail(error,"periodic dual carrier is nonfinite");
			}
			FireProductionPeriodicDualMomentumResult computed;
			for( unsigned int component=0u;component<3u;++component ) {
				FireProductionCellPalindromeRequest dual;
				dual.shape=shape;dual.componentCount=2u;dual.timeStepS=request.timeStepS;
				dual.boundary.fill(FireProductionProjectionPeriodic);
				dual.conservativeValues.resize(2u*cells);
				dual.ambientValues.assign(2u,0.0f);
				for( std::size_t z=0u;z<shape.nz;++z )
					for( std::size_t y=0u;y<shape.ny;++y )
						for( std::size_t x=0u;x<shape.nx;++x ) {
							const std::size_t cell=CellIndex(shape,x,y,z);
							const std::size_t face=FaceIndex(shape,component,x,y,z);
							dual.conservativeValues[cell]=request.beginningFaceDensity[component][face];
							dual.conservativeValues[cells+cell]=request.beginningMomentum[component][face];
						}
				for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
					dual.frozenVelocityMPerS[sweepAxis]=
						PeriodicDualCarrier(request,component,sweepAxis);
				FireProductionCellPalindromeResult dualResult;
				if( !RemapFireProductionCellPalindromeCPU(dual,dualResult,error) ) return false;
				computed.auxiliaryFaceDensity[component].assign(
					FireProductionProjectionFaceCount(shape,component),0.0f);
				computed.momentum[component].assign(
					FireProductionProjectionFaceCount(shape,component),0.0f);
				for( std::size_t z=0u;z<shape.nz;++z )
					for( std::size_t y=0u;y<shape.ny;++y )
						for( std::size_t x=0u;x<shape.nx;++x ) {
							const std::size_t cell=CellIndex(shape,x,y,z);
							const std::size_t face=FaceIndex(shape,component,x,y,z);
							computed.auxiliaryFaceDensity[component][face]=
								dualResult.conservativeValues[cell];
							computed.momentum[component][face]=
								dualResult.conservativeValues[cells+cell];
						}
				PublishPeriodicDualSeam(shape,component,
					computed.auxiliaryFaceDensity[component],computed.momentum[component],
					computed.canonicalSeamCopyCount);
			}
			computed.executedSubmapCount=15u;
			result=std::move(computed);
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionPeriodicDualMomentumResult();
			FailWithoutThrow(error,"periodic dual momentum allocation failed");
			return false;
		}
	}

	bool ValidateFireProductionDualMomentumRequest(
		const FireProductionDualMomentumRequest& request, std::string* error )
	{
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3) )
				return Fail(error,"dual momentum shape, schedule, or ambient density is invalid");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
					return Fail(error,"dual momentum boundary pairing is invalid");
				if( !AxisIsPeriodic(request,axis)&&AxisCoordinateExtent(shape,axis)<5u )
					return Fail(error,"dual momentum nonperiodic normal line is too short");
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces )
					return Fail(error,"dual momentum face shape is invalid");
				if( AxisIsPeriodic(request,axis)&&
					(!PeriodicFaceSeamEqual(shape,request.beginningFaceDensity[axis],axis)||
					 !PeriodicFaceSeamEqual(shape,request.beginningMomentum[axis],axis)||
					 !PeriodicFaceSeamEqual(shape,request.frozenVelocityMPerS[axis],axis)) )
					return Fail(error,"dual momentum periodic seam is invalid");
				for( const float density : request.beginningFaceDensity[axis] )
					if( !(density>0.0f)||!std::isfinite(density) )
						return Fail(error,"dual momentum density is invalid");
				for( const float momentum : request.beginningMomentum[axis] )
					if( !std::isfinite(momentum) )
						return Fail(error,"dual momentum is nonfinite");
				for( const float velocity : request.frozenVelocityMPerS[axis] )
					if( !std::isfinite(velocity) )
						return Fail(error,"dual momentum carrier is nonfinite");
			}
			std::uint64_t nestedWorkingSetBytes=0u;
			for( unsigned int component=0u;component<3u;++component )
				for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis ) {
					std::size_t length=0u,lines=0u;
					if( !DualLineDimensions(request,component,sweepAxis,length,lines) )
						return Fail(error,"dual momentum owned line is invalid");
					FireProductionRemapRequest resource;
					resource.lineLength=length;resource.lineCount=lines;resource.componentCount=2u;
					resource.lineSpecificAmbientValues=!AxisIsPeriodic(request,sweepAxis);
					std::uint64_t bytes=0u;
					if( !FireProductionRemapWorkingSetBytes(resource,bytes) )
						return Fail(error,"dual momentum nested working-set calculation failed");
					nestedWorkingSetBytes=std::max(nestedWorkingSetBytes,bytes);
				}
			const std::uint64_t allFaces=
				static_cast<std::uint64_t>(FireProductionProjectionFaceCount(shape,0u))+
				FireProductionProjectionFaceCount(shape,1u)+
				FireProductionProjectionFaceCount(shape,2u);
			std::uint64_t combinedWorkingSetBytes=nestedWorkingSetBytes;
			if( !AddBytes(allFaces,15u*sizeof(float),combinedWorkingSetBytes)||
				combinedWorkingSetBytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"dual momentum combined working set exceeds two GiB");
			const float axisTimeStep[]={0.5f*request.timeStepS,
				0.5f*request.timeStepS,request.timeStepS};
			for( unsigned int component=0u;component<3u;++component )
				for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis ) {
					FireProductionRemapRequest preflight;
					if( !BuildDualAxisRequest(request,component,sweepAxis,
						axisTimeStep[sweepAxis],request.beginningFaceDensity[component],
						request.beginningMomentum[component],preflight,error)||
						!ValidateFireProductionRemapRequest(preflight,error) ) return false;
				}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			FailWithoutThrow(error,"dual momentum validation allocation failed");return false;
		}
	}

	bool BuildFireProductionDualAxisRequest(
		const FireProductionDualMomentumRequest& request,
		unsigned int transportedComponent,
		unsigned int sweepAxis,
		float timeStepS,
		const std::vector<float>& density,
		const std::vector<float>& momentum,
		FireProductionRemapRequest& lineRequest,
		std::string* error )
	{
		lineRequest=FireProductionRemapRequest();
		if( transportedComponent>=3u||sweepAxis>=3u )
			return Fail(error,"dual momentum axis selection is invalid");
		try {
			if( !BuildDualAxisRequest(request,transportedComponent,sweepAxis,timeStepS,
				density,momentum,lineRequest,error) ) return false;
			if( !ValidateFireProductionRemapRequest(lineRequest,error) ) {
				lineRequest=FireProductionRemapRequest();return false;
			}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			lineRequest=FireProductionRemapRequest();
			FailWithoutThrow(error,"dual momentum axis packing allocation failed");
			return false;
		}
	}

	bool RemapFireProductionDualMomentumCPU(
		const FireProductionDualMomentumRequest& request,
		FireProductionDualMomentumResult& result, std::string* error )
	{
		result=FireProductionDualMomentumResult();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3) )
				return Fail(error,"dual momentum shape, schedule, or ambient density is invalid");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
					return Fail(error,"dual momentum boundary pairing is invalid");
				if( !AxisIsPeriodic(request,axis)&&AxisCoordinateExtent(shape,axis)<5u )
					return Fail(error,"dual momentum nonperiodic normal line is too short");
			}
			std::uint64_t nestedWorkingSetBytes=0u;
			for( unsigned int component=0u;component<3u;++component )
				for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis ) {
					std::size_t length=0u,lines=0u;
					if( !DualLineDimensions(request,component,sweepAxis,length,lines) )
						return Fail(error,"dual momentum owned line is invalid");
					FireProductionRemapRequest resource;
					resource.lineLength=length;resource.lineCount=lines;resource.componentCount=2u;
					resource.lineSpecificAmbientValues=!AxisIsPeriodic(request,sweepAxis);
					std::uint64_t bytes=0u;
					if( !FireProductionRemapWorkingSetBytes(resource,bytes) )
						return Fail(error,"dual momentum nested working-set calculation failed");
					nestedWorkingSetBytes=std::max(nestedWorkingSetBytes,bytes);
				}
			const std::uint64_t allFaces=
				static_cast<std::uint64_t>(FireProductionProjectionFaceCount(shape,0u))+
				FireProductionProjectionFaceCount(shape,1u)+
				FireProductionProjectionFaceCount(shape,2u);
			std::uint64_t combinedWorkingSetBytes=nestedWorkingSetBytes;
			if( !AddBytes(allFaces,15u*sizeof(float),combinedWorkingSetBytes)||
				combinedWorkingSetBytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"dual momentum combined working set exceeds two GiB");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces )
					return Fail(error,"dual momentum face shape is invalid");
				if( AxisIsPeriodic(request,axis)&&
					(!PeriodicFaceSeamEqual(shape,request.beginningFaceDensity[axis],axis)||
					 !PeriodicFaceSeamEqual(shape,request.beginningMomentum[axis],axis)||
					 !PeriodicFaceSeamEqual(shape,request.frozenVelocityMPerS[axis],axis)) )
					return Fail(error,"dual momentum periodic seam is invalid");
				for( const float density : request.beginningFaceDensity[axis] )
					if( !(density>0.0f)||!std::isfinite(density) )
						return Fail(error,"dual momentum density is invalid");
				for( const float momentum : request.beginningMomentum[axis] )
					if( !std::isfinite(momentum) )
						return Fail(error,"dual momentum is nonfinite");
				for( const float velocity : request.frozenVelocityMPerS[axis] )
					if( !std::isfinite(velocity) )
						return Fail(error,"dual momentum carrier is nonfinite");
			}
			FireProductionDualMomentumResult computed;
			computed.auxiliaryFaceDensity=request.beginningFaceDensity;
			computed.momentum=request.beginningMomentum;
			for( unsigned int component=0u;component<3u;++component ) {
				const std::size_t extent=AxisCoordinateExtent(shape,component);
				const std::size_t firstExtent=AxisCoordinateExtent(shape,(component+1u)%3u);
				const std::size_t secondExtent=AxisCoordinateExtent(shape,(component+2u)%3u);
				for( std::size_t second=0u;second<secondExtent;++second )
					for( std::size_t first=0u;first<firstExtent;++first ) {
						std::size_t x=0u,y=0u,z=0u;
						SetAxisCoordinate((component+1u)%3u,first,x,y,z);
						SetAxisCoordinate((component+2u)%3u,second,x,y,z);
						if( request.boundary[2u*component]==FireProductionProjectionWall ) {
							SetAxisCoordinate(component,0u,x,y,z);
							computed.momentum[component][FaceIndex(shape,component,x,y,z)]=0.0f;
						}
						if( request.boundary[2u*component+1u]==FireProductionProjectionWall ) {
							SetAxisCoordinate(component,extent,x,y,z);
							computed.momentum[component][FaceIndex(shape,component,x,y,z)]=0.0f;
						}
					}
			}
			const float axisTimeStep[]={0.5f*request.timeStepS,
				0.5f*request.timeStepS,request.timeStepS};
			for( unsigned int component=0u;component<3u;++component )
				for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis ) {
					FireProductionRemapRequest preflight;
					if( !BuildDualAxisRequest(request,component,sweepAxis,
						axisTimeStep[sweepAxis],computed.auxiliaryFaceDensity[component],
						computed.momentum[component],preflight,error)||
						!ValidateFireProductionRemapRequest(preflight,error) ) return false;
				}
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			for( unsigned int component=0u;component<3u;++component ) {
				for( const unsigned int sweepAxis : axes )
					if( !ApplyDualAxis(request,component,sweepAxis,axisTimeStep[sweepAxis],
						computed.auxiliaryFaceDensity[component],computed.momentum[component],
						error) ) return false;
				if( AxisIsPeriodic(request,component) ) PublishPeriodicDualSeam(shape,component,
					computed.auxiliaryFaceDensity[component],computed.momentum[component],
					computed.canonicalSeamCopyCount);
			}
			computed.executedSubmapCount=15u;
			result=std::move(computed);
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionDualMomentumResult();
			FailWithoutThrow(error,"dual momentum allocation failed");
			return false;
		}
	}

	bool ValidateFireProductionCompatibleDualMomentumRequest(
		const FireProductionDualMomentumRequest& request, std::string* error )
	{
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS) )
				return Fail(error,"compatible dual momentum shape or schedule is invalid");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
					return Fail(error,"compatible dual momentum boundary pairing is invalid");
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces )
					return Fail(error,"compatible dual momentum face shape is invalid");
				if( lower==FireProductionProjectionPeriodic&&
					(!PeriodicFaceSeamEqual(shape,request.beginningFaceDensity[axis],axis)||
					 !PeriodicFaceSeamEqual(shape,request.beginningMomentum[axis],axis)) )
					return Fail(error,"compatible dual momentum periodic seam is invalid");
				for( const float density : request.beginningFaceDensity[axis] )
					if( !(density>0.0f)||!std::isfinite(density) )
						return Fail(error,"compatible dual momentum density is invalid");
				for( const float momentum : request.beginningMomentum[axis] )
					if( !std::isfinite(momentum) )
						return Fail(error,"compatible dual momentum is nonfinite");
			}
			std::uint64_t bytes=0u;
			if( !FireProductionCompatibleDualMomentumResidentWorkingSetBytes(shape,bytes)||
				bytes>(std::uint64_t(2u)<<30u) )
				return Fail(error,"compatible dual momentum working set exceeds two GiB");
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			FailWithoutThrow(error,"compatible dual momentum validation allocation failed");
			return false;
		}
	}

	bool RemapFireProductionCompatibleDualMomentumCPU(
		const FireProductionDualMomentumRequest& request,
		const std::array<std::vector<float>,5>& acceptedGasMassDoseKGPerM2,
		FireProductionDualMomentumResult& result, std::string* error )
	{
		result=FireProductionDualMomentumResult();
		try {
			if( !ValidateFireProductionCompatibleDualMomentumRequest(request,error) ) return false;
			const FireProductionProjectionShape& shape=request.shape;
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			for( unsigned int pass=0u;pass<5u;++pass ) {
				const unsigned int axis=axes[pass];
				if( acceptedGasMassDoseKGPerM2[pass].size()!=
					FireProductionProjectionFaceCount(shape,axis) )
					return Fail(error,"compatible dual momentum mass-dose shape is invalid");
				for( const float value : acceptedGasMassDoseKGPerM2[pass] )
					if( !std::isfinite(value) )
						return Fail(error,"compatible dual momentum mass dose is nonfinite");
				if( AxisIsPeriodic(request,axis)&&!PeriodicFaceSeamEqual(shape,
					acceptedGasMassDoseKGPerM2[pass],axis) )
					return Fail(error,"compatible dual momentum mass-dose seam is invalid");
			}
			FireProductionDualMomentumResult computed;
			computed.auxiliaryFaceDensity=request.beginningFaceDensity;
			computed.momentum=request.beginningMomentum;
			for( unsigned int pass=0u;pass<5u;++pass ) {
				const unsigned int derivative=axes[pass];
				const std::vector<float>& massDose=acceptedGasMassDoseKGPerM2[pass];
				auto oldDensity=computed.auxiliaryFaceDensity;
				auto oldMomentum=computed.momentum;
				for( unsigned int component=0u;component<3u;++component ) {
					const std::size_t componentExtent=AxisCoordinateExtent(shape,component);
					const std::size_t normalCount=componentExtent+1u;
					const std::size_t firstCount=component==0u?shape.ny:shape.nx;
					const std::size_t secondCount=component==2u?shape.ny:shape.nz;
					const bool componentPeriodic=AxisIsPeriodic(request,component);
					const std::size_t activeNormalCount=componentPeriodic?componentExtent:normalCount;
					for( std::size_t second=0u;second<secondCount;++second )
						for( std::size_t first=0u;first<firstCount;++first )
							for( std::size_t normal=0u;normal<activeNormalCount;++normal ) {
								std::size_t x=0u,y=0u,z=0u;
								SetAxisCoordinate(component,normal,x,y,z);
								SetAxisCoordinate(component==0u?1u:0u,first,x,y,z);
								SetAxisCoordinate(component==2u?1u:2u,second,x,y,z);
								const std::size_t componentFace=FaceIndex(shape,component,x,y,z);
								const bool lowerWall=normal==0u&&request.boundary[2u*component]==
									FireProductionProjectionWall;
								const bool upperWall=normal+1u==normalCount&&
									request.boundary[2u*component+1u]==FireProductionProjectionWall;
								auto velocityAt=[&](std::size_t vx,std::size_t vy,std::size_t vz) {
									const std::size_t face=FaceIndex(shape,component,vx,vy,vz);
									return oldMomentum[component][face]/oldDensity[component][face];
								};
								auto massDoseAt=[&](std::size_t fx,std::size_t fy,std::size_t fz) {
									return massDose[FaceIndex(shape,derivative,fx,fy,fz)];
								};
								float upperMass=0.0f,lowerMass=0.0f,upperVelocity=0.0f,
									lowerVelocity=0.0f;
								if( derivative==component ) {
									const std::size_t previous=componentPeriodic?
										(normal==0u?componentExtent-1u:normal-1u):
										(normal==0u?0u:normal-1u);
									const std::size_t next=componentPeriodic?
										(normal+1u==componentExtent?0u:normal+1u):
										(normal+1u<normalCount?normal+1u:normal);
									std::size_t px=x,py=y,pz=z,nx=x,ny=y,nz=z;
									SetAxisCoordinate(component,previous,px,py,pz);
									SetAxisCoordinate(component,next,nx,ny,nz);
									lowerMass=0.5f*(massDoseAt(px,py,pz)+massDoseAt(x,y,z));
									upperMass=0.5f*(massDoseAt(x,y,z)+massDoseAt(nx,ny,nz));
									lowerVelocity=0.5f*(velocityAt(px,py,pz)+velocityAt(x,y,z));
									upperVelocity=0.5f*(velocityAt(x,y,z)+velocityAt(nx,ny,nz));
								} else {
									const std::size_t derivativeExtent=AxisCoordinateExtent(shape,derivative);
									const std::size_t position=AxisCoordinate(derivative,x,y,z);
									const std::size_t componentLower=componentPeriodic?
										(normal==0u?componentExtent-1u:normal-1u):
										(normal==0u?0u:normal-1u);
									const std::size_t componentUpper=componentPeriodic?normal:
										std::min(normal,componentExtent-1u);
									auto restricted=[&](std::size_t boundary) {
										std::size_t lx=x,ly=y,lz=z,ux=x,uy=y,uz=z;
										SetAxisCoordinate(component,componentLower,lx,ly,lz);
										SetAxisCoordinate(component,componentUpper,ux,uy,uz);
										SetAxisCoordinate(derivative,boundary,lx,ly,lz);
										SetAxisCoordinate(derivative,boundary,ux,uy,uz);
										const float restricted=0.5f*(massDoseAt(lx,ly,lz)+
											massDoseAt(ux,uy,uz));
										// A nonperiodic component-normal face contains one interior
										// half and one fixed ambient/ghost half.  Only the interior
										// half receives the transverse scalar dose.
										return !componentPeriodic&&(normal==0u||normal+1u==normalCount)?
											0.5f*restricted:restricted;
									};
									lowerMass=restricted(position);upperMass=restricted(position+1u);
									if( position==0u&&request.boundary[2u*derivative]==
										FireProductionProjectionWall ) lowerMass=0.0f;
									if( position+1u==derivativeExtent&&request.boundary[2u*derivative+1u]==
										FireProductionProjectionWall ) upperMass=0.0f;
									const bool derivativePeriodic=AxisIsPeriodic(request,derivative);
									const std::size_t previous=derivativePeriodic?
										(position==0u?derivativeExtent-1u:position-1u):
										(position==0u?0u:position-1u);
									const std::size_t next=derivativePeriodic?
										(position+1u==derivativeExtent?0u:position+1u):
										std::min(position+1u,derivativeExtent-1u);
									std::size_t px=x,py=y,pz=z,nx=x,ny=y,nz=z;
									SetAxisCoordinate(derivative,previous,px,py,pz);
									SetAxisCoordinate(derivative,next,nx,ny,nz);
									lowerVelocity=0.5f*(velocityAt(px,py,pz)+velocityAt(x,y,z));
									upperVelocity=0.5f*(velocityAt(x,y,z)+velocityAt(nx,ny,nz));
								}
								const float density=oldDensity[component][componentFace]-
									(upperMass-lowerMass)/shape.cellWidthM;
								const float momentum=(lowerWall||upperWall)?0.0f:
									oldMomentum[component][componentFace]-
									(upperMass*upperVelocity-lowerMass*lowerVelocity)/shape.cellWidthM;
								if( !(density>0.0f)||!std::isfinite(density)||!std::isfinite(momentum) )
									return Fail(error,"compatible dual momentum update is inadmissible");
								computed.auxiliaryFaceDensity[component][componentFace]=density;
								computed.momentum[component][componentFace]=momentum;
							}
					if( componentPeriodic ) PublishPeriodicDualSeam(shape,component,
						computed.auxiliaryFaceDensity[component],computed.momentum[component],
						computed.canonicalSeamCopyCount);
				}
			}
			computed.executedSubmapCount=15u;
			result=std::move(computed);
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionDualMomentumResult();
			FailWithoutThrow(error,"compatible dual momentum allocation failed");
			return false;
		}
	}

	bool QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& workingSetBytes,
		std::string* error )
	{
		workingSetBytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return Fail(error,
				"physical scalar-flux prerequisite query shape is invalid");
		const std::uint64_t cells=shape.CellCount(),allFaces=
			FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u),boundaryFaces=2u*(
			shape.ny*shape.nz+shape.nx*shape.nz+shape.nx*shape.ny);
		if(!AddBytes(cells,12u*sizeof(float),workingSetBytes)||
			!AddBytes(allFaces,11u*sizeof(float),workingSetBytes)||
			!AddBytes(boundaryFaces,sizeof(unsigned char),workingSetBytes)){
			workingSetBytes=0u;return Fail(error,
				"physical scalar-flux prerequisite query exceeds uint64 capacity");
		}
		if(error)error->clear();return true;
	}

	bool BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		const FireProductionScalarPhysicalFluxPrerequisiteRequest& request,
		FireProductionScalarPhysicalFluxPrerequisiteResult& result, std::string* error )
	{
		result=FireProductionScalarPhysicalFluxPrerequisiteResult();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			const RISE::FireCertifiedNullspace& projection=record.NonadvectiveFluxProjection();
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!std::isfinite(1.0f/shape.cellWidthM)||
				!record.IsValid()||projection.stateDimension!=8u||projection.nullity==0u||
				projection.nullity>8u||projection.orthonormalBasis.size()!=8u*projection.nullity)
				return Fail(error,"physical scalar-flux prerequisite shape or record is invalid");
			std::uint64_t workingSetBytes=0u;
			if(!QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
				shape,workingSetBytes,error))return false;
			if(workingSetBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"physical scalar-flux prerequisite working set exceeds two GiB");
			const std::size_t cells=shape.CellCount();
			if(request.conservativeValues.size()!=9u*cells||request.temperatureK.size()!=cells||
				request.diffusivityM2PerS.size()!=cells||request.conductivityWPerMK.size()!=cells||
				!std::isfinite(request.ambientTemperatureK)||
				static_cast<double>(request.ambientTemperatureK)<record.TemperatureMinK()||
				static_cast<double>(request.ambientTemperatureK)>record.TemperatureMaxK())
				return Fail(error,"physical scalar-flux prerequisite cell input is invalid");
			auto sideCount=[&](unsigned int side){return side<2u?shape.ny*shape.nz:
				(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);};
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis],
					upper=request.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))||request.frozenVelocityMPerS[axis].size()!=
					FireProductionProjectionFaceCount(shape,axis))return Fail(error,
						"physical scalar-flux prerequisite boundary or velocity shape is invalid");
				for(float value:request.frozenVelocityMPerS[axis])if(!std::isfinite(value))
					return Fail(error,"physical scalar-flux prerequisite velocity is nonfinite");
				if(lower==FireProductionProjectionPeriodic&&
					!PeriodicFaceSeamBitEqual(shape,request.frozenVelocityMPerS[axis],axis))
					return Fail(error,"physical scalar-flux prerequisite periodic velocity seam is invalid");
			}
			for(unsigned int side=0u;side<6u;++side){
				if(request.pressureOpenInflow[side].size()!=sideCount(side))return Fail(error,
					"physical scalar-flux prerequisite inflow shape is invalid");
				for(unsigned char value:request.pressureOpenInflow[side])if(value>1u||
					(request.boundary[side]!=FireProductionProjectionPressureOpen&&value!=0u))
					return Fail(error,"physical scalar-flux prerequisite inflow value is invalid");
			}
			for(float value:request.conservativeValues)if(!std::isfinite(value))return Fail(error,
				"physical scalar-flux prerequisite conservative value is nonfinite");
			for(std::size_t cell=0u;cell<cells;++cell)if(
				!std::isfinite(request.temperatureK[cell])||
				static_cast<double>(request.temperatureK[cell])<record.TemperatureMinK()||
				static_cast<double>(request.temperatureK[cell])>record.TemperatureMaxK()||
				request.diffusivityM2PerS[cell]<0.0f||
				!std::isfinite(request.diffusivityM2PerS[cell])||request.conductivityWPerMK[cell]<0.0f||
				!std::isfinite(request.conductivityWPerMK[cell]))return Fail(error,
					"physical scalar-flux prerequisite transport value is invalid");
			for(float value:request.ambient)if(!std::isfinite(value))return Fail(error,
				"physical scalar-flux prerequisite ambient value is nonfinite");

			auto enthalpy=[&](std::size_t species,float temperature,float& value){
				const RISE::FireThermochemistrySpecies* item=record.FindSpecies(
					record.SpeciesOrder()[species].c_str());
				if(!item||item->segments.empty())return false;
				const RISE::FireThermochemistrySegment* selected=&item->segments.front();
				for(const RISE::FireThermochemistrySegment& segment:item->segments)if(
					temperature>=static_cast<float>(segment.temperatureMinK)&&
					(temperature<static_cast<float>(segment.temperatureMaxK)||
					 &segment==&item->segments.back()))selected=&segment;
				const double t=static_cast<double>(temperature),inverse=1.0/t,
					logT=std::log(t),t2=t*t,t3=t2*t,t4=t3*t,t5=t4*t;
				const double* a=selected->coefficients;
				const double primitive=-a[0]*inverse+a[1]*logT+a[2]*t+a[3]*t2/2.0+
					a[4]*t3/3.0+a[5]*t4/4.0+a[6]*t5/5.0;
				value=static_cast<float>(8314.46261815324*primitive/
					item->molecularWeightKGPerKMol+selected->sensibleEnthalpyOffsetJPerKG);
				return std::isfinite(value);
			};
			FireProductionScalarPhysicalFluxPrerequisiteResult computed;
			computed.shape=shape;computed.boundary=request.boundary;
			computed.packedFaceOffset[0]=0u;
			computed.packedFaceOffset[1]=FireProductionProjectionFaceCount(shape,0u);
			computed.packedFaceOffset[2]=computed.packedFaceOffset[1]+
				FireProductionProjectionFaceCount(shape,1u);
			const std::size_t allFaces=computed.packedFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			computed.physicalMassFluxKGPerM2S.assign(8u*allFaces,0.0f);
			computed.physicalEnergyFluxWPerM2.assign(allFaces,0.0f);
			for(unsigned int axis=0u;axis<3u;++axis)
				computed.physicalGasFluxKGPerM2S[axis].assign(
					FireProductionProjectionFaceCount(shape,axis),0.0f);
			computed.methaneRecordId=record.RecordId();
			auto sideIndex=[&](unsigned int side,std::size_t x,std::size_t y,std::size_t z){
				return side<2u?z*shape.ny+y:(side<4u?z*shape.nx+x:y*shape.nx+x);};
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t xEnd=shape.nx+(axis==0u?1u:0u),
					yEnd=shape.ny+(axis==1u?1u:0u),zEnd=shape.nz+(axis==2u?1u:0u),
					extent=AxisCoordinateExtent(shape,axis);
				for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
					for(std::size_t x=0u;x<xEnd;++x){
						std::size_t normal=AxisCoordinate(axis,x,y,z);
						const std::size_t face=FaceIndex(shape,axis,x,y,z),
							packed=computed.packedFaceOffset[axis]+face;
						bool boundaryFace=normal==0u||normal==extent;
						if(request.boundary[2u*axis]==FireProductionProjectionPeriodic){
							boundaryFace=false;if(normal==extent)normal=0u;
						}
						std::size_t lx=x,ly=y,lz=z,rx=x,ry=y,rz=z,left=0u,right=0u;
						float leftTemperature=0.0f,rightTemperature=0.0f,rhoD=0.0f,
							conductivity=0.0f,distance=shape.cellWidthM;
						bool leftAmbient=false,rightAmbient=false;
						if(boundaryFace){
							const bool upper=normal==extent;const unsigned int side=2u*axis+(upper?1u:0u);
							if(request.boundary[side]==FireProductionProjectionWall||
								request.pressureOpenInflow[side][sideIndex(side,x,y,z)]==0u)continue;
							std::size_t ix=x,iy=y,iz=z;SetAxisCoordinate(axis,upper?extent-1u:0u,ix,iy,iz);
							const std::size_t interior=CellIndex(shape,ix,iy,iz);left=right=interior;
							leftAmbient=!upper;rightAmbient=upper;leftTemperature=leftAmbient?
								request.ambientTemperatureK:request.temperatureK[interior];
							rightTemperature=rightAmbient?request.ambientTemperatureK:
								request.temperatureK[interior];
							float total=0.0f;for(std::size_t species=0u;species<7u;++species)
								total+=request.conservativeValues[(1u+species)*cells+interior];
							if(!(total>0.0f)||!std::isfinite(total))return Fail(error,
								"physical scalar-flux prerequisite boundary mass is invalid");
							rhoD=total*request.diffusivityM2PerS[interior];
							conductivity=request.conductivityWPerMK[interior];distance=0.5f*shape.cellWidthM;
						}else{
							const std::size_t leftNormal=normal==0u?extent-1u:normal-1u,
								rightNormal=normal==extent?0u:normal;
							SetAxisCoordinate(axis,leftNormal,lx,ly,lz);SetAxisCoordinate(axis,rightNormal,rx,ry,rz);
							left=CellIndex(shape,lx,ly,lz);right=CellIndex(shape,rx,ry,rz);
							leftTemperature=request.temperatureK[left];rightTemperature=request.temperatureK[right];
							float totalLeft=0.0f,totalRight=0.0f;for(std::size_t species=0u;species<7u;++species){
								totalLeft+=request.conservativeValues[(1u+species)*cells+left];
								totalRight+=request.conservativeValues[(1u+species)*cells+right];}
							if(!(totalLeft>0.0f)||!(totalRight>0.0f))return Fail(error,
								"physical scalar-flux prerequisite interior mass is invalid");
							auto harmonic=[](float a,float b){return a>0.0f&&b>0.0f?2.0f*a*b/(a+b):0.0f;};
							rhoD=harmonic(totalLeft*request.diffusivityM2PerS[left],
								totalRight*request.diffusivityM2PerS[right]);
							conductivity=harmonic(request.conductivityWPerMK[left],
								request.conductivityWPerMK[right]);
						}
						float totalLeft=0.0f,totalRight=0.0f;
						for(std::size_t species=0u;species<7u;++species){
							totalLeft+=leftAmbient?request.ambient[1u+species]:
								request.conservativeValues[(1u+species)*cells+left];
							totalRight+=rightAmbient?request.ambient[1u+species]:
								request.conservativeValues[(1u+species)*cells+right];}
						if(!(totalLeft>0.0f)||!(totalRight>0.0f))return Fail(error,
							"physical scalar-flux prerequisite face mass is invalid");
						std::array<float,8> raw={{}},projected={{}};
						for(std::size_t component=0u;component<8u;++component){
							const float ql=leftAmbient?request.ambient[component]:
								request.conservativeValues[component*cells+left];
							const float qr=rightAmbient?request.ambient[component]:
								request.conservativeValues[component*cells+right];
							raw[component]=-rhoD*(qr/totalRight-ql/totalLeft)/distance;
						}
						std::array<double,8> raw64={{}},projected64={{}};
						double rawScale=0.0;
						for(std::size_t component=0u;component<8u;++component){
							raw64[component]=static_cast<double>(raw[component]);
							rawScale+=std::fabs(raw64[component]);
						}
						if(!projection.Project(raw64.data(),raw64.size(),projected64.data(),
							projected64.size(),error))return false;
						for(std::size_t component=0u;component<8u;++component){
							projected[component]=static_cast<float>(projected64[component]);
							if(!std::isfinite(projected[component]))return Fail(error,
								"physical scalar-flux prerequisite projection cast is nonfinite");
						}
						const double epsilon=std::numeric_limits<double>::epsilon();
						const double gamma=(8.0*epsilon)/(1.0-8.0*epsilon);
						const double projectionFactor=record.AcceptedStateFeasibilityEnvelope().
							nullspaceProjectionFactorEpsilon64;
						if(!(projectionFactor>0.0)||!std::isfinite(projectionFactor))return Fail(error,
							"physical scalar-flux prerequisite projection certificate is invalid");
						for(std::size_t row=0u;row<projection.constraintRows;++row){
								double residual=0.0,referenceResidual=0.0,castBound=0.0,
									arithmeticScale=0.0,referenceScale=0.0,constraintNorm=0.0;
								for(std::size_t component=0u;component<8u;++component){
									const double coefficient=projection.constraintMatrix[row*8u+component];
									constraintNorm+=std::fabs(coefficient);
								residual+=coefficient*static_cast<double>(projected[component]);
								referenceResidual+=coefficient*projected64[component];
								referenceScale+=std::fabs(coefficient*projected64[component]);
								castBound+=std::fabs(coefficient)*std::fabs(
									static_cast<double>(projected[component])-projected64[component]);
								arithmeticScale+=std::fabs(coefficient*
									static_cast<double>(projected[component]));
							}
								// The record's 1040-eps64 certificate covers the fixed N(N^T raw)
								// operation.  Its scale must therefore come from raw, not the possibly
								// tiny projected output.  Propagate that componentwise envelope through
								// this constraint row, then separately cover the final row reduction.
								const double referenceBound=projectionFactor*epsilon*
									std::max(1.0,rawScale)*std::max(1.0,constraintNorm)+
									gamma*referenceScale;
								if(!std::isfinite(rawScale)||!std::isfinite(constraintNorm)||
									!std::isfinite(referenceResidual)||!std::isfinite(referenceBound)||
								std::fabs(referenceResidual)>referenceBound)return Fail(error,
									"physical scalar-flux prerequisite fp64 reference exceeds its certificate");
							const double bound=referenceBound+castBound+
								gamma*arithmeticScale;
							computed.maximumFP64ReferenceResidualKGPerM2S=std::max(
								computed.maximumFP64ReferenceResidualKGPerM2S,std::fabs(referenceResidual));
							computed.fp64ReferenceForwardErrorBoundKGPerM2S=std::max(
								computed.fp64ReferenceForwardErrorBoundKGPerM2S,referenceBound);
							computed.maximumConstraintResidualKGPerM2S=std::max(
								computed.maximumConstraintResidualKGPerM2S,std::fabs(residual));
							computed.constraintForwardErrorBoundKGPerM2S=std::max(
								computed.constraintForwardErrorBoundKGPerM2S,bound);
							if(!std::isfinite(residual)||!std::isfinite(bound)||std::fabs(residual)>bound)
								return Fail(error,"physical scalar-flux prerequisite fp64 identity check failed");
						}
						const float faceTemperature=0.5f*(leftTemperature+rightTemperature);
						float energy=0.0f,gas=0.0f;
						for(std::size_t component=0u;component<8u;++component){
							if(!std::isfinite(projected[component]))return Fail(error,
								"physical scalar-flux prerequisite mass flux is nonfinite");
							computed.physicalMassFluxKGPerM2S[component*allFaces+packed]=projected[component];
							if(component>=1u&&component<=6u)gas+=projected[component];
						}
						for(std::size_t species=0u;species<7u;++species){float h=0.0f;
							if(!enthalpy(species,faceTemperature,h))return Fail(error,
								"physical scalar-flux prerequisite enthalpy is invalid");
							energy+=h*projected[1u+species];}
						energy-=conductivity*(rightTemperature-leftTemperature)/distance;
						if(!std::isfinite(energy)||!std::isfinite(gas))return Fail(error,
							"physical scalar-flux prerequisite aggregate flux is nonfinite");
						computed.physicalEnergyFluxWPerM2[packed]=energy;
						computed.physicalGasFluxKGPerM2S[axis][face]=gas;
					}
			}
			computed.fp64ReferenceIdentityVerified=true;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarPhysicalFluxPrerequisiteResult();
			FailWithoutThrow(error,"physical scalar-flux prerequisite allocation failed");return false;}
	}

	bool QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& payloadBytes,
		std::string* error )
	{
		payloadBytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return Fail(error,
				"scalar Heun flux-stage query shape is invalid");
		const std::uint64_t allFaces=FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		if(!AddBytes(allFaces,30u*sizeof(float),payloadBytes)){
			payloadBytes=0u;return Fail(error,
				"scalar Heun flux-stage query exceeds uint64 capacity");
		}
		if(error)error->clear();return true;
	}

	namespace
	{
		bool ValidScalarHeunFluxStage(
			const FireProductionScalarHeunFluxStage& stage,std::string* error )
		{
			const FireProductionScalarFCTFluxPair& pair=stage.compositeFluxPair;
			const FireProductionProjectionShape& shape=pair.shape;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!(pair.timeStepS>0.0f)||
				!std::isfinite(pair.timeStepS)||stage.attemptIdentity==0u||
				!ValidHeunFluxRole(stage.role)||stage.stageInputIdentity==0u||
				stage.sharedFCTContractIdentity==0u||stage.fctRequestIdentity==0u||
				stage.frozenVelocityIdentity==0u||
				!stage.fp64ReferenceIdentityVerified||
				!std::isfinite(stage.physicalConstraintForwardErrorBoundKGPerM2S)||
				stage.physicalConstraintForwardErrorBoundKGPerM2S<0.0||
				!std::isfinite(stage.physicalGasAveragingForwardErrorBoundKGPerM2S)||
				stage.physicalGasAveragingForwardErrorBoundKGPerM2S<0.0||
				((stage.role==FireProductionScalarHeunFluxRole::HeunAverage)!=
					(stage.parentCompositionIdentity[0]!=0u&&
					 stage.parentCompositionIdentity[1]!=0u))||
				stage.methaneRecordId!=RISE::FireSimulationMethaneRecord::PhysicalV1().RecordId())
				return Fail(error,"scalar Heun flux-stage identity is invalid");
			std::array<std::size_t,3> expectedOffset={{0u,
				FireProductionProjectionFaceCount(shape,0u),0u}};
			expectedOffset[2]=expectedOffset[1]+FireProductionProjectionFaceCount(shape,1u);
			const std::size_t allFaces=expectedOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			std::uint64_t workingSetBytes=0u;
			if(!QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
				shape,workingSetBytes,error))return false;
			if(workingSetBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"scalar Heun flux-stage working set exceeds two GiB");
			if(pair.packedFaceOffset!=expectedOffset||pair.lowFlux.size()!=9u*allFaces||
				pair.fluxDelta.size()!=9u*allFaces||
				stage.physicalMassFluxKGPerM2S.size()!=8u*allFaces||
				stage.physicalEnergyFluxWPerM2.size()!=allFaces)
				return Fail(error,"scalar Heun flux-stage payload shape is invalid");
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				const FireProductionProjectionBoundary lower=pair.boundary[2u*axis],
					upper=pair.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))||
					stage.physicalGasFluxKGPerM2S[axis].size()!=faces||
					stage.advectiveGasLowFluxKGPerM2S[axis].size()!=faces||
					stage.advectiveGasFluxDeltaKGPerM2S[axis].size()!=faces)
					return Fail(error,"scalar Heun flux-stage boundary or gas shape is invalid");
			}
			for(const float value:pair.lowFlux)if(!std::isfinite(value))
				return Fail(error,"scalar Heun composite low flux is nonfinite");
			for(const float value:pair.fluxDelta)if(!std::isfinite(value))
				return Fail(error,"scalar Heun composite flux delta is nonfinite");
			for(const float value:stage.physicalMassFluxKGPerM2S)if(!std::isfinite(value))
				return Fail(error,"scalar Heun physical mass flux is nonfinite");
			for(const float value:stage.physicalEnergyFluxWPerM2)if(!std::isfinite(value))
				return Fail(error,"scalar Heun physical energy flux is nonfinite");
			for(unsigned int axis=0u;axis<3u;++axis){
				for(const float value:stage.physicalGasFluxKGPerM2S[axis])if(!std::isfinite(value))
					return Fail(error,"scalar Heun physical gas flux is nonfinite");
				for(const float value:stage.advectiveGasLowFluxKGPerM2S[axis])if(!std::isfinite(value))
					return Fail(error,"scalar Heun advective gas low flux is nonfinite");
				for(const float value:stage.advectiveGasFluxDeltaKGPerM2S[axis])if(!std::isfinite(value))
					return Fail(error,"scalar Heun advective gas delta is nonfinite");
				if(pair.boundary[2u*axis]==FireProductionProjectionPeriodic){
					const std::size_t extent=AxisCoordinateExtent(shape,axis),
						xEnd=axis==0u?1u:shape.nx,yEnd=axis==1u?1u:shape.ny,
						zEnd=axis==2u?1u:shape.nz;
					for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
						for(std::size_t x=0u;x<xEnd;++x){
							std::size_t hx=x,hy=y,hz=z;SetAxisCoordinate(axis,extent,hx,hy,hz);
							const std::size_t lowPacked=expectedOffset[axis]+
								FaceIndex(shape,axis,x,y,z),highPacked=expectedOffset[axis]+
								FaceIndex(shape,axis,hx,hy,hz),lowFace=lowPacked-expectedOffset[axis],
								highFace=highPacked-expectedOffset[axis];
							for(std::size_t component=0u;component<9u;++component)if(
								!SameFloatBits(pair.lowFlux[component*allFaces+lowPacked],
									pair.lowFlux[component*allFaces+highPacked])||
								!SameFloatBits(pair.fluxDelta[component*allFaces+lowPacked],
									pair.fluxDelta[component*allFaces+highPacked]))return Fail(error,
									"scalar Heun composite periodic seam differs");
							for(std::size_t component=0u;component<8u;++component)if(!SameFloatBits(
								stage.physicalMassFluxKGPerM2S[component*allFaces+lowPacked],
								stage.physicalMassFluxKGPerM2S[component*allFaces+highPacked]))
								return Fail(error,"scalar Heun physical periodic seam differs");
							if(!SameFloatBits(stage.physicalEnergyFluxWPerM2[lowPacked],
								stage.physicalEnergyFluxWPerM2[highPacked])||
								!SameFloatBits(stage.physicalGasFluxKGPerM2S[axis][lowFace],
									stage.physicalGasFluxKGPerM2S[axis][highFace])||
								!SameFloatBits(stage.advectiveGasLowFluxKGPerM2S[axis][lowFace],
									stage.advectiveGasLowFluxKGPerM2S[axis][highFace])||
								!SameFloatBits(stage.advectiveGasFluxDeltaKGPerM2S[axis][lowFace],
									stage.advectiveGasFluxDeltaKGPerM2S[axis][highFace]))return Fail(error,
									"scalar Heun gas periodic seam differs");
						}
				}
			}
			const RISE::FireCertifiedNullspace& projection=
				RISE::FireSimulationMethaneRecord::PhysicalV1().NonadvectiveFluxProjection();
			for(std::size_t face=0u;face<allFaces;++face)
				for(std::size_t row=0u;row<projection.constraintRows;++row){
					double residual=0.0;for(std::size_t component=0u;component<8u;++component)
						residual+=projection.constraintMatrix[row*8u+component]*
							static_cast<double>(stage.physicalMassFluxKGPerM2S[
								component*allFaces+face]);
					if(!std::isfinite(residual)||std::fabs(residual)>
						stage.physicalConstraintForwardErrorBoundKGPerM2S)return Fail(error,
							"scalar Heun physical affine certificate differs");
				}
			for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
				FireProductionProjectionFaceCount(shape,axis);for(std::size_t face=0u;
					face<faces;++face){const std::size_t packed=expectedOffset[axis]+face;
					float gas=0.0f;for(std::size_t component=1u;component<=6u;++component)
						gas+=stage.physicalMassFluxKGPerM2S[component*allFaces+packed];
					if(stage.role==FireProductionScalarHeunFluxRole::HeunAverage){
						const double residual=std::fabs(static_cast<double>(gas)-
							static_cast<double>(stage.physicalGasFluxKGPerM2S[axis][face]));
						if(!std::isfinite(residual)||residual>
							stage.physicalGasAveragingForwardErrorBoundKGPerM2S)return Fail(error,
								"scalar Heun averaged J_g exceeds its forward bound");
					}else if(!SameFloatBits(gas,stage.physicalGasFluxKGPerM2S[axis][face]))
						return Fail(error,
							"scalar Heun J_g differs from retained f_N bytes");
				}
			}
			if(stage.compositionIdentity!=HeunFluxStageIdentity(stage))return Fail(error,
				"scalar Heun flux-stage content identity differs");
			return true;
		}
	}

	bool ComposeFireProductionScalarHeunFluxStageCPU(
		const std::uint64_t attemptIdentity,const FireProductionScalarHeunFluxRole role,
		const FireProductionScalarFCTRequest& advectiveRequest,
		const FireProductionScalarPhysicalFluxPrerequisiteRequest& physicalRequest,
		FireProductionScalarHeunFluxStage& result,std::string* error )
	{
		result=FireProductionScalarHeunFluxStage();
		try {
			const FireProductionProjectionShape& shape=advectiveRequest.shape;
			std::uint64_t workingSetBytes=0u;
			if(!QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
				shape,workingSetBytes,error)||!AddBytes(
				FireProductionProjectionFaceCount(shape,0u)+
				FireProductionProjectionFaceCount(shape,1u)+
				FireProductionProjectionFaceCount(shape,2u),28u*sizeof(float),workingSetBytes))
				return Fail(error,"scalar Heun flux-stage live set exceeds uint64 capacity");
			if(workingSetBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"scalar Heun flux-stage working set exceeds two GiB");
			if(attemptIdentity==0u||(role!=FireProductionScalarHeunFluxRole::R0&&
				role!=FireProductionScalarHeunFluxRole::R1)||
				physicalRequest.shape.nx!=shape.nx||physicalRequest.shape.ny!=shape.ny||
				physicalRequest.shape.nz!=shape.nz||
				physicalRequest.shape.cellWidthM!=shape.cellWidthM||
				physicalRequest.boundary!=advectiveRequest.boundary||
				!SameFloatVectorBits(physicalRequest.conservativeValues,
					advectiveRequest.beginning)||
				physicalRequest.pressureOpenInflow!=advectiveRequest.pressureOpenInflow)
				return Fail(error,"scalar Heun raw stage identity is invalid");
			for(unsigned int axis=0u;axis<3u;++axis)if(!SameFloatVectorBits(
				physicalRequest.frozenVelocityMPerS[axis],
				advectiveRequest.frozenVelocityMPerS[axis]))return Fail(error,
					"scalar Heun raw stage velocity differs");
			for(std::size_t component=0u;component<9u;++component)if(!SameFloatBits(
				physicalRequest.ambient[component],advectiveRequest.ambient[component]))
				return Fail(error,"scalar Heun raw stage ambient differs");
			FireProductionScalarFCTFluxPair advectiveFluxPair;
			if(!BuildFireProductionScalarFCTFluxPairCPU(
				advectiveRequest,advectiveFluxPair,error))return false;
			FireProductionScalarPhysicalFluxPrerequisiteResult physicalFlux;
			if(!BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
				physicalRequest,physicalFlux,error))return false;
			std::array<std::size_t,3> expectedOffset={{0u,
				FireProductionProjectionFaceCount(shape,0u),0u}};
			expectedOffset[2]=expectedOffset[1]+FireProductionProjectionFaceCount(shape,1u);
			const std::size_t allFaces=expectedOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			if(!(advectiveFluxPair.timeStepS>0.0f)||!std::isfinite(
				advectiveFluxPair.timeStepS)||advectiveFluxPair.packedFaceOffset!=expectedOffset||
				advectiveFluxPair.lowFlux.size()!=9u*allFaces||
				advectiveFluxPair.fluxDelta.size()!=9u*allFaces||
				physicalFlux.shape.nx!=shape.nx||physicalFlux.shape.ny!=shape.ny||
				physicalFlux.shape.nz!=shape.nz||physicalFlux.shape.cellWidthM!=shape.cellWidthM||
				physicalFlux.boundary!=advectiveFluxPair.boundary||
				physicalFlux.packedFaceOffset!=expectedOffset||
				physicalFlux.physicalMassFluxKGPerM2S.size()!=8u*allFaces||
				physicalFlux.physicalEnergyFluxWPerM2.size()!=allFaces||
				physicalFlux.methaneRecordId!=record.RecordId()||
				!physicalFlux.fp64ReferenceIdentityVerified||
				!std::isfinite(physicalFlux.maximumFP64ReferenceResidualKGPerM2S)||
				!std::isfinite(physicalFlux.fp64ReferenceForwardErrorBoundKGPerM2S)||
				physicalFlux.maximumFP64ReferenceResidualKGPerM2S>
					physicalFlux.fp64ReferenceForwardErrorBoundKGPerM2S||
				!std::isfinite(physicalFlux.maximumConstraintResidualKGPerM2S)||
				!std::isfinite(physicalFlux.constraintForwardErrorBoundKGPerM2S)||
				physicalFlux.maximumConstraintResidualKGPerM2S>
					physicalFlux.constraintForwardErrorBoundKGPerM2S)return Fail(error,
					"scalar Heun flux-stage input identity is invalid");
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				const FireProductionProjectionBoundary lower=advectiveFluxPair.boundary[2u*axis],
					upper=advectiveFluxPair.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))||
					physicalFlux.physicalGasFluxKGPerM2S[axis].size()!=faces)return Fail(error,
						"scalar Heun flux-stage boundary or gas input is invalid");
			}
			for(const float value:advectiveFluxPair.lowFlux)if(!std::isfinite(value))
				return Fail(error,"scalar Heun advective low flux is nonfinite");
			for(const float value:advectiveFluxPair.fluxDelta)if(!std::isfinite(value))
				return Fail(error,"scalar Heun advective flux delta is nonfinite");
			for(const float value:physicalFlux.physicalMassFluxKGPerM2S)
				if(!std::isfinite(value))return Fail(error,
					"scalar Heun physical mass input is nonfinite");
			for(const float value:physicalFlux.physicalEnergyFluxWPerM2)
				if(!std::isfinite(value))return Fail(error,
					"scalar Heun physical energy input is nonfinite");
			for(unsigned int axis=0u;axis<3u;++axis)for(const float value:
				physicalFlux.physicalGasFluxKGPerM2S[axis])if(!std::isfinite(value))
					return Fail(error,"scalar Heun physical gas input is nonfinite");

			FireProductionScalarHeunFluxStage computed;
			computed.compositeFluxPair=advectiveFluxPair;
			computed.physicalMassFluxKGPerM2S=physicalFlux.physicalMassFluxKGPerM2S;
			computed.physicalEnergyFluxWPerM2=physicalFlux.physicalEnergyFluxWPerM2;
			computed.physicalGasFluxKGPerM2S=physicalFlux.physicalGasFluxKGPerM2S;
			computed.methaneRecordId=physicalFlux.methaneRecordId;
			computed.attemptIdentity=attemptIdentity;computed.role=role;
			computed.stageInputIdentity=HeunStageInputIdentity(
				attemptIdentity,role,advectiveRequest,physicalRequest);
			computed.sharedFCTContractIdentity=HeunSharedFCTContractIdentity(
				advectiveRequest);
			computed.fctRequestIdentity=HeunFCTRequestIdentity(advectiveRequest);
			computed.frozenVelocityIdentity=HeunVelocityIdentity(
				advectiveRequest.frozenVelocityMPerS);
			computed.physicalConstraintForwardErrorBoundKGPerM2S=
				physicalFlux.constraintForwardErrorBoundKGPerM2S;
			computed.fp64ReferenceIdentityVerified=true;
			for(std::size_t component=0u;component<8u;++component)
				for(std::size_t face=0u;face<allFaces;++face){const std::size_t index=
					component*allFaces+face;computed.compositeFluxPair.lowFlux[index]=
						advectiveFluxPair.lowFlux[index]+
						computed.physicalMassFluxKGPerM2S[index];}
			for(std::size_t face=0u;face<allFaces;++face)computed.compositeFluxPair.lowFlux[
				8u*allFaces+face]=advectiveFluxPair.lowFlux[8u*allFaces+face]+
				computed.physicalEnergyFluxWPerM2[face];
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				computed.advectiveGasLowFluxKGPerM2S[axis].assign(faces,0.0f);
				computed.advectiveGasFluxDeltaKGPerM2S[axis].assign(faces,0.0f);
				for(std::size_t face=0u;face<faces;++face){const std::size_t packed=
					expectedOffset[axis]+face;float physicalGas=0.0f,advectiveLow=0.0f,
						advectiveDelta=0.0f;
					for(std::size_t component=1u;component<=6u;++component){
						const std::size_t index=component*allFaces+packed;
						physicalGas+=computed.physicalMassFluxKGPerM2S[index];
						advectiveLow+=advectiveFluxPair.lowFlux[index];
						advectiveDelta+=advectiveFluxPair.fluxDelta[index];
					}
					if(!SameFloatBits(physicalGas,
						computed.physicalGasFluxKGPerM2S[axis][face]))return Fail(error,
							"scalar Heun J_g input differs from published f_N bytes");
					computed.advectiveGasLowFluxKGPerM2S[axis][face]=advectiveLow;
					computed.advectiveGasFluxDeltaKGPerM2S[axis][face]=advectiveDelta;
				}
			}
			computed.compositionIdentity=HeunFluxStageIdentity(computed);
			if(!ValidScalarHeunFluxStage(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarHeunFluxStage();
			FailWithoutThrow(error,"scalar Heun flux-stage allocation failed");return false;}
	}

	bool QueryFireProductionBaseDivergenceTargetCPUPayloadBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& payloadBytes,
		std::string* error )
	{
		payloadBytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return Fail(error,
				"scalar base divergence-target query shape is invalid");
		if(!AddBytes(shape.CellCount(),sizeof(float),payloadBytes)){
			payloadBytes=0u;return Fail(error,
				"scalar base divergence-target query exceeds uint64 capacity");
		}
		if(error)error->clear();return true;
	}

	bool FireProductionScalarDivergenceTargetSealMatches(
		const FireProductionScalarDivergenceTargetSeal& seal,std::string* error )
	{
		const FireProductionProjectionShape& shape=seal.Shape();
		if(!seal.IsSealed()||shape.nx<4u||shape.nx>1024u||shape.ny<4u||
			shape.ny>1024u||shape.nz<4u||shape.nz>1024u||
			!(shape.cellWidthM>0.0f)||!std::isfinite(shape.cellWidthM)||
			!(seal.TimeStepS()>0.0f)||!std::isfinite(seal.TimeStepS())||
			seal.AttemptIdentity()==0u||!ValidDivergenceTargetRole(seal.Role())||
			!ValidProjectionBoundaryTopology(seal.Boundary())||
			seal.MethaneRecordId()!=RISE::FireSimulationMethaneRecord::PhysicalV1().RecordId()||
			seal.TargetPerS().size()!=shape.CellCount()||seal.SourcePacketIdentity()==0u||
			seal.FluxCompositionIdentity()==0u||!std::isfinite(
				seal.MaximumScaledExpansion()))return Fail(error,
					"scalar base divergence-target seal is malformed");
		for(const float value:seal.TargetPerS())if(!std::isfinite(value)||
			(value==0.0f&&std::signbit(static_cast<double>(value))))return Fail(error,
				"scalar base divergence-target value is noncanonical");
		if(seal.TargetIdentity()==0u||seal.TargetIdentity()!=
			BaseDivergenceTargetIdentity(seal))return Fail(error,
				"scalar base divergence-target identity differs");
		if(error)error->clear();return true;
	}

	bool ComposeFireProductionBaseDivergenceTargetCPU(
		const std::uint64_t attemptIdentity,
		const FireProductionScalarDivergenceTargetRole role,
		const FireProductionScalarFCTRequest& advectiveRequest,
		const FireProductionScalarPhysicalFluxPrerequisiteRequest& physicalRequest,
		const FireProductionFrozenSourcePacketSeal& source,
		FireProductionScalarDivergenceTargetSeal& result,std::string* error )
	{
		result=FireProductionScalarDivergenceTargetSeal();
		try {
			const FireProductionProjectionShape& shape=advectiveRequest.shape;
			std::uint64_t stageBytes=0u,targetBytes=0u;
			if(!QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(shape,stageBytes,error)||
				!QueryFireProductionBaseDivergenceTargetCPUPayloadBytes(
					shape,targetBytes,error)||targetBytes>
				std::numeric_limits<std::uint64_t>::max()/2u||stageBytes>
				std::numeric_limits<std::uint64_t>::max()-2u*targetBytes)return Fail(error,
					"scalar base divergence-target live set exceeds uint64 capacity");
			if(stageBytes+2u*targetBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"scalar base divergence-target working set exceeds two GiB");
			const FireProductionScalarHeunFluxRole fluxRole=
				role==FireProductionScalarDivergenceTargetRole::R0Base?
					FireProductionScalarHeunFluxRole::R0:
					FireProductionScalarHeunFluxRole::R1;
			if(attemptIdentity==0u||!ValidDivergenceTargetRole(role)||
				!FireProductionFrozenSourcePacketSealMatches(source,error)||
				source.AttemptIdentity()!=attemptIdentity||source.TimeStepS()!=
					advectiveRequest.timeStepS||source.Shape().nx!=shape.nx||
				source.Shape().ny!=shape.ny||source.Shape().nz!=shape.nz||
				source.Shape().cellWidthM!=shape.cellWidthM||
				!SameFloatVectorBits(source.SourceDelta(),advectiveRequest.sourceDelta))
				return Fail(error,"scalar base divergence-target parent identity is invalid");
			FireProductionScalarHeunFluxStage stage;
			if(!ComposeFireProductionScalarHeunFluxStageCPU(attemptIdentity,fluxRole,
				advectiveRequest,physicalRequest,stage,error))return false;
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			if(stage.methaneRecordId!=source.MethaneRecordId()||
				stage.methaneRecordId!=record.RecordId())return Fail(error,
					"scalar base divergence-target record identity differs");
			const std::size_t cells=shape.CellCount();
			std::vector<float> canonicalTemperatureK(cells,0.0f);
			for(std::size_t cell=0u;cell<cells;++cell){
				if(role==FireProductionScalarDivergenceTargetRole::R0Base)
					canonicalTemperatureK[cell]=source.BeginningTemperatureK()[cell];
				else {
					std::array<double,9> tuple={{}};
					for(std::size_t component=0u;component<9u;++component)
						tuple[component]=static_cast<double>(
							advectiveRequest.beginning[component*cells+cell]);
					double temperatureK=0.0,pressureRatio=0.0;
					if(!record.InvertAcceptedConservativeStateByComponentOrder(tuple.data(),
						tuple.size(),record.TemperatureMinK(),record.TemperatureMaxK(),
						FireStateProducerPrecision::Binary32,temperatureK,pressureRatio,error))return false;
					canonicalTemperatureK[cell]=static_cast<float>(temperatureK);
				}
				if(canonicalTemperatureK[cell]!=physicalRequest.temperatureK[cell])return Fail(error,
						"scalar base divergence-target temperature lacks accepted-state authority");
			}
			if(role==FireProductionScalarDivergenceTargetRole::R0Base&&
				CanonicalSourceBeginningStateIdentity(shape,advectiveRequest.beginning,
					canonicalTemperatureK)!=source.BeginningStateIdentity())return Fail(error,
						"scalar R0 base divergence-target beginning state differs from source authority");
			const std::size_t allFaces=stage.compositeFluxPair.packedFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			FireProductionScalarDivergenceTargetSeal computed;
			computed.shape_=shape;computed.timeStepS_=advectiveRequest.timeStepS;
			computed.attemptIdentity_=attemptIdentity;computed.role_=role;
			computed.boundary_=stage.compositeFluxPair.boundary;
			computed.methaneRecordId_=stage.methaneRecordId;
			computed.targetPerS_.resize(cells,0.0f);
			computed.maximumScaledExpansion_=0.0;
			computed.sourcePacketIdentity_=source.PacketIdentity();
			computed.fluxCompositionIdentity_=stage.compositionIdentity;
			const double dt=static_cast<double>(advectiveRequest.timeStepS);
			const double inverseWidth=1.0/static_cast<double>(shape.cellWidthM);
			for(std::size_t z=0u;z<shape.nz;++z)for(std::size_t y=0u;y<shape.ny;++y)
				for(std::size_t x=0u;x<shape.nx;++x){
					const std::size_t cell=CellIndex(shape,x,y,z);
					std::array<double,9> state={{}},physicalRate={{}};
					for(std::size_t component=0u;component<9u;++component){
						state[component]=static_cast<double>(
							advectiveRequest.beginning[component*cells+cell]);
						physicalRate[component]=0.0;
					}
					for(unsigned int axis=0u;axis<3u;++axis){
						std::size_t rx=x,ry=y,rz=z;
						if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
						const std::size_t left=stage.compositeFluxPair.packedFaceOffset[axis]+
							FaceIndex(shape,axis,x,y,z);
						const std::size_t right=stage.compositeFluxPair.packedFaceOffset[axis]+
							FaceIndex(shape,axis,rx,ry,rz);
						for(std::size_t component=0u;component<8u;++component)
							physicalRate[component]+=inverseWidth*(
								static_cast<double>(stage.physicalMassFluxKGPerM2S[
									component*allFaces+left])-static_cast<double>(
									stage.physicalMassFluxKGPerM2S[component*allFaces+right]));
						physicalRate[8u]+=inverseWidth*(static_cast<double>(
							stage.physicalEnergyFluxWPerM2[left])-static_cast<double>(
							stage.physicalEnergyFluxWPerM2[right]));
					}
					double physicalPerS=0.0;
					if(!record.DivergenceFromDiscreteRateByComponentOrder(state.data(),
						state.size(),physicalRate.data(),physicalRate.size(),static_cast<double>(
							physicalRequest.temperatureK[cell]),
						FireStateProducerPrecision::Binary32,physicalPerS,error))return false;
					const double targetPerS=physicalPerS+static_cast<double>(
						source.DivergenceTargetPerS()[cell]);
					const double scaled=dt*targetPerS;
					float target=static_cast<float>(targetPerS);
					if(!std::isfinite(scaled)||!std::isfinite(target))return Fail(error,
						"scalar base divergence-target composition is nonfinite");
					if(target==0.0f)target=0.0f;
					computed.targetPerS_[cell]=target;
					computed.maximumScaledExpansion_=std::max(
						computed.maximumScaledExpansion_,std::fabs(scaled));
				}
			computed.sealed_=true;
			computed.targetIdentity_=BaseDivergenceTargetIdentity(computed);
			if(!FireProductionScalarDivergenceTargetSealMatches(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarDivergenceTargetSeal();
			FailWithoutThrow(error,"scalar base divergence-target allocation failed");return false;}
	}

	bool FireProductionScalarProjectionTargetSealMatches(
		const FireProductionScalarProjectionTargetSeal& seal,std::string* error )
	{
		const FireProductionProjectionShape& shape=seal.Shape();
		if(!seal.IsSealed()||shape.nx<4u||shape.nx>1024u||shape.ny<4u||
			shape.ny>1024u||shape.nz<4u||shape.nz>1024u||
			!(shape.cellWidthM>0.0f)||!std::isfinite(shape.cellWidthM)||
			!(seal.TimeStepS()>0.0f)||!std::isfinite(seal.TimeStepS())||
			seal.AttemptIdentity()==0u||!ValidDivergenceTargetRole(seal.Role())||
			!ValidProjectionBoundaryTopology(seal.Boundary())||
			seal.TargetPerS().size()!=shape.CellCount()||seal.BaseTargetIdentity()==0u||
			((seal.CorrectionIteration()==0u)!=(seal.ParentTargetIdentity()==0u))||
			((seal.CorrectionIteration()==0u)!=(seal.AcceptedCandidateIdentity()==0u)))
			return Fail(error,
				"scalar projection-target seal is malformed");
		for(const float value:seal.TargetPerS())if(!std::isfinite(value)||
			(value==0.0f&&std::signbit(static_cast<double>(value))))return Fail(error,
				"scalar projection-target value is noncanonical");
		if(seal.TargetIdentity()==0u||seal.TargetIdentity()!=
			ScalarProjectionTargetIdentity(seal))return Fail(error,
				"scalar projection-target identity differs");
		if(error)error->clear();return true;
	}

	bool ComposeFireProductionInitialProjectionTargetCPU(
		const FireProductionScalarDivergenceTargetSeal& base,
		FireProductionScalarProjectionTargetSeal& result,std::string* error )
	{
		result=FireProductionScalarProjectionTargetSeal();
		try {
			std::uint64_t payloadBytes=0u;
			if(!QueryFireProductionBaseDivergenceTargetCPUPayloadBytes(
				base.Shape(),payloadBytes,error)||payloadBytes>
				(std::uint64_t(2u)<<30u)/2u)return Fail(error,
					"scalar initial projection-target working set exceeds two GiB");
			if(!FireProductionScalarDivergenceTargetSealMatches(base,error))return Fail(error,
				"scalar initial projection-target parent is invalid");
			FireProductionScalarProjectionTargetSeal computed;
			computed.shape_=base.Shape();computed.timeStepS_=base.TimeStepS();
			computed.attemptIdentity_=base.AttemptIdentity();computed.role_=base.Role();
			computed.boundary_=base.Boundary();computed.targetPerS_=base.TargetPerS();
			bool pressureOpen=false;
			for(const FireProductionProjectionBoundary value:computed.boundary_)
				pressureOpen=pressureOpen||value==FireProductionProjectionPressureOpen;
			if(!pressureOpen&&!computed.targetPerS_.empty()){
				double mean=0.0;for(const float value:computed.targetPerS_)
					mean+=static_cast<double>(value);
				mean/=static_cast<double>(computed.targetPerS_.size());
				for(float& value:computed.targetPerS_){
					value=static_cast<float>(static_cast<double>(value)-mean);
					if(value==0.0f)value=0.0f;
				}
			}
			computed.baseTargetIdentity_=base.TargetIdentity();
			computed.parentTargetIdentity_=0u;computed.acceptedCandidateIdentity_=0u;
			computed.correctionIteration_=0u;
			computed.sealed_=true;
			computed.targetIdentity_=ScalarProjectionTargetIdentity(computed);
			if(!FireProductionScalarProjectionTargetSealMatches(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&) {
			result=FireProductionScalarProjectionTargetSeal();
			FailWithoutThrow(error,"scalar initial projection-target allocation failed");
			return false;
		}
	}

	std::uint64_t FireProductionProjectedHeunCandidateIdentity(
		const std::uint64_t attemptIdentity,const std::uint8_t stageOrdinal,
		const std::uint64_t parentCandidateIdentity,const std::uint64_t fluxIdentity,
		const std::vector<float>& candidate,
		const std::array<std::vector<float>,3>& alpha )
	{
		std::uint64_t hash=UINT64_C(14695981039346656037);
		static const char domain[]="RISE projected-Heun accepted candidate v1";
		for(const unsigned char byte:domain)HashEOSByte(hash,byte);
		HashEOSUInt64(hash,attemptIdentity);HashEOSUInt64(hash,stageOrdinal);
		HashEOSUInt64(hash,parentCandidateIdentity);HashEOSUInt64(hash,fluxIdentity);
		HashFluxValues(hash,candidate);
		for(const std::vector<float>& axis:alpha)HashFluxValues(hash,axis);
		return hash;
	}

	bool FireProductionMonitoredManifoldPolicy::SignedTailDrainPerS(
		const double representedPressureRatio,const double timeStepS,
		double& drainPerS,std::string* error )
	{
		drainPerS=0.0;
		if(!std::isfinite(representedPressureRatio)||!std::isfinite(timeStepS)||
			timeStepS<=0.0)return Fail(error,
				"monitored-manifold tail policy input is invalid");
		const double signedDeviation=representedPressureRatio-1.0;
		const double magnitude=std::fabs(signedDeviation);
		if(magnitude<=EngagementThreshold){if(error)error->clear();return true;}
		drainPerS=-std::copysign((magnitude-EngagementThreshold)/timeStepS,
			signedDeviation);
		if(!std::isfinite(drainPerS))return Fail(error,
			"monitored-manifold tail policy overflowed");
		if(error)error->clear();return true;
	}

	bool FireProductionProjectedHeunTargetAuthority::Correct(
		const FireProductionScalarProjectionTargetSeal& current,
		const std::vector<float>& acceptedCandidate,
		const std::uint64_t acceptedCandidateIdentity,
		const std::uint8_t stageOrdinal,
		const std::uint64_t parentCandidateIdentity,
		const std::uint64_t fluxIdentity,
		const std::array<std::vector<float>,3>& alpha,
		FireProductionScalarProjectionTargetSeal& result,std::string* error )
	{
		result=FireProductionScalarProjectionTargetSeal();
		try {
			if(!FireProductionScalarProjectionTargetSealMatches(current,error)||
				acceptedCandidateIdentity==0u||stageOrdinal==0u||
				acceptedCandidateIdentity!=FireProductionProjectedHeunCandidateIdentity(
					current.AttemptIdentity(),stageOrdinal,parentCandidateIdentity,fluxIdentity,
					acceptedCandidate,alpha)||
				current.CorrectionIteration()==std::numeric_limits<std::uint32_t>::max()||
				acceptedCandidate.size()!=9u*current.Shape().CellCount())return Fail(error,
					"projected-Heun r70 candidate lineage is invalid");
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			const std::size_t cells=current.Shape().CellCount();
			std::vector<double> correction(cells,0.0);
			std::vector<double> corrected(cells,0.0);
			double mean=0.0;
			for(std::size_t cell=0u;cell<cells;++cell){
				std::array<double,9> tuple={{}};
				for(std::size_t component=0u;component<9u;++component)
					tuple[component]=static_cast<double>(
						acceptedCandidate[component*cells+cell]);
				double volumeRatio=0.0;
				if(!record.AcceptedConservativeVolumeRatioByComponentOrder(tuple.data(),
					tuple.size(),FireStateProducerPrecision::Binary32,volumeRatio,error))return false;
				if(!FireProductionMonitoredManifoldPolicy::SignedTailDrainPerS(volumeRatio,
					static_cast<double>(current.TimeStepS()),correction[cell],error))return false;
				corrected[cell]=static_cast<double>(current.TargetPerS()[cell])+
					correction[cell];
				mean+=corrected[cell];
			}
			bool pressureOpen=false;
			for(const FireProductionProjectionBoundary value:current.Boundary())
				pressureOpen=pressureOpen||value==FireProductionProjectionPressureOpen;
			if(!pressureOpen)mean/=static_cast<double>(cells);else mean=0.0;
			FireProductionScalarProjectionTargetSeal computed;
			computed.shape_=current.Shape();computed.timeStepS_=current.TimeStepS();
			computed.attemptIdentity_=current.AttemptIdentity();computed.role_=current.Role();
			computed.boundary_=current.Boundary();computed.targetPerS_.resize(cells);
			computed.baseTargetIdentity_=current.BaseTargetIdentity();
			computed.parentTargetIdentity_=current.TargetIdentity();
			computed.acceptedCandidateIdentity_=acceptedCandidateIdentity;
			computed.correctionIteration_=current.CorrectionIteration()+1u;
			for(std::size_t cell=0u;cell<cells;++cell){
				float value=static_cast<float>(corrected[cell]-mean);
				if(!std::isfinite(value))return Fail(error,
					"projected-Heun r70 target overflowed");
				if(value==0.0f)value=0.0f;
				computed.targetPerS_[cell]=value;
			}
			computed.sealed_=true;
			computed.targetIdentity_=ScalarProjectionTargetIdentity(computed);
			if(!FireProductionScalarProjectionTargetSealMatches(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){
			result=FireProductionScalarProjectionTargetSeal();
			return Fail(error,"projected-Heun r70 target allocation failed");
		}
	}

	bool FireProductionProjectedHeunTargetAuthority::EndpointBase(
		const FireProductionScalarPhysicalFluxPrerequisiteResult& endpointFlux,
		const std::vector<float>& committedConservativeValues,
		const std::vector<float>& committedTemperatureK,
		const FireProductionFrozenSourcePacketSeal& source,
		FireProductionScalarProjectionTargetSeal& result,std::string* error )
	{
		result=FireProductionScalarProjectionTargetSeal();
		try {
			if(!FireProductionFrozenSourcePacketSealMatches(source,error))return Fail(error,
					"projected-Heun endpoint target parent is invalid");
			const FireProductionProjectionShape& shape=endpointFlux.shape;
			const std::size_t cells=shape.CellCount();
			if(!SameProjectionShape(source.Shape(),shape)||
				committedConservativeValues.size()!=9u*cells||
				committedTemperatureK.size()!=cells)return Fail(error,
					"projected-Heun endpoint target lineage differs");
			const std::size_t allFaces=endpointFlux.packedFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			if(endpointFlux.physicalMassFluxKGPerM2S.size()!=8u*allFaces||
				endpointFlux.physicalEnergyFluxWPerM2.size()!=allFaces||
				!endpointFlux.fp64ReferenceIdentityVerified)return Fail(error,
					"projected-Heun endpoint physical flux is invalid");
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			FireProductionScalarProjectionTargetSeal computed;
			computed.shape_=shape;computed.timeStepS_=source.TimeStepS();
			computed.attemptIdentity_=source.AttemptIdentity();
			computed.role_=FireProductionScalarDivergenceTargetRole::R1Base;
			computed.boundary_=endpointFlux.boundary;
			computed.targetPerS_.assign(cells,0.0f);
			std::uint64_t baseHash=UINT64_C(14695981039346656037);
			static const char domain[]="RISE projected-Heun endpoint base target v1";
			for(const unsigned char byte:domain)HashEOSByte(baseHash,byte);
			HashFluxValues(baseHash,endpointFlux.physicalMassFluxKGPerM2S);
			HashFluxValues(baseHash,endpointFlux.physicalEnergyFluxWPerM2);
			HashEOSUInt64(baseHash,source.PacketIdentity());
			HashFluxValues(baseHash,committedConservativeValues);
			HashFluxValues(baseHash,committedTemperatureK);
			computed.baseTargetIdentity_=baseHash;
			computed.parentTargetIdentity_=0u;computed.acceptedCandidateIdentity_=0u;
			computed.correctionIteration_=0u;
			const double inverseWidth=1.0/static_cast<double>(shape.cellWidthM);
			for(std::size_t z=0u;z<shape.nz;++z)for(std::size_t y=0u;y<shape.ny;++y)
				for(std::size_t x=0u;x<shape.nx;++x){
					const std::size_t cell=CellIndex(shape,x,y,z);
					std::array<double,9> state={{}},rate={{}};
					for(std::size_t component=0u;component<9u;++component)
						state[component]=static_cast<double>(
							committedConservativeValues[component*cells+cell]);
					for(unsigned int axis=0u;axis<3u;++axis){
						std::size_t rx=x,ry=y,rz=z;
						if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
						const std::size_t left=endpointFlux.packedFaceOffset[axis]+
							FaceIndex(shape,axis,x,y,z);
						const std::size_t right=endpointFlux.packedFaceOffset[axis]+
							FaceIndex(shape,axis,rx,ry,rz);
						for(std::size_t component=0u;component<8u;++component)
							rate[component]+=inverseWidth*(static_cast<double>(
								endpointFlux.physicalMassFluxKGPerM2S[component*allFaces+left])-
								static_cast<double>(endpointFlux.physicalMassFluxKGPerM2S[
									component*allFaces+right]));
						rate[8u]+=inverseWidth*(static_cast<double>(
							endpointFlux.physicalEnergyFluxWPerM2[left])-static_cast<double>(
							endpointFlux.physicalEnergyFluxWPerM2[right]));
					}
					double physicalPerS=0.0;
					if(!record.DivergenceFromDiscreteRateByComponentOrder(state.data(),state.size(),
						rate.data(),rate.size(),static_cast<double>(committedTemperatureK[cell]),
						FireStateProducerPrecision::Binary32,physicalPerS,error))return false;
					float target=static_cast<float>(physicalPerS+
						static_cast<double>(source.DivergenceTargetPerS()[cell]));
					if(!std::isfinite(target))return Fail(error,
						"projected-Heun endpoint target is nonfinite");
					if(target==0.0f)target=0.0f;
					computed.targetPerS_[cell]=target;
				}
			bool pressureOpen=false;
			for(const FireProductionProjectionBoundary value:computed.boundary_)
				pressureOpen=pressureOpen||value==FireProductionProjectionPressureOpen;
			if(!pressureOpen&&!computed.targetPerS_.empty()){
				double mean=0.0;for(const float value:computed.targetPerS_)
					mean+=static_cast<double>(value);
				mean/=static_cast<double>(computed.targetPerS_.size());
				for(float& value:computed.targetPerS_){
					value=static_cast<float>(static_cast<double>(value)-mean);
					if(value==0.0f)value=0.0f;
				}
			}
			computed.sealed_=true;
			computed.targetIdentity_=ScalarProjectionTargetIdentity(computed);
			if(!FireProductionScalarProjectionTargetSealMatches(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){
			result=FireProductionScalarProjectionTargetSeal();
			return Fail(error,"projected-Heun endpoint target allocation failed");
		}
	}

	bool ProjectFireProductionScalarTargetCPU(
		FireProductionProjectionRequest request,
		const FireProductionScalarProjectionTargetSeal& target,
		FireProductionProjectionResult& result,std::string* error )
	{
		result=FireProductionProjectionResult();
		try {
			if(!request.divergenceTargetPerS.empty()||
				!FireProductionScalarProjectionTargetSealMatches(target,error)||
				!SameProjectionShape(request.shape,target.Shape())||
				request.timeStepS!=target.TimeStepS()||request.boundary!=target.Boundary())
				return Fail(error,
					"scalar authenticated projection request or target lineage is invalid");
			request.divergenceTargetPerS=target.TargetPerS();
			return ProjectFireProductionCPU(request,result,error);
		} catch(const std::bad_alloc&) {
			result=FireProductionProjectionResult();
			return Fail(error,"scalar authenticated projection allocation failed");
		}
	}

	bool AverageFireProductionScalarHeunFluxStagesCPU(
		const FireProductionScalarHeunFluxStage& first,
		const FireProductionScalarHeunFluxStage& second,
		FireProductionScalarHeunFluxStage& result,std::string* error )
	{
		result=FireProductionScalarHeunFluxStage();
		try {
			const FireProductionScalarFCTFluxPair& a=first.compositeFluxPair;
			const FireProductionScalarFCTFluxPair& b=second.compositeFluxPair;
			std::uint64_t firstPayload=0u,secondPayload=0u;
			if(!QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
				a.shape,firstPayload,error)||
				!QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
					b.shape,secondPayload,error))return false;
			std::uint64_t liveBytes=firstPayload;
			if(liveBytes>std::numeric_limits<std::uint64_t>::max()-firstPayload||
				(liveBytes+=firstPayload)>std::numeric_limits<std::uint64_t>::max()-
					secondPayload)return Fail(error,
						"scalar Heun flux-stage average working set exceeds uint64 capacity");
			liveBytes+=secondPayload;
			if(
				liveBytes>(std::uint64_t(2u)<<30u))return Fail(error,
					"scalar Heun flux-stage average working set exceeds two GiB");
			if(a.shape.nx!=b.shape.nx||a.shape.ny!=b.shape.ny||a.shape.nz!=b.shape.nz||
				a.shape.cellWidthM!=b.shape.cellWidthM||a.timeStepS!=b.timeStepS||
				a.boundary!=b.boundary||a.packedFaceOffset!=b.packedFaceOffset||
				first.methaneRecordId!=second.methaneRecordId||
				first.attemptIdentity!=second.attemptIdentity||
				first.sharedFCTContractIdentity!=second.sharedFCTContractIdentity||
				first.role!=FireProductionScalarHeunFluxRole::R0||
				second.role!=FireProductionScalarHeunFluxRole::R1)return Fail(error,
					"scalar Heun flux-stage average identity is invalid");
			if(!ValidScalarHeunFluxStage(first,error)||!ValidScalarHeunFluxStage(second,error))
				return false;
			const std::size_t allFaces=FireProductionProjectionFaceCount(a.shape,0u)+
				FireProductionProjectionFaceCount(a.shape,1u)+
				FireProductionProjectionFaceCount(a.shape,2u);
			FireProductionScalarHeunFluxStage computed;
			computed.compositeFluxPair.shape=a.shape;
			computed.compositeFluxPair.timeStepS=a.timeStepS;
			computed.compositeFluxPair.boundary=a.boundary;
			computed.compositeFluxPair.packedFaceOffset=a.packedFaceOffset;
			computed.methaneRecordId=first.methaneRecordId;
			computed.attemptIdentity=first.attemptIdentity;
			computed.role=FireProductionScalarHeunFluxRole::HeunAverage;
			computed.stageInputIdentity=HeunAverageInputIdentity(first,second);
			computed.sharedFCTContractIdentity=first.sharedFCTContractIdentity;
			computed.fctRequestIdentity=first.fctRequestIdentity;
			computed.frozenVelocityIdentity=HeunAverageInputIdentity(first,second);
			computed.parentCompositionIdentity={{first.compositionIdentity,
				second.compositionIdentity}};
			auto average=[](const std::vector<float>& x,const std::vector<float>& y,
				std::vector<float>& output){output.resize(x.size());for(std::size_t value=0u;
					value<x.size();++value){const float mean=0.5f*(x[value]+y[value]);
					if(!std::isfinite(mean))return false;output[value]=mean;}return true;};
			if(!average(a.lowFlux,b.lowFlux,computed.compositeFluxPair.lowFlux)||
				!average(a.fluxDelta,b.fluxDelta,computed.compositeFluxPair.fluxDelta)||
				!average(first.physicalMassFluxKGPerM2S,second.physicalMassFluxKGPerM2S,
					computed.physicalMassFluxKGPerM2S)||
				!average(first.physicalEnergyFluxWPerM2,second.physicalEnergyFluxWPerM2,
					computed.physicalEnergyFluxWPerM2))return Fail(error,
					"scalar Heun averaged flux is nonfinite");
			for(unsigned int axis=0u;axis<3u;++axis)if(
				!average(first.physicalGasFluxKGPerM2S[axis],
					second.physicalGasFluxKGPerM2S[axis],
					computed.physicalGasFluxKGPerM2S[axis])||
				!average(first.advectiveGasLowFluxKGPerM2S[axis],
					second.advectiveGasLowFluxKGPerM2S[axis],
					computed.advectiveGasLowFluxKGPerM2S[axis])||
				!average(first.advectiveGasFluxDeltaKGPerM2S[axis],
					second.advectiveGasFluxDeltaKGPerM2S[axis],
					computed.advectiveGasFluxDeltaKGPerM2S[axis]))return Fail(error,
						"scalar Heun averaged gas flux is nonfinite");
			const RISE::FireCertifiedNullspace& projection=
				RISE::FireSimulationMethaneRecord::PhysicalV1().NonadvectiveFluxProjection();
			const double epsilon64=std::numeric_limits<double>::epsilon(),
				gamma64=(8.0*epsilon64)/(1.0-8.0*epsilon64);
			for(std::size_t face=0u;face<allFaces;++face)
				for(std::size_t row=0u;row<projection.constraintRows;++row){
					double publicationError=0.0,reductionScale=0.0;
					for(std::size_t component=0u;component<8u;++component){
						const std::size_t index=component*allFaces+face;
						const double exactMean=0.5*(static_cast<double>(
							first.physicalMassFluxKGPerM2S[index])+static_cast<double>(
							second.physicalMassFluxKGPerM2S[index]));
						const double published=static_cast<double>(
							computed.physicalMassFluxKGPerM2S[index]);
						const double coefficient=projection.constraintMatrix[row*8u+component];
						publicationError+=std::fabs(coefficient)*std::fabs(published-exactMean);
						reductionScale+=std::fabs(coefficient*published);
					}
					computed.physicalConstraintForwardErrorBoundKGPerM2S=std::max(
						computed.physicalConstraintForwardErrorBoundKGPerM2S,
						0.5*(first.physicalConstraintForwardErrorBoundKGPerM2S+
						second.physicalConstraintForwardErrorBoundKGPerM2S)+
						publicationError+gamma64*reductionScale);
				}
			const double epsilon32=static_cast<double>(std::numeric_limits<float>::epsilon()),
				gamma20=(20.0*epsilon32)/(1.0-20.0*epsilon32);
			for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
				FireProductionProjectionFaceCount(a.shape,axis);for(std::size_t face=0u;
					face<faces;++face){const std::size_t packed=a.packedFaceOffset[axis]+face;
					double scale=1.0;for(std::size_t component=1u;component<=6u;++component){
						const std::size_t index=component*allFaces+packed;
						scale+=std::fabs(static_cast<double>(first.physicalMassFluxKGPerM2S[index]));
						scale+=std::fabs(static_cast<double>(second.physicalMassFluxKGPerM2S[index]));
					}
					computed.physicalGasAveragingForwardErrorBoundKGPerM2S=std::max(
						computed.physicalGasAveragingForwardErrorBoundKGPerM2S,gamma20*scale);
				}
			}
			computed.fp64ReferenceIdentityVerified=true;
			computed.compositionIdentity=HeunFluxStageIdentity(computed);
			if(!ValidScalarHeunFluxStage(computed,error))return false;
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarHeunFluxStage();
			FailWithoutThrow(error,"scalar Heun flux-stage average allocation failed");return false;}
	}

	bool SolveFireProductionScalarHeunFluxStageCPU(
		const std::uint64_t attemptIdentity,const FireProductionScalarFCTRequest& request,
		const FireProductionScalarHeunFluxStage& averagedStage,
		FireProductionScalarHeunSolveResult& result,std::string* error )
	{
		result=FireProductionScalarHeunSolveResult();
		if(!ValidScalarHeunFluxStage(averagedStage,error))return false;
		if(attemptIdentity==0u||averagedStage.attemptIdentity!=attemptIdentity||
			averagedStage.role!=FireProductionScalarHeunFluxRole::HeunAverage||
			averagedStage.sharedFCTContractIdentity!=HeunSharedFCTContractIdentity(request)||
			averagedStage.fctRequestIdentity!=HeunFCTRequestIdentity(request))return Fail(error,
				"scalar Heun fresh-alpha solve identity differs");
		FireProductionScalarHeunSolveResult computed;
		if(!SolveFireProductionScalarFCTFluxPairCPU(request,
			averagedStage.compositeFluxPair,computed.scalar,error))return false;
		computed.attemptIdentity=attemptIdentity;
		computed.averageCompositionIdentity=averagedStage.compositionIdentity;
		computed.sharedFCTContractIdentity=averagedStage.sharedFCTContractIdentity;
		computed.parentCompositionIdentity=averagedStage.parentCompositionIdentity;
		computed.alphaIdentity=HeunAlphaIdentity(attemptIdentity,
			computed.averageCompositionIdentity,computed.parentCompositionIdentity,
			computed.scalar.sharedFaceAlpha);
		result=std::move(computed);if(error)error->clear();return true;
	}

	bool QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& workingSetBytes,
		std::string* error )
	{
		workingSetBytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
			!std::isfinite(shape.cellWidthM))return Fail(error,
				"scalar EOS acceptance query shape is invalid");
		if(!AddBytes(shape.CellCount(),10u*sizeof(float),workingSetBytes)){
			workingSetBytes=0u;return Fail(error,
				"scalar EOS acceptance query exceeds uint64 capacity");
		}
		if(error)error->clear();return true;
	}

	bool EvaluateFireProductionScalarEOSAcceptanceCPU(
		const FireProductionScalarEOSAcceptanceRequest& request,
		FireProductionScalarEOSAcceptanceResult& result,std::string* error )
	{
		result=FireProductionScalarEOSAcceptanceResult();
		try {
			const RISE::FireSimulationMethaneRecord& record=
				RISE::FireSimulationMethaneRecord::PhysicalV1();
			RISE::FireCase::RecordV1 sealedCase;std::string caseError;
			if(!record.IsValid()||request.methaneRecordId!=record.RecordId()||
				!CanonicalEOSRecordId(request.methaneRecordId)||
				!ValidEOSStage(request.stage)||
				request.producerPrecision!=FireStateProducerPrecision::Binary32||
				!(request.timeStepS>0.0f)||!std::isfinite(request.timeStepS)||
				request.attemptIdentity==0u||!RISE::FireCase::ValidateMethaneEnvelopeV1(
					request.caseRecordEnvelope,record,sealedCase,caseError)||
				sealedCase.authored.fuelRecordId!=record.RecordId()||
				!std::isfinite(sealedCase.derived.pilotAmbientTemperatureK)||
				!std::isfinite(sealedCase.derived.maximumAcceptedTemperatureK)||
				sealedCase.derived.pilotAmbientTemperatureK<record.TemperatureMinK()||
				sealedCase.derived.maximumAcceptedTemperatureK>record.TemperatureMaxK()||
				sealedCase.derived.pilotAmbientTemperatureK>=
					sealedCase.derived.maximumAcceptedTemperatureK)return Fail(error,
					"scalar EOS acceptance metadata or payload is invalid");
			std::uint64_t workingSetBytes=0u;
			if(!QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
				request.shape,workingSetBytes,error))return false;
			if(workingSetBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"scalar EOS acceptance working set exceeds two GiB");
			const std::size_t cells=request.shape.CellCount();
			if(request.conservativeValues.size()!=9u*cells)return Fail(error,
				"scalar EOS acceptance metadata or payload is invalid");
			const double lowerTemperatureK=sealedCase.derived.pilotAmbientTemperatureK,
				upperTemperatureK=sealedCase.derived.maximumAcceptedTemperatureK;
			FireProductionScalarEOSAcceptanceResult computed;
			computed.shape=request.shape;computed.timeStepS=request.timeStepS;
			computed.attemptIdentity=request.attemptIdentity;computed.stage=request.stage;
			computed.producerPrecision=request.producerPrecision;
			computed.methaneRecordId=request.methaneRecordId;
			computed.caseRecordId=sealedCase.caseRecordId;
			computed.lowerTemperatureK=lowerTemperatureK;
			computed.upperTemperatureK=upperTemperatureK;
			computed.temperatureK.resize(cells);
			for(std::size_t cell=0u;cell<cells;++cell){
				std::array<double,9> state;
				for(std::size_t component=0u;component<9u;++component){
					const float value=request.conservativeValues[component*cells+cell];
					if(!std::isfinite(value))return Fail(error,
						"scalar EOS acceptance conservative value is nonfinite");
					state[component]=static_cast<double>(value);
				}
				double temperature=0.0,pressureRatio=0.0;
				if(!record.InvertAcceptedConservativeStateByComponentOrder(state.data(),
					state.size(),lowerTemperatureK,upperTemperatureK,
					request.producerPrecision,temperature,pressureRatio,error))return false;
				if(temperature>=upperTemperatureK)return Fail(error,
					"scalar EOS acceptance temperature reaches the case ceiling");
				const float publishedTemperature=static_cast<float>(temperature);
				if(!std::isfinite(publishedTemperature)||
					static_cast<double>(publishedTemperature)<lowerTemperatureK||
					static_cast<double>(publishedTemperature)>=upperTemperatureK)
					return Fail(error,
						"scalar EOS acceptance temperature publication is outside the case bounds");
				double publishedPressureRatio=0.0;
				if(!record.AcceptedConservativePressureRatioAtTemperatureByComponentOrder(
					state.data(),state.size(),static_cast<double>(publishedTemperature),
					request.producerPrecision,publishedPressureRatio,error))return false;
				const double residual=std::fabs(publishedPressureRatio-1.0);
				if(!std::isfinite(residual))return Fail(error,
					"scalar EOS acceptance pressure residual is nonfinite");
				computed.temperatureK[cell]=publishedTemperature;
				computed.maximumEOSResidual=std::max(computed.maximumEOSResidual,residual);
			}
			computed.stateDigest=EOSValueDigest(request.conservativeValues);
			computed.temperatureDigest=EOSValueDigest(computed.temperatureK);
			computed.acceptanceIdentity=EOSAcceptanceIdentity(request,computed.caseRecordId,
				lowerTemperatureK,upperTemperatureK,
				computed.stateDigest,computed.temperatureDigest,computed.maximumEOSResidual);
			computed.accepted=true;result=std::move(computed);
			if(error)error->clear();return true;
		} catch(const std::bad_alloc&) {
			result=FireProductionScalarEOSAcceptanceResult();
			FailWithoutThrow(error,"scalar EOS acceptance allocation failed");return false;
		}
	}

	bool FireProductionScalarEOSAcceptanceMatches(
		const FireProductionScalarEOSAcceptanceRequest& request,
		const FireProductionScalarEOSAcceptanceResult& result )
	{
		const RISE::FireSimulationMethaneRecord& record=
			RISE::FireSimulationMethaneRecord::PhysicalV1();
		RISE::FireCase::RecordV1 sealedCase;std::string caseError;
		if(!result.accepted||!ValidEOSStage(request.stage)||!record.IsValid()||
			request.methaneRecordId!=record.RecordId()||
			!RISE::FireCase::ValidateMethaneEnvelopeV1(request.caseRecordEnvelope,record,
				sealedCase,caseError)||
			request.shape.nx!=result.shape.nx||request.shape.ny!=result.shape.ny||
			request.shape.nz!=result.shape.nz||
			EOSDoubleBits(static_cast<double>(request.shape.cellWidthM))!=
				EOSDoubleBits(static_cast<double>(result.shape.cellWidthM))||
			EOSDoubleBits(static_cast<double>(request.timeStepS))!=
				EOSDoubleBits(static_cast<double>(result.timeStepS))||
			request.attemptIdentity!=result.attemptIdentity||request.stage!=result.stage||
			request.producerPrecision!=result.producerPrecision||
			request.methaneRecordId!=result.methaneRecordId||
			sealedCase.caseRecordId!=result.caseRecordId||
			EOSDoubleBits(sealedCase.derived.pilotAmbientTemperatureK)!=
				EOSDoubleBits(result.lowerTemperatureK)||
			EOSDoubleBits(sealedCase.derived.maximumAcceptedTemperatureK)!=
				EOSDoubleBits(result.upperTemperatureK)||
			result.temperatureK.size()!=request.shape.CellCount()||
			!std::isfinite(result.maximumEOSResidual)||result.maximumEOSResidual<0.0||
			request.conservativeValues.size()!=9u*request.shape.CellCount())return false;
		for(const float value:request.conservativeValues)if(!std::isfinite(value))return false;
		for(const float value:result.temperatureK)if(!std::isfinite(value)||
			static_cast<double>(value)<result.lowerTemperatureK||
			static_cast<double>(value)>=result.upperTemperatureK)return false;
		const std::uint64_t stateDigest=EOSValueDigest(request.conservativeValues),
			temperatureDigest=EOSValueDigest(result.temperatureK);
		return stateDigest==result.stateDigest&&temperatureDigest==result.temperatureDigest&&
			EOSAcceptanceIdentity(request,sealedCase.caseRecordId,
				sealedCase.derived.pilotAmbientTemperatureK,
				sealedCase.derived.maximumAcceptedTemperatureK,stateDigest,temperatureDigest,
				result.maximumEOSResidual)==
				result.acceptanceIdentity;
	}

	static bool EvaluateFireProductionCompatibleFCTMomentumCPUImpl(
		const FireProductionCompatibleFCTMomentumRequest& request,
		const bool secondFluxIsDelta,
		FireProductionCompatibleFCTMomentumResult& result, std::string* error )
	{
		result=FireProductionCompatibleFCTMomentumResult();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!std::isfinite(1.0f/shape.cellWidthM) )
				return Fail(error,"compatible FCT momentum shape is invalid");
			bool anyPeriodic=false,allPeriodic=true;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic)) )
					return Fail(error,"compatible FCT momentum boundary pairing is invalid");
				const bool periodic=lower==FireProductionProjectionPeriodic;
				anyPeriodic=anyPeriodic||periodic;allPeriodic=allPeriodic&&periodic;
			}
			if( anyPeriodic&&!allPeriodic ) return Fail(error,
				"compatible FCT momentum hybrid periodic topology has no authoritative oracle");
			std::uint64_t allFaces=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if( request.lowGasFluxKGPerM2S[axis].size()!=faces||
					request.highGasFluxKGPerM2S[axis].size()!=faces||
					request.sharedFaceAlpha[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces||
					(!request.physicalGasFluxKGPerM2S[axis].empty()&&
					 request.physicalGasFluxKGPerM2S[axis].size()!=faces) )
					return Fail(error,"compatible FCT momentum face shape is invalid");
				for( std::size_t face=0u;face<faces;++face ) {
					const float low=request.lowGasFluxKGPerM2S[axis][face];
					const float second=request.highGasFluxKGPerM2S[axis][face];
					const float alpha=request.sharedFaceAlpha[axis][face];
					const float velocity=request.frozenVelocityMPerS[axis][face];
					const float physical=request.physicalGasFluxKGPerM2S[axis].empty()?0.0f:
						request.physicalGasFluxKGPerM2S[axis][face];
					if( !std::isfinite(low)||!std::isfinite(second)||!std::isfinite(physical)||
						!std::isfinite(alpha)||alpha<0.0f||alpha>1.0f||
						!std::isfinite(velocity) ) return Fail(error,
						"compatible FCT momentum face value is invalid");
				}
				if( allPeriodic&&(!PeriodicFaceSeamBitEqual(shape,
					request.lowGasFluxKGPerM2S[axis],axis)||
					!PeriodicFaceSeamBitEqual(shape,request.highGasFluxKGPerM2S[axis],axis)||
					!PeriodicFaceSeamBitEqual(shape,request.sharedFaceAlpha[axis],axis)||
					!PeriodicFaceSeamBitEqual(shape,request.frozenVelocityMPerS[axis],axis)||
					(!request.physicalGasFluxKGPerM2S[axis].empty()&&
					 !PeriodicFaceSeamBitEqual(shape,
						request.physicalGasFluxKGPerM2S[axis],axis))) )
					return Fail(error,"compatible FCT momentum periodic seam is invalid");
				allFaces+=faces;
			}
			std::uint64_t outputBytes=0u;
			if( !AddBytes(allFaces,2u*sizeof(float),outputBytes)||
				outputBytes>(std::uint64_t(2u)<<30u) ) return Fail(error,
				"compatible FCT momentum output exceeds two GiB");

			FireProductionCompatibleFCTMomentumResult computed;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				computed.acceptedGasFluxKGPerM2S[axis].resize(faces);
				for( std::size_t face=0u;face<faces;++face ) {
					const float low=request.lowGasFluxKGPerM2S[axis][face];
					const float second=request.highGasFluxKGPerM2S[axis][face];
					const float physical=request.physicalGasFluxKGPerM2S[axis].empty()?0.0f:
						request.physicalGasFluxKGPerM2S[axis][face];
					const float delta=secondFluxIsDelta?second:second-low;
					const float accepted=low+request.sharedFaceAlpha[axis][face]*delta+
						physical;
					if( !std::isfinite(accepted) ) return Fail(error,
						"compatible FCT momentum accepted flux is nonfinite");
					computed.acceptedGasFluxKGPerM2S[axis][face]=accepted;
				}
			}

			if( allPeriodic ) {
				auto previousCoordinate=[]( const std::size_t coordinate,
					const std::size_t extent ) {return coordinate?coordinate-1u:extent-1u;};
				auto nextCoordinate=[]( const std::size_t coordinate,
					const std::size_t extent ) {return coordinate+1u==extent?0u:coordinate+1u;};
				for( unsigned int component=0u;component<3u;++component ) {
					computed.advectionRateKGPerM2S2[component].assign(
						FireProductionProjectionFaceCount(shape,component),0.0f);
					for( std::size_t z=0u;z<shape.nz;++z )
						for( std::size_t y=0u;y<shape.ny;++y )
							for( std::size_t x=0u;x<shape.nx;++x ) {
								const std::size_t componentFace=FaceIndex(shape,component,x,y,z);
								float divergence=0.0f;
								for( unsigned int derivative=0u;derivative<3u;++derivative ) {
									std::size_t ncx=x,ncy=y,ncz=z,pdx=x,pdy=y,pdz=z,
										pcx=x,pcy=y,pcz=z,ndx=x,ndy=y,ndz=z;
									const std::size_t componentCoordinate=AxisCoordinate(component,x,y,z);
									const std::size_t derivativeCoordinate=AxisCoordinate(derivative,x,y,z);
									const std::size_t nextDerivative=nextCoordinate(derivativeCoordinate,
										AxisCoordinateExtent(shape,derivative));
									SetAxisCoordinate(component,nextCoordinate(componentCoordinate,
										AxisCoordinateExtent(shape,component)),ncx,ncy,ncz);
									SetAxisCoordinate(derivative,previousCoordinate(derivativeCoordinate,
										AxisCoordinateExtent(shape,derivative)),pdx,pdy,pdz);
									SetAxisCoordinate(component,previousCoordinate(componentCoordinate,
										AxisCoordinateExtent(shape,component)),pcx,pcy,pcz);
									SetAxisCoordinate(derivative,nextDerivative,ndx,ndy,ndz);
									auto massFlux=[&]( std::size_t fx, std::size_t fy,
										std::size_t fz ) {return computed.acceptedGasFluxKGPerM2S[
										derivative][FaceIndex(shape,derivative,fx,fy,fz)];};
									float upper=0.0f,lower=0.0f;
									if( derivative==component ) {
										upper=0.25f*(massFlux(x,y,z)+massFlux(ncx,ncy,ncz))*
											(request.frozenVelocityMPerS[component][componentFace]+
											 request.frozenVelocityMPerS[component][FaceIndex(shape,
											 component,ncx,ncy,ncz)]);
										lower=0.25f*(massFlux(pdx,pdy,pdz)+massFlux(x,y,z))*
											(request.frozenVelocityMPerS[component][FaceIndex(shape,
											 component,pdx,pdy,pdz)]+
											 request.frozenVelocityMPerS[component][componentFace]);
									} else {
										std::size_t pcux=pcx,pcuy=pcy,pcuz=pcz,
											cux=x,cuy=y,cuz=z;
										SetAxisCoordinate(derivative,nextDerivative,pcux,pcuy,pcuz);
										SetAxisCoordinate(derivative,nextDerivative,cux,cuy,cuz);
										upper=0.25f*(massFlux(pcux,pcuy,pcuz)+massFlux(cux,cuy,cuz))*
											(request.frozenVelocityMPerS[component][componentFace]+
											 request.frozenVelocityMPerS[component][FaceIndex(shape,
											 component,ndx,ndy,ndz)]);
										lower=0.25f*(massFlux(pcx,pcy,pcz)+massFlux(x,y,z))*
											(request.frozenVelocityMPerS[component][FaceIndex(shape,
											 component,pdx,pdy,pdz)]+
											 request.frozenVelocityMPerS[component][componentFace]);
									}
									divergence+=(upper-lower)/shape.cellWidthM;
								}
								if( !std::isfinite(divergence) ) return Fail(error,
									"compatible FCT momentum periodic rate is nonfinite");
								computed.advectionRateKGPerM2S2[component][componentFace]=divergence;
							}
					const std::size_t firstEnd=component==0u?shape.ny:shape.nx;
					const std::size_t secondEnd=component==2u?shape.ny:shape.nz;
					for( std::size_t second=0u;second<secondEnd;++second )
						for( std::size_t first=0u;first<firstEnd;++first ) {
							std::size_t lowX=component==0u?0u:first;
							std::size_t lowY=component==0u?first:(component==1u?0u:second);
							std::size_t lowZ=component==2u?0u:second;
							std::size_t highX=lowX,highY=lowY,highZ=lowZ;
							SetAxisCoordinate(component,AxisCoordinateExtent(shape,component),
								highX,highY,highZ);
							computed.advectionRateKGPerM2S2[component][FaceIndex(shape,component,
								highX,highY,highZ)]=computed.advectionRateKGPerM2S2[component][
								FaceIndex(shape,component,lowX,lowY,lowZ)];
						}
				}
			} else {
				for( unsigned int component=0u;component<3u;++component ) {
					computed.advectionRateKGPerM2S2[component].assign(
						FireProductionProjectionFaceCount(shape,component),0.0f);
					const std::size_t normalCount=AxisCoordinateExtent(shape,component)+1u;
					const std::size_t firstCount=component==0u?shape.ny:shape.nx;
					const std::size_t secondCount=component==2u?shape.ny:shape.nz;
					for( std::size_t second=0u;second<secondCount;++second )
						for( std::size_t first=0u;first<firstCount;++first )
							for( std::size_t normal=0u;normal<normalCount;++normal ) {
								std::size_t x=0u,y=0u,z=0u;
								if( component==0u ){x=normal;y=first;z=second;}
								if( component==1u ){x=first;y=normal;z=second;}
								if( component==2u ){x=first;y=second;z=normal;}
								const std::size_t componentFace=FaceIndex(shape,component,x,y,z);
								float divergence=0.0f;
								for( unsigned int derivative=0u;derivative<3u;++derivative ) {
									if( derivative==component ) {
										const std::size_t previousNormal=normal?normal-1u:normal;
										const std::size_t nextNormal=normal+1u<normalCount?
											normal+1u:normal;
										std::size_t px=x,py=y,pz=z,nx=x,ny=y,nz=z;
										SetAxisCoordinate(component,previousNormal,px,py,pz);
										SetAxisCoordinate(component,nextNormal,nx,ny,nz);
										const std::size_t previousFace=FaceIndex(shape,component,px,py,pz);
										const std::size_t nextFace=FaceIndex(shape,component,nx,ny,nz);
										const float upper=0.25f*(
											computed.acceptedGasFluxKGPerM2S[component][componentFace]+
											computed.acceptedGasFluxKGPerM2S[component][nextFace])*(
											request.frozenVelocityMPerS[component][componentFace]+
											request.frozenVelocityMPerS[component][nextFace]);
										const float lower=0.25f*(
											computed.acceptedGasFluxKGPerM2S[component][previousFace]+
											computed.acceptedGasFluxKGPerM2S[component][componentFace])*(
											request.frozenVelocityMPerS[component][previousFace]+
											request.frozenVelocityMPerS[component][componentFace]);
										const float normalScale=normal==0u||normal+1u==normalCount?
											2.0f:1.0f;
										divergence+=normalScale*(upper-lower)/shape.cellWidthM;
									} else {
										const std::size_t derivativeExtent=AxisCoordinateExtent(shape,derivative);
										const std::size_t derivativePosition=AxisCoordinate(derivative,x,y,z);
										const std::size_t componentExtent=AxisCoordinateExtent(shape,component);
										const std::size_t componentLower=normal?normal-1u:0u;
										const std::size_t componentUpper=normal<componentExtent?
											normal:componentExtent-1u;
										auto derivativeFlux=[&]( const std::size_t componentCell,
											const std::size_t derivativeBoundary ) {
											std::size_t fx=x,fy=y,fz=z;
											SetAxisCoordinate(component,componentCell,fx,fy,fz);
											SetAxisCoordinate(derivative,derivativeBoundary,fx,fy,fz);
											return computed.acceptedGasFluxKGPerM2S[derivative][
												FaceIndex(shape,derivative,fx,fy,fz)];
										};
										auto shiftedVelocity=[&]( const bool upper ) {
											std::size_t vx=x,vy=y,vz=z;
											const std::size_t shifted=upper?std::min(derivativePosition+1u,
												derivativeExtent-1u):(derivativePosition?
												derivativePosition-1u:0u);
											SetAxisCoordinate(derivative,shifted,vx,vy,vz);
											return request.frozenVelocityMPerS[component][
												FaceIndex(shape,component,vx,vy,vz)];
										};
										float upper=0.25f*(derivativeFlux(componentLower,
											derivativePosition+1u)+derivativeFlux(componentUpper,
											derivativePosition+1u))*(
											request.frozenVelocityMPerS[component][componentFace]+
											shiftedVelocity(true));
										float lower=0.25f*(derivativeFlux(componentLower,
											derivativePosition)+derivativeFlux(componentUpper,
											derivativePosition))*(shiftedVelocity(false)+
											request.frozenVelocityMPerS[component][componentFace]);
										if( derivativePosition==0u&&request.boundary[2u*derivative]!=
											FireProductionProjectionPressureOpen ) lower=0.0f;
										if( derivativePosition+1u==derivativeExtent&&
											request.boundary[2u*derivative+1u]!=
											FireProductionProjectionPressureOpen ) upper=0.0f;
										divergence+=(upper-lower)/shape.cellWidthM;
									}
								}
								const bool lowerBoundary=normal==0u;
								const bool upperBoundary=normal+1u==normalCount;
								if( (lowerBoundary||upperBoundary)&&request.boundary[
									2u*component+(upperBoundary?1u:0u)]!=
									FireProductionProjectionPressureOpen ) divergence=0.0f;
								else if( lowerBoundary||upperBoundary ) divergence*=0.5f;
								if( !std::isfinite(divergence) ) return Fail(error,
									"compatible FCT momentum open rate is nonfinite");
								computed.advectionRateKGPerM2S2[component][componentFace]=divergence;
							}
				}
			}
			result=std::move(computed);
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCompatibleFCTMomentumResult();
			FailWithoutThrow(error,"compatible FCT momentum allocation failed");
			return false;
		}
	}

	bool EvaluateFireProductionCompatibleFCTMomentumCPU(
		const FireProductionCompatibleFCTMomentumRequest& request,
		FireProductionCompatibleFCTMomentumResult& result,std::string* error )
	{
		return EvaluateFireProductionCompatibleFCTMomentumCPUImpl(
			request,false,result,error);
	}

	bool EvaluateFireProductionCompatibleFCTMomentumDeltaCPU(
		const FireProductionCompatibleFCTMomentumRequest& request,
		FireProductionCompatibleFCTMomentumResult& result,std::string* error )
	{
		return EvaluateFireProductionCompatibleFCTMomentumCPUImpl(
			request,true,result,error);
	}

	bool EvaluateFireProductionCompatibleHeunMomentumCPU(
		const FireProductionScalarHeunFluxStage& stage,
		const FireProductionScalarHeunSolveResult& solve,
		const std::array<std::vector<float>,3>& frozenVelocityMPerS,
		FireProductionCompatibleFCTMomentumResult& result,std::string* error )
	{
		result=FireProductionCompatibleFCTMomentumResult();
		if(!ValidScalarHeunFluxStage(stage,error))return false;
		if((stage.role!=FireProductionScalarHeunFluxRole::R0&&
			stage.role!=FireProductionScalarHeunFluxRole::R1)||
			HeunVelocityIdentity(frozenVelocityMPerS)!=stage.frozenVelocityIdentity||
			solve.attemptIdentity!=stage.attemptIdentity||
			solve.sharedFCTContractIdentity!=stage.sharedFCTContractIdentity||
			solve.averageCompositionIdentity==0u||
			solve.parentCompositionIdentity[stage.role==
				FireProductionScalarHeunFluxRole::R0?0u:1u]!=stage.compositionIdentity||
			solve.alphaIdentity!=HeunAlphaIdentity(solve.attemptIdentity,
				solve.averageCompositionIdentity,solve.parentCompositionIdentity,
				solve.scalar.sharedFaceAlpha))
			return Fail(error,"compatible Heun momentum stage or velocity identity differs");
		FireProductionCompatibleFCTMomentumRequest request;
		request.shape=stage.compositeFluxPair.shape;
		request.boundary=stage.compositeFluxPair.boundary;
		request.lowGasFluxKGPerM2S=stage.advectiveGasLowFluxKGPerM2S;
		request.highGasFluxKGPerM2S=stage.advectiveGasFluxDeltaKGPerM2S;
		request.physicalGasFluxKGPerM2S=stage.physicalGasFluxKGPerM2S;
		request.sharedFaceAlpha=solve.scalar.sharedFaceAlpha;
		request.frozenVelocityMPerS=frozenVelocityMPerS;
		return EvaluateFireProductionCompatibleFCTMomentumCPUImpl(
			request,true,result,error);
	}

	static bool EvaluateFireProductionScalarFCTStagesCPU(
		const FireProductionScalarFCTRequest& request,
		const FireProductionScalarFCTFluxPair* suppliedFluxPair,
		const bool fluxPairOnly,
		FireProductionScalarFCTResult& result, std::string* error )
	{
		result=FireProductionScalarFCTResult();
		try {
			const FireProductionProjectionShape& shape=request.shape;
			constexpr std::size_t components=9u,inequalities=11u;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!std::isfinite(1.0f/shape.cellWidthM)||
				!(request.timeStepS>0.0f)||!std::isfinite(request.timeStepS)||
				request.nullity==0u||request.nullity>8u||
				request.nullspaceBasis.size()!=8u*request.nullity||
				request.coordinateProjector.size()!=request.nullity*request.nullity||
				!std::isfinite(request.feasibilityFactor)||request.feasibilityFactor<=0.0f||
				!std::isfinite(request.assemblyReserveFactor)||
				request.assemblyReserveFactor<0.0f||
				request.assemblyReserveFactor>request.feasibilityFactor)
				return Fail(error,"scalar FCT shape, schedule, or certificate is invalid");
			const std::size_t cells=shape.CellCount();
			if(request.beginning.size()!=components*cells||
				request.sourceDelta.size()!=components*cells)
				return Fail(error,"scalar FCT cell tuple shape is invalid");
			auto sideFaceCount=[&](const unsigned int side){return side<2u?shape.ny*shape.nz:
				(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);};
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))) return Fail(error,
					"scalar FCT boundary pairing is invalid");
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if(request.frozenVelocityMPerS[axis].size()!=faces)
					return Fail(error,"scalar FCT velocity shape is invalid");
				for(const float velocity:request.frozenVelocityMPerS[axis])
					if(!std::isfinite(velocity))
						return Fail(error,"scalar FCT velocity is nonfinite");
				if(lower==FireProductionProjectionPeriodic&&
					!PeriodicFaceSeamBitEqual(shape,request.frozenVelocityMPerS[axis],axis))
					return Fail(error,"scalar FCT periodic velocity seam is invalid");
			}
			for(unsigned int side=0u;side<6u;++side){
				if(request.pressureOpenInflow[side].size()!=sideFaceCount(side))
					return Fail(error,"scalar FCT inflow shape is invalid");
				for(const unsigned char value:request.pressureOpenInflow[side]){
					if(value>1u)return Fail(error,"scalar FCT inflow value is invalid");
					if(request.boundary[side]!=FireProductionProjectionPressureOpen&&value!=0u)
						return Fail(error,"scalar FCT inactive inflow identity is noncanonical");
				}
			}
			for(const float value:request.beginning)if(!std::isfinite(value))
				return Fail(error,"scalar FCT beginning is nonfinite");
			for(const float value:request.sourceDelta)if(!std::isfinite(value))
				return Fail(error,"scalar FCT source is nonfinite");
			for(const float value:request.ambient)if(!std::isfinite(value))
				return Fail(error,"scalar FCT ambient tuple is nonfinite");
			for(const float value:request.nullspaceBasis)if(!std::isfinite(value))
				return Fail(error,"scalar FCT nullspace basis is nonfinite");
			for(const float value:request.coordinateProjector)if(!std::isfinite(value))
				return Fail(error,"scalar FCT coordinate projector is nonfinite");
			for(const float value:request.enthalpyBoundsJPerKG)if(!std::isfinite(value))
				return Fail(error,"scalar FCT enthalpy bound is nonfinite");

			FireProductionScalarFCTResult computed;
			computed.packedFaceOffset[0]=0u;
			computed.packedFaceOffset[1]=FireProductionProjectionFaceCount(shape,0u);
			computed.packedFaceOffset[2]=computed.packedFaceOffset[1]+
				FireProductionProjectionFaceCount(shape,1u);
			const std::size_t allFaces=computed.packedFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			std::uint64_t outputBytes=0u;
			const std::uint64_t outputValues=2u*components*allFaces+
				2u*components*cells+inequalities*cells+4u*allFaces;
			if(!AddBytes(outputValues,sizeof(float),outputBytes)||
				outputBytes>(std::uint64_t(2u)<<30u))return Fail(error,
				"scalar FCT output exceeds two GiB");
			computed.lowFlux.assign(components*allFaces,0.0f);
			computed.fluxDelta.assign(components*allFaces,0.0f);
			if( suppliedFluxPair ) {
				const FireProductionProjectionShape& suppliedShape=suppliedFluxPair->shape;
				if(suppliedShape.nx!=shape.nx||suppliedShape.ny!=shape.ny||
					suppliedShape.nz!=shape.nz||suppliedShape.cellWidthM!=shape.cellWidthM||
					suppliedFluxPair->timeStepS!=request.timeStepS||
					suppliedFluxPair->boundary!=request.boundary||
					suppliedFluxPair->packedFaceOffset!=computed.packedFaceOffset||
					suppliedFluxPair->lowFlux.size()!=components*allFaces||
					suppliedFluxPair->fluxDelta.size()!=components*allFaces)
					return Fail(error,"scalar FCT supplied flux pair identity is invalid");
				for(const float value:suppliedFluxPair->lowFlux)if(!std::isfinite(value))
					return Fail(error,"scalar FCT supplied low flux is nonfinite");
				for(const float value:suppliedFluxPair->fluxDelta)if(!std::isfinite(value))
					return Fail(error,"scalar FCT supplied flux delta is nonfinite");
				computed.lowFlux=suppliedFluxPair->lowFlux;
				computed.fluxDelta=suppliedFluxPair->fluxDelta;
				for(unsigned int axis=0u;axis<3u;++axis)if(
					request.boundary[2u*axis]==FireProductionProjectionPeriodic){
					const std::size_t extent=AxisCoordinateExtent(shape,axis);
					const std::size_t xEnd=axis==0u?1u:shape.nx;
					const std::size_t yEnd=axis==1u?1u:shape.ny;
					const std::size_t zEnd=axis==2u?1u:shape.nz;
					for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
						for(std::size_t x=0u;x<xEnd;++x){
							std::size_t hx=x,hy=y,hz=z;
							SetAxisCoordinate(axis,extent,hx,hy,hz);
							const std::size_t low=computed.packedFaceOffset[axis]+
								FaceIndex(shape,axis,x,y,z);
							const std::size_t high=computed.packedFaceOffset[axis]+
								FaceIndex(shape,axis,hx,hy,hz);
							for(std::size_t component=0u;component<components;++component){
								const std::size_t lowIndex=component*allFaces+low;
								const std::size_t highIndex=component*allFaces+high;
								if(!SameFloatBits(computed.lowFlux[lowIndex],
									computed.lowFlux[highIndex])||
									!SameFloatBits(computed.fluxDelta[lowIndex],
										computed.fluxDelta[highIndex]))
									return Fail(error,
										"scalar FCT supplied periodic flux seam differs");
							}
						}
				}
			} else {
			auto sideIndex=[&](const unsigned int side,const std::size_t x,
				const std::size_t y,const std::size_t z){return side<2u?z*shape.ny+y:
				(side<4u?z*shape.nx+x:y*shape.nx+x);};
			auto stageValue=[&](const std::size_t component,std::size_t x,std::size_t y,
				std::size_t z,const unsigned int axis,const int shift){
				const int coordinate=static_cast<int>(AxisCoordinate(axis,x,y,z))+shift;
				const int extent=static_cast<int>(AxisCoordinateExtent(shape,axis));
				if(coordinate>=0&&coordinate<extent){SetAxisCoordinate(axis,
					static_cast<std::size_t>(coordinate),x,y,z);return request.beginning[
					component*cells+CellIndex(shape,x,y,z)];}
				const unsigned int side=2u*axis+(coordinate>=extent?1u:0u);
				if(request.boundary[side]==FireProductionProjectionPeriodic){
					SetAxisCoordinate(axis,coordinate<0?AxisCoordinateExtent(shape,axis)-1u:0u,
						x,y,z);return request.beginning[component*cells+CellIndex(shape,x,y,z)];}
				SetAxisCoordinate(axis,coordinate<0?0u:AxisCoordinateExtent(shape,axis)-1u,
					x,y,z);
				const float interior=request.beginning[component*cells+CellIndex(shape,x,y,z)];
				return request.boundary[side]==FireProductionProjectionPressureOpen&&
					request.pressureOpenInflow[side][sideIndex(side,x,y,z)]!=0u?
					request.ambient[component]:interior;
			};
			auto mc=[](const float backward,const float forward)->float{
				if(backward*forward<=0.0f)return 0.0f;
				const float centered=0.5f*(backward+forward);
				const float sign=centered<0.0f?-1.0f:1.0f;
				return sign*std::min(std::fabs(centered),2.0f*std::min(
					std::fabs(backward),std::fabs(forward)));
			};
			auto massSlope=[&](const std::size_t component,const std::size_t x,
				const std::size_t y,const std::size_t z,const unsigned int axis){
				std::array<float,8> coordinateSlope={{}};
				for(std::size_t basis=0u;basis<request.nullity;++basis){
					float backward=0.0f,forward=0.0f;
					for(std::size_t row=0u;row<8u;++row){
						const float center=request.beginning[row*cells+CellIndex(shape,x,y,z)];
						const float coefficient=request.nullspaceBasis[
							row*request.nullity+basis];
						backward+=coefficient*(center-stageValue(row,x,y,z,axis,-1));
						forward+=coefficient*(stageValue(row,x,y,z,axis,1)-center);
					}
					coordinateSlope[basis]=mc(backward,forward);
				}
				float slope=0.0f;
				for(std::size_t basis=0u;basis<request.nullity;++basis){
					float projected=0.0f;
					for(std::size_t column=0u;column<request.nullity;++column)
						projected+=request.coordinateProjector[
							basis*request.nullity+column]*coordinateSlope[column];
					slope+=request.nullspaceBasis[component*request.nullity+basis]*projected;
				}
				return slope;
			};
			for(std::size_t component=0u;component<components;++component)
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t extent=AxisCoordinateExtent(shape,axis);
					const std::size_t xEnd=shape.nx+(axis==0u?1u:0u);
					const std::size_t yEnd=shape.ny+(axis==1u?1u:0u);
					const std::size_t zEnd=shape.nz+(axis==2u?1u:0u);
					for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
						for(std::size_t x=0u;x<xEnd;++x){
							const std::size_t face=FaceIndex(shape,axis,x,y,z);
							const std::size_t packed=computed.packedFaceOffset[axis]+face;
							const std::size_t output=component*allFaces+packed;
							const std::size_t coordinate=AxisCoordinate(axis,x,y,z);
							const float velocity=request.frozenVelocityMPerS[axis][face];
							if((coordinate==0u||coordinate==extent)&&request.boundary[
								2u*axis+(coordinate==extent?1u:0u)]==
								FireProductionProjectionWall)continue;
							if((coordinate==0u||coordinate==extent)&&request.boundary[
								2u*axis+(coordinate==extent?1u:0u)]!=
								FireProductionProjectionPeriodic){
								const int donorShift=velocity>=0.0f?-1:0;
								const float donor=stageValue(component,x,y,z,axis,donorShift);
								computed.lowFlux[output]=velocity*donor;continue;
							}
							const std::size_t rightCoordinate=coordinate==extent?0u:coordinate;
							const std::size_t leftCoordinate=rightCoordinate==0u?extent-1u:
								rightCoordinate-1u;
							std::size_t lx=x,ly=y,lz=z,rx=x,ry=y,rz=z;
							SetAxisCoordinate(axis,leftCoordinate,lx,ly,lz);
							SetAxisCoordinate(axis,rightCoordinate,rx,ry,rz);
							const bool fromLeft=velocity>=0.0f;
							const std::size_t dx=fromLeft?lx:rx,dy=fromLeft?ly:ry,
								dz=fromLeft?lz:rz;
							const float donor=request.beginning[
								component*cells+CellIndex(shape,dx,dy,dz)];
							const float slope=component<8u?massSlope(component,dx,dy,dz,axis):
								mc(donor-stageValue(component,dx,dy,dz,axis,-1),
									stageValue(component,dx,dy,dz,axis,1)-donor);
							const float high=donor+(fromLeft?0.5f:-0.5f)*slope;
							computed.lowFlux[output]=velocity*donor;
							computed.fluxDelta[output]=velocity*(high-donor);
						}
				}
			}
			for(const float value:computed.lowFlux)if(!std::isfinite(value))
				return Fail(error,"scalar FCT low flux is nonfinite");
			for(const float value:computed.fluxDelta)if(!std::isfinite(value))
				return Fail(error,"scalar FCT flux delta is nonfinite");
			if( fluxPairOnly ) {
				result=std::move(computed);
				if(error)error->clear();
				return true;
			}

			auto packedCellFace=[&](const std::size_t cell,const unsigned int axis,
				const bool upper){std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
					z=cell/(shape.nx*shape.ny);
				if(upper)SetAxisCoordinate(axis,AxisCoordinate(axis,x,y,z)+1u,x,y,z);
				return computed.packedFaceOffset[axis]+FaceIndex(shape,axis,x,y,z);
			};
			auto inequalityValue=[&](const float* value,const std::size_t inequality){
				if(inequality==0u)return -value[0];
				if(inequality==1u){
					float closure=value[0];
					for(std::size_t species=0u;species<7u;++species)
						closure-=value[1u+species];
					return closure;
				}
				if(inequality<9u)return -value[inequality-1u];
				if(inequality==9u){
					float lower=-value[8];
					for(std::size_t species=0u;species<7u;++species)
						lower+=request.enthalpyBoundsJPerKG[species]*value[1u+species];
					return lower;
				}
				float upper=value[8];for(std::size_t species=0u;species<7u;++species)
					upper-=request.enthalpyBoundsJPerKG[7u+species]*value[1u+species];
				return upper;
			};
			computed.lowState.assign(components*cells,0.0f);
			std::array<std::vector<float>,6> correction;
			for(auto& direction:correction)direction.assign(components*cells,0.0f);
			const float scale=request.timeStepS/shape.cellWidthM;
			for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=0u;
				component<components;++component){
				float low=request.beginning[component*cells+cell]+
					request.sourceDelta[component*cells+cell];
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t lower=packedCellFace(cell,axis,false);
					const std::size_t upper=packedCellFace(cell,axis,true);
					low+=scale*(computed.lowFlux[component*allFaces+lower]-
						computed.lowFlux[component*allFaces+upper]);
					correction[2u*axis][component*cells+cell]=scale*
						computed.fluxDelta[component*allFaces+lower];
					correction[2u*axis+1u][component*cells+cell]=-scale*
						computed.fluxDelta[component*allFaces+upper];
				}
				if(!std::isfinite(low))return Fail(error,"scalar FCT low state is nonfinite");
				computed.lowState[component*cells+cell]=low;
			}
			for(std::size_t cell=0u;cell<cells;++cell){
				std::array<float,9> low={{}};float rowScale=1.0f;
				for(std::size_t component=0u;component<components;++component){
					low[component]=computed.lowState[component*cells+cell];
					rowScale+=std::fabs(low[component]);
				}
				for(std::size_t inequality=0u;inequality<inequalities;++inequality){
					const float excess=inequalityValue(low.data(),inequality);
					if(!std::isfinite(excess)||excess>request.feasibilityFactor*rowScale)
						return Fail(error,"scalar FCT low state exceeds r60 envelope");
				}
			}
			computed.limiterRatio.assign(inequalities*cells,1.0f);
			for(std::size_t inequality=0u;inequality<inequalities;++inequality)
				for(std::size_t cell=0u;cell<cells;++cell){
					std::array<float,9> low={{}};std::array<std::array<float,9>,6> local={{}};
					for(std::size_t component=0u;component<components;++component){
						low[component]=computed.lowState[component*cells+cell];
						for(unsigned int direction=0u;direction<6u;++direction)
							local[direction][component]=correction[direction][component*cells+cell];
					}
					float rowScale=0.0f;
					for(std::size_t component=0u;component<components;++component){
						float lower=low[component],upper=low[component];
						for(unsigned int direction=0u;direction<6u;++direction){
							const float delta=local[direction][component];
							if(delta<0.0f)lower+=delta;else upper+=delta;
						}
						const float minimum=lower<=0.0f&&upper>=0.0f?0.0f:
							std::min(std::fabs(lower),std::fabs(upper));
						if(inequality<9u){if(component<8u)rowScale+=minimum;}
						else if(component==8u)rowScale+=minimum;
						else if(component>0u&&component<8u)rowScale+=(std::fabs(
							request.enthalpyBoundsJPerKG[component-1u])+std::fabs(
							request.enthalpyBoundsJPerKG[7u+component-1u]))*minimum;
					}
					rowScale=std::max(1.0f,rowScale);float requested=0.0f;
					for(unsigned int direction=0u;direction<6u;++direction)requested+=
						std::max(0.0f,inequalityValue(local[direction].data(),inequality));
					const float budget=std::max(0.0f,(request.feasibilityFactor-
						request.assemblyReserveFactor)*rowScale-
						inequalityValue(low.data(),inequality));
					const float ratio=requested>0.0f?std::min(1.0f,budget/requested):1.0f;
					if(!std::isfinite(ratio)||ratio<0.0f||ratio>1.0f)return Fail(error,
						"scalar FCT limiter ratio is invalid");
					computed.limiterRatio[inequality*cells+cell]=ratio;
				}
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faceCount=FireProductionProjectionFaceCount(shape,axis);
				computed.sharedFaceAlpha[axis].assign(faceCount,1.0f);
				const std::size_t extent=AxisCoordinateExtent(shape,axis);
				const std::size_t xEnd=shape.nx+(axis==0u?1u:0u),
					yEnd=shape.ny+(axis==1u?1u:0u),zEnd=shape.nz+(axis==2u?1u:0u);
				for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
					for(std::size_t x=0u;x<xEnd;++x){
						const std::size_t face=FaceIndex(shape,axis,x,y,z);
						const std::size_t packed=computed.packedFaceOffset[axis]+face;
						const std::size_t coordinate=AxisCoordinate(axis,x,y,z);
						const bool periodic=request.boundary[2u*axis]==
							FireProductionProjectionPeriodic;
						const bool haveLeft=periodic||coordinate>0u;
						const bool haveRight=periodic||coordinate<extent;
						std::size_t lx=x,ly=y,lz=z,rx=x,ry=y,rz=z;
						SetAxisCoordinate(axis,coordinate?coordinate-1u:extent-1u,lx,ly,lz);
						SetAxisCoordinate(axis,coordinate==extent?0u:coordinate,rx,ry,rz);
						const std::size_t left=haveLeft?CellIndex(shape,lx,ly,lz):0u;
						const std::size_t right=haveRight?CellIndex(shape,rx,ry,rz):0u;
						float accepted=1.0f;std::array<float,9> faceCorrection={{}};
						for(std::size_t inequality=0u;inequality<inequalities;++inequality){
							for(std::size_t component=0u;component<components;++component)
								faceCorrection[component]=-scale*computed.fluxDelta[
									component*allFaces+packed];
							if(haveLeft&&inequalityValue(faceCorrection.data(),inequality)>0.0f)
								accepted=std::min(accepted,computed.limiterRatio[
									inequality*cells+left]);
							for(float& value:faceCorrection)value=-value;
							if(haveRight&&inequalityValue(faceCorrection.data(),inequality)>0.0f)
								accepted=std::min(accepted,computed.limiterRatio[
									inequality*cells+right]);
						}
						if(!std::isfinite(accepted)||accepted<0.0f||accepted>1.0f)
							return Fail(error,"scalar FCT shared alpha is invalid");
						computed.sharedFaceAlpha[axis][face]=accepted;
					}
			}
			computed.accepted=computed.lowState;
			for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=0u;
				component<components;++component){
				float value=computed.lowState[component*cells+cell];
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t lower=packedCellFace(cell,axis,false);
					const std::size_t upper=packedCellFace(cell,axis,true);
					value+=scale*(computed.sharedFaceAlpha[axis][lower-
						computed.packedFaceOffset[axis]]*computed.fluxDelta[
						component*allFaces+lower]-computed.sharedFaceAlpha[axis][upper-
						computed.packedFaceOffset[axis]]*computed.fluxDelta[
						component*allFaces+upper]);
				}
				if(!std::isfinite(value))return Fail(error,"scalar FCT accepted state is nonfinite");
				computed.accepted[component*cells+cell]=value;
			}
			for(std::size_t cell=0u;cell<cells;++cell){
				std::array<float,9> value={{}};float rowScale=1.0f;
				for(std::size_t component=0u;component<components;++component){
					value[component]=computed.accepted[component*cells+cell];
					rowScale+=std::fabs(value[component]);
				}
				for(std::size_t inequality=0u;inequality<inequalities;++inequality){
					const float excess=inequalityValue(value.data(),inequality);
					if(!std::isfinite(excess)||excess>request.feasibilityFactor*rowScale)
						return Fail(error,"scalar FCT accepted state exceeds r60 envelope");
				}
			}
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				computed.acceptedGasFluxKGPerM2S[axis].assign(faces,0.0f);
				for(std::size_t face=0u;face<faces;++face){
					const std::size_t packed=computed.packedFaceOffset[axis]+face;
					float gas=0.0f;
					for(std::size_t component=1u;component<=6u;++component)
						gas+=computed.lowFlux[component*allFaces+packed]+
							computed.sharedFaceAlpha[axis][face]*computed.fluxDelta[
								component*allFaces+packed];
					if(!std::isfinite(gas))return Fail(error,"scalar FCT accepted gas flux is nonfinite");
					computed.acceptedGasFluxKGPerM2S[axis][face]=gas;
				}
			}

			std::vector<float> baseGas(cells,0.0f),acceptedGas(cells,0.0f);
			float ambientGas=0.0f;
			for(std::size_t component=1u;component<=6u;++component)
				ambientGas+=request.ambient[component];
			for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=1u;
				component<=6u;++component){baseGas[cell]+=request.beginning[component*cells+cell]+
					request.sourceDelta[component*cells+cell];
				acceptedGas[cell]+=computed.accepted[component*cells+cell];}
			FireProductionCompatibleFCTMomentumRequest identityRequest;
			identityRequest.shape=shape;identityRequest.boundary=request.boundary;
			for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
				FireProductionProjectionFaceCount(shape,axis);
				identityRequest.lowGasFluxKGPerM2S[axis]=computed.acceptedGasFluxKGPerM2S[axis];
				identityRequest.highGasFluxKGPerM2S[axis]=computed.acceptedGasFluxKGPerM2S[axis];
				identityRequest.sharedFaceAlpha[axis].assign(faces,0.0f);
				identityRequest.frozenVelocityMPerS[axis].assign(faces,1.0f);
			}
			FireProductionCompatibleFCTMomentumResult identityRate;
			if(!EvaluateFireProductionCompatibleFCTMomentumCPU(identityRequest,identityRate,error))
				return false;
			computed.commutingIdentityAvailable=true;
			for(unsigned int component=0u;component<3u;++component){
				const std::size_t xEnd=shape.nx+(component==0u?1u:0u),
					yEnd=shape.ny+(component==1u?1u:0u),
					zEnd=shape.nz+(component==2u?1u:0u),
					extent=AxisCoordinateExtent(shape,component);
				for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
					for(std::size_t x=0u;x<xEnd;++x){const std::size_t normal=
						AxisCoordinate(component,x,y,z),face=FaceIndex(shape,component,x,y,z);
						const bool boundaryFace=normal==0u||normal==extent;
						const unsigned int side=2u*component+(normal==extent?1u:0u);
						if(boundaryFace&&request.boundary[side]==FireProductionProjectionWall){
							std::uint32_t rateBits=0u;std::memcpy(&rateBits,
								&identityRate.advectionRateKGPerM2S2[component][face],sizeof(rateBits));
							if(rateBits!=0u)return Fail(error,
								"scalar FCT wall commuting rate is not positive zero");
							continue;
						}
						auto restricted=[&](const std::vector<float>& gas){
							std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
							if(!boundaryFace){SetAxisCoordinate(component,normal-1u,
								lowX,lowY,lowZ);return 0.5f*(gas[CellIndex(shape,lowX,lowY,lowZ)]+
								gas[CellIndex(shape,highX,highY,highZ)]);}
							if(request.boundary[side]==FireProductionProjectionPeriodic){
								SetAxisCoordinate(component,extent-1u,lowX,lowY,lowZ);
								SetAxisCoordinate(component,0u,highX,highY,highZ);
								return 0.5f*(gas[CellIndex(shape,lowX,lowY,lowZ)]+
									gas[CellIndex(shape,highX,highY,highZ)]);}
							SetAxisCoordinate(component,normal==extent?extent-1u:0u,
								highX,highY,highZ);return 0.5f*(gas[CellIndex(shape,highX,highY,highZ)]+
								ambientGas);
						};
						const float base=restricted(baseGas),acceptedValue=restricted(acceptedGas),
							advanced=base-request.timeStepS*
							identityRate.advectionRateKGPerM2S2[component][face];
						computed.commutingIdentityScaleKGPerM3=std::max(
							computed.commutingIdentityScaleKGPerM3,std::max(std::fabs(base),
							std::fabs(acceptedValue)));
						const float residual=std::fabs(acceptedValue-advanced);
						if(residual>computed.maximumCommutingResidualKGPerM3){
							computed.maximumCommutingResidualKGPerM3=residual;
							computed.commutingIdentityRestrictedAcceptedKGPerM3=acceptedValue;
							computed.commutingIdentityAdvancedKGPerM3=advanced;
							computed.commutingIdentityComponent=component;
							computed.commutingIdentityFace=face;
						}
					}
			}
			result=std::move(computed);if(error)error->clear();return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarFCTResult();
			FailWithoutThrow(error,"scalar FCT allocation failed");return false;}
	}

	bool BuildFireProductionScalarFCTFluxPairCPU(
		const FireProductionScalarFCTRequest& request,
		FireProductionScalarFCTFluxPair& result, std::string* error )
	{
		result=FireProductionScalarFCTFluxPair();
		FireProductionScalarFCTResult staged;
		if(!EvaluateFireProductionScalarFCTStagesCPU(request,0,true,staged,error))return false;
		result.shape=request.shape;
		result.timeStepS=request.timeStepS;
		result.boundary=request.boundary;
		result.packedFaceOffset=staged.packedFaceOffset;
		result.lowFlux=std::move(staged.lowFlux);
		result.fluxDelta=std::move(staged.fluxDelta);
		if(error)error->clear();
		return true;
	}

	bool AverageFireProductionScalarFCTFluxPairsCPU(
		const FireProductionScalarFCTFluxPair& first,
		const FireProductionScalarFCTFluxPair& second,
		FireProductionScalarFCTFluxPair& result, std::string* error )
	{
		result=FireProductionScalarFCTFluxPair();
		try {
			const FireProductionProjectionShape& a=first.shape;
			const FireProductionProjectionShape& b=second.shape;
			if(a.nx<4u||a.nx>1024u||a.ny<4u||a.ny>1024u||
				a.nz<4u||a.nz>1024u||!(a.cellWidthM>0.0f)||
				!std::isfinite(a.cellWidthM)||a.nx!=b.nx||a.ny!=b.ny||a.nz!=b.nz||
				a.cellWidthM!=b.cellWidthM||!(first.timeStepS>0.0f)||
				!std::isfinite(first.timeStepS)||first.timeStepS!=second.timeStepS||
				first.boundary!=second.boundary)
				return Fail(error,"scalar FCT stage flux pair identity is invalid");
			std::array<std::size_t,3> expectedOffset={{0u,
				FireProductionProjectionFaceCount(a,0u),0u}};
			expectedOffset[2]=expectedOffset[1]+FireProductionProjectionFaceCount(a,1u);
			const std::size_t allFaces=expectedOffset[2]+
				FireProductionProjectionFaceCount(a,2u);
			if(first.packedFaceOffset!=expectedOffset||
				second.packedFaceOffset!=expectedOffset||
				first.lowFlux.size()!=9u*allFaces||
				first.fluxDelta.size()!=9u*allFaces||
				second.lowFlux.size()!=9u*allFaces||
				second.fluxDelta.size()!=9u*allFaces)
				return Fail(error,"scalar FCT stage flux pair identity is invalid");
			result.shape=a;
			result.timeStepS=first.timeStepS;
			result.boundary=first.boundary;
			result.packedFaceOffset=first.packedFaceOffset;
			result.lowFlux.resize(first.lowFlux.size());
			result.fluxDelta.resize(first.fluxDelta.size());
			for(std::size_t value=0u;value<first.lowFlux.size();++value){
				const float low=0.5f*(first.lowFlux[value]+second.lowFlux[value]);
				const float delta=0.5f*(first.fluxDelta[value]+second.fluxDelta[value]);
				if(!std::isfinite(low)||!std::isfinite(delta)){
					result=FireProductionScalarFCTFluxPair();
					return Fail(error,"scalar FCT averaged stage flux is nonfinite");
				}
				result.lowFlux[value]=low;
				result.fluxDelta[value]=delta;
			}
			if(error)error->clear();
			return true;
		} catch(const std::bad_alloc&){result=FireProductionScalarFCTFluxPair();
			FailWithoutThrow(error,"scalar FCT flux average allocation failed");return false;}
	}

	bool SolveFireProductionScalarFCTFluxPairCPU(
		const FireProductionScalarFCTRequest& request,
		const FireProductionScalarFCTFluxPair& fluxPair,
		FireProductionScalarFCTResult& result, std::string* error )
	{
		return EvaluateFireProductionScalarFCTStagesCPU(
			request,&fluxPair,false,result,error);
	}

	bool EvaluateFireProductionScalarFCTCPU(
		const FireProductionScalarFCTRequest& request,
		FireProductionScalarFCTResult& result, std::string* error )
	{
		FireProductionScalarFCTFluxPair fluxPair;
		if(!BuildFireProductionScalarFCTFluxPairCPU(request,fluxPair,error)){
			result=FireProductionScalarFCTResult();
			return false;
		}
		return SolveFireProductionScalarFCTFluxPairCPU(request,fluxPair,result,error);
	}
}
