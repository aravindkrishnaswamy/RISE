// r273 collapse diagnosis (read-only): where the soot-radiation loss lands on one retained state.
// One production source step (BuildFrozenMethaneSourcePackets) with soot radiation at the given cap and the sheet gate
// off, then per-cell census of reaction heat and realised radiative loss, the budget arithmetic (S/G, applied
// fraction, cap, escape factor), soot and gas Planck-mean optical depth along horizontal lines through the axis, and
// the flame-scale mean beam length 3.6 V/A of the region above 1000 K. Setup (mixing times, eligibility) as
// frozen_response.cpp. The self-absorption opt-in (RISE_FIRE_SOOT_SELF_ABSORPTION) is passed explicitly per arm.
// argv: checkpoint [cap, default 0.45] [carbon scale, default 1] [tier, default 8]
#define R273_SELF_ABSORPTION_ARMS 1
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
	if(argc<2)return 2;
	const double cap=argc>2?std::atof(argv[2]):0.45,carbonScale=argc>3?std::atof(argv[3]):1.0,tier=argc>4?std::atof(argv[4]):8.0;
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
	const std::size_t cells=shape.CellCount(),nx=shape.nx,ny=shape.ny,nz=shape.nz;
	std::vector<ConservativeVector> conservative(cells);std::vector<double> temperature(cells);
	for(std::size_t cell=0u;cell<cells;++cell){conservative[cell]=ToConservativeVector(checkpoint.states[cell]);
		temperature[cell]=checkpoint.states[cell].temperatureK;}
	OpenMACField3D velocity;velocity.component=checkpoint.velocity.component;std::vector<CellTransportEvaluation> transport;
	if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperature,velocity,boundary,false,fuel,tr,
		FireStateProducerPrecision::Binary32,transport,&error,8u)){std::cerr<<error<<'\n';return 3;}
	IgnitionGrid grid;grid.nx=nx;grid.ny=ny;grid.nz=nz;grid.cells=checkpoint.states;
	grid.pilotMask.resize(cells,false);
	for(std::size_t cell=0u;cell<cells;++cell)grid.pilotMask[cell]=ProductionPilotCommandCell(cell,nx,ny,canonicalPilotMask,sourcePattern);
	std::vector<bool> eligible;if(!BuildIgnitionEligibility(grid,fuel,fuel,tr,eligible,&error)){std::cerr<<error<<'\n';return 4;}
	const double dx=shape.cellWidthM,dt=checkpoint.lastAcceptedStepS,volume=dx*dx*dx,Tox=fuel.SootOxidationTemperatureK();
	std::vector<MethaneCellState> states=checkpoint.states;std::vector<MethaneReactionStep> steps(cells);
	for(std::size_t cell=0u;cell<cells;++cell){
		MethaneCellState& st=states[cell];
		if(carbonScale!=1.0&&st.constituent[MethaneCarbon]>0.0){
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
	const double Ta=caseRecord.derived.pilotAmbientTemperatureK,sigma=5.670374419e-8;
	// Unscaled per-cell gas and soot exchange on the post-reaction state, as the kernel forms them.
	std::vector<double> g(cells,0.0),s(cells,0.0),kSoot(cells,0.0),kGas(cells,0.0),Tpost(cells,0.0);
	for(std::size_t cell=0u;cell<cells;++cell){
		MethaneSourcePacket reaction;MethaneCellState post;
		if(!BuildMethaneReactionPacket(states[cell],fuel,steps[cell],reaction,&error,&post)){std::cerr<<error<<'\n';return 7;}
		GasExchangeEvaluation e;if(!EvaluateGasExchange(post,post.temperatureK,Ta,fuel,opacity,e,&error)){std::cerr<<error<<'\n';return 7;}
		const double coefficient=MethaneSootExchangeCoefficientWPerM3K5(post);
		g[cell]=e.exchangeWPerM3;s[cell]=MethaneSootExchangeWPerM3(coefficient,post.temperatureK,Ta);Tpost[cell]=post.temperatureK;
		kSoot[cell]=coefficient*post.temperatureK/(4.0*sigma);   // Planck-mean soot absorption, 1/m
		const double T4=std::pow(post.temperatureK,4)-std::pow(Ta,4);kGas[cell]=T4>0.0?g[cell]/(4.0*sigma*T4):0.0;}
	std::printf("state %s t=%.4f s step=%llu dt=%.6f carbon_scale=%g yield=%g chi=%g cap=%g dx=%.4f grid=%zux%zux%zu\n",argv[1],
		checkpoint.simulationTimeS,(unsigned long long)checkpoint.acceptedSteps,dt,carbonScale,MethaneGrossSootYieldKGPerKGFuel(fuel),chi,cap,dx,nx,ny,nz);
	// Arms: soot radiation at the cap; and, if the self-absorption opt-in is compiled in, with it on (flame-scale L).
	struct Arm{const char* name;double cap;double selfAbsorption;};
	std::vector<Arm> arms={{"gas only",-1.0,-1.0},{"soot thin",cap,-1.0}};
#ifdef R273_SELF_ABSORPTION_ARMS
	arms.push_back({"soot self-abs",cap,1.0});
#endif
	for(const Arm& arm:arms){
		std::vector<MethaneSourcePacket> packets;RadiationEscapeFactor factor;double applied=0.0;
#ifdef R273_SELF_ABSORPTION_ARMS
		if(!BuildFrozenMethaneSourcePackets(states,steps,cellVolume,Ta,caseRecord.derived.referenceHeatReleaseRateW,chi,false,
			fuel,fuel,opacity,packets,factor,&error,8u,true,arm.cap,0,&applied,arm.selfAbsorption>0.0?1:0,nx,ny,nz))
#else
		if(!BuildFrozenMethaneSourcePackets(states,steps,cellVolume,Ta,caseRecord.derived.referenceHeatReleaseRateW,chi,false,
			fuel,fuel,opacity,packets,factor,&error,8u,true,arm.cap,0,&applied))
#endif
			{std::cerr<<arm.name<<": "<<error<<'\n';return 8;}
		double Q=0,R=0,G=0,S=0,Qhot=0,Rhot=0,Rhot15=0,Qhot15=0,Rreact=0,Qreact=0,RsootHot=0,ox=0,formed=0;
		std::size_t hot=0,hotOver=0,react=0,reactOver=0;
		for(std::size_t c=0;c<cells;++c){const MethaneSourcePacket& p=packets[c];
			const double q=(p.gasHeatReleaseWPerM3+p.sootHeatReleaseWPerM3)*volume,r=p.radiativeCoolingWPerM3*volume;
			Q+=q;R+=r;G+=g[c]*volume;S+=s[c]*volume;ox+=p.oxidizedCarbonKGPerM3*volume/dt;formed+=p.grossCarbonFormedKGPerM3*volume/dt;
			const double T=Tpost[c];
			if(T>Tox){Qhot+=q;Rhot+=r;++hot;if(r>q)++hotOver;if(g[c]+s[c]>0.0)RsootHot+=r*(arm.cap>0.0?s[c]/(g[c]+s[c]):0.0);}
			if(T>1500.0){Qhot15+=q;Rhot15+=r;}
			if(q>0.0){Qreact+=q;Rreact+=r;++react;if(r>q)++reactOver;}}
		std::printf("\n[%s] Q=%.2f kW  G=%.2f kW  S=%.2f kW  S/G=%.3f  chi(1+S/G)=%.3f  applied=%.3f%s  beta=%.4f  realised R=%.2f kW  R/Q=%.3f\n",
			arm.name,Q*1e-3,G*1e-3,S*1e-3,G>0?S/G:0.0,chi*(1.0+(G>0?S/G:0.0)),applied,applied>=std::max(chi,arm.cap)-1e-12&&arm.cap>0?" (AT CAP)":"",
			factor.accepted,R*1e-3,R/Q);
		std::printf("  soot formed %.1f mg/s oxidised %.1f mg/s\n",formed*1e6,ox*1e6);
		std::printf("  cells above %.0f K: %zu (%.2f L); hold %.3f of Q and %.3f of R; R/Q there %.2f; %zu of them lose more than they release; soot share of that loss %.3f\n",
			Tox,hot,hot*volume*1e3,Qhot/Q,Rhot/R,Qhot>0?Rhot/Qhot:0.0,hotOver,Rhot>0?RsootHot/Rhot:0.0);
		std::printf("  cells above 1500 K: Q %.2f kW R %.2f kW (R/Q %.2f)\n",Qhot15*1e-3,Rhot15*1e-3,Qhot15>0?Rhot15/Qhot15:0.0);
		std::printf("  reacting cells: %zu, R/Q %.3f, %zu lose more than they release\n",react,Qreact>0?Rreact/Qreact:0.0,reactOver);
		// Loss and heat by temperature band.
		const double edges[7]={0,800,1000,1200,1300,1500,1e9};
		std::printf("  band_K       Q_kW   R_kW  R_soot_kW  soot_mg  volume_L\n");
		for(int b=0;b<6;++b){double qb=0,rb=0,sb=0,mb=0,vb=0;
			for(std::size_t c=0;c<cells;++c){const double T=Tpost[c];if(T<edges[b]||T>=edges[b+1])continue;const MethaneSourcePacket& p=packets[c];
				qb+=(p.gasHeatReleaseWPerM3+p.sootHeatReleaseWPerM3)*volume;rb+=p.radiativeCoolingWPerM3*volume;
				if(arm.cap>0&&g[c]+s[c]>0)sb+=p.radiativeCoolingWPerM3*volume*s[c]/(g[c]+s[c]);
				mb+=std::max(0.0,states[c].constituent[MethaneCarbon])*volume;if(T>800)vb+=volume;}
			std::printf("  %5.0f-%-5.0f %6.2f %6.2f  %6.2f    %7.2f  %7.2f\n",edges[b],std::min(edges[b+1],9999.0),qb*1e-3,rb*1e-3,sb*1e-3,mb*1e6,vb*1e3);}
	}
#ifdef R273_SELF_ABSORPTION_ARMS
	{	// Attribution of the self-absorption closure: column escape inside the old (gas-normalised) budget, the
		// decoupled budget without the column, and both. Q from a gas-only pass; fractions before the cap.
		std::vector<double> escape;if(!ComputeMethaneSootColumnEscape(nx,ny,nz,dx,kSoot,escape,&error)){std::cerr<<error<<'\n';return 9;}
		std::vector<MethaneSourcePacket> packets;RadiationEscapeFactor factor;
		if(!BuildFrozenMethaneSourcePackets(states,steps,cellVolume,Ta,caseRecord.derived.referenceHeatReleaseRateW,chi,false,
			fuel,fuel,opacity,packets,factor,&error,8u,true,-1.0,0,0,0)){std::cerr<<error<<'\n';return 9;}
		double Q=0,G=0,S=0,SP=0,SPhot=0,Shot=0;for(std::size_t c=0;c<cells;++c){const MethaneSourcePacket& p=packets[c];
			Q+=(p.gasHeatReleaseWPerM3+p.sootHeatReleaseWPerM3)*volume;G+=g[c]*volume;S+=s[c]*volume;SP+=escape[c]*std::max(0.0,s[c])*volume;
			if(Tpost[c]>Tox){SPhot+=escape[c]*std::max(0.0,s[c])*volume;Shot+=std::max(0.0,s[c])*volume;}}
		std::printf("\nattribution (uncapped fractions): gas-normalised thin %.3f; gas-normalised with column escape %.3f; own emission thin %.3f; own emission with column escape %.3f\n",
			chi*(1+S/G),chi*(1+SP/G),chi+S/Q,chi+SP/Q);
		std::printf("  gas escape factor beta %.4f; soot column escape: emission-weighted mean %.3f, in cells above %.0f K %.3f\n",chi*Q/G,S>0?SP/S:1.0,Tox,Shot>0?SPhot/Shot:1.0);
		std::printf("  column escape on the axis by height:");
		for(const double h:{0.05,0.1,0.2,0.3,0.5,0.8}){const std::size_t z=std::min(nz-1,std::size_t(h/dx));std::printf(" %.2fm %.3f",h,escape[(z*ny+ny/2)*nx+nx/2]);}
		std::printf("\n");
	}
#endif
	// Optical depth along x through the axis, and the y-maximum over chords at each height.
	std::printf("\nheight_m  tau_soot_axis  tau_gas_axis  tau_soot_max_chord  kappa_soot_max_1/m  fv_max_ppm  T_max_K\n");
	for(const double h:{0.05,0.1,0.15,0.2,0.3,0.45,0.6,0.8,1.0}){
		const std::size_t z=std::min(nz-1,std::size_t(h/dx));double tauAxis=0,tauGasAxis=0,tauMax=0,kMax=0,fvMax=0,TMax=0;
		for(std::size_t y=0;y<ny;++y){double tau=0;for(std::size_t x=0;x<nx;++x){const std::size_t c=(z*ny+y)*nx+x;tau+=kSoot[c]*dx;
				kMax=std::max(kMax,kSoot[c]);fvMax=std::max(fvMax,states[c].constituent[MethaneCarbon]/SootEmissionDensityKGPerM3);TMax=std::max(TMax,Tpost[c]);
				if(y==ny/2)tauGasAxis+=kGas[c]*dx;}
			if(y==ny/2)tauAxis=tau;tauMax=std::max(tauMax,tau);}
		std::printf("%5.2f     %8.3f       %8.3f      %8.3f            %8.3f          %7.3f    %6.0f\n",h,tauAxis,tauGasAxis,tauMax,kMax,fvMax*1e6,TMax);}
	// Flame-scale mean beam length of the region above 1000 K (Hottel, L = 3.6 V/A, A from exposed cell faces).
	for(const double threshold:{1000.0,800.0}){
		std::size_t inside=0,faces=0;auto in=[&](long x,long y,long z){return x>=0&&y>=0&&z>=0&&x<long(nx)&&y<long(ny)&&z<long(nz)&&Tpost[(z*ny+y)*nx+x]>threshold;};
		double kWeighted=0,sWeight=0;
		for(long z=0;z<long(nz);++z)for(long y=0;y<long(ny);++y)for(long x=0;x<long(nx);++x)if(in(x,y,z)){++inside;
			const std::size_t c=(z*ny+y)*nx+x;kWeighted+=kSoot[c]*s[c];sWeight+=s[c];
			faces+=!in(x-1,y,z)+!in(x+1,y,z)+!in(x,y-1,z)+!in(x,y+1,z)+(z>0&&!in(x,y,z-1))+!in(x,y,z+1);}
		const double V=inside*volume,A=faces*dx*dx,L=A>0?3.6*V/A:0.0,kMean=sWeight>0?kWeighted/sWeight:0.0;
		// Thin vs gray-slab-attenuated soot emission with this L and the local kappa.
		double Sthin=0,Sabs=0;for(std::size_t c=0;c<cells;++c){Sthin+=s[c]*volume;const double t=kSoot[c]*L;Sabs+=s[c]*volume*(t>1e-12?-std::expm1(-t)/t:1.0);}
		std::printf("region T>%.0f K: V=%.2f L  A=%.3f m2  L_m=3.6V/A=%.3f m  emission-weighted soot kappa %.3f 1/m (tau %.3f)  soot emission thin %.2f kW, local-kappa x L attenuated %.2f kW (%.3f)\n",
			threshold,V*1e3,A,L,kMean,kMean*L,Sthin*1e-3,Sabs*1e-3,Sthin>0?Sabs/Sthin:1.0);}
	return 0;
}
