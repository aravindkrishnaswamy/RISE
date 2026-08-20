#ifndef FIRE_PRODUCTION_DYADIC_CALIBRATION_FIXTURE_H
#define FIRE_PRODUCTION_DYADIC_CALIBRATION_FIXTURE_H

namespace FireProductionDyadicCalibration
{
	static constexpr double Pi=3.141592653589793238462643383279502884;
	static constexpr double Gravity=9.80665;
	static constexpr double VerifiedOrder=1.8;
	static const std::array<unsigned int,4> Tiers={{5u,10u,6u,12u}};
	static const std::array<std::size_t,3> BaseDimensions={{4u,4u,6u}};

	bool BuildCase(const double tier,FireCase::RecordV1& record,std::string& error)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
		authored.poolDiameterM=CapstonePoolDiameterM;
		authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;
		authored.envelope={{0.0,1.0}};authored.durationS=1.0;
		authored.quality="dstar";authored.numericDStarTier=tier;
		authored.seed=1234u;authored.outputFramesPerS=4.0;authored.plumeLaw=true;
		const RISECBOR64::Bytes aerosol=AerosolRecord(),chem=SyntheticChemRecord();
		return FireCase::BuildMethaneV1(authored,fuel,{
			RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),
			RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem)},record,error);
	}

	bool AmbientAndInjected(MethaneCellState& ambient,MethaneCellState& injected,
		std::string& error)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		auto close=[&](MethaneCellState& state,const std::vector<double>& mass,
			const bool tagged){
			state=MethaneCellState();state.temperatureK=300.0;double inverseWeight=0.0;
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species){
				state.constituent[species]=mass[species];
				if(species<MethaneCarbon){const FireThermochemistrySpecies* record=
					fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
					if(record)inverseWeight+=mass[species]/record->molecularWeightKGPerKMol;}
			}
			if(!(inverseWeight>0.0))return false;
			const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
				state.temperatureK*inverseWeight);
			for(double& value:state.constituent)value*=density;
			state.rhoTotalZ=tagged?state.TotalDensity():0.0;
			return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),
				state.temperatureK,state.sensibleEnergyJPerM3,&error);
		};
		return close(ambient,fuel.AmbientMassFractions(),false)&&
			close(injected,fuel.InjectedMassFractions(),true);
	}

	double AnalyticVelocity(const unsigned int component,const double x,const double y,
		const double z,const double lx,const double ly,const double lz,const double amplitude)
	{
		const double px=2.0*Pi*x/lx,py=2.0*Pi*y/ly,pz=2.0*Pi*z/lz;
		if(component==0u)return amplitude*std::sin(px)*std::cos(py)*std::cos(pz);
		if(component==1u)return -amplitude*std::cos(px)*std::sin(py)*std::cos(pz);
		return 0.0;
	}

	std::size_t CellIndex(const MethaneRunCheckpoint& state,const std::size_t x,
		const std::size_t y,const std::size_t z)
	{
		return (z*state.dimensions[1]+y)*state.dimensions[0]+x;
	}

	bool BuildAnalyticState(const unsigned int tier,MethaneRunCheckpoint& result,
		std::string& error)
	{
		result=MethaneRunCheckpoint();FireCase::RecordV1 record;
		if(!BuildCase(static_cast<double>(tier),record,error))return false;
		const double dStar=record.derived.characteristicDiameterM;
		if(dStar!=0x1.f53cd43f813a5p-3){
			std::fprintf(stderr,"dyadic Dstar mismatch actual=%.17g hex=%a\n",dStar,dStar);
			return false;}
		result.caseRecordId=record.caseRecordId;result.producerBuildId="r109_analytic_smooth_v1";
		for(unsigned int axis=0u;axis<3u;++axis)
			result.dimensions[axis]=BaseDimensions[axis]*tier;
		result.cellWidthM=dStar/static_cast<double>(tier);
		result.values.characteristicDiameterM=dStar;
		result.values.dimensions=result.dimensions;result.values.cellWidthM=result.cellWidthM;
		const std::size_t cells=result.dimensions[0]*result.dimensions[1]*result.dimensions[2];
		result.states.resize(cells);
		MethaneCellState ambient,injected;if(!AmbientAndInjected(ambient,injected,error))return false;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		const double lx=BaseDimensions[0]*dStar,ly=BaseDimensions[1]*dStar,
			lz=BaseDimensions[2]*dStar;
		for(std::size_t z=0u;z<result.dimensions[2];++z)
			for(std::size_t y=0u;y<result.dimensions[1];++y)
				for(std::size_t x=0u;x<result.dimensions[0];++x){
					const double px=2.0*Pi*(static_cast<double>(x)+0.5)/result.dimensions[0];
					const double py=2.0*Pi*(static_cast<double>(y)+0.5)/result.dimensions[1];
					const double pz=2.0*Pi*(static_cast<double>(z)+0.5)/result.dimensions[2];
					const double mode0=std::sin(px)*std::sin(py)*std::sin(pz);
					const double mixture=0.04+0.01*mode0;
					MethaneCellState state;state.temperatureK=330.0+
						20.0*std::cos(px)*std::cos(py)*std::cos(pz);
					std::array<double,MethaneSpeciesCount> mass={{}};double inverseWeight=0.0;
					for(std::size_t species=0u;species<MethaneSpeciesCount;++species){
						const double ambientMass=ambient.constituent[species]/ambient.GasDensity();
						const double injectedMass=injected.constituent[species]/injected.GasDensity();
						mass[species]=(1.0-mixture)*ambientMass+mixture*injectedMass;
						if(species<MethaneCarbon){const FireThermochemistrySpecies* speciesRecord=
							fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
							if(speciesRecord)inverseWeight+=mass[species]/
								speciesRecord->molecularWeightKGPerKMol;}
					}
					const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
						state.temperatureK*inverseWeight);
					for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
						state.constituent[species]=density*mass[species];
					const double reacted=0.2*std::min(state.constituent[MethaneCH4],
						state.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
					const std::vector<double>& reaction=fuel.PrimaryReactionDelta();
					for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
						state.constituent[species]+=reacted*reaction[species];
					const FireThermochemistrySpecies* co=fuel.FindSpecies("CO");
					const FireThermochemistrySpecies* co2=fuel.FindSpecies("CO2");
					const FireThermochemistrySpecies* o2=fuel.FindSpecies("O2");
					const FireThermochemistrySpecies* carbon=fuel.FindSpecies("C(gr)");
					if(!co||!co2||!o2||!carbon)return false;
					const double co2ToCO=0.02*state.constituent[MethaneCO2];
					state.constituent[MethaneCO2]-=co2ToCO;
					state.constituent[MethaneCO]+=co2ToCO*co->molecularWeightKGPerKMol/
						co2->molecularWeightKGPerKMol;
					state.constituent[MethaneO2]+=co2ToCO*0.5*o2->molecularWeightKGPerKMol/
						co2->molecularWeightKGPerKMol;
					const double co2ToCarbon=0.01*state.constituent[MethaneCO2];
					state.constituent[MethaneCO2]-=co2ToCarbon;
					state.constituent[MethaneCarbon]+=co2ToCarbon*carbon->molecularWeightKGPerKMol/
						co2->molecularWeightKGPerKMol;
					state.constituent[MethaneO2]+=co2ToCarbon*o2->molecularWeightKGPerKMol/
						co2->molecularWeightKGPerKMol;
					state.rhoTotalZ=mixture*state.TotalDensity();
					if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),
						state.temperatureK,state.sensibleEnergyJPerM3,&error))return false;
					result.states[CellIndex(result,x,y,z)]=state;
				}
		const double amplitude=0.05*std::sqrt(Gravity*dStar),h=result.cellWidthM;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t nx=result.dimensions[0],ny=result.dimensions[1],nz=result.dimensions[2];
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz;
			result.momentum.component[axis].resize(ex*ey*ez);
			result.velocity.component[axis].resize(ex*ey*ez);
			for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
				for(std::size_t x=0u;x<ex;++x){
					const std::size_t face=(z*ey+y)*ex+x;
					std::array<std::size_t,3> lower={{x%nx,y%ny,z%nz}},upper=lower;
					const std::size_t normal=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=result.dimensions[axis];
					lower[axis]=normal==0u?0u:normal-1u;
					upper[axis]=normal==extent?extent-1u:normal;
					const double lowerDensity=result.states[CellIndex(result,lower[0],lower[1],
						lower[2])].GasDensity(),upperDensity=result.states[CellIndex(result,upper[0],
						upper[1],upper[2])].GasDensity();
					const double density=(normal==0u||normal==extent)?
						0.5*((normal==0u?upperDensity:lowerDensity)+ambient.GasDensity()):
						0.5*(lowerDensity+upperDensity);
					const double fx=(static_cast<double>(x)+(axis==0u?1.0:0.5))*h;
					const double fy=(static_cast<double>(y)+(axis==1u?1.0:0.5))*h;
					const double fz=(static_cast<double>(z)+(axis==2u?1.0:0.5))*h;
					const double velocity=AnalyticVelocity(axis,fx,fy,fz,lx,ly,lz,amplitude);
					result.velocity.component[axis][face]=velocity;
					result.momentum.component[axis][face]=density*velocity;
				}
		}
		return true;
	}

	template<class Integer> void AppendInteger(RISECBOR64::Bytes& bytes,const Integer value)
	{
		const std::uint64_t encoded=static_cast<std::uint64_t>(value);
		for(unsigned int shift=0u;shift<64u;shift+=8u)
			bytes.push_back(static_cast<unsigned char>((encoded>>shift)&0xffu));
	}

	void AppendDouble(RISECBOR64::Bytes& bytes,const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));AppendInteger(bytes,bits);
	}

	std::string AnalyticStateDigest(const MethaneRunCheckpoint& state)
	{
		RISECBOR64::Bytes bytes;
		for(const std::size_t dimension:state.dimensions)AppendInteger(bytes,dimension);
		AppendDouble(bytes,state.cellWidthM);
		for(const MethaneCellState& cell:state.states){AppendDouble(bytes,cell.rhoTotalZ);
			for(const double value:cell.constituent)AppendDouble(bytes,value);
			AppendDouble(bytes,cell.sensibleEnergyJPerM3);AppendDouble(bytes,cell.temperatureK);}
		for(unsigned int axis=0u;axis<3u;++axis){
			for(const double value:state.momentum.component[axis])AppendDouble(bytes,value);
			for(const double value:state.velocity.component[axis])AppendDouble(bytes,value);
		}
		return RISECBOR64::SHA256Hex(bytes);
	}

	double BSplinePrimitive(const double value)
	{
		const double sign=value<0.0?-1.0:1.0,x=std::fabs(value);double result=0.0;
		if(x>=2.0)result=0.5;
		else if(x>=1.0){const double remaining=2.0-x;
			result=0.5-remaining*remaining*remaining*remaining/24.0;}
		else {const double x2=x*x;result=(2.0/3.0)*x-(1.0/3.0)*x2*x+0.125*x2*x2;}
		return sign*result;
	}

	bool AxisWeights(const std::size_t samples,const std::size_t source,const double length,
		const double width,std::vector<std::vector<std::pair<std::size_t,double> > >& weights)
	{
		weights.assign(samples,{});const double sourceH=length/static_cast<double>(source);
		for(std::size_t sample=0u;sample<samples;++sample){
			const double location=(static_cast<double>(sample)+2.5)*width;
			double sum=0.0,sumAbs=0.0;
			for(std::size_t cell=0u;cell<source;++cell){double weight=0.0;
				const double low=static_cast<double>(cell)*sourceH,high=low+sourceH;
				weight=BSplinePrimitive((high-location)/width)-
					BSplinePrimitive((low-location)/width);
				if(weight<0.0||!std::isfinite(weight))return false;
				if(weight!=0.0){weights[sample].push_back({cell,weight});sum+=weight;
					sumAbs+=std::fabs(weight);}
			}
			const double operations=12.0*static_cast<double>(source)+3.0;
			const double u=std::numeric_limits<double>::epsilon()*0.5;
			if(!(operations*u<1.0)||std::fabs(sum-1.0)>
				operations*u/(1.0-operations*u)*sumAbs)return false;
		}
		return true;
	}

	struct FilteredField
	{
		std::array<std::size_t,3> dimensions={{0u,0u,0u}};
		std::vector<ConservativeVector> value;
	};

	bool FilterConservative(const MethaneRunCheckpoint& geometry,
		const std::vector<ConservativeVector>& source,const double dStar,FilteredField& result)
	{
		result=FilteredField();result.dimensions={{16u,16u,26u}};
		if(source.size()!=geometry.states.size())return false;
		const std::array<double,3> lengths={{4.0*dStar,4.0*dStar,6.0*dStar}};
		const double width=dStar/5.0;
		std::array<std::vector<std::vector<std::pair<std::size_t,double> > >,3> weights;
		for(unsigned int axis=0u;axis<3u;++axis)if(!AxisWeights(result.dimensions[axis],
			geometry.dimensions[axis],lengths[axis],width,weights[axis]))return false;
		const std::size_t sx=result.dimensions[0],sy=result.dimensions[1],sz=result.dimensions[2];
		const std::size_t nx=geometry.dimensions[0],ny=geometry.dimensions[1],nz=geometry.dimensions[2];
		std::vector<ConservativeVector> xStage(sx*ny*nz);
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<ny;++y)
			for(std::size_t x=0u;x<sx;++x){ConservativeVector& target=xStage[(z*ny+y)*sx+x];
				for(const auto& entry:weights[0][x])for(std::size_t c=0u;c<9u;++c)
					target[c]+=entry.second*source[(z*ny+y)*nx+entry.first][c];}
		std::vector<ConservativeVector> yStage(sx*sy*nz);
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<sy;++y)
			for(std::size_t x=0u;x<sx;++x){ConservativeVector& target=yStage[(z*sy+y)*sx+x];
				for(const auto& entry:weights[1][y])for(std::size_t c=0u;c<9u;++c)
					target[c]+=entry.second*xStage[(z*ny+entry.first)*sx+x][c];}
		result.value.assign(sx*sy*sz,ConservativeVector());
		for(std::size_t z=0u;z<sz;++z)for(std::size_t y=0u;y<sy;++y)
			for(std::size_t x=0u;x<sx;++x){ConservativeVector& target=result.value[(z*sy+y)*sx+x];
				for(const auto& entry:weights[2][z])for(std::size_t c=0u;c<9u;++c)
					target[c]+=entry.second*yStage[(entry.first*sy+y)*sx+x][c];}
		return std::all_of(result.value.begin(),result.value.end(),[](const ConservativeVector& v){
			return std::all_of(v.value.begin(),v.value.end(),
				[](const double x){return std::isfinite(x);});});
	}

	std::array<double,9> FieldDistance(const FilteredField& a,const FilteredField& b)
	{
		std::array<double,9> result={{}};if(a.dimensions!=b.dimensions||a.value.size()!=b.value.size()){
			result.fill(std::numeric_limits<double>::infinity());return result;}
		for(std::size_t cell=0u;cell<a.value.size();++cell)for(std::size_t c=0u;c<9u;++c)
			result[c]+=std::fabs(a.value[cell][c]-b.value[cell][c]);
		for(double& value:result)value/=static_cast<double>(a.value.size());return result;
	}

	FilteredField Extrapolate(const FilteredField& coarse,const FilteredField& fine)
	{
		FilteredField result;result.dimensions=coarse.dimensions;
		if(coarse.dimensions!=fine.dimensions||coarse.value.size()!=fine.value.size())return result;
		const double factor=std::pow(2.0,VerifiedOrder),denominator=factor-1.0;
		result.value.resize(coarse.value.size());
		for(std::size_t cell=0u;cell<coarse.value.size();++cell)for(std::size_t c=0u;c<9u;++c)
			result.value[cell][c]=(factor*fine.value[cell][c]-coarse.value[cell][c])/denominator;
		return result;
	}

	bool WriteTextAtomically(const std::filesystem::path& path,const std::string& text)
	{
		const std::filesystem::path partial=path.string()+".partial";
		if(std::filesystem::exists(path)||std::filesystem::exists(partial))return false;
		std::ofstream output(partial,std::ios::binary|std::ios::trunc);output<<text;
		output.flush();output.close();if(!output)return false;
		std::error_code error;std::filesystem::rename(partial,path,error);return !error;
	}

	int SealProtocol(const std::filesystem::path& directory)
	{
		if(std::filesystem::exists(directory))return 163;std::error_code directoryError;
		if(!std::filesystem::create_directories(directory,directoryError)||directoryError)return 164;
		std::array<MethaneRunCheckpoint,4> states;std::array<std::string,4> digests,cases;
		std::string error;double dStar=0.0;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error)){
				std::fprintf(stderr,"dyadic analytic tier %u failed: %s\n",Tiers[index],
					error.c_str());return 165;}
			digests[index]=AnalyticStateDigest(states[index]);cases[index]=states[index].caseRecordId;
			dStar=states[index].values.characteristicDiameterM;
		}
		const double flowThrough=6.0*std::sqrt(dStar/Gravity);
		std::ostringstream manifest;manifest<<std::setprecision(17)
			<<"fire_production_dyadic_protocol_v1\n"
			<<"scope short_horizon_smooth_filtered_spatial\n"
			<<"domain_Dstar 4 4 6\nDstar_m "<<dStar<<"\n"
			<<"tiers 5 10 6 12\nbase_dimensions 4 4 6\n"
			<<"boundary pressure_open pressure_open pressure_open pressure_open pressure_open pressure_open\n"
			<<"state analytic_affine_admissible_all_channel_taylor_green_v3\n"
			<<"mixture_fraction 0.04_plus_0.01_sinX_sinY_sinZ\n"
			<<"composition ambient_injected_mixture_line\n"
			<<"primary_reaction_extent 0.2_of_limiting_reactant\n"
			<<"reverse_CO2_to_CO_fraction 0.02\n"
			<<"reverse_CO2_to_C_gr_fraction 0.01_after_CO_partition\n"
			<<"temperature_K 330_plus_20_cosX_cosY_cosZ\n"
			<<"velocity taylor_green_xy_amplitude_0.05_sqrt_gDstar\n"
			<<"sources exact_positive_zero\n"
			<<"t_ft_s "<<flowThrough<<"\nhorizon_fraction 0.015625\n"
			<<"horizon_s "<<flowThrough/64.0<<"\nstep_count 8\ndt_s "<<flowThrough/512.0<<"\n"
			<<"filter tensor_cubic_cardinal_bspline_exact_cell_integral_v1\n"
			<<"filter_scale_m "<<dStar/5.0<<"\nfilter_support_radius_m "<<2.0*dStar/5.0<<"\n"
			<<"observation_lattice 16 16 26 tier5_indices_2_through_extent_minus_3\n"
			<<"observation_window inset_2h5_each_side_no_boundary_extension\nverified_order 1.8\n"
			<<"decision independent_limit_balls_overlap_and_cross_pair_refinement\n"
			<<"golden_checkpoint_sha256 1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947\n"
			<<"fuel_record_id "<<FireSimulationMethaneRecord::PhysicalV1().RecordId()<<"\n"
			<<"transport_record_id "<<FireSimulationTransportRecord::OpenV1().RecordId()<<"\n"
			<<"oracle_core_sha256 "<<DigestFile("tools/fire_simulator_core.h")<<"\n"
			<<"oracle_advance_sha256 "<<DigestFile("tools/fire_simulator_3d_advance.h")<<"\n";
		for(std::size_t index=0u;index<Tiers.size();++index)manifest<<"tier "<<Tiers[index]
			<<" analytic_state_sha256 "<<digests[index]<<" case_record_id "<<cases[index]<<"\n";
		if(!WriteTextAtomically(directory/"dyadic_protocol.v1",manifest.str()))return 166;
		std::fprintf(stderr,"dyadic protocol sealed manifest_sha256=%s\n",
			DigestFile(directory/"dyadic_protocol.v1").c_str());return 0;
	}

	int SealTargets(const std::filesystem::path& directory,const char* expectedProtocol)
	{
		std::fprintf(stderr,"dyadic target extraction begins\n");
		if(!expectedProtocol||std::strlen(expectedProtocol)!=64u||
			DigestFile(directory/"dyadic_protocol.v1")!=expectedProtocol)return 167;
		std::array<std::string,4> targetDigests;std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){MethaneRunCheckpoint state;
			if(!BuildAnalyticState(Tiers[index],state,error))return 168;
			std::fprintf(stderr,"dyadic target tier %u advancing cells=%zu\n",Tiers[index],
				state.states.size());
			const double flowThrough=6.0*std::sqrt(state.values.characteristicDiameterM/Gravity);
			OracleSpatialCalibrationResult result;
			if(!RunOracleSpatialCalibrationTrajectory(state,flowThrough/512.0,8u,result,error,false)){
				std::fprintf(stderr,"dyadic target tier %u failed: %s\n",Tiers[index],
					error.c_str());return 169;}
			const std::filesystem::path path=directory/(std::string("oracle_tier")+
				std::to_string(Tiers[index])+"_sdiv_x8.f64");
			if(!WriteCalibrationDoublePayload(path,result.divergenceTargetsPerS))return 170;
			targetDigests[index]=DigestFile(path);
		}
		std::ostringstream manifest;manifest<<"fire_production_dyadic_targets_v1\nprotocol_sha256 "
			<<expectedProtocol<<"\n";for(std::size_t index=0u;index<Tiers.size();++index)
			manifest<<"tier "<<Tiers[index]<<" sdiv_sha256 "<<targetDigests[index]<<"\n";
		if(!WriteTextAtomically(directory/"dyadic_targets.v1",manifest.str()))return 171;
		std::fprintf(stderr,"dyadic targets sealed manifest_sha256=%s\n",
			DigestFile(directory/"dyadic_targets.v1").c_str());return 0;
	}

	bool ReadTargetDigests(const std::filesystem::path& path,std::array<std::string,4>& digests)
	{
		std::ifstream input(path);std::string token;std::size_t found=0u;
		while(input>>token){if(token=="tier"){unsigned int tier=0u;std::string label,digest;
			if(!(input>>tier>>label>>digest)||label!="sdiv_sha256")return false;
			auto it=std::find(Tiers.begin(),Tiers.end(),tier);if(it==Tiers.end())return false;
			digests[static_cast<std::size_t>(it-Tiers.begin())]=digest;++found;}}
		return found==4u&&std::all_of(digests.begin(),digests.end(),[](const std::string& digest){
			return digest.size()==64u;});
	}

	int CheckOracle(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 173;
		std::array<std::string,4> targetDigests;if(!ReadTargetDigests(
			directory/"dyadic_targets.v1",targetDigests))return 174;
		std::array<MethaneRunCheckpoint,4> states;std::array<FilteredField,4> filtered;
		std::array<OracleSpatialCalibrationResult,4> outputs;std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error))return 175;
			const double flowThrough=6.0*std::sqrt(states[index].values.characteristicDiameterM/Gravity);
			std::vector<std::vector<double> > sealed;
			const std::filesystem::path target=directory/(std::string("oracle_tier")+
				std::to_string(Tiers[index])+"_sdiv_x8.f64");
			if(DigestFile(target)!=targetDigests[index]||!ReadCalibrationDoublePayload(target,
				states[index].states.size(),8u,sealed)||!RunOracleSpatialCalibrationTrajectory(
				states[index],flowThrough/512.0,8u,outputs[index],error,false))return 176;
			if(outputs[index].divergenceTargetsPerS.size()!=sealed.size())return 177;
			for(std::size_t step=0u;step<sealed.size();++step)if(sealed[step].size()!=
				outputs[index].divergenceTargetsPerS[step].size()||std::memcmp(sealed[step].data(),
				outputs[index].divergenceTargetsPerS[step].data(),sealed[step].size()*sizeof(double))!=0)
				return 177;
			if(!FilterConservative(states[index],outputs[index].conservative,
				states[index].values.characteristicDiameterM,filtered[index]))return 178;
		}
		const std::array<double,9> dA=FieldDistance(filtered[0],filtered[1]);
		const std::array<double,9> dB=FieldDistance(filtered[2],filtered[3]);
		const FilteredField limitA=Extrapolate(filtered[0],filtered[1]);
		const FilteredField limitB=Extrapolate(filtered[2],filtered[3]);
		const std::array<double,9> limitDifference=FieldDistance(limitA,limitB);
		const std::array<double,9> coarseAToB=FieldDistance(filtered[0],limitB);
		const std::array<double,9> fineAToB=FieldDistance(filtered[1],limitB);
		const std::array<double,9> coarseBToA=FieldDistance(filtered[2],limitA);
		const std::array<double,9> fineBToA=FieldDistance(filtered[3],limitA);
		static const std::array<double,9> expectedA={{
			2.7470874096308602e-05,2.1976699277047011e-05,4.0231720360505535e-05,
			8.1681230739515595e-05,1.4623092049747496e-05,1.233964089521796e-05,
			1.918563438587409e-07,4.0311216479063973e-08,27.213697534765799}};
		static const std::array<double,9> expectedB={{
			1.9110121790822129e-05,1.5288097432657347e-05,2.7979909266403962e-05,
			5.6950264505725857e-05,1.0172558363063742e-05,8.5840748836714389e-06,
			1.3346492305372179e-07,2.8042509814226353e-08,19.056231150677373}};
		static const std::array<double,9> expectedLimitDifference={{
			5.7638488509996388e-07,4.6110790808047211e-07,8.6342690894070087e-07,
			1.7491251816547303e-06,3.0681692913582059e-07,2.5890630471503395e-07,
			4.0254669845263587e-09,8.4579674447443766e-10,0.56813643742138875}};
		if(dA!=expectedA||dB!=expectedB||limitDifference!=expectedLimitDifference)return 172;
		bool accepted=true;
		for(std::size_t component=0u;component<9u;++component){
			FireProductionCalibration::DyadicDistanceEstimate estimateA,estimateB;
			const bool estimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
				dA[component],VerifiedOrder,estimateA)&&
				FireProductionCalibration::DyadicDistanceAtVerifiedOrder(dB[component],
				VerifiedOrder,estimateB);
			const bool overlap=estimated&&FireProductionCalibration::DyadicLimitBallsOverlap(
				limitDifference[component],estimateA,estimateB);
			const bool approaches=coarseAToB[component]>fineAToB[component]&&
				coarseBToA[component]>fineBToA[component];
			const double rescaledA=estimated?FireProductionCalibration::NextUp(
				estimateA.coarseDistance*std::pow(5.0/6.0,VerifiedOrder)):0.0;
			const double tier6Distance=estimated?std::max(rescaledA,estimateB.coarseDistance):0.0;
			accepted=accepted&&overlap&&approaches;
			std::fprintf(stderr,"dyadic oracle component=%zu D5_10=%.17g D6_12=%.17g "
				"limit_delta=%.17g radius_sum=%.17g approachA=%d approachB=%d E6=%.17g "
				"accepted=%d\n",component,dA[component],dB[component],limitDifference[component],
				estimated?estimateA.fineRadius+estimateB.fineRadius:0.0,
				coarseAToB[component]>fineAToB[component]?1:0,
				coarseBToA[component]>fineBToA[component]?1:0,tier6Distance,
				overlap&&approaches?1:0);
		}
		return accepted?0:179;
	}

	bool BuildProductionRequest(const MethaneRunCheckpoint& state,
		const std::vector<double>& divergenceTarget,const double timeStepS,
		RISE::FireProductionResidentStepRequest& request,std::string& error)
	{
		request=RISE::FireProductionResidentStepRequest();
		const std::size_t cells=state.states.size();
		if(cells!=state.dimensions[0]*state.dimensions[1]*state.dimensions[2]||
			divergenceTarget.size()!=cells)return false;
		MethaneCellState ambient,injected;if(!AmbientAndInjected(ambient,injected,error))return false;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		const FireSimulationTransportRecord transport=FireSimulationTransportRecord::OpenV1();
		request.force.shape.nx=state.dimensions[0];request.force.shape.ny=state.dimensions[1];
		request.force.shape.nz=state.dimensions[2];
		request.force.shape.cellWidthM=static_cast<float>(state.cellWidthM);
		request.force.timeStepS=static_cast<float>(timeStepS);
		request.force.ambientDensityKGPerM3=static_cast<float>(ambient.GasDensity());
		request.force.vremanCoefficient=0.07f;
		request.force.gravityMPerS2={{0.0f,0.0f,-static_cast<float>(Gravity)}};
		request.force.boundary.fill(RISE::FireProductionProjectionPressureOpen);
		request.force.cellGasDensityKGPerM3.resize(cells);
		request.force.molecularKinematicViscosityM2PerS.resize(cells);
		request.cellTransport.shape=request.force.shape;request.cellTransport.componentCount=9u;
		request.cellTransport.timeStepS=request.force.timeStepS;
		request.cellTransport.boundary=request.force.boundary;
		request.cellTransport.conservativeValues.resize(9u*cells);
		const ConservativeVector ambientVector=ToConservativeVector(ambient);
		request.cellTransport.ambientValues.resize(9u);
		for(std::size_t component=0u;component<9u;++component)
			request.cellTransport.ambientValues[component]=static_cast<float>(ambientVector[component]);
		for(std::size_t cell=0u;cell<cells;++cell){
			const ConservativeVector conservative=ToConservativeVector(state.states[cell]);
			const double gas=state.states[cell].GasDensity();CellMolecularTransportEvaluation molecular;
			if(!(gas>0.0)||!EvaluateCellMolecularTransport(state.states[cell],fuel,transport,
				molecular,&error))return false;
			request.force.molecularKinematicViscosityM2PerS[cell]=
				static_cast<float>(molecular.molecularViscosityPaS/gas);
			for(std::size_t component=0u;component<9u;++component)
				request.cellTransport.conservativeValues[component*cells+cell]=
					static_cast<float>(conservative[component]);
			float packedGas=request.cellTransport.conservativeValues[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				packedGas+=request.cellTransport.conservativeValues[component*cells+cell];
			request.force.cellGasDensityKGPerM3[cell]=packedGas;
		}
		request.dualTransport.shape=request.force.shape;
		request.dualTransport.timeStepS=request.force.timeStepS;
		request.dualTransport.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
		request.dualTransport.boundary=request.force.boundary;
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*state.dimensions[1]+y)*state.dimensions[0]+x;};
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t nx=state.dimensions[0],ny=state.dimensions[1],nz=state.dimensions[2];
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz,faces=ex*ey*ez;
			if(state.momentum.component[axis].size()!=faces||
				state.velocity.component[axis].size()!=faces)return false;
			request.force.faceDensityKGPerM3[axis].resize(faces);
			request.force.beginningMomentumKGPerM2S[axis].resize(faces);
			request.cellTransport.frozenVelocityMPerS[axis].resize(faces);
			for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
				for(std::size_t x=0u;x<ex;++x){
					const std::size_t face=(z*ey+y)*ex+x;
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					std::array<std::size_t,3> low={{x,y,z}},high=low;
					if(coordinate>0u&&coordinate<extent)low[axis]=coordinate-1u;
					else {low[axis]=coordinate==0u?0u:extent-1u;high[axis]=low[axis];}
					const float lowDensity=request.force.cellGasDensityKGPerM3[
						cellIndex(low[0],low[1],low[2])];
					float density=0.0f;
					if(coordinate==0u||coordinate==extent)density=0.5f*lowDensity+
						0.5f*request.force.ambientDensityKGPerM3;
					else {high[axis]=coordinate;density=0.5f*lowDensity+0.5f*
						request.force.cellGasDensityKGPerM3[cellIndex(high[0],high[1],high[2])];}
					request.force.faceDensityKGPerM3[axis][face]=density;
					request.force.beginningMomentumKGPerM2S[axis][face]=
						static_cast<float>(state.momentum.component[axis][face]);
					request.cellTransport.frozenVelocityMPerS[axis][face]=
						static_cast<float>(state.velocity.component[axis][face]);
				}
			request.dualTransport.beginningFaceDensity[axis]=request.force.faceDensityKGPerM3[axis];
			request.dualTransport.beginningMomentum[axis]=request.force.beginningMomentumKGPerM2S[axis];
			request.dualTransport.frozenVelocityMPerS[axis]=request.cellTransport.frozenVelocityMPerS[axis];
			request.momentumSourceIncrement[axis].assign(faces,0.0f);
		}
		request.cellSourceIncrement.assign(9u*cells,0.0f);
		request.divergenceTargetPerS.resize(cells);
		for(std::size_t cell=0u;cell<cells;++cell)
			request.divergenceTargetPerS[cell]=static_cast<float>(divergenceTarget[cell]);
		return RISE::ValidateFireProductionFrozenForceRequest(request.force,&error)&&
			RISE::ValidateFireProductionCellPalindromeRequest(request.cellTransport,&error)&&
			RISE::ValidateFireProductionDualMomentumRequest(request.dualTransport,&error);
	}

	bool ApplyProductionResult(const RISE::FireProductionResidentStepResult& production,
		MethaneRunCheckpoint& state,std::string& error)
	{
		const std::size_t cells=state.states.size();if(production.conservativeValues.size()!=9u*cells)
			return false;
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			for(std::size_t component=0u;component<9u;++component)
				conservative[cell][component]=production.conservativeValues[component*cells+cell];
			state.states[cell]=FromConservativeVector(conservative[cell]);
		}
		std::vector<double> temperature;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
			fuel.TemperatureMaxK(),temperature,&error,1u)||temperature.size()!=cells)return false;
		for(std::size_t cell=0u;cell<cells;++cell)state.states[cell].temperatureK=temperature[cell];
		for(unsigned int axis=0u;axis<3u;++axis){
			state.momentum.component[axis].assign(production.projection.momentumKGPerM2S[axis].begin(),
				production.projection.momentumKGPerM2S[axis].end());
			state.velocity.component[axis].assign(production.projection.velocityMPerS[axis].begin(),
				production.projection.velocityMPerS[axis].end());
		}
		return true;
	}

	struct ProductionAffineResidual
	{
		double maximumAbsolute;
		double maximumScaled;
		std::size_t cell;
		std::size_t row;
		ProductionAffineResidual() : maximumAbsolute(0.0),maximumScaled(0.0),cell(0u),row(0u) {}
	};

	ProductionAffineResidual MeasureProductionAffineResidual(
		const RISE::FireProductionResidentStepResult& production,const std::size_t cells)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		const FireCertifiedNullspace& closure=fuel.ConservativeReconstruction();
		ProductionAffineResidual result;
		for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t row=0u;
			row<closure.constraintRows;++row){double residual=0.0,scale=0.0;
			for(std::size_t column=0u;column<closure.stateDimension;++column){const double term=
				closure.constraintMatrix[row*closure.stateDimension+column]*
				production.conservativeValues[column*cells+cell];residual+=term;scale+=std::fabs(term);}
			const double relative=std::fabs(residual)/std::max(1.0,scale);
			if(relative>result.maximumScaled){result.maximumScaled=relative;
				result.maximumAbsolute=std::fabs(residual);result.cell=cell;result.row=row;}
		}
		return result;
	}

	std::string ProductionConservativeDigest(
		const RISE::FireProductionResidentStepResult& production)
	{
		RISECBOR64::Bytes bytes;for(const float value:production.conservativeValues){
			std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));AppendInteger(bytes,bits);}
		return RISECBOR64::SHA256Hex(bytes);
	}

	std::uint64_t ExpectedProjectionSweeps(const RISE::FireProductionProjectionShape& shape)
	{
		std::size_t nx=shape.nx,ny=shape.ny,nz=shape.nz,levels=1u;
		while(nx>4u||ny>4u||nz>4u){if(nx>4u)nx=(nx+1u)/2u;
			if(ny>4u)ny=(ny+1u)/2u;if(nz>4u)nz=(nz+1u)/2u;++levels;}
		return 16u*(32u+6u*(levels-1u));
	}

	int CheckProduction(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 180;
		std::array<std::string,4> targetDigests;if(!ReadTargetDigests(
			directory/"dyadic_targets.v1",targetDigests))return 181;
		std::array<MethaneRunCheckpoint,4> states;std::array<FilteredField,4> filtered;
		std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error))return 182;
			std::vector<std::vector<double> > sealed;const std::filesystem::path target=
				directory/(std::string("oracle_tier")+std::to_string(Tiers[index])+"_sdiv_x8.f64");
			if(DigestFile(target)!=targetDigests[index]||!ReadCalibrationDoublePayload(target,
				states[index].states.size(),8u,sealed))return 183;
			const double flowThrough=6.0*std::sqrt(states[index].values.characteristicDiameterM/Gravity);
			for(std::size_t step=0u;step<8u;++step){
				RISE::FireProductionResidentStepRequest request;
				if(!BuildProductionRequest(states[index],sealed[step],flowThrough/512.0,request,error))
					return 184;
				RISE::FireProductionResidentStepResult production;
				const std::uint64_t commitsBefore=
					RISE::FireProductionResidentStepMetalCommandCommitCount();
				if(!RISE::AdvanceFireProductionResidentStepMetal(request,production,&error)){
					std::fprintf(stderr,"dyadic production tier %u step %zu failed: %s commits=%llu\n",
						Tiers[index],step,error.c_str(),static_cast<unsigned long long>(
							RISE::FireProductionResidentStepMetalCommandCommitCount()-commitsBefore));return 185;}
				if(production.interstageFullGridTransferCount!=0u||
					production.residentProjectionInvocationCount!=1u||
					production.projection.executedVCycleCount!=16u||
					production.projection.executedJacobiSweepCount!=
						ExpectedProjectionSweeps(request.force.shape)){
					std::fprintf(stderr,"dyadic production topology tier=%u step=%zu transfers=%u "
						"projection=%u cycles=%u sweeps=%llu expected=%llu\n",Tiers[index],step,
						production.interstageFullGridTransferCount,
						production.residentProjectionInvocationCount,
						production.projection.executedVCycleCount,
						static_cast<unsigned long long>(
							production.projection.executedJacobiSweepCount),
						static_cast<unsigned long long>(ExpectedProjectionSweeps(
							request.force.shape)));return 186;}
				if(!ApplyProductionResult(production,states[index],error)){
					const ProductionAffineResidual residual=MeasureProductionAffineResidual(
						production,states[index].states.size());
					const std::string digest=ProductionConservativeDigest(production);
					std::fprintf(stderr,"dyadic production affine residual max_abs=%.17g "
						"max_scaled=%.17g cell=%zu row=%zu fp64_envelope=%.17g digest=%s\n",
						residual.maximumAbsolute,residual.maximumScaled,residual.cell,residual.row,
						FireSimulationMethaneRecord::PhysicalV1().AcceptedStateFeasibilityEnvelope().
							kappaEpsilon64*std::numeric_limits<double>::epsilon(),digest.c_str());
					std::fprintf(stderr,"dyadic production state reconstruction tier=%u step=%zu "
						"failed: %s\n",Tiers[index],step,error.c_str());
					if(index==0u&&step==0u&&
						residual.maximumAbsolute==5.2451771873310863e-08&&
						residual.maximumScaled==5.2451771873310863e-08&&
						residual.cell==4915u&&residual.row==2u&&
						digest=="03faf5aad21ef47b5127213dde0e744e92d0d89f4a2a5345b7bd373979564e50"&&
						error.find("certified affine rows")!=std::string::npos)return 190;
					return 187;}
				std::fprintf(stderr,"dyadic production tier=%u step=%zu residual=%.9g valid=%d\n",
					Tiers[index],step,production.projection.maximumPostProjectionResidualPerS,
					production.projection.validationPassed?1:0);
			}
			std::vector<ConservativeVector> conservative(states[index].states.size());
			for(std::size_t cell=0u;cell<conservative.size();++cell)
				conservative[cell]=ToConservativeVector(states[index].states[cell]);
			if(!FilterConservative(states[index],conservative,
				states[index].values.characteristicDiameterM,filtered[index]))return 188;
		}
		const std::array<double,9> dA=FieldDistance(filtered[0],filtered[1]);
		const std::array<double,9> dB=FieldDistance(filtered[2],filtered[3]);
		const FilteredField limitA=Extrapolate(filtered[0],filtered[1]);
		const FilteredField limitB=Extrapolate(filtered[2],filtered[3]);
		const std::array<double,9> limitDifference=FieldDistance(limitA,limitB);
		const std::array<double,9> coarseAToB=FieldDistance(filtered[0],limitB);
		const std::array<double,9> fineAToB=FieldDistance(filtered[1],limitB);
		const std::array<double,9> coarseBToA=FieldDistance(filtered[2],limitA);
		const std::array<double,9> fineBToA=FieldDistance(filtered[3],limitA);
		bool accepted=true;
		for(std::size_t component=0u;component<9u;++component){
			FireProductionCalibration::DyadicDistanceEstimate estimateA,estimateB;
			const bool estimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
				dA[component],VerifiedOrder,estimateA)&&
				FireProductionCalibration::DyadicDistanceAtVerifiedOrder(dB[component],
					VerifiedOrder,estimateB);
			const bool overlap=estimated&&FireProductionCalibration::DyadicLimitBallsOverlap(
				limitDifference[component],estimateA,estimateB);
			const bool approaches=coarseAToB[component]>fineAToB[component]&&
				coarseBToA[component]>fineBToA[component];
			accepted=accepted&&overlap&&approaches;
			std::fprintf(stderr,"dyadic production component=%zu D5_10=%.17g D6_12=%.17g "
				"limit_delta=%.17g radius_sum=%.17g approachA=%d approachB=%d accepted=%d\n",
				component,dA[component],dB[component],limitDifference[component],estimated?
				estimateA.fineRadius+estimateB.fineRadius:0.0,
				coarseAToB[component]>fineAToB[component]?1:0,
				coarseBToA[component]>fineBToA[component]?1:0,overlap&&approaches?1:0);
		}
		return accepted?0:189;
	}
}

#endif
