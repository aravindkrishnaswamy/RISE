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

struct CalibrationOverlapDifference
{
	std::array<double,9> componentVolumeL1;
	double velocityVolumeL2;
	CalibrationOverlapDifference() : velocityVolumeL2(0.0){componentVolumeL1.fill(0.0);}
};

template<class Accumulator>
bool VisitCalibrationOverlaps(const MethaneRunCheckpoint& first,
	const MethaneRunCheckpoint& second,Accumulator accumulate,double& commonVolume)
{
	const double h1=first.cellWidthM,h2=second.cellWidthM;
	const double lx=std::min(h1*first.dimensions[0],h2*second.dimensions[0]);
	const double ly=std::min(h1*first.dimensions[1],h2*second.dimensions[1]);
	const double lz=std::min(h1*first.dimensions[2],h2*second.dimensions[2]);
	const std::array<double,3> low={{-0.5*lx,-0.5*ly,0.0}};
	const std::array<double,3> high={{0.5*lx,0.5*ly,lz}};
	const std::array<double,3> firstOrigin={{-0.5*h1*first.dimensions[0],
		-0.5*h1*first.dimensions[1],0.0}};
	const std::array<double,3> secondOrigin={{-0.5*h2*second.dimensions[0],
		-0.5*h2*second.dimensions[1],0.0}};
	commonVolume=lx*ly*lz;
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
								const std::size_t secondCell=(bz*second.dimensions[1]+by)*
									second.dimensions[0]+bx;
								accumulate(firstCell,secondCell,volume);
							}
						}
			}
	return commonVolume>0.0;
}

std::array<double,3> CalibrationCellVelocity(const MethaneRunCheckpoint& state,
	const std::size_t cell)
{
	PeriodicMACShape shape;shape.nx=state.dimensions[0];shape.ny=state.dimensions[1];
	shape.nz=state.dimensions[2];shape.cellWidthM=state.cellWidthM;
	std::array<double,3> velocity={{}};
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
		const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
		velocity[axis]=0.5*(state.velocity.component[axis][lower]+
			state.velocity.component[axis][upper]);
	}
	return velocity;
}

bool CalibrationDifference(const MethaneRunCheckpoint& first,
	const MethaneRunCheckpoint& second,CalibrationOverlapDifference& difference)
{
	difference=CalibrationOverlapDifference();double volume=0.0;
	const bool visited=VisitCalibrationOverlaps(first,second,
		[&](const std::size_t firstCell,const std::size_t secondCell,const double weight){
			const ConservativeVector firstValue=ToConservativeVector(first.states[firstCell]);
			const ConservativeVector secondValue=ToConservativeVector(second.states[secondCell]);
			for(std::size_t component=0u;component<9u;++component)
				difference.componentVolumeL1[component]+=weight*
					std::fabs(firstValue[component]-secondValue[component]);
			const std::array<double,3> firstVelocity=CalibrationCellVelocity(first,firstCell);
			const std::array<double,3> secondVelocity=CalibrationCellVelocity(second,secondCell);
			for(unsigned int axis=0u;axis<3u;++axis){
				const double delta=firstVelocity[axis]-secondVelocity[axis];
				difference.velocityVolumeL2+=weight*delta*delta;
			}
		},volume);
	if(!visited||!(volume>0.0))return false;
	for(double& value:difference.componentVolumeL1)value/=volume;
	difference.velocityVolumeL2=std::sqrt(difference.velocityVolumeL2/volume);
	return std::all_of(difference.componentVolumeL1.begin(),
		difference.componentVolumeL1.end(),[](double value){return std::isfinite(value);})&&
		std::isfinite(difference.velocityVolumeL2);
}

int CheckProductionCalibrationInputConvergence(const std::filesystem::path& inputDirectory)
{
	static const char* manifestDigest=
		"2c19f5c0750674196ecdf0fe1c0317b51b7d33ed440ec60c8c7bba2342e2b416";
	if(DigestFile(inputDirectory/"input_manifest.v2")!=manifestDigest)return 142;
	std::array<MethaneRunCheckpoint,3> states;std::string error;
	for(std::size_t index=0u;index<3u;++index){
		const std::filesystem::path path=inputDirectory/(std::string("tier")+
			std::to_string(index+5u)+"_t0p32.checkpoint");
		if(!LoadMethaneRunCheckpoint(path,states[index],error))return 143;
	}
	CalibrationOverlapDifference difference56,difference67;
	if(!CalibrationDifference(states[0],states[1],difference56)||
		!CalibrationDifference(states[1],states[2],difference67))return 144;
	bool asymptotic=difference56.velocityVolumeL2>difference67.velocityVolumeL2&&
		difference67.velocityVolumeL2>0.0;
	std::fprintf(stderr,"calibration input velocity_l2 D56=%.17g D67=%.17g ratio=%.17g\n",
		difference56.velocityVolumeL2,difference67.velocityVolumeL2,
		difference56.velocityVolumeL2/difference67.velocityVolumeL2);
	for(std::size_t component=0u;component<9u;++component){
		const bool componentAsymptotic=difference56.componentVolumeL1[component]>
			difference67.componentVolumeL1[component]&&difference67.componentVolumeL1[component]>0.0;
		asymptotic=asymptotic&&componentAsymptotic;
		std::fprintf(stderr,"calibration input component=%zu L1_D56=%.17g L1_D67=%.17g "
			"ratio=%.17g asymptotic=%d\n",component,difference56.componentVolumeL1[component],
			difference67.componentVolumeL1[component],difference56.componentVolumeL1[component]/
			difference67.componentVolumeL1[component],componentAsymptotic?1:0);
	}
	return asymptotic?0:145;
}
