#include "fire_production_fp64/SourceManifest.h"
#include "FireProductionCalibrationMath.h"

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
	const std::filesystem::path manifest=inputDirectory/"state_family_manifest.v3";
	const std::filesystem::path partial=inputDirectory/"state_family_manifest.v3.partial";
	if(std::filesystem::exists(manifest)||std::filesystem::exists(partial))return 138;
	std::ofstream output(partial,std::ios::binary|std::ios::trunc);
	if(!output)return 139;
	output<<"fire_production_calibration_state_family_v3\n"
		"scope pre_solver_asymptotic_family_gate\n"
		"full_campaign_manifest false\n"
		"physical_time_s 0.32\n"
		"formal_spatial_order 2\n"
		"evaluation_fp binary64_strict_no_contract\n"
		"horizontal_alignment domain_center\n"
		"vertical_alignment common_floor\n"
		"common_support_m "<<std::setprecision(17)<<-0.5*commonX<<' '<<0.5*commonX<<' '
		<<-0.5*commonY<<' '<<0.5*commonY<<" 0 "<<commonZ<<"\n"
		"overlap_topology piecewise_constant_exact_cell_intersection_v1\n"
		"metric 0 rho_total_Z kg_per_m3 volume_normalized_L1\n"
		"metric 1 CH4 kg_per_m3 volume_normalized_L1\n"
		"metric 2 O2 kg_per_m3 volume_normalized_L1\n"
		"metric 3 N2 kg_per_m3 volume_normalized_L1\n"
		"metric 4 CO2 kg_per_m3 volume_normalized_L1\n"
		"metric 5 H2O kg_per_m3 volume_normalized_L1\n"
		"metric 6 CO kg_per_m3 volume_normalized_L1\n"
		"metric 7 C_gr kg_per_m3 volume_normalized_L1\n"
		"metric 8 sensible_enthalpy J_per_m3 volume_normalized_L1\n"
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

struct CalibrationOverlapDifference
{
	std::array<double,9> componentVolumeL1;
	CalibrationOverlapDifference(){componentVolumeL1.fill(0.0);}
};

template<class Accumulator>
bool VisitCalibrationOverlaps(const MethaneRunCheckpoint& first,
	const MethaneRunCheckpoint& second,const std::array<double,3>& commonLength,
	Accumulator accumulate,double& commonVolume,std::size_t& overlapCount)
{
	const double h1=first.cellWidthM,h2=second.cellWidthM;
	const double lx=commonLength[0],ly=commonLength[1],lz=commonLength[2];
	const std::array<double,3> low={{-0.5*lx,-0.5*ly,0.0}};
	const std::array<double,3> high={{0.5*lx,0.5*ly,lz}};
	const std::array<double,3> firstOrigin={{-0.5*h1*first.dimensions[0],
		-0.5*h1*first.dimensions[1],0.0}};
	const std::array<double,3> secondOrigin={{-0.5*h2*second.dimensions[0],
		-0.5*h2*second.dimensions[1],0.0}};
	commonVolume=lx*ly*lz;
	overlapCount=0u;
	for(std::size_t z=0u;z<first.dimensions[2];++z)
		for(std::size_t y=0u;y<first.dimensions[1];++y)
			for(std::size_t x=0u;x<first.dimensions[0];++x){
				const std::array<std::size_t,3> firstCoordinate={{x,y,z}};
				std::array<double,3> firstLow,firstHigh;
				bool inside=true;
				for(unsigned int axis=0u;axis<3u;++axis){
					firstLow[axis]=std::max(low[axis],firstOrigin[axis]+
						static_cast<double>(firstCoordinate[axis])*h1);
					firstHigh[axis]=std::min(high[axis],firstOrigin[axis]+
						static_cast<double>(firstCoordinate[axis]+1u)*h1);
					inside=inside&&firstHigh[axis]>firstLow[axis];
				}
				if(!inside)continue;
				std::array<std::size_t,3> begin,end;
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t extent=second.dimensions[axis];
					const double relativeLow=(firstLow[axis]-secondOrigin[axis])/h2;
					const double relativeHigh=(firstHigh[axis]-secondOrigin[axis])/h2;
					const long lower=static_cast<long>(std::floor(relativeLow));
					const long upper=static_cast<long>(std::ceil(relativeHigh));
					begin[axis]=static_cast<std::size_t>(std::max(0L,std::min(
						static_cast<long>(extent),lower)));
					end[axis]=static_cast<std::size_t>(std::max(0L,std::min(
						static_cast<long>(extent),upper)));
				}
				const std::size_t firstCell=(z*first.dimensions[1]+y)*first.dimensions[0]+x;
				for(std::size_t bz=begin[2];bz<end[2];++bz)
					for(std::size_t by=begin[1];by<end[1];++by)
						for(std::size_t bx=begin[0];bx<end[0];++bx){
							const std::array<std::size_t,3> coordinate={{bx,by,bz}};
							double volume=1.0;
							for(unsigned int axis=0u;axis<3u;++axis){
								const double secondLow=secondOrigin[axis]+
									static_cast<double>(coordinate[axis])*h2;
								const double secondHigh=secondLow+h2;
								volume*=std::max(0.0,std::min(firstHigh[axis],secondHigh)-
									std::max(firstLow[axis],secondLow));
							}
							if(volume>0.0){
								++overlapCount;
								const std::size_t secondCell=(bz*second.dimensions[1]+by)*
									second.dimensions[0]+bx;
								accumulate(firstCell,secondCell,volume);
							}
						}
			}
	return commonVolume>0.0;
}

bool CalibrationDifference(const MethaneRunCheckpoint& first,
	const MethaneRunCheckpoint& second,const std::array<double,3>& commonLength,
	CalibrationOverlapDifference& difference)
{
	difference=CalibrationOverlapDifference();double volume=0.0,visitedVolume=0.0;
	double volumeCompensation=0.0;std::size_t overlapCount=0u;
	const bool visited=VisitCalibrationOverlaps(first,second,commonLength,
		[&](const std::size_t firstCell,const std::size_t secondCell,const double weight){
			const double corrected=weight-volumeCompensation;
			const double updated=visitedVolume+corrected;
			volumeCompensation=(updated-visitedVolume)-corrected;visitedVolume=updated;
			const ConservativeVector firstValue=ToConservativeVector(first.states[firstCell]);
			const ConservativeVector secondValue=ToConservativeVector(second.states[secondCell]);
			for(std::size_t component=0u;component<9u;++component)
				difference.componentVolumeL1[component]+=weight*
					std::fabs(firstValue[component]-secondValue[component]);
		},volume,overlapCount);
	const double volumeDifference=std::fabs(visitedVolume-volume);
	const double operationCount=20.0*static_cast<double>(overlapCount)+3.0;
	const double unitRoundoff=std::numeric_limits<double>::epsilon()*0.5;
	if(!(operationCount*unitRoundoff<1.0))return false;
	const double coverageBound=std::nextafter(operationCount*unitRoundoff/
		(1.0-operationCount*unitRoundoff)*volume,
		std::numeric_limits<double>::infinity());
	if(!visited||!(volume>0.0)||volumeDifference>coverageBound){
		std::fprintf(stderr,"calibration overlap coverage expected=%.17g visited=%.17g "
			"difference=%.17g bound=%.17g overlaps=%zu\n",volume,visitedVolume,
			volumeDifference,coverageBound,overlapCount);return false;
	}
	for(double& value:difference.componentVolumeL1)value/=volume;
	return std::all_of(difference.componentVolumeL1.begin(),
		difference.componentVolumeL1.end(),[](double value){return std::isfinite(value);});
}

int CheckProductionCalibrationInputConvergence(const std::filesystem::path& inputDirectory,
	const char* expectedManifestDigest=nullptr)
{
	if(!expectedManifestDigest||std::strlen(expectedManifestDigest)!=64u||
		DigestFile(inputDirectory/"state_family_manifest.v3")!=expectedManifestDigest)return 142;
	std::array<MethaneRunCheckpoint,3> states;std::string error;
	static const std::array<const char*,3> checkpointDigests={{
		"ce0b47fe2bfff897d3c9327e1207583243825ac70291b26e09216d2b55d04970",
		"7e53de9f426bca9d07e5d9c94cf7c78f947b94e00247a0fb3d1d075ea866b8d3",
		"3f9f1eaf9636dd0217b4804592a6dc6d467b2966680ded8756dc8f75ccda5571"}};
	static const std::array<std::array<std::size_t,3>,3> dimensions={{{{43u,43u,66u}},
		{{52u,52u,80u}},{{61u,61u,93u}}}};
	static const std::array<double,3> spacing={{0.04894898570785762,
		0.040790821423214683,0.034963561219898305}};
	static const std::array<const char*,3> cases={{
		"fe43358bbc0994b9f4a1a2414255d990136da28337fc473f81fe8e5852dec38c",
		"69673466269cda5ae65e18ea16dfd3a04d5eedf588fc7a3012a460e4357f171f",
		"b1518c2737bbc8cae3a96cbc7c4961f22f81cf0385a71d5baf30cc0a70960e32"}};
	for(std::size_t index=0u;index<3u;++index){
		const std::filesystem::path path=inputDirectory/(std::string("tier")+
			std::to_string(index+5u)+"_t0p32.checkpoint");
		if(DigestFile(path)!=checkpointDigests[index]||
			!LoadMethaneRunCheckpoint(path,states[index],error)||
			states[index].dimensions!=dimensions[index]||
			states[index].cellWidthM!=spacing[index]||states[index].caseRecordId!=cases[index]||
			states[index].simulationTimeS!=0.32)return 143;
	}
	std::array<double,3> commonLength={{
		std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::infinity()}};
	for(const MethaneRunCheckpoint& state:states)for(unsigned int axis=0u;axis<3u;++axis)
		commonLength[axis]=std::min(commonLength[axis],state.cellWidthM*state.dimensions[axis]);
	CalibrationOverlapDifference difference56,difference67;
	if(!CalibrationDifference(states[0],states[1],commonLength,difference56)||
		!CalibrationDifference(states[1],states[2],commonLength,difference67))return 144;
	bool asymptotic=true;
	for(std::size_t component=0u;component<9u;++component){
		double order=0.0,distance=0.0;
		const bool componentAsymptotic=FireProductionCalibration::GeneralizedGridRichardson(
			difference56.componentVolumeL1[component],difference67.componentVolumeL1[component],
			spacing[0],spacing[1],spacing[2],2.0,order,distance);
		asymptotic=asymptotic&&componentAsymptotic;
		std::fprintf(stderr,"calibration input component=%zu L1_D56=%.17g L1_D67=%.17g "
			"ratio=%.17g asymptotic=%d order=%.17g E6=%.17g\n",component,
			difference56.componentVolumeL1[component],
			difference67.componentVolumeL1[component],difference56.componentVolumeL1[component]/
			difference67.componentVolumeL1[component],componentAsymptotic?1:0,order,distance);
	}
	return asymptotic?0:145;
}
