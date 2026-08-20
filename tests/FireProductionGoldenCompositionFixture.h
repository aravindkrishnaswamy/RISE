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
	const std::array<float,8> expectedProjectionResiduals={{
		0x1.5054p-8f,0x1.4704p-8f,0x1.4cd4p-8f,0x1.4af4p-8f,
		0x1.49bp-8f,0x1.4b28p-8f,0x1.49c8p-8f,0x1.4954p-8f}};
	const std::array<double,8> expectedIndependentProjectionResiduals={{
		0.0051404458276261911,0.0049875522159004513,0.0050794154282387217,
		0.0050499014713932307,0.0050310902116719207,0.0050479588015629375,
		0.0050315443766649108,0.0050194765847817882}};
	const std::array<double,8> expectedOracleProjectionResiduals={{
		0.00034786510337991849,0.0003471533644059388,0.00034558991067679123,
		0.0003456175668508088,0.00034565036028277873,0.0003473171417491816,
		0.00034591189175281478,0.00034485221191227211}};
	const std::array<double,8> expectedOraclePreProjectionResiduals={{
		37.867561455994299,37.850684107492093,37.865344827868185,37.89103061402438,
		37.89242874081063,37.905966640982733,37.946340026137996,38.003479059369397}};
	const std::array<double,8> expectedReductionRatios={{
		14.750826431310701,14.37360898839597,14.695793195959434,14.612270245955902,
		14.555735341802336,14.551049735665265,14.547225116309614,14.57220858600661}};
	const std::array<std::array<std::size_t,3>,2> steepFrontCoordinates={{{{40u,40u,1u}},
		{{41u,44u,3u}}}};
	const std::array<std::size_t,2> steepFrontComponents={{0u,8u}};
	const std::array<double,2> expectedMinimumPinnedProbeContrast={{
		0.30206703454394657,363818.26731442177}};
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
		for(std::size_t cell=0u;cell<cells;++cell)
			request.divergenceTargetPerS[cell]=static_cast<float>(oracle.divergenceHeunPerS[cell]);
		RISE::FireProductionResidentStepResult production;
		if(!RISE::AdvanceFireProductionResidentStepMetal(request,production,&error)){std::fprintf(stderr,
			"production golden resident slice %zu failed: %s\n",slice,error.c_str());return 119;}
		if(production.interstageFullGridTransferCount!=0u||production.residentProjectionInvocationCount!=1u||
			production.conservativeProducerPrecision!=RISE::FireStateProducerPrecision::Binary32||
			production.projection.executedVCycleCount!=16u||
			production.projection.executedJacobiSweepCount!=1088u)
			return 120;
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
	if(DigestFile(checkpointPath)!=checkpointDigest||
		projectionResiduals!=expectedProjectionResiduals||
		independentProjectionResiduals!=expectedIndependentProjectionResiduals||
		oracleProjectionResiduals!=expectedOracleProjectionResiduals||
		oraclePreProjectionResiduals!=expectedOraclePreProjectionResiduals||
		reductionRatios!=expectedReductionRatios||
		*std::max_element(projectionResiduals.begin(),projectionResiduals.end())!=0x1.5054p-8f)
		return 121;
	if(monitoredProjectionMisses!=0u)return 122;
	if(maximumPinnedProbeOvershoot>0.0||minimumConservativeDensity<minimumBeginningDensity||
		minimumPinnedProbeContrast!=expectedMinimumPinnedProbeContrast)return 126;
	std::fprintf(stderr,"production golden composition calibration is not sealed: "
		"tier-6 adjacent-grid and analytic fp32 bands are absent\n");
	return 129;
}
