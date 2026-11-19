// r271 rim oxygen (read-only): on the same retained RISE state, the oxygen consumption rate of RISE's source step against
// FDS r263's reactor. FDS (fire.f90 FIRE_FORWARD_EULER, ZETA_0 = INITIAL_UNMIXED_FRACTION = 1 every step) mixes the fraction
// 1-exp(-dt/tau) of the cell and burns that part to completion unless EXTINCT_2 fails (equivalent to RISE's stoichiometric
// parcel test, at methane's 1780.15 K); every cell with fuel and oxygen is a candidate (auto-ignition -273 C).
//   RISE: s (1-exp(-dt_R/tau_R)) min(rhoY_F, rhoY_O2/s) / dt_R on ignition-eligible cells (RISE dt from the checkpoint)
//   FDS : s (1-exp(-dt_F/tau_F)) min(...) / dt_F, dt_F = 2.751 ms (r263 mean step), tau_F the FDS formula
// By height band and radius (core r<0.06 m, rim 0.06-0.15 m), the rates (kg O2/m3/s, cell means) and the difference split
// into eligibility (cells RISE does not burn), extinction (cells FDS extinguishes), and tau+dt (cells both burn).
// Heat release from the RISE rate (LHV x fuel rate) checks against the frames' measured reaction field.
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
		const double dtF=25.091040127212182/9119.,lhv=fuel.LowerHeatingValueJPerKG();
		for(std::size_t z=0;z<shape.nz;++z){const double h=(z+.5)*dx;int band=h<.031?0:h<.062?1:h<.092?2:h<.123?3:h<.21?4:h<.31?5:-1;if(band<0)continue;
		 for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){const std::size_t cell=x+shape.nx*(y+shape.ny*z);
		  const double rx=(x+.5)*dx-.5*shape.nx*dx,ry=(y+.5)*dx-.5*shape.ny*dx,r=std::hypot(rx,ry);const int zone=r<.06?0:r<.15?1:-1;if(zone<0)continue;
		  double* a=&acc[(band*2+zone)*8];a[7]+=1;
		  const MethaneCellState& st=checkpoint.states[cell];const CellTransportEvaluation& e=transport[cell];
		  const double avail=std::min(std::max(0.,st.constituent[MethaneCH4]),std::max(0.,st.constituent[MethaneO2])/s);if(!(avail>0.))continue;
		  double tauR=0.;if(!ComputeMixingTimeS(st,e,tr,dx,ambient.GasDensity(),g,false,tauR,&error))return 5;
		  const double rho=st.GasDensity(),kF=std::pow((e.effectiveViscosityPaS/rho)/(0.1*dx),2.),fd=dx*dx/std::max(e.molecularDiffusivityM2PerS,1e-30),
		   fu=0.4*dx/std::sqrt(2./3.*kF+1e-10),fg=std::sqrt(2.*dx/(g+1e-10)),fmin=std::max(1e-5,dx/100.);
		  const double tauF=std::max(std::max(tr.ChemicalTimeS(),fmin),std::min({fd,fu,fg}));
		  bool fdsBurns=false;if(!StoichiometricParcelPassesCFT(st,fuel,fuel,1780.15,fdsBurns,&error))return 8;
		  const double rR=eligible[cell]?-std::expm1(-dt/tauR)*avail/dt*s:0.,rF=fdsBurns?-std::expm1(-dtF/tauF)*avail/dtF*s:0.;
		  a[0]+=rR;a[1]+=rF;
		  if(!eligible[cell])a[2]+=rF;else if(!fdsBurns)a[3]-=rR;else a[4]+=rF-rR;
		  a[5]+=rR/s*lhv*1e-6;a[6]+=eligible[cell]?1.:0.;}}
		std::cerr<<"state "<<checkpointPath.filename().string()<<" t="<<checkpoint.simulationTimeS<<" dt="<<dt<<'\n';return 0;}
}
int main(int argc,char** argv){
 std::vector<double> acc(6*2*8,0.);for(int a=1;a<argc;++a){const int rc=Run(argv[a],acc);if(rc)return rc;}
 const char* bands[6]={"0.00-0.03","0.03-0.06","0.06-0.09","0.09-0.12","0.12-0.21","0.21-0.31"};const char* zones[2]={"core","rim "};
 std::cout<<std::fixed<<std::setprecision(3)<<"states "<<argc-1<<"\nband(m)    zone   O2 rate RISE   FDS reactor  FDS/RISE   diff: eligibility  extinction  tau+dt   RISE q MW/m3  RISE eligible share\n";
 for(int b=0;b<6;++b)for(int z=0;z<2;++z){const double* a=&acc[(b*2+z)*8];const double n=a[7];
  std::cout<<bands[b]<<"  "<<zones[z]<<"   "<<std::setw(9)<<a[0]/n<<"    "<<std::setw(9)<<a[1]/n<<"    "<<std::setw(6)<<a[1]/a[0]<<"        "<<std::setw(8)<<a[2]/n<<"   "<<std::setw(8)<<a[3]/n<<"  "<<std::setw(8)<<a[4]/n
   <<"     "<<std::setw(7)<<a[5]/n<<"     "<<std::setw(6)<<a[6]/n<<'\n';}
 return 0;
}
