// r271 mixing time (read-only): RISE's tau_mix (ComputeMixingTimeS on the transport the run used,
// BuildOpenStageTransportEvaluations3D) against FDS r263's formula (fire.f90:809-821) on the same retained state.
//   RISE: max(tau_chem=1e-4, min(D^2/(D_mol+D_sgs), D/sqrt(2k), sqrt(2D/g')))     g' reduced gravity
//   FDS : max(max(tau_chem, D/100), min(D^2/D_mol, 0.4 D/sqrt(2k/3), sqrt(2D/g)))    k from total mu, C=0.1
// Per base band: reactant-limited extent per step (1-exp(-dt/tau)) min(rhoY_F, rhoY_O2/s) on eligible cells, summed
// for each formula; the share of that extent under each active term; and the extent-weighted mean tau.
// Case and boundary setup copied from RunProductionCheckpointPhysicsDiagnosticChild (tests/FireSequenceTest.cpp).
// argv: checkpoints.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
namespace {
int Run(const std::filesystem::path& checkpointPath,std::vector<double>& acc){
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)){std::cerr<<error<<'\n';return 2;}
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
		authored.poolDiameterM=CapstonePoolDiameterM;
		authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;authored.envelope={{0.0,1.0}};
		authored.durationS=1.0;authored.quality="dstar";
		authored.numericDStarTier=8.0;
		authored.seed=1234;authored.outputFramesPerS=1.0;authored.plumeLaw=true;
		FireCase::RecordV1 caseRecord;const RISECBOR64::Bytes aerosol=AerosolRecord();
		const RISECBOR64::Bytes chem=SyntheticChemRecord();
		if(!FireCase::BuildMethaneV1(authored,fuel,{
			RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),
			RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem)},caseRecord,error))return 191;
		PeriodicMACShape shape;shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
		shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
		if(checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
			checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount())return 192;

		auto canonicalState=[&](const std::vector<double>& fractions,
			const bool injected,MethaneCellState& state)->bool{
			if(fractions.size()!=MethaneSpeciesCount)return false;
			state=MethaneCellState();state.temperatureK=300.0;
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				state.constituent[species]=fractions[species];
			double inverseWeight=0.0;
			for(std::size_t species=0u;species<MethaneCarbon;++species){
				const FireThermochemistrySpecies* record=fuel.FindSpecies(
					fuel.SpeciesOrder()[species].c_str());
				if(record)inverseWeight+=state.constituent[species]/record->molecularWeightKGPerKMol;
			}
			const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
				state.temperatureK*inverseWeight);
			for(double& value:state.constituent)value*=density;
			state.rhoTotalZ=injected?state.TotalDensity():0.0;
			return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),
				state.temperatureK,state.sensibleEnergyJPerM3,&error);
		};
		MethaneCellState ambient,injected;
		if(!canonicalState(fuel.AmbientMassFractions(),false,ambient)||
			!canonicalState(fuel.InjectedMassFractions(),true,injected))return 193;
		OpenBoundaryConfig3D boundary;boundary.ambientDensityKGPerM3=ambient.GasDensity();
		boundary.injectedGasDensityKGPerM3=injected.GasDensity();
		boundary.ambientState=ToConservativeVector(ambient);
		boundary.injectedState=ToConservativeVector(injected);
		boundary.fuelMassFluxKGPerM2S=0.0;
		const double referenceVelocity=std::sqrt(9.80665*
			caseRecord.derived.characteristicDiameterM);
		boundary.velocityToleranceMPerS=1.0e-3*referenceVelocity;
		boundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*
			boundary.velocityToleranceMPerS;
		std::vector<double> sourcePattern;
		std::vector<std::uint8_t> canonicalPilotMask;
		if(!FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error)||
			!FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error))return 194;
		boundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
		boundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
		for(std::size_t face=0u;face<sourcePattern.size();++face)if(sourcePattern[face]!=0.0){
			boundary.bottomFuelMask[face]=true;
			boundary.bottomFuelMassFluxKGPerM2S[face]=
				caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
		}


		std::vector<ConservativeVector> conservative(shape.CellCount());std::vector<double> temperature(shape.CellCount());
		for(std::size_t cell=0u;cell<shape.CellCount();++cell){conservative[cell]=ToConservativeVector(checkpoint.states[cell]);
			temperature[cell]=checkpoint.states[cell].temperatureK;}
		OpenMACField3D velocity;velocity.component=checkpoint.velocity.component;std::vector<CellTransportEvaluation> transport;
		if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperature,velocity,boundary,false,fuel,
			FireSimulationTransportRecord::OpenV1(),FireStateProducerPrecision::Binary32,transport,&error,8u))return 3;
		IgnitionGrid grid;grid.nx=shape.nx;grid.ny=shape.ny;grid.nz=shape.nz;grid.cells=checkpoint.states;
		grid.pilotMask.resize(shape.CellCount(),false);
		for(std::size_t cell=0u;cell<shape.CellCount();++cell)grid.pilotMask[cell]=ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,sourcePattern);
		std::vector<bool> eligible;if(!BuildIgnitionEligibility(grid,fuel,fuel,FireSimulationTransportRecord::OpenV1(),eligible,&error))return 4;
		const double dx=shape.cellWidthM,dt=checkpoint.lastAcceptedStepS,g=9.80665,s=fuel.StoichiometricOxygenKGPerKGFuel();
		const FireSimulationTransportRecord tr=FireSimulationTransportRecord::OpenV1();
		for(std::size_t z=0;z<shape.nz;++z){const double h=(z+.5)*dx;int band=h<.06?0:h<.12?1:h<.21?2:h<.31?3:h<.49?4:h<1.0?5:-1;if(band<0)continue;
		 for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){const std::size_t cell=x+shape.nx*(y+shape.ny*z);
		  if(!eligible[cell])continue;const MethaneCellState& st=checkpoint.states[cell];const CellTransportEvaluation& e=transport[cell];
		  const double avail=std::min(std::max(0.,st.constituent[MethaneCH4]),std::max(0.,st.constituent[MethaneO2])/s);if(!(avail>0.))continue;
		  double tauR=0.;if(!ComputeMixingTimeS(st,e,tr,dx,ambient.GasDensity(),g,false,tauR,&error))return 5;
		  const double rho=st.GasDensity(),gr=std::max(0.,g*(ambient.GasDensity()-rho)/rho);
		  const double rd=dx*dx/e.totalDiffusivityM2PerS,ku=e.eddyViscosityM2PerS>0.?std::pow(e.eddyViscosityM2PerS/(tr.VremanCnu()*dx),2.):0.;
		  const double ru=ku>0.?dx/std::sqrt(2.*ku):1e30,rg=gr>0.?std::sqrt(2.*dx/gr):1e30;
		  const double rm=std::min({rd,ru,rg});int termR=rm<=tr.ChemicalTimeS()?3:(rm==rd?0:(rm==ru?1:2));
		  if(!FireFDSMixingTimeSelected()&&std::abs(std::max(tr.ChemicalTimeS(),rm)-tauR)>1e-12*tauR)return 6;  // must reproduce ComputeMixingTimeS
		  const double kF=std::pow((e.effectiveViscosityPaS/rho)/(0.1*dx),2.),fd=dx*dx/std::max(e.molecularDiffusivityM2PerS,1e-30),
		   fu=0.4*dx/std::sqrt(2./3.*kF+1e-10),fg=std::sqrt(2.*dx/(g+1e-10)),fmin=std::max(1e-5,dx/100.);
		  const double fm=std::min({fd,fu,fg}),tauF=std::max(std::max(tr.ChemicalTimeS(),fmin),fm);
		  // With RISE_FIRE_FDS_MIXING_TIME=1 the production function must reproduce this independent FDS form.
		  if(FireFDSMixingTimeSelected()){acc[15]+=1;if(std::abs(tauF-tauR)>1e-12*tauF)return 7;}int termF=tauF==fmin?3:(fm==fd?0:(fm==fu?1:2));
		  const double eR=-std::expm1(-dt/tauR)*avail,eF=-std::expm1(-dt/tauF)*avail;double* a=&acc[band*16];
		  a[0]+=eR;a[1]+=eF;a[2+termR]+=eR;a[6+termF]+=eF;a[10]+=eR*tauR;a[11]+=eF*tauF;a[12]+=1;a[13]+=eR*std::log(tauF/tauR);a[14]+=eR*(e.eddyViscosityM2PerS);}}
		std::cerr<<"state "<<checkpointPath.filename().string()<<" t="<<checkpoint.simulationTimeS<<" dt="<<dt<<'\n';return 0;}
}
int main(int argc,char** argv){
 std::vector<double> acc(6*16,0.);for(int a=1;a<argc;++a){const int rc=Run(argv[a],acc);if(rc)return rc;}
 const char* bands[]={"0.00-0.06","0.06-0.12","0.12-0.21","0.21-0.31","0.31-0.49","0.49-1.00"};
 std::cout<<std::setprecision(4)<<"states "<<argc-1<<"\nband(m)    cells  extent FDS/RISE  RISE share d/u/g/chem      FDS share d/u/g/min       mean tau RISE  FDS (s)  <ln tauF/tauR>\n";
 for(int b=0;b<6;++b){const double* a=&acc[b*16];std::cout<<bands[b]<<"  "<<std::setw(7)<<a[12]/(argc-1)<<"   "<<std::setw(6)<<a[1]/a[0]<<"      ";
  for(int t=0;t<4;++t)std::cout<<std::setw(5)<<a[2+t]/a[0]<<' ';std::cout<<"   ";for(int t=0;t<4;++t)std::cout<<std::setw(5)<<a[6+t]/a[1]<<' ';
  std::cout<<"     "<<std::setw(7)<<a[10]/a[0]<<"  "<<std::setw(7)<<a[11]/a[1]<<"   "<<std::setw(6)<<a[13]/a[0]<<'\n';}
 if(FireFDSMixingTimeSelected())std::cout<<"fds_switch_cells_matched "<<[&]{double n=0;for(int b=0;b<6;++b)n+=acc[b*16+15];return n;}()<<'\n';
 return 0;}
