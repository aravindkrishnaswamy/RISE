int RunProductionGoldenCompositionFixture(const std::filesystem::path& checkpointPath,
	const std::filesystem::path& snapshotDirectory,
	const unsigned int manifoldRetryCandidate=0u,const double manifoldRetryStepS=0.0,
	const std::size_t manifoldRetrySlice=0u,
	const std::vector<std::tuple<std::size_t,unsigned int,double>>& manifoldRetrySchedule={})
{
	auto setFixtureEnvironment=[](const char* name,const char* value){
#if defined(_WIN32)
		return _putenv_s(name,value)==0;
#else
		return setenv(name,value,1)==0;
#endif
	};
	auto clearFixtureEnvironment=[](const char* name){
#if defined(_WIN32)
		return _putenv_s(name,"")==0;
#else
		return unsetenv(name)==0;
#endif
	};
	auto continueDrainAwareRetry=[&](const unsigned int candidate,const double step,
		const std::size_t retrySlice){
		auto nextSchedule=manifoldRetrySchedule;
		const auto existing=std::find_if(nextSchedule.begin(),nextSchedule.end(),
			[&](const auto& value){return std::get<0>(value)==retrySlice;});
		if(existing==nextSchedule.end())nextSchedule.emplace_back(retrySlice,candidate,step);
		else *existing=std::make_tuple(retrySlice,candidate,step);
		return RunProductionGoldenCompositionFixture(checkpointPath,snapshotDirectory,
			candidate,step,retrySlice,nextSchedule);
	};
	auto continueAfterDrainAwareRefusal=[&](const unsigned int candidate,
		const RISE::FireProductionResidentStepResult& attempted,
		RISE::FireProductionResidentStepResult* releaseAttempted,
		RISE::FireProductionResidentStepRequest* releaseRequest,
		MethaneRunCheckpoint* releaseBeginning,const std::size_t retrySlice){
		unsigned int nextCandidate=0u;double nextStep=0.0;
		if(RISE::ClassifyFireProductionResidentStepAttempt(candidate,attempted,
			nextCandidate,nextStep)!=
			RISE::FireProductionResidentStepAttemptDisposition::RetryAtSuggestedTimeStep)
			return 203;
		if(releaseAttempted)*releaseAttempted=RISE::FireProductionResidentStepResult();
		if(releaseRequest)*releaseRequest=RISE::FireProductionResidentStepRequest();
		if(releaseBeginning)*releaseBeginning=MethaneRunCheckpoint();
		return continueDrainAwareRetry(nextCandidate,nextStep,retrySlice);
	};
	static const char* checkpointDigest=
		"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
	static const std::array<const char*,8> beginningDigests={{
		"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947",
		"e27a165101c2f7b1c4f64228ddd3d8915df6b2df4f3d3f1c9d1709c6be3e6ca6",
		"a2bb82402afca8cade69010b050cafd0c007caadfc4c55c753991c1c09f41ebb",
		"1b3a1b70f79356174624c331ffbf1a19af51e03a6c0b07cc32223cdcf4a32b81",
		"4f289f0eb239acf3834a3592dee08557f7fd614e52d9a4cbbf0ba525c40892e2",
		"9933b5711d57765c02dd7d0ed06578ff2786aef88a69dcd7f83f584f5fd9f74e",
		"39332f556e068646e3a649ab8a8a34c495daff0e280600cf13600d192b07e726",
		"000a7fe24c64b00a61ec6256b75d0403a193100752abacb39e9f68149f20402b"}};
	if(DigestFile(checkpointPath)!=checkpointDigest){std::fprintf(stderr,
		"production golden composition checkpoint digest mismatch\n");return 110;}
	const char* retryControllerREDValue=std::getenv(
		"RISE_FIRE_DRAIN_AWARE_RETRY_CONTROLLER_RED");
	if(retryControllerREDValue&&std::strcmp(retryControllerREDValue,"1")!=0)return 203;
	if(retryControllerREDValue){
		if(manifoldRetryCandidate==2u&&manifoldRetryStepS==0x1p-12){
			std::fprintf(stderr,"DRAIN_AWARE_RETRY_CONTROLLER_RED refused_candidate=1 "
				"next_candidate=2 cap=%u golden=%s\n",
				RISE::FireStepRejectionRetryCap,checkpointDigest);
			return 204;
		}
		if(manifoldRetryCandidate!=0u||manifoldRetryStepS!=0.0)return 203;
		RISE::FireProductionResidentStepResult refused;
		refused.representedTimeStepS=0x1p-11f;
		refused.manifoldPlateauPassed=false;
		refused.manifoldNextTimeStepAvailable=true;
		refused.suggestedManifoldTimeStepS=0x1p-12;
		return continueAfterDrainAwareRefusal(1u,refused,&refused,nullptr,nullptr,0u);
	}
	const std::array<std::uint64_t,8> stepBits={{
		4543432537948766955ull,4544197666642132584ull,4544654590867399846ull,
		4545157207515193834ull,4545710085827767221ull,4546318251971597947ull,
		4542483635102441249ull,4543219516136476427ull}};
	const std::array<std::array<std::size_t,3>,2> steepFrontCoordinates={{{{40u,40u,1u}},
		{{41u,44u,3u}}}};
	const std::array<std::size_t,2> steepFrontComponents={{0u,8u}};
	auto fromBits=[](std::uint64_t bits){double value=0.0;std::memcpy(&value,&bits,sizeof(value));return value;};
	const FireSimulationMethaneRecord fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord transport=FireSimulationTransportRecord::OpenV1();
	const FireSimulationGasOpacityRecord opacity=
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	std::string error;
	MethaneCellState ambient;ambient.temperatureK=300.0;
	for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
		ambient.constituent[species]=fuel.AmbientMassFractions()[species];
	double ambientInverseWeight=0.0;
	for(std::size_t species=0u;species<MethaneCarbon;++species){
		const FireThermochemistrySpecies* record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
		if(record)ambientInverseWeight+=ambient.constituent[species]/record->molecularWeightKGPerKMol;
	}
	const double ambientDensity=fuel.ThermodynamicPressurePa()/(8314.46261815324*
		ambient.temperatureK*ambientInverseWeight);
	for(double& value:ambient.constituent)value*=ambientDensity;
	ambient.rhoTotalZ=0.0;
	if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),
		ambient.temperatureK,ambient.sensibleEnergyJPerM3,&error))return 111;
	MethaneCellState injected;injected.temperatureK=300.0;
	for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
		injected.constituent[species]=fuel.InjectedMassFractions()[species];
	double injectedInverseWeight=0.0;
	for(std::size_t species=0u;species<MethaneCarbon;++species){
		const FireThermochemistrySpecies* record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
		if(record)injectedInverseWeight+=injected.constituent[species]/record->molecularWeightKGPerKMol;
	}
	const double injectedDensity=fuel.ThermodynamicPressurePa()/(8314.46261815324*
		injected.temperatureK*injectedInverseWeight);
	for(double& value:injected.constituent)value*=injectedDensity;
	injected.rhoTotalZ=injected.TotalDensity();
	if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(injected),
		injected.temperatureK,injected.sensibleEnergyJPerM3,&error))return 112;

	double maximumScalarRelative=0.0,maximumScalarAbsolute=0.0;
	double maximumVelocityAbsolute=0.0,maximumLedgerRelative=0.0;
	double maximumOracleThreadScalar=0.0,maximumOracleThreadVelocity=0.0;
	double maximumProductionResidual=0.0,maximumOracleResidual=0.0;
		double maximumEnvelopeOvershoot=0.0;
	double maximumPinnedProbeOvershoot=0.0,minimumBeginningDensity=0.0;
	double minimumConservativeDensity=0.0;
	std::array<float,8> projectionResiduals={{}};
	std::array<double,8> independentProjectionResiduals={{}};
	std::array<double,8> oracleProjectionResiduals={{}},oraclePreProjectionResiduals={{}};
	std::array<double,8> reductionRatios={{}};
	std::array<double,2> minimumPinnedProbeContrast={{
		std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()}};
	unsigned int monitoredProjectionMisses=0u;
	const char* plateauProbeValue=std::getenv("RISE_FIRE_RESTORATION_PLATEAU_PROBE");
	if(plateauProbeValue&&std::strcmp(plateauProbeValue,"1")!=0)return 246;
	const bool plateauProbe=plateauProbeValue!=nullptr;
	const char* manifoldProbeValue=std::getenv("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE");
	if(manifoldProbeValue&&std::strcmp(manifoldProbeValue,"1")!=0)return 255;
	const bool manifoldProbe=manifoldProbeValue!=nullptr;
	const char* stageBudgetProbeValue=std::getenv("RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE");
	if(stageBudgetProbeValue&&std::strcmp(stageBudgetProbeValue,"1")!=0)return 223;
	const bool stageBudgetProbe=stageBudgetProbeValue!=nullptr;
	const char* timestepVelocityAuditValue=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
	const bool timestepVelocityAuditMalformed=timestepVelocityAuditValue&&
		std::strcmp(timestepVelocityAuditValue,"1")!=0;
	const bool timestepVelocityAudit=timestepVelocityAuditValue&&
		std::strcmp(timestepVelocityAuditValue,"1")==0;
	const bool timestepVelocityAuditPresent=timestepVelocityAuditValue!=nullptr;
	const char* longShadowValue=std::getenv("RISE_FIRE_GOLDEN_LONG_SHADOW");
	if(longShadowValue&&std::strcmp(longShadowValue,"1")!=0)return 249;
	const bool longShadow=longShadowValue!=nullptr;
	const char* hostResidualProbeValue=std::getenv("RISE_FIRE_HOST_RESIDUAL_PROBE");
	if(hostResidualProbeValue&&std::strcmp(hostResidualProbeValue,"1")!=0)return 208;
	const bool hostResidualProbe=hostResidualProbeValue!=nullptr;
	const char* acceptedLongShadowValue=std::getenv(
		"RISE_FIRE_ACCEPTED_LONG_SHADOW");
	if(acceptedLongShadowValue&&std::strcmp(acceptedLongShadowValue,"1")!=0)return 205;
	const bool acceptedLongShadow=acceptedLongShadowValue!=nullptr;
	const char* monitoredLongShadowValue=std::getenv(
		"RISE_FIRE_MONITORED_MANIFOLD_SHADOW");
	if(monitoredLongShadowValue&&std::strcmp(monitoredLongShadowValue,"1")!=0)return 198;
	const bool monitoredLongShadow=monitoredLongShadowValue!=nullptr;
	const char* monitoredLongShadowSmokeValue=std::getenv(
		"RISE_FIRE_MONITORED_MANIFOLD_SHADOW_SMOKE");
	if(monitoredLongShadowSmokeValue&&
		std::strcmp(monitoredLongShadowSmokeValue,"1")!=0)return 195;
	const bool monitoredLongShadowSmoke=monitoredLongShadowSmokeValue!=nullptr;
	const char* tailThresholdREDValue=std::getenv(
		"RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED");
	if(tailThresholdREDValue&&std::strcmp(tailThresholdREDValue,"1")!=0)return 193;
	const bool tailThresholdRED=tailThresholdREDValue!=nullptr;
	const char* hardBoundRetryREDValue=std::getenv(
		"RISE_FIRE_MANIFOLD_HARD_BOUND_RETRY_RED");
	if(hardBoundRetryREDValue&&std::strcmp(hardBoundRetryREDValue,"1")!=0)return 192;
	const bool hardBoundRetryRED=hardBoundRetryREDValue!=nullptr;
	const char* goldenSubdominanceValue=std::getenv(
		"RISE_FIRE_GOLDEN_SUBDOMINANCE");
	if(goldenSubdominanceValue&&std::strcmp(goldenSubdominanceValue,"1")!=0)return 191;
	const bool goldenSubdominance=goldenSubdominanceValue!=nullptr;
	const char* equalTimeReadmissionValue=std::getenv(
		"RISE_FIRE_EQUAL_TIME_READMISSION");
	if(equalTimeReadmissionValue&&std::strcmp(equalTimeReadmissionValue,"1")!=0)return 187;
	const bool equalTimeReadmission=equalTimeReadmissionValue!=nullptr;
	const char* thermoSourceMapsValue=std::getenv("RISE_FIRE_THERMO_SOURCE_MAPS");
	if(thermoSourceMapsValue&&std::strcmp(thermoSourceMapsValue,"1")!=0)return 185;
	const bool thermoSourceMaps=thermoSourceMapsValue!=nullptr;
	std::array<std::string,8> goldenSubdominanceBeginningDigests;
	if(goldenSubdominance||equalTimeReadmission||thermoSourceMaps){
		const char* protocolPath=std::getenv("RISE_FIRE_GOLDEN_SUBDOMINANCE_PROTOCOL");
		if(!protocolPath)return 191;
		std::ifstream protocol(protocolPath,std::ios::binary);
		if(!protocol)return 191;
		std::string line;
		while(std::getline(protocol,line))for(std::size_t slice=0u;slice<8u;++slice){
			const std::string prefix="beginning_"+std::to_string(slice)+"_state_sha256 ";
			if(line.rfind(prefix,0u)==0u)
				goldenSubdominanceBeginningDigests[slice]=line.substr(prefix.size());
		}
		if(std::any_of(goldenSubdominanceBeginningDigests.begin(),
			goldenSubdominanceBeginningDigests.end(),[](const std::string& digest){
				return digest.size()!=64u;}))return 191;
		if(equalTimeReadmission||thermoSourceMaps){
			const char* temporalPath=std::getenv("RISE_FIRE_FILTERED_TEMPORAL_EVIDENCE");
			if(!temporalPath||DigestFile(temporalPath)!=
				"c85364852d2a48cc4d6b147dddb6c040254ef3931153a387b3950b33da93f5f4")
				return 187;
		}
	}
	const char* physicalRetryREDValue=std::getenv(
		"RISE_FIRE_PHYSICAL_PROJECTION_RETRY_RED");
	if(physicalRetryREDValue&&std::strcmp(physicalRetryREDValue,"1")!=0)return 202;
	const bool physicalRetryRED=physicalRetryREDValue!=nullptr;
	const char* contractionProbeValue=std::getenv(
		"RISE_FIRE_EQUAL_TIME_CONTRACTION_PROBE");
	if(contractionProbeValue&&std::strcmp(contractionProbeValue,"1")!=0)return 216;
	const bool contractionProbe=contractionProbeValue!=nullptr;
	const char* closureConvergenceValue=std::getenv(
		"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE");
	if(closureConvergenceValue&&std::strcmp(closureConvergenceValue,"1")!=0)return 200;
	const bool closureConvergence=closureConvergenceValue!=nullptr;
	const char* singleStageFCTSmokeValue=std::getenv(
		"RISE_FIRE_SINGLE_STAGE_FCT_SMOKE");
	if(singleStageFCTSmokeValue&&std::strcmp(singleStageFCTSmokeValue,"1")!=0)return 180;
	const bool singleStageFCTSmoke=singleStageFCTSmokeValue!=nullptr;
	if((plateauProbe&&manifoldProbe)||(plateauProbe&&stageBudgetProbe)||
		(manifoldProbe&&stageBudgetProbe)||(timestepVelocityAuditPresent&&plateauProbe)||
		(timestepVelocityAuditPresent&&manifoldProbe)||
		(timestepVelocityAuditPresent&&stageBudgetProbe)||
		(hostResidualProbe&&!longShadow)||(acceptedLongShadow&&(!longShadow||hostResidualProbe))||
		(monitoredLongShadow&&(!longShadow||hostResidualProbe||acceptedLongShadow))||
		(monitoredLongShadowSmoke&&!monitoredLongShadow)||
		(tailThresholdRED&&(!monitoredLongShadow||monitoredLongShadowSmoke))||
		(hardBoundRetryRED&&!tailThresholdRED)||
		((goldenSubdominance||equalTimeReadmission||thermoSourceMaps)&&(longShadow||plateauProbe||manifoldProbe||stageBudgetProbe||
			timestepVelocityAuditPresent||contractionProbe||closureConvergence))||
		((goldenSubdominance?1u:0u)+(equalTimeReadmission?1u:0u)+
			(thermoSourceMaps?1u:0u)>1u)||
		(physicalRetryRED&&(!longShadow||hostResidualProbe||acceptedLongShadow))||
		(longShadow&&(plateauProbe||manifoldProbe||stageBudgetProbe||
			timestepVelocityAuditPresent||contractionProbe))||
		(contractionProbe&&(plateauProbe||manifoldProbe||stageBudgetProbe||
			timestepVelocityAuditPresent))||
		(closureConvergence&&(plateauProbe||manifoldProbe||stageBudgetProbe||
			timestepVelocityAuditPresent||longShadow||contractionProbe))||
		(singleStageFCTSmoke&&(plateauProbe||manifoldProbe||stageBudgetProbe||
			timestepVelocityAuditPresent||longShadow||contractionProbe||closureConvergence||
			goldenSubdominance||equalTimeReadmission||thermoSourceMaps)))return 224;
	std::array<double,9> productionDistance={{}},oracleDistance={{}},scalarBound={{}},
		oracleScalarBound={{}},inventoryDistance={{}},oracleInventoryDistance={{}},
		inventoryBound={{}},oracleInventoryBound={{}};
	double velocityDistance=0.0,oracleVelocityDistance=0.0,velocityBound=0.0,
		oracleVelocityBound=0.0;
	std::array<double,9> maximumPrecisionScalar={{}},maximumPrecisionInventory={{}};
	double maximumPrecisionVelocity=0.0;
	double minimumScalarSubdominanceMargin=std::numeric_limits<double>::infinity();
	double minimumInventorySubdominanceMargin=std::numeric_limits<double>::infinity();
	double minimumVelocitySubdominanceMargin=std::numeric_limits<double>::infinity();
	std::size_t precisionGateCount=0u,physicalRetryCount=0u;
	for(std::size_t component=0u;component<9u;++component)
		if(!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
			FireProductionDyadicCalibration::ExpectedProductionScalarEvidence[component][0],
			FireProductionDyadicCalibration::ExpectedProductionScalarEvidence[component][1],
			FireProductionDyadicCalibration::VerifiedOrder,productionDistance[component],
			scalarBound[component])||
			!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
			FireProductionDyadicCalibration::ExpectedProductionInventoryEvidence[component][0],
			FireProductionDyadicCalibration::ExpectedProductionInventoryEvidence[component][1],
				FireProductionDyadicCalibration::VerifiedOrder,inventoryDistance[component],
				inventoryBound[component])||
			!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
				FireProductionDyadicCalibration::ExpectedOracleScalarEvidence[component][0],
				FireProductionDyadicCalibration::ExpectedOracleScalarEvidence[component][1],
				FireProductionDyadicCalibration::VerifiedOrder,oracleDistance[component],
				oracleScalarBound[component])||
			!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
				FireProductionDyadicCalibration::ExpectedOracleInventoryEvidence[component][0],
				FireProductionDyadicCalibration::ExpectedOracleInventoryEvidence[component][1],
				FireProductionDyadicCalibration::VerifiedOrder,oracleInventoryDistance[component],
				oracleInventoryBound[component]))return 130;
	if(!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
		FireProductionDyadicCalibration::ExpectedProductionVelocityEvidence[0],
		FireProductionDyadicCalibration::ExpectedProductionVelocityEvidence[1],
		FireProductionDyadicCalibration::VerifiedOrder,velocityDistance,velocityBound)||
		!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
		FireProductionDyadicCalibration::ExpectedOracleVelocityEvidence[0],
		FireProductionDyadicCalibration::ExpectedOracleVelocityEvidence[1],
		FireProductionDyadicCalibration::VerifiedOrder,oracleVelocityDistance,
		oracleVelocityBound))
		return 130;
	static const std::array<double,9> productionTemporal={{
		3.3888571164267637e-06,2.7108173821817564e-06,1.6159364810834396e-05,
		6.1893639007923478e-05,1.8037400675670603e-06,1.5220697786566516e-06,
		2.3666127133488299e-08,4.9725209139647414e-09,2.7328226439472538}};
	static const std::array<double,9> oracleTemporal={{
		6.239484288564373e-09,4.9915874284309687e-09,1.239123663934076e-08,
		3.8793498314296024e-08,3.3213560206185572e-09,2.8027137098095474e-09,
		4.3576503523885752e-11,9.155922778680957e-12,0.0012732090063250962}};
	static const std::array<double,9> productionInventoryTemporal={{
		4.439765595612102e-07,3.5545857596541104e-07,1.2805048754671455e-06,
		3.0645565915350953e-06,2.3653321227820405e-07,1.994172191316885e-07,
		3.1040925640894887e-09,6.5188314704960754e-10,0.36013833851793453}};
	static const std::array<double,9> oracleInventoryTemporal={{
		1.3465158969386569e-06,1.0772127175457343e-06,2.3958178689437078e-06,
		4.3874068464757684e-06,7.1676736013986942e-07,6.0484142474779291e-07,
		9.4040552195169564e-09,1.9758997701890746e-09,1.0513592731731316}};
	static constexpr double ProductionVelocityTemporal=1.5098888236479335e-05;
	static constexpr double OracleVelocityTemporal=4.1666621096787029e-05;
	std::size_t readmissionGateCount=0u,readmissionFailureCount=0u;
	double maximumScalarContractRatio=0.0,maximumInventoryContractRatio=0.0,
		velocityContractRatio=0.0;
	RISECBOR64::Bytes readmissionTrace;
	RISECBOR64::Bytes precisionTrace;
	static const std::size_t LongShadowSteps=FireProductionCalibration::LongShadowSteps;
	static const std::size_t LongShadowWindow=FireProductionCalibration::LongShadowWindow;
	std::vector<double> longShadowFieldMaximum;
	std::vector<double> longShadowFieldP95;
	std::vector<double> longShadowFieldP50;
	std::vector<double> longShadowPredictorGeneration;
	std::vector<double> longShadowAcceptedStepS;
	std::vector<unsigned int> longShadowAcceptedCandidate;
	std::vector<std::string> longShadowActiveLimit;
	std::vector<double> longShadowMaximumVelocity,longShadowMaximumReducedGravity,
		longShadowMaximumDiffusivity;
	std::vector<std::uint32_t> longShadowTailCellCount;
	std::vector<double> longShadowTailExcessSum,longShadowTailDrainedVolumeM3;
	std::vector<double> longShadowDeviceMS,longShadowWallMS;
	std::size_t longShadowAllowanceCrossings=0u,longShadowCeilingCrossings=0u;
	RISECBOR64::Bytes longShadowTrace;
	MethaneRunCheckpoint longShadowState;
	MethaneRunCheckpoint thermoSourceState;
	std::array<double,2> thermoSourceFieldMaximum={{}};
	std::array<double,2> thermoSourceFieldP95={{}};
	std::array<double,2> thermoSourceFieldP50={{}};
	std::array<std::uint32_t,2> thermoSourceTailCells={{}};
	std::array<double,2> thermoSourceTailExcess={{}};
	std::array<double,2> thermoSourceTailDrainM3={{}};
	std::array<double,2> thermoSourceDeviceMS={{}};
	std::array<double,2> thermoSourceWallMS={{}};
	double thermoSourceMinimumSubdominanceMargin=0.0;
	std::vector<ConservativeVector> longShadowReferenceConservative;
	PeriodicMACField longShadowReferenceMomentum;
	double longShadowFinalStepS=0.0;
	std::size_t longShadowPhysicalValidationCount=0u;
	std::size_t longShadowRestorationValidationCount=0u;
	std::uint32_t longShadowPhysicalOpenVCycleCount=physicalRetryRED?12u:19u;
	std::size_t longShadowPhysicalRetryCount=0u;
	std::uint32_t longShadowPhysicalRetryFinalCount=longShadowPhysicalOpenVCycleCount;
	const std::size_t requestedLongShadowSteps=(physicalRetryRED||monitoredLongShadowSmoke)?
		1u:LongShadowSteps;
	std::vector<unsigned int> longShadowManifoldRefusalCount(requestedLongShadowSteps,0u);
	std::vector<double> longShadowFirstRefusedField(requestedLongShadowSteps,
		std::numeric_limits<double>::quiet_NaN());
	for(std::size_t slice=0u;slice<((contractionProbe||closureConvergence)?1u:
		(thermoSourceMaps?2u:
		(longShadow?requestedLongShadowSteps:8u)));++slice){
		const std::filesystem::path beginningPath=slice==0u?checkpointPath:
			snapshotDirectory/(std::string("step_0")+std::to_string(slice)+".checkpoint");
		MethaneRunCheckpoint beginning;
		if(longShadow&&slice>0u)beginning=std::move(longShadowState);
		else if(thermoSourceMaps&&slice>0u)beginning=std::move(thermoSourceState);
		else if((longShadow?slice==0u&&DigestFile(beginningPath)!=checkpointDigest:
			(!(goldenSubdominance||equalTimeReadmission||thermoSourceMaps)&&
				DigestFile(beginningPath)!=beginningDigests[slice]))||
			!LoadMethaneRunCheckpoint(beginningPath,beginning,error)){
			std::fprintf(stderr,"production golden composition beginning %zu failed: %s\n",
				slice,error.c_str());return 113;}
		if(goldenSubdominance||equalTimeReadmission||(thermoSourceMaps&&slice==0u)){
			std::string stateDigest;
			if(!CheckpointProductionBeginningSHA256(beginning,stateDigest)||
				stateDigest!=goldenSubdominanceBeginningDigests[slice]){
				std::fprintf(stderr,
					"production golden composition beginning %zu state digest mismatch\n",slice);
				return 113;
			}
		}
		if(
			beginning.acceptedSteps!=3479u+slice){std::fprintf(stderr,
			"production golden composition beginning %zu accepted-step mismatch actual=%llu "
			"expected=%zu\n",slice,static_cast<unsigned long long>(beginning.acceptedSteps),
			3479u+slice);return 113;}
		unsigned int sliceRetryCandidate=0u;
		double sliceRetryStepS=0.0;
		const auto scheduledRetry=std::find_if(manifoldRetrySchedule.begin(),
			manifoldRetrySchedule.end(),[&](const auto& value){return std::get<0>(value)==slice;});
		if(scheduledRetry!=manifoldRetrySchedule.end()){
			sliceRetryCandidate=std::get<1>(*scheduledRetry);
			sliceRetryStepS=std::get<2>(*scheduledRetry);
		}else if(!acceptedLongShadow||slice==manifoldRetrySlice){
			sliceRetryCandidate=manifoldRetryCandidate;
			sliceRetryStepS=manifoldRetryStepS;
		}
		bool sliceAccepted=false;
		double sliceDeviceMS=0.0,sliceWallMS=0.0;
		while(!sliceAccepted){
		PeriodicMACShape shape;shape.nx=beginning.dimensions[0];shape.ny=beginning.dimensions[1];
		shape.nz=beginning.dimensions[2];shape.cellWidthM=beginning.cellWidthM;
		const char* closureMode=std::getenv("RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST");
		const bool limitedClosure=longShadow&&closureMode&&
			std::strcmp(closureMode,"limited")==0;
		const bool disabledClosure=longShadow&&closureMode&&
			std::strcmp(closureMode,"disabled")==0;
		if((acceptedLongShadow||physicalRetryRED)&&!limitedClosure)return 205;
		const unsigned int effectiveManifoldRetryCandidate=sliceRetryCandidate;
		const double effectiveManifoldRetryStepS=sliceRetryStepS;
		if(hostResidualProbe&&slice==0u)std::fprintf(stderr,
			"HOST_RESIDUAL_PROFILE candidate=%u step=%.17g\n",
			effectiveManifoldRetryCandidate,effectiveManifoldRetryStepS);
		double limitedStep=0.0;
		if(limitedClosure){
			if(effectiveManifoldRetryCandidate>0u){
				if(effectiveManifoldRetryCandidate>=RISE::FireStepRejectionRetryCap||
					!(effectiveManifoldRetryStepS>0.0)||
					!std::isfinite(effectiveManifoldRetryStepS))return 250;
				limitedStep=effectiveManifoldRetryStepS;
			}else if(acceptedLongShadow&&slice>0u){
				const RISE::FireProductionAcceptedManifoldObservation& observation=
					beginning.productionManifoldObservation;
				if(!observation.Available())return 250;
				if(observation.MaximumGeneration()>0.0){
					if(!RISE::DeriveFireProductionManifoldTimeStep(observation.TimeStepS(),
						observation.MaximumGeneration(),observation.RestorationDrainFraction(),
						limitedStep,&error))return 250;
				}else{
					// A moving distribution has accepted-state authority but no material
					// Eulerian-G authority.  Hold the last accepted operating point as the
					// next candidate; the ordinary realized-plateau refusal remains the
					// fail-closed detector and supplies any drain-aware reduction.
					limitedStep=beginning.previousStepS;
					if(!(limitedStep>0.0)||!std::isfinite(limitedStep))return 250;
				}
			}else{
				if(!RISE::DeriveFireProductionInitialManifoldTimeStep(
					0.00057953997747972608,0.024358630180358887,limitedStep,&error))return 250;
			}
		}else if(!monitoredLongShadow&&
			(effectiveManifoldRetryCandidate!=0u||effectiveManifoldRetryStepS!=0.0))
			return 250;
		else if(monitoredLongShadow&&effectiveManifoldRetryCandidate>0u&&
			(effectiveManifoldRetryCandidate>=RISE::FireStepRejectionRetryCap||
			 !(effectiveManifoldRetryStepS>0.0)||!std::isfinite(effectiveManifoldRetryStepS)))
			return 197;
		if(contractionProbe&&!RISE::DeriveFireProductionManifoldTimeStep(
			0.0016462659696117043,0.066569089889526367,
			0.99987278979872063,limitedStep,&error))return 250;
		const std::size_t cells=shape.CellCount();double dt=thermoSourceMaps?
			static_cast<double>(0.0016462659696117043f):(longShadow||contractionProbe)?
			((limitedClosure||contractionProbe)?static_cast<double>(static_cast<float>(limitedStep)):
				0.0016462659696117043):
			fromBits(stepBits[slice]);
		if(beginning.states.size()!=cells)return 114;
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell)conservative[cell]=ToConservativeVector(beginning.states[cell]);
		if((acceptedLongShadow||physicalRetryRED)&&slice==0u&&
			longShadowReferenceConservative.empty()){
			longShadowReferenceConservative=conservative;
			longShadowReferenceMomentum=beginning.momentum;
		}
		for(std::size_t component=0u;component<8u;++component)
			for(std::size_t cell=0u;cell<cells;++cell)minimumBeginningDensity=
				std::min(minimumBeginningDensity,conservative[cell][component]);
		ConservativeAdvance3DConfig shadowConfig;shadowConfig.transport.cellWidthM=shape.cellWidthM;
		// The physical Heun target is part of the same-scheme step DAG and must be
		// re-derived at the represented production duration used by this candidate.
		shadowConfig.transport.deltaTimeS=dt;
		shadowConfig.transport.ambientTemperatureK=300.0;
		shadowConfig.transport.adiabaticTemperatureK=2300.0;
		shadowConfig.transport.ambientGasDensityKGPerM3=ambient.GasDensity();
		// A long monitored target begins from accepted Binary32 production bytes.
		// Keep that inherited producer's r60 envelope while performing the target
		// generator arithmetic in binary64; silently promoting its admissibility
		// class rejects roundoff that production was already certified to carry.
		shadowConfig.transport.producerPrecision=(monitoredLongShadow||thermoSourceMaps)?
			RISE::FireStateProducerPrecision::Binary32:
			RISE::FireStateProducerPrecision::Binary64;
		shadowConfig.gravityMPerS2={{0.0,0.0,-9.80665}};shadowConfig.periodicBoundaries=false;
		shadowConfig.dns=false;shadowConfig.retainStageDiagnostics=true;
		// r168 monitored production owns a tangent-divergence target, not an oracle
		// accepted state.  Keep every r60/temperature/transport gate, but do not
		// import the oracle's absolute-P0 validity detector into this target seam.
		shadowConfig.injectedTemperatureK=300.0;
		shadowConfig.openBoundary.ambientDensityKGPerM3=ambient.GasDensity();
		shadowConfig.openBoundary.injectedGasDensityKGPerM3=injected.GasDensity();
		shadowConfig.openBoundary.ambientState=ToConservativeVector(ambient);
		shadowConfig.openBoundary.injectedState=ToConservativeVector(injected);
		shadowConfig.openBoundary.bottomFuelMask.clear();
		shadowConfig.openBoundary.bottomFuelMassFluxKGPerM2S.clear();
		const double referenceVelocity=std::sqrt(9.80665*beginning.values.characteristicDiameterM);
		const double referenceLength=shape.cellWidthM*std::max({shape.nx,shape.ny,shape.nz});
		shadowConfig.projectionTolerancePerS=1.0e-3*referenceVelocity/referenceLength;
		shadowConfig.openBoundary.velocityToleranceMPerS=
			shadowConfig.projectionTolerancePerS*referenceLength;
		shadowConfig.openBoundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*
			shadowConfig.openBoundary.velocityToleranceMPerS;
		std::string monitoredActiveLimit;
		double monitoredMaximumVelocity=0.0,monitoredMaximumReducedGravity=0.0,
			monitoredMaximumDiffusivity=0.0;
		if(monitoredLongShadow){
			for(unsigned int axis=0u;axis<3u;++axis)
				for(const double velocity:beginning.velocity.component[axis]){
					if(!std::isfinite(velocity))return 197;
					monitoredMaximumVelocity=std::max(monitoredMaximumVelocity,
						std::fabs(velocity));
				}
			std::vector<double> temperatureK(cells);
			RISE::FireStateProducerPrecision producerPrecision=
				beginning.states.empty()?RISE::FireStateProducerPrecision::Unknown:
				beginning.states.front().producerPrecision;
			for(std::size_t cell=0u;cell<cells;++cell){
				if(beginning.states[cell].producerPrecision!=producerPrecision)return 197;
				temperatureK[cell]=beginning.states[cell].temperatureK;
			}
			OpenMACField3D currentVelocity;
			currentVelocity.component=beginning.velocity.component;
			std::vector<CellTransportEvaluation> evaluations;
			if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperatureK,
				currentVelocity,shadowConfig.openBoundary,shadowConfig.dns,fuel,transport,
				producerPrecision,evaluations,&error,16u))return 197;
			for(std::size_t cell=0u;cell<cells;++cell){
				const double gas=beginning.states[cell].GasDensity();
				const CellTransportEvaluation& evaluation=evaluations[cell];
				if(!(gas>0.0)||!std::isfinite(gas))return 197;
				monitoredMaximumReducedGravity=std::max(monitoredMaximumReducedGravity,
					std::max(0.0,9.80665*(ambient.GasDensity()-gas)/gas));
				monitoredMaximumDiffusivity=std::max({monitoredMaximumDiffusivity,
					evaluation.totalDiffusivityM2PerS,
					evaluation.effectiveViscosityPaS/gas,
					evaluation.effectiveConductivityWPerMK/(gas*evaluation.gasCpJPerKGK)});
			}
			const double representedWidth=static_cast<double>(static_cast<float>(shape.cellWidthM));
			const double previous=slice?beginning.previousStepS:0.0;
			const double selected=RISE::FireCase::SelectTimeStepS(representedWidth,
				monitoredMaximumVelocity,monitoredMaximumReducedGravity,
				monitoredMaximumDiffusivity,previous);
			if(!(selected>0.0)||!std::isfinite(selected))return 197;
			const double advective=monitoredMaximumVelocity>0.0?
				0.5*representedWidth/monitoredMaximumVelocity:
				std::numeric_limits<double>::infinity();
			const double buoyant=monitoredMaximumReducedGravity>0.0?
				0.5*std::sqrt(2.0*representedWidth/monitoredMaximumReducedGravity):
				std::numeric_limits<double>::infinity();
			const double diffusive=monitoredMaximumDiffusivity>0.0?
				representedWidth*representedWidth/(8.0*monitoredMaximumDiffusivity):
				std::numeric_limits<double>::infinity();
			const double growth=previous>0.0?1.1*previous:
				std::numeric_limits<double>::infinity();
			monitoredActiveLimit=selected==advective?"advective_CFL":
				(selected==buoyant?"buoyant_acceleration":
				 (selected==diffusive?"explicit_diffusion":
				  (selected==growth?"growth_limit":"unknown")));
			if(monitoredActiveLimit=="unknown")return 197;
			if(effectiveManifoldRetryCandidate>0u){
				if(!(effectiveManifoldRetryStepS<=selected))return 197;
				dt=static_cast<double>(static_cast<float>(effectiveManifoldRetryStepS));
				monitoredActiveLimit="dynamics_bound_retry";
			}else dt=static_cast<double>(static_cast<float>(selected));
			shadowConfig.transport.deltaTimeS=dt;
		}
		std::vector<MethaneSourcePacket> zeroPackets(cells);
		std::vector<MethaneSourcePacket> sourcePackets32,sourcePackets64;
		std::vector<ConservativeVector> sourceDelta32(cells);
		std::array<double,9> sourceProducerDifference={{}};
		double sourceProducerMinimumMargin=std::numeric_limits<double>::infinity();
		std::size_t sourceActiveCellCount=0u;
		double sourceHeatReleaseW=0.0;
		std::string sourcePacketDigest;
		if(thermoSourceMaps){
			std::vector<MethaneReactionStep> reactionStep(cells);
			std::vector<double> cellVolumeM3(cells,std::pow(shape.cellWidthM,3.0));
			std::vector<MethaneCellState> beginning32=beginning.states;
			std::vector<MethaneCellState> beginning64=beginning.states;
			IgnitionGrid eligibilityGrid;
			eligibilityGrid.nx=shape.nx;eligibilityGrid.ny=shape.ny;
			eligibilityGrid.nz=shape.nz;eligibilityGrid.cells=beginning.states;
			eligibilityGrid.pilotMask.assign(cells,false);
			std::vector<bool> sourceEligibility;
			if(!BuildIgnitionEligibility(eligibilityGrid,fuel,fuel,transport,
				sourceEligibility,&error))return 183;
			for(std::size_t cell=0u;cell<cells;++cell){
				reactionStep[cell].deltaTimeS=dt;
				reactionStep[cell].mixingTimeS=0.05;
				reactionStep[cell].primaryEligible=sourceEligibility[cell];
				reactionStep[cell].sootOxidationEnabled=true;
				reactionStep[cell].maximumAcceptedTemperatureK=
					fuel.TemperatureMaxK();
				beginning32[cell].producerPrecision=FireStateProducerPrecision::Binary32;
				beginning64[cell].producerPrecision=FireStateProducerPrecision::Binary64;
			}
			RadiationEscapeFactor escape32,escape64;
			const double nominalHeatReleaseW=1000.0*CapstoneHeatReleaseRateKW;
			const double radiativeFraction=0.20;
			if(!(nominalHeatReleaseW>0.0)){
				std::fprintf(stderr,"THERMO_SOURCE_MAP nominal heat release invalid %.17g\n",
					nominalHeatReleaseW);return 183;
			}
			error.clear();
			const bool built32=BuildFrozenMethaneSourcePackets(beginning32,reactionStep,
				cellVolumeM3,300.0,nominalHeatReleaseW,radiativeFraction,false,fuel,fuel,
				opacity,sourcePackets32,escape32,&error,16u);
			if(!built32){std::fprintf(stderr,
				"THERMO_SOURCE_MAP binary32 packet construction failed: %s\n",error.c_str());
				return 183;}
			if(slice==0u){
				error.clear();
				const bool built64=BuildFrozenMethaneSourcePackets(beginning64,reactionStep,
					cellVolumeM3,300.0,nominalHeatReleaseW,radiativeFraction,false,fuel,fuel,
					opacity,sourcePackets64,escape64,&error,16u);
				if(!built64){std::fprintf(stderr,
					"THERMO_SOURCE_MAP binary64 packet construction failed: %s\n",error.c_str());
					return 183;}
			}
			RISECBOR64::Bytes sourceBytes;
			for(std::size_t cell=0u;cell<cells;++cell){
				if(!CertifiedBinary32SourcePacket(sourcePackets32[cell],fuel)){
					ConservativeVector diagnostic{};
					for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
						diagnostic[1u+species]=sourcePackets32[cell].constituentDelta[species];
					diagnostic[8]=sourcePackets32[cell].sensibleEnergyDeltaJPerM3;
					std::fprintf(stderr,"THERMO_SOURCE_MAP source certificate failed cell=%zu "
						"delta_certified=%d values=",cell,
						CertifiedBinary32SourceDelta(diagnostic,fuel)?1:0);
					for(std::size_t component=0u;component<9u;++component)
						std::fprintf(stderr," %.17g/%d",diagnostic[component],
							std::signbit(diagnostic[component])?1:0);
					const FireCertifiedNullspace& closure=fuel.ConservativeReconstruction();
					std::fprintf(stderr," factor=%.17g roundtrip=",
						fuel.AcceptedStateFeasibilityEnvelope().sourcePacketFactorEpsilon32*
						std::numeric_limits<float>::epsilon());
					for(std::size_t component=0u;component<9u;++component)
						std::fprintf(stderr,"%d",static_cast<double>(static_cast<float>(
							diagnostic[component]))==diagnostic[component]?1:0);
					std::fprintf(stderr," rows=");
					for(std::size_t row=0u;row<closure.constraintRows;++row){
						double residual=0.0;
						for(std::size_t column=0u;column<closure.stateDimension;++column)
							residual+=closure.constraintMatrix[row*closure.stateDimension+column]*
								diagnostic[column];
						std::fprintf(stderr," %.17g",residual);
					}
					std::fprintf(stderr," diagnostics=%.17g/%.17g/%.17g/%.17g/%.17g/%.17g/%.17g/%.17g\n",
						sourcePackets32[cell].reactedFuelKGPerM3,
						sourcePackets32[cell].oxidizedCarbonKGPerM3,
						sourcePackets32[cell].grossCarbonFormedKGPerM3,
						sourcePackets32[cell].gasHeatReleaseWPerM3,
						sourcePackets32[cell].sootHeatReleaseWPerM3,
						sourcePackets32[cell].pilotEnergyDeltaJPerM3,
						sourcePackets32[cell].pilotExpansionIntegral,
						sourcePackets32[cell].radiativeCoolingWPerM3);return 183;
				}
				sourceDelta32[cell][0]=0.0;
				for(std::size_t species=0u;species<MethaneSpeciesCount;++species){
					const std::size_t component=1u+species;
					sourceDelta32[cell][component]=sourcePackets32[cell].constituentDelta[species];
				if(slice==0u)sourceProducerDifference[component]=std::max(
					sourceProducerDifference[component],std::fabs(
						sourcePackets32[cell].constituentDelta[species]-
						sourcePackets64[cell].constituentDelta[species]));
				}
				sourceDelta32[cell][8]=sourcePackets32[cell].sensibleEnergyDeltaJPerM3;
				if(slice==0u)sourceProducerDifference[8]=std::max(
					sourceProducerDifference[8],std::fabs(
					sourcePackets32[cell].sensibleEnergyDeltaJPerM3-
						sourcePackets64[cell].sensibleEnergyDeltaJPerM3));
				if(sourcePackets32[cell].reactedFuelKGPerM3>0.0||
					sourcePackets32[cell].oxidizedCarbonKGPerM3>0.0)++sourceActiveCellCount;
				sourceHeatReleaseW+=(sourcePackets32[cell].gasHeatReleaseWPerM3+
					sourcePackets32[cell].sootHeatReleaseWPerM3)*cellVolumeM3[cell];
				for(std::size_t component=0u;component<9u;++component)
					FireProductionDyadicCalibration::AppendDouble(sourceBytes,
						sourceDelta32[cell][component]);
			}
			for(std::size_t component=0u;slice==0u&&component<9u;++component){
				const double bound=0x1p-3*productionDistance[component];
				if(sourceProducerDifference[component]>bound){
					std::fprintf(stderr,"THERMO_SOURCE_MAP subdominance failed component=%zu "
						"difference=%.17g bound=%.17g distance=%.17g\n",component,
						sourceProducerDifference[component],bound,productionDistance[component]);
					return 183;
				}
				if(sourceProducerDifference[component]>0.0)
					sourceProducerMinimumMargin=std::min(sourceProducerMinimumMargin,
						bound/sourceProducerDifference[component]);
			}
			if(slice==0u)thermoSourceMinimumSubdominanceMargin=sourceProducerMinimumMargin;
			else sourceProducerMinimumMargin=thermoSourceMinimumSubdominanceMargin;
			sourcePacketDigest=RISECBOR64::SHA256Hex(sourceBytes);
			std::fprintf(stderr,"THERMO_SOURCE_MAP_PREPARED active_cells=%zu "
				"source_digest=%s source_margin=%.17g\n",sourceActiveCellCount,
				sourcePacketDigest.c_str(),sourceProducerMinimumMargin);
		}
		ConservativeAdvance3DResult oracle,oracleSerial;
		if(contractionProbe){
			const double baseStep=dt;
			std::array<bool,4> converged={{false,false,false,false}};
			std::array<double,4> testedStep={{baseStep,baseStep*0.5,baseStep*0.25,
				baseStep*0.125}};
			std::array<std::string,4> classification;
			RISECBOR64::Bytes contractionTrace;
			std::size_t largestConverged=testedStep.size();
			bool monotoneDomain=true;
			for(std::size_t level=0u;level<testedStep.size();++level){
				ConservativeAdvance3DConfig levelConfig=shadowConfig;
				levelConfig.transport.deltaTimeS=testedStep[level];
				levelConfig.workerCount=16u;
				std::vector<OpenPicardContractionDiagnostic> diagnostics;
				levelConfig.openPicardDiagnostics=&diagnostics;
				ConservativeAdvance3DResult levelResult;
				error.clear();
				converged[level]=AdvanceConservative3D(shape,conservative,beginning.momentum,
					zeroPackets,levelConfig,fuel,fuel,transport,levelResult,&error);
				bool cycle=false;
				for(const OpenPicardContractionDiagnostic& diagnostic:diagnostics)
					cycle=cycle||diagnostic.activeSetCycleLength>0u;
				classification[level]=cycle?"discrete_cycle":
					(converged[level]?"converged":"smooth_noncontraction");
				if(converged[level]&&largestConverged==testedStep.size())largestConverged=level;
				if(level&&converged[level-1u]&&!converged[level])monotoneDomain=false;
				FireProductionDyadicCalibration::AppendDouble(contractionTrace,testedStep[level]);
				FireProductionDyadicCalibration::AppendInteger(contractionTrace,
					converged[level]?1u:0u);
				FireProductionDyadicCalibration::AppendInteger(contractionTrace,
					diagnostics.size());
				std::fprintf(stderr,"EQUAL_TIME_CONTRACTION level=%zu dt=%.17g converged=%d "
					"class=%s stages=%zu",level,testedStep[level],converged[level]?1:0,
					classification[level].c_str(),diagnostics.size());
				for(std::size_t stage=0u;stage<diagnostics.size();++stage){
					const OpenPicardContractionDiagnostic& diagnostic=diagnostics[stage];
					std::fprintf(stderr," stage%zu=[",stage);
					FireProductionDyadicCalibration::AppendInteger(contractionTrace,
						diagnostic.converged?1u:0u);
					FireProductionDyadicCalibration::AppendInteger(contractionTrace,
						diagnostic.activeSetCycleLength);
					for(std::size_t iteration=0u;iteration<diagnostic.residualPerS.size();
						++iteration){
						if(iteration)std::fprintf(stderr,",");
						std::fprintf(stderr,"%.17g",diagnostic.residualPerS[iteration]);
						FireProductionDyadicCalibration::AppendDouble(contractionTrace,
							diagnostic.residualPerS[iteration]);
					}
					std::fprintf(stderr,"] target=%.17g mass=%.17g coefficient=%.17g "
						"active_changed=%d cycle=%zu differing_faces=%zu",
						diagnostic.targetResidualPerS,diagnostic.massResidualPerS,
						diagnostic.coefficientResidual,diagnostic.activeSetChanged?1:0,
						diagnostic.activeSetCycleLength,
						diagnostic.activeSetDifferingFaceCount);
				}
				std::fprintf(stderr," error=%s\n",converged[level]?"none":error.c_str());
			}
			const std::string traceDigest=RISECBOR64::SHA256Hex(contractionTrace);
			std::fprintf(stderr,"EQUAL_TIME_CONTRACTION_SUMMARY base_dt=%.17g "
				"largest_converged_dt=%.17g largest_converged_level=%zu monotone=%d "
				"trace=%s golden=%s\n",baseStep,
				largestConverged<testedStep.size()?testedStep[largestConverged]:0.0,
				largestConverged,monotoneDomain?1:0,traceDigest.c_str(),
				DigestFile(checkpointPath).c_str());
			const bool exactDiagnostic=converged==std::array<bool,4>{{false,false,false,true}}&&
				classification==std::array<std::string,4>{{"discrete_cycle","discrete_cycle",
					"discrete_cycle","discrete_cycle"}}&&largestConverged==3u&&
				testedStep[3]==7.244249718496576e-05&&monotoneDomain&&
				traceDigest=="900a7acc56a0c9c51d07788753132d59269bdb591aee699960a3c62b448a051c";
			return exactDiagnostic&&
				DigestFile(checkpointPath)==checkpointDigest?217:215;
		}
		auto publishedTargetDigest=[](const std::vector<float>& target){
			RISECBOR64::Bytes bytes;
			FireProductionDyadicCalibration::AppendInteger(bytes,target.size());
			for(const float value:target)
				FireProductionDyadicCalibration::AppendDouble(bytes,static_cast<double>(value));
			return RISECBOR64::SHA256Hex(bytes);
		};
		std::string equalTimeReferenceScheduleDigest,equalTimeReferenceSerialDigest;
		std::string equalTimeTerminalTargetDigest,equalTimeSerialTerminalTargetDigest;
		std::vector<float> equalTimeTerminalTarget,equalTimeSerialTerminalTarget;
		double equalTimeTerminalTargetTime=0.0,equalTimeSerialTerminalTargetTime=0.0;
		std::size_t equalTimeReferenceSubstepCount=0u;
		if(monitoredLongShadow&&slice==0u){
			std::vector<double> ignoredTemperature;
			std::vector<ConservativeVector> pressureMutation=conservative;
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				pressureMutation.front()[component]*=1.00102354;
			std::string strictPressureError;
			const bool strictPressureAccepted=InvertPeriodicTemperaturesWithinBounds(
				pressureMutation,fuel,shadowConfig.transport.ambientTemperatureK,
				shadowConfig.transport.adiabaticTemperatureK,
				shadowConfig.transport.producerPrecision,ignoredTemperature,
				&strictPressureError,16u,true);
			std::string monitoredPressureError;
			const bool monitoredPressureAccepted=InvertPeriodicTemperaturesWithinBounds(
				pressureMutation,fuel,shadowConfig.transport.ambientTemperatureK,
				shadowConfig.transport.adiabaticTemperatureK,
				shadowConfig.transport.producerPrecision,ignoredTemperature,
				&monitoredPressureError,16u,false);
			std::vector<ConservativeVector> affineMutation=conservative;
			affineMutation.front()[0]+=0.01;
			std::string affineError;
			const bool affineMutationAccepted=InvertPeriodicTemperaturesWithinBounds(
				affineMutation,fuel,shadowConfig.transport.ambientTemperatureK,
				shadowConfig.transport.adiabaticTemperatureK,
				shadowConfig.transport.producerPrecision,ignoredTemperature,
				&affineError,16u,false);
			if(strictPressureAccepted||!monitoredPressureAccepted||strictPressureError.find(
				"accepted-state EOS gate")==std::string::npos||affineMutationAccepted||
				affineError.find("affine rows")==std::string::npos){
				std::fprintf(stderr,"MONITORED_TARGET_POLICY_RED_FAILED strict_accepted=%d "
					"strict_error=%s monitored_accepted=%d monitored_error=%s "
					"affine_accepted=%d affine_error=%s\n",
					strictPressureAccepted?1:0,strictPressureError.c_str(),
					monitoredPressureAccepted?1:0,monitoredPressureError.c_str(),
					affineMutationAccepted?1:0,affineError.c_str());return 197;
			}
			std::fprintf(stderr,"MONITORED_TARGET_POLICY absolute_reference_pressure_gate=0 "
				"producer_precision=%u strict_pressure_detector_refused=1 affine_RED_refused=1\n",
				static_cast<unsigned int>(shadowConfig.transport.producerPrecision));
		}
		auto advanceReference=[&](unsigned int workerCount,
			ConservativeAdvance3DResult& composed,std::string& scheduleDigest,
			std::vector<float>& terminalTarget,std::string& terminalTargetDigest,
			double& terminalTargetTime)->bool{
			if(monitoredLongShadow||thermoSourceMaps){
				OpenMACField3D currentVelocity;
				currentVelocity.component=beginning.velocity.component;
				std::vector<ConservativeVector> emptySourceDelta;
				if(!thermoSourceMaps)emptySourceDelta.resize(cells);
				const std::vector<ConservativeVector>& sourceDelta=thermoSourceMaps?
					sourceDelta32:emptySourceDelta;
				ConservativeAdvance3DConfig tangentConfig=shadowConfig;
				tangentConfig.workerCount=workerCount;
				std::vector<double> tangentTarget;
				if(!MonitoredProductionTangentDivergenceTarget3D(shape,conservative,
					currentVelocity,sourceDelta,tangentConfig,fuel,fuel,transport,
					tangentTarget,&error))return false;
				terminalTarget.resize(tangentTarget.size());
				for(std::size_t cell=0u;cell<tangentTarget.size();++cell)
					terminalTarget[cell]=static_cast<float>(tangentTarget[cell]);
				terminalTargetDigest=publishedTargetDigest(terminalTarget);
				terminalTargetTime=dt;
				RISECBOR64::Bytes scheduleTrace;
				FireProductionDyadicCalibration::AppendInteger(scheduleTrace,
					thermoSourceMaps?0x72313735u:0x72313638u);
				FireProductionDyadicCalibration::AppendDouble(scheduleTrace,dt);
				FireProductionDyadicCalibration::AppendInteger(scheduleTrace,
					static_cast<unsigned int>(shadowConfig.transport.producerPrecision));
				for(const double target:tangentTarget)
					FireProductionDyadicCalibration::AppendDouble(scheduleTrace,target);
				scheduleDigest=RISECBOR64::SHA256Hex(scheduleTrace);
				equalTimeReferenceSubstepCount=0u;
				composed=ConservativeAdvance3DResult();
				return true;
			}
			if(!limitedClosure&&!monitoredLongShadow&&!equalTimeReadmission){
				ConservativeAdvance3DConfig directConfig=shadowConfig;
				directConfig.workerCount=workerCount;
				const bool advanced=AdvanceConservative3D(shape,conservative,
					beginning.momentum,zeroPackets,
					directConfig,fuel,fuel,transport,composed,&error);
				if(advanced){
					terminalTarget.resize(composed.divergenceHeunPerS.size());
					for(std::size_t cell=0u;cell<terminalTarget.size();++cell)
						terminalTarget[cell]=static_cast<float>(composed.divergenceHeunPerS[cell]);
					terminalTargetDigest=publishedTargetDigest(terminalTarget);
					terminalTargetTime=dt;
				}
				return advanced;
			}
			std::size_t referenceSubstepCount=8u;
			for(;;){
				const double referenceStep=dt/static_cast<double>(referenceSubstepCount);
				std::vector<double> schedule(referenceSubstepCount,referenceStep);
				double representedPrefix=0.0;
				for(std::size_t substep=1u;substep<referenceSubstepCount;++substep)
					representedPrefix+=referenceStep;
				schedule.back()=dt-representedPrefix;
				if(!FireProductionCalibration::EqualTimeReferenceSchedule(dt,schedule,dt,dt,
					"precomposition","precomposition")){
					double diagnosticSum=0.0;
					for(const double step:schedule)diagnosticSum+=step;
					std::fprintf(stderr,"equal-time schedule precomposition rejected dt=%.17g "
						"substeps=%zu first=%.17g last=%.17g sum=%.17g\n",dt,
						schedule.size(),schedule.front(),schedule.back(),diagnosticSum);
					error="equal-time reference schedule does not reach the production endpoint";
					return false;
				}
				// The enforced equal-time comparison owns a separately accumulated
				// reference trajectory.  A monitored production shadow does not: its
				// tangent S_div target is regenerated from each accepted production
				// beginning so an oracle-state drift cannot become a stale target.
				std::vector<ConservativeVector> referenceState=acceptedLongShadow?
					longShadowReferenceConservative:conservative;
				PeriodicMACField referenceMomentum=acceptedLongShadow?
					longShadowReferenceMomentum:beginning.momentum;
				RISECBOR64::Bytes scheduleTrace;
				double referenceTime=0.0;
				std::vector<float> penultimateTarget;
				bool converged=true;
				for(std::size_t substep=0u;substep<referenceSubstepCount;++substep){
				ConservativeAdvance3DConfig referenceConfig=shadowConfig;
				const double representedReferenceStep=schedule[substep];
				referenceConfig.transport.deltaTimeS=representedReferenceStep;
				referenceConfig.workerCount=workerCount;
				ConservativeAdvance3DResult advanced;
				if(!AdvanceConservative3D(shape,referenceState,referenceMomentum,zeroPackets,
					referenceConfig,fuel,fuel,transport,advanced,&error)){
					error=std::string("equal-time reference substep ")+
						std::to_string(substep)+": "+error;
					converged=false;break;
				}
				referenceTime+=representedReferenceStep;
				FireProductionDyadicCalibration::AppendDouble(scheduleTrace,
					representedReferenceStep);
				FireProductionDyadicCalibration::AppendDouble(scheduleTrace,referenceTime);
				FireProductionDyadicCalibration::AppendDouble(scheduleTrace,
					advanced.maximumDivergenceResidualPerS);
				for(const double target:advanced.divergenceHeunPerS)
					FireProductionDyadicCalibration::AppendDouble(scheduleTrace,target);
				std::vector<float> publishedTarget(advanced.divergenceHeunPerS.size());
				for(std::size_t cell=0u;cell<publishedTarget.size();++cell)
					publishedTarget[cell]=static_cast<float>(advanced.divergenceHeunPerS[cell]);
				if(substep+2u==referenceSubstepCount)penultimateTarget=publishedTarget;
				if(substep+1u==referenceSubstepCount){
					terminalTarget=std::move(publishedTarget);
					terminalTargetDigest=publishedTargetDigest(terminalTarget);
					terminalTargetTime=referenceTime;
				}
				referenceState=advanced.conservative;
				referenceMomentum=advanced.momentumKGPerM2S;
				composed=std::move(advanced);
				}
				if(!converged){
					if(!(acceptedLongShadow||monitoredLongShadow||equalTimeReadmission)||
						referenceSubstepCount>=64u)return false;
					std::fprintf(stderr,"EQUAL_TIME_REFERENCE_RETRY slice=%zu failed_substeps=%zu "
						"next_substeps=%zu error=%s\n",slice,referenceSubstepCount,
						referenceSubstepCount*2u,error.c_str());
					referenceSubstepCount*=2u;error.clear();terminalTarget.clear();
					terminalTargetDigest.clear();terminalTargetTime=0.0;continue;
				}
				if(referenceTime!=dt||!FireProductionCalibration::EqualTimeReferenceSchedule(
					dt,schedule,referenceTime,terminalTargetTime,terminalTargetDigest,
					terminalTargetDigest)){
					error="equal-time reference endpoint drifted from production";
					return false;
				}
				if(penultimateTarget.empty()||FireProductionCalibration::EqualTimeReferenceSchedule(
					dt,schedule,referenceTime,terminalTargetTime,terminalTargetDigest,
					publishedTargetDigest(penultimateTarget))){
					error="equal-time reference accepted its penultimate target at the endpoint";
					return false;
				}
				equalTimeReferenceSubstepCount=referenceSubstepCount;
				scheduleDigest=RISECBOR64::SHA256Hex(scheduleTrace);
				return true;
			}
		};
		error.clear();
		if(!advanceReference(16u,oracle,equalTimeReferenceScheduleDigest,
			equalTimeTerminalTarget,equalTimeTerminalTargetDigest,
			equalTimeTerminalTargetTime)){
			std::fprintf(stderr,"production golden parallel reference %zu failed: %s\n",
				slice,error.c_str());return 115;
		}
		if(monitoredLongShadow&&monitoredLongShadowSmoke){
			error.clear();
			if(!advanceReference(1u,oracleSerial,equalTimeReferenceSerialDigest,
				equalTimeSerialTerminalTarget,equalTimeSerialTerminalTargetDigest,
				equalTimeSerialTerminalTargetTime)||
				equalTimeReferenceScheduleDigest!=equalTimeReferenceSerialDigest||
				equalTimeTerminalTargetDigest!=equalTimeSerialTerminalTargetDigest||
				equalTimeTerminalTarget!=equalTimeSerialTerminalTarget||
				equalTimeTerminalTargetTime!=equalTimeSerialTerminalTargetTime){
				std::fprintf(stderr,"monitored tangent target worker mismatch: %s\n",
					error.c_str());return 116;
			}
		}else if(equalTimeReadmission){
			// r112/r139 already own oracle worker-identity.  Readmission consumes the
			// authoritative parallel schedule once per slice; a second one-worker
			// trajectory is not an additive-contract term.
			oracleSerial=oracle;
			equalTimeReferenceSerialDigest=equalTimeReferenceScheduleDigest;
			equalTimeSerialTerminalTarget=equalTimeTerminalTarget;
			equalTimeSerialTerminalTargetDigest=equalTimeTerminalTargetDigest;
			equalTimeSerialTerminalTargetTime=equalTimeTerminalTargetTime;
		}else if(!(acceptedLongShadow||monitoredLongShadow)){
			error.clear();
			if(!advanceReference(1u,oracleSerial,equalTimeReferenceSerialDigest,
				equalTimeSerialTerminalTarget,equalTimeSerialTerminalTargetDigest,
				equalTimeSerialTerminalTargetTime)){std::fprintf(stderr,
				"production golden serial reference %zu failed: %s\n",slice,error.c_str());return 116;}
			if(limitedClosure&&(equalTimeReferenceScheduleDigest!=equalTimeReferenceSerialDigest||
				equalTimeTerminalTargetDigest!=equalTimeSerialTerminalTargetDigest||
				equalTimeTerminalTarget!=equalTimeSerialTerminalTarget||
				equalTimeTerminalTargetTime!=equalTimeSerialTerminalTargetTime))
				return 128;
			if(oracle.maximumPreProjectionDivergenceResidualPerS!=
				oracleSerial.maximumPreProjectionDivergenceResidualPerS)return 128;
		}
		RISE::FireProductionResidentStepRequest request;
		if(acceptedLongShadow||monitoredLongShadow||physicalRetryRED)
			request.physicalOpenProjectionVCycleCount=
			longShadowPhysicalOpenVCycleCount;
		request.force.shape.nx=shape.nx;request.force.shape.ny=shape.ny;
		request.force.shape.nz=shape.nz;request.force.shape.cellWidthM=static_cast<float>(shape.cellWidthM);
		request.force.timeStepS=static_cast<float>(dt);
		request.force.ambientDensityKGPerM3=static_cast<float>(ambient.GasDensity());
		request.force.vremanCoefficient=0.07f;request.force.gravityMPerS2={{0.0f,0.0f,-9.80665f}};
		request.force.boundary={RISE::FireProductionProjectionPressureOpen,
			RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionPressureOpen,
			RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionWall,
			RISE::FireProductionProjectionPressureOpen};
		request.force.cellGasDensityKGPerM3.resize(cells);
		request.force.molecularKinematicViscosityM2PerS.resize(cells);
		request.cellTransport.shape=request.force.shape;request.cellTransport.componentCount=9u;
		request.cellTransport.timeStepS=request.force.timeStepS;
		request.cellTransport.boundary=request.force.boundary;
		request.cellTransport.conservativeValues.resize(9u*cells);
		request.cellTransport.ambientValues.resize(9u);
		const ConservativeVector ambientVector=ToConservativeVector(ambient);
		for(std::size_t component=0u;component<9u;++component)
			request.cellTransport.ambientValues[component]=static_cast<float>(ambientVector[component]);
		for(std::size_t cell=0u;cell<cells;++cell){
			const double gas=beginning.states[cell].GasDensity();
			CellMolecularTransportEvaluation molecular;
			if(!EvaluateCellMolecularTransport(beginning.states[cell],fuel,transport,molecular,&error))return 117;
			request.force.molecularKinematicViscosityM2PerS[cell]=
				static_cast<float>(molecular.molecularViscosityPaS/gas);
			for(std::size_t component=0u;component<9u;++component)
				request.cellTransport.conservativeValues[component*cells+cell]=
					static_cast<float>(conservative[cell][component]);
			float packedGas=request.cellTransport.conservativeValues[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				packedGas+=request.cellTransport.conservativeValues[component*cells+cell];
			request.force.cellGasDensityKGPerM3[cell]=packedGas;
		}
		request.dualTransport.shape=request.force.shape;
		request.dualTransport.timeStepS=request.force.timeStepS;
		request.dualTransport.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
		request.dualTransport.boundary=request.force.boundary;
		auto cellIndex=[&](std::size_t x,std::size_t y,std::size_t z){return (z*shape.ny+y)*shape.nx+x;};
		auto faceIndex=[&](unsigned int axis,std::size_t x,std::size_t y,std::size_t z){
			if(axis==0u)return (z*shape.ny+y)*(shape.nx+1u)+x;
			if(axis==1u)return (z*(shape.ny+1u)+y)*shape.nx+x;
			return (z*shape.ny+y)*shape.nx+x;};
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t faces=RISE::FireProductionProjectionFaceCount(request.force.shape,axis);
			request.force.faceDensityKGPerM3[axis].resize(faces);
			request.force.beginningMomentumKGPerM2S[axis].resize(faces);
			request.cellTransport.frozenVelocityMPerS[axis].resize(faces);
			const std::size_t xFaces=axis==0u?shape.nx+1u:shape.nx;
			const std::size_t yFaces=axis==1u?shape.ny+1u:shape.ny;
			const std::size_t zFaces=axis==2u?shape.nz+1u:shape.nz;
			if(beginning.momentum.component[axis].size()!=faces||beginning.velocity.component[axis].size()!=faces)
				return 118;
			for(std::size_t z=0u;z<zFaces;++z)for(std::size_t y=0u;y<yFaces;++y)
				for(std::size_t x=0u;x<xFaces;++x){
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
					float density=0.0f;
					if(coordinate>0u&&coordinate<extent){
						if(axis==0u)--lowX;if(axis==1u)--lowY;if(axis==2u)--lowZ;
						density=0.5f*request.force.cellGasDensityKGPerM3[cellIndex(lowX,lowY,lowZ)]+
							0.5f*request.force.cellGasDensityKGPerM3[cellIndex(highX,highY,highZ)];
					}else{
						if(axis==0u)highX=coordinate?shape.nx-1u:0u;
						if(axis==1u)highY=coordinate?shape.ny-1u:0u;
						if(axis==2u)highZ=coordinate?shape.nz-1u:0u;
						const float interior=request.force.cellGasDensityKGPerM3[cellIndex(highX,highY,highZ)];
						const unsigned int side=2u*axis+(coordinate?1u:0u);
						density=request.force.boundary[side]==RISE::FireProductionProjectionPressureOpen?
							0.5f*interior+0.5f*request.force.ambientDensityKGPerM3:interior;
					}
					const std::size_t face=faceIndex(axis,x,y,z);
					request.force.faceDensityKGPerM3[axis][face]=density;
					request.force.beginningMomentumKGPerM2S[axis][face]=
						static_cast<float>(beginning.momentum.component[axis][face]);
					float velocity=static_cast<float>(beginning.velocity.component[axis][face]);
					const unsigned int side=2u*axis+(coordinate?1u:0u);
					if((coordinate==0u||coordinate==extent)&&
						request.force.boundary[side]==RISE::FireProductionProjectionWall)velocity=0.0f;
					request.cellTransport.frozenVelocityMPerS[axis][face]=velocity;
				}
			request.dualTransport.beginningFaceDensity[axis]=request.force.faceDensityKGPerM3[axis];
			request.dualTransport.beginningMomentum[axis]=request.force.beginningMomentumKGPerM2S[axis];
			request.dualTransport.frozenVelocityMPerS[axis]=request.cellTransport.frozenVelocityMPerS[axis];
			request.momentumSourceIncrement[axis].assign(faces,0.0f);
		}
		request.cellSourceIncrement.assign(9u*cells,0.0f);
		if(thermoSourceMaps)for(std::size_t cell=0u;cell<cells;++cell)
			for(std::size_t component=0u;component<9u;++component)
				request.cellSourceIncrement[component*cells+cell]=
					static_cast<float>(sourceDelta32[cell][component]);
		request.divergenceTargetPerS.resize(cells);
		request.restorationDivergenceTargetPerS.resize(cells);
		request.beginningManifoldDeviationPerCell.resize(cells);
		if((limitedClosure||monitoredLongShadow||equalTimeReadmission||thermoSourceMaps)&&
			equalTimeTerminalTarget.size()!=cells)return 129;
		for(std::size_t cell=0u;cell<cells;++cell){
			request.divergenceTargetPerS[cell]=(limitedClosure||monitoredLongShadow||
				equalTimeReadmission||thermoSourceMaps)?
				equalTimeTerminalTarget[cell]:
				static_cast<float>(oracle.divergenceHeunPerS[cell]);
			double volumeRatio=0.0;
			if(!AcceptedConservativeVolumeRatio(ToConservativeVector(beginning.states[cell]),fuel,
				beginning.states[cell].producerPrecision,volumeRatio,&error))return 117;
			request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
				(volumeRatio-1.0)/static_cast<double>(request.force.timeStepS));
			request.beginningManifoldDeviationPerCell[cell]=volumeRatio-1.0;
		}
		if(limitedClosure){
			const std::vector<double> referenceSchedule(equalTimeReferenceSubstepCount,
				dt/static_cast<double>(equalTimeReferenceSubstepCount));
			if(!FireProductionCalibration::EqualTimeReferenceSchedule(dt,referenceSchedule,dt,
				equalTimeTerminalTargetTime,equalTimeTerminalTargetDigest,
				publishedTargetDigest(request.divergenceTargetPerS)))return 129;
		}else if(monitoredLongShadow){
			if(equalTimeReferenceSubstepCount!=0u||equalTimeTerminalTargetTime!=dt||
				equalTimeTerminalTargetDigest!=publishedTargetDigest(request.divergenceTargetPerS)||
				equalTimeReferenceScheduleDigest.empty())return 129;
		}else if(equalTimeReadmission){
			if(equalTimeTerminalTargetTime!=dt||equalTimeTerminalTargetDigest!=
				publishedTargetDigest(request.divergenceTargetPerS)||
				equalTimeReferenceScheduleDigest.empty())return 129;
		}else if(thermoSourceMaps){
			if(equalTimeReferenceSubstepCount!=0u||equalTimeTerminalTargetTime!=dt||
				equalTimeTerminalTargetDigest!=publishedTargetDigest(request.divergenceTargetPerS)||
				equalTimeReferenceScheduleDigest.empty())return 129;
		}
		request.monitorManifoldDiagnostics=true;
		request.enforceManifoldPlateau=longShadow&&!monitoredLongShadow;
		if(closureConvergence){
			if(slice!=0u)return 200;
			constexpr float representedCFLStep=0.0016462659696117043f;
			constexpr double physicalDynamicPressureScale=0.00065;
			constexpr double subdominanceMargin=0x1p-3;
			constexpr double convergenceTolerance=
				physicalDynamicPressureScale*subdominanceMargin;
			request.force.timeStepS=representedCFLStep;
			request.cellTransport.timeStepS=representedCFLStep;
			request.dualTransport.timeStepS=representedCFLStep;
			request.enforceManifoldPlateau=true;
			ConservativeAdvance3DConfig cflConfig=shadowConfig;
			cflConfig.transport.deltaTimeS=representedCFLStep;cflConfig.workerCount=16u;
			ConservativeAdvance3DResult cflOracle;
			if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
				cflConfig,fuel,fuel,transport,cflOracle,&error)){
				std::fprintf(stderr,"ANOMALY_CLOSURE_CONVERGENCE target failed: %s\n",
					error.c_str());return 200;
			}
			for(std::size_t cell=0u;cell<cells;++cell){
				request.divergenceTargetPerS[cell]=static_cast<float>(
					cflOracle.divergenceHeunPerS[cell]);
				request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
					request.beginningManifoldDeviationPerCell[cell]/
					static_cast<double>(representedCFLStep));
			}
			std::array<double,8> generation32={{}},generation64={{}},field32={{}},field64={{}},
				deviceP95={{}},wallP95={{}};
			for(std::uint32_t pass=1u;pass<=8u;++pass){
				const std::string passText=std::to_string(pass);
				if(!setFixtureEnvironment("RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES",
					passText.c_str()))return 200;
				std::array<double,5> device={{}},wall={{}};
				std::uint64_t baselinePayload=0u;bool baselineAvailable=false;
				RISE::FireProductionResidentStepResult measured;
				for(std::size_t trial=0u;trial<=wall.size();++trial){
					const auto start=std::chrono::steady_clock::now();
					RISE::FireProductionResidentStepResult current;
					const bool succeeded=RISE::AttemptFireProductionResidentStepMetal(
						request,current,&error);
					const auto end=std::chrono::steady_clock::now();
					if(!succeeded&&!(current.manifoldNextTimeStepAvailable&&
						!current.manifoldPlateauPassed&&!current.HasAcceptedManifoldToken())){
						std::fprintf(stderr,"ANOMALY_CLOSURE_CONVERGENCE fp32 pass=%u failed: %s\n",
							pass,error.c_str());return 200;
					}
					const std::uint64_t payload=
						RISE::FireProductionAcceptedManifoldPayloadDigest(current);
					if(!baselineAvailable){baselinePayload=payload;measured=current;
						baselineAvailable=true;}
					else if(payload!=baselinePayload||
						current.maximumManifoldGeneration!=measured.maximumManifoldGeneration||
						current.maximumAcceptedManifoldDeviation!=
							measured.maximumAcceptedManifoldDeviation||
						current.advectiveAnomalyClosurePassCount!=pass||
						current.cellSubmapCount!=5u*pass||
						current.sourceCommandCommitCount!=pass||
						current.manifoldScalarDeviceToHostTransferCount!=pass||
						current.residentProjectionInvocationCount!=std::max(2u,pass)||
						current.interstageFullGridTransferCount!=0u||
						current.HasAcceptedManifoldToken())return 200;
					if(trial>0u){
						wall[trial-1u]=std::chrono::duration<double,std::milli>(end-start).count();
						device[trial-1u]=current.deviceMakespanMS;
					}
				}
				std::sort(device.begin(),device.end());std::sort(wall.begin(),wall.end());
				generation32[pass-1u]=measured.maximumManifoldGeneration;
				field32[pass-1u]=measured.maximumAcceptedManifoldDeviation;
				deviceP95[pass-1u]=device.back();wallP95[pass-1u]=wall.back();
				FireProductionCalibration::ClosureConvergence64Result mirrored;
				if(!FireProductionCalibration::AdvanceResidentStep64Closure(request,
					measured.forceDiagnostics.outwardLambdaPerS,fuel,pass,mirrored,&error)||
					mirrored.executedPassCount!=pass){
					std::fprintf(stderr,"ANOMALY_CLOSURE_CONVERGENCE fp64 pass=%u failed: %s\n",
						pass,error.c_str());return 200;
				}
				generation64[pass-1u]=mirrored.maximumGeneration;
				field64[pass-1u]=mirrored.maximumDeviation;
				std::fprintf(stderr,"ANOMALY_CLOSURE_CONVERGENCE pass=%u tolerance=%.17g "
					"G32=%.17g field32=%.17g G64=%.17g field64=%.17g "
					"ratio32=%.17g ratio64=%.17g device_p95_ms=%.17g wall_p95_ms=%.17g "
					"cell_submaps=%u source_commits=%u scalar_reads=%u projections=%u token=%d\n",
					pass,convergenceTolerance,generation32[pass-1u],field32[pass-1u],
					generation64[pass-1u],field64[pass-1u],
					pass>1u?generation32[pass-1u]/generation32[pass-2u]:0.0,
					pass>1u?generation64[pass-1u]/generation64[pass-2u]:0.0,
					deviceP95[pass-1u],wallP95[pass-1u],measured.cellSubmapCount,
					measured.sourceCommandCommitCount,
					measured.manifoldScalarDeviceToHostTransferCount,
					measured.residentProjectionInvocationCount,
					measured.HasAcceptedManifoldToken()?1:0);
			}
			if(!clearFixtureEnvironment("RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES"))
				return 200;
			std::ifstream raw(snapshotDirectory/
				"rendered/fire_production_calibration/r166_distribution_long_shadow/"
				"accepted_long_shadow.raw.v1");
			std::vector<double> transcriptGeneration,transcriptField;std::string line;
			while(std::getline(raw,line)){
				std::size_t step=0u;double stepS=0.0,predictor=0.0,generation=0.0,field=0.0;
				if(std::sscanf(line.c_str(),"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=%zu "
					"dt=%lf predictor_G=%lf G=%lf field_max=%lf",&step,&stepS,&predictor,
					&generation,&field)==5){transcriptGeneration.push_back(generation);
					transcriptField.push_back(field);}
			}
			if(transcriptGeneration.size()!=7u||transcriptField.size()!=7u)return 200;
			double meanBeginning=0.0,meanGeneration=0.0;
			for(std::size_t pair=0u;pair<6u;++pair){meanBeginning+=transcriptField[pair];
				meanGeneration+=transcriptGeneration[pair+1u];}
			meanBeginning/=6.0;meanGeneration/=6.0;
			double covariance=0.0,beginningVariance=0.0,generationVariance=0.0;
			for(std::size_t pair=0u;pair<6u;++pair){
				const double dx=transcriptField[pair]-meanBeginning;
				const double dy=transcriptGeneration[pair+1u]-meanGeneration;
				covariance+=dx*dy;beginningVariance+=dx*dx;generationVariance+=dy*dy;
			}
			const double gain=covariance/beginningVariance;
			const double intercept=meanGeneration-gain*meanBeginning;
			const double correlation=covariance/std::sqrt(beginningVariance*generationVariance);
			std::fprintf(stderr,"ANOMALY_CLOSURE_FEEDBACK_GAIN pairs=6 slope=%.17g "
				"intercept=%.17g pearson=%.17g raw=%s golden=%s\n",gain,intercept,
				correlation,DigestFile(snapshotDirectory/
					"rendered/fire_production_calibration/r166_distribution_long_shadow/"
					"accepted_long_shadow.raw.v1").c_str(),checkpointDigest);
			const bool converged=generation32.back()<=convergenceTolerance&&
				generation64.back()<=convergenceTolerance;
			const bool stalled=generation32.back()>0.001&&generation64.back()>0.001;
			std::fprintf(stderr,"ANOMALY_CLOSURE_CONVERGENCE_VERDICT tolerance=%.17g "
				"G32_pass8=%.17g G64_pass8=%.17g converged=%d stalled_above_1e-3=%d "
				"CFL_dt=%.17g golden=%s\n",convergenceTolerance,generation32.back(),
				generation64.back(),converged?1:0,stalled?1:0,
				static_cast<double>(representedCFLStep),checkpointDigest);
			return stalled?201:(converged?198:200);
		}
		if(timestepVelocityAuditMalformed){
			const std::uint64_t beginningCommandCount=
				RISE::FireProductionResidentStepMetalCommandCommitCount();
			RISE::FireProductionResidentStepResult rejected;
			rejected.conservativeValues.push_back(1.0f);
			rejected.projection.velocityMPerS[0].push_back(1.0f);
			rejected.cellSubmapCount=1u;rejected.residentProjectionInvocationCount=1u;
			rejected.deviceElapsedMS=1.0;rejected.manifoldPlateauPassed=true;
			const bool succeeded=RISE::AdvanceFireProductionResidentStepMetal(
				request,rejected,&error);
			const bool defaultResult=rejected.conservativeValues.empty()&&
				rejected.projection.velocityMPerS[0].empty()&&rejected.cellSubmapCount==0u&&
				rejected.residentProjectionInvocationCount==0u&&rejected.deviceElapsedMS==0.0&&
				!rejected.manifoldPlateauPassed&&!rejected.HasAcceptedManifoldToken();
			return !succeeded&&defaultResult&&error==
				"production timestep velocity audit activation is invalid"&&
				RISE::FireProductionResidentStepMetalCommandCommitCount()==beginningCommandCount?222:225;
		}
		if(timestepVelocityAudit){
			if(slice!=0u)return 222;
			request.enforceManifoldPlateau=true;
			if(!setFixtureEnvironment("RISE_FIRE_RESTORATION_PLATEAU_PROBE","1")||
				!setFixtureEnvironment("RISE_FIRE_PRODUCTION_RESTORATION_TEST","removed"))return 225;
			RISE::FireProductionResidentStepResult physicalOnly;
			if(!RISE::AdvanceFireProductionResidentStepMetal(request,physicalOnly,&error)){
				std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT physical-only failed: %s\n",
					error.c_str());return 225;
			}
			if(!clearFixtureEnvironment("RISE_FIRE_PRODUCTION_RESTORATION_TEST")||
				!clearFixtureEnvironment("RISE_FIRE_RESTORATION_PLATEAU_PROBE"))return 225;
			RISE::FireProductionResidentStepResult measured;
			if(!RISE::AdvanceFireProductionResidentStepMetal(request,measured,&error)){
				std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT failed: %s\n",error.c_str());
				return 225;
			}
			double transportMaximum=0.0,beginningMomentumVelocityMaximum=0.0;
			double beginningCorrectionMaximum=0.0,physicalMaximum=0.0,finalMaximum=0.0;
			double restorationCorrectionMaximum=0.0;
			unsigned int transportAxis=0u,beginningCorrectionAxis=0u,physicalAxis=0u,
				finalAxis=0u,restorationCorrectionAxis=0u;
			std::size_t transportFace=0u,beginningCorrectionFace=0u,physicalFace=0u,
				finalFace=0u,restorationCorrectionFace=0u;
			for(unsigned int axis=0u;axis<3u;++axis){
				for(std::size_t face=0u;face<request.cellTransport.frozenVelocityMPerS[axis].size();
					++face){
					const double transportVelocity=request.cellTransport.frozenVelocityMPerS[axis][face];
					const double momentumVelocity=request.force.beginningMomentumKGPerM2S[axis][face]/
						request.force.faceDensityKGPerM3[axis][face];
					if(std::fabs(transportVelocity)>transportMaximum){transportMaximum=
						std::fabs(transportVelocity);transportAxis=axis;transportFace=face;}
					beginningMomentumVelocityMaximum=std::max(beginningMomentumVelocityMaximum,
						std::fabs(momentumVelocity));
					if(std::fabs(transportVelocity-momentumVelocity)>beginningCorrectionMaximum){
						beginningCorrectionMaximum=std::fabs(transportVelocity-momentumVelocity);
						beginningCorrectionAxis=axis;beginningCorrectionFace=face;}
				}
				for(std::size_t face=0u;face<physicalOnly.projection.velocityMPerS[axis].size();
					++face){
					const double physical=physicalOnly.projection.velocityMPerS[axis][face];
					const double final=measured.projection.velocityMPerS[axis][face];
					if(std::fabs(physical)>physicalMaximum){physicalMaximum=std::fabs(physical);
						physicalAxis=axis;physicalFace=face;}
					if(std::fabs(final)>finalMaximum){finalMaximum=std::fabs(final);
						finalAxis=axis;finalFace=face;}
					if(std::fabs(final-physical)>restorationCorrectionMaximum){
						restorationCorrectionMaximum=std::fabs(final-physical);
						restorationCorrectionAxis=axis;restorationCorrectionFace=face;}
				}
			}
			const double selectorMaximum=0.5*shape.cellWidthM/dt;
			const double transportCFL=transportMaximum>0.0?0.5*shape.cellWidthM/transportMaximum:
				std::numeric_limits<double>::infinity();
			const double physicalCFL=physicalMaximum>0.0?0.5*shape.cellWidthM/physicalMaximum:
				std::numeric_limits<double>::infinity();
			std::vector<double> temperatureK(cells);
			for(std::size_t cell=0u;cell<cells;++cell)
				temperatureK[cell]=beginning.states[cell].temperatureK;
			OpenMACField3D transportVelocity;transportVelocity.component=beginning.velocity.component;
			std::vector<CellTransportEvaluation> transportEvaluation;
			if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperatureK,
				transportVelocity,shadowConfig.openBoundary,shadowConfig.dns,fuel,transport,
				FireStateProducerPrecision::Binary64,transportEvaluation,&error,16u))return 225;
			double maximumReducedGravity=0.0,maximumActiveDiffusivity=0.0;
			for(std::size_t cell=0u;cell<cells;++cell){
				const double gas=beginning.states[cell].GasDensity();
				const CellTransportEvaluation& evaluation=transportEvaluation[cell];
				maximumReducedGravity=std::max(maximumReducedGravity,
					std::max(0.0,9.80665*(ambient.GasDensity()-gas)/gas));
				maximumActiveDiffusivity=std::max({maximumActiveDiffusivity,
					evaluation.totalDiffusivityM2PerS,evaluation.effectiveViscosityPaS/gas,
					evaluation.effectiveConductivityWPerMK/(gas*evaluation.gasCpJPerKGK)});
			}
			RISE::FireProductionStableTimeStep auditedSelection;
			const RISE::FireProductionAcceptedCheckpointStateView initialState;
			const RISE::FireProductionAcceptedManifoldObservation initialObservation;
			if(!RISE::SelectFireProductionStableTimeStep(shape.cellWidthM,transportMaximum,
				maximumReducedGravity,maximumActiveDiffusivity,0.0,initialState,
				initialObservation,auditedSelection,&error))return 225;
			const float representedAuditedStep=static_cast<float>(auditedSelection.seconds);
			request.force.timeStepS=representedAuditedStep;
			request.cellTransport.timeStepS=representedAuditedStep;
			request.dualTransport.timeStepS=representedAuditedStep;
			shadowConfig.transport.deltaTimeS=representedAuditedStep;shadowConfig.workerCount=16u;
			ConservativeAdvance3DResult auditedOracle;
			if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
				shadowConfig,fuel,fuel,transport,auditedOracle,&error))return 225;
			for(std::size_t cell=0u;cell<cells;++cell){
				request.divergenceTargetPerS[cell]=static_cast<float>(
					auditedOracle.divergenceHeunPerS[cell]);
				request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
					request.beginningManifoldDeviationPerCell[cell]/
					static_cast<double>(representedAuditedStep));
			}
			auto sameProjectionArithmetic=[](const RISE::FireProductionProjectionResult& left,
				const RISE::FireProductionProjectionResult& right){return
				left.maximumPreProjectionResidualPerS==right.maximumPreProjectionResidualPerS&&
				left.maximumPostProjectionResidualPerS==right.maximumPostProjectionResidualPerS&&
				left.maximumOpenComplementarityDiscrepancyMPerS==
					right.maximumOpenComplementarityDiscrepancyMPerS&&
				left.removedFineRightHandSideMean==right.removedFineRightHandSideMean&&
				left.executedVCycleCount==right.executedVCycleCount&&
				left.executedJacobiSweepCount==right.executedJacobiSweepCount&&
				left.residentUploadStagingCount==right.residentUploadStagingCount&&
				left.residentInterstageDeviceToHostTransferCount==
					right.residentInterstageDeviceToHostTransferCount&&
				left.residentTerminalStagingCount==right.residentTerminalStagingCount&&
				left.residentCommandCommitCount==right.residentCommandCommitCount&&
				left.residentProjectionInvocationCount==right.residentProjectionInvocationCount&&
				left.residentCertifiedWorkingSetBytes==right.residentCertifiedWorkingSetBytes&&
				left.residentActualMetalAllocationBytes==right.residentActualMetalAllocationBytes&&
				left.validationPassed==right.validationPassed;};
			auto sameResidentArithmetic=[&](const RISE::FireProductionResidentStepResult& left,
				const RISE::FireProductionResidentStepResult& right){return
				RISE::FireProductionAcceptedManifoldPayloadDigest(left)==
					RISE::FireProductionAcceptedManifoldPayloadDigest(right)&&
				left.forceSchedule.substepCount==right.forceSchedule.substepCount&&
				left.forceSchedule.substepTimeS==right.forceSchedule.substepTimeS&&
				left.forceSchedule.outwardWork==right.forceSchedule.outwardWork&&
				left.forceSchedule.representedProductUpper==
					right.forceSchedule.representedProductUpper&&
				left.forceDiagnostics.outwardLambdaPerS==right.forceDiagnostics.outwardLambdaPerS&&
				left.forceDiagnostics.scalarDiagnosticTransferCount==
					right.forceDiagnostics.scalarDiagnosticTransferCount&&
				left.forceDiagnostics.substepLoopDeviceToHostTransferCount==
					right.forceDiagnostics.substepLoopDeviceToHostTransferCount&&
				left.forceDiagnostics.terminalStagingCount==right.forceDiagnostics.terminalStagingCount&&
				left.forceDiagnostics.commandCommitCount==right.forceDiagnostics.commandCommitCount&&
				left.forceDiagnostics.certifiedWorkingSetBytes==
					right.forceDiagnostics.certifiedWorkingSetBytes&&
				left.forceDiagnostics.actualMetalAllocationBytes==
					right.forceDiagnostics.actualMetalAllocationBytes&&
				left.transportedDual.executedSubmapCount==right.transportedDual.executedSubmapCount&&
				left.transportedDual.canonicalSeamCopyCount==
					right.transportedDual.canonicalSeamCopyCount&&
				left.transportedDual.commandCommitCount==right.transportedDual.commandCommitCount&&
				left.transportedDual.interstageFullGridTransferCount==
					right.transportedDual.interstageFullGridTransferCount&&
				left.transportedDual.actualMetalAllocationBytes==
					right.transportedDual.actualMetalAllocationBytes&&
				left.cellSubmapCount==right.cellSubmapCount&&left.dualSubmapCount==right.dualSubmapCount&&
				left.sourceCommandCommitCount==right.sourceCommandCommitCount&&
				left.residentProjectionInvocationCount==right.residentProjectionInvocationCount&&
				left.interstageFullGridTransferCount==right.interstageFullGridTransferCount&&
				left.terminalStagingCount==right.terminalStagingCount&&
				left.combinedCertifiedWorkingSetBytes==right.combinedCertifiedWorkingSetBytes&&
				left.combinedActualMetalAllocationBytes==right.combinedActualMetalAllocationBytes&&
				left.representedTimeStepS==right.representedTimeStepS&&
				left.maximumManifoldGeneration==right.maximumManifoldGeneration&&
				left.maximumAcceptedManifoldDeviation==right.maximumAcceptedManifoldDeviation&&
				left.acceptedManifoldDeviationP95==right.acceptedManifoldDeviationP95&&
				left.acceptedManifoldDeviationP50==right.acceptedManifoldDeviationP50&&
				left.manifoldGenerationAuthoritative==right.manifoldGenerationAuthoritative&&
				left.manifoldTailRestorationApplied==right.manifoldTailRestorationApplied&&
				left.manifoldTailCellCount==right.manifoldTailCellCount&&
				left.manifoldTailExcessSum==right.manifoldTailExcessSum&&
				left.manifoldTailDrainedVolumeM3==right.manifoldTailDrainedVolumeM3&&
				left.manifoldDynamicsBoundPassed==right.manifoldDynamicsBoundPassed&&
				left.manifoldMapCellCount==right.manifoldMapCellCount&&
				left.manifoldScalarDeviceToHostTransferCount==
					right.manifoldScalarDeviceToHostTransferCount&&
				left.manifoldFullGridDeviceToHostTransferCount==
					right.manifoldFullGridDeviceToHostTransferCount&&
				left.manifoldStageGeneration==right.manifoldStageGeneration&&
				left.requiredRestorationDrainFraction==right.requiredRestorationDrainFraction&&
				left.deliveredRestorationDrainFraction==right.deliveredRestorationDrainFraction&&
				left.restorationResidualBandPerS==right.restorationResidualBandPerS&&
				left.suggestedManifoldTimeStepS==right.suggestedManifoldTimeStepS&&
				left.manifoldNextTimeStepAvailable==right.manifoldNextTimeStepAvailable&&
				left.manifoldPlateauPassed==right.manifoldPlateauPassed&&
				left.HasAcceptedManifoldToken()==right.HasAcceptedManifoldToken()&&
				left.conservativeProducerPrecision==right.conservativeProducerPrecision&&
				left.acceptedShape.nx==right.acceptedShape.nx&&left.acceptedShape.ny==right.acceptedShape.ny&&
				left.acceptedShape.nz==right.acceptedShape.nz&&
				left.acceptedShape.cellWidthM==right.acceptedShape.cellWidthM&&
				sameProjectionArithmetic(left.physicalProjection,right.physicalProjection)&&
				sameProjectionArithmetic(left.projection,right.projection);};
			std::array<double,5> serialWall={{}},serialDevice={{}},auditedWall={{}},auditedDevice={{}};
			RISE::FireProductionResidentStepResult auditedResident,serialResident;
			bool serialBaselineAvailable=false;
			auto timeSelectedStep=[&](const char* mode,const bool establishSerialBaseline,
				std::array<double,5>& wall,std::array<double,5>& device){
				if(!setFixtureEnvironment("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE",mode))return false;
				for(std::size_t trial=0u;trial<=wall.size();++trial){
					const auto trialStart=std::chrono::steady_clock::now();
					const bool auditedAccepted=RISE::AttemptFireProductionResidentStepMetal(
						request,auditedResident,&error);
					if(!auditedAccepted&&!(auditedResident.manifoldNextTimeStepAvailable&&
						!auditedResident.manifoldPlateauPassed&&!auditedResident.HasAcceptedManifoldToken())){
						std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT selected-step mode=%s "
							"trial=%zu failed: %s\n",mode,trial,error.c_str());return false;}
					if(auditedResident.representedTimeStepS!=representedAuditedStep||
						auditedResident.forceSchedule.substepCount!=1u||
						auditedResident.forceSchedule.substepTimeS!=representedAuditedStep){
						std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT selected-step mode=%s "
							"trial=%zu did not execute the represented selected step\n",mode,trial);
						return false;}
					const auto trialEnd=std::chrono::steady_clock::now();
					if(!serialBaselineAvailable){
						if(!establishSerialBaseline)return false;
						serialResident=auditedResident;serialBaselineAvailable=true;
					}else if(!sameResidentArithmetic(serialResident,auditedResident)){
						std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT selected-step mode=%s "
							"trial=%zu changed arithmetic\n",mode,trial);return false;}
					if(trial>0u){wall[trial-1u]=std::chrono::duration<double,std::milli>(
						trialEnd-trialStart).count();
						device[trial-1u]=auditedResident.deviceMakespanMS;}
				}
				return true;
			};
			if(!timeSelectedStep("serial",true,serialWall,serialDevice)||
				!timeSelectedStep("parallel",false,auditedWall,auditedDevice)||
				!clearFixtureEnvironment("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE"))return 225;
			const bool serialParallelArithmeticIdentical=serialBaselineAvailable&&
				sameResidentArithmetic(serialResident,auditedResident);
			std::sort(serialWall.begin(),serialWall.end());
			std::sort(serialDevice.begin(),serialDevice.end());
			std::sort(auditedWall.begin(),auditedWall.end());
			std::sort(auditedDevice.begin(),auditedDevice.end());
			std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT selector_gprime=%.17g "
				"selector_diffusivity=%.17g selected_dt=%.17g represented_dt=%.17g "
				"active_limit=%s selected_substeps=%u serial_device_p95_ms=%.17g "
				"serial_wall_p95_ms=%.17g device_makespan_p95_ms=%.17g wall_p95_ms=%.17g\n",
				maximumReducedGravity,maximumActiveDiffusivity,auditedSelection.seconds,
				static_cast<double>(representedAuditedStep),
				auditedSelection.activeLimit?auditedSelection.activeLimit:"null",
				auditedResident.forceSchedule.substepCount,serialDevice.back(),serialWall.back(),
				auditedDevice.back(),auditedWall.back());
			std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT dt=%.17g dx=%.17g selector_max=%.17g "
				"transport_max=%.17g transport_axis=%u transport_face=%zu "
				"beginning_momentum_velocity_max=%.17g beginning_correction_max=%.17g "
				"beginning_correction_axis=%u beginning_correction_face=%zu\n",
				dt,shape.cellWidthM,selectorMaximum,transportMaximum,transportAxis,transportFace,
				beginningMomentumVelocityMaximum,beginningCorrectionMaximum,
				beginningCorrectionAxis,beginningCorrectionFace);
			std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT physical_max=%.17g physical_axis=%u "
				"physical_face=%zu final_max=%.17g final_axis=%u final_face=%zu "
				"restoration_correction_max=%.17g restoration_axis=%u restoration_face=%zu "
				"restoration_impulse_max=%.17g transport_cfl_dt=%.17g physical_cfl_dt=%.17g\n",
				physicalMaximum,physicalAxis,physicalFace,finalMaximum,finalAxis,finalFace,
				restorationCorrectionMaximum,restorationCorrectionAxis,restorationCorrectionFace,
				restorationCorrectionMaximum*dt,transportCFL,physicalCFL);
			const bool topologyBound=
				physicalOnly.residentProjectionInvocationCount==1u&&
				physicalOnly.interstageFullGridTransferCount==0u&&
				physicalOnly.projection.validationPassed&&
				physicalOnly.projection.executedVCycleCount==17u&&
				physicalOnly.manifoldScalarDeviceToHostTransferCount==0u&&
				physicalOnly.manifoldFullGridDeviceToHostTransferCount==0u&&
				measured.residentProjectionInvocationCount==2u&&
				measured.interstageFullGridTransferCount==0u&&
				measured.physicalProjection.validationPassed&&
				measured.physicalProjection.executedVCycleCount==17u&&
				physicalOnly.projection.executedJacobiSweepCount==
					measured.physicalProjection.executedJacobiSweepCount&&
				physicalOnly.projection.maximumPreProjectionResidualPerS==
					measured.physicalProjection.maximumPreProjectionResidualPerS&&
				physicalOnly.projection.maximumPostProjectionResidualPerS==
					measured.physicalProjection.maximumPostProjectionResidualPerS&&
				physicalOnly.projection.maximumOpenComplementarityDiscrepancyMPerS==
					measured.physicalProjection.maximumOpenComplementarityDiscrepancyMPerS&&
				measured.projection.validationPassed&&
				measured.projection.executedVCycleCount==16u&&
				measured.manifoldScalarDeviceToHostTransferCount==1u&&
				measured.manifoldFullGridDeviceToHostTransferCount==0u;
			const bool valuesBound=
				dt==5.6295254283638751e-05&&shape.cellWidthM==0.02447449285392881&&
				selectorMaximum==217.37616398903009&&
				transportMaximum==7.4333348274230957&&transportAxis==2u&&
				transportFace==714534u&&
				beginningMomentumVelocityMaximum==7.4333348274230957&&
				beginningCorrectionMaximum==0.03793315589427948&&
				beginningCorrectionAxis==2u&&beginningCorrectionFace==3657u&&
				physicalMaximum==7.371121883392334&&physicalAxis==2u&&
				physicalFace==714534u&&finalMaximum==7.371121883392334&&
				finalAxis==2u&&finalFace==714534u&&
				restorationCorrectionMaximum==1.7818529158830643e-06&&
				restorationCorrectionAxis==2u&&restorationCorrectionFace==978626u&&
				restorationCorrectionMaximum*dt==1.0030986299568027e-10&&
				transportCFL==0.0016462660045688639&&
				physicalCFL==0.0016601606404766957&&
				maximumReducedGravity==48.944695265891369&&
				maximumActiveDiffusivity==0.0030345390611787094&&
				auditedSelection.seconds==transportCFL&&auditedSelection.activeLimit&&
				std::strcmp(auditedSelection.activeLimit,"advective_CFL")==0&&
				representedAuditedStep==0.0016462659696117043f&&
				auditedResident.forceSchedule.substepCount==1u&&
				auditedResident.representedTimeStepS==representedAuditedStep&&
				auditedResident.forceSchedule.substepTimeS==representedAuditedStep&&
				serialParallelArithmeticIdentical&&
				std::isfinite(serialDevice.back())&&std::isfinite(serialWall.back())&&
				auditedWall.back()<serialWall.back()&&
				std::isfinite(auditedDevice.back())&&auditedDevice.back()<=125.0&&
				std::isfinite(auditedWall.back())&&auditedWall.back()<=200.0;
			std::fprintf(stderr,"TIMESTEP_VELOCITY_AUDIT topology=%d values=%d "
				"physical_only_invocations=%u cycles=%u validation=%d scalar_reads=%u full_reads=%u "
				"normal_invocations=%u physical_cycles=%u physical_validation=%d "
				"restoration_cycles=%u restoration_validation=%d scalar_reads=%u full_reads=%u "
				"audited_selected=%.17g audited_limit=%s\n",topologyBound?1:0,valuesBound?1:0,
				physicalOnly.residentProjectionInvocationCount,
				physicalOnly.projection.executedVCycleCount,
				physicalOnly.projection.validationPassed?1:0,
				physicalOnly.manifoldScalarDeviceToHostTransferCount,
				physicalOnly.manifoldFullGridDeviceToHostTransferCount,
				measured.residentProjectionInvocationCount,
				measured.physicalProjection.executedVCycleCount,
				measured.physicalProjection.validationPassed?1:0,
				measured.projection.executedVCycleCount,
				measured.projection.validationPassed?1:0,
				measured.manifoldScalarDeviceToHostTransferCount,
				measured.manifoldFullGridDeviceToHostTransferCount,auditedSelection.seconds,
				auditedSelection.activeLimit?auditedSelection.activeLimit:"null");
			return DigestFile(checkpointPath)==checkpointDigest&&topologyBound&&valuesBound?247:225;
		}
		if(stageBudgetProbe){
			if(slice!=0u)return 225;
			std::array<std::array<double,3>,3> stageGeneration={{{{0.0,0.0,0.0}},
				{{0.0,0.0,0.0}},{{0.0,0.0,0.0}}}};
			std::array<double,3> independentGeneration={{0.0,0.0,0.0}},
				reconstructionGeneration={{0.0,0.0,0.0}},
				reconstructionBeginningDeviation={{0.0,0.0,0.0}},
				reconstructionTerminalDeviation={{0.0,0.0,0.0}},
				reconstructionProductionMaximumDifference={{0.0,0.0,0.0}},
				reconstructionMaximumEnergyLedgerResidual={{0.0,0.0,0.0}},
				reconstructionMaximumEnergyLedgerRelative={{0.0,0.0,0.0}},
				reconstructionMaximumLowerEndpointProjection={{0.0,0.0,0.0}},
				reconstructionMaximumUpperEndpointProjection={{0.0,0.0,0.0}},
				reconstructionMaximumLowOrderLowerExcursion={{0.0,0.0,0.0}},
				reconstructionMaximumLowOrderUpperExcursion={{0.0,0.0,0.0}},
				deviceMS={{0.0,0.0,0.0}},wallMS={{0.0,0.0,0.0}};
			std::array<std::string,3> reconstructionFieldDigest,reconstructionTraceDigest,
				productionFieldDigest;
			std::array<std::size_t,3> reconstructionLowerEndpointProjectionCount={{0u,0u,0u}},
				reconstructionUpperEndpointProjectionCount={{0u,0u,0u}},
				reconstructionLowOrderLowerInfeasibleCount={{0u,0u,0u}},
				reconstructionLowOrderUpperInfeasibleCount={{0u,0u,0u}},
				reconstructionValidatedFaceCount={{0u,0u,0u}},
				reconstructionValidatedPassCellCount={{0u,0u,0u}};
			std::array<std::size_t,3> allLowOrderFirstPass={{5u,5u,5u}},
				allLowOrderFirstLine={{0u,0u,0u}},allLowOrderFirstDonor={{0u,0u,0u}},
				allLowOrderFirstCell={{0u,0u,0u}};
			std::array<double,3> allLowOrderFirstTemperature={{0.0,0.0,0.0}},
				allLowOrderFirstEnergy={{0.0,0.0,0.0}},
				allLowOrderFirstLowerEnergy={{0.0,0.0,0.0}},
				allLowOrderFirstTolerance={{0.0,0.0,0.0}},
				allLowOrderGeneration={{0.0,0.0,0.0}},
				allLowOrderBeginningDeviation={{0.0,0.0,0.0}},
				allLowOrderTerminalDeviation={{0.0,0.0,0.0}},
				allLowOrderMaximumEndpointExcursion={{0.0,0.0,0.0}},
				allLowOrderMaximumEnergyLedgerResidual={{0.0,0.0,0.0}},
				allLowOrderMaximumEnergyLedgerRelative={{0.0,0.0,0.0}};
			std::array<std::size_t,3> allLowOrderCell={{0u,0u,0u}},
				allLowOrderEndpointProjectionCount={{0u,0u,0u}};
			std::array<std::string,3> allLowOrderFieldDigest;
			std::array<std::string,3> allLowOrderFirstDonorDigest,
				greedyAtAllLowOrderFirstDonorDigest;
			std::array<double,3> greedyAtAllLowOrderFirstTemperature={{0.0,0.0,0.0}};
			std::array<std::size_t,3> allLowOrderMaximumExcursionPass={{5u,5u,5u}},
				allLowOrderMaximumExcursionLine={{0u,0u,0u}},
				allLowOrderMaximumExcursionDonor={{0u,0u,0u}},
				greedyMaximumExcursionPass={{5u,5u,5u}},
				greedyMaximumExcursionLine={{0u,0u,0u}},
				greedyMaximumExcursionDonor={{0u,0u,0u}};
			std::array<std::string,3> allLowOrderMaximumExcursionDigest,
				greedyMaximumExcursionDigest;
			std::array<std::size_t,3> reconstructionCell={{0u,0u,0u}};
			for(std::size_t level=0u;level<3u;++level){
				const float representedStep=static_cast<float>(dt/std::pow(2.0,level));
				RISE::FireProductionResidentStepRequest sweepRequest=request;
				sweepRequest.force.timeStepS=representedStep;
				sweepRequest.cellTransport.timeStepS=representedStep;
				sweepRequest.dualTransport.timeStepS=representedStep;
				ConservativeAdvance3DConfig sweepConfig=shadowConfig;
				sweepConfig.transport.deltaTimeS=representedStep;sweepConfig.workerCount=16u;
				ConservativeAdvance3DResult sweepOracle;
				if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
					sweepConfig,fuel,fuel,transport,sweepOracle,&error))return 225;
				for(std::size_t cell=0u;cell<cells;++cell){
					sweepRequest.divergenceTargetPerS[cell]=static_cast<float>(
						sweepOracle.divergenceHeunPerS[cell]);
					sweepRequest.restorationDivergenceTargetPerS[cell]=static_cast<float>(
						sweepRequest.beginningManifoldDeviationPerCell[cell]/representedStep);
				}
				sweepRequest.enforceManifoldPlateau=true;
				RISE::FireProductionResidentStepResult measured;
				const auto wallBeginning=std::chrono::steady_clock::now();
				if(!RISE::AdvanceFireProductionResidentStepMetal(sweepRequest,measured,&error)){
					std::fprintf(stderr,"MANIFOLD_STAGE_BUDGET level=%zu failed: %s\n",
						level,error.c_str());return 225;}
				wallMS[level]=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-wallBeginning).count();
				deviceMS[level]=measured.deviceElapsedMS;
				stageGeneration[level]=measured.manifoldStageGeneration;
				std::vector<ConservativeVector> terminal;
				if(!FireProductionDyadicCalibration::UnpackProductionConservative(
					measured.conservativeValues,cells,terminal))return 225;
				for(std::size_t cell=0u;cell<cells;++cell){double terminalRatio=0.0;
					if(!AcceptedConservativeVolumeRatio(terminal[cell],fuel,
						FireStateProducerPrecision::Binary32,terminalRatio,&error))return 225;
					independentGeneration[level]=std::max(independentGeneration[level],std::fabs(
						(terminalRatio-1.0)-sweepRequest.beginningManifoldDeviationPerCell[cell]));
				}

				// Retained diagnostic-only execution of the ruled conservative remedy.
				// The transported tuple replaces energy with n*T.  One shared alpha
				// limits rhoZ, all seven constituents, and n*T; the energy ledger is
				// updated only by face fluxes rebuilt from each swept slug's composition
				// and temperature.  No cell-average repair occurs.
				std::vector<float> reconstructed=sweepRequest.cellTransport.conservativeValues;
				std::vector<float> transported(9u*cells),energy(cells);
				RISECBOR64::Bytes reconstructionTrace;
				const float manifoldNT=static_cast<float>(fuel.ThermodynamicPressurePa()/
					8314.46261815324);
				for(std::size_t cell=0u;cell<cells;++cell){
					for(std::size_t component=0u;component<8u;++component)
						transported[component*cells+cell]=reconstructed[component*cells+cell];
					transported[8u*cells+cell]=manifoldNT;
					energy[cell]=reconstructed[8u*cells+cell];
				}
				std::vector<float> allLowOrderTransported=transported,
					allLowOrderEnergy=energy;
				std::array<double,MethaneSpeciesCount> lowerEndpointEnthalpy,
					upperEndpointEnthalpy;
				if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
					lowerEndpointEnthalpy.data(),lowerEndpointEnthalpy.size(),&error)||
					!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
						upperEndpointEnthalpy.data(),upperEndpointEnthalpy.size(),&error))return 225;
				std::array<float,9> transportedAmbient{};
				for(std::size_t component=0u;component<8u;++component)
					transportedAmbient[component]=sweepRequest.cellTransport.ambientValues[component];
				transportedAmbient[8]=manifoldNT;
				auto axisExtent=[&](unsigned int axis){return axis==0u?
					sweepRequest.cellTransport.shape.nx:(axis==1u?
						sweepRequest.cellTransport.shape.ny:sweepRequest.cellTransport.shape.nz);};
				auto axisLines=[&](unsigned int axis){return axis==0u?
					sweepRequest.cellTransport.shape.ny*sweepRequest.cellTransport.shape.nz:
					(axis==1u?sweepRequest.cellTransport.shape.nx*
						sweepRequest.cellTransport.shape.nz:sweepRequest.cellTransport.shape.nx*
							sweepRequest.cellTransport.shape.ny);};
				auto coordinates=[&](unsigned int axis,std::size_t line,std::size_t coordinate,
					std::size_t& x,std::size_t& y,std::size_t& z){
					if(axis==0u){x=coordinate;y=line%sweepRequest.cellTransport.shape.ny;
						z=line/sweepRequest.cellTransport.shape.ny;return;}
					if(axis==1u){x=line%sweepRequest.cellTransport.shape.nx;y=coordinate;
						z=line/sweepRequest.cellTransport.shape.nx;return;}
					x=line%sweepRequest.cellTransport.shape.nx;
					y=line/sweepRequest.cellTransport.shape.nx;z=coordinate;};
				auto cellIndex=[&](std::size_t x,std::size_t y,std::size_t z){return
					(z*sweepRequest.cellTransport.shape.ny+y)*
						sweepRequest.cellTransport.shape.nx+x;};
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				const float steps[]={0.5f*representedStep,0.5f*representedStep,
					representedStep,0.5f*representedStep,0.5f*representedStep};
				for(unsigned int pass=0u;pass<5u;++pass){
					const unsigned int axis=axes[pass];const std::size_t length=axisExtent(axis),
						lines=axisLines(axis);
					// Fixed pressure is an algebraic manifold constraint, not a conserved
					// tracer.  Re-establish n*T=P/R before every directional sweep so every
					// reconstructed slug, rather than only the first x half-sweep, lies on
					// the same fixed-pressure EOS surface.
					for(std::size_t cell=0u;cell<cells;++cell)
						transported[8u*cells+cell]=manifoldNT;
					RISE::FireProductionRemapRequest lineRequest;lineRequest.lineLength=length;
					lineRequest.lineCount=lines;lineRequest.componentCount=9u;
					lineRequest.cellWidthM=sweepRequest.cellTransport.shape.cellWidthM;
					lineRequest.timeStepS=steps[pass];lineRequest.asymmetricBoundaries=true;
					auto remapBoundary=[](RISE::FireProductionProjectionBoundary value){return
						value==RISE::FireProductionProjectionPeriodic?RISE::FireProductionRemapPeriodic:
							(value==RISE::FireProductionProjectionPressureOpen?
								RISE::FireProductionRemapPressureOpen:
								RISE::FireProductionRemapWall);};
					lineRequest.lowerBoundary=remapBoundary(
						sweepRequest.cellTransport.boundary[2u*axis]);
					lineRequest.upperBoundary=remapBoundary(
						sweepRequest.cellTransport.boundary[2u*axis+1u]);
					lineRequest.ambientValues.assign(transportedAmbient.begin(),transportedAmbient.end());
					lineRequest.values.resize(9u*lines*length);
					lineRequest.faceVelocityMPerS.resize(lines*(length+1u));
					for(std::size_t line=0u;line<lines;++line){
						for(std::size_t face=0u;face<=length;++face){std::size_t x=0u,y=0u,z=0u;
							coordinates(axis,line,face,x,y,z);std::size_t faceIndex=0u;
							if(axis==0u)faceIndex=(z*sweepRequest.cellTransport.shape.ny+y)*
								(sweepRequest.cellTransport.shape.nx+1u)+x;
							else if(axis==1u)faceIndex=(z*(sweepRequest.cellTransport.shape.ny+1u)+y)*
								sweepRequest.cellTransport.shape.nx+x;
							else faceIndex=(z*sweepRequest.cellTransport.shape.ny+y)*
								sweepRequest.cellTransport.shape.nx+x;
							lineRequest.faceVelocityMPerS[line*(length+1u)+face]=
								sweepRequest.cellTransport.frozenVelocityMPerS[axis][faceIndex];}
						for(std::size_t coordinate=0u;coordinate<length;++coordinate){
							std::size_t x=0u,y=0u,z=0u;coordinates(axis,line,coordinate,x,y,z);
							const std::size_t cell=cellIndex(x,y,z);
							for(std::size_t component=0u;component<9u;++component)
								lineRequest.values[(component*lines+line)*length+coordinate]=
									transported[component*cells+cell];}}
					// Independent all-low-order trajectory.  Every preceding directional
					// pass uses alpha=0, so a later rejected donor cannot be repaired by
					// backtracking an earlier alpha.  The obstruction is tested with the
					// authoritative r60 energy-scale predicate, never a temperature proxy.
					RISE::FireProductionRemapRequest allLowOrderRequest=lineRequest;
					for(std::size_t line=0u;line<lines;++line)
						for(std::size_t coordinate=0u;coordinate<length;++coordinate){
							std::size_t x=0u,y=0u,z=0u;coordinates(axis,line,coordinate,x,y,z);
							const std::size_t cell=cellIndex(x,y,z);
							for(std::size_t component=0u;component<8u;++component)
								allLowOrderRequest.values[(component*lines+line)*length+coordinate]=
									allLowOrderTransported[component*cells+cell];
							allLowOrderRequest.values[(8u*lines+line)*length+coordinate]=manifoldNT;}
					RISE::FireProductionRemapResult allLowOrderResult;
					if(!RISE::RemapFireProductionCPU(allLowOrderRequest,allLowOrderResult,&error)){
						std::fprintf(stderr,"MANIFOLD_ALL_LOW_ORDER remap failed level=%zu pass=%u: %s\n",
							level,pass,error.c_str());return 225;}
					const bool allLowPeriodic=allLowOrderRequest.lowerBoundary==
						RISE::FireProductionRemapPeriodic&&allLowOrderRequest.upperBoundary==
						RISE::FireProductionRemapPeriodic;
					auto allLowDonorCoordinate=[&](const std::size_t face,const float velocity,
						std::size_t& donor,bool& ambientDonor){ambientDonor=false;
						if(velocity>=0.0f){if(face>0u){donor=face-1u;return true;}
							if(allLowPeriodic){donor=length-1u;return true;}
							ambientDonor=allLowOrderRequest.lowerBoundary==
								RISE::FireProductionRemapPressureOpen;return ambientDonor;}
						if(face<length){donor=face;return true;}
						if(allLowPeriodic){donor=0u;return true;}
						ambientDonor=allLowOrderRequest.upperBoundary==
							RISE::FireProductionRemapPressureOpen;return ambientDonor;};
					auto allLowDonorValue=[&](const std::size_t component,const std::size_t line,
						const std::size_t donor,const bool ambientDonor){return ambientDonor?
						allLowOrderRequest.ambientValues[component]:allLowOrderRequest.values[
							(component*lines+line)*length+donor];};
					std::vector<float> allLowEnergyFlux(lines*(length+1u),0.0f),
						allLowNextEnergy=allLowOrderEnergy;
					for(std::size_t line=0u;line<lines;++line)for(std::size_t face=0u;
						face<(allLowPeriodic?length:length+1u);++face){
						const std::size_t fluxBase=line*(length+1u)+face;
						const float velocity=allLowOrderRequest.faceVelocityMPerS[fluxBase];
						std::size_t donor=0u;bool ambientDonor=false;
						if(velocity==0.0f||!allLowDonorCoordinate(face,velocity,donor,ambientDonor))continue;
						const float courant=allLowOrderRequest.timeStepS*velocity/
							allLowOrderRequest.cellWidthM;
						for(std::size_t component=0u;component<9u;++component){
							const std::size_t flux=(component*lines+line)*(length+1u)+face;
							allLowOrderResult.faceFluxes[flux]=allLowOrderRequest.cellWidthM*
								(courant*allLowDonorValue(component,line,donor,ambientDonor));}
						double donorMolarDensity=0.0;
						for(std::size_t species=0u;species<MethaneCarbon;++species){
							const FireThermochemistrySpecies* record=fuel.FindSpecies(
								fuel.SpeciesOrder()[species].c_str());if(!record)return 225;
							donorMolarDensity+=allLowDonorValue(species+1u,line,donor,ambientDonor)/
								record->molecularWeightKGPerKMol;}
						if(!(donorMolarDensity>0.0)){std::fprintf(stderr,
							"MANIFOLD_ALL_LOW_ORDER molar failed level=%zu pass=%u line=%zu face=%zu\n",
							level,pass,line,face);return 225;}
						const double donorTemperature=(fuel.ThermodynamicPressurePa()/
							8314.46261815324)/donorMolarDensity;
						ConservativeVector donorState{};
						for(std::size_t component=0u;component<8u;++component)
							donorState[component]=allLowDonorValue(component,line,donor,ambientDonor);
						double lowerEnergy=0.0,upperEnergy=0.0;
						for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							{lowerEnergy+=donorState[species+1u]*lowerEndpointEnthalpy[species];
							upperEnergy+=donorState[species+1u]*upperEndpointEnthalpy[species];}
						donorState[MethaneMassStateDimension]=donorTemperature<fuel.TemperatureMinK()?
							lowerEnergy:upperEnergy;
						std::string endpointError;
						if(!AcceptedStateAdmissible(donorState,
							lowerEndpointEnthalpy,upperEndpointEnthalpy,fuel,
							FireStateProducerPrecision::Binary32,&endpointError)){std::fprintf(stderr,
								"MANIFOLD_ALL_LOW_ORDER endpoint state failed level=%zu pass=%u "
								"line=%zu face=%zu: %s\n",level,pass,line,face,
								endpointError.c_str());return 225;}
						const double endpointTolerance=AcceptedStateRoundoffFactor(
							fuel.AcceptedStateFeasibilityEnvelope(),
							FireStateProducerPrecision::Binary32)*AcceptedStateEnergyScale(
								donorState,lowerEndpointEnthalpy,upperEndpointEnthalpy);
						std::size_t donorX=0u,donorY=0u,donorZ=0u;
						coordinates(axis,line,donor,donorX,donorY,donorZ);
						const std::size_t donorCell=cellIndex(donorX,donorY,donorZ);
						const bool alphaZeroThermochemistryUnavailable=!ambientDonor&&pass==1u&&
							donorTemperature<fuel.TemperatureMinK();
						if(!ambientDonor&&(donorTemperature<fuel.TemperatureMinK()||
							donorTemperature>fuel.TemperatureMaxK())){
							++allLowOrderEndpointProjectionCount[level];
							const double endpointExcursion=donorTemperature<fuel.TemperatureMinK()?
								fuel.TemperatureMinK()-donorTemperature:
									donorTemperature-fuel.TemperatureMaxK();
							if(endpointExcursion>allLowOrderMaximumEndpointExcursion[level]){
								allLowOrderMaximumEndpointExcursion[level]=endpointExcursion;
								allLowOrderMaximumExcursionPass[level]=pass;
								allLowOrderMaximumExcursionLine[level]=line;
								allLowOrderMaximumExcursionDonor[level]=donor;
								RISECBOR64::Bytes donorBytes;
								for(std::size_t component=0u;component<8u;++component){
									const float value=allLowDonorValue(component,line,donor,false);
									std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
									FireProductionDyadicCalibration::AppendInteger(donorBytes,bits);}
								allLowOrderMaximumExcursionDigest[level]=
									RISECBOR64::SHA256Hex(donorBytes);}}
						if(alphaZeroThermochemistryUnavailable&&
							allLowOrderFirstPass[level]==5u){
							allLowOrderFirstPass[level]=pass;allLowOrderFirstLine[level]=line;
							allLowOrderFirstDonor[level]=donor;
							allLowOrderFirstCell[level]=donorCell;
							std::array<double,MethaneSpeciesCount> unavailableEnthalpy;
							std::string domainError;
							if(fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(donorTemperature,
								unavailableEnthalpy.data(),unavailableEnthalpy.size(),&domainError)||
								domainError!="methane thermochemistry lookup is out of domain")return 225;
							allLowOrderFirstTemperature[level]=donorTemperature;
							allLowOrderFirstEnergy[level]=donorState[MethaneMassStateDimension];
							allLowOrderFirstLowerEnergy[level]=lowerEnergy;
							allLowOrderFirstTolerance[level]=endpointTolerance;
							RISECBOR64::Bytes firstDonorBytes;
							for(std::size_t component=0u;component<8u;++component){
								const float value=allLowDonorValue(component,line,donor,false);
								std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
								FireProductionDyadicCalibration::AppendInteger(firstDonorBytes,bits);}
							allLowOrderFirstDonorDigest[level]=RISECBOR64::SHA256Hex(firstDonorBytes);
							std::fprintf(stderr,"MANIFOLD_ALL_LOW_ORDER_WITNESS level=%zu pass=%u "
								"line=%zu donor=%zu cell=%zu temperature=%.17g energy=%.17g "
								"lower_energy=%.17g tolerance=%.17g domain_rejected=1\n",
								level,pass,line,
								donor,allLowOrderFirstCell[level],donorTemperature,
								donorState[MethaneMassStateDimension],
								lowerEnergy,endpointTolerance);}
						std::array<double,MethaneSpeciesCount> donorEnthalpy;
						if(donorTemperature<=fuel.TemperatureMinK())
							donorEnthalpy=lowerEndpointEnthalpy;
						else if(donorTemperature>=fuel.TemperatureMaxK())
							donorEnthalpy=upperEndpointEnthalpy;
						else if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(donorTemperature,
							donorEnthalpy.data(),donorEnthalpy.size(),&error))return 225;
						double fluxEnergy=0.0;for(std::size_t species=0u;
							species<MethaneSpeciesCount;++species)fluxEnergy+=
								allLowOrderResult.faceFluxes[((species+1u)*lines+line)*
									(length+1u)+face]*donorEnthalpy[species];
						allLowEnergyFlux[fluxBase]=static_cast<float>(fluxEnergy);}
					if(allLowPeriodic)for(std::size_t component=0u;component<9u;++component)
						for(std::size_t line=0u;line<lines;++line)
							allLowOrderResult.faceFluxes[(component*lines+line)*(length+1u)+length]=
								allLowOrderResult.faceFluxes[(component*lines+line)*(length+1u)];
					if(allLowPeriodic)for(std::size_t line=0u;line<lines;++line)
						allLowEnergyFlux[line*(length+1u)+length]=
							allLowEnergyFlux[line*(length+1u)];
					double allLowEnergyBefore=0.0,allLowBoundaryFluxDifference=0.0;
					for(const float value:allLowOrderEnergy)allLowEnergyBefore+=value;
					for(std::size_t line=0u;line<lines;++line)
						allLowBoundaryFluxDifference+=static_cast<double>(
							allLowEnergyFlux[line*(length+1u)+length])-static_cast<double>(
								allLowEnergyFlux[line*(length+1u)]);
					for(std::size_t line=0u;line<lines;++line)for(std::size_t coordinate=0u;
						coordinate<length;++coordinate){std::size_t x=0u,y=0u,z=0u;
						coordinates(axis,line,coordinate,x,y,z);const std::size_t cell=cellIndex(x,y,z);
						allLowNextEnergy[cell]=allLowOrderEnergy[cell]-
							(allLowEnergyFlux[line*(length+1u)+coordinate+1u]-
								allLowEnergyFlux[line*(length+1u)+coordinate])/
									allLowOrderRequest.cellWidthM;
						for(std::size_t component=0u;component<8u;++component){
							const std::size_t value=(component*lines+line)*length+coordinate;
							allLowOrderTransported[component*cells+cell]=
								allLowOrderRequest.values[value]-
								(allLowOrderResult.faceFluxes[(component*lines+line)*
									(length+1u)+coordinate+1u]-allLowOrderResult.faceFluxes[
										(component*lines+line)*(length+1u)+coordinate])/
										allLowOrderRequest.cellWidthM;}}
					double allLowEnergyAfter=0.0;
					for(const float value:allLowNextEnergy)allLowEnergyAfter+=value;
					const double allLowExpectedEnergyAfter=allLowEnergyBefore-
						allLowBoundaryFluxDifference/allLowOrderRequest.cellWidthM;
					const double allLowLedgerResidual=std::fabs(allLowEnergyAfter-
						allLowExpectedEnergyAfter);
					allLowOrderMaximumEnergyLedgerResidual[level]=std::max(
						allLowOrderMaximumEnergyLedgerResidual[level],allLowLedgerResidual);
					allLowOrderMaximumEnergyLedgerRelative[level]=std::max(
						allLowOrderMaximumEnergyLedgerRelative[level],allLowLedgerResidual/
							std::max(1.0,std::max(std::fabs(allLowEnergyAfter),
								std::fabs(allLowExpectedEnergyAfter))));
					allLowOrderEnergy.swap(allLowNextEnergy);
					RISE::FireProductionRemapResult lineResult;
					if(!RISE::RemapFireProductionCPU(lineRequest,lineResult,&error))return 225;
					// Retained r154 high-order diagnostic.  Its temperature-scaled endpoint
					// census is historical only: r155 supersedes the inference with the
					// independent all-alpha-zero trajectory above and the composition-
					// dependent r60 energy-scale width.
					std::vector<float> alphaFraction(lines*length,1.0f),
						boundaryFaceFraction(lines*(length+1u),1.0f);
					std::vector<unsigned char> lowOrderLowerInfeasible(lines*length,0u),
						lowOrderUpperInfeasible(lines*length,0u);
					const bool periodic=lineRequest.lowerBoundary==RISE::FireProductionRemapPeriodic&&
						lineRequest.upperBoundary==RISE::FireProductionRemapPeriodic;
					auto donorCoordinate=[&](const std::size_t face,const float velocity,
						std::size_t& donor,bool& ambientDonor){ambientDonor=false;
						if(velocity>=0.0f){if(face>0u){donor=face-1u;return true;}
							if(periodic){donor=length-1u;return true;}
							ambientDonor=lineRequest.lowerBoundary==
								RISE::FireProductionRemapPressureOpen;return ambientDonor;}
						if(face<length){donor=face;return true;}
						if(periodic){donor=0u;return true;}
						ambientDonor=lineRequest.upperBoundary==
							RISE::FireProductionRemapPressureOpen;return ambientDonor;};
					auto donorValue=[&](const std::size_t component,const std::size_t line,
						const std::size_t donor,const bool ambientDonor){return ambientDonor?
						lineRequest.ambientValues[component]:lineRequest.values[
							(component*lines+line)*length+donor];};
					const double molarDensityMinimum=fuel.ThermodynamicPressurePa()/
						(8314.46261815324*fuel.TemperatureMaxK());
					const double molarDensityMaximum=fuel.ThermodynamicPressurePa()/
						(8314.46261815324*fuel.TemperatureMinK());
					const double endpointFactor=fuel.AcceptedStateFeasibilityEnvelope().
						kappaEpsilon32*std::numeric_limits<float>::epsilon();
					const double lowerEndpointExterior=fuel.TemperatureMinK()-endpointFactor*
						std::max(1.0,std::fabs(fuel.TemperatureMinK()));
					const double upperEndpointExterior=fuel.TemperatureMaxK()+endpointFactor*
						std::max(1.0,std::fabs(fuel.TemperatureMaxK()));
					for(std::size_t line=0u;line<lines;++line)for(std::size_t face=0u;
						face<(periodic?length:length+1u);++face){
						const std::size_t fluxBase=line*(length+1u)+face;
						const float velocity=lineRequest.faceVelocityMPerS[fluxBase];
						if(velocity==0.0f)continue;
						std::size_t donor=0u;bool ambientDonor=false;
						if(!donorCoordinate(face,velocity,donor,ambientDonor))continue;
						const double sweptLength=static_cast<double>(lineRequest.timeStepS)*
							static_cast<double>(velocity);
						double donorMolarDensity=0.0,faceMolarFlux=0.0;
						for(std::size_t species=0u;species<MethaneCarbon;++species){
							const FireThermochemistrySpecies* record=fuel.FindSpecies(
								fuel.SpeciesOrder()[species].c_str());if(!record)return 225;
							donorMolarDensity+=donorValue(species+1u,line,donor,ambientDonor)/
								record->molecularWeightKGPerKMol;
							faceMolarFlux+=lineResult.faceFluxes[((species+1u)*lines+line)*
								(length+1u)+face]/record->molecularWeightKGPerKMol;}
						const double faceMolarDensity=faceMolarFlux/sweptLength;
						double fraction=1.0;
						const double donorTemperature=(fuel.ThermodynamicPressurePa()/
							8314.46261815324)/donorMolarDensity;
						if(!ambientDonor&&pass==allLowOrderFirstPass[level]&&
							line==allLowOrderFirstLine[level]&&donor==allLowOrderFirstDonor[level]){
							greedyAtAllLowOrderFirstTemperature[level]=donorTemperature;
							RISECBOR64::Bytes donorBytes;
							for(std::size_t component=0u;component<8u;++component){
								const float value=donorValue(component,line,donor,false);
								std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
								FireProductionDyadicCalibration::AppendInteger(donorBytes,bits);}
							greedyAtAllLowOrderFirstDonorDigest[level]=
								RISECBOR64::SHA256Hex(donorBytes);}
						if(!ambientDonor&&donorTemperature<lowerEndpointExterior){
							lowOrderLowerInfeasible[line*length+donor]=1u;
							const double excursion=fuel.TemperatureMinK()-donorTemperature;
							if(excursion>reconstructionMaximumLowOrderLowerExcursion[level]){
								reconstructionMaximumLowOrderLowerExcursion[level]=excursion;
								greedyMaximumExcursionPass[level]=pass;
								greedyMaximumExcursionLine[level]=line;
								greedyMaximumExcursionDonor[level]=donor;
								RISECBOR64::Bytes donorBytes;
								for(std::size_t component=0u;component<8u;++component){
									const float value=donorValue(component,line,donor,false);
									std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
									FireProductionDyadicCalibration::AppendInteger(donorBytes,bits);}
								greedyMaximumExcursionDigest[level]=RISECBOR64::SHA256Hex(donorBytes);}
							fraction=0.0;}
						else if(!ambientDonor&&donorTemperature>upperEndpointExterior){
							lowOrderUpperInfeasible[line*length+donor]=1u;
							reconstructionMaximumLowOrderUpperExcursion[level]=std::max(
								reconstructionMaximumLowOrderUpperExcursion[level],
								donorTemperature-fuel.TemperatureMaxK());fraction=0.0;}
						else if(faceMolarDensity>molarDensityMaximum)
							fraction=(molarDensityMaximum-donorMolarDensity)/
								(faceMolarDensity-donorMolarDensity);
						else if(faceMolarDensity<molarDensityMinimum)
							fraction=(molarDensityMinimum-donorMolarDensity)/
								(faceMolarDensity-donorMolarDensity);
						fraction=std::max(0.0,std::min(1.0,fraction));
						float representedFraction=static_cast<float>(fraction);
						if(static_cast<double>(representedFraction)>fraction)
							representedFraction=std::nextafter(representedFraction,0.0f);
						if(ambientDonor)boundaryFaceFraction[fluxBase]=representedFraction;
						else alphaFraction[line*length+donor]=std::min(
							alphaFraction[line*length+donor],representedFraction);
					}
					for(const unsigned char value:lowOrderLowerInfeasible)
						reconstructionLowOrderLowerInfeasibleCount[level]+=value!=0u?1u:0u;
					for(const unsigned char value:lowOrderUpperInfeasible)
						reconstructionLowOrderUpperInfeasibleCount[level]+=value!=0u?1u:0u;
					for(std::size_t line=0u;line<lines;++line)for(std::size_t face=0u;
						face<(periodic?length:length+1u);++face){
						const std::size_t fluxBase=line*(length+1u)+face;
						const float velocity=lineRequest.faceVelocityMPerS[fluxBase];
						std::size_t donor=0u;bool ambientDonor=false;
						if(velocity==0.0f||!donorCoordinate(face,velocity,donor,ambientDonor))continue;
						const float fraction=ambientDonor?boundaryFaceFraction[fluxBase]:
							alphaFraction[line*length+donor];
						const float courant=lineRequest.timeStepS*velocity/
							lineRequest.cellWidthM;
						for(std::size_t component=0u;component<9u;++component){
							const std::size_t flux=(component*lines+line)*(length+1u)+face;
							const float lowOrderFlux=lineRequest.cellWidthM*(courant*
								donorValue(component,line,donor,ambientDonor));
							lineResult.faceFluxes[flux]=lowOrderFlux+fraction*
								(lineResult.faceFluxes[flux]-lowOrderFlux);}
					}
					if(periodic)for(std::size_t component=0u;component<9u;++component)
						for(std::size_t line=0u;line<lines;++line)
							lineResult.faceFluxes[(component*lines+line)*(length+1u)+length]=
								lineResult.faceFluxes[(component*lines+line)*(length+1u)];
					for(std::size_t line=0u;line<lines;++line)for(std::size_t coordinate=0u;
						coordinate<length;++coordinate){
						lineResult.sharedLimiterAlpha[line*length+coordinate]*=
							alphaFraction[line*length+coordinate];
						for(std::size_t component=0u;component<9u;++component){
							const std::size_t value=(component*lines+line)*length+coordinate;
							lineResult.updatedValues[value]=lineRequest.values[value]-
								(lineResult.faceFluxes[(component*lines+line)*(length+1u)+coordinate+1u]-
									lineResult.faceFluxes[(component*lines+line)*(length+1u)+coordinate])/
										lineRequest.cellWidthM;}}
					FireProductionDyadicCalibration::AppendInteger(reconstructionTrace,pass);
					FireProductionDyadicCalibration::AppendInteger(reconstructionTrace,
						lineResult.sharedLimiterAlpha.size());
					for(const float alpha:lineResult.sharedLimiterAlpha){std::uint32_t bits=0u;
						std::memcpy(&bits,&alpha,sizeof(bits));
						FireProductionDyadicCalibration::AppendInteger(reconstructionTrace,bits);}
					std::vector<float> energyFlux(lines*(length+1u),0.0f),nextEnergy=energy;
					for(std::size_t line=0u;line<lines;++line)for(std::size_t face=0u;face<=length;++face){
						const std::size_t fluxBase=line*(length+1u)+face;
						double molarFlux=0.0;
						for(std::size_t species=0u;species<MethaneCarbon;++species){
							const FireThermochemistrySpecies* record=fuel.FindSpecies(
								fuel.SpeciesOrder()[species].c_str());if(!record)return 225;
							molarFlux+=lineResult.faceFluxes[((species+1u)*lines+line)*
								(length+1u)+face]/record->molecularWeightKGPerKMol;}
						// The fixed-pressure auxiliary is algebraic.  Its exact face integral is
						// (P/R)*dt*u for this sub-cell-Courant sweep; derive it independently
						// instead of inheriting the rounded constant-tracer flux.
						const double sweptLength=static_cast<double>(steps[pass])*static_cast<double>(
							lineRequest.faceVelocityMPerS[fluxBase]);
						const double qFlux=(fuel.ThermodynamicPressurePa()/8314.46261815324)*
							sweptLength;
						if(molarFlux==0.0&&qFlux==0.0)continue;
						if(molarFlux==0.0)return 225;
						double temperature=qFlux/molarFlux;
						if(!std::isfinite(temperature)||temperature<=0.0){std::fprintf(stderr,
								"MANIFOLD_RECONSTRUCTION_TEMPERATURE level=%zu pass=%u line=%zu "
								"face=%zu molar_flux=%.17g nt_flux=%.17g temperature=%.17g\n",
								level,pass,line,face,molarFlux,qFlux,temperature);return 225;}
						if(temperature<fuel.TemperatureMinK()){
							reconstructionMaximumLowerEndpointProjection[level]=std::max(
								reconstructionMaximumLowerEndpointProjection[level],
								fuel.TemperatureMinK()-temperature);
							temperature=fuel.TemperatureMinK();
							++reconstructionLowerEndpointProjectionCount[level];}
						else if(temperature>fuel.TemperatureMaxK()){
							reconstructionMaximumUpperEndpointProjection[level]=std::max(
								reconstructionMaximumUpperEndpointProjection[level],
								temperature-fuel.TemperatureMaxK());
							temperature=fuel.TemperatureMaxK();
							++reconstructionUpperEndpointProjectionCount[level];}
						std::array<double,MethaneSpeciesCount> enthalpy;
						if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(temperature,
							enthalpy.data(),enthalpy.size(),&error))return 225;
						double fluxEnergy=0.0;for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							fluxEnergy+=lineResult.faceFluxes[((species+1u)*lines+line)*
								(length+1u)+face]*enthalpy[species];
						energyFlux[fluxBase]=static_cast<float>(fluxEnergy);
						ConservativeVector faceState{};
						for(std::size_t component=0u;component<8u;++component)
							faceState[component]=lineResult.faceFluxes[(component*lines+line)*
								(length+1u)+face]/sweptLength;
						faceState[MethaneMassStateDimension]=energyFlux[fluxBase]/sweptLength;
						std::string faceError;
						if(!AcceptedStateAdmissible(faceState,lowerEndpointEnthalpy,
							upperEndpointEnthalpy,fuel,FireStateProducerPrecision::Binary32,
							&faceError)){std::fprintf(stderr,"MANIFOLD_RECONSTRUCTION_FACE_REJECTED "
								"level=%zu pass=%u line=%zu face=%zu temperature=%.17g: %s\n",
								level,pass,line,face,temperature,faceError.c_str());return 225;}
						++reconstructionValidatedFaceCount[level];}
					FireProductionDyadicCalibration::AppendInteger(reconstructionTrace,
						energyFlux.size());
					for(const float flux:energyFlux){std::uint32_t bits=0u;
						std::memcpy(&bits,&flux,sizeof(bits));
						FireProductionDyadicCalibration::AppendInteger(reconstructionTrace,bits);}
					double energyBefore=0.0,boundaryFluxDifference=0.0;
					for(const float value:energy)energyBefore+=value;
					for(std::size_t line=0u;line<lines;++line)
						boundaryFluxDifference+=static_cast<double>(
							energyFlux[line*(length+1u)+length])-static_cast<double>(
								energyFlux[line*(length+1u)]);
					for(std::size_t line=0u;line<lines;++line)for(std::size_t coordinate=0u;
						coordinate<length;++coordinate){std::size_t x=0u,y=0u,z=0u;
						coordinates(axis,line,coordinate,x,y,z);const std::size_t cell=cellIndex(x,y,z);
						nextEnergy[cell]=energy[cell]-(energyFlux[line*(length+1u)+coordinate+1u]-
							energyFlux[line*(length+1u)+coordinate])/
								sweepRequest.cellTransport.shape.cellWidthM;
						for(std::size_t component=0u;component<8u;++component)
							transported[component*cells+cell]=lineResult.updatedValues[
								(component*lines+line)*length+coordinate];}
					for(std::size_t cell=0u;cell<cells;++cell){ConservativeVector passState{};
						for(std::size_t component=0u;component<8u;++component)
							passState[component]=transported[component*cells+cell];
						passState[MethaneMassStateDimension]=nextEnergy[cell];
						std::string passError;
						if(!AcceptedStateAdmissible(passState,lowerEndpointEnthalpy,
							upperEndpointEnthalpy,fuel,FireStateProducerPrecision::Binary32,
							&passError)){std::fprintf(stderr,"MANIFOLD_RECONSTRUCTION_PASS_REJECTED "
								"level=%zu pass=%u cell=%zu: %s\n",level,pass,cell,
								passError.c_str());return 225;}
						++reconstructionValidatedPassCellCount[level];}
					double energyAfter=0.0;for(const float value:nextEnergy)energyAfter+=value;
					const double ledgerResidual=std::fabs(energyAfter-(energyBefore-
						boundaryFluxDifference/sweepRequest.cellTransport.shape.cellWidthM));
					reconstructionMaximumEnergyLedgerResidual[level]=std::max(
						reconstructionMaximumEnergyLedgerResidual[level],ledgerResidual);
					const double expectedEnergyAfter=energyBefore-boundaryFluxDifference/
						sweepRequest.cellTransport.shape.cellWidthM;
					reconstructionMaximumEnergyLedgerRelative[level]=std::max(
						reconstructionMaximumEnergyLedgerRelative[level],ledgerResidual/
							std::max(1.0,std::max(std::fabs(energyAfter),
								std::fabs(expectedEnergyAfter))));
					energy.swap(nextEnergy);
				}
				for(std::size_t cell=0u;cell<cells;++cell){
					for(std::size_t component=0u;component<8u;++component)
						reconstructed[component*cells+cell]=transported[component*cells+cell];
					reconstructed[8u*cells+cell]=energy[cell];
				}
				RISECBOR64::Bytes reconstructionFieldBytes,productionFieldBytes;
				for(std::size_t value=0u;value<reconstructed.size();++value){
					std::uint32_t reconstructedBits=0u,productionBits=0u;
					std::memcpy(&reconstructedBits,&reconstructed[value],sizeof(reconstructedBits));
					std::memcpy(&productionBits,&measured.conservativeValues[value],
						sizeof(productionBits));
					FireProductionDyadicCalibration::AppendInteger(reconstructionFieldBytes,
						reconstructedBits);
					FireProductionDyadicCalibration::AppendInteger(productionFieldBytes,
						productionBits);
					reconstructionProductionMaximumDifference[level]=std::max(
						reconstructionProductionMaximumDifference[level],std::fabs(
							static_cast<double>(reconstructed[value])-static_cast<double>(
								measured.conservativeValues[value])));
				}
				reconstructionFieldDigest[level]=RISECBOR64::SHA256Hex(reconstructionFieldBytes);
				reconstructionTraceDigest[level]=RISECBOR64::SHA256Hex(reconstructionTrace);
				productionFieldDigest[level]=RISECBOR64::SHA256Hex(productionFieldBytes);
				if(reconstructionFieldDigest[level]==productionFieldDigest[level]||
					reconstructionProductionMaximumDifference[level]==0.0)return 225;
				for(std::size_t cell=0u;cell<cells;++cell){
					ConservativeVector rebuilt{};
					for(std::size_t component=0u;component<9u;++component)
						rebuilt[component]=static_cast<double>(
							reconstructed[component*cells+cell]);
					double rebuiltRatio=0.0;if(!AcceptedConservativeVolumeRatio(rebuilt,fuel,
						FireStateProducerPrecision::Binary32,rebuiltRatio,&error))return 225;
					const double reconstructionDelta=std::fabs((rebuiltRatio-1.0)-
						sweepRequest.beginningManifoldDeviationPerCell[cell]);
					if(reconstructionDelta>reconstructionGeneration[level]){
						reconstructionGeneration[level]=reconstructionDelta;
						reconstructionCell[level]=cell;
						reconstructionBeginningDeviation[level]=
							sweepRequest.beginningManifoldDeviationPerCell[cell];
						reconstructionTerminalDeviation[level]=rebuiltRatio-1.0;
					}
				}
				RISECBOR64::Bytes allLowOrderFieldBytes;
				for(std::size_t cell=0u;cell<cells;++cell){
					ConservativeVector allLowOrderState{};
					for(std::size_t component=0u;component<8u;++component)
						allLowOrderState[component]=allLowOrderTransported[component*cells+cell];
					allLowOrderState[8]=allLowOrderEnergy[cell];
					for(std::size_t component=0u;component<9u;++component){
						const float represented=static_cast<float>(allLowOrderState[component]);
						std::uint32_t bits=0u;std::memcpy(&bits,&represented,sizeof(bits));
						FireProductionDyadicCalibration::AppendInteger(allLowOrderFieldBytes,bits);}
					double allLowOrderRatio=0.0;
					if(!AcceptedConservativeVolumeRatio(allLowOrderState,fuel,
						FireStateProducerPrecision::Binary32,allLowOrderRatio,&error))return 225;
					const double allLowOrderDelta=std::fabs((allLowOrderRatio-1.0)-
						sweepRequest.beginningManifoldDeviationPerCell[cell]);
					if(allLowOrderDelta>allLowOrderGeneration[level]){
						allLowOrderGeneration[level]=allLowOrderDelta;allLowOrderCell[level]=cell;
						allLowOrderBeginningDeviation[level]=
							sweepRequest.beginningManifoldDeviationPerCell[cell];
						allLowOrderTerminalDeviation[level]=allLowOrderRatio-1.0;}}
				allLowOrderFieldDigest[level]=RISECBOR64::SHA256Hex(allLowOrderFieldBytes);
			}
			std::array<std::array<double,3>,2> exponent={{{{0.0,0.0,0.0}},{{0.0,0.0,0.0}}}};
			for(std::size_t interval=0u;interval<2u;++interval)
				for(std::size_t stage=0u;stage<3u;++stage)
					exponent[interval][stage]=stageGeneration[interval+1u][stage]>0.0?
						std::log2(stageGeneration[interval][stage]/stageGeneration[interval+1u][stage]):0.0;
			for(std::size_t level=0u;level<3u;++level)std::fprintf(stderr,
				"MANIFOLD_STAGE_BUDGET level=%zu dt=%.17g remap=%.17g physical=%.17g "
				"restoration=%.17g independent=%.17g reconstruction=%.17g "
				"reconstruction_cell=%zu reconstruction_beginning=%.17g "
				"reconstruction_terminal=%.17g reconstruction_production_max=%.17g "
				"energy_ledger_residual=%.17g energy_ledger_relative=%.17g "
				"lower_endpoint_projections=%zu upper_endpoint_projections=%zu "
				"maximum_lower_endpoint_projection=%.17g "
				"maximum_upper_endpoint_projection=%.17g "
				"low_order_lower_infeasible=%zu low_order_upper_infeasible=%zu "
				"validated_faces=%zu validated_pass_cells=%zu "
				"maximum_low_order_lower_excursion=%.17g "
				"maximum_low_order_upper_excursion=%.17g "
				"field_digest=%s production_digest=%s trace_digest=%s "
				"device_ms=%.17g wall_ms=%.17g\n",level,
				dt/std::pow(2.0,level),stageGeneration[level][0],stageGeneration[level][1],
				stageGeneration[level][2],independentGeneration[level],
				reconstructionGeneration[level],reconstructionCell[level],
				reconstructionBeginningDeviation[level],reconstructionTerminalDeviation[level],
				reconstructionProductionMaximumDifference[level],
				reconstructionMaximumEnergyLedgerResidual[level],
				reconstructionMaximumEnergyLedgerRelative[level],
				reconstructionLowerEndpointProjectionCount[level],
				reconstructionUpperEndpointProjectionCount[level],
				reconstructionMaximumLowerEndpointProjection[level],
				reconstructionMaximumUpperEndpointProjection[level],
				reconstructionLowOrderLowerInfeasibleCount[level],
				reconstructionLowOrderUpperInfeasibleCount[level],
				reconstructionValidatedFaceCount[level],
				reconstructionValidatedPassCellCount[level],
				reconstructionMaximumLowOrderLowerExcursion[level],
				reconstructionMaximumLowOrderUpperExcursion[level],
				reconstructionFieldDigest[level].c_str(),
				productionFieldDigest[level].c_str(),
				reconstructionTraceDigest[level].c_str(),
				deviceMS[level],wallMS[level]);
			for(std::size_t level=0u;level<3u;++level)std::fprintf(stderr,
				"MANIFOLD_ALL_LOW_ORDER level=%zu G=%.17g cell=%zu beginning=%.17g "
				"terminal=%.17g endpoint_projections=%zu "
				"max_endpoint_excursion_K=%.17g energy_ledger_residual=%.17g "
				"energy_ledger_relative=%.17g first_outside_pass=%zu "
				"first_line=%zu first_donor=%zu first_temperature=%.17g first_digest=%s "
				"greedy_first_temperature=%.17g greedy_first_digest=%s "
				"max_pass=%zu max_line=%zu max_donor=%zu max_donor_digest=%s greedy_max_pass=%zu "
				"greedy_max_line=%zu greedy_max_donor=%zu greedy_max_donor_digest=%s "
				"field_digest=%s\n",
				level,allLowOrderGeneration[level],allLowOrderCell[level],
				allLowOrderBeginningDeviation[level],allLowOrderTerminalDeviation[level],
				allLowOrderEndpointProjectionCount[level],
				allLowOrderMaximumEndpointExcursion[level],
				allLowOrderMaximumEnergyLedgerResidual[level],
				allLowOrderMaximumEnergyLedgerRelative[level],allLowOrderFirstPass[level],
				allLowOrderFirstLine[level],allLowOrderFirstDonor[level],
				allLowOrderFirstTemperature[level],allLowOrderFirstDonorDigest[level].c_str(),
				greedyAtAllLowOrderFirstTemperature[level],
				greedyAtAllLowOrderFirstDonorDigest[level].c_str(),
				allLowOrderMaximumExcursionPass[level],allLowOrderMaximumExcursionLine[level],
				allLowOrderMaximumExcursionDonor[level],
				allLowOrderMaximumExcursionDigest[level].c_str(),
				greedyMaximumExcursionPass[level],greedyMaximumExcursionLine[level],
				greedyMaximumExcursionDonor[level],greedyMaximumExcursionDigest[level].c_str(),
				allLowOrderFieldDigest[level].c_str());
			std::fprintf(stderr,"MANIFOLD_STAGE_BUDGET exponent remap=(%.17g,%.17g) "
				"physical=(%.17g,%.17g) restoration=(%.17g,%.17g) golden=%d\n",
				exponent[0][0],exponent[1][0],exponent[0][1],exponent[1][1],
				exponent[0][2],exponent[1][2],DigestFile(checkpointPath)==checkpointDigest?1:0);
			const bool remapDecision=stageGeneration[0][0]==0.0025327801704406738&&
				stageGeneration[1][0]==0.0025155544281005859&&
				stageGeneration[2][0]==0.0025067925453186035&&
				stageGeneration[0][1]==0.0&&stageGeneration[0][2]==0.0&&
				independentGeneration[0]==0.0025328069638265172&&
				independentGeneration[1]==0.0025155729299433105&&
				independentGeneration[2]==0.0025068855498342479&&
				allLowOrderGeneration[0]==0.0025328069638265172&&
				allLowOrderGeneration[1]==0.0025155729299433105&&
				allLowOrderGeneration[2]==0.0025068855498342479&&
				allLowOrderCell[0]==3227u&&allLowOrderCell[1]==3227u&&
				allLowOrderCell[2]==3227u&&
				allLowOrderBeginningDeviation[0]==-1.1871614802316799e-12&&
				allLowOrderTerminalDeviation[0]==-0.0025328069650136786&&
				allLowOrderEndpointProjectionCount[0]==2532883u&&
				allLowOrderEndpointProjectionCount[1]==2585162u&&
				allLowOrderEndpointProjectionCount[2]==2665212u&&
				allLowOrderMaximumEndpointExcursion[0]==0.3536001375753699&&
				allLowOrderMaximumEndpointExcursion[1]==0.17615370759477855&&
				allLowOrderMaximumEndpointExcursion[2]==0.087340369030073362&&
				allLowOrderMaximumEnergyLedgerResidual[0]==0.34937524795532227&&
				allLowOrderMaximumEnergyLedgerResidual[1]==0.2306828498840332&&
				allLowOrderMaximumEnergyLedgerResidual[2]==0.48347091674804688&&
				allLowOrderMaximumEnergyLedgerRelative[0]==1.0231602153538168e-10&&
				allLowOrderMaximumEnergyLedgerRelative[1]==6.7554416725572504e-11&&
				allLowOrderMaximumEnergyLedgerRelative[2]==1.4158011510440571e-10&&
				allLowOrderFirstPass[0]==1u&&allLowOrderFirstPass[1]==1u&&
				allLowOrderFirstPass[2]==1u&&
				allLowOrderFirstLine[0]==0u&&allLowOrderFirstDonor[0]==0u&&
				allLowOrderFirstCell[0]==0u&&
				allLowOrderFirstTemperature[0]==299.99999426911722&&
				allLowOrderFirstTolerance[0]==845.8114886122496&&
				allLowOrderFirstDonorDigest[0]==
					"5a48fa156dedcef3aea0d60c0fdfd0144e6354cdd3ea342cd12200378266659a"&&
				greedyAtAllLowOrderFirstTemperature[0]==299.99999426911722&&
				greedyAtAllLowOrderFirstDonorDigest[0]==allLowOrderFirstDonorDigest[0]&&
				allLowOrderMaximumExcursionPass[0]==4u&&
				allLowOrderMaximumExcursionLine[0]==9337u&&
				allLowOrderMaximumExcursionDonor[0]==28u&&
				allLowOrderMaximumExcursionDigest[0]==
					"eb5428f27ab1bd9e2ad5e272d09ebe9600dd0b644ff49e09db86039ffee947ed"&&
				greedyMaximumExcursionPass[0]==4u&&greedyMaximumExcursionLine[0]==9337u&&
				greedyMaximumExcursionDonor[0]==28u&&
				greedyMaximumExcursionDigest[0]==allLowOrderMaximumExcursionDigest[0]&&
				allLowOrderFieldDigest[0]==
					"d4df6114047f27a79bc807ac68ed69d946dfe20a755ae934a91bc5dd013e470b"&&
				allLowOrderFieldDigest[1]==
					"cc81e2b16fd40466d51b973db65efe43d1d050af3178bdfd265afb2ab7d56d59"&&
				allLowOrderFieldDigest[2]==
					"8b79b507626d8bf9baed7db839b25cf51129a17d4c8cc3e2b7fc60e5030e0c87"&&
				reconstructionGeneration[0]==0.0025328069638265172&&
				reconstructionGeneration[1]==0.0025155729299433105&&
				reconstructionGeneration[2]==0.0025068855498342479&&
				reconstructionCell[0]==3227u&&reconstructionCell[1]==3227u&&
				reconstructionCell[2]==3227u&&
				reconstructionBeginningDeviation[0]==-1.1871614802316799e-12&&
				reconstructionTerminalDeviation[0]==-0.0025328069650136786&&
				reconstructionProductionMaximumDifference[0]==308.03125&&
				reconstructionProductionMaximumDifference[1]==153.9375&&
				reconstructionProductionMaximumDifference[2]==76.9375&&
				reconstructionMaximumEnergyLedgerResidual[0]==0.31164741516113281&&
				reconstructionMaximumEnergyLedgerResidual[1]==0.19146203994750977&&
				reconstructionMaximumEnergyLedgerResidual[2]==0.47876596450805664&&
				reconstructionMaximumEnergyLedgerRelative[0]==9.1267265857582786e-11&&
				reconstructionMaximumEnergyLedgerRelative[1]==5.6067095289423908e-11&&
				reconstructionMaximumEnergyLedgerRelative[2]==1.4020231210267571e-10&&
				reconstructionLowerEndpointProjectionCount[0]==2307844u&&
				reconstructionLowerEndpointProjectionCount[1]==2326957u&&
				reconstructionLowerEndpointProjectionCount[2]==2356525u&&
				reconstructionUpperEndpointProjectionCount[0]==0u&&
				reconstructionUpperEndpointProjectionCount[1]==0u&&
				reconstructionUpperEndpointProjectionCount[2]==0u&&
				reconstructionMaximumLowerEndpointProjection[0]==0.35360660028368329&&
				reconstructionMaximumLowerEndpointProjection[1]==0.17614886943493957&&
				reconstructionMaximumLowerEndpointProjection[2]==0.087340680831175632&&
				reconstructionMaximumUpperEndpointProjection[0]==0.0&&
				reconstructionMaximumUpperEndpointProjection[1]==0.0&&
				reconstructionMaximumUpperEndpointProjection[2]==0.0&&
				reconstructionLowOrderLowerInfeasibleCount[0]==30381u&&
				reconstructionLowOrderLowerInfeasibleCount[1]==4732u&&
				reconstructionLowOrderLowerInfeasibleCount[2]==130u&&
				reconstructionLowOrderUpperInfeasibleCount[0]==0u&&
				reconstructionLowOrderUpperInfeasibleCount[1]==0u&&
				reconstructionLowOrderUpperInfeasibleCount[2]==0u&&
				reconstructionValidatedFaceCount[0]==4926768u&&
				reconstructionValidatedFaceCount[1]==4926768u&&
				reconstructionValidatedFaceCount[2]==4926768u&&
				reconstructionValidatedPassCellCount[0]==4881360u&&
				reconstructionValidatedPassCellCount[1]==4881360u&&
				reconstructionValidatedPassCellCount[2]==4881360u&&
				reconstructionMaximumLowOrderLowerExcursion[0]==0.3536001375753699&&
				reconstructionMaximumLowOrderLowerExcursion[1]==0.17615370759477855&&
				reconstructionMaximumLowOrderLowerExcursion[2]==0.087340369030073362&&
				reconstructionMaximumLowOrderUpperExcursion[0]==0.0&&
				reconstructionMaximumLowOrderUpperExcursion[1]==0.0&&
				reconstructionMaximumLowOrderUpperExcursion[2]==0.0&&
				reconstructionFieldDigest[0]==
					"77ea23b2d9b390dc50ce0f873edc7bea9af62db9def7ae127fd40e7a6169ac04"&&
				reconstructionFieldDigest[1]==
					"5ebbbeb2283d1342a4bf957e80f95410d36dd558092b74768a4686446b4cd8d1"&&
				reconstructionFieldDigest[2]==
					"114d116e156abbb1a45d5a96e257f3a8cc0055f4239df09bdc849e6be9c4e553"&&
				productionFieldDigest[0]==
					"0d00decc071ff85108435d59f8118a1ab2daba9e2b9f5c8c7cc6a77344383b8c"&&
				productionFieldDigest[1]==
					"26d16d971c825fbfe5e146a65d3cbd88b2773467b3f875a73a8edf99c799c690"&&
				productionFieldDigest[2]==
					"41d0a4170d42a52185ba2ebe69f631e917f1f21bb7843fdd7ae5652305fafd4b"&&
				reconstructionTraceDigest[0]==
					"f09eebb2f0f329699928d9d1c5647b367f909dfb557301b1d44c7a073694cd90"&&
				reconstructionTraceDigest[1]==
					"a857c93c947a7c2c03648440617f45ca842f294abfbe3284c8d23a22d8674e7a"&&
				reconstructionTraceDigest[2]==
					"49510fe3c962d453e2c78c7434322b038688be35096abd5adc171db3ea599dc0"&&
				exponent[0][0]==0.0098454605227652776&&
				exponent[1][0]==0.0050337970393511964;
			return remapDecision&&DigestFile(checkpointPath)==checkpointDigest?245:226;
		}
		if(manifoldProbe){
			if(slice!=0u)return 255;
			double selectedStep=0.0;
			if(!RISE::DeriveFireProductionManifoldTimeStep(dt,0.0025328069638265172,
				0.9533406144549903,selectedStep,&error))return 255;
			const float representedStep=static_cast<float>(selectedStep);
			request.force.timeStepS=representedStep;
			request.cellTransport.timeStepS=representedStep;
			request.dualTransport.timeStepS=representedStep;
			shadowConfig.transport.deltaTimeS=representedStep;shadowConfig.workerCount=16u;
			ConservativeAdvance3DResult limitedOracle;
			if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
				shadowConfig,fuel,fuel,transport,limitedOracle,&error))return 255;
			for(std::size_t cell=0u;cell<cells;++cell){
				request.divergenceTargetPerS[cell]=static_cast<float>(
					limitedOracle.divergenceHeunPerS[cell]);
				double volumeRatio=0.0;
				if(!AcceptedConservativeVolumeRatio(ToConservativeVector(beginning.states[cell]),
					fuel,beginning.states[cell].producerPrecision,volumeRatio,&error))return 255;
				request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
					(volumeRatio-1.0)/static_cast<double>(representedStep));
			}
			request.enforceManifoldPlateau=true;
			std::array<double,5> wall={{}},device={{}};
			RISE::FireProductionResidentStepResult limited;
			for(std::size_t trial=0u;trial<=wall.size();++trial){
				const auto start=std::chrono::steady_clock::now();
				if(!RISE::AdvanceFireProductionResidentStepMetal(request,limited,&error)){
					std::fprintf(stderr,"MANIFOLD_TIMESTEP failed trial=%zu error=%s\n",
						trial,error.c_str());return 255;}
				if(trial>0u){wall[trial-1u]=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-start).count();
				device[trial-1u]=limited.deviceElapsedMS;}
			}
			std::sort(wall.begin(),wall.end());std::sort(device.begin(),device.end());
			std::vector<ConservativeVector> limitedConservative;
			double independentGeneration=0.0,independentField=0.0;
			if(!FireProductionDyadicCalibration::UnpackProductionConservative(
				limited.conservativeValues,cells,limitedConservative))return 258;
			for(std::size_t cell=0u;cell<cells;++cell){
				double beginningRatio=0.0,terminalRatio=0.0;
				if(!AcceptedConservativeVolumeRatio(ToConservativeVector(beginning.states[cell]),
					fuel,beginning.states[cell].producerPrecision,beginningRatio,&error)||
					!AcceptedConservativeVolumeRatio(limitedConservative[cell],fuel,
						FireStateProducerPrecision::Binary32,terminalRatio,&error))return 258;
				independentGeneration=std::max(independentGeneration,std::fabs(
					(terminalRatio-1.0)-(beginningRatio-1.0)));
				independentField=std::max(independentField,std::fabs(terminalRatio-1.0));
			}
			auto setEnvironment=[](const char* name,const std::string& value){
#if defined(_WIN32)
				return _putenv_s(name,value.c_str())==0;
#else
				return setenv(name,value.c_str(),1)==0;
#endif
			};
			auto clearEnvironment=[](const char* name){
#if defined(_WIN32)
				return _putenv_s(name,"")==0;
#else
				return unsetenv(name)==0;
#endif
			};
			auto projectionDefault=[](const RISE::FireProductionProjectionResult& value){
				bool empty=value.pressurePa.empty();
				for(unsigned int axis=0u;axis<3u;++axis)empty=empty&&
					value.faceDensityKGPerM3[axis].empty()&&value.velocityMPerS[axis].empty()&&
					value.momentumKGPerM2S[axis].empty();
				for(const auto& side:value.pressureOpenInflow)empty=empty&&side.empty();
				return empty&&value.maximumPreProjectionResidualPerS==0.0f&&
					value.maximumPostProjectionResidualPerS==0.0f&&
					value.maximumOpenComplementarityDiscrepancyMPerS==0.0f&&
					value.removedFineRightHandSideMean==0.0f&&value.executedVCycleCount==0u&&
					value.executedJacobiSweepCount==0u&&value.residentUploadStagingCount==0u&&
					value.residentInterstageDeviceToHostTransferCount==0u&&
					value.residentTerminalStagingCount==0u&&value.residentCommandCommitCount==0u&&
					value.residentProjectionInvocationCount==0u&&
					value.residentCertifiedWorkingSetBytes==0u&&
					value.residentActualMetalAllocationBytes==0u&&!value.validationPassed&&
					value.deviceElapsedMS==0.0&&value.deviceStartTimeS==0.0&&
					value.deviceEndTimeS==0.0;
			};
			auto residentStepDefault=[&](const RISE::FireProductionResidentStepResult& value){
				bool dualEmpty=true;for(unsigned int axis=0u;axis<3u;++axis)dualEmpty=dualEmpty&&
					value.transportedDual.auxiliaryFaceDensity[axis].empty()&&
					value.transportedDual.momentum[axis].empty();
				return value.conservativeValues.empty()&&dualEmpty&&
					value.transportedDual.executedSubmapCount==0u&&
					value.transportedDual.canonicalSeamCopyCount==0u&&
					value.transportedDual.commandCommitCount==0u&&
					value.transportedDual.interstageFullGridTransferCount==0u&&
					value.transportedDual.actualMetalAllocationBytes==0u&&
					value.transportedDual.deviceElapsedMS==0.0&&projectionDefault(value.physicalProjection)&&
					projectionDefault(value.projection)&&value.forceSchedule.substepCount==0u&&
					value.forceSchedule.substepTimeS==0.0f&&value.forceSchedule.outwardWork==0.0&&
					value.forceSchedule.representedProductUpper==0.0&&
					value.forceDiagnostics.outwardLambdaPerS==0.0f&&
					value.forceDiagnostics.scalarDiagnosticTransferCount==0u&&
					value.forceDiagnostics.substepLoopDeviceToHostTransferCount==0u&&
					value.forceDiagnostics.terminalStagingCount==0u&&
					value.forceDiagnostics.commandCommitCount==0u&&
					value.forceDiagnostics.certifiedWorkingSetBytes==0u&&
					value.forceDiagnostics.actualMetalAllocationBytes==0u&&
					value.forceDiagnostics.preflightDeviceElapsedMS==0.0&&
					value.forceDiagnostics.advanceDeviceElapsedMS==0.0&&
					value.forceDiagnostics.deviceStartTimeS==0.0&&
					value.forceDiagnostics.deviceEndTimeS==0.0&&
					value.cellSubmapCount==0u&&value.dualSubmapCount==0u&&
					value.sourceCommandCommitCount==0u&&value.residentProjectionInvocationCount==0u&&
					value.interstageFullGridTransferCount==0u&&value.terminalStagingCount==0u&&
					value.combinedCertifiedWorkingSetBytes==0u&&
					value.combinedActualMetalAllocationBytes==0u&&value.deviceElapsedMS==0.0&&
					value.deviceMakespanMS==0.0&&
					value.representedTimeStepS==0.0f&&
					value.maximumManifoldGeneration==0.0&&value.maximumAcceptedManifoldDeviation==0.0&&
					value.acceptedManifoldDeviationP95==0.0&&
					value.acceptedManifoldDeviationP50==0.0&&
					!value.manifoldGenerationAuthoritative&&
					value.manifoldMapCellCount==0u&&
					value.manifoldScalarDeviceToHostTransferCount==0u&&
					value.manifoldFullGridDeviceToHostTransferCount==0u&&
					value.manifoldStageGeneration[0]==0.0&&
					value.manifoldStageGeneration[1]==0.0&&
					value.manifoldStageGeneration[2]==0.0&&
					value.requiredRestorationDrainFraction==0.0&&
					value.deliveredRestorationDrainFraction==0.0&&
					value.restorationResidualBandPerS==0.0&&
					value.suggestedManifoldTimeStepS==0.0&&
					!value.manifoldNextTimeStepAvailable&&
					!value.manifoldDiagnosticsMonitored&&!value.manifoldPlateauEnforced&&
					!value.manifoldAllowanceExceeded&&!value.manifoldCeilingExceeded&&
					!value.manifoldPlateauPassed&&
					!value.HasAcceptedManifoldToken()&&
					value.conservativeProducerPrecision==FireStateProducerPrecision::Unknown;
			};
			RISE::FireProductionResidentStepResult rejected=limited;
			if(!clearEnvironment("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE"))return 258;
			error.clear();
			const bool unexpectedlyAccepted=RISE::AdvanceFireProductionResidentStepMetal(
				request,rejected,&error);
			if(!setEnvironment("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE","1"))return 258;
			const bool atomicRejection=!unexpectedlyAccepted&&residentStepDefault(rejected)&&
				error.find("production manifold generation exceeds the accepted-step allowance")!=
					std::string::npos;
			std::fprintf(stderr,"MANIFOLD_TIMESTEP cfl_dt=%.17g derived_dt=%.17g "
				"represented_dt=%.17g tightening=%.17g G=%.17g field=%.17g "
				"required=%.17g delivered=%.17g band=%.17g pre=%.17g post=%.17g "
				"mechanism=%d plateau=%d device_p95_ms=%.17g wall_p95_ms=%.17g\n",
				dt,selectedStep,static_cast<double>(representedStep),dt/representedStep,
				limited.maximumManifoldGeneration,limited.maximumAcceptedManifoldDeviation,
				limited.requiredRestorationDrainFraction,
				limited.deliveredRestorationDrainFraction,limited.restorationResidualBandPerS,
				static_cast<double>(limited.projection.maximumPreProjectionResidualPerS),
				static_cast<double>(limited.projection.maximumPostProjectionResidualPerS),
				limited.projection.validationPassed?1:0,limited.manifoldPlateauPassed?1:0,
				device.back(),wall.back());
			std::fprintf(stderr,"MANIFOLD_TIMESTEP audit independent_G=%.17g "
				"independent_field=%.17g atomic=%d golden=%d\n",independentGeneration,
				independentField,atomicRejection?1:0,
				DigestFile(checkpointPath)==checkpointDigest?1:0);
			const bool exact=selectedStep==1.589201814710624e-5&&
				representedStep==0x1.0a9fb2p-16f&&dt/representedStep==3.5423604574130363&&
				limited.maximumManifoldGeneration==0.0025081038475036621&&
				limited.maximumAcceptedManifoldDeviation==0.0025081038475036621&&
				limited.requiredRestorationDrainFraction==3.3441384633382163&&
				limited.deliveredRestorationDrainFraction==0.97489008508207653&&
				limited.restorationResidualBandPerS==0.0&&
				limited.projection.maximumPreProjectionResidualPerS==0x1.8ce8bp-11f&&
				limited.projection.maximumPostProjectionResidualPerS==0x1.3eec56p-16f&&
				!limited.projection.validationPassed&&!limited.manifoldPlateauPassed&&
				independentGeneration-limited.maximumManifoldGeneration==
					5.8727789631340954e-08&&
				independentField-limited.maximumAcceptedManifoldDeviation==
					5.8728976792821186e-08&&
				atomicRejection&&std::isfinite(device.back())&&device.back()>0.0&&
				device.back()<=75.0&&std::isfinite(wall.back())&&wall.back()>0.0&&
				wall.back()<=200.0&&
				DigestFile(checkpointPath)==checkpointDigest;
			return exact?254:252;
		}
		if(plateauProbe){
			if(slice!=0u)return 247;
			struct PlateauRegimeEvidence
			{
				double generationField=0.0,generationBeginning=0.0,generationOutput=0.0,
					removedField=0.0;
				std::size_t generationCell=0u;
				float physicalPreResidual=0.0f,physicalPostResidual=0.0f;
				std::uint32_t physicalSweeps=0u;
				std::array<float,17> residual={{}};
				std::array<std::uint32_t,16> sweeps={{}};
			};
			auto setEnvironment=[](const char* name,const std::string& value){
#if defined(_WIN32)
				return _putenv_s(name,value.c_str())==0;
#else
				return setenv(name,value.c_str(),1)==0;
#endif
			};
			auto clearEnvironment=[](const char* name){
#if defined(_WIN32)
				return _putenv_s(name,"")==0;
#else
				return unsetenv(name)==0;
#endif
			};
			auto measureRegime=[&](const char* label,const MethaneRunCheckpoint& state,
				const RISE::FireProductionResidentStepRequest& regimeRequest,
				PlateauRegimeEvidence& evidence){
				std::vector<double> beginningDeviation(state.states.size(),0.0);
				for(std::size_t cell=0u;cell<state.states.size();++cell){double ratio=0.0;
					if(!AcceptedConservativeVolumeRatio(ToConservativeVector(state.states[cell]),fuel,
						state.states[cell].producerPrecision,ratio,&error)){std::fprintf(stderr,
						"RESTORATION_PLATEAU regime=%s beginning_cell=%zu failed: %s\n",
						label,cell,error.c_str());return false;}
					beginningDeviation[cell]=ratio-1.0;}
				if(!setEnvironment("RISE_FIRE_PRODUCTION_RESTORATION_TEST","removed"))return false;
				RISE::FireProductionResidentStepResult removed;
				const bool removedSucceeded=RISE::AdvanceFireProductionResidentStepMetal(
					regimeRequest,removed,&error);
				const bool removedEnvironmentCleared=clearEnvironment(
					"RISE_FIRE_PRODUCTION_RESTORATION_TEST");
				if(!removedEnvironmentCleared||!removedSucceeded||
					removed.residentProjectionInvocationCount!=1u||
					removed.interstageFullGridTransferCount!=0u||
					!removed.projection.validationPassed||
					removed.projection.executedVCycleCount!=17u){std::fprintf(stderr,
					"RESTORATION_PLATEAU regime=%s removed failed success=%d invocations=%u "
					"transfers=%u cycles=%u error=%s\n",label,removedSucceeded?1:0,
					removed.residentProjectionInvocationCount,
					removed.interstageFullGridTransferCount,
					removed.projection.executedVCycleCount,error.c_str());return false;}
				std::vector<ConservativeVector> removedConservative;
				if(!FireProductionDyadicCalibration::UnpackProductionConservative(
					removed.conservativeValues,state.states.size(),removedConservative))return false;
				double generationField=0.0,removedField=0.0,generationBeginning=0.0,
					generationOutput=0.0;std::size_t generationCell=0u;
				for(std::size_t cell=0u;cell<state.states.size();++cell){double ratio=0.0;
					if(!AcceptedConservativeVolumeRatio(removedConservative[cell],fuel,
						FireStateProducerPrecision::Binary32,ratio,&error)){std::fprintf(stderr,
						"RESTORATION_PLATEAU regime=%s removed_cell=%zu failed: %s\n",
						label,cell,error.c_str());return false;}
					const double deviation=ratio-1.0;
					const double generation=std::fabs(deviation-beginningDeviation[cell]);
					if(generation>generationField){generationField=generation;
						generationCell=cell;generationBeginning=beginningDeviation[cell];
						generationOutput=deviation;}
					removedField=std::max(removedField,std::fabs(deviation));}
				std::array<float,17> residual={{}};std::array<std::uint32_t,16> sweeps={{}};
				for(unsigned int cycles=1u;cycles<=16u;++cycles){
					if(!setEnvironment("RISE_FIRE_PRODUCTION_RESTORATION_CYCLE_PROBE",
						std::to_string(cycles)))return false;
					RISE::FireProductionResidentStepResult result;
					const bool succeeded=RISE::AdvanceFireProductionResidentStepMetal(
						regimeRequest,result,&error);
					const bool cycleEnvironmentCleared=clearEnvironment(
						"RISE_FIRE_PRODUCTION_RESTORATION_CYCLE_PROBE");
					if(!cycleEnvironmentCleared||!succeeded||
						result.residentProjectionInvocationCount!=2u||
						result.interstageFullGridTransferCount!=0u||
						result.projection.executedVCycleCount!=cycles||
						!result.physicalProjection.validationPassed||
						result.physicalProjection.executedVCycleCount!=17u||
						result.physicalProjection.executedJacobiSweepCount!=
							removed.projection.executedJacobiSweepCount||
						result.physicalProjection.validationPassed!=
							removed.projection.validationPassed||
						result.physicalProjection.maximumPreProjectionResidualPerS!=
							removed.projection.maximumPreProjectionResidualPerS||
						result.physicalProjection.maximumPostProjectionResidualPerS!=
							removed.projection.maximumPostProjectionResidualPerS||
						result.physicalProjection.maximumOpenComplementarityDiscrepancyMPerS!=
							removed.projection.maximumOpenComplementarityDiscrepancyMPerS){std::fprintf(stderr,
						"RESTORATION_PLATEAU regime=%s cycles=%u failed success=%d "
						"executed=%u invocations=%u transfers=%u error=%s\n",label,cycles,
						succeeded?1:0,result.projection.executedVCycleCount,
						result.residentProjectionInvocationCount,
						result.interstageFullGridTransferCount,error.c_str());return false;}
					if(cycles==1u)residual[0]=result.projection.maximumPreProjectionResidualPerS;
					else if(residual[0]!=result.projection.maximumPreProjectionResidualPerS)return false;
					residual[cycles]=result.projection.maximumPostProjectionResidualPerS;
					sweeps[cycles-1u]=result.projection.executedJacobiSweepCount;
				}
				std::fprintf(stderr,"RESTORATION_PLATEAU regime=%s G_field=%.17g "
					"G_cell=%zu G_beginning=%.17g G_output=%.17g removed_field=%.17g "
					"physical_pre=%.17g physical_post=%.17g physical_sweeps=%llu residual=",
					label,generationField,generationCell,generationBeginning,
					generationOutput,removedField,static_cast<double>(
						removed.projection.maximumPreProjectionResidualPerS),static_cast<double>(
						removed.projection.maximumPostProjectionResidualPerS),
					static_cast<unsigned long long>(
						removed.projection.executedJacobiSweepCount));
				for(const float value:residual)std::fprintf(stderr," %.17g",
					static_cast<double>(value));
				std::fprintf(stderr," contraction=");
				for(std::size_t cycle=1u;cycle<residual.size();++cycle)
					std::fprintf(stderr," %.17g",static_cast<double>(residual[cycle])/
						static_cast<double>(residual[cycle-1u]));
				std::fprintf(stderr," sweeps=");
				for(const std::uint32_t value:sweeps)std::fprintf(stderr," %u",value);
				std::fprintf(stderr,"\n");
				evidence.generationField=generationField;
				evidence.generationBeginning=generationBeginning;
				evidence.generationOutput=generationOutput;
				evidence.removedField=removedField;evidence.generationCell=generationCell;
				evidence.physicalPreResidual=
					removed.projection.maximumPreProjectionResidualPerS;
				evidence.physicalPostResidual=
					removed.projection.maximumPostProjectionResidualPerS;
				evidence.physicalSweeps=removed.projection.executedJacobiSweepCount;
				evidence.residual=residual;evidence.sweeps=sweeps;return true;
			};
			PlateauRegimeEvidence burningEvidence,coldEvidence;
			if(!measureRegime("burning",beginning,request,burningEvidence))return 248;
			std::array<std::string,4> stateDigests,targetDigests;
			const std::filesystem::path coldDirectory=
				"rendered/fire_production_calibration/r112_dyadic_smooth_open";
			if(!FireProductionDyadicCalibration::ReadProtocolStateDigests(
				coldDirectory/"dyadic_protocol.v1",stateDigests)||
				!FireProductionDyadicCalibration::ReadTargetDigests(
				coldDirectory/"dyadic_targets.v1",targetDigests))return 249;
			MethaneRunCheckpoint cold;
			if(!FireProductionDyadicCalibration::BuildAnalyticState(12u,cold,error)||
				FireProductionDyadicCalibration::AnalyticStateDigest(cold)!=stateDigests[3])return 249;
			std::vector<std::vector<double> > coldTargets;
			const std::filesystem::path coldTarget=coldDirectory/"oracle_tier12_sdiv_x8.f64";
			if(DigestFile(coldTarget)!=targetDigests[3]||
				!ReadCalibrationDoublePayload(coldTarget,
					cold.states.size(),8u,coldTargets))return 249;
			const double coldFlow=6.0*std::sqrt(cold.values.characteristicDiameterM/
				FireProductionDyadicCalibration::Gravity);
			RISE::FireProductionResidentStepRequest coldRequest;
			if(!FireProductionDyadicCalibration::BuildProductionRequest(cold,coldTargets[0],
				coldFlow/512.0,coldRequest,error)||
				!measureRegime("cold",cold,coldRequest,coldEvidence))return 250;
			static const std::array<float,17> expectedBurningResidual={{
				0x1.c02f92p-13f,0x1.46e7dcp-17f,0x1.46e7dcp-17f,0x1.46e7dcp-17f,
				0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,
				0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,
				0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,0x1.4e97dcp-17f,
				0x1.4e97dcp-17f}};
			static const std::array<float,17> expectedColdResidual={{
				0x1.7e3b26p-4f,0x1.f8d616p-2f,0x1.2bc126p-3f,0x1.a4abbcp-4f,
				0x1.db4c28p-5f,0x1.2eb028p-5f,0x1.81bc28p-6f,0x1.f670ep-7f,
				0x1.49e27p-7f,0x1.b41f6p-8f,0x1.23406p-8f,0x1.86734p-9f,
				0x1.060b4p-9f,0x1.5ff88p-10f,0x1.d8fap-11f,0x1.3e14p-11f,
				0x1.abaep-12f}};
			bool exact=burningEvidence.generationField==0.0025328069638265172&&
				burningEvidence.generationCell==3227u&&
				burningEvidence.generationBeginning==-1.1871614802316799e-12&&
				burningEvidence.generationOutput==-0.0025328069650136786&&
				burningEvidence.removedField==0.0025328069650136786&&
				burningEvidence.physicalPreResidual==0x1.2efac6p+5f&&
				burningEvidence.physicalPostResidual==0x1.eb58p-9f&&
				burningEvidence.physicalSweeps==1156u&&
				burningEvidence.residual==expectedBurningResidual&&
				coldEvidence.generationField==0.00012031080315666465&&
				coldEvidence.generationCell==81216u&&
				coldEvidence.generationBeginning==0.00013951373206966267&&
				coldEvidence.generationOutput==0.00025982453522632731&&
				coldEvidence.removedField==0.00026066224468057619&&
				coldEvidence.physicalPreResidual==0x1.3bd084p-3f&&
				coldEvidence.physicalPostResidual==0x1.7b8c4p-17f&&
				coldEvidence.physicalSweeps==1054u&&
				coldEvidence.residual==expectedColdResidual;
			for(std::size_t cycle=0u;cycle<16u;++cycle)exact=exact&&
				burningEvidence.sweeps[cycle]==68u*(cycle+1u)&&
				coldEvidence.sweeps[cycle]==62u*(cycle+1u);
			const double burningRequiredDrain=burningEvidence.generationField/0.00075;
			const double coldRequiredDrain=coldEvidence.generationField/0.00075;
			const double burningDeliveredDrain=1.0-static_cast<double>(
				burningEvidence.residual[16])/static_cast<double>(burningEvidence.residual[0]);
			const double coldDeliveredDrain=1.0-static_cast<double>(
				coldEvidence.residual[16])/static_cast<double>(coldEvidence.residual[0]);
			exact=exact&&burningRequiredDrain==3.3770759517686897&&
				coldRequiredDrain==0.1604144042088862&&
				burningDeliveredDrain==0.9533406144549903&&
				coldDeliveredDrain==0.99562928290235475&&burningRequiredDrain>1.0&&
				DigestFile(checkpointPath)==checkpointDigest;
			std::fprintf(stderr,"RESTORATION_PLATEAU decision burning_r_req=%.17g "
				"burning_r16=%.17g cold_r_req=%.17g cold_r16=%.17g G_ratio=%.17g\n",
				burningRequiredDrain,burningDeliveredDrain,coldRequiredDrain,
				coldDeliveredDrain,burningEvidence.generationField/coldEvidence.generationField);
			return exact?253:254;
		}
		// The equal-time target and its digests are sealed above.  The two full
		// binary64 reference payloads are no longer inputs to a limited production
		// attempt; release them before allocating the 1.63-GiB resident Metal owner.
		std::vector<ConservativeVector> acceptedReferenceConservative;
		PeriodicMACField acceptedReferenceMomentum;
		if(limitedClosure||monitoredLongShadow){
			if(acceptedLongShadow){
				acceptedReferenceConservative=std::move(oracle.conservative);
				acceptedReferenceMomentum=std::move(oracle.momentumKGPerM2S);
			}
			oracle=ConservativeAdvance3DResult();oracleSerial=ConservativeAdvance3DResult();
			std::vector<float>().swap(equalTimeTerminalTarget);
			std::vector<float>().swap(equalTimeSerialTerminalTarget);
			std::vector<ConservativeVector>().swap(conservative);
			std::vector<MethaneSourcePacket>().swap(zeroPackets);
		}
		if(singleStageFCTSmoke){
			RISE::FireProductionScalarFCTRequest firstScalarStage;
			firstScalarStage.shape.nx=4u;firstScalarStage.shape.ny=4u;
			firstScalarStage.shape.nz=4u;firstScalarStage.shape.cellWidthM=1.0f;
			firstScalarStage.timeStepS=0.25f;
			firstScalarStage.boundary.fill(RISE::FireProductionProjectionPeriodic);
			const std::size_t stageCells=firstScalarStage.shape.CellCount();
			firstScalarStage.beginning.assign(9u*stageCells,0.0f);
			firstScalarStage.sourceDelta.assign(9u*stageCells,0.0f);
			const float stagePattern[4]={1.0f,1.25f,1.5f,1.25f};
			for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
				for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
					firstScalarStage.beginning[cell]=stagePattern[x];
					firstScalarStage.beginning[stageCells+cell]=stagePattern[x];}
			firstScalarStage.sourceDelta[0u]=0.125f;
			firstScalarStage.sourceDelta[stageCells]=0.125f;
			for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
				RISE::FireProductionProjectionFaceCount(firstScalarStage.shape,axis);
				firstScalarStage.frozenVelocityMPerS[axis].assign(
					faces,axis==0u?1.0f:0.0f);}
			for(auto& side:firstScalarStage.pressureOpenInflow)side.assign(16u,0u);
			firstScalarStage.ambient[0]=1.0f;firstScalarStage.ambient[1]=1.0f;
			firstScalarStage.nullity=1u;firstScalarStage.nullspaceBasis.assign(8u,0.0f);
			firstScalarStage.nullspaceBasis[1]=1.0f;
			firstScalarStage.coordinateProjector.assign(1u,1.0f);
			firstScalarStage.feasibilityFactor=1.0f/1024.0f;
			firstScalarStage.assemblyReserveFactor=firstScalarStage.feasibilityFactor;
			RISE::FireProductionScalarFCTRequest secondScalarStage=firstScalarStage;
			secondScalarStage.sourceDelta.assign(9u*stageCells,0.0f);
			for(std::size_t cell=0u;cell<stageCells;++cell){
				secondScalarStage.beginning[cell]=1.0f;
				secondScalarStage.beginning[stageCells+cell]=1.0f;}
			RISE::FireProductionScalarFCTFluxPair firstScalarPair,secondScalarPair,
				averagedScalarPair;
			RISE::FireProductionScalarFCTResult firstScalarCPU,secondScalarCPU,
				averagedScalarCPU;
			std::string stageError;
			const bool scalarCPUStages=
				RISE::BuildFireProductionScalarFCTFluxPairCPU(
					firstScalarStage,firstScalarPair,&stageError)&&
				RISE::BuildFireProductionScalarFCTFluxPairCPU(
					secondScalarStage,secondScalarPair,&stageError)&&
				RISE::AverageFireProductionScalarFCTFluxPairsCPU(
					firstScalarPair,secondScalarPair,averagedScalarPair,&stageError)&&
				RISE::SolveFireProductionScalarFCTFluxPairCPU(
					firstScalarStage,firstScalarPair,firstScalarCPU,&stageError)&&
				RISE::SolveFireProductionScalarFCTFluxPairCPU(
					firstScalarStage,secondScalarPair,secondScalarCPU,&stageError)&&
				RISE::SolveFireProductionScalarFCTFluxPairCPU(
					firstScalarStage,averagedScalarPair,averagedScalarCPU,&stageError);
			RISE::FireProductionScalarFCTMetalStageDiagnosticResult scalarMetalStages;
			const bool scalarMetalComputed=scalarCPUStages&&
				RISE::EvaluateFireProductionScalarFCTMetalStageDiagnostic(
					firstScalarStage,secondScalarStage,scalarMetalStages,&stageError);
			auto sameSolve=[](const RISE::FireProductionScalarFCTMetalAcceptanceStageResult& first,
				const RISE::FireProductionScalarFCTResult& second){return
				first.packedFaceOffset==second.packedFaceOffset&&
				first.lowFlux==second.lowFlux&&first.fluxDelta==second.fluxDelta&&
				first.lowState==second.lowState&&first.limiterRatio==second.limiterRatio&&
				first.sharedFaceAlpha==second.sharedFaceAlpha&&first.accepted==second.accepted;};
			bool freshAlphaDistinguished=false;
			if(scalarCPUStages)for(unsigned int axis=0u;axis<3u;++axis)
				for(std::size_t face=0u;face<averagedScalarCPU.sharedFaceAlpha[axis].size();++face)
					freshAlphaDistinguished=freshAlphaDistinguished||
						averagedScalarCPU.sharedFaceAlpha[axis][face]!=0.5f*(
							firstScalarCPU.sharedFaceAlpha[axis][face]+
							secondScalarCPU.sharedFaceAlpha[axis][face]);
			const bool scalarMetalStagesPassed=scalarMetalComputed&&
				scalarMetalStages.failureBitmap==std::array<std::uint32_t,3>{{0u,0u,0u}}&&
				scalarMetalStages.firstFluxPair.lowFlux==firstScalarPair.lowFlux&&
				scalarMetalStages.firstFluxPair.fluxDelta==firstScalarPair.fluxDelta&&
				scalarMetalStages.secondFluxPair.lowFlux==secondScalarPair.lowFlux&&
				scalarMetalStages.secondFluxPair.fluxDelta==secondScalarPair.fluxDelta&&
				scalarMetalStages.averagedFluxPair.lowFlux==averagedScalarPair.lowFlux&&
				scalarMetalStages.averagedFluxPair.fluxDelta==averagedScalarPair.fluxDelta&&
				sameSolve(scalarMetalStages.firstSolve,firstScalarCPU)&&
				sameSolve(scalarMetalStages.secondSolve,secondScalarCPU)&&
				sameSolve(scalarMetalStages.averagedSolve,averagedScalarCPU)&&
				freshAlphaDistinguished&&scalarMetalStages.commandCommitCount==7u;
			RISE::FireProductionSingleStageFCTBoundaryState boundaryState;
			boundaryState.statePayloadIdentity=RISE::FireProductionAcceptedStatePayloadDigestFast(
				request.force.shape,request.cellTransport.conservativeValues,
				request.force.beginningMomentumKGPerM2S,
				request.cellTransport.frozenVelocityMPerS);
			for(unsigned int side=0u;side<6u;++side){
				const std::size_t count=side<2u?request.force.shape.ny*request.force.shape.nz:
					(side<4u?request.force.shape.nx*request.force.shape.nz:
					request.force.shape.nx*request.force.shape.ny);
				boundaryState.pressureOpenInflow[side].assign(count,0u);
			}
			std::string boundaryError;
			const bool sealed=RISE::SealFireProductionSingleStageFCTBoundaryState(
				request.force.shape,request.force.boundary,boundaryState,&boundaryError);
			RISE::FireProductionSingleStageFCTDiagnosticResult candidate;
			const std::uint64_t commitsBefore=RISE::FireProductionResidentStepMetalCommandCommitCount();
			const auto start=std::chrono::steady_clock::now();
			const bool computed=sealed&&RISE::AttemptFireProductionSingleStageFCTDiagnosticMetal(
				request,boundaryState,candidate,&boundaryError);
			const double wallMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-start).count();
			const std::uint64_t commitsAfter=RISE::FireProductionResidentStepMetalCommandCommitCount();
			const bool accepted=scalarMetalStagesPassed&&computed&&candidate.accepted&&
				candidate.phase==RISE::FireProductionSingleStageFCTDiagnosticPhase::Accepted&&
				candidate.operatorVersion==1u&&candidate.pipelineIdentityComplete&&
				candidate.fluxPairBuildCount==1u&&candidate.fctSolveCount==1u&&
				candidate.compatibleRateApplicationCount==1u&&
				candidate.sourceApplicationCount==1u&&candidate.scalarAdmissible&&
				candidate.scalarStageProducerEquivalencePassed&&
				candidate.scalarStageExactZeroPassed&&
				candidate.compatibleRateProducerEquivalencePassed&&
				candidate.compatibleRateExactZeroPassed&&candidate.affineIdentityPassed&&
				candidate.commutingIdentityPassed&&candidate.failureBitmap==0u&&
				candidate.terminalStagingCount>=1u&&
				candidate.actualMetalAllocationBytes<=candidate.certifiedWorkingSetBytes&&
				candidate.beginningStateIdentity==boundaryState.statePayloadIdentity&&
				candidate.boundaryStateIdentity==boundaryState.identity&&commitsAfter>commitsBefore;
			std::fprintf(stderr,"SINGLE_STAGE_FCT_SMOKE sealed=%d computed=%d accepted=%d stages=%d "
				"phase=%u operator=%u pipelines=%d flux_pairs=%u solves=%u rates=%u sources=%u "
				"scalar_identity=%d scalar_equivalent=%d scalar_zero=%d scalar_normalized=%.9g "
				"rate_equivalent=%d rate_zero=%d rate_normalized=%.9g commuting=%d "
				"residual=%.9g projections=%u transfers=%u "
				"terminal=%u certified=%llu actual=%llu commits=%llu wall_ms=%.17g error=%s\n",
				sealed?1:0,computed?1:0,accepted?1:0,scalarMetalStagesPassed?1:0,
				static_cast<unsigned int>(candidate.phase),
				candidate.operatorVersion,candidate.pipelineIdentityComplete?1:0,
				candidate.fluxPairBuildCount,candidate.fctSolveCount,
				candidate.compatibleRateApplicationCount,candidate.sourceApplicationCount,
				candidate.scalarStageIdentityPassed?1:0,
				candidate.scalarStageProducerEquivalencePassed?1:0,
				candidate.scalarStageExactZeroPassed?1:0,
				candidate.maximumScalarStageNormalizedDifference,
				candidate.compatibleRateProducerEquivalencePassed?1:0,
				candidate.compatibleRateExactZeroPassed?1:0,
				candidate.maximumCompatibleRateNormalizedDifference,
				candidate.commutingIdentityPassed?1:0,
				candidate.maximumCommutingResidual,candidate.residentProjectionInvocationCount,
				candidate.interstageFullGridTransferCount,candidate.terminalStagingCount,
				static_cast<unsigned long long>(candidate.certifiedWorkingSetBytes),
				static_cast<unsigned long long>(candidate.actualMetalAllocationBytes),
				static_cast<unsigned long long>(commitsAfter-commitsBefore),wallMS,
				(stageError.empty()?boundaryError:stageError).c_str());
			return accepted?179:180;
		}
		RISE::FireProductionResidentStepResult production;
		bool productionSucceeded=false;
		double productionWallMS=0.0;
		for(;;){
			const auto productionWallStart=std::chrono::steady_clock::now();
			productionSucceeded=(longShadow||goldenSubdominance||equalTimeReadmission||
				thermoSourceMaps)?
				RISE::AttemptFireProductionResidentStepMetal(request,production,&error):
				RISE::AdvanceFireProductionResidentStepMetal(request,production,&error);
			productionWallMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-productionWallStart).count();
			if(std::isfinite(production.deviceMakespanMS)&&production.deviceMakespanMS>=0.0)
				sliceDeviceMS+=production.deviceMakespanMS;
			if(std::isfinite(productionWallMS)&&productionWallMS>=0.0)
				sliceWallMS+=productionWallMS;
			// Attempt returns false for a manifold refusal while preserving the
			// tokenless diagnostic payload.  Physical validation is independent and
			// must be brought into band before the owner classifies that ordinary
			// refusal; the boolean alone cannot distinguish it from a fatal/default
			// result.
			if((!acceptedLongShadow&&!physicalRetryRED&&!goldenSubdominance&&
				!equalTimeReadmission&&!thermoSourceMaps)||
				production.HasAcceptedManifoldToken()||
				production.physicalProjection.validationPassed||
				(production.residentProjectionInvocationCount!=2u&&!thermoSourceMaps))break;
			const double pre=production.physicalProjection.maximumPreProjectionResidualPerS;
			const double post=production.physicalProjection.maximumPostProjectionResidualPerS;
			const double band=production.physicalProjection.validationBandPerS;
			const std::uint32_t cycles=production.physicalProjection.executedVCycleCount;
			if(production.HasAcceptedManifoldToken()||!production.projection.validationPassed||
				cycles!=request.physicalOpenProjectionVCycleCount||cycles<1u||cycles>=64u||
				!std::isfinite(pre)||!std::isfinite(post)||!std::isfinite(band)||
				!(pre>post)||!(post>0.0)||!(band>0.0)||!(post>band)){
				std::fprintf(stderr,"PHYSICAL_PROJECTION_RETRY_INVALID source_mode=%d token=%d "
					"terminal_valid=%d invocations=%u cycles=%u requested=%u pre=%.17g "
					"post=%.17g band=%.17g error=%s\n",thermoSourceMaps?1:0,
					production.HasAcceptedManifoldToken()?1:0,
					production.projection.validationPassed?1:0,
					production.residentProjectionInvocationCount,cycles,
					request.physicalOpenProjectionVCycleCount,pre,post,band,error.c_str());return 120;
			}
			const double contraction=std::pow(post/pre,1.0/static_cast<double>(cycles));
			if(!std::isfinite(contraction)||!(contraction>0.0)||!(contraction<1.0))return 120;
			const double requiredReal=std::ceil(std::log(band/pre)/std::log(contraction));
			if(!std::isfinite(requiredReal)||requiredReal<=static_cast<double>(cycles)||
				requiredReal>64.0)return 120;
			const std::uint32_t required=static_cast<std::uint32_t>(requiredReal);
			std::fprintf(stderr,"PHYSICAL_PROJECTION_RETRY step=%zu refused_cycles=%u "
				"pre=%.17g post=%.17g band=%.17g contraction=%.17g next_cycles=%u cap=64 "
				"accepted_token=0\n",slice,cycles,pre,post,band,contraction,required);
			request.physicalOpenProjectionVCycleCount=required;
			longShadowPhysicalOpenVCycleCount=required;
			++longShadowPhysicalRetryCount;
			++physicalRetryCount;
			longShadowPhysicalRetryFinalCount=required;
			production=RISE::FireProductionResidentStepResult();
			error.clear();
		}
		const bool monitoredDynamicsRefusal=monitoredLongShadow&&!productionSucceeded&&
			!production.manifoldDynamicsBoundPassed&&
			production.maximumAcceptedManifoldDeviation>0x1p-2&&
			!production.HasAcceptedManifoldToken();
		if(!productionSucceeded&&!monitoredDynamicsRefusal&&
			!(longShadow&&production.manifoldNextTimeStepAvailable&&
			!production.manifoldPlateauPassed&&!production.HasAcceptedManifoldToken())){
			std::fprintf(stderr,
			"production golden resident slice %zu failed: %s\n",slice,error.c_str());return 119;}
		if(thermoSourceMaps){
			if(slice>=thermoSourceFieldMaximum.size())return 183;
			const bool tailBeginningExpected=slice>0u;
			const bool projectionTopologyValid=
				(!tailBeginningExpected&&production.residentProjectionInvocationCount==1u&&
					!production.manifoldTailRestorationApplied)||
				(tailBeginningExpected&&production.residentProjectionInvocationCount==2u&&
					production.manifoldTailRestorationApplied&&
					production.physicalProjection.validationPassed);
			const bool accepted=productionSucceeded&&production.HasAcceptedManifoldToken()&&
				projectionTopologyValid&&production.projection.validationPassed&&
				production.manifoldDynamicsBoundPassed&&production.manifoldDiagnosticsMonitored&&
				!production.manifoldPlateauEnforced&&sourceActiveCellCount>0u&&
				!sourcePacketDigest.empty()&&
				production.manifoldTailRestorationApplied==
					(production.manifoldTailCellCount>0u);
			thermoSourceFieldMaximum[slice]=production.maximumAcceptedManifoldDeviation;
			thermoSourceFieldP95[slice]=production.acceptedManifoldDeviationP95;
			thermoSourceFieldP50[slice]=production.acceptedManifoldDeviationP50;
			thermoSourceTailCells[slice]=production.manifoldTailCellCount;
			thermoSourceTailExcess[slice]=production.manifoldTailExcessSum;
			thermoSourceTailDrainM3[slice]=production.manifoldTailDrainedVolumeM3;
			thermoSourceDeviceMS[slice]=production.deviceMakespanMS;
			thermoSourceWallMS[slice]=productionWallMS;
			std::fprintf(stderr,"THERMO_SOURCE_MAP_STEP step=%zu dt=%.17g active_cells=%zu "
				"heat_release_W=%.17g "
				"source_margin=%.17g source_digest=%s target_digest=%s schedule=%s "
				"field_max=%.17g field_p95=%.17g field_p50=%.17g tail_cells=%u "
				"tail_excess=%.17g tail_drain_m3=%.17g hard_bound=%d "
				"projection_invocations=%u physical_status=%s restoration_valid=%d "
				"device_ms=%.17g wall_ms=%.17g accepted=%d golden=%s\n",
				slice,dt,sourceActiveCellCount,sourceHeatReleaseW,sourceProducerMinimumMargin,
				sourcePacketDigest.c_str(),equalTimeTerminalTargetDigest.c_str(),
				equalTimeReferenceScheduleDigest.c_str(),
				production.maximumAcceptedManifoldDeviation,
				production.acceptedManifoldDeviationP95,
				production.acceptedManifoldDeviationP50,production.manifoldTailCellCount,
				production.manifoldTailExcessSum,production.manifoldTailDrainedVolumeM3,
				production.manifoldDynamicsBoundPassed?0:1,
				production.residentProjectionInvocationCount,
				production.residentProjectionInvocationCount==1u?"not_invoked":
					(production.physicalProjection.validationPassed?"valid":"invalid"),
				production.projection.validationPassed?1:0,production.deviceMakespanMS,
				productionWallMS,accepted?1:0,checkpointDigest);
			if(!accepted||DigestFile(checkpointPath)!=checkpointDigest)return 183;
			RISE::FireProductionAcceptedManifoldObservation acceptedObservation;
			if(!RISE::PublishFireProductionAcceptedManifoldObservation(
				static_cast<double>(production.representedTimeStepS),production,
				acceptedObservation,&error)||!acceptedObservation.Available()||
				acceptedObservation.MaximumGeneration()!=0.0||
				acceptedObservation.RestorationDrainFraction()!=0.0||
				!FireProductionDyadicCalibration::ApplyAcceptedProductionResult(
					production,acceptedObservation,beginning,error))return 183;
			const double representedStep=static_cast<double>(production.representedTimeStepS);
			beginning.simulationTimeS+=representedStep;
			beginning.previousStepS=representedStep;
			beginning.lastAcceptedStepS=representedStep;
			++beginning.acceptedSteps;
			beginning.values.acceptedTimeStepHistoryS.push_back(representedStep);
			beginning.productionManifoldObservation=acceptedObservation;
			if(slice==0u){
				thermoSourceState=std::move(beginning);
				sliceAccepted=true;
				break;
			}
			const double deviceProjectionHours=(thermoSourceDeviceMS[0]+
				thermoSourceDeviceMS[1])*25.0/(2.0*dt*3600000.0);
			const double wallProjectionHours=(thermoSourceWallMS[0]+
				thermoSourceWallMS[1])*25.0/(2.0*dt*3600000.0);
			std::fprintf(stderr,"THERMO_SOURCE_MAP_COMPLETE steps=2 field_max=%.17g/%.17g "
				"field_p95=%.17g/%.17g field_p50=%.17g/%.17g tail_cells=%u/%u "
				"tail_excess=%.17g/%.17g tail_drain_m3=%.17g/%.17g "
				"device_projection_hours=%.17g wall_projection_hours=%.17g golden=%s\n",
				thermoSourceFieldMaximum[0],thermoSourceFieldMaximum[1],
				thermoSourceFieldP95[0],thermoSourceFieldP95[1],
				thermoSourceFieldP50[0],thermoSourceFieldP50[1],
				thermoSourceTailCells[0],thermoSourceTailCells[1],
				thermoSourceTailExcess[0],thermoSourceTailExcess[1],
				thermoSourceTailDrainM3[0],thermoSourceTailDrainM3[1],
				deviceProjectionHours,wallProjectionHours,checkpointDigest);
			return thermoSourceTailCells[0]==0u&&thermoSourceTailCells[1]>0u&&
				thermoSourceTailDrainM3[1]>0.0?184:183;
		}
		if(physicalRetryRED){
			std::fprintf(stderr,"PHYSICAL_PROJECTION_RETRY_RED initial_cycles=12 "
				"retry_count=%zu final_cycles=%u physical_valid=%d manifold_refused=%d "
				"accepted_token=%d golden=%s\n",longShadowPhysicalRetryCount,
				longShadowPhysicalRetryFinalCount,
				production.physicalProjection.validationPassed?1:0,
				(!productionSucceeded&&production.manifoldNextTimeStepAvailable&&
					!production.manifoldPlateauPassed)?1:0,
				production.HasAcceptedManifoldToken()?1:0,checkpointDigest);
			return effectiveManifoldRetryCandidate==0u&&
				longShadowPhysicalRetryCount>0u&&longShadowPhysicalRetryFinalCount>12u&&
				production.physicalProjection.validationPassed&&
				production.projection.validationPassed&&!productionSucceeded&&
				production.manifoldNextTimeStepAvailable&&!production.manifoldPlateauPassed&&
				!production.HasAcceptedManifoldToken()&&
				DigestFile(checkpointPath)==checkpointDigest?209:202;
		}
		if(monitoredLongShadow){
			const double fieldMaximum=production.maximumAcceptedManifoldDeviation;
			const bool tailRestored=production.manifoldTailRestorationApplied;
			if(monitoredDynamicsRefusal){
				RISE::FireProductionResidentStepResult ordinaryResult;
				ordinaryResult.conservativeValues.push_back(1.0f);
				std::string ordinaryError;
				const bool ordinaryAtomic=!RISE::AdvanceFireProductionResidentStepMetal(
					request,ordinaryResult,&ordinaryError)&&ordinaryResult.conservativeValues.empty()&&
					ordinaryResult.representedTimeStepS==0.0f&&
					!ordinaryResult.HasAcceptedManifoldToken()&&
					ordinaryError==
						"production realized manifold deviation exceeds the dynamics-validity bound";
				unsigned int nextCandidate=0u;double nextStepS=0.0;
				const RISE::FireProductionResidentStepAttemptDisposition disposition=
					RISE::ClassifyFireProductionResidentStepAttempt(
						effectiveManifoldRetryCandidate,production,nextCandidate,nextStepS);
				std::fprintf(stderr,"OUTLIER_BOUNDED_MANIFOLD_REFUSAL step=%zu candidate=%u dt=%.17g "
					"field_max=%.17g field_p95=%.17g field_p50=%.17g tail_cells=%u "
					"tail_excess=%.17g tail_drain_m3=%.17g restoration_passes=%d "
					"physical_valid=%d restoration_valid=%d accepted_token=0 "
					"beginning_max_velocity=%.17g suggested_dt=%.17g next_candidate=%u cap=%u "
					"ordinary_atomic=%d golden=%s\n",slice,effectiveManifoldRetryCandidate,
					static_cast<double>(production.representedTimeStepS),fieldMaximum,
					production.acceptedManifoldDeviationP95,
					production.acceptedManifoldDeviationP50,
					production.manifoldTailCellCount,production.manifoldTailExcessSum,
					production.manifoldTailDrainedVolumeM3,tailRestored?1:0,
					production.physicalProjection.validationPassed?1:0,
					production.projection.validationPassed?1:0,monitoredMaximumVelocity,
					nextStepS,nextCandidate,RISE::FireStepRejectionRetryCap,
					ordinaryAtomic?1:0,checkpointDigest);
				if(tailThresholdRED&&!hardBoundRetryRED)return slice==33u&&
					effectiveManifoldRetryCandidate==0u&&
					tailRestored&&fieldMaximum==0.25728172063827515&&
					monitoredMaximumVelocity==10.222167015075684&&
					production.physicalProjection.validationPassed&&
					production.projection.validationPassed&&ordinaryAtomic&&
					disposition==RISE::FireProductionResidentStepAttemptDisposition::
						RetryAtSuggestedTimeStep&&
					DigestFile(checkpointPath)==checkpointDigest?193:197;
				if(!ordinaryAtomic||disposition!=
					RISE::FireProductionResidentStepAttemptDisposition::
						RetryAtSuggestedTimeStep||
					nextCandidate!=effectiveManifoldRetryCandidate+1u||
					!(nextStepS>0.0)||!(nextStepS<production.representedTimeStepS))return 197;
				++longShadowManifoldRefusalCount[slice];
				if(!std::isfinite(longShadowFirstRefusedField[slice]))
					longShadowFirstRefusedField[slice]=fieldMaximum;
				sliceRetryCandidate=nextCandidate;sliceRetryStepS=nextStepS;
				production=RISE::FireProductionResidentStepResult();request=
					RISE::FireProductionResidentStepRequest();error.clear();
				continue;
			}
			const bool monitoredAccepted=productionSucceeded&&
				production.manifoldDiagnosticsMonitored&&
				!production.manifoldPlateauEnforced&&production.manifoldPlateauPassed&&
				production.HasAcceptedManifoldToken()&&
				production.projection.validationPassed&&
				production.residentProjectionInvocationCount==(tailRestored?2u:1u)&&
				production.interstageFullGridTransferCount==0u&&
				production.advectiveAnomalyClosurePassCount==0u&&
				production.manifoldScalarDeviceToHostTransferCount==1u&&
				production.manifoldDynamicsBoundPassed&&
				production.manifoldTailRestorationApplied==
					(production.manifoldTailCellCount>0u)&&
				(!tailRestored||production.physicalProjection.validationPassed)&&
				!production.manifoldNextTimeStepAvailable&&
				production.suggestedManifoldTimeStepS==0.0&&
				std::isfinite(fieldMaximum)&&fieldMaximum>=0.0&&
				std::isfinite(production.acceptedManifoldDeviationP95)&&
				std::isfinite(production.acceptedManifoldDeviationP50)&&
				production.manifoldAllowanceExceeded==
					(fieldMaximum>0.0234375)&&
				production.manifoldCeilingExceeded==
					(fieldMaximum>0.03125);
			if(!monitoredAccepted){
				std::fprintf(stderr,"MONITORED_MANIFOLD_SHADOW step=%zu failed: %s\n",
					slice,error.c_str());return 197;
			}
			if(hardBoundRetryRED&&effectiveManifoldRetryCandidate>0u){
				std::fprintf(stderr,"OUTLIER_BOUNDED_HARD_RETRY_ACCEPTED step=%zu candidate=%u "
					"dt=%.17g field_max=%.17g physical_valid=%d restoration_valid=%d "
					"accepted_token=%d golden=%s\n",slice,effectiveManifoldRetryCandidate,
					static_cast<double>(production.representedTimeStepS),fieldMaximum,
					production.physicalProjection.validationPassed?1:0,
					production.projection.validationPassed?1:0,
					production.HasAcceptedManifoldToken()?1:0,checkpointDigest);
				return slice==33u&&effectiveManifoldRetryCandidate==1u&&
					longShadowManifoldRefusalCount[slice]==1u&&
					fieldMaximum<=0x1p-2&&production.manifoldDynamicsBoundPassed&&
					production.physicalProjection.validationPassed&&
					production.projection.validationPassed&&
					production.HasAcceptedManifoldToken()&&
					DigestFile(checkpointPath)==checkpointDigest?192:197;
			}
			longShadowFieldMaximum.push_back(fieldMaximum);
			longShadowFieldP95.push_back(production.acceptedManifoldDeviationP95);
			longShadowFieldP50.push_back(production.acceptedManifoldDeviationP50);
			longShadowPredictorGeneration.push_back(production.maximumManifoldGeneration);
			longShadowAcceptedStepS.push_back(
				static_cast<double>(production.representedTimeStepS));
			longShadowAcceptedCandidate.push_back(effectiveManifoldRetryCandidate);
			longShadowActiveLimit.push_back(monitoredActiveLimit);
			longShadowMaximumVelocity.push_back(monitoredMaximumVelocity);
			longShadowMaximumReducedGravity.push_back(monitoredMaximumReducedGravity);
			longShadowMaximumDiffusivity.push_back(monitoredMaximumDiffusivity);
			longShadowTailCellCount.push_back(production.manifoldTailCellCount);
			longShadowTailExcessSum.push_back(production.manifoldTailExcessSum);
			longShadowTailDrainedVolumeM3.push_back(production.manifoldTailDrainedVolumeM3);
			longShadowDeviceMS.push_back(sliceDeviceMS);
			longShadowWallMS.push_back(sliceWallMS);
			longShadowAllowanceCrossings+=production.manifoldAllowanceExceeded?1u:0u;
			longShadowCeilingCrossings+=production.manifoldCeilingExceeded?1u:0u;
			++longShadowPhysicalValidationCount;
			longShadowRestorationValidationCount+=tailRestored?1u:0u;
			longShadowFinalStepS=static_cast<double>(production.representedTimeStepS);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,slice);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				static_cast<double>(production.representedTimeStepS));
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.maximumManifoldGeneration);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,fieldMaximum);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.acceptedManifoldDeviationP95);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.acceptedManifoldDeviationP50);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
				production.manifoldAllowanceExceeded?1u:0u);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
				production.manifoldCeilingExceeded?1u:0u);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
				production.manifoldTailCellCount);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
				effectiveManifoldRetryCandidate);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
				longShadowManifoldRefusalCount[slice]);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				std::isfinite(longShadowFirstRefusedField[slice])?
					longShadowFirstRefusedField[slice]:0.0);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.manifoldTailExcessSum);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.manifoldTailDrainedVolumeM3);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				monitoredMaximumVelocity);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				monitoredMaximumReducedGravity);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				monitoredMaximumDiffusivity);
			for(const char character:monitoredActiveLimit)
				FireProductionDyadicCalibration::AppendInteger(longShadowTrace,
					static_cast<unsigned char>(character));
			std::fprintf(stderr,"MONITORED_MANIFOLD_SHADOW_STEP step=%zu dt=%.17g "
				"G_eulerian=%.17g field_max=%.17g field_p95=%.17g field_p50=%.17g "
				"allowance_crossed=%d ceiling_crossed=%d projection_valid=1 "
				"restoration_passes=%d tail_cells=%u tail_excess=%.17g "
				"tail_drain_m3=%.17g dynamics_bound=1 scalar_reads=1 selector=%s max_velocity=%.17g "
				"max_reduced_gravity=%.17g max_diffusivity=%.17g device_ms=%.17g wall_ms=%.17g "
				"candidate=%u refusals=%u first_refused_field=%.17g "
				"attempt_device_ms=%.17g attempt_wall_ms=%.17g step_device_ms=%.17g "
				"step_wall_ms=%.17g target=%s schedule=%s\n",slice,
				static_cast<double>(production.representedTimeStepS),
				production.maximumManifoldGeneration,fieldMaximum,
				production.acceptedManifoldDeviationP95,
				production.acceptedManifoldDeviationP50,
				production.manifoldAllowanceExceeded?1:0,
				production.manifoldCeilingExceeded?1:0,tailRestored?1:0,
				production.manifoldTailCellCount,production.manifoldTailExcessSum,
				production.manifoldTailDrainedVolumeM3,monitoredActiveLimit.c_str(),
				monitoredMaximumVelocity,monitoredMaximumReducedGravity,
				monitoredMaximumDiffusivity,production.deviceMakespanMS,productionWallMS,
				effectiveManifoldRetryCandidate,longShadowManifoldRefusalCount[slice],
				std::isfinite(longShadowFirstRefusedField[slice])?
					longShadowFirstRefusedField[slice]:0.0,
				production.deviceMakespanMS,productionWallMS,sliceDeviceMS,sliceWallMS,
				equalTimeTerminalTargetDigest.c_str(),
				equalTimeReferenceScheduleDigest.c_str());
			RISE::FireProductionAcceptedManifoldObservation acceptedObservation;
			if(!RISE::PublishFireProductionAcceptedManifoldObservation(
				static_cast<double>(production.representedTimeStepS),production,
				acceptedObservation,&error)||!acceptedObservation.Available()||
				acceptedObservation.MaximumGeneration()!=0.0||
				acceptedObservation.RestorationDrainFraction()!=0.0||
				!FireProductionDyadicCalibration::ApplyAcceptedProductionResult(
					production,acceptedObservation,beginning,error))return 197;
			const double representedStep=static_cast<double>(production.representedTimeStepS);
			beginning.simulationTimeS+=representedStep;
			beginning.previousStepS=representedStep;
			beginning.lastAcceptedStepS=representedStep;
			++beginning.acceptedSteps;
			beginning.values.acceptedTimeStepHistoryS.push_back(representedStep);
			beginning.productionManifoldObservation=acceptedObservation;
			longShadowState=std::move(beginning);
			sliceAccepted=true;
			continue;
		}
		if(limitedClosure&&effectiveManifoldRetryCandidate>0u&&productionSucceeded&&
			production.manifoldPlateauPassed&&!production.HasAcceptedManifoldToken()){
			unsigned int ignoredCandidate=0u;double ignoredStep=0.0;
			const double fieldMaximum=production.maximumAcceptedManifoldDeviation;
			const bool exactMaterialAuthorityRefusal=effectiveManifoldRetryCandidate==1u&&
				RISE::ClassifyFireProductionResidentStepAttempt(
					effectiveManifoldRetryCandidate,production,ignoredCandidate,ignoredStep)==
					RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
				static_cast<double>(production.representedTimeStepS)==
					0.00055692793102934957&&
				production.maximumPredictedAdvectiveManifoldAnomaly==
					0.019709885120391846&&
				production.maximumManifoldGeneration==0.023429989814758301&&
				fieldMaximum==0.023429989814758301&&
				equalTimeReferenceScheduleDigest==
					"0db10079074f5006eff7b2f9e27b2b6f5fc2c2017d1d333c29264e113c03b08b"&&
				equalTimeTerminalTargetDigest==
					"cf67f48c2e6320404d7af6794c87966c4c199b3c652fdb5b62d849c691068bae"&&
				fieldMaximum<=0.0234375&&fieldMaximum<0.03125&&
				production.physicalProjection.validationPassed&&
				production.projection.validationPassed&&
				production.advectiveAnomalyClosurePassCount==2u&&
				production.cellSubmapCount==10u&&production.dualSubmapCount==15u&&
				production.sourceCommandCommitCount==2u&&
				production.manifoldScalarDeviceToHostTransferCount==2u&&
				production.interstageFullGridTransferCount==0u&&
				DigestFile(checkpointPath)==checkpointDigest;
			std::fprintf(stderr,"MATERIAL_MANIFOLD_AUTHORITY_REFUSAL candidate=%u "
				"dt=%.17g schedule=%s terminal_target=%s G_eulerian=%.17g "
				"field_max=%.17g plateau_passed=1 "
				"accepted_token=0 ordinary_disposition=rejected golden=%s\n",
				effectiveManifoldRetryCandidate,
				static_cast<double>(production.representedTimeStepS),
				equalTimeReferenceScheduleDigest.c_str(),
				equalTimeTerminalTargetDigest.c_str(),
				production.maximumManifoldGeneration,fieldMaximum,checkpointDigest);
			return exactMaterialAuthorityRefusal?214:212;
		}
		double closureDeviceP95MS=0.0,closureWallP95MS=0.0;
		if(longShadow&&!disabledClosure&&hostResidualProbe){
			const std::uint64_t baselinePayload=
				RISE::FireProductionAcceptedManifoldPayloadDigest(production);
			const bool baselinePlateauPassed=production.manifoldPlateauPassed;
			const bool baselineAcceptedToken=production.HasAcceptedManifoldToken();
			const bool baselineNextStepAvailable=production.manifoldNextTimeStepAvailable;
			const double baselineSuggestedStep=production.suggestedManifoldTimeStepS;
			std::array<double,5> serialDevice={{}},serialWall={{}},parallelDevice={{}},parallelWall={{}};
			auto measureSchedulingMode=[&](const char* mode,std::array<double,5>& device,
				std::array<double,5>& wall){
				if(!setFixtureEnvironment("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE",mode))return false;
				for(std::size_t sample=0u;sample<wall.size();++sample){
					RISE::FireProductionResidentStepResult trial;
					const auto trialStart=std::chrono::steady_clock::now();
					const bool trialAccepted=RISE::AttemptFireProductionResidentStepMetal(
						request,trial,&error);
					if(!trialAccepted&&!(trial.manifoldNextTimeStepAvailable&&
						!trial.manifoldPlateauPassed&&!trial.HasAcceptedManifoldToken()))return false;
					wall[sample]=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-trialStart).count();
					device[sample]=trial.deviceMakespanMS;
					if(trialAccepted!=productionSucceeded||
						trial.manifoldPlateauPassed!=baselinePlateauPassed||
						trial.HasAcceptedManifoldToken()!=baselineAcceptedToken||
						trial.manifoldNextTimeStepAvailable!=baselineNextStepAvailable||
						trial.suggestedManifoldTimeStepS!=baselineSuggestedStep||
						RISE::FireProductionAcceptedManifoldPayloadDigest(trial)!=baselinePayload||
						trial.maximumPredictedAdvectiveManifoldAnomaly!=
							production.maximumPredictedAdvectiveManifoldAnomaly||
						trial.maximumManifoldGeneration!=production.maximumManifoldGeneration||
						trial.maximumAcceptedManifoldDeviation!=
							production.maximumAcceptedManifoldDeviation||
						trial.deliveredRestorationDrainFraction!=
							production.deliveredRestorationDrainFraction||
						trial.advectiveAnomalyClosurePassCount!=2u||trial.cellSubmapCount!=10u||
						trial.dualSubmapCount!=15u||trial.sourceCommandCommitCount!=2u||
						trial.manifoldScalarDeviceToHostTransferCount!=2u||
						trial.interstageFullGridTransferCount!=0u)return false;
				}
				return true;
			};
			if(!measureSchedulingMode("serial",serialDevice,serialWall)||
				!measureSchedulingMode("parallel",parallelDevice,parallelWall)||
				!clearFixtureEnvironment("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE"))return 250;
			auto maximum=[](const std::array<double,5>& values){return
				*std::max_element(values.begin(),values.end());};
			auto mean=[](const std::array<double,5>& values){double sum=0.0;
				for(const double value:values)sum+=value;return sum/static_cast<double>(values.size());};
			auto standardDeviation=[&](const std::array<double,5>& values){
				const double average=mean(values);double squareSum=0.0;
				for(const double value:values){const double delta=value-average;squareSum+=delta*delta;}
				return std::sqrt(squareSum/static_cast<double>(values.size()));};
			closureDeviceP95MS=maximum(parallelDevice);
			closureWallP95MS=maximum(parallelWall);
			std::array<double,5> parallelHostResidual={{}};
			for(std::size_t sample=0u;sample<parallelHostResidual.size();++sample)
				parallelHostResidual[sample]=parallelWall[sample]-parallelDevice[sample];
			std::fprintf(stderr,"HOST_RESIDUAL_SAMPLES candidate=%u "
				"serial_wall=%.9g,%.9g,%.9g,%.9g,%.9g serial_device=%.9g,%.9g,%.9g,%.9g,%.9g "
				"parallel_wall=%.9g,%.9g,%.9g,%.9g,%.9g parallel_device=%.9g,%.9g,%.9g,%.9g,%.9g "
				"serial_wall_mean=%.17g serial_wall_stddev=%.17g "
				"parallel_wall_mean=%.17g parallel_wall_stddev=%.17g "
				"parallel_host_residual=%.9g,%.9g,%.9g,%.9g,%.9g "
				"parallel_host_residual_p95=%.17g parallel_host_residual_mean=%.17g "
				"parallel_host_residual_stddev=%.17g\n",
				manifoldRetryCandidate,serialWall[0],serialWall[1],serialWall[2],serialWall[3],
				serialWall[4],serialDevice[0],serialDevice[1],serialDevice[2],serialDevice[3],
				serialDevice[4],parallelWall[0],parallelWall[1],parallelWall[2],parallelWall[3],
				parallelWall[4],parallelDevice[0],parallelDevice[1],parallelDevice[2],
				parallelDevice[3],parallelDevice[4],mean(serialWall),standardDeviation(serialWall),
				mean(parallelWall),standardDeviation(parallelWall),parallelHostResidual[0],
				parallelHostResidual[1],parallelHostResidual[2],parallelHostResidual[3],
				parallelHostResidual[4],maximum(parallelHostResidual),mean(parallelHostResidual),
				standardDeviation(parallelHostResidual));
		}
		const float maximumRestorationTarget=*std::max_element(
			request.restorationDivergenceTargetPerS.begin(),
			request.restorationDivergenceTargetPerS.end(),[](const float a,const float b){
				return std::fabs(a)<std::fabs(b);});
		std::fprintf(stderr,"golden projection slice=%zu physical_valid=%d physical_pre=%.9g "
			"physical_post=%.9g physical_band=%.9g "
			"physical_cycles=%u restoration_valid=%d restoration_pre=%.9g "
			"restoration_post=%.9g restoration_target_max=%.9g restoration_band=%.9g\n",
			slice,production.physicalProjection.validationPassed?1:0,
			production.physicalProjection.maximumPreProjectionResidualPerS,
			production.physicalProjection.maximumPostProjectionResidualPerS,
			production.physicalProjection.validationBandPerS,
			production.physicalProjection.executedVCycleCount,
			production.projection.validationPassed?1:0,
			production.projection.maximumPreProjectionResidualPerS,
			production.projection.maximumPostProjectionResidualPerS,
			std::fabs(maximumRestorationTarget),0.005f*std::fabs(maximumRestorationTarget));
		if(goldenSubdominance||equalTimeReadmission){
			if(production.interstageFullGridTransferCount!=0u||
				production.residentProjectionInvocationCount!=1u||
				production.conservativeProducerPrecision!=
					RISE::FireStateProducerPrecision::Binary32||
				!production.projection.validationPassed||
				production.projection.executedVCycleCount!=16u||
				production.projection.executedJacobiSweepCount!=1088u)return 120;
		}else if(production.interstageFullGridTransferCount!=0u||
			production.residentProjectionInvocationCount!=2u||
			production.conservativeProducerPrecision!=RISE::FireStateProducerPrecision::Binary32||
			!production.physicalProjection.validationPassed||
			production.projection.executedVCycleCount!=16u||
			production.projection.executedJacobiSweepCount!=1088u)return 120;
		if(!production.projection.validationPassed){
			const bool exactRefusal=slice==0u&&
				production.physicalProjection.executedVCycleCount==17u&&
				production.projection.executedVCycleCount==16u&&
				production.physicalProjection.maximumPreProjectionResidualPerS==
					0x1.2efac6p+5f&&
				production.physicalProjection.maximumPostProjectionResidualPerS==
					0x1.eb58p-9f&&
				production.projection.maximumPreProjectionResidualPerS==
					0x1.c02f92p-13f&&
				production.projection.maximumPostProjectionResidualPerS==
					0x1.4e97dcp-17f&&
				std::fabs(maximumRestorationTarget)==0x1.c02f92p-13f&&
				0.005f*std::fabs(maximumRestorationTarget)==0x1.1ed6c4p-20f&&
				DigestFile(checkpointPath)==checkpointDigest;
			return exactRefusal?244:245;
		}
		if(longShadow){
			constexpr double LowMachValidityCeiling=0x1p-5;
			constexpr double PlateauHeadroomAllowance=(1.0-0x1p-2)*LowMachValidityCeiling;
			const double fieldMaximum=production.maximumAcceptedManifoldDeviation;
			if(limitedClosure){
				const double projectedSteps=25.0/static_cast<double>(
					production.representedTimeStepS);
				const double deviceProjectionHours=projectedSteps*closureDeviceP95MS/3600000.0;
				const double wallProjectionHours=projectedSteps*closureWallP95MS/3600000.0;
				const double followingManifoldStep=production.suggestedManifoldTimeStepS;
				const bool followingStepDerived=production.manifoldNextTimeStepAvailable;
				const char* timingStatus=hostResidualProbe?"measured":"unmeasured";
				char deviceP95Text[64]="unmeasured",wallP95Text[64]="unmeasured";
				char deviceHoursText[64]="invalid",wallHoursText[64]="invalid";
				if(hostResidualProbe){
					std::snprintf(deviceP95Text,sizeof(deviceP95Text),"%.17g",closureDeviceP95MS);
					std::snprintf(wallP95Text,sizeof(wallP95Text),"%.17g",closureWallP95MS);
					std::snprintf(deviceHoursText,sizeof(deviceHoursText),"%.17g",deviceProjectionHours);
					std::snprintf(wallHoursText,sizeof(wallHoursText),"%.17g",wallProjectionHours);
				}
				std::fprintf(stderr,"EQUAL_TIME_LIMITED_PRODUCTION candidate=%u dt=%.17g reference_substeps=%zu "
					"reference_substep_dt=%.17g reference_end=%.17g target_time=%.17g "
					"schedule=%s terminal_target=%s predictor_G=%.17g G=%.17g field_max=%.17g "
					"field_p95=%.17g field_p50=%.17g G_material_authority=%d "
					"initial_audit_dt=%.17g initial_audit_G=%.17g initial_selected_dt=%.17g "
					"initial_calibration=%d "
					"headroom_allowance=%.17g low_mach_ceiling=%.17g headroom_met=%d "
					"delivered_drain=%.17g next_dt_manifold=%.17g limiter_binding=1 "
					"timing_status=%s device_p95_ms=%s wall_p95_ms=%s "
					"tier10_device_hours=%s tier10_wall_hours=%s "
					"passes=%u cell_submaps=%u dual_submaps=%u "
					"source_commits=%u scalar_reads=%u certified_bytes=%llu actual_bytes=%llu "
					"accepted_token=%d golden=%s\n",
					effectiveManifoldRetryCandidate,
					static_cast<double>(production.representedTimeStepS),
					equalTimeReferenceSubstepCount,
					dt/static_cast<double>(equalTimeReferenceSubstepCount),dt,dt,
					equalTimeReferenceScheduleDigest.c_str(),
					equalTimeTerminalTargetDigest.c_str(),
					production.maximumPredictedAdvectiveManifoldAnomaly,
					production.maximumManifoldGeneration,fieldMaximum,
					production.acceptedManifoldDeviationP95,
					production.acceptedManifoldDeviationP50,
					production.manifoldGenerationAuthoritative?1:0,
					0.00057953997747972608,0.024358630180358887,limitedStep,1,
					PlateauHeadroomAllowance,LowMachValidityCeiling,
					fieldMaximum<=PlateauHeadroomAllowance?1:0,
					production.deliveredRestorationDrainFraction,followingManifoldStep,timingStatus,
					deviceP95Text,wallP95Text,deviceHoursText,wallHoursText,
					production.advectiveAnomalyClosurePassCount,
					production.cellSubmapCount,production.dualSubmapCount,
					production.sourceCommandCommitCount,
					production.manifoldScalarDeviceToHostTransferCount,
					static_cast<unsigned long long>(production.combinedCertifiedWorkingSetBytes),
					static_cast<unsigned long long>(production.combinedActualMetalAllocationBytes),
					production.HasAcceptedManifoldToken()?1:0,
					DigestFile(checkpointPath).c_str());
				bool ordinaryAdvanceRefused=false;
				if(effectiveManifoldRetryCandidate==0u){
					RISE::FireProductionResidentStepResult ordinaryResult;
					ordinaryResult.conservativeValues.push_back(1.0f);
					ordinaryAdvanceRefused=!RISE::AdvanceFireProductionResidentStepMetal(
						request,ordinaryResult,&error)&&ordinaryResult.conservativeValues.empty()&&
						ordinaryResult.representedTimeStepS==0.0f&&
						ordinaryResult.maximumManifoldGeneration==0.0&&
						ordinaryResult.suggestedManifoldTimeStepS==0.0&&
						!ordinaryResult.manifoldNextTimeStepAvailable&&
						!ordinaryResult.manifoldPlateauPassed&&!ordinaryResult.HasAcceptedManifoldToken();
				}
				const bool exactPredictorRefusal=effectiveManifoldRetryCandidate==0u&&
					!productionSucceeded&&ordinaryAdvanceRefused&&
					production.manifoldNextTimeStepAvailable&&!production.manifoldPlateauPassed&&
					!production.HasAcceptedManifoldToken()&&followingStepDerived&&
					limitedStep==0.0005576244690940563&&
					production.maximumPredictedAdvectiveManifoldAnomaly==
						0.019734203815460205&&
					production.maximumManifoldGeneration==0.023458957672119141&&
					fieldMaximum==0.023458957672119141&&
					production.deliveredRestorationDrainFraction==0.99965005669264828&&
					followingManifoldStep==0.00055691943198942959&&
					fieldMaximum>PlateauHeadroomAllowance&&
					fieldMaximum<LowMachValidityCeiling&&
					production.advectiveAnomalyClosurePassCount==2u&&
					production.cellSubmapCount==10u&&production.dualSubmapCount==15u&&
					production.sourceCommandCommitCount==2u&&
					production.manifoldScalarDeviceToHostTransferCount==2u&&
					production.interstageFullGridTransferCount==0u&&
					(hostResidualProbe?(std::isfinite(closureDeviceP95MS)&&
						closureDeviceP95MS>=75.0&&closureDeviceP95MS<=150.0&&
						std::isfinite(closureWallP95MS)&&closureWallP95MS>=100.0&&
						closureWallP95MS<=200.0&&deviceProjectionHours>=0.8&&
						deviceProjectionHours<=1.9&&wallProjectionHours>=1.0&&
						wallProjectionHours<=2.5):
						(closureDeviceP95MS==0.0&&closureWallP95MS==0.0&&
						deviceProjectionHours==0.0&&wallProjectionHours==0.0))&&
					equalTimeTerminalTargetDigest==
						"522347125277cf97fe0c58fb4db86e906892ef81182f280b95a92ac35d68f833"&&
					equalTimeReferenceScheduleDigest==
						"1c7944ddde6e673330ccf115c388b0a424027cfcb86e0b8bf8d503f22d7c531d"&&
					DigestFile(checkpointPath)==checkpointDigest;
				unsigned int nextRetryCandidate=0u;double classifiedRetryStep=0.0;
				const RISE::FireProductionResidentStepAttemptDisposition attemptDisposition=
					RISE::ClassifyFireProductionResidentStepAttempt(
						effectiveManifoldRetryCandidate,production,nextRetryCandidate,
						classifiedRetryStep);
				const bool retryAllowed=attemptDisposition==
					RISE::FireProductionResidentStepAttemptDisposition::RetryAtSuggestedTimeStep&&
					classifiedRetryStep==followingManifoldStep;
				const bool retryDispositionValid=!productionSucceeded&&
					production.manifoldNextTimeStepAvailable&&!production.manifoldPlateauPassed&&
					!production.HasAcceptedManifoldToken();
				if(effectiveManifoldRetryCandidate==0u)std::fprintf(stderr,
					"DRAIN_AWARE_RETRY_GATE exact=%d retry_allowed=%d attempt_succeeded=%d "
					"ordinary_refused=%d diagnostics=%d\n",exactPredictorRefusal?1:0,
					retryAllowed?1:0,productionSucceeded?1:0,ordinaryAdvanceRefused?1:0,
					production.manifoldNextTimeStepAvailable?1:0);
				if(exactPredictorRefusal&&retryAllowed){
					std::fprintf(stderr,"DRAIN_AWARE_PLATEAU_RETRY refused_candidate=%u "
						"refused_dt=%.17g field_max=%.17g allowance=%.17g suggested_dt=%.17g "
						"next_candidate=%u cap=%u ordinary_advance_refused=1 attempt_diagnostics=1\n",
						effectiveManifoldRetryCandidate,
						static_cast<double>(production.representedTimeStepS),fieldMaximum,
						PlateauHeadroomAllowance,followingManifoldStep,
						nextRetryCandidate,
						RISE::FireStepRejectionRetryCap);
				}
				const bool controllerAccepted=attemptDisposition==
					RISE::FireProductionResidentStepAttemptDisposition::Accepted;
				bool laterAcceptanceIndexIndependent=true,mutatedAcceptedPayloadRejected=true,
					capAcceptedPayloadRejected=true,overflowAcceptedPayloadRejected=true;
				if(controllerAccepted&&productionSucceeded){
					unsigned int redCandidate=0u;double redStep=0.0;
					laterAcceptanceIndexIndependent=
						RISE::ClassifyFireProductionResidentStepAttempt(2u,production,
							redCandidate,redStep)==
						RISE::FireProductionResidentStepAttemptDisposition::Accepted;
					capAcceptedPayloadRejected=RISE::ClassifyFireProductionResidentStepAttempt(
						RISE::FireStepRejectionRetryCap,production,redCandidate,redStep)==
						RISE::FireProductionResidentStepAttemptDisposition::Rejected;
					overflowAcceptedPayloadRejected=
						RISE::ClassifyFireProductionResidentStepAttempt(
							std::numeric_limits<unsigned int>::max(),production,
							redCandidate,redStep)==
						RISE::FireProductionResidentStepAttemptDisposition::Rejected;
					if(production.conservativeValues.empty())mutatedAcceptedPayloadRejected=false;
					else{
						std::uint32_t bits=0u;std::memcpy(&bits,&production.conservativeValues[0],
							sizeof(bits));const std::uint32_t originalBits=bits;++bits;
						std::memcpy(&production.conservativeValues[0],&bits,sizeof(bits));
						mutatedAcceptedPayloadRejected=
							production.HasAcceptedManifoldToken()&&
							RISE::ClassifyFireProductionResidentStepAttempt(
								effectiveManifoldRetryCandidate,production,redCandidate,redStep)==
								RISE::FireProductionResidentStepAttemptDisposition::Rejected;
						std::memcpy(&production.conservativeValues[0],&originalBits,sizeof(originalBits));
						mutatedAcceptedPayloadRejected=mutatedAcceptedPayloadRejected&&
							production.HasAcceptedManifoldToken()&&
							RISE::ClassifyFireProductionResidentStepAttempt(
								effectiveManifoldRetryCandidate,production,redCandidate,redStep)==
								RISE::FireProductionResidentStepAttemptDisposition::Accepted;
					}
				}
				const bool retryAccepted=effectiveManifoldRetryCandidate>0u&&productionSucceeded&&
					controllerAccepted&&
					laterAcceptanceIndexIndependent&&mutatedAcceptedPayloadRejected&&
					capAcceptedPayloadRejected&&overflowAcceptedPayloadRejected&&
					followingStepDerived&&
					production.manifoldPlateauPassed&&production.HasAcceptedManifoldToken()&&
					effectiveManifoldRetryCandidate==1u&&
					limitedStep==0.00055691943198942959&&
					static_cast<double>(production.representedTimeStepS)==
						0.0005569194327108562&&
					equalTimeReferenceScheduleDigest==
						"1640f2922f8030fe5fe6f983e3c3cae90eb1324bff0b927dde4439df52ed3e3c"&&
					equalTimeTerminalTargetDigest==
						"673d3fc35e9b56e3d04499f37c82aaee9330a8d62ae84f25f49deb7854a9c95a"&&
					production.maximumPredictedAdvectiveManifoldAnomaly==
						0.019709646701812744&&
					production.maximumManifoldGeneration==0.023430228233337402&&
					fieldMaximum==0.023430228233337402&&
					production.acceptedManifoldDeviationP95==5.245208740234375e-06&&
					production.acceptedManifoldDeviationP50==7.152557373046875e-07&&
					!production.manifoldGenerationAuthoritative&&
					production.deliveredRestorationDrainFraction==0.99964850077117484&&
					followingManifoldStep==0.00055689645979380338&&
					fieldMaximum<=PlateauHeadroomAllowance&&
					fieldMaximum<LowMachValidityCeiling&&
					production.advectiveAnomalyClosurePassCount==2u&&
					production.cellSubmapCount==10u&&production.dualSubmapCount==15u&&
					production.sourceCommandCommitCount==2u&&
					production.manifoldScalarDeviceToHostTransferCount==2u&&
					production.interstageFullGridTransferCount==0u&&
					(hostResidualProbe?(std::isfinite(closureDeviceP95MS)&&
						closureDeviceP95MS>=75.0&&closureDeviceP95MS<=150.0&&
						std::isfinite(closureWallP95MS)&&closureWallP95MS>=100.0&&
						closureWallP95MS<=175.0&&deviceProjectionHours>=0.8&&
						deviceProjectionHours<=1.9&&wallProjectionHours>=1.0&&
						wallProjectionHours<=2.0):
						(closureDeviceP95MS==0.0&&closureWallP95MS==0.0&&
						deviceProjectionHours==0.0&&wallProjectionHours==0.0))&&
					DigestFile(checkpointPath)==checkpointDigest;
				if(retryAccepted){
					std::fprintf(stderr,"DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED candidate=%u "
						"dt=%.17g field_max=%.17g field_p95=%.17g field_p50=%.17g "
						"allowance=%.17g next_dt=%.17g "
						"accepted_token=1 golden=%s\n",effectiveManifoldRetryCandidate,
						static_cast<double>(production.representedTimeStepS),fieldMaximum,
						production.acceptedManifoldDeviationP95,
						production.acceptedManifoldDeviationP50,
						PlateauHeadroomAllowance,followingManifoldStep,checkpointDigest);
				}
				if(controllerAccepted&&productionSucceeded&&
					laterAcceptanceIndexIndependent&&mutatedAcceptedPayloadRejected&&
					capAcceptedPayloadRejected&&overflowAcceptedPayloadRejected&&
					(acceptedLongShadow||effectiveManifoldRetryCandidate!=1u||retryAccepted)){
					std::fprintf(stderr,"DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED_GENERIC candidate=%u "
						"dt=%.17g accepted_token=1\n",effectiveManifoldRetryCandidate,
						static_cast<double>(production.representedTimeStepS));
					if(!acceptedLongShadow)return 206;
				}
				if(retryAllowed&&retryDispositionValid){
					if(acceptedLongShadow){
						if(longShadowManifoldRefusalCount[slice]==0u)
							longShadowFirstRefusedField[slice]=fieldMaximum;
						++longShadowManifoldRefusalCount[slice];
					}
					std::fprintf(stderr,"DRAIN_AWARE_PLATEAU_RETRY_CONTINUE refused_candidate=%u "
						"refused_dt=%.17g suggested_dt=%.17g next_candidate=%u cap=%u\n",
						effectiveManifoldRetryCandidate,
						static_cast<double>(production.representedTimeStepS),followingManifoldStep,
						nextRetryCandidate,
						RISE::FireStepRejectionRetryCap);
					// Recursive evaluation otherwise keeps the refused candidate's full
					// resident payload and request alive while the next 1.63-GiB owner is
					// allocated.  The next attempt reloads the shared golden beginning and
					// rebuilds its independently sealed equal-time target.
					if(acceptedLongShadow){
						sliceRetryCandidate=nextRetryCandidate;
						sliceRetryStepS=followingManifoldStep;
						production=RISE::FireProductionResidentStepResult();
						request=RISE::FireProductionResidentStepRequest();
						error.clear();
						continue;
					}
					return continueAfterDrainAwareRefusal(effectiveManifoldRetryCandidate,
						production,&production,&request,&beginning,slice);
				}
				if(!controllerAccepted||!productionSucceeded||
					!laterAcceptanceIndexIndependent||!mutatedAcceptedPayloadRejected||
					!capAcceptedPayloadRejected||!overflowAcceptedPayloadRejected||
					(!acceptedLongShadow&&effectiveManifoldRetryCandidate==1u&&!retryAccepted))return 212;
			}
			if(!production.manifoldPlateauPassed){
				const bool acceptedTokenMinted=production.HasAcceptedManifoldToken();
				if(limitedClosure)return 250;
				const double projectedSteps=25.0/static_cast<double>(
					production.representedTimeStepS);
				const double closureDeviceProjectionHours=
					projectedSteps*closureDeviceP95MS/3600000.0;
				const double closureWallProjectionHours=
					projectedSteps*closureWallP95MS/3600000.0;
				std::fprintf(stderr,"GOLDEN_LONG_SHADOW_REFUSAL step=%zu dt=%.17g G=%.17g "
					"field_max=%.17g low_mach_ceiling=%.17g delivered_drain=%.17g "
					"pre_residual=%.17g post_residual=%.17g passes=%u cell_submaps=%u "
					"dual_submaps=%u source_commits=%u scalar_reads=%u certified_bytes=%llu "
					"actual_bytes=%llu device_p95_ms=%.17g wall_p95_ms=%.17g "
					"tier10_device_hours=%.17g tier10_wall_hours=%.17g "
					"accepted_token=%d golden=%s\n",slice,
					static_cast<double>(production.representedTimeStepS),
					production.maximumManifoldGeneration,fieldMaximum,
					LowMachValidityCeiling,production.deliveredRestorationDrainFraction,
					static_cast<double>(production.projection.maximumPreProjectionResidualPerS),
					static_cast<double>(production.projection.maximumPostProjectionResidualPerS),
					production.advectiveAnomalyClosurePassCount,production.cellSubmapCount,
					production.dualSubmapCount,production.sourceCommandCommitCount,
					production.manifoldScalarDeviceToHostTransferCount,
					static_cast<unsigned long long>(production.combinedCertifiedWorkingSetBytes),
					static_cast<unsigned long long>(production.combinedActualMetalAllocationBytes),
					closureDeviceP95MS,closureWallP95MS,closureDeviceProjectionHours,
					closureWallProjectionHours,
					acceptedTokenMinted?1:0,
					DigestFile(checkpointPath).c_str());
				const bool closureEvidenceExact=disabledClosure?
					(production.maximumManifoldGeneration==0.085895776748657227&&
						fieldMaximum==0.085895776748657227):
					(production.advectiveAnomalyClosurePassCount==2u&&
						production.cellSubmapCount==10u&&production.dualSubmapCount==15u&&
						production.sourceCommandCommitCount==2u&&
						production.manifoldScalarDeviceToHostTransferCount==2u&&
						production.interstageFullGridTransferCount==0u&&
						production.combinedCertifiedWorkingSetBytes==1919317208u&&
						production.combinedActualMetalAllocationBytes==1630052936u&&
						std::isfinite(closureDeviceP95MS)&&closureDeviceP95MS>0.0&&
						std::isfinite(closureWallP95MS)&&closureWallP95MS>0.0&&
						closureWallProjectionHours<=2.0);
				return fieldMaximum>LowMachValidityCeiling&&closureEvidenceExact&&
					!acceptedTokenMinted&&
					DigestFile(checkpointPath)==checkpointDigest?252:250;
			}
			if(!production.manifoldPlateauPassed||!production.HasAcceptedManifoldToken()||
				!std::isfinite(fieldMaximum)||fieldMaximum<0.0||
				fieldMaximum>LowMachValidityCeiling)return 250;
			longShadowFieldMaximum.push_back(fieldMaximum);
			longShadowFieldP95.push_back(production.acceptedManifoldDeviationP95);
			longShadowFieldP50.push_back(production.acceptedManifoldDeviationP50);
			longShadowPredictorGeneration.push_back(
				production.maximumPredictedAdvectiveManifoldAnomaly);
			longShadowAcceptedStepS.push_back(
				static_cast<double>(production.representedTimeStepS));
			longShadowAcceptedCandidate.push_back(effectiveManifoldRetryCandidate);
			longShadowFinalStepS=static_cast<double>(production.representedTimeStepS);
			if(production.physicalProjection.validationPassed)
				++longShadowPhysicalValidationCount;
			if(production.projection.validationPassed)
				++longShadowRestorationValidationCount;
			std::fprintf(stderr,"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=%zu dt=%.17g "
				"predictor_G=%.17g G=%.17g field_max=%.17g field_p95=%.17g "
				"field_p50=%.17g delivered_drain=%.17g "
				"passes=%u cell_submaps=%u source_commits=%u scalar_reads=%u device_ms=%.17g "
				"wall_ms=%.17g\n",
				slice,static_cast<double>(production.representedTimeStepS),
				production.maximumPredictedAdvectiveManifoldAnomaly,
				production.maximumManifoldGeneration,fieldMaximum,
				production.acceptedManifoldDeviationP95,
				production.acceptedManifoldDeviationP50,
				production.deliveredRestorationDrainFraction,
				production.advectiveAnomalyClosurePassCount,production.cellSubmapCount,
				production.sourceCommandCommitCount,
				production.manifoldScalarDeviceToHostTransferCount,
				production.deviceElapsedMS,productionWallMS);
			FireProductionDyadicCalibration::AppendInteger(longShadowTrace,slice);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.maximumManifoldGeneration);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,fieldMaximum);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.acceptedManifoldDeviationP95);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.acceptedManifoldDeviationP50);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.deliveredRestorationDrainFraction);
			FireProductionDyadicCalibration::AppendDouble(longShadowTrace,
				production.projection.maximumPostProjectionResidualPerS);
			RISE::FireProductionAcceptedManifoldObservation acceptedObservation;
			if(!RISE::PublishFireProductionAcceptedManifoldObservation(
				static_cast<double>(production.representedTimeStepS),production,
				acceptedObservation,&error)||
				!FireProductionDyadicCalibration::ApplyAcceptedProductionResult(
					production,acceptedObservation,beginning,error)){
				std::fprintf(stderr,"GOLDEN_LONG_SHADOW publication failed: %s\n",error.c_str());
				return 251;
			}
			const double representedStep=static_cast<double>(production.representedTimeStepS);
			beginning.simulationTimeS+=representedStep;
			beginning.previousStepS=representedStep;
			beginning.lastAcceptedStepS=representedStep;
			++beginning.acceptedSteps;
			beginning.values.acceptedTimeStepHistoryS.push_back(representedStep);
			beginning.productionManifoldObservation=acceptedObservation;
			if(acceptedLongShadow){
				longShadowReferenceConservative=std::move(acceptedReferenceConservative);
				longShadowReferenceMomentum=std::move(acceptedReferenceMomentum);
			}
			longShadowState=std::move(beginning);
			sliceAccepted=true;
			continue;
		}
		::FireProductionCalibration::ResidentStep64Result production64;
		if(!::FireProductionCalibration::AdvanceResidentStep64(request,
			static_cast<double>(production.forceDiagnostics.outwardLambdaPerS),production64,&error)){
			std::fprintf(stderr,"production golden fp64 slice %zu failed: %s\n",slice,
				error.c_str());return 131;}
		if(production64.force.schedule.substepCount!=production.forceSchedule.substepCount||
			!production64.projection.validationPassed||
			production64.projection.executedVCycleCount!=
				production.projection.executedVCycleCount)return 132;
		const bool precisionRestorationActive=
			production.residentProjectionInvocationCount==2u;
		if(precisionRestorationActive!=
			(production64.physicalProjection.executedVCycleCount!=0u)||
			(precisionRestorationActive&&(!production64.physicalProjection.validationPassed||
				production64.physicalProjection.executedVCycleCount!=
					production.physicalProjection.executedVCycleCount)))return 132;
		std::vector<ConservativeVector> conservative32,conservative64;
		PeriodicMACField velocity32,velocity64;
		FireProductionDyadicCalibration::FilteredField filtered32,filtered64;
		FireProductionDyadicCalibration::FilteredVelocityField filteredVelocity32,
			filteredVelocity64;
		if(!FireProductionDyadicCalibration::UnpackProductionConservative(
			production.conservativeValues,cells,conservative32)||
			!FireProductionDyadicCalibration::UnpackProductionConservative(
			production64.conservativeValues,cells,conservative64)||
			!FireProductionDyadicCalibration::UnpackProductionVelocity(
			production.projection.velocityMPerS,velocity32)||
			!FireProductionDyadicCalibration::UnpackProductionVelocity(
			production64.projection.velocityMPerS,velocity64)||
			!FireProductionDyadicCalibration::FilterConservative(beginning,conservative32,
				beginning.values.characteristicDiameterM,filtered32)||
			!FireProductionDyadicCalibration::FilterConservative(beginning,conservative64,
				beginning.values.characteristicDiameterM,filtered64)||
			!FireProductionDyadicCalibration::FilterVelocity(beginning,velocity32,
				beginning.values.characteristicDiameterM,filteredVelocity32)||
			!FireProductionDyadicCalibration::FilterVelocity(beginning,velocity64,
				beginning.values.characteristicDiameterM,filteredVelocity64))return 133;
		const std::array<double,9> precisionScalar=
			FireProductionDyadicCalibration::FieldDistance(filtered32,filtered64);
		const double precisionVelocity=FireProductionDyadicCalibration::VelocityDistance(
			filteredVelocity32,filteredVelocity64);
		const std::array<double,9> inventory32=
			FireProductionDyadicCalibration::ComponentInventoryDensity(conservative32);
		const std::array<double,9> inventory64=
			FireProductionDyadicCalibration::ComponentInventoryDensity(conservative64);
		if(equalTimeReadmission){
			FireProductionDyadicCalibration::FilteredField filteredOracle;
			FireProductionDyadicCalibration::FilteredVelocityField filteredOracleVelocity;
			if(!FireProductionDyadicCalibration::FilterConservative(beginning,
				oracle.conservative,beginning.values.characteristicDiameterM,filteredOracle)||
				!FireProductionDyadicCalibration::FilterVelocity(beginning,
				oracle.velocityMPerS,beginning.values.characteristicDiameterM,
				filteredOracleVelocity))return 186;
			const std::array<double,9> schemeScalar=
				FireProductionDyadicCalibration::FieldDistance(filtered32,filteredOracle);
			const double schemeVelocity=FireProductionDyadicCalibration::VelocityDistance(
				filteredVelocity32,filteredOracleVelocity);
			const std::array<double,9> oracleInventory=
				FireProductionDyadicCalibration::ComponentInventoryDensity(oracle.conservative);
			for(std::size_t component=0u;component<9u;++component){
				double scalarTolerance=0.0,inventoryTolerance=0.0;
				const double schemeInventory=std::fabs(inventory32[component]-
					oracleInventory[component]);
				const bool scalarToleranceDefined=FireProductionCalibration::TriangleTolerance(
					productionDistance[component],productionTemporal[component],
					oracleDistance[component],oracleTemporal[component],scalarBound[component],
					scalarTolerance);
				const bool inventoryToleranceDefined=FireProductionCalibration::TriangleTolerance(
					inventoryDistance[component],productionInventoryTemporal[component],
					oracleInventoryDistance[component],oracleInventoryTemporal[component],
					inventoryBound[component],inventoryTolerance);
				const bool scalarPassed=scalarToleranceDefined&&
					schemeScalar[component]<=scalarTolerance;
				const bool inventoryPassed=inventoryToleranceDefined&&
					schemeInventory<=inventoryTolerance;
				readmissionGateCount+=2u;
				readmissionFailureCount+=scalarPassed?0u:1u;
				readmissionFailureCount+=inventoryPassed?0u:1u;
				const double scalarRatio=schemeScalar[component]/scalarTolerance;
				const double inventoryRatio=schemeInventory/inventoryTolerance;
				maximumScalarContractRatio=std::max(maximumScalarContractRatio,scalarRatio);
				maximumInventoryContractRatio=std::max(maximumInventoryContractRatio,
					inventoryRatio);
				FireProductionDyadicCalibration::AppendDouble(readmissionTrace,
					schemeScalar[component]);
				FireProductionDyadicCalibration::AppendDouble(readmissionTrace,scalarTolerance);
				FireProductionDyadicCalibration::AppendDouble(readmissionTrace,schemeInventory);
				FireProductionDyadicCalibration::AppendDouble(readmissionTrace,inventoryTolerance);
				std::fprintf(stderr,"equal-time readmission slice=%zu component=%zu "
					"scalar=%.17g tolerance=%.17g ratio=%.17g accepted=%d "
					"inventory=%.17g inventory_tolerance=%.17g inventory_ratio=%.17g "
					"inventory_accepted=%d\n",slice,component,schemeScalar[component],
					scalarTolerance,scalarRatio,scalarPassed?1:0,schemeInventory,
					inventoryTolerance,inventoryRatio,inventoryPassed?1:0);
			}
			double velocityTolerance=0.0;
			const bool velocityToleranceDefined=FireProductionCalibration::TriangleTolerance(
				velocityDistance,ProductionVelocityTemporal,oracleVelocityDistance,
				OracleVelocityTemporal,velocityBound,velocityTolerance);
			const bool velocityPassed=velocityToleranceDefined&&schemeVelocity<=velocityTolerance;
			++readmissionGateCount;readmissionFailureCount+=velocityPassed?0u:1u;
			velocityContractRatio=std::max(velocityContractRatio,
				schemeVelocity/velocityTolerance);
			FireProductionDyadicCalibration::AppendDouble(readmissionTrace,schemeVelocity);
			FireProductionDyadicCalibration::AppendDouble(readmissionTrace,velocityTolerance);
			std::fprintf(stderr,"equal-time readmission slice=%zu velocity=%.17g "
				"tolerance=%.17g ratio=%.17g accepted=%d reference_substeps=%zu "
				"schedule=%s\n",slice,schemeVelocity,velocityTolerance,
				schemeVelocity/velocityTolerance,velocityPassed?1:0,
				equalTimeReferenceSubstepCount,equalTimeReferenceScheduleDigest.c_str());
		}
		for(std::size_t component=0u;component<9u;++component){
			const double precisionInventory=std::fabs(inventory32[component]-
				inventory64[component]);
			if(!(precisionScalar[component]<=scalarBound[component])||
				!(precisionInventory<=inventoryBound[component]))return 134;
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,
				precisionScalar[component]);
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,
				precisionInventory);
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,scalarBound[component]);
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,inventoryBound[component]);
			maximumPrecisionScalar[component]=std::max(maximumPrecisionScalar[component],
				precisionScalar[component]);
			maximumPrecisionInventory[component]=std::max(
				maximumPrecisionInventory[component],precisionInventory);
			minimumScalarSubdominanceMargin=std::min(minimumScalarSubdominanceMargin,
				precisionScalar[component]>0.0?scalarBound[component]/precisionScalar[component]:
				std::numeric_limits<double>::infinity());
			minimumInventorySubdominanceMargin=std::min(minimumInventorySubdominanceMargin,
				precisionInventory>0.0?inventoryBound[component]/precisionInventory:
				std::numeric_limits<double>::infinity());
			precisionGateCount+=2u;
		}
		if(!(precisionVelocity<=velocityBound))return 134;
		FireProductionDyadicCalibration::AppendDouble(precisionTrace,precisionVelocity);
		FireProductionDyadicCalibration::AppendDouble(precisionTrace,velocityBound);
		maximumPrecisionVelocity=std::max(maximumPrecisionVelocity,precisionVelocity);
		minimumVelocitySubdominanceMargin=std::min(minimumVelocitySubdominanceMargin,
			precisionVelocity>0.0?velocityBound/precisionVelocity:
			std::numeric_limits<double>::infinity());
		++precisionGateCount;
		FireProductionDyadicCalibration::AppendInteger(precisionTrace,
			production.forceSchedule.substepCount);
		FireProductionDyadicCalibration::AppendInteger(precisionTrace,
			production.residentProjectionInvocationCount);
		std::fprintf(stderr,"golden subdominance slice=%zu velocity=%.17g bound=%.17g "
			"margin=%.17g scalar_value_bound_margin=",slice,precisionVelocity,velocityBound,
			precisionVelocity>0.0?velocityBound/precisionVelocity:
			std::numeric_limits<double>::infinity());
		for(std::size_t component=0u;component<9u;++component)
			std::fprintf(stderr," %.17g/%.17g/%.17g",precisionScalar[component],
				scalarBound[component],precisionScalar[component]>0.0?
				scalarBound[component]/precisionScalar[component]:
				std::numeric_limits<double>::infinity());
		std::fprintf(stderr," inventory_value_bound_margin=");
		for(std::size_t component=0u;component<9u;++component){
			const double value=std::fabs(inventory32[component]-inventory64[component]);
			std::fprintf(stderr," %.17g/%.17g/%.17g",value,inventoryBound[component],
				value>0.0?inventoryBound[component]/value:
				std::numeric_limits<double>::infinity());
		}
		std::fprintf(stderr,"\n");
		projectionResiduals[slice]=production.projection.maximumPostProjectionResidualPerS;
		oracleProjectionResiduals[slice]=oracle.maximumDivergenceResidualPerS;
		oraclePreProjectionResiduals[slice]=oracle.maximumPreProjectionDivergenceResidualPerS;
		if(!production.projection.validationPassed)++monitoredProjectionMisses;
		for(const float value:production.conservativeValues)if(!std::isfinite(value))return 127;
		for(unsigned int axis=0u;axis<3u;++axis){
			for(const float value:production.transportedDual.auxiliaryFaceDensity[axis])
				if(!(value>0.0f)||!std::isfinite(value))return 127;
			for(const float value:production.transportedDual.momentum[axis])
				if(!std::isfinite(value))return 127;
			for(const float value:production.projection.velocityMPerS[axis])
				if(!std::isfinite(value))return 127;
		}
		for(std::size_t component=0u;component<8u;++component)
			for(std::size_t cell=0u;cell<cells;++cell)minimumConservativeDensity=
				std::min(minimumConservativeDensity,
					static_cast<double>(production.conservativeValues[component*cells+cell]));
		for(std::size_t probe=0u;probe<steepFrontCoordinates.size();++probe){
			const std::size_t component=steepFrontComponents[probe];
			const std::size_t px=steepFrontCoordinates[probe][0],py=steepFrontCoordinates[probe][1];
			const std::size_t pz=steepFrontCoordinates[probe][2];
			double lower=static_cast<double>(request.cellTransport.ambientValues[component]);
			double upper=lower;
			for(std::size_t z=pz-1u;z<=pz+1u;++z)for(std::size_t y=py-1u;y<=py+1u;++y)
				for(std::size_t x=px-1u;x<=px+1u;++x){const double value=
					conservative[cellIndex(x,y,z)][component];lower=std::min(lower,value);
					upper=std::max(upper,value);}
			const double observed=production.conservativeValues[
				component*cells+cellIndex(px,py,pz)];
			minimumPinnedProbeContrast[probe]=std::min(minimumPinnedProbeContrast[probe],upper-lower);
			maximumPinnedProbeOvershoot=std::max(maximumPinnedProbeOvershoot,
				std::max(lower-observed,observed-upper));
		}
		maximumProductionResidual=std::max(maximumProductionResidual,
			static_cast<double>(production.projection.maximumPostProjectionResidualPerS));
		maximumOracleResidual=std::max(maximumOracleResidual,oracle.maximumDivergenceResidualPerS);
		for(std::size_t component=0u;component<9u;++component){
			double oracleSum=0.0,productionSum=0.0,oracleMinimum=std::numeric_limits<double>::infinity();
			double oracleMaximum=-std::numeric_limits<double>::infinity();
			double componentAbsolute=0.0;
			for(std::size_t cell=0u;cell<cells;++cell){
				const double reference=oracle.conservative[cell][component];
				const double observed=production.conservativeValues[component*cells+cell];
				const double serial=oracleSerial.conservative[cell][component];
				oracleSum+=reference;productionSum+=observed;
				oracleMinimum=std::min(oracleMinimum,reference);oracleMaximum=std::max(oracleMaximum,reference);
				componentAbsolute=std::max(componentAbsolute,std::fabs(observed-reference));
				maximumOracleThreadScalar=std::max(maximumOracleThreadScalar,std::fabs(serial-reference));
			}
			maximumScalarAbsolute=std::max(maximumScalarAbsolute,componentAbsolute);
			const double scale=std::max({1.0,std::fabs(oracleMinimum),std::fabs(oracleMaximum)});
			maximumScalarRelative=std::max(maximumScalarRelative,componentAbsolute/scale);
			maximumEnvelopeOvershoot=std::max(maximumEnvelopeOvershoot,std::max(0.0,
				std::max(oracleMinimum-*std::min_element(production.conservativeValues.begin()+component*cells,
					production.conservativeValues.begin()+(component+1u)*cells),
					*std::max_element(production.conservativeValues.begin()+component*cells,
					production.conservativeValues.begin()+(component+1u)*cells)-oracleMaximum)));
			maximumLedgerRelative=std::max(maximumLedgerRelative,
				std::fabs(productionSum-oracleSum)/std::max(1.0,std::fabs(oracleSum)));
		}
		double productionMaximumVelocity=0.0;
		for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t face=0u;
			face<production.projection.velocityMPerS[axis].size();++face){
			productionMaximumVelocity=std::max(productionMaximumVelocity,std::fabs(
				static_cast<double>(production.projection.velocityMPerS[axis][face])));
			productionMaximumVelocity=std::max(productionMaximumVelocity,std::fabs(
				static_cast<double>(production.transportedDual.momentum[axis][face])/
				static_cast<double>(production.projection.faceDensityKGPerM3[axis][face])));
			maximumVelocityAbsolute=std::max(maximumVelocityAbsolute,std::fabs(
				static_cast<double>(production.projection.velocityMPerS[axis][face])-
				oracle.velocityMPerS.component[axis][face]));
			maximumOracleThreadVelocity=std::max(maximumOracleThreadVelocity,std::fabs(
				oracleSerial.velocityMPerS.component[axis][face]-oracle.velocityMPerS.component[axis][face]));
		}
		double independentProductionResidual=0.0;
		for(std::size_t z=0u;z<shape.nz;++z)for(std::size_t y=0u;y<shape.ny;++y)
			for(std::size_t x=0u;x<shape.nx;++x){
				double divergence=0.0;
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t low=faceIndex(axis,x,y,z);
					const std::size_t high=faceIndex(axis,axis==0u?x+1u:x,
						axis==1u?y+1u:y,axis==2u?z+1u:z);
					divergence+=(static_cast<double>(production.projection.velocityMPerS[axis][high])-
						static_cast<double>(production.projection.velocityMPerS[axis][low]))/shape.cellWidthM;
				}
				independentProductionResidual=std::max(independentProductionResidual,std::fabs(
					divergence-static_cast<double>(request.divergenceTargetPerS[cellIndex(x,y,z)])));
			}
		independentProjectionResiduals[slice]=independentProductionResidual;
		reductionRatios[slice]=
			(static_cast<double>(production.projection.maximumPostProjectionResidualPerS)/
			 static_cast<double>(production.projection.maximumPreProjectionResidualPerS))/
			(oracle.maximumDivergenceResidualPerS/oracle.maximumPreProjectionDivergenceResidualPerS);
		const double validationBand=0.005*productionMaximumVelocity/
			(static_cast<double>(request.force.shape.cellWidthM)*
			std::max({request.force.shape.nx,request.force.shape.ny,request.force.shape.nz}));
		std::fprintf(stderr,"golden resident slice=%zu step=%llu dt=%.17g scalar_abs=%.17g "
			"scalar_rel=%.17g velocity_abs=%.17g ledger_rel=%.17g envelope=%.17g "
			"prod_pre=%.17g prod_residual=%.17g independent_residual=%.17g "
			"oracle_pre=%.17g oracle_residual=%.17g band=%.17g reduction=%.17g "
			"oracle_reduction=%.17g reduction_ratio=%.17g oracle_thread_scalar=%.17g "
			"oracle_thread_velocity=%.17g validation=%d\n",slice,
			static_cast<unsigned long long>(beginning.acceptedSteps+1u),dt,maximumScalarAbsolute,
			maximumScalarRelative,maximumVelocityAbsolute,maximumLedgerRelative,maximumEnvelopeOvershoot,
			static_cast<double>(production.projection.maximumPreProjectionResidualPerS),
			static_cast<double>(production.projection.maximumPostProjectionResidualPerS),
			independentProductionResidual,oracle.maximumPreProjectionDivergenceResidualPerS,
			oracle.maximumDivergenceResidualPerS,validationBand,
			static_cast<double>(production.projection.maximumPostProjectionResidualPerS)/
				static_cast<double>(production.projection.maximumPreProjectionResidualPerS),
			oracle.maximumDivergenceResidualPerS/oracle.maximumPreProjectionDivergenceResidualPerS,
			reductionRatios[slice],
			maximumOracleThreadScalar,maximumOracleThreadVelocity,
			production.projection.validationPassed?1:0);
			sliceAccepted=true;
		}
	}
	if(equalTimeReadmission){
		const std::string trace=RISECBOR64::SHA256Hex(readmissionTrace);
		std::fprintf(stderr,"equal-time readmission complete slices=8 gates=%zu failures=%zu "
			"scalar_max_ratio=%.17g velocity_ratio=%.17g inventory_max_ratio=%.17g "
			"original_excesses=83/28/8.7 manifold_floor_attribution=%d trace=%s golden=%s\n",
			readmissionGateCount,readmissionFailureCount,maximumScalarContractRatio,
			velocityContractRatio,maximumInventoryContractRatio,
			readmissionFailureCount>0u?1:0,trace.c_str(),checkpointDigest);
		return readmissionGateCount==152u&&DigestFile(checkpointPath)==checkpointDigest?
			(readmissionFailureCount==0u?188:187):186;
	}
	if(longShadow){
		if(monitoredLongShadow){
			if(monitoredLongShadowSmoke){
				const bool exact=longShadowFieldMaximum.size()==1u&&
					longShadowFieldP95.size()==1u&&longShadowFieldP50.size()==1u&&
					longShadowDeviceMS.size()==1u&&longShadowWallMS.size()==1u&&
					longShadowActiveLimit==std::vector<std::string>{"advective_CFL"}&&
					longShadowMaximumVelocity.size()==1u&&
					longShadowMaximumReducedGravity.size()==1u&&
					longShadowMaximumDiffusivity.size()==1u&&
					longShadowPhysicalValidationCount==1u&&
					longShadowRestorationValidationCount==0u&&
					longShadowTailCellCount==std::vector<std::uint32_t>{0u}&&
					longShadowTailExcessSum==std::vector<double>{0.0}&&
					longShadowTailDrainedVolumeM3==std::vector<double>{0.0}&&
					longShadowAcceptedCandidate==std::vector<unsigned int>{0u}&&
					longShadowFinalStepS==static_cast<double>(0.0016462659696117043f)&&
					DigestFile(checkpointPath)==checkpointDigest;
				std::fprintf(stderr,"MONITORED_MANIFOLD_SHADOW_SMOKE dt=%.17g "
					"field_max=%.17g field_p95=%.17g field_p50=%.17g "
					"allowance_crossed=%zu ceiling_crossed=%zu projection_valid=1 "
					"restoration_passes=0 accepted=1 trace=%s golden=%s\n",
					longShadowFinalStepS,longShadowFieldMaximum.front(),
					longShadowFieldP95.front(),longShadowFieldP50.front(),
					longShadowAllowanceCrossings,longShadowCeilingCrossings,
					RISECBOR64::SHA256Hex(longShadowTrace).c_str(),checkpointDigest);
				return exact?195:197;
			}
			if(longShadowFieldMaximum.size()!=LongShadowSteps||
				longShadowFieldP95.size()!=LongShadowSteps||
				longShadowFieldP50.size()!=LongShadowSteps||
				longShadowDeviceMS.size()!=LongShadowSteps||
				longShadowWallMS.size()!=LongShadowSteps||
				longShadowActiveLimit.size()!=LongShadowSteps||
				longShadowMaximumVelocity.size()!=LongShadowSteps||
				longShadowMaximumReducedGravity.size()!=LongShadowSteps||
				longShadowMaximumDiffusivity.size()!=LongShadowSteps||
				longShadowTailCellCount.size()!=LongShadowSteps||
				longShadowTailExcessSum.size()!=LongShadowSteps||
				longShadowTailDrainedVolumeM3.size()!=LongShadowSteps||
				longShadowPhysicalValidationCount!=LongShadowSteps||
				longShadowRestorationValidationCount!=std::count_if(
					longShadowTailCellCount.begin(),longShadowTailCellCount.end(),
					[](const std::uint32_t value){return value>0u;}))return 197;
			std::vector<double> sortedDevice=longShadowDeviceMS,sortedWall=longShadowWallMS;
			std::sort(sortedDevice.begin(),sortedDevice.end());
			std::sort(sortedWall.begin(),sortedWall.end());
			const std::size_t p95Index=(95u*LongShadowSteps+99u)/100u-1u;
			const double deviceP95=sortedDevice[p95Index],wallP95=sortedWall[p95Index];
			const double simulatedDuration=std::accumulate(longShadowAcceptedStepS.begin(),
				longShadowAcceptedStepS.end(),0.0);
			const double averageStep=simulatedDuration/static_cast<double>(LongShadowSteps);
			const double projectedSteps=25.0/averageStep;
			const double deviceP95Hours=projectedSteps*deviceP95/3600000.0;
			const double wallP95Hours=projectedSteps*wallP95/3600000.0;
			const double deviceMeanRateHours=25.0*std::accumulate(longShadowDeviceMS.begin(),
				longShadowDeviceMS.end(),0.0)/(simulatedDuration*3600000.0);
			const double wallMeanRateHours=25.0*std::accumulate(longShadowWallMS.begin(),
				longShadowWallMS.end(),0.0)/(simulatedDuration*3600000.0);
			const double maximum=*std::max_element(longShadowFieldMaximum.begin(),
				longShadowFieldMaximum.end());
			const double maximumVelocity=*std::max_element(longShadowMaximumVelocity.begin(),
				longShadowMaximumVelocity.end());
			const double p95Maximum=*std::max_element(longShadowFieldP95.begin(),
				longShadowFieldP95.end());
			const double p50Maximum=*std::max_element(longShadowFieldP50.begin(),
				longShadowFieldP50.end());
			const std::uint32_t tailPopulationPeak=*std::max_element(
				longShadowTailCellCount.begin(),longShadowTailCellCount.end());
			const double tailDrainTotal=std::accumulate(longShadowTailDrainedVolumeM3.begin(),
				longShadowTailDrainedVolumeM3.end(),0.0);
			const std::uint32_t totalHardBoundRetries=std::accumulate(
				longShadowManifoldRefusalCount.begin(),longShadowManifoldRefusalCount.end(),0u);
			const std::string traceDigest=RISECBOR64::SHA256Hex(longShadowTrace);
			const std::string finalStateDigest=
				FireProductionDyadicCalibration::AnalyticStateDigest(longShadowState);
			std::fprintf(stderr,"MONITORED_MANIFOLD_SHADOW_COMPLETE steps=%zu first_dt=%.17g "
				"final_dt=%.17g average_dt=%.17g simulated_time=%.17g "
				"max_peak=%.17g p95_peak=%.17g p50_peak=%.17g final_max=%.17g "
				"final_p95=%.17g final_p50=%.17g allowance_crossings=%zu "
				"ceiling_crossings=%zu physical_validations=%zu restoration_passes=%zu "
				"tail_population_peak=%u tail_drain_total_m3=%.17g max_velocity=%.17g "
				"hard_bound_retries=%u "
				"step33_refusals=%u step33_first_refused_field=%.17g step33_candidate=%u "
				"device_p95_ms=%.17g wall_p95_ms=%.17g tier10_device_hours=%.17g "
				"tier10_wall_hours=%.17g tier10_device_mean_rate_hours=%.17g "
				"tier10_wall_mean_rate_hours=%.17g trace=%s final_state=%s golden=%s\n",
				LongShadowSteps,longShadowAcceptedStepS.front(),longShadowFinalStepS,
				averageStep,simulatedDuration,maximum,p95Maximum,p50Maximum,
				longShadowFieldMaximum.back(),longShadowFieldP95.back(),
				longShadowFieldP50.back(),longShadowAllowanceCrossings,
				longShadowCeilingCrossings,longShadowPhysicalValidationCount,
				longShadowRestorationValidationCount,tailPopulationPeak,tailDrainTotal,
				maximumVelocity,totalHardBoundRetries,longShadowManifoldRefusalCount[33u],
				longShadowFirstRefusedField[33u],longShadowAcceptedCandidate[33u],
				deviceP95,wallP95,deviceP95Hours,wallP95Hours,deviceMeanRateHours,
				wallMeanRateHours,traceDigest.c_str(),
				finalStateDigest.c_str(),DigestFile(checkpointPath).c_str());
			return longShadowAcceptedStepS.front()==
					static_cast<double>(0.0016462659696117043f)&&
				std::all_of(longShadowAcceptedStepS.begin(),longShadowAcceptedStepS.end(),
					[](const double value){return std::isfinite(value)&&value>0.0;})&&
				std::none_of(longShadowActiveLimit.begin(),longShadowActiveLimit.end(),
					[](const std::string& value){return value=="unknown";})&&
				std::all_of(longShadowAcceptedCandidate.begin(),
					longShadowAcceptedCandidate.end(),[](const unsigned int value){
						return value<RISE::FireStepRejectionRetryCap;})&&
				maximum==0.15430498123168945&&maximumVelocity==10.871506690979004&&
				tailPopulationPeak==9698u&&tailDrainTotal==0.063814808515304383&&
				longShadowRestorationValidationCount==100u&&totalHardBoundRetries==0u&&
				longShadowManifoldRefusalCount[33u]==0u&&
				longShadowAcceptedCandidate[33u]==0u&&
				longShadowFieldMaximum[33u]==0.14402782917022705&&
				std::isfinite(deviceP95)&&deviceP95>0.0&&
				std::isfinite(wallP95)&&wallP95>0.0&&
				DigestFile(checkpointPath)==checkpointDigest?196:197;
		}
		if(longShadowFieldMaximum.size()!=LongShadowSteps||
			longShadowFieldP95.size()!=LongShadowSteps||
			longShadowFieldP50.size()!=LongShadowSteps||
			longShadowPredictorGeneration.size()!=LongShadowSteps||
			longShadowAcceptedStepS.size()!=LongShadowSteps||
			longShadowAcceptedCandidate.size()!=LongShadowSteps)return 211;
		const std::size_t first=longShadowFieldMaximum.size()-2u*LongShadowWindow;
		const double priorMaximum=*std::max_element(longShadowFieldMaximum.begin()+first,
			longShadowFieldMaximum.begin()+first+LongShadowWindow);
		const double terminalMaximum=*std::max_element(
			longShadowFieldMaximum.begin()+first+LongShadowWindow,longShadowFieldMaximum.end());
		const double priorP95=*std::max_element(longShadowFieldP95.begin()+first,
			longShadowFieldP95.begin()+first+LongShadowWindow);
		const double terminalP95=*std::max_element(
			longShadowFieldP95.begin()+first+LongShadowWindow,longShadowFieldP95.end());
		const double priorP50=*std::max_element(longShadowFieldP50.begin()+first,
			longShadowFieldP50.begin()+first+LongShadowWindow);
		const double terminalP50=*std::max_element(
			longShadowFieldP50.begin()+first+LongShadowWindow,longShadowFieldP50.end());
		const bool maximumPassed=FireProductionCalibration::LongShadowNonsecular(
			longShadowFieldMaximum);
		const bool p95Passed=FireProductionCalibration::LongShadowNonsecular(
			longShadowFieldP95);
		const bool p50Passed=FireProductionCalibration::LongShadowNonsecular(
			longShadowFieldP50);
		std::fprintf(stderr,"GOLDEN_LONG_SHADOW steps=%zu dt=%.17g prior32_max=%.17g "
			"terminal32_max=%.17g prior32_p95=%.17g terminal32_p95=%.17g "
			"prior32_p50=%.17g terminal32_p50=%.17g "
			"final_max=%.17g final_p95=%.17g final_p50=%.17g "
			"low_mach_ceiling=%.17g max_nonsecular=%d p95_nonsecular=%d p50_nonsecular=%d "
			"physical_validations=%zu restoration_validations=%zu trace=%s final_state=%s "
			"golden=%s\n",
			longShadowFieldMaximum.size(),longShadowFinalStepS,priorMaximum,terminalMaximum,
			priorP95,terminalP95,priorP50,terminalP50,
			longShadowFieldMaximum.back(),longShadowFieldP95.back(),longShadowFieldP50.back(),
			0x1p-5,maximumPassed?1:0,p95Passed?1:0,p50Passed?1:0,
			longShadowPhysicalValidationCount,longShadowRestorationValidationCount,
			RISECBOR64::SHA256Hex(longShadowTrace).c_str(),
			FireProductionDyadicCalibration::AnalyticStateDigest(longShadowState).c_str(),
			DigestFile(checkpointPath).c_str());
		return FireProductionCalibration::LongShadowDistributionNonsecular(
				longShadowFieldMaximum,longShadowFieldP95,longShadowFieldP50)&&
			maximumPassed&&p95Passed&&p50Passed&&
			longShadowPhysicalValidationCount==LongShadowSteps&&
			longShadowRestorationValidationCount==LongShadowSteps&&
			DigestFile(checkpointPath)==checkpointDigest?248:252;
	}
	std::fprintf(stderr,"golden resident aggregate scalar_abs=%.17g scalar_rel=%.17g "
		"velocity_abs=%.17g ledger_rel=%.17g envelope=%.17g probe_overshoot=%.17g "
		"minimum_beginning_density=%.17g minimum_density=%.17g oracle_thread_scalar=%.17g "
		"oracle_thread_velocity=%.17g prod_residual=%.17g oracle_residual=%.17g "
		"probe_contrast_0=%.17g probe_contrast_1=%.17g misses=%u\n",
		maximumScalarAbsolute,maximumScalarRelative,maximumVelocityAbsolute,maximumLedgerRelative,
		maximumEnvelopeOvershoot,maximumPinnedProbeOvershoot,minimumBeginningDensity,
		minimumConservativeDensity,
		maximumOracleThreadScalar,maximumOracleThreadVelocity,
		maximumProductionResidual,maximumOracleResidual,minimumPinnedProbeContrast[0],
		minimumPinnedProbeContrast[1],monitoredProjectionMisses);
	const std::string precisionDigest=RISECBOR64::SHA256Hex(precisionTrace);
	std::fprintf(stderr,"golden subdominance complete trace=%s slices=8 gates=%zu "
		"physical_retries=%zu velocity_max=%.17g velocity_bound=%.17g "
		"velocity_margin=%.17g scalar_min_margin=%.17g inventory_min_margin=%.17g\n",
		precisionDigest.c_str(),precisionGateCount,physicalRetryCount,
		maximumPrecisionVelocity,velocityBound,minimumVelocitySubdominanceMargin,
		minimumScalarSubdominanceMargin,minimumInventorySubdominanceMargin);
	return DigestFile(checkpointPath)==checkpointDigest?(goldenSubdominance?190:243):135;
}
