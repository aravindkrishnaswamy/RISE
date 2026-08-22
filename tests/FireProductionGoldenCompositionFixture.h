int RunProductionGoldenCompositionFixture(const std::filesystem::path& checkpointPath,
	const std::filesystem::path& snapshotDirectory)
{
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
	if(plateauProbe&&manifoldProbe)return 255;
	std::array<double,9> productionDistance={{}},scalarBound={{}},inventoryDistance={{}},
		inventoryBound={{}};double velocityDistance=0.0,velocityBound=0.0;
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
			inventoryBound[component]))return 130;
	if(!FireProductionCalibration::Tier6DistanceFromDyadicPairs(
		FireProductionDyadicCalibration::ExpectedProductionVelocityEvidence[0],
		FireProductionDyadicCalibration::ExpectedProductionVelocityEvidence[1],
		FireProductionDyadicCalibration::VerifiedOrder,velocityDistance,velocityBound))return 130;
	RISECBOR64::Bytes precisionTrace;
	for(std::size_t slice=0u;slice<8u;++slice){
		const std::filesystem::path beginningPath=slice==0u?checkpointPath:
			snapshotDirectory/(std::string("step_0")+std::to_string(slice)+".checkpoint");
		MethaneRunCheckpoint beginning;
		if(DigestFile(beginningPath)!=beginningDigests[slice]||
			!LoadMethaneRunCheckpoint(beginningPath,beginning,error)||
			beginning.acceptedSteps!=3479u+slice){std::fprintf(stderr,
			"production golden composition beginning %zu failed: %s\n",slice,error.c_str());return 113;}
		PeriodicMACShape shape;shape.nx=beginning.dimensions[0];shape.ny=beginning.dimensions[1];
		shape.nz=beginning.dimensions[2];shape.cellWidthM=beginning.cellWidthM;
		const std::size_t cells=shape.CellCount();const double dt=fromBits(stepBits[slice]);
		if(beginning.states.size()!=cells)return 114;
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell)conservative[cell]=ToConservativeVector(beginning.states[cell]);
		for(std::size_t component=0u;component<8u;++component)
			for(std::size_t cell=0u;cell<cells;++cell)minimumBeginningDensity=
				std::min(minimumBeginningDensity,conservative[cell][component]);
		ConservativeAdvance3DConfig shadowConfig;shadowConfig.transport.cellWidthM=shape.cellWidthM;
		shadowConfig.transport.deltaTimeS=dt;shadowConfig.transport.ambientTemperatureK=300.0;
		shadowConfig.transport.adiabaticTemperatureK=2300.0;
		shadowConfig.transport.ambientGasDensityKGPerM3=ambient.GasDensity();
		shadowConfig.gravityMPerS2={{0.0,0.0,-9.80665}};shadowConfig.periodicBoundaries=false;
		shadowConfig.dns=false;shadowConfig.retainStageDiagnostics=true;
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
		std::vector<MethaneSourcePacket> zeroPackets(cells);
		ConservativeAdvance3DResult oracle,oracleSerial;
		shadowConfig.workerCount=16u;
		if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
			shadowConfig,fuel,fuel,transport,oracle,&error)){std::fprintf(stderr,
			"production golden parallel shadow %zu failed: %s\n",slice,error.c_str());return 115;}
		shadowConfig.workerCount=1u;
		if(!AdvanceConservative3D(shape,conservative,beginning.momentum,zeroPackets,
			shadowConfig,fuel,fuel,transport,oracleSerial,&error)){std::fprintf(stderr,
			"production golden serial shadow %zu failed: %s\n",slice,error.c_str());return 116;}
		if(oracle.maximumPreProjectionDivergenceResidualPerS!=
			oracleSerial.maximumPreProjectionDivergenceResidualPerS)return 128;
		RISE::FireProductionResidentStepRequest request;
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
		request.divergenceTargetPerS.resize(cells);
		request.restorationDivergenceTargetPerS.resize(cells);
		request.beginningManifoldDeviationPerCell.resize(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			request.divergenceTargetPerS[cell]=static_cast<float>(oracle.divergenceHeunPerS[cell]);
			double volumeRatio=0.0;
			if(!AcceptedConservativeVolumeRatio(ToConservativeVector(beginning.states[cell]),fuel,
				beginning.states[cell].producerPrecision,volumeRatio,&error))return 117;
			request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
				(volumeRatio-1.0)/static_cast<double>(request.force.timeStepS));
			request.beginningManifoldDeviationPerCell[cell]=volumeRatio-1.0;
		}
		if(manifoldProbe){
			if(slice!=0u)return 255;
			RISE::FireProductionAcceptedManifoldObservation previous;
			previous.available=true;previous.timeStepS=dt;
			previous.maximumGeneration=0.0025328069638265172;
			previous.restorationDrainFraction=0.9533406144549903;
			RISE::FireProductionStableTimeStep selected;
			if(!RISE::SelectFireProductionStableTimeStep(shape.cellWidthM,0.0,0.0,0.0,
				dt,previous,selected,&error))return 255;
			const float representedStep=static_cast<float>(selected.seconds);
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
			for(std::size_t trial=0u;trial<wall.size();++trial){
				const auto start=std::chrono::steady_clock::now();
				if(!RISE::AdvanceFireProductionResidentStepMetal(request,limited,&error)){
					std::fprintf(stderr,"MANIFOLD_TIMESTEP failed trial=%zu error=%s\n",
						trial,error.c_str());return 255;}
				wall[trial]=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-start).count();
				device[trial]=limited.deviceElapsedMS;
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
			RISE::FireProductionResidentStepResult rejected;
			rejected.conservativeValues.push_back(1.0f);rejected.cellSubmapCount=1u;
			rejected.maximumManifoldGeneration=1.0;rejected.manifoldPlateauPassed=true;
			if(!clearEnvironment("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE"))return 258;
			const bool unexpectedlyAccepted=RISE::AdvanceFireProductionResidentStepMetal(
				request,rejected,&error);
			if(!setEnvironment("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE","1"))return 258;
			const bool atomicRejection=!unexpectedlyAccepted&&rejected.conservativeValues.empty()&&
				rejected.cellSubmapCount==0u&&rejected.maximumManifoldGeneration==0.0&&
				!rejected.manifoldPlateauPassed&&rejected.conservativeProducerPrecision==
					FireStateProducerPrecision::Unknown;
			std::fprintf(stderr,"MANIFOLD_TIMESTEP cfl_dt=%.17g derived_dt=%.17g "
				"represented_dt=%.17g tightening=%.17g G=%.17g field=%.17g "
				"required=%.17g delivered=%.17g band=%.17g pre=%.17g post=%.17g "
				"mechanism=%d plateau=%d device_p95_ms=%.17g wall_p95_ms=%.17g\n",
				dt,selected.seconds,static_cast<double>(representedStep),dt/representedStep,
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
			const bool exact=selected.seconds==1.589201814710624e-5&&
				representedStep==0x1.0a9fb2p-16f&&dt/representedStep==3.5423604574130363&&
				limited.maximumManifoldGeneration==0.0025081625752932935&&
				limited.maximumAcceptedManifoldDeviation==0.0025081625764804549&&
				limited.requiredRestorationDrainFraction==3.3442167670577247&&
				limited.deliveredRestorationDrainFraction==0.97489008508207653&&
				limited.restorationResidualBandPerS==0.0&&
				limited.projection.maximumPreProjectionResidualPerS==0x1.8ce8bp-11f&&
				limited.projection.maximumPostProjectionResidualPerS==0x1.3eec56p-16f&&
				!limited.projection.validationPassed&&!limited.manifoldPlateauPassed&&
				independentGeneration==limited.maximumManifoldGeneration&&
				independentField==limited.maximumAcceptedManifoldDeviation&&
				atomicRejection&&std::isfinite(device.back())&&std::isfinite(wall.back())&&
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
		RISE::FireProductionResidentStepResult production;
		if(!RISE::AdvanceFireProductionResidentStepMetal(request,production,&error)){std::fprintf(stderr,
			"production golden resident slice %zu failed: %s\n",slice,error.c_str());return 119;}
		const float maximumRestorationTarget=*std::max_element(
			request.restorationDivergenceTargetPerS.begin(),
			request.restorationDivergenceTargetPerS.end(),[](const float a,const float b){
				return std::fabs(a)<std::fabs(b);});
		std::fprintf(stderr,"golden projection slice=%zu physical_valid=%d physical_pre=%.9g "
			"physical_post=%.9g restoration_valid=%d restoration_pre=%.9g "
			"restoration_post=%.9g restoration_target_max=%.9g restoration_band=%.9g\n",
			slice,production.physicalProjection.validationPassed?1:0,
			production.physicalProjection.maximumPreProjectionResidualPerS,
			production.physicalProjection.maximumPostProjectionResidualPerS,
			production.projection.validationPassed?1:0,
			production.projection.maximumPreProjectionResidualPerS,
			production.projection.maximumPostProjectionResidualPerS,
			std::fabs(maximumRestorationTarget),0.005f*std::fabs(maximumRestorationTarget));
		if(production.interstageFullGridTransferCount!=0u||production.residentProjectionInvocationCount!=2u||
			production.conservativeProducerPrecision!=RISE::FireStateProducerPrecision::Binary32||
			!production.physicalProjection.validationPassed||
			production.projection.executedVCycleCount!=16u||
			production.projection.executedJacobiSweepCount!=1088u)
			return 120;
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
		::FireProductionCalibration::ResidentStep64Result production64;
		if(!::FireProductionCalibration::AdvanceResidentStep64(request,
			static_cast<double>(production.forceDiagnostics.outwardLambdaPerS),production64,&error)){
			std::fprintf(stderr,"production golden fp64 slice %zu failed: %s\n",slice,
				error.c_str());return 131;}
		if(production64.force.schedule.substepCount!=production.forceSchedule.substepCount||
			!production64.physicalProjection.validationPassed||
			!production64.projection.validationPassed||
			production64.physicalProjection.executedVCycleCount!=
				production.physicalProjection.executedVCycleCount||
			production64.projection.executedVCycleCount!=
				production.projection.executedVCycleCount)return 132;
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
		for(std::size_t component=0u;component<9u;++component){
			const double precisionInventory=std::fabs(inventory32[component]-
				inventory64[component]);
			if(!(precisionScalar[component]<=scalarBound[component])||
				!(precisionInventory<=inventoryBound[component]))return 134;
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,
				precisionScalar[component]);
			FireProductionDyadicCalibration::AppendDouble(precisionTrace,
				precisionInventory);
		}
		if(!(precisionVelocity<=velocityBound))return 134;
		FireProductionDyadicCalibration::AppendDouble(precisionTrace,precisionVelocity);
		FireProductionDyadicCalibration::AppendInteger(precisionTrace,
			production.forceSchedule.substepCount);
		FireProductionDyadicCalibration::AppendInteger(precisionTrace,
			production.residentProjectionInvocationCount);
		std::fprintf(stderr,"golden subdominance slice=%zu velocity=%.17g bound=%.17g "
			"margin=%.17g scalar=",slice,precisionVelocity,velocityBound,
			precisionVelocity>0.0?velocityBound/precisionVelocity:
			std::numeric_limits<double>::infinity());
		for(const double value:precisionScalar)std::fprintf(stderr," %.17g",value);
		std::fprintf(stderr," inventory=");
		for(std::size_t component=0u;component<9u;++component)
			std::fprintf(stderr," %.17g",std::fabs(inventory32[component]-
				inventory64[component]));
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
	std::fprintf(stderr,"golden subdominance complete trace=%s slices=8\n",
		precisionDigest.c_str());
	return DigestFile(checkpointPath)==checkpointDigest?243:135;
}
