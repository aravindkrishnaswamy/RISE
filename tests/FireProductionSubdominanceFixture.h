#ifndef FIRE_PRODUCTION_SUBDOMINANCE_FIXTURE_H
#define FIRE_PRODUCTION_SUBDOMINANCE_FIXTURE_H

namespace FireProductionDyadicCalibration
{
	template<class T> bool UnpackProductionConservative(const std::vector<T>& packed,
		const std::size_t cells,std::vector<ConservativeVector>& result)
	{
		result.clear();if(cells==0u||packed.size()!=9u*cells)return false;
		result.assign(cells,ConservativeVector());
		for(std::size_t cell=0u;cell<cells;++cell)
			for(std::size_t component=0u;component<9u;++component){
				const double value=static_cast<double>(packed[component*cells+cell]);
				if(!std::isfinite(value))return false;result[cell][component]=value;
			}
		return true;
	}

	template<class T> bool UnpackProductionVelocity(
		const std::array<std::vector<T>,3>& packed,PeriodicMACField& result)
	{
		result=PeriodicMACField();
		for(unsigned int axis=0u;axis<3u;++axis){
			result.component[axis].assign(packed[axis].begin(),packed[axis].end());
			if(!std::all_of(result.component[axis].begin(),result.component[axis].end(),
				[](const double value){return std::isfinite(value);}))return false;
		}
		return true;
	}

	int MeasureProductionSubdominance(const std::filesystem::path& directory,
		const char* expectedProtocol,const char* expectedTargets,
		const std::filesystem::path& subdominanceProtocol,const char* expectedSubdominance)
	{
		static const char* GoldenDigest=
			"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
		if(!expectedProtocol||!expectedTargets||!expectedSubdominance||
			std::strlen(expectedProtocol)!=64u||std::strlen(expectedTargets)!=64u||
			std::strlen(expectedSubdominance)!=64u||
			DigestFile(directory/"dyadic_protocol.v1")!=expectedProtocol||
			DigestFile(directory/"dyadic_targets.v1")!=expectedTargets||
			DigestFile(subdominanceProtocol)!=expectedSubdominance||
			DigestFile("rendered/fire_methane_capstone/tier10.run.checkpoint")!=GoldenDigest)
			return 244;
		std::array<std::string,4> targetDigests;
		if(!ReadTargetDigests(directory/"dyadic_targets.v1",targetDigests))return 245;
		const std::size_t tierIndex=2u;MethaneRunCheckpoint geometry;std::string error;
		if(Tiers[tierIndex]!=6u||!BuildAnalyticState(6u,geometry,error))return 246;
		std::vector<std::vector<double> > sealed;
		const std::filesystem::path target=directory/"oracle_tier6_sdiv_x8.f64";
		if(DigestFile(target)!=targetDigests[tierIndex]||!ReadCalibrationDoublePayload(
			target,geometry.states.size(),8u,sealed))return 247;
		std::array<double,9> scalarDistance={{}},scalarBound={{}},inventoryDistance={{}},
			inventoryBound={{}};double velocityDistance=0.0,velocityBound=0.0;
		for(std::size_t component=0u;component<9u;++component)
			if(!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
				ExpectedProductionScalarEvidence[component][0],
				ExpectedProductionScalarEvidence[component][1],VerifiedOrder,
				scalarDistance[component],scalarBound[component])||
				!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
				ExpectedProductionInventoryEvidence[component][0],
				ExpectedProductionInventoryEvidence[component][1],VerifiedOrder,
				inventoryDistance[component],inventoryBound[component]))return 248;
		if(!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
			ExpectedProductionVelocityEvidence[0],ExpectedProductionVelocityEvidence[1],
			VerifiedOrder,velocityDistance,velocityBound))return 248;
		static const std::array<double,9> ExpectedScalar={{
			4.2072923741404349e-10,3.8028058179413025e-10,1.6655732775263646e-09,
			7.2615928643090542e-09,2.2307379249055176e-10,2.0911832754768065e-10,
			3.3266140763578376e-12,6.2593004501949552e-13,0.00036797249426876111}};
		static const std::array<double,9> ExpectedInventory={{
			7.1521330524682014e-12,1.7611793967642342e-11,3.1616376183762895e-12,
			7.8709760931161554e-10,6.8423461341282632e-12,7.3172613801464337e-12,
			6.6908013417882328e-14,4.4276533123482203e-14,2.9797323804814368e-05}};
		static const std::array<double,8> ExpectedVelocity={{
			1.1126672816676097e-09,1.0956868019298051e-09,1.1052013355606296e-09,
			1.1122253318560699e-09,1.1159243123152109e-09,1.1165273069908068e-09,
			1.1016848904627674e-09,1.1153024226446355e-09}};
		std::vector<unsigned char> trace;
		for(std::size_t step=0u;step<sealed.size();++step){
			MethaneRunCheckpoint beginning;if(!BuildAnalyticState(6u,beginning,error)||
				AnalyticStateDigest(beginning)!=AnalyticStateDigest(geometry))return 249;
			const double flowThrough=6.0*std::sqrt(
				beginning.values.characteristicDiameterM/Gravity);
			RISE::FireProductionResidentStepRequest request;
			if(!BuildProductionRequest(beginning,sealed[step],flowThrough/512.0,
				request,error))return 250;
			RISE::FireProductionResidentStepResult production32;
			if(!RISE::AdvanceFireProductionResidentStepMetal(request,production32,&error)){
				std::fprintf(stderr,"subdominance Metal slice=%zu failed: %s\n",step,
					error.c_str());return 251;}
			if(production32.conservativeProducerPrecision!=
					FireStateProducerPrecision::Binary32||
				production32.residentProjectionInvocationCount!=2u||
				production32.interstageFullGridTransferCount!=0u||
				!production32.physicalProjection.validationPassed||
				!production32.projection.validationPassed||
				production32.forceDiagnostics.outwardLambdaPerS<=0.0f)return 252;
			::FireProductionCalibration::ResidentStep64Result production64;
			if(!::FireProductionCalibration::AdvanceResidentStep64(request,
				static_cast<double>(production32.forceDiagnostics.outwardLambdaPerS),
				production64,&error)){
				std::fprintf(stderr,"subdominance fp64 slice=%zu failed: %s\n",step,
					error.c_str());return 253;}
			if(production64.force.schedule.substepCount!=production32.forceSchedule.substepCount||
				!production64.physicalProjection.validationPassed||
				!production64.projection.validationPassed||
				production64.physicalProjection.executedVCycleCount!=
					production32.physicalProjection.executedVCycleCount||
				production64.projection.executedVCycleCount!=
					production32.projection.executedVCycleCount)return 254;
			std::vector<ConservativeVector> conservative32,conservative64;
			PeriodicMACField velocity32,velocity64;
			FilteredField filtered32,filtered64;
			FilteredVelocityField filteredVelocity32,filteredVelocity64;
			if(!UnpackProductionConservative(production32.conservativeValues,
				beginning.states.size(),conservative32)||
				!UnpackProductionConservative(production64.conservativeValues,
				beginning.states.size(),conservative64)||
				!UnpackProductionVelocity(production32.projection.velocityMPerS,velocity32)||
				!UnpackProductionVelocity(production64.projection.velocityMPerS,velocity64)||
				!FilterConservative(beginning,conservative32,
					beginning.values.characteristicDiameterM,filtered32)||
				!FilterConservative(beginning,conservative64,
					beginning.values.characteristicDiameterM,filtered64)||
				!FilterVelocity(beginning,velocity32,beginning.values.characteristicDiameterM,
					filteredVelocity32)||
				!FilterVelocity(beginning,velocity64,beginning.values.characteristicDiameterM,
					filteredVelocity64))return 255;
			const std::array<double,9> scalar=FieldDistance(filtered32,filtered64);
			const double velocity=VelocityDistance(filteredVelocity32,filteredVelocity64);
			const std::array<double,9> inventory32=ComponentInventoryDensity(conservative32);
			const std::array<double,9> inventory64=ComponentInventoryDensity(conservative64);
			std::array<double,9> inventory={{}};
			for(std::size_t component=0u;component<9u;++component){
				inventory[component]=std::fabs(inventory32[component]-inventory64[component]);
				if(!(scalar[component]<=scalarBound[component])||
					!(inventory[component]<=inventoryBound[component]))return 256;
				AppendDouble(trace,scalar[component]);AppendDouble(trace,inventory[component]);
			}
			if(scalar!=ExpectedScalar||inventory!=ExpectedInventory||
				velocity!=ExpectedVelocity[step]||!(velocity<=velocityBound))return 256;
			AppendDouble(trace,velocity);
			AppendInteger(trace,production32.forceSchedule.substepCount);
			AppendInteger(trace,production32.residentProjectionInvocationCount);
			std::fprintf(stderr,"subdominance slice=%zu velocity=%.17g bound=%.17g "
				"margin=%.17g scalar=",step,velocity,velocityBound,velocity>0.0?
				velocityBound/velocity:std::numeric_limits<double>::infinity());
			for(const double value:scalar)std::fprintf(stderr," %.17g",value);
			std::fprintf(stderr," inventory=");
			for(const double value:inventory)std::fprintf(stderr," %.17g",value);
			std::fprintf(stderr,"\n");
		}
		const std::string traceDigest=RISECBOR64::SHA256Hex(trace);
		std::fprintf(stderr,"subdominance complete trace=%s slices=%zu\n",
			traceDigest.c_str(),sealed.size());
		return traceDigest=="f90a2508803f769665e68fc2c10e7672ea5f7ee5bf647fa2b8ff8b227551cebf"&&
			DigestFile("rendered/fire_methane_capstone/tier10.run.checkpoint")==GoldenDigest?
			243:257;
	}
}

#endif
