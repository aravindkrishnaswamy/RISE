// r273 poor-burn closure (read-only): one frozen source step on a retained tier-8 state under each opt-in arm.
// The grid kernel (BuildFrozenMethaneSourcePackets) is the production one; the arms are passed explicitly, so no
// arm depends on RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP or RISE_FIRE_SOOT_SHEET_OXIDATION. The gross yield is the
// process setting RISE_FIRE_METHANE_SOOT_YIELD and should match the state. Mixing times and eligibility as
// cft_check.cpp (interior ignition graph; the burner-contact waiver and the pilot are left out).
// argv: checkpoint [carbon scale, default 1] [cap, default 0.45] [tier, default 8]. A carbon scale of 10 turns a yield-0.03 state into a
// stand-in for yield 0.3 (r272 found soot close to a passive tracer); its temperature field is the 0.03 one. The
// added carbon is taken out of CO2 through the record's soot-oxidation direction, so elements stay balanced.
// Second part: a closed product parcel (mean of the soot-bearing cells above 1300 K) cooled with each arm's frozen
// escape factor for 0.4 s: how fast it leaves the oxidising range and how much of its soot burns before that.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
	if(argc<2)return 2;
	const double carbonScale=argc>2?std::atof(argv[2]):1.0,cap=argc>3?std::atof(argv[3]):0.45,tier=argc>4?std::atof(argv[4]):8.0;
	MethaneRunCheckpoint checkpoint;std::string error;
	if(!LoadMethaneRunCheckpoint(argv[1],checkpoint,error)){std::cerr<<error<<'\n';return 2;}
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationGasOpacityRecord& opacity=FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	const FireSimulationTransportRecord& tr=FireSimulationTransportRecord::OpenV1();
	FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
	authored.poolDiameterM=CapstonePoolDiameterM;
	authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;authored.envelope={{0.0,1.0}};
	authored.durationS=1.0;authored.quality="dstar";authored.numericDStarTier=tier;
	authored.seed=1234;authored.outputFramesPerS=1.0;authored.plumeLaw=true;
	FireCase::RecordV1 caseRecord;const RISECBOR64::Bytes aerosol=AerosolRecord();
	const RISECBOR64::Bytes chem=SyntheticChemRecord();
	if(!FireCase::BuildMethaneV1(authored,fuel,{
		RISECBOR64::SHA256Hex(fuel.RecordBytes()),
		RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
		RISECBOR64::SHA256Hex(tr.RecordBytes()),RISECBOR64::SHA256Hex(opacity.RecordBytes()),
		RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),
		RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem)},caseRecord,error)){std::cerr<<error<<'\n';return 191;}
	PeriodicMACShape shape;shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
	shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
	if(checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
		checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount())return 192;
	auto canonicalState=[&](const std::vector<double>& fractions,const bool injected,MethaneCellState& state)->bool{
		state=MethaneCellState();state.temperatureK=300.0;
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)state.constituent[species]=fractions[species];
		double inverseWeight=0.0;
		for(std::size_t species=0u;species<MethaneCarbon;++species){
			const FireThermochemistrySpecies* record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
			if(record)inverseWeight+=state.constituent[species]/record->molecularWeightKGPerKMol;}
		const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*state.temperatureK*inverseWeight);
		for(double& value:state.constituent)value*=density;
		state.rhoTotalZ=injected?state.TotalDensity():0.0;
		return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),state.temperatureK,state.sensibleEnergyJPerM3,&error);};
	MethaneCellState ambient,injected;
	if(!canonicalState(fuel.AmbientMassFractions(),false,ambient)||!canonicalState(fuel.InjectedMassFractions(),true,injected))return 193;
	OpenBoundaryConfig3D boundary;boundary.ambientDensityKGPerM3=ambient.GasDensity();
	boundary.injectedGasDensityKGPerM3=injected.GasDensity();
	boundary.ambientState=ToConservativeVector(ambient);boundary.injectedState=ToConservativeVector(injected);
	boundary.fuelMassFluxKGPerM2S=0.0;
	const double referenceVelocity=std::sqrt(9.80665*caseRecord.derived.characteristicDiameterM);
	boundary.velocityToleranceMPerS=1.0e-3*referenceVelocity;
	boundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*boundary.velocityToleranceMPerS;
	std::vector<double> sourcePattern;std::vector<std::uint8_t> canonicalPilotMask;
	if(!FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error)||
		!FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error))return 194;
	boundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
	boundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
	for(std::size_t face=0u;face<sourcePattern.size();++face)if(sourcePattern[face]!=0.0){
		boundary.bottomFuelMask[face]=true;
		boundary.bottomFuelMassFluxKGPerM2S[face]=caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];}
	const std::size_t cells=shape.CellCount();
	std::vector<ConservativeVector> conservative(cells);std::vector<double> temperature(cells);
	for(std::size_t cell=0u;cell<cells;++cell){conservative[cell]=ToConservativeVector(checkpoint.states[cell]);
		temperature[cell]=checkpoint.states[cell].temperatureK;}
	OpenMACField3D velocity;velocity.component=checkpoint.velocity.component;std::vector<CellTransportEvaluation> transport;
	if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperature,velocity,boundary,false,fuel,tr,
		FireStateProducerPrecision::Binary32,transport,&error,8u)){std::cerr<<error<<'\n';return 3;}
	IgnitionGrid grid;grid.nx=shape.nx;grid.ny=shape.ny;grid.nz=shape.nz;grid.cells=checkpoint.states;
	grid.pilotMask.resize(cells,false);
	for(std::size_t cell=0u;cell<cells;++cell)grid.pilotMask[cell]=ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,sourcePattern);
	std::vector<bool> eligible;if(!BuildIgnitionEligibility(grid,fuel,fuel,tr,eligible,&error)){std::cerr<<error<<'\n';return 4;}
	const double dx=shape.cellWidthM,dt=checkpoint.lastAcceptedStepS,volume=dx*dx*dx,Tox=fuel.SootOxidationTemperatureK();
	std::vector<MethaneCellState> states=checkpoint.states;std::vector<MethaneReactionStep> steps(cells);
	for(std::size_t cell=0u;cell<cells;++cell){
		MethaneCellState& st=states[cell];
		if(carbonScale!=1.0&&st.constituent[MethaneCarbon]>0.0){
			// More carbon held back means less CO2 and more O2: reverse the record's soot oxidation.
			const double extra=(carbonScale-1.0)*st.constituent[MethaneCarbon];
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)st.constituent[species]=static_cast<double>(
				static_cast<float>(st.constituent[species]-extra*fuel.SootOxidationDelta()[species]));
			if(!SignedMixtureSensibleEnergy(st,st.temperatureK,fuel,st.sensibleEnergyJPerM3,&error)){std::cerr<<error<<'\n';return 5;}
			st.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(st.sensibleEnergyJPerM3));
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(st,fuel.TemperatureMinK(),fuel.TemperatureMaxK(),fuel,st.temperatureK,&error)){std::cerr<<error<<'\n';return 5;}}
		steps[cell].deltaTimeS=dt;steps[cell].maximumAcceptedTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		steps[cell].primaryEligible=eligible[cell];steps[cell].sootOxidationEnabled=true;
		if(!ComputeMixingTimeS(checkpoint.states[cell],transport[cell],tr,dx,ambient.GasDensity(),9.80665,false,steps[cell].mixingTimeS,&error)){std::cerr<<error<<'\n';return 6;}}
	const std::vector<double> cellVolume(cells,volume);const double chi=caseRecord.derived.effectiveRadiativeFraction;
	const double Ta=caseRecord.derived.pilotAmbientTemperatureK;
	double inventory=0.0,hotInventory=0.0;
	for(std::size_t cell=0u;cell<cells;++cell){const double c=std::max(0.0,states[cell].constituent[MethaneCarbon])*volume;inventory+=c;if(states[cell].temperatureK>Tox)hotInventory+=c;}
	std::printf("state %s t=%.3f s dt=%.5f s carbon_scale=%g yield=%g chi=%g cap=%g\n",argv[1],checkpoint.simulationTimeS,dt,carbonScale,
		MethaneGrossSootYieldKGPerKGFuel(fuel),chi,cap);
	std::printf("soot inventory %.2f mg, %.3f of it in cells above %.0f K\n",inventory*1e6,hotInventory/inventory,Tox);
	struct Arm{const char* name;double cap;int sheet;};
	const Arm arms[4]={{"off",-1.0,0},{"soot radiation",cap,0},{"sheet gate",-1.0,1},{"both",cap,1}};
	const double bandTop[5]={0.15,0.30,0.50,1.00,1e9};
	double escape[4]={0,0,0,0};
	std::printf("\narm             HRR_kW  rad_kW  rad/HRR applied  beta    formed_mg/s oxidised_mg/s ox/formed  oxidising_cells soot_share_oxidising  oxidised mg/s by height 0-0.15 0.15-0.3 0.3-0.5 0.5-1 >1 m   hot-soot cooling K/s\n");
	for(int a=0;a<4;++a){
		std::vector<MethaneSourcePacket> packets;RadiationEscapeFactor factor;double applied=0.0;
		if(!BuildFrozenMethaneSourcePackets(states,steps,cellVolume,Ta,caseRecord.derived.referenceHeatReleaseRateW,chi,false,
			fuel,fuel,opacity,packets,factor,&error,8u,true,arms[a].cap,arms[a].sheet,&applied)){std::cerr<<arms[a].name<<": "<<error<<'\n';return 7;}
		double hrr=0,rad=0,formed=0,oxidised=0,share=0,band[5]={0,0,0,0,0},coolNumerator=0,coolWeight=0;std::size_t oxCells=0;
		for(std::size_t cell=0u;cell<cells;++cell){const MethaneSourcePacket& p=packets[cell];
			hrr+=(p.gasHeatReleaseWPerM3+p.sootHeatReleaseWPerM3)*volume;rad+=p.radiativeCoolingWPerM3*volume;
			formed+=p.grossCarbonFormedKGPerM3*volume/dt;oxidised+=p.oxidizedCarbonKGPerM3*volume/dt;
			const double c=std::max(0.0,states[cell].constituent[MethaneCarbon]);
			if(p.oxidizedCarbonKGPerM3>0.0){++oxCells;share+=c*volume;
				const double h=(cell/(shape.nx*shape.ny)+0.5)*dx;int b=0;while(h>=bandTop[b])++b;band[b]+=p.oxidizedCarbonKGPerM3*volume/dt;}
			if(states[cell].temperatureK>Tox&&c>0.0){
				// cooling rate of the gas, K/s, with rho*cp from the certified lower bound (so this is an upper estimate)
				const double cp=MixtureCertifiedCpLowerJPerM3K(states[cell],Tox,states[cell].temperatureK,fuel);
				if(cp>0.0){coolNumerator+=c*p.radiativeCoolingWPerM3/cp;coolWeight+=c;}}}
		escape[a]=factor.accepted;
		std::printf("%-15s %6.2f  %6.2f  %6.3f  %6.3f  %6.3f  %9.2f   %9.2f     %6.3f     %8zu        %6.3f            %8.2f %8.2f %8.2f %8.2f %8.2f      %8.1f\n",
			arms[a].name,hrr*1e-3,rad*1e-3,rad/hrr,applied,factor.accepted,formed*1e6,oxidised*1e6,formed>0?oxidised/formed:0.0,oxCells,share/inventory,
			band[0]*1e6,band[1]*1e6,band[2]*1e6,band[3]*1e6,band[4]*1e6,coolWeight>0?coolNumerator/coolWeight:0.0);
	}
	// Closed product parcel: carbon-weighted mean of the soot-bearing cells above the oxidation temperature.
	MethaneCellState parcel;parcel.producerPrecision=states[0].producerPrecision;double weight=0.0,tau=0.0;
	for(std::size_t cell=0u;cell<cells;++cell){const MethaneCellState& st=states[cell];const double c=st.constituent[MethaneCarbon];
		if(!(st.temperatureK>Tox&&c>0.0))continue;
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)parcel.constituent[species]+=c*st.constituent[species];
		parcel.sensibleEnergyJPerM3+=c*st.sensibleEnergyJPerM3;parcel.rhoTotalZ+=c*st.rhoTotalZ;tau+=c*steps[cell].mixingTimeS;weight+=c;}
	if(!(weight>0.0))return 0;
	for(double& value:parcel.constituent)value=static_cast<double>(static_cast<float>(value/weight));
	parcel.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(parcel.sensibleEnergyJPerM3/weight));
	parcel.rhoTotalZ=static_cast<double>(static_cast<float>(parcel.rhoTotalZ/weight));tau/=weight;
	if(!InvertMethaneTemperatureWithinAcceptedEnvelope(parcel,fuel.TemperatureMinK(),fuel.TemperatureMaxK(),fuel,parcel.temperatureK,&error)){std::cerr<<error<<'\n';return 8;}
	std::printf("\nclosed product parcel: T0=%.0f K, soot %.3g kg/m3 (fv %.3g ppm), O2 %.3g kg/m3, mixing time %.4f s, no fuel burning\n",
		parcel.temperatureK,parcel.constituent[MethaneCarbon],parcel.constituent[MethaneCarbon]/SootEmissionDensityKGPerM3*1e6,parcel.constituent[MethaneO2],tau);
	std::printf("arm             time_to_%.0fK_s  T_at_0.1s  T_at_0.2s  T_at_0.4s  soot_oxidised_fraction_at_0.4s\n",Tox);
	for(int a=0;a<2;++a){
		MethaneCellState st=parcel;const double carbon0=st.constituent[MethaneCarbon];double oxidised=0.0,cross=-1.0,T1=0,T2=0,T4=0;
		MethaneReactionStep step;step.deltaTimeS=dt;step.mixingTimeS=tau;step.maximumAcceptedTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		step.primaryEligible=false;step.sootOxidationEnabled=true;
		const int count=static_cast<int>(std::ceil(0.4/dt));
		for(int n=0;n<count;++n){MethaneSourcePacket packet;MethaneCellState next;
			if(!BuildFrozenMethaneSourcePacket(st,step,Ta,escape[a],fuel,fuel,opacity,packet,&error,a==1)||
				!ApplySourcePacket(st,packet,fuel,next,&error)){std::cerr<<"parcel "<<arms[a].name<<": "<<error<<'\n';return 9;}
			oxidised+=packet.oxidizedCarbonKGPerM3;st=next;const double t=(n+1)*dt;
			if(cross<0.0&&st.temperatureK<=Tox)cross=t;
			if(t<=0.1)T1=st.temperatureK;if(t<=0.2)T2=st.temperatureK;T4=st.temperatureK;}
		std::printf("%-15s %8.3f       %7.0f    %7.0f    %7.0f      %6.3f\n",arms[a].name,cross,T1,T2,T4,oxidised/carbon0);
	}
	return 0;
}
