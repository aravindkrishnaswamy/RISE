int RunProductionGoldenProjectionFixture(const std::filesystem::path& checkpointPath)
{
	static const char* checkpointDigest=
		"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
	if(DigestFile(checkpointPath)!=checkpointDigest){std::fprintf(stderr,
		"production golden projection checkpoint digest mismatch\n");return 90;}
	MethaneRunCheckpoint checkpoint;std::string error;
	if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
		checkpoint.acceptedSteps!=3479u){std::fprintf(stderr,
		"production golden projection checkpoint load failed: %s\n",error.c_str());return 91;}
	RISE::FireProductionProjectionRequest request;
	request.shape.nx=checkpoint.dimensions[0];request.shape.ny=checkpoint.dimensions[1];
	request.shape.nz=checkpoint.dimensions[2];request.shape.cellWidthM=
		static_cast<float>(checkpoint.cellWidthM);
	request.timeStepS=static_cast<float>(checkpoint.lastAcceptedStepS);
	request.ambientDensityKGPerM3=1.2f;
	request.boundary.fill(RISE::FireProductionProjectionPeriodic);
	const std::size_t cells=request.shape.CellCount();
	if(checkpoint.states.size()!=cells)return 92;
	request.gasDensityKGPerM3.resize(cells);
	for(std::size_t cell=0u;cell<cells;++cell)
		request.gasDensityKGPerM3[cell]=static_cast<float>(checkpoint.states[cell].GasDensity());
	auto cellIndex=[&](std::size_t x,std::size_t y,std::size_t z){
		return (z*request.shape.ny+y)*request.shape.nx+x;};
	auto faceIndex=[&](unsigned int axis,std::size_t x,std::size_t y,std::size_t z){
		if(axis==0u)return (z*request.shape.ny+y)*(request.shape.nx+1u)+x;
		if(axis==1u)return (z*(request.shape.ny+1u)+y)*request.shape.nx+x;
		return (z*request.shape.ny+y)*request.shape.nx+x;};
	std::array<std::vector<float>,3> faceDensity,provisionalVelocity;
	float maximumVelocity=0.0f;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faceCount=RISE::FireProductionProjectionFaceCount(request.shape,axis);
		request.provisionalMomentumKGPerM2S[axis].assign(faceCount,0.0f);
		faceDensity[axis].assign(faceCount,0.0f);provisionalVelocity[axis].assign(faceCount,0.0f);
		if(checkpoint.velocity.component[axis].size()!=faceCount)return 93;
		const std::size_t xFaces=axis==0u?request.shape.nx+1u:request.shape.nx;
		const std::size_t yFaces=axis==1u?request.shape.ny+1u:request.shape.ny;
		const std::size_t zFaces=axis==2u?request.shape.nz+1u:request.shape.nz;
		for(std::size_t z=0u;z<zFaces;++z)for(std::size_t y=0u;y<yFaces;++y)
			for(std::size_t x=0u;x<xFaces;++x){
				std::size_t leftX=x%request.shape.nx,leftY=y%request.shape.ny,
					leftZ=z%request.shape.nz,rightX=leftX,rightY=leftY,rightZ=leftZ;
				if(axis==0u){leftX=(x+request.shape.nx-1u)%request.shape.nx;rightX=x%request.shape.nx;}
				if(axis==1u){leftY=(y+request.shape.ny-1u)%request.shape.ny;rightY=y%request.shape.ny;}
				if(axis==2u){leftZ=(z+request.shape.nz-1u)%request.shape.nz;rightZ=z%request.shape.nz;}
				const float density=0.5f*(request.gasDensityKGPerM3[cellIndex(leftX,leftY,leftZ)]+
					request.gasDensityKGPerM3[cellIndex(rightX,rightY,rightZ)]);
				const std::size_t face=faceIndex(axis,x,y,z);
				// The accepted velocity is the checkpoint's authoritative MAC state.  Its
				// momentum was formed against the preceding coupled-stage density, so pairing
				// it with the accepted cell density would manufacture a divergence jump that
				// is not a projection error.  Re-materialize the fp32 momentum from the
				// accepted velocity and this projection request's stored face density.
				const float momentum=static_cast<float>(checkpoint.velocity.component[axis][face])*density;
				request.provisionalMomentumKGPerM2S[axis][face]=momentum;
				faceDensity[axis][face]=density;provisionalVelocity[axis][face]=momentum/density;
				maximumVelocity=std::max(maximumVelocity,std::fabs(provisionalVelocity[axis][face]));
			}
		const std::size_t extent=axis==0u?request.shape.nx:
			(axis==1u?request.shape.ny:request.shape.nz);
		const std::size_t plane=faceCount/(extent+1u);
		for(std::size_t line=0u;line<plane;++line){
			const std::size_t low=axis==0u?line*(extent+1u):
				(axis==1u?(line/request.shape.nx)*(extent+1u)*request.shape.nx+
				line%request.shape.nx:line);
			const std::size_t high=axis==0u?low+extent:
				(axis==1u?low+extent*request.shape.nx:
				low+extent*request.shape.nx*request.shape.ny);
			request.provisionalMomentumKGPerM2S[axis][low]=
				request.provisionalMomentumKGPerM2S[axis][high];
			faceDensity[axis][low]=faceDensity[axis][high];
			provisionalVelocity[axis][low]=provisionalVelocity[axis][high];
		}
	}
	std::vector<float> knownPressure(cells,0.0f);
	const float twoPi=6.2831853071795864769f;
	for(std::size_t z=0u;z<request.shape.nz;++z)for(std::size_t y=0u;y<request.shape.ny;++y)
		for(std::size_t x=0u;x<request.shape.nx;++x)
			knownPressure[cellIndex(x,y,z)]=0.03f*(
				std::sin(twoPi*(static_cast<float>(x)+0.5f)/static_cast<float>(request.shape.nx))+
				0.5f*std::sin(twoPi*(static_cast<float>(y)+0.5f)/static_cast<float>(request.shape.ny))+
				0.25f*std::sin(twoPi*(static_cast<float>(z)+0.5f)/static_cast<float>(request.shape.nz)));
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t xFaces=axis==0u?request.shape.nx+1u:request.shape.nx;
		const std::size_t yFaces=axis==1u?request.shape.ny+1u:request.shape.ny;
		const std::size_t zFaces=axis==2u?request.shape.nz+1u:request.shape.nz;
		for(std::size_t z=0u;z<zFaces;++z)for(std::size_t y=0u;y<yFaces;++y)
			for(std::size_t x=0u;x<xFaces;++x){
				std::size_t leftX=x%request.shape.nx,leftY=y%request.shape.ny,
					leftZ=z%request.shape.nz,rightX=leftX,rightY=leftY,rightZ=leftZ;
				if(axis==0u){leftX=(x+request.shape.nx-1u)%request.shape.nx;rightX=x%request.shape.nx;}
				if(axis==1u){leftY=(y+request.shape.ny-1u)%request.shape.ny;rightY=y%request.shape.ny;}
				if(axis==2u){leftZ=(z+request.shape.nz-1u)%request.shape.nz;rightZ=z%request.shape.nz;}
				const std::size_t face=faceIndex(axis,x,y,z);
				request.provisionalMomentumKGPerM2S[axis][face]+=request.timeStepS*
					(knownPressure[cellIndex(rightX,rightY,rightZ)]-
					knownPressure[cellIndex(leftX,leftY,leftZ)])/request.shape.cellWidthM;
			}
	}
	maximumVelocity=0.0f;
	for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t face=0u;
		face<request.provisionalMomentumKGPerM2S[axis].size();++face)
		maximumVelocity=std::max(maximumVelocity,std::fabs(
			request.provisionalMomentumKGPerM2S[axis][face]/faceDensity[axis][face]));
	request.divergenceTargetPerS.assign(cells,0.0f);
	float maximumPreResidual=0.0f;
	for(std::size_t z=0u;z<request.shape.nz;++z)
		for(std::size_t y=0u;y<request.shape.ny;++y)
			for(std::size_t x=0u;x<request.shape.nx;++x){
				float target=0.0f,divergence=0.0f;
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t low=faceIndex(axis,x,y,z);
					const std::size_t high=faceIndex(axis,axis==0u?x+1u:x,
						axis==1u?y+1u:y,axis==2u?z+1u:z);
					target+=(provisionalVelocity[axis][high]-provisionalVelocity[axis][low])/
						request.shape.cellWidthM;
					divergence+=(request.provisionalMomentumKGPerM2S[axis][high]/faceDensity[axis][high]-
						request.provisionalMomentumKGPerM2S[axis][low]/faceDensity[axis][low])/
						request.shape.cellWidthM;
				}
				request.divergenceTargetPerS[cellIndex(x,y,z)]=target;
				maximumPreResidual=std::max(maximumPreResidual,std::fabs(divergence-target));
			}
	RISE::FireProductionProjectionResult production;
	if(!RISE::ProjectFireProductionMetal(request,production,&error)){std::fprintf(stderr,
		"production golden projection Metal failed: %s\n",error.c_str());return 94;}
	FireSim::PeriodicMACShape oracleShape;oracleShape.nx=request.shape.nx;
	oracleShape.ny=request.shape.ny;oracleShape.nz=request.shape.nz;
	oracleShape.cellWidthM=request.shape.cellWidthM;
	std::vector<double> oracleDensity(request.gasDensityKGPerM3.begin(),request.gasDensityKGPerM3.end());
	std::vector<double> oracleTarget(request.divergenceTargetPerS.begin(),
		request.divergenceTargetPerS.end());
	FireSim::PeriodicMACField oracleMomentum;
	for(unsigned int axis=0u;axis<3u;++axis){oracleMomentum.component[axis].resize(cells);
		for(std::size_t z=0u;z<request.shape.nz;++z)
			for(std::size_t y=0u;y<request.shape.ny;++y)
				for(std::size_t x=0u;x<request.shape.nx;++x)
					oracleMomentum.component[axis][cellIndex(x,y,z)]=
						request.provisionalMomentumKGPerM2S[axis][faceIndex(axis,
							axis==0u?x+1u:x,axis==1u?y+1u:y,axis==2u?z+1u:z)];}
	const double length=static_cast<double>(request.shape.cellWidthM)*
		std::max(request.shape.nx,std::max(request.shape.ny,request.shape.nz));
	const double tolerance=0.005*static_cast<double>(maximumVelocity)/length;
	FireSim::PeriodicMACProjection3DResult oracle;
	double oraclePreResidual=0.0,targetMaximum=0.0;
	for(std::size_t cell=0u;cell<cells;++cell){
		oraclePreResidual=std::max(oraclePreResidual,std::fabs(
			FireSim::PeriodicMACDivergence3D(oracleShape,oracleMomentum,cell)-oracleTarget[cell]));
		targetMaximum=std::max(targetMaximum,std::fabs(oracleTarget[cell]));
	}
	if(!FireSim::ProjectPeriodicMACVelocity3D(oracleShape,oracleDensity,oracleMomentum,
		oracleTarget,request.timeStepS,tolerance,oracle,&error)){std::fprintf(stderr,
		"production golden projection oracle failed: %s U=%.17g pre=%.17g band=%.17g post=%.17g\n",
		error.c_str(),static_cast<double>(maximumVelocity),static_cast<double>(maximumPreResidual),
		tolerance,static_cast<double>(production.maximumPostProjectionResidualPerS));return 95;}
	double oracleResidual=0.0,velocityDifference=0.0;
	for(std::size_t cell=0u;cell<cells;++cell)oracleResidual=std::max(oracleResidual,std::fabs(
		FireSim::PeriodicMACDivergence3D(oracleShape,oracle.velocityMPerS,cell)-
		oracleTarget[cell]));
	for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t z=0u;z<request.shape.nz;++z)
		for(std::size_t y=0u;y<request.shape.ny;++y)for(std::size_t x=0u;x<request.shape.nx;++x){
			const std::size_t productionFace=faceIndex(axis,axis==0u?x+1u:x,
				axis==1u?y+1u:y,axis==2u?z+1u:z),cell=cellIndex(x,y,z);
			velocityDifference=std::max(velocityDifference,std::fabs(
				static_cast<double>(production.velocityMPerS[axis][productionFace])-
				oracle.velocityMPerS.component[axis][cell]));
		}
	std::fprintf(stderr,"production golden projection step=%llu pre=%.17g post=%.17g "
		"oracle_pre=%.17g oracle=%.17g ratio=%.17g velocity_delta=%.17g band=%.17g target=%.17g\n",
		static_cast<unsigned long long>(checkpoint.acceptedSteps),
		static_cast<double>(maximumPreResidual),
		static_cast<double>(production.maximumPostProjectionResidualPerS),oraclePreResidual,oracleResidual,
		static_cast<double>(production.maximumPostProjectionResidualPerS)/oracleResidual,
		velocityDifference,tolerance,targetMaximum);
	if(DigestFile(checkpointPath)!=checkpointDigest||!production.validationPassed||
		maximumPreResidual!=0x1.3ap-11f||
		production.maximumPostProjectionResidualPerS!=0x1.48p-15f||
		production.maximumPostProjectionResidualPerS>tolerance||
		production.maximumPostProjectionResidualPerS>1.25*oracleResidual||
		production.maximumPostProjectionResidualPerS>0.1f*maximumPreResidual||
		!(oracleResidual>0.0)||oracleResidual>tolerance||
		!std::isfinite(velocityDifference)||velocityDifference>3.0e-5)return 96;
	return 0;
}
