#include "fire_production_fp64/SourceManifest.h"

int RunProductionCalibrationStateGeneration(const std::filesystem::path& outputDirectory)
{
	static const char* goldenCheckpoint=
		"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
	static const std::array<const char*,8> goldenBeginnings={{
		"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947",
		"e27a165101c2f7b1c4f64228ddd3d8915df6b2df4f3d3f1c9d1709c6be3e6ca6",
		"a2bb82402afca8cade69010b050cafd0c007caadfc4c55c753991c1c09f41ebb",
		"1b3a1b70f79356174624c331ffbf1a19af51e03a6c0b07cc32223cdcf4a32b81",
		"4f289f0eb239acf3834a3592dee08557f7fd614e52d9a4cbbf0ba525c40892e2",
		"9933b5711d57765c02dd7d0ed06578ff2786aef88a69dcd7f83f584f5fd9f74e",
		"39332f556e068646e3a649ab8a8a34c495daff0e280600cf13600d192b07e726",
		"000a7fe24c64b00a61ec6256b75d0403a193100752abacb39e9f68149f20402b"}};
	if(std::filesystem::exists(outputDirectory)){
		std::fprintf(stderr,"calibration input directory already exists: %s\n",
			outputDirectory.string().c_str());return 130;
	}
	std::error_code directoryError;
	if(!std::filesystem::create_directories(outputDirectory,directoryError)||directoryError){
		std::fprintf(stderr,"calibration input directory creation failed: %s\n",
			directoryError.message().c_str());return 131;
	}
	const std::array<double,3> tiers={{5.0,6.0,7.0}};
	std::array<std::filesystem::path,3> paths;
	for(std::size_t index=0u;index<tiers.size();++index){
		paths[index]=outputDirectory/(std::string("tier")+
			std::to_string(static_cast<unsigned int>(tiers[index]))+"_t0p32.checkpoint");
		RunPersistenceOptions persistence;persistence.finalCheckpointPath=paths[index];
		const SolverFrameValues values=RunMethaneFrameProbe(16u,1u,0.32,1.0,4.0,
			tiers[index],CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		if(!values.succeeded||values.simulatedTimeS!=0.32||!std::filesystem::exists(paths[index])){
			std::fprintf(stderr,"tier %.0f calibration state failed: %s time=%.17g\n",
				tiers[index],values.structuredError.c_str(),values.simulatedTimeS);return 132;
		}
	}
	const std::filesystem::path partial=outputDirectory/"input_manifest.v1.partial";
	const std::filesystem::path manifest=outputDirectory/"input_manifest.v1";
	std::array<MethaneRunCheckpoint,3> states;
	double commonX=std::numeric_limits<double>::infinity();
	double commonY=std::numeric_limits<double>::infinity();
	double commonZ=std::numeric_limits<double>::infinity();
	for(std::size_t index=0u;index<paths.size();++index){
		std::string error;
		if(!LoadMethaneRunCheckpoint(paths[index],states[index],error))return 134;
		commonX=std::min(commonX,states[index].cellWidthM*states[index].dimensions[0]);
		commonY=std::min(commonY,states[index].cellWidthM*states[index].dimensions[1]);
		commonZ=std::min(commonZ,states[index].cellWidthM*states[index].dimensions[2]);
	}
	std::ofstream output(partial,std::ios::binary|std::ios::trunc);
	if(!output)return 133;
	output<<"fire_production_calibration_input_v1\nphysical_time_s 0.32\n"
		<<"baseline_horizon_s 0.002\ntemporal_steps_s 0.002 0.001 0.0005\n"
		<<"formal_order production_space 2 production_time 1 oracle_space 2 oracle_time 2\n"
		<<"horizontal_alignment domain_center\nvertical_alignment common_floor\n"
		<<"common_support_m "<<std::setprecision(17)<<-0.5*commonX<<' '<<0.5*commonX
		<<' '<<-0.5*commonY<<' '<<0.5*commonY<<" 0 "<<commonZ<<'\n'
		<<"metric_registry component_volume_l1,component_integral,velocity_volume_l2,projection_residual\n"
		<<"golden_checkpoint_sha256 "<<goldenCheckpoint<<'\n';
	for(std::size_t index=0u;index<goldenBeginnings.size();++index)
		output<<"golden_beginning_"<<index<<"_sha256 "<<goldenBeginnings[index]<<'\n';
	output<<"mirror_generator_sha256 "<<RISEFireProductionFP64::SourceManifest::Generator<<'\n'
		<<"advection_source_sha256 "
		<<RISEFireProductionFP64::SourceManifest::FireProductionAdvectionSource<<'\n'
		<<"transport_source_sha256 "
		<<RISEFireProductionFP64::SourceManifest::FireProductionTransportSource<<'\n'
		<<"force_source_sha256 "
		<<RISEFireProductionFP64::SourceManifest::FireProductionForceSource<<'\n'
		<<"projection_source_sha256 "
		<<RISEFireProductionFP64::SourceManifest::FireProductionProjectionSource<<'\n';
	for(std::size_t index=0u;index<paths.size();++index){
		const MethaneRunCheckpoint& state=states[index];
		output<<"tier "<<static_cast<unsigned int>(tiers[index])<<" shape "
			<<state.dimensions[0]<<' '<<state.dimensions[1]<<' '<<state.dimensions[2]
			<<" dx "<<std::setprecision(17)<<state.cellWidthM<<" sha256 "
			<<DigestFile(paths[index])<<" case "<<state.caseRecordId<<'\n';
	}
	output.flush();output.close();
	if(!output||std::filesystem::exists(manifest))return 135;
	std::filesystem::rename(partial,manifest,directoryError);
	if(directoryError)return 136;
	std::fprintf(stderr,"calibration inputs sealed manifest_sha256=%s\n",
		DigestFile(manifest).c_str());return 0;
}

int SealExistingProductionCalibrationInputs(const std::filesystem::path& inputDirectory)
{
	const std::array<unsigned int,3> tiers={{5u,6u,7u}};
	std::array<std::filesystem::path,3> paths;
	std::array<MethaneRunCheckpoint,3> states;
	double commonX=std::numeric_limits<double>::infinity();
	double commonY=std::numeric_limits<double>::infinity();
	double commonZ=std::numeric_limits<double>::infinity();
	for(std::size_t index=0u;index<tiers.size();++index){
		paths[index]=inputDirectory/(std::string("tier")+std::to_string(tiers[index])+
			"_t0p32.checkpoint");
		std::string error;
		if(!LoadMethaneRunCheckpoint(paths[index],states[index],error)||
			states[index].simulationTimeS!=0.32)return 137;
		commonX=std::min(commonX,states[index].cellWidthM*states[index].dimensions[0]);
		commonY=std::min(commonY,states[index].cellWidthM*states[index].dimensions[1]);
		commonZ=std::min(commonZ,states[index].cellWidthM*states[index].dimensions[2]);
	}
	const std::filesystem::path manifest=inputDirectory/"input_manifest.v2";
	const std::filesystem::path partial=inputDirectory/"input_manifest.v2.partial";
	if(std::filesystem::exists(manifest)||std::filesystem::exists(partial))return 138;
	std::ofstream output(partial,std::ios::binary|std::ios::trunc);
	if(!output)return 139;
	output<<"fire_production_calibration_input_v2\n"
		"physical_time_s 0.32\n"
		"baseline_horizon_s 0.002\n"
		"temporal_steps_s 0.002 0.001 0.0005\n"
		"formal_order production_space 2 production_time 1 oracle_space 2 oracle_time 2\n"
		"horizontal_alignment domain_center\n"
		"vertical_alignment common_floor\n"
		"common_support_m "<<std::setprecision(17)<<-0.5*commonX<<' '<<0.5*commonX<<' '
		<<-0.5*commonY<<' '<<0.5*commonY<<" 0 "<<commonZ<<"\n"
		"metric_registry component_volume_l1,component_integral,velocity_volume_l2,projection_residual\n"
		"golden_checkpoint_sha256 1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947\n"
		"mirror_generator_sha256 "<<RISEFireProductionFP64::SourceManifest::Generator<<"\n"
		"advection_source_sha256 "<<RISEFireProductionFP64::SourceManifest::FireProductionAdvectionSource<<"\n"
		"transport_source_sha256 "<<RISEFireProductionFP64::SourceManifest::FireProductionTransportSource<<"\n"
		"force_source_sha256 "<<RISEFireProductionFP64::SourceManifest::FireProductionForceSource<<"\n"
		"projection_source_sha256 "<<RISEFireProductionFP64::SourceManifest::FireProductionProjectionSource<<"\n";
	for(std::size_t index=0u;index<paths.size();++index){
		const MethaneRunCheckpoint& state=states[index];
		output<<"tier "<<tiers[index]<<" shape "<<state.dimensions[0]<<' '
			<<state.dimensions[1]<<' '<<state.dimensions[2]<<" dx "<<std::setprecision(17)
			<<state.cellWidthM<<" sha256 "<<DigestFile(paths[index])<<" case "
			<<state.caseRecordId<<'\n';
	}
	output.flush();output.close();if(!output)return 140;
	std::error_code renameError;std::filesystem::rename(partial,manifest,renameError);
	if(renameError)return 141;
	std::fprintf(stderr,"calibration inputs v2 sealed manifest_sha256=%s\n",
		DigestFile(manifest).c_str());return 0;
}
