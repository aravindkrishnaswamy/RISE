// r271 burner face (read-only): how much of RISE's base burning happens in cells that FDS's critical flame temperature
// would extinguish. RISE tests the stoichiometric parcel (StoichiometricParcelPassesCFT) at 1700 K, the generic
// hydrocarbon value; FDS r263 uses methane's 1507 C = 1780.15 K in the equivalent EXTINCT_2 parcel (fire.f90:1420-1490).
// On RISE's own eligible cells: the reactant-limited extent per step (1-exp(-dt/tau)) min(rhoY_F, rhoY_O2/s) with RISE's
// tau, and the share of it in cells whose parcel fails at 1740 K and at 1780.15 K. By height band and by radius:
// core r<0.06 m, rim 0.06-0.15 m, outside r>=0.15 m. Case and boundary setup as mixing_time.cpp.
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
		const double limits[2]={1740.0,1780.15};
		for(std::size_t z=0;z<shape.nz;++z){const double h=(z+.5)*dx;int band=h<.031?0:h<.062?1:h<.12?2:h<.21?3:h<.31?4:h<.49?5:-1;if(band<0)continue;
		 for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){const std::size_t cell=x+shape.nx*(y+shape.ny*z);
		  if(!eligible[cell])continue;const MethaneCellState& st=checkpoint.states[cell];const CellTransportEvaluation& e=transport[cell];
		  const double avail=std::min(std::max(0.,st.constituent[MethaneCH4]),std::max(0.,st.constituent[MethaneO2])/s);if(!(avail>0.))continue;
		  double tauR=0.;if(!ComputeMixingTimeS(st,e,tr,dx,ambient.GasDensity(),g,false,tauR,&error))return 5;
		  const double rx=(x+.5)*dx-.5*shape.nx*dx,ry=(y+.5)*dx-.5*shape.ny*dx,r=std::hypot(rx,ry);const int zone=r<.06?0:r<.15?1:2;
		  const bool rich=st.constituent[MethaneCH4]*s>st.constituent[MethaneO2];
		  const double eR=-std::expm1(-dt/tauR)*avail;double* a=&acc[(band*3+zone)*6];a[0]+=eR;a[5]+=rich?eR:0.;
		  bool passes=false;if(!StoichiometricParcelPassesCFT(st,fuel,fuel,tr.CriticalFlameTemperatureK(),passes,&error))return 8;a[1]+=passes?eR:0.;
		  for(int l=0;l<2;++l){if(!StoichiometricParcelPassesCFT(st,fuel,fuel,limits[l],passes,&error))return 8;if(!passes)a[2+l]+=eR;}
		  a[4]+=1;}}
		std::cerr<<"state "<<checkpointPath.filename().string()<<" t="<<checkpoint.simulationTimeS<<" cft="<<tr.CriticalFlameTemperatureK()<<'\n';return 0;}
}
int main(int argc,char** argv){
 std::vector<double> acc(6*3*6,0.);for(int a=1;a<argc;++a){const int rc=Run(argv[a],acc);if(rc)return rc;}
 const char* bands[6]={"0.00-0.03","0.03-0.06","0.06-0.12","0.12-0.21","0.21-0.31","0.31-0.49"};const char* zones[3]={"core","rim ","out "};
 std::cout<<std::fixed<<std::setprecision(3)<<"states "<<argc-1<<"\nband(m)    zone  extent share  pass@RISE CFT  fail@1740  fail@1780  rich share  cells/state\n";
 double tot=0;for(int i=0;i<18;++i)tot+=acc[i*6];
 for(int b=0;b<6;++b)for(int zn=0;zn<3;++zn){const double* a=&acc[(b*3+zn)*6];if(a[4]==0)continue;
  std::cout<<bands[b]<<"  "<<zones[zn]<<"   "<<std::setw(6)<<a[0]/tot<<"        "<<std::setw(6)<<a[1]/a[0]<<"      "<<std::setw(6)<<a[2]/a[0]<<"     "<<std::setw(6)<<a[3]/a[0]
   <<"     "<<std::setw(6)<<a[5]/a[0]<<"     "<<std::setw(7)<<std::setprecision(1)<<a[4]/(argc-1)<<std::setprecision(3)<<'\n';}
 double f40=0,f80=0;for(int i=0;i<18;++i){f40+=acc[i*6+2];f80+=acc[i*6+3];}
 std::cout<<"all z<0.49 m: fail@1740 "<<f40/tot<<"  fail@1780 "<<f80/tot<<'\n';return 0;
}
