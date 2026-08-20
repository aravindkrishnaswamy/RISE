#include "fire_production_fp64/SourceManifest.h"
#include "FireProductionCalibrationMath.h"

int SealExistingProductionCalibrationInputs(const std::filesystem::path& inputDirectory);

int RunProductionCalibrationStateGeneration(const std::filesystem::path& outputDirectory)
{
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
	return SealExistingProductionCalibrationInputs(outputDirectory);
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
	std::fprintf(stderr,"calibration state family v3 sealed manifest_sha256=%s\n",
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

bool LoadPinnedCalibrationStateFamily(const std::filesystem::path& inputDirectory,
	std::array<MethaneRunCheckpoint,3>& states,std::string& error)
{
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
			states[index].simulationTimeS!=0.32)return false;
	}
	return true;
}

int DiagnoseProductionCalibrationInputFamily(const std::filesystem::path& inputDirectory,
	const char* expectedManifestDigest=
		"338d7c66ee83c1c43e8f12d311389261af320335b70476203c42ff85dc66c3f5")
{
	if(!expectedManifestDigest||std::strlen(expectedManifestDigest)!=64u||
		DigestFile(inputDirectory/"state_family_manifest.v3")!=expectedManifestDigest)return 142;
	std::array<MethaneRunCheckpoint,3> states;std::string error;
	static const std::array<double,3> spacing={{0.04894898570785762,
		0.040790821423214683,0.034963561219898305}};
	if(!LoadPinnedCalibrationStateFamily(inputDirectory,states,error))return 143;
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
	std::fprintf(stderr,"calibration input family diagnostic asymptotic=%d "
		"(diagnostic only; solver outputs own Richardson acceptance)\n",asymptotic?1:0);
	return 0;
}

struct OracleSpatialCalibrationResult
{
	std::vector<ConservativeVector> conservative;
	PeriodicMACField momentumKGPerM2S,velocityMPerS;
	std::vector<std::vector<double> > divergenceTargetsPerS;
};

bool RunOracleSpatialCalibrationTrajectory(const MethaneRunCheckpoint& beginning,
	const double timeStepS,const unsigned int stepCount,OracleSpatialCalibrationResult& result,
	std::string& error)
{
	result=OracleSpatialCalibrationResult();
	if(!(timeStepS>0.0)||stepCount==0u)return false;
	PeriodicMACShape shape;shape.nx=beginning.dimensions[0];shape.ny=beginning.dimensions[1];
	shape.nz=beginning.dimensions[2];shape.cellWidthM=beginning.cellWidthM;
	const std::size_t cells=shape.CellCount();if(beginning.states.size()!=cells)return false;
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
	MethaneCellState ambient;ambient.temperatureK=300.0;
	for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
		ambient.constituent[species]=fuel.AmbientMassFractions()[species];
	double ambientInverseWeight=0.0;
	for(std::size_t species=0u;species<MethaneCarbon;++species){
		const FireThermochemistrySpecies* record=fuel.FindSpecies(
			fuel.SpeciesOrder()[species].c_str());
		if(record)ambientInverseWeight+=ambient.constituent[species]/
			record->molecularWeightKGPerKMol;
	}
	const double ambientDensity=fuel.ThermodynamicPressurePa()/(8314.46261815324*
		ambient.temperatureK*ambientInverseWeight);
	for(double& value:ambient.constituent)value*=ambientDensity;
	ambient.rhoTotalZ=0.0;
	if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),
		ambient.temperatureK,ambient.sensibleEnergyJPerM3,&error))return false;
	MethaneCellState injected;injected.temperatureK=300.0;
	for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
		injected.constituent[species]=fuel.InjectedMassFractions()[species];
	double injectedInverseWeight=0.0;
	for(std::size_t species=0u;species<MethaneCarbon;++species){
		const FireThermochemistrySpecies* record=fuel.FindSpecies(
			fuel.SpeciesOrder()[species].c_str());
		if(record)injectedInverseWeight+=injected.constituent[species]/
			record->molecularWeightKGPerKMol;
	}
	const double injectedDensity=fuel.ThermodynamicPressurePa()/(8314.46261815324*
		injected.temperatureK*injectedInverseWeight);
	for(double& value:injected.constituent)value*=injectedDensity;
	injected.rhoTotalZ=injected.TotalDensity();
	if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(injected),
		injected.temperatureK,injected.sensibleEnergyJPerM3,&error))return false;
	ConservativeAdvance3DConfig config;config.transport.cellWidthM=shape.cellWidthM;
	config.transport.deltaTimeS=timeStepS;config.transport.ambientTemperatureK=300.0;
	config.transport.adiabaticTemperatureK=2300.0;
	config.transport.ambientGasDensityKGPerM3=ambient.GasDensity();
	config.gravityMPerS2={{0.0,0.0,-9.80665}};config.periodicBoundaries=false;
	config.dns=false;config.retainStageDiagnostics=true;config.injectedTemperatureK=300.0;
	config.openBoundary.ambientDensityKGPerM3=ambient.GasDensity();
	config.openBoundary.injectedGasDensityKGPerM3=injected.GasDensity();
	config.openBoundary.ambientState=ToConservativeVector(ambient);
	config.openBoundary.injectedState=ToConservativeVector(injected);
	config.openBoundary.bottomFuelMask.clear();
	config.openBoundary.bottomFuelMassFluxKGPerM2S.clear();
	const double referenceVelocity=std::sqrt(9.80665*beginning.values.characteristicDiameterM);
	const double referenceLength=shape.cellWidthM*std::max({shape.nx,shape.ny,shape.nz});
	config.projectionTolerancePerS=1.0e-3*referenceVelocity/referenceLength;
	config.openBoundary.velocityToleranceMPerS=config.projectionTolerancePerS*referenceLength;
	config.openBoundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*
		config.openBoundary.velocityToleranceMPerS;config.workerCount=16u;
	std::vector<ConservativeVector> conservative(cells);
	for(std::size_t cell=0u;cell<cells;++cell)
		conservative[cell]=ToConservativeVector(beginning.states[cell]);
	PeriodicMACField momentum=beginning.momentum;
	const std::vector<MethaneSourcePacket> zeroPackets(cells);
	result.divergenceTargetsPerS.reserve(stepCount);
	for(unsigned int step=0u;step<stepCount;++step){
		ConservativeAdvance3DResult advanced;
		if(!AdvanceConservative3D(shape,conservative,momentum,zeroPackets,config,
			fuel,fuel,transport,advanced,&error))return false;
		if(advanced.divergenceHeunPerS.size()!=cells)return false;
		result.divergenceTargetsPerS.push_back(advanced.divergenceHeunPerS);
		conservative=std::move(advanced.conservative);
		momentum=std::move(advanced.momentumKGPerM2S);
		result.velocityMPerS=std::move(advanced.velocityMPerS);
	}
	result.conservative=std::move(conservative);result.momentumKGPerM2S=std::move(momentum);
	return result.conservative.size()==cells;
}

bool WriteCalibrationDoublePayload(const std::filesystem::path& path,
	const std::vector<std::vector<double> >& slices)
{
	const std::filesystem::path partial=path.string()+".partial";
	if(std::filesystem::exists(path)||std::filesystem::exists(partial))return false;
	std::ofstream output(partial,std::ios::binary|std::ios::trunc);if(!output)return false;
	for(const std::vector<double>& slice:slices)if(!slice.empty())output.write(
		reinterpret_cast<const char*>(slice.data()),static_cast<std::streamsize>(
			slice.size()*sizeof(double)));
	output.flush();output.close();if(!output)return false;
	std::error_code renameError;std::filesystem::rename(partial,path,renameError);
	return !renameError;
}

bool ReadCalibrationDoublePayload(const std::filesystem::path& path,
	const std::size_t sliceSize,const unsigned int sliceCount,
	std::vector<std::vector<double> >& slices)
{
	slices.clear();if(sliceSize==0u||sliceCount==0u)return false;
	std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return false;
	const std::uintmax_t expected=static_cast<std::uintmax_t>(sliceSize)*sliceCount*sizeof(double);
	if(input.tellg()!=static_cast<std::streamoff>(expected))return false;
	input.seekg(0);slices.assign(sliceCount,std::vector<double>(sliceSize));
	for(std::vector<double>& slice:slices)input.read(reinterpret_cast<char*>(slice.data()),
		static_cast<std::streamsize>(slice.size()*sizeof(double)));
	return static_cast<bool>(input);
}

int SealOracleSpatialCalibrationInputs(const std::filesystem::path& inputDirectory)
{
	if(DigestFile(inputDirectory/"state_family_manifest.v3")!=
		"338d7c66ee83c1c43e8f12d311389261af320335b70476203c42ff85dc66c3f5")return 146;
	std::array<MethaneRunCheckpoint,3> states;std::string error;
	if(!LoadPinnedCalibrationStateFamily(inputDirectory,states,error))return 147;
	static const double timeStepS=0.0005;static const unsigned int stepCount=4u;
	std::array<std::filesystem::path,3> targetPaths;
	for(std::size_t tier=0u;tier<3u;++tier){
		OracleSpatialCalibrationResult result;
		if(!RunOracleSpatialCalibrationTrajectory(states[tier],timeStepS,stepCount,
			result,error)){std::fprintf(stderr,"oracle spatial target tier %zu failed: %s\n",
			tier+5u,error.c_str());return 148;}
		targetPaths[tier]=inputDirectory/(std::string("oracle_tier")+
			std::to_string(tier+5u)+"_sdiv_dt0p0005_x4.f64");
		if(!WriteCalibrationDoublePayload(targetPaths[tier],result.divergenceTargetsPerS))
			return 149;
	}
	const std::filesystem::path manifest=inputDirectory/"oracle_spatial_input.v1";
	const std::filesystem::path partial=inputDirectory/"oracle_spatial_input.v1.partial";
	if(std::filesystem::exists(manifest)||std::filesystem::exists(partial))return 150;
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
	std::ofstream output(partial,std::ios::binary|std::ios::trunc);if(!output)return 151;
	output<<"fire_production_oracle_spatial_input_v1\n"
		"scope oracle_output_spatial_richardson\n"
		"physical_begin_s 0.32\nphysical_end_s 0.322\n"
		"dt_s 0.00050000000000000001\nstep_count 4\nworker_count 16\n"
		"sources exact_positive_zero\nbottom_fuel_overlay disabled_restore_wall\n"
		"boundary pressure_open pressure_open pressure_open pressure_open wall pressure_open\n"
		"gravity_m_per_s2 0 0 -9.8066500000000008\n"
		"metric component_volume_normalized_L1\nformal_spatial_order 2\n"
		"common_support inherited_state_family_v3\n"
		"state_family_manifest_sha256 338d7c66ee83c1c43e8f12d311389261af320335b70476203c42ff85dc66c3f5\n"
		"fuel_record_id "<<fuel.RecordId()<<"\ntransport_record_id "<<transport.RecordId()<<"\n"
		"oracle_core_sha256 "<<DigestFile("tools/fire_simulator_core.h")<<"\n"
		"oracle_advance_sha256 "<<DigestFile("tools/fire_simulator_3d_advance.h")<<"\n"
		"record_header_sha256 "<<DigestFile("src/Library/Utilities/FireSimulationRecords.h")<<"\n"
		"record_source_sha256 "<<DigestFile("src/Library/Utilities/FireSimulationRecords.cpp")<<"\n";
	for(std::size_t tier=0u;tier<3u;++tier)output<<"tier "<<tier+5u<<" sdiv_sha256 "
		<<DigestFile(targetPaths[tier])<<"\n";
	output.flush();output.close();if(!output)return 152;
	std::error_code renameError;std::filesystem::rename(partial,manifest,renameError);
	if(renameError)return 153;
	std::fprintf(stderr,"oracle spatial inputs sealed manifest_sha256=%s\n",
		DigestFile(manifest).c_str());return 0;
}

bool CalibrationOutputDifference(const MethaneRunCheckpoint& firstGeometry,
	const std::vector<ConservativeVector>& first,const MethaneRunCheckpoint& secondGeometry,
	const std::vector<ConservativeVector>& second,const std::array<double,3>& commonLength,
	CalibrationOverlapDifference& difference)
{
	if(first.size()!=firstGeometry.states.size()||second.size()!=secondGeometry.states.size())
		return false;
	difference=CalibrationOverlapDifference();double volume=0.0,visitedVolume=0.0;
	double compensation=0.0;std::size_t overlapCount=0u;
	const bool visited=VisitCalibrationOverlaps(firstGeometry,secondGeometry,commonLength,
		[&](const std::size_t firstCell,const std::size_t secondCell,const double weight){
			const double corrected=weight-compensation,updated=visitedVolume+corrected;
			compensation=(updated-visitedVolume)-corrected;visitedVolume=updated;
			for(std::size_t component=0u;component<9u;++component)
				difference.componentVolumeL1[component]+=weight*std::fabs(
					first[firstCell][component]-second[secondCell][component]);
		},volume,overlapCount);
	const double operationCount=20.0*static_cast<double>(overlapCount)+3.0;
	const double unitRoundoff=std::numeric_limits<double>::epsilon()*0.5;
	if(!visited||!(volume>0.0)||!(operationCount*unitRoundoff<1.0))return false;
	const double bound=std::nextafter(operationCount*unitRoundoff/
		(1.0-operationCount*unitRoundoff)*volume,std::numeric_limits<double>::infinity());
	if(std::fabs(visitedVolume-volume)>bound)return false;
	for(double& value:difference.componentVolumeL1)value/=volume;
	return std::all_of(difference.componentVolumeL1.begin(),
		difference.componentVolumeL1.end(),[](double value){return std::isfinite(value);});
}

int CheckOracleSpatialCalibrationOutput(const std::filesystem::path& inputDirectory,
	const char* expectedManifestDigest)
{
	if(!expectedManifestDigest||std::strlen(expectedManifestDigest)!=64u||
		DigestFile(inputDirectory/"oracle_spatial_input.v1")!=expectedManifestDigest)return 154;
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
	if(fuel.RecordId()!=
		"dfb8a9f09556f82e2206bcb02a5ed9e231e92a0acc9c3ba566fff53e8dea575d"||
		transport.RecordId()!=
		"0b5c719e47b6a708b79c4a1c583d14fc2e17b0cd119c7fc20f13420622e7cd1a"||
		DigestFile("tools/fire_simulator_core.h")!=
		"8995d96f4104fa7b9ca5d6e54108221ac121dd3478ac800766e51d8b0da13a29"||
		DigestFile("tools/fire_simulator_3d_advance.h")!=
		"bc7a78f5006f78ba2a8fd5fd7dbf305d797a6c5b6655a93b2421537dca6adfd7"||
		DigestFile("src/Library/Utilities/FireSimulationRecords.h")!=
		"8040567e2d679086845dbae2d9de0600d2b4755ee532b1bec76c63522968677d"||
		DigestFile("src/Library/Utilities/FireSimulationRecords.cpp")!=
		"071ea4eade818946c613bad6e443c30b3037e0d20980e14db571b3062bd5ae9a")
		return 155;
	std::array<MethaneRunCheckpoint,3> states;std::string error;
	if(!LoadPinnedCalibrationStateFamily(inputDirectory,states,error))return 156;
	static const std::array<const char*,3> targetDigests={{
		"b629dcdeca6fb3ffcbb3cad99fa0729490d57117840dbd23be099c1bd14d9b90",
		"399d7833d37fee379953c6d5692e5fe3a609484046f19e9b31e153d99f5b3640",
		"f98ea7f84c43569e1e76df25cde225707e51170651734803f2e5e2ffbc7935ef"}};
	std::array<OracleSpatialCalibrationResult,3> results;
	for(std::size_t tier=0u;tier<3u;++tier){
		const std::filesystem::path targetPath=inputDirectory/(std::string("oracle_tier")+
			std::to_string(tier+5u)+"_sdiv_dt0p0005_x4.f64");
		std::vector<std::vector<double> > sealedTargets;
		if(DigestFile(targetPath)!=targetDigests[tier]||!ReadCalibrationDoublePayload(
			targetPath,states[tier].states.size(),4u,sealedTargets))return 157;
		if(!RunOracleSpatialCalibrationTrajectory(states[tier],0.0005,4u,results[tier],error))
			return 158;
		if(results[tier].divergenceTargetsPerS.size()!=sealedTargets.size())return 159;
		for(std::size_t step=0u;step<sealedTargets.size();++step)if(
			results[tier].divergenceTargetsPerS[step].size()!=sealedTargets[step].size()||
			std::memcmp(results[tier].divergenceTargetsPerS[step].data(),
				sealedTargets[step].data(),sealedTargets[step].size()*sizeof(double))!=0)return 159;
	}
	std::array<double,3> commonLength={{
		std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::infinity()}};
	for(const MethaneRunCheckpoint& state:states)for(unsigned int axis=0u;axis<3u;++axis)
		commonLength[axis]=std::min(commonLength[axis],state.cellWidthM*state.dimensions[axis]);
	CalibrationOverlapDifference difference56,difference67;
	if(!CalibrationOutputDifference(states[0],results[0].conservative,states[1],
		results[1].conservative,commonLength,difference56)||!CalibrationOutputDifference(
		states[1],results[1].conservative,states[2],results[2].conservative,
		commonLength,difference67))return 161;
	static const std::array<double,9> expectedDifference56={{
		6.0315927215682195e-06,5.6457339952424706e-06,0.00010250313448132516,
		0.00033194873319439803,1.5490014963835757e-06,1.2681669038625223e-06,
		5.9099354295372759e-19,2.2279131374063514e-19,135.7301690853277}};
	static const std::array<double,9> expectedDifference67={{
		5.2927330540405051e-06,4.8468634113535512e-06,5.5189180398912391e-05,
		0.00017492063460296605,1.6338297167211502e-06,1.3376157338325683e-06,
		7.8785279713170043e-19,3.0077357103079101e-19,71.259230903712393}};
	if(difference56.componentVolumeL1!=expectedDifference56||
		difference67.componentVolumeL1!=expectedDifference67)return 161;
	static const std::array<double,3> spacing={{0.04894898570785762,
		0.040790821423214683,0.034963561219898305}};
	bool asymptotic=true;
	for(std::size_t component=0u;component<9u;++component){
		double order=0.0,distance=0.0;
		const bool componentAsymptotic=FireProductionCalibration::GeneralizedGridRichardson(
			difference56.componentVolumeL1[component],difference67.componentVolumeL1[component],
			spacing[0],spacing[1],spacing[2],2.0,order,distance);
		asymptotic=asymptotic&&componentAsymptotic;
		std::fprintf(stderr,"oracle output component=%zu L1_D56=%.17g L1_D67=%.17g "
			"ratio=%.17g asymptotic=%d order=%.17g E6=%.17g\n",component,
			difference56.componentVolumeL1[component],difference67.componentVolumeL1[component],
			difference56.componentVolumeL1[component]/difference67.componentVolumeL1[component],
			componentAsymptotic?1:0,order,distance);
	}
	return asymptotic?0:162;
}
