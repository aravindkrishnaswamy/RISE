#ifndef FIRE_PRODUCTION_DYADIC_CALIBRATION_FIXTURE_H
#define FIRE_PRODUCTION_DYADIC_CALIBRATION_FIXTURE_H

namespace FireProductionDyadicCalibration
{
	static constexpr double Pi=3.141592653589793238462643383279502884;
	static constexpr double Gravity=9.80665;
	static constexpr double VerifiedOrder=1.8;
	static const std::array<unsigned int,4> Tiers={{5u,10u,6u,12u}};
	static const std::array<std::size_t,3> BaseDimensions={{4u,4u,6u}};
	static const std::array<double,3> ExpectedOracleVelocityEvidence={{
		0.0039631780862326585,0.0032641652172793294,0.00040722205192891077}};
	static const std::array<std::array<double,3>,9> ExpectedOracleScalarEvidence={{
		{{2.7470874096308602e-05,1.9110121790822129e-05,5.7638488509996388e-07}},
		{{2.1976699277047011e-05,1.5288097432657347e-05,4.6110790808047211e-07}},
		{{4.0231720360505535e-05,2.7979909266403962e-05,8.6342690894070087e-07}},
		{{8.1681230739515595e-05,5.6950264505725857e-05,1.7491251816547303e-06}},
		{{1.4623092049747496e-05,1.0172558363063742e-05,3.0681692913582059e-07}},
		{{1.233964089521796e-05,8.5840748836714389e-06,2.5890630471503395e-07}},
		{{1.918563438587409e-07,1.3346492305372179e-07,4.0254669845263587e-09}},
		{{4.0311216479063973e-08,2.8042509814226353e-08,8.4579674447443766e-10}},
		{{27.213697534765799,19.056231150677373,0.56813643742138875}}
	}};
	static const std::array<std::array<double,3>,9> ExpectedOracleInventoryEvidence={{
		{{3.9462242692744898e-06,2.8839280563353054e-06,5.4857818981846052e-08}},
		{{3.1569794154140407e-06,2.3071424450696321e-06,4.3886255171599053e-08}},
		{{6.9891011217348975e-06,5.0936897178499585e-06,1.1226331092517583e-07}},
		{{1.2751139269795431e-05,9.2725341582777787e-06,2.2700562130051338e-07}},
		{{2.1006248485631873e-06,1.5351512035952086e-06,2.9201507514148295e-08}},
		{{1.7726043301004302e-06,1.2954315344350376e-06,2.4641581625878262e-08}},
		{{2.7560395701080667e-08,2.0141328262775898e-08,3.831265268147005e-10}},
		{{5.7907549732782678e-09,4.2319238834949294e-09,8.0499273845640977e-11}},
		{{3.0335881874780171,2.1990585236198967,0.062040384087595157}}
	}};
	static const std::array<std::array<double,3>,9> ExpectedProductionScalarEvidence={{
		{{2.7796305039946874e-05,1.9376400517682078e-05,6.582228033548485e-07}},
		{{2.2237020351149294e-05,1.5501218720141468e-05,5.2663655672729056e-07}},
		{{4.1616123281438155e-05,2.9297085613832641e-05,1.4219871059640226e-06}},
		{{8.8636991453610776e-05,6.386297406400966e-05,4.4544372513748042e-06}},
		{{1.4796337119822723e-05,1.0314382013297282e-05,3.5045957484289135e-07}},
		{{1.2485877457563494e-05,8.7037707862563385e-06,2.9567194577678921e-07}},
		{{1.9412955088147213e-07,1.3532660750105176e-07,4.5981908279813335e-09}},
		{{4.0788857371058287e-08,2.8433477024719178e-08,9.6625722505421453e-10}},
		{{27.368788679904416,19.185604845693799,0.60313182040724933}}
	}};
	static const std::array<double,3> ExpectedProductionVelocityEvidence={{
		0.005031015340016892,0.0041988689735742426,0.00054170919403134169}};
	static const std::array<std::array<double,3>,9> ExpectedProductionInventoryEvidence={{
		{{4.3003033303318228e-06,3.4038920052811839e-06,2.5070017945633127e-07}},
		{{3.4403398555399312e-06,2.7230951471016174e-06,2.0051584434521708e-07}},
		{{7.493419107051924e-06,5.9067473400509218e-06,4.1751729240591118e-07}},
		{{1.3489697749502483e-05,1.0597683779645095e-05,7.2355948221858313e-07}},
		{{2.2892032478308288e-06,1.8119569771719857e-06,1.3342105658828429e-07}},
		{{1.9316575974974437e-06,1.529023471828761e-06,1.126187700282999e-07}},
		{{3.0033289325111234e-08,2.3773386398037201e-08,1.7516446411836722e-09}},
		{{6.3103335605794791e-09,4.9948896377026712e-09,3.6787960725663885e-10}},
		{{3.2317103983587003,2.5744190392724704,0.20393986767885508}}
	}};

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

	void AppendText(RISECBOR64::Bytes& bytes,const char* value)
	{
		const std::size_t length=std::strlen(value);AppendInteger(bytes,length);
		bytes.insert(bytes.end(),value,value+length);
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

	struct FilteredVelocityField
	{
		std::array<std::size_t,3> dimensions={{0u,0u,0u}};
		std::vector<std::array<double,3> > value;
	};

	bool FilterVelocity(const MethaneRunCheckpoint& geometry,const PeriodicMACField& source,
		const double dStar,FilteredVelocityField& result)
	{
		result=FilteredVelocityField();result.dimensions={{16u,16u,26u}};
		const std::size_t nx=geometry.dimensions[0],ny=geometry.dimensions[1],
			nz=geometry.dimensions[2],cells=nx*ny*nz;
		std::vector<std::array<double,3> > centered(cells);
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz;if(source.component[axis].size()!=ex*ey*ez)return false;
			for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<ny;++y)
				for(std::size_t x=0u;x<nx;++x){const std::size_t low=(z*ey+y)*ex+x;
					const std::size_t high=low+(axis==0u?1u:(axis==1u?ex:ex*ey));
					centered[(z*ny+y)*nx+x][axis]=
						0.5*source.component[axis][low]+0.5*source.component[axis][high];}
		}
		const std::array<double,3> lengths={{4.0*dStar,4.0*dStar,6.0*dStar}};
		const double width=dStar/5.0;
		std::array<std::vector<std::vector<std::pair<std::size_t,double> > >,3> weights;
		for(unsigned int axis=0u;axis<3u;++axis)if(!AxisWeights(result.dimensions[axis],
			geometry.dimensions[axis],lengths[axis],width,weights[axis]))return false;
		const std::size_t sx=result.dimensions[0],sy=result.dimensions[1],sz=result.dimensions[2];
		std::vector<std::array<double,3> > xStage(sx*ny*nz),yStage(sx*sy*nz);
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<ny;++y)
			for(std::size_t x=0u;x<sx;++x)for(const auto& entry:weights[0][x])
				for(unsigned int axis=0u;axis<3u;++axis)xStage[(z*ny+y)*sx+x][axis]+=
					entry.second*centered[(z*ny+y)*nx+entry.first][axis];
		for(std::size_t z=0u;z<nz;++z)for(std::size_t y=0u;y<sy;++y)
			for(std::size_t x=0u;x<sx;++x)for(const auto& entry:weights[1][y])
				for(unsigned int axis=0u;axis<3u;++axis)yStage[(z*sy+y)*sx+x][axis]+=
					entry.second*xStage[(z*ny+entry.first)*sx+x][axis];
		result.value.assign(sx*sy*sz,std::array<double,3>{{0.0,0.0,0.0}});
		for(std::size_t z=0u;z<sz;++z)for(std::size_t y=0u;y<sy;++y)
			for(std::size_t x=0u;x<sx;++x)for(const auto& entry:weights[2][z])
				for(unsigned int axis=0u;axis<3u;++axis)result.value[(z*sy+y)*sx+x][axis]+=
					entry.second*yStage[(entry.first*sy+y)*sx+x][axis];
		return std::all_of(result.value.begin(),result.value.end(),[](const std::array<double,3>& v){
			return std::all_of(v.begin(),v.end(),[](const double x){return std::isfinite(x);});});
	}

	double VelocityDistance(const FilteredVelocityField& a,const FilteredVelocityField& b)
	{
		if(a.dimensions!=b.dimensions||a.value.size()!=b.value.size()||a.value.empty())
			return std::numeric_limits<double>::infinity();
		double sum=0.0;for(std::size_t cell=0u;cell<a.value.size();++cell)
			for(unsigned int axis=0u;axis<3u;++axis){const double difference=
				a.value[cell][axis]-b.value[cell][axis];sum+=difference*difference;}
		return std::sqrt(sum/static_cast<double>(a.value.size()));
	}

	FilteredVelocityField ExtrapolateVelocity(const FilteredVelocityField& coarse,
		const FilteredVelocityField& fine)
	{
		FilteredVelocityField result;result.dimensions=coarse.dimensions;
		if(coarse.dimensions!=fine.dimensions||coarse.value.size()!=fine.value.size())return result;
		const double factor=std::pow(2.0,VerifiedOrder),denominator=factor-1.0;
		result.value.resize(coarse.value.size());
		for(std::size_t cell=0u;cell<coarse.value.size();++cell)
			for(unsigned int axis=0u;axis<3u;++axis)result.value[cell][axis]=
				(factor*fine.value[cell][axis]-coarse.value[cell][axis])/denominator;
		return result;
	}

	std::array<double,9> ComponentInventoryDensity(const std::vector<ConservativeVector>& state)
	{
		std::array<double,9> result={{}},compensation={{}};
		for(const ConservativeVector& cell:state)for(std::size_t component=0u;component<9u;++component){
			const double corrected=cell[component]-compensation[component];
			const double updated=result[component]+corrected;
			compensation[component]=(updated-result[component])-corrected;result[component]=updated;}
		for(double& value:result)value/=static_cast<double>(state.size());return result;
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

	bool ReadProtocolStateDigests(const std::filesystem::path& path,
		std::array<std::string,4>& digests)
	{
		std::ifstream input(path);std::string token;std::size_t found=0u;
		while(input>>token)if(token=="tier"){unsigned int tier=0u;std::string label,digest,
			caseLabel,caseDigest;if(!(input>>tier>>label>>digest>>caseLabel>>caseDigest)||
				label!="analytic_state_sha256"||caseLabel!="case_record_id")return false;
			auto it=std::find(Tiers.begin(),Tiers.end(),tier);if(it==Tiers.end())return false;
			digests[static_cast<std::size_t>(it-Tiers.begin())]=digest;++found;}
		return found==4u&&std::all_of(digests.begin(),digests.end(),[](const std::string& digest){
			return digest.size()==64u;});
	}

	int SealSupplementalMetrics(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 191;
		std::ostringstream manifest;manifest<<
			"fire_production_dyadic_metrics_v1\n"
			"protocol_sha256 "<<expectedProtocol<<"\n"
			"targets_sha256 "<<expectedTargets<<"\n"
			"velocity cell_centered_arithmetic_MAC_then_tensor_cubic_bspline_v1\n"
			"velocity_norm volume_RMS_vector_L2\n"
			"ledger final_component_inventory_density_kahan_sum_all_cells\n"
			"ledger_norm componentwise_absolute_difference\n"
			"verified_order 1.8\n"
			"decision independent_limit_balls_overlap_and_cross_pair_refinement\n";
		if(!WriteTextAtomically(directory/"dyadic_metrics.v1",manifest.str()))return 192;
		std::fprintf(stderr,"dyadic supplemental metrics sealed manifest_sha256=%s\n",
			DigestFile(directory/"dyadic_metrics.v1").c_str());return 0;
	}

	int CheckOracle(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 173;
		std::array<std::string,4> targetDigests;if(!ReadTargetDigests(
			directory/"dyadic_targets.v1",targetDigests))return 174;
		std::array<std::string,4> stateDigests;if(!ReadProtocolStateDigests(
			directory/"dyadic_protocol.v1",stateDigests))return 174;
		std::array<MethaneRunCheckpoint,4> states;std::array<FilteredField,4> filtered;
		std::array<OracleSpatialCalibrationResult,4> outputs;std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error))return 175;
			if(AnalyticStateDigest(states[index])!=stateDigests[index])return 175;
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

	int CheckSupplementalOracleMetrics(const std::filesystem::path& directory,
		const char* expectedProtocol,const char* expectedTargets,const char* expectedMetrics)
	{
		if(!expectedMetrics||std::strlen(expectedMetrics)!=64u||
			DigestFile(directory/"dyadic_metrics.v1")!=expectedMetrics)return 193;
		const int scalarStatus=CheckOracle(directory,expectedProtocol,expectedTargets);
		if(scalarStatus!=0)return scalarStatus;
		std::array<std::string,4> targetDigests,stateDigests;
		if(!ReadTargetDigests(directory/"dyadic_targets.v1",targetDigests)||
			!ReadProtocolStateDigests(directory/"dyadic_protocol.v1",stateDigests))return 194;
		std::array<MethaneRunCheckpoint,4> states;
		std::array<OracleSpatialCalibrationResult,4> outputs;
		std::array<FilteredVelocityField,4> velocity;
		std::array<std::array<double,9>,4> inventory;std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error)||
				AnalyticStateDigest(states[index])!=stateDigests[index])return 195;
			std::vector<std::vector<double> > sealed;const std::filesystem::path target=
				directory/(std::string("oracle_tier")+std::to_string(Tiers[index])+"_sdiv_x8.f64");
			const double flowThrough=6.0*std::sqrt(states[index].values.characteristicDiameterM/Gravity);
			if(DigestFile(target)!=targetDigests[index]||!ReadCalibrationDoublePayload(target,
				states[index].states.size(),8u,sealed)||!RunOracleSpatialCalibrationTrajectory(
				states[index],flowThrough/512.0,8u,outputs[index],error,false))return 196;
			for(std::size_t step=0u;step<sealed.size();++step)if(sealed[step].size()!=
				outputs[index].divergenceTargetsPerS[step].size()||std::memcmp(sealed[step].data(),
				outputs[index].divergenceTargetsPerS[step].data(),sealed[step].size()*sizeof(double))!=0)
				return 197;
			if(!FilterVelocity(states[index],outputs[index].velocityMPerS,
				states[index].values.characteristicDiameterM,velocity[index]))return 198;
			inventory[index]=ComponentInventoryDensity(outputs[index].conservative);
		}
		const double velocityA=VelocityDistance(velocity[0],velocity[1]);
		const double velocityB=VelocityDistance(velocity[2],velocity[3]);
		const FilteredVelocityField velocityLimitA=ExtrapolateVelocity(velocity[0],velocity[1]);
		const FilteredVelocityField velocityLimitB=ExtrapolateVelocity(velocity[2],velocity[3]);
		FireProductionCalibration::DyadicDistanceEstimate velocityEstimateA,velocityEstimateB;
		const bool velocityEstimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
			velocityA,VerifiedOrder,velocityEstimateA)&&
			FireProductionCalibration::DyadicDistanceAtVerifiedOrder(velocityB,VerifiedOrder,
				velocityEstimateB);
		const double velocityLimitDelta=VelocityDistance(velocityLimitA,velocityLimitB);
		const bool velocityOverlap=velocityEstimated&&
			FireProductionCalibration::DyadicLimitBallsOverlap(velocityLimitDelta,
				velocityEstimateA,velocityEstimateB);
		const bool velocityApproach=VelocityDistance(velocity[0],velocityLimitB)>
			VelocityDistance(velocity[1],velocityLimitB)&&
			VelocityDistance(velocity[2],velocityLimitA)>
			VelocityDistance(velocity[3],velocityLimitA);
		const bool velocityExact=velocityA==ExpectedOracleVelocityEvidence[0]&&
			velocityB==ExpectedOracleVelocityEvidence[1]&&
			velocityLimitDelta==ExpectedOracleVelocityEvidence[2];
		std::fprintf(stderr,"dyadic oracle velocity D5_10=%.17g D6_12=%.17g "
			"limit_delta=%.17g radius_sum=%.17g approach=%d accepted=%d\n",velocityA,
			velocityB,velocityLimitDelta,velocityEstimated?
			velocityEstimateA.fineRadius+velocityEstimateB.fineRadius:0.0,
			velocityApproach?1:0,velocityExact&&velocityOverlap&&velocityApproach?1:0);
		bool accepted=velocityExact&&velocityOverlap&&velocityApproach;
		const double factor=std::pow(2.0,VerifiedOrder),denominator=factor-1.0;
		for(std::size_t component=0u;component<9u;++component){
			const double dA=std::fabs(inventory[0][component]-inventory[1][component]);
			const double dB=std::fabs(inventory[2][component]-inventory[3][component]);
			const double limitA=(factor*inventory[1][component]-inventory[0][component])/denominator;
			const double limitB=(factor*inventory[3][component]-inventory[2][component])/denominator;
			FireProductionCalibration::DyadicDistanceEstimate estimateA,estimateB;
			const bool estimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(dA,
				VerifiedOrder,estimateA)&&FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
				dB,VerifiedOrder,estimateB);
			const bool overlap=estimated&&FireProductionCalibration::DyadicLimitBallsOverlap(
				std::fabs(limitA-limitB),estimateA,estimateB);
			const bool approach=std::fabs(inventory[0][component]-limitB)>
				std::fabs(inventory[1][component]-limitB)&&std::fabs(inventory[2][component]-limitA)>
				std::fabs(inventory[3][component]-limitA);
			const double limitDelta=std::fabs(limitA-limitB);
			const bool exact=dA==ExpectedOracleInventoryEvidence[component][0]&&
				dB==ExpectedOracleInventoryEvidence[component][1]&&
				limitDelta==ExpectedOracleInventoryEvidence[component][2];
			accepted=accepted&&exact&&overlap&&approach;
			std::fprintf(stderr,"dyadic oracle ledger component=%zu D5_10=%.17g D6_12=%.17g "
				"limit_delta=%.17g radius_sum=%.17g approach=%d accepted=%d\n",component,dA,dB,
				limitDelta,estimated?estimateA.fineRadius+estimateB.fineRadius:0.0,
				approach?1:0,exact&&overlap&&approach?1:0);
		}
		return accepted?0:199;
	}

	inline bool ResidentStepRequestsExactlyEqual(
		const RISE::FireProductionResidentStepRequest& a,
		const RISE::FireProductionResidentStepRequest& b)
	{
		auto sameShape=[](const RISE::FireProductionProjectionShape& x,
			const RISE::FireProductionProjectionShape& y){return x.nx==y.nx&&x.ny==y.ny&&
			x.nz==y.nz&&x.cellWidthM==y.cellWidthM;};
		return sameShape(a.force.shape,b.force.shape)&&
			a.force.timeStepS==b.force.timeStepS&&
			a.force.ambientDensityKGPerM3==b.force.ambientDensityKGPerM3&&
			a.force.vremanCoefficient==b.force.vremanCoefficient&&
			a.force.gravityMPerS2==b.force.gravityMPerS2&&a.force.boundary==b.force.boundary&&
			a.force.cellGasDensityKGPerM3==b.force.cellGasDensityKGPerM3&&
			a.force.molecularKinematicViscosityM2PerS==
				b.force.molecularKinematicViscosityM2PerS&&
			a.force.faceDensityKGPerM3==b.force.faceDensityKGPerM3&&
			a.force.beginningMomentumKGPerM2S==b.force.beginningMomentumKGPerM2S&&
			sameShape(a.cellTransport.shape,b.cellTransport.shape)&&
			a.cellTransport.componentCount==b.cellTransport.componentCount&&
			a.cellTransport.timeStepS==b.cellTransport.timeStepS&&
			a.cellTransport.boundary==b.cellTransport.boundary&&
			a.cellTransport.conservativeValues==b.cellTransport.conservativeValues&&
			a.cellTransport.frozenVelocityMPerS==b.cellTransport.frozenVelocityMPerS&&
			a.cellTransport.ambientValues==b.cellTransport.ambientValues&&
			sameShape(a.dualTransport.shape,b.dualTransport.shape)&&
			a.dualTransport.timeStepS==b.dualTransport.timeStepS&&
			a.dualTransport.ambientDensityKGPerM3==b.dualTransport.ambientDensityKGPerM3&&
			a.dualTransport.boundary==b.dualTransport.boundary&&
			a.dualTransport.beginningFaceDensity==b.dualTransport.beginningFaceDensity&&
			a.dualTransport.beginningMomentum==b.dualTransport.beginningMomentum&&
			a.dualTransport.frozenVelocityMPerS==b.dualTransport.frozenVelocityMPerS&&
			a.cellSourceIncrement==b.cellSourceIncrement&&
			a.momentumSourceIncrement==b.momentumSourceIncrement&&
			a.divergenceTargetPerS==b.divergenceTargetPerS&&
			a.restorationDivergenceTargetPerS==b.restorationDivergenceTargetPerS&&
			a.beginningManifoldDeviationPerCell==b.beginningManifoldDeviationPerCell&&
			a.physicalOpenProjectionVCycleCount==b.physicalOpenProjectionVCycleCount&&
			a.monitorManifoldDiagnostics==b.monitorManifoldDiagnostics&&
			a.enforceManifoldPlateau==b.enforceManifoldPlateau&&
			a.restoreManifoldOutliers==b.restoreManifoldOutliers;
	}

	bool BuildProductionRequest(const MethaneRunCheckpoint& state,
		const std::vector<double>& divergenceTarget,const double timeStepS,
		RISE::FireProductionResidentStepRequest& request,std::string& error,
		const unsigned int workerCount,
		const std::vector<CellTransportEvaluation>* reusableTransport,
		const std::vector<unsigned char>* invalidReusableTransport)
	{
		request=RISE::FireProductionResidentStepRequest();
		const std::size_t cells=state.states.size();
		if(cells!=state.dimensions[0]*state.dimensions[1]*state.dimensions[2]||
			divergenceTarget.size()!=cells||
			(reusableTransport&&reusableTransport->size()!=cells)||
			(invalidReusableTransport&&(!reusableTransport||
				invalidReusableTransport->size()!=cells)))return false;
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
		request.divergenceTargetPerS.resize(cells);
		request.restorationDivergenceTargetPerS.resize(cells);
		request.beginningManifoldDeviationPerCell.resize(cells);
		if(cells>static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())){
			error="production request cell count exceeds the worker partition range";return false;
		}
		const unsigned int workers=std::max(1u,std::min(workerCount,
			static_cast<unsigned int>(cells)));
		const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
		std::vector<std::size_t> failureCell(workers,noFailure);
		std::vector<std::string> failureMessage(workers);
		auto buildCellRange=[&](const unsigned int worker){
			const std::size_t first=cells*worker/workers,last=cells*(worker+1u)/workers;
			for(std::size_t cell=first;cell<last;++cell){
			std::string cellError;
			const ConservativeVector conservative=ToConservativeVector(state.states[cell]);
			const double gas=state.states[cell].GasDensity();CellMolecularTransportEvaluation molecular;
			if(!(gas>0.0)){
				failureCell[worker]=cell;
				failureMessage[worker]="production request gas density is nonpositive";break;
			}
			const bool reusable=reusableTransport&&(!invalidReusableTransport||
				(*invalidReusableTransport)[cell]==0u);
			if(reusable){
				molecular.molecularViscosityPaS=(*reusableTransport)[cell].molecularViscosityPaS;
			}else if(!EvaluateCellMolecularTransport(state.states[cell],fuel,transport,
				state.states[cell].producerPrecision,molecular,&cellError)){
				failureCell[worker]=cell;failureMessage[worker]=cellError;break;}
			request.force.molecularKinematicViscosityM2PerS[cell]=
				static_cast<float>(molecular.molecularViscosityPaS/gas);
			for(std::size_t component=0u;component<9u;++component)
				request.cellTransport.conservativeValues[component*cells+cell]=
					static_cast<float>(conservative[component]);
			float packedGas=request.cellTransport.conservativeValues[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				packedGas+=request.cellTransport.conservativeValues[component*cells+cell];
			request.force.cellGasDensityKGPerM3[cell]=packedGas;
			double volumeRatio=0.0;
			if(!AcceptedConservativeVolumeRatio(conservative,fuel,
				state.states[cell].producerPrecision,volumeRatio,&cellError)){
				failureCell[worker]=cell;failureMessage[worker]=cellError;break;}
			request.divergenceTargetPerS[cell]=static_cast<float>(divergenceTarget[cell]);
			request.beginningManifoldDeviationPerCell[cell]=volumeRatio-1.0;
			request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
				(volumeRatio-1.0)/static_cast<double>(request.force.timeStepS));
			}
		};
		if(workers==1u)buildCellRange(0u);
		else FireWorkerPool().Run(workers,buildCellRange);
		std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
		for(unsigned int worker=0u;worker<workers;++worker)if(failureCell[worker]<firstFailure){
			firstFailure=failureCell[worker];failedWorker=worker;}
		if(firstFailure!=noFailure){error=failureMessage[failedWorker];return false;}
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
			ParallelFireSlices(ez,workerCount,[&](const std::size_t z){for(std::size_t y=0u;y<ey;++y)
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
				}});
			request.dualTransport.beginningFaceDensity[axis]=request.force.faceDensityKGPerM3[axis];
			request.dualTransport.beginningMomentum[axis]=request.force.beginningMomentumKGPerM2S[axis];
			request.dualTransport.frozenVelocityMPerS[axis]=request.cellTransport.frozenVelocityMPerS[axis];
			request.momentumSourceIncrement[axis].assign(faces,0.0f);
		}
		request.cellSourceIncrement.assign(9u*cells,0.0f);
		return RISE::ValidateFireProductionFrozenForceRequest(request.force,&error)&&
			RISE::ValidateFireProductionCellPalindromeRequest(request.cellTransport,&error)&&
			RISE::ValidateFireProductionDualMomentumRequest(request.dualTransport,&error);
	}

	bool ApplyProductionResultUnchecked(const RISE::FireProductionResidentStepResult& production,
		MethaneRunCheckpoint& state,std::string& error,
		const bool enforceOracleEOSValidityDetector=true,
		const unsigned int temperatureWorkerCount=1u)
	{
		const std::size_t cells=state.states.size();if(production.conservativeValues.size()!=9u*cells)
			return false;
		if(production.conservativeProducerPrecision!=FireStateProducerPrecision::Binary32)
			return Fail(&error,"production resident state lacks binary32 producer metadata");
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			for(std::size_t component=0u;component<9u;++component)
				conservative[cell][component]=production.conservativeValues[component*cells+cell];
			state.states[cell]=FromConservativeVector(conservative[cell],
				production.conservativeProducerPrecision);
		}
		std::vector<double> temperature;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(enforceOracleEOSValidityDetector){
			if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
				fuel.TemperatureMaxK(),production.conservativeProducerPrecision,temperature,&error,
				temperatureWorkerCount)||
				temperature.size()!=cells)return false;
		}else{
			if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
				fuel.TemperatureMaxK(),production.conservativeProducerPrecision,temperature,&error,
				temperatureWorkerCount,false)||temperature.size()!=cells)return false;
		}
		for(std::size_t cell=0u;cell<cells;++cell)state.states[cell].temperatureK=temperature[cell];
		for(unsigned int axis=0u;axis<3u;++axis){
			state.momentum.component[axis].assign(production.projection.momentumKGPerM2S[axis].begin(),
				production.projection.momentumKGPerM2S[axis].end());
			state.velocity.component[axis].assign(production.projection.velocityMPerS[axis].begin(),
				production.projection.velocityMPerS[axis].end());
		}
		return true;
	}

	bool ApplyProductionMirrorResult(const FireProductionCalibration::ResidentStep64Result& production,
		MethaneRunCheckpoint& state,std::string& error)
	{
		const std::size_t cells=state.states.size();
		if(production.conservativeValues.size()!=MethaneConservativeDimension*cells)return false;
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				conservative[cell][component]=static_cast<double>(static_cast<float>(
					production.conservativeValues[component*cells+cell]));
			state.states[cell]=FromConservativeVector(conservative[cell],
				FireStateProducerPrecision::Binary32);
		}
		std::vector<double> temperature;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
			fuel.TemperatureMaxK(),FireStateProducerPrecision::Binary32,temperature,&error,1u,false)||
			temperature.size()!=cells)return false;
		for(std::size_t cell=0u;cell<cells;++cell)state.states[cell].temperatureK=temperature[cell];
		for(unsigned int axis=0u;axis<3u;++axis){
			state.momentum.component[axis].resize(
				production.projection.momentumKGPerM2S[axis].size());
			state.velocity.component[axis].resize(production.projection.velocityMPerS[axis].size());
			for(std::size_t face=0u;face<state.momentum.component[axis].size();++face){
				state.momentum.component[axis][face]=static_cast<double>(static_cast<float>(
					production.projection.momentumKGPerM2S[axis][face]));
				state.velocity.component[axis][face]=static_cast<double>(static_cast<float>(
					production.projection.velocityMPerS[axis][face]));
			}
		}
		return true;
	}

	bool TemporalBeginningMatchesProtocol(const MethaneRunCheckpoint& state)
	{
		return AnalyticStateDigest(state)==
			"5116d667f31d197fe9fbbc98348093cc86a3d70c693163579d6c6f3023a1adff"&&
			std::all_of(state.states.begin(),state.states.end(),[](const MethaneCellState& cell){
				return cell.producerPrecision==FireStateProducerPrecision::Binary64;
			});
	}

	bool TemporalTargetMatchesSealedSchedule(const std::vector<std::vector<double> >& schedule,
		const std::size_t step,const std::vector<double>& target)
	{
		return step<schedule.size()&&schedule[step].size()==target.size()&&
			std::memcmp(schedule[step].data(),target.data(),target.size()*sizeof(double))==0;
	}

	int SealTemporalTargets(const std::filesystem::path& directory,
		const std::filesystem::path& protocolPath,const char* expectedProtocol)
	{
		static const std::array<unsigned int,3> stepCounts={{8u,16u,32u}};
		if(!expectedProtocol||std::strlen(expectedProtocol)!=64u||
			DigestFile(protocolPath)!=expectedProtocol)return 194;
		MethaneRunCheckpoint beginning;std::string error;
		if(!BuildAnalyticState(6.0,beginning,error)||!TemporalBeginningMatchesProtocol(beginning))
			return 195;
		MethaneRunCheckpoint precisionMutant=beginning;
		precisionMutant.states.front().producerPrecision=FireStateProducerPrecision::Binary32;
		if(AnalyticStateDigest(precisionMutant)!=AnalyticStateDigest(beginning)||
			TemporalBeginningMatchesProtocol(precisionMutant))return 196;
		std::error_code directoryError;std::filesystem::create_directories(directory,directoryError);
		if(directoryError)return 197;
		const double flowThrough=6.0*std::sqrt(
			beginning.values.characteristicDiameterM/Gravity);
		const double baseline=static_cast<double>(static_cast<float>(flowThrough/512.0));
		std::array<std::string,3> targetDigests;
		for(std::size_t level=0u;level<stepCounts.size();++level){
			const double timeStep=std::ldexp(baseline,-static_cast<int>(level));
			OracleSpatialCalibrationResult result;
			if(!RunOracleSpatialCalibrationTrajectory(beginning,timeStep,stepCounts[level],
				result,error,false))return 198;
			const std::filesystem::path target=directory/(std::string("temporal_level")+
				std::to_string(level)+"_sdiv.f64");
			if(!WriteCalibrationDoublePayload(target,result.divergenceTargetsPerS))return 199;
			targetDigests[level]=DigestFile(target);
		}
		std::ostringstream manifest;manifest<<std::setprecision(17)
			<<"fire_production_temporal_targets_v1\nprotocol_sha256 "<<expectedProtocol<<"\n"
			<<"beginning_sha256 "<<AnalyticStateDigest(beginning)<<"\n"
			<<"beginning_producer_precision binary64_all_cells\n"
			<<"baseline_dt_s "<<baseline<<"\n";
		for(std::size_t level=0u;level<stepCounts.size();++level)manifest<<"level "<<level
			<<" step_count "<<stepCounts[level]<<" dt_s "
			<<std::ldexp(baseline,-static_cast<int>(level))<<" sdiv_sha256 "
			<<targetDigests[level]<<"\n";
		if(!WriteTextAtomically(directory/"temporal_targets.v1",manifest.str()))return 200;
		std::fprintf(stderr,"temporal targets sealed manifest_sha256=%s\n",
			DigestFile(directory/"temporal_targets.v1").c_str());
		return 194;
	}

	bool ReadTemporalTargetManifest(const std::filesystem::path& path,
		const char* expectedProtocol,std::array<unsigned int,3>& stepCounts,
		std::array<double,3>& timeSteps,std::array<std::string,3>& targetDigests)
	{
		std::ifstream input(path);std::string token,protocol,beginning,precision;
		if(!(input>>token)||token!="fire_production_temporal_targets_v1")return false;
		if(!(input>>token>>protocol)||token!="protocol_sha256"||protocol!=expectedProtocol)
			return false;
		if(!(input>>token>>beginning)||token!="beginning_sha256"||beginning!=
			"5116d667f31d197fe9fbbc98348093cc86a3d70c693163579d6c6f3023a1adff")
			return false;
		if(!(input>>token>>precision)||token!="beginning_producer_precision"||
			precision!="binary64_all_cells")return false;
		double baseline=0.0;if(!(input>>token>>baseline)||token!="baseline_dt_s"||
			baseline!=0.0018513043178245425)return false;
		for(std::size_t expectedLevel=0u;expectedLevel<3u;++expectedLevel){
			std::size_t level=0u;std::string stepLabel,dtLabel,digestLabel;
			if(!(input>>token>>level>>stepLabel>>stepCounts[expectedLevel]>>dtLabel>>
				timeSteps[expectedLevel]>>digestLabel>>targetDigests[expectedLevel])||
				token!="level"||level!=expectedLevel||stepLabel!="step_count"||
				dtLabel!="dt_s"||digestLabel!="sdiv_sha256"||
				targetDigests[expectedLevel].size()!=64u)return false;
		}
		return true;
	}

	int MeasureTemporalRefinement(const std::filesystem::path& directory,
		const std::filesystem::path& protocolPath,const char* expectedProtocol,
		const char* expectedTargets,const std::filesystem::path& amendmentPath,
		const char* expectedAmendment)
	{
		if(!expectedProtocol||!expectedTargets||!expectedAmendment||
			std::strlen(expectedProtocol)!=64u||std::strlen(expectedAmendment)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(protocolPath)!=expectedProtocol||
			DigestFile(amendmentPath)!=expectedAmendment||
			DigestFile(directory/"temporal_targets.v1")!=expectedTargets)return 173;
		std::array<unsigned int,3> stepCounts;std::array<double,3> sealedTimeSteps;
		std::array<std::string,3> targetFileDigests;
		if(!ReadTemporalTargetManifest(directory/"temporal_targets.v1",expectedProtocol,
			stepCounts,sealedTimeSteps,targetFileDigests)||stepCounts!=
			std::array<unsigned int,3>{{8u,16u,32u}})return 173;
		MethaneRunCheckpoint beginning;std::string error;
		if(!BuildAnalyticState(6.0,beginning,error)||!TemporalBeginningMatchesProtocol(beginning))
			return 173;
		MethaneRunCheckpoint precisionMutant=beginning;
		precisionMutant.states.front().producerPrecision=FireStateProducerPrecision::Binary32;
		if(AnalyticStateDigest(precisionMutant)!=AnalyticStateDigest(beginning)||
			TemporalBeginningMatchesProtocol(precisionMutant))return 173;
		const double flowThrough=6.0*std::sqrt(
			beginning.values.characteristicDiameterM/Gravity);
		const double baseline=static_cast<double>(static_cast<float>(flowThrough/512.0));
		std::array<std::vector<std::vector<double> >,3> loadedSchedules;
		for(std::size_t level=0u;level<stepCounts.size();++level){
			const std::filesystem::path target=directory/(std::string("temporal_level")+
				std::to_string(level)+"_sdiv.f64");
			if(sealedTimeSteps[level]!=std::ldexp(baseline,-static_cast<int>(level))||
				DigestFile(target)!=targetFileDigests[level]||!ReadCalibrationDoublePayload(target,
					beginning.states.size(),stepCounts[level],loadedSchedules[level]))return 173;
		}
		const std::array<std::vector<std::vector<double> >,3> sealedSchedules=
			std::move(loadedSchedules);
		if(TemporalTargetMatchesSealedSchedule(sealedSchedules[0],1u,
			sealedSchedules[0][0]))return 173;
		std::array<OracleSpatialCalibrationResult,3> oracle;
		std::array<std::string,3> targetDigest;
		for(std::size_t level=0u;level<stepCounts.size();++level){
			const double timeStep=std::ldexp(baseline,-static_cast<int>(level));
			if(!RunOracleSpatialCalibrationTrajectory(beginning,timeStep,stepCounts[level],
				oracle[level],error,false)){std::fprintf(stderr,
				"temporal oracle level=%zu failed: %s\n",level,error.c_str());return 174;}
			if(oracle[level].divergenceTargetsPerS.size()!=sealedSchedules[level].size())
				return 174;
			for(std::size_t step=0u;step<sealedSchedules[level].size();++step)
				if(!TemporalTargetMatchesSealedSchedule(sealedSchedules[level],step,
					oracle[level].divergenceTargetsPerS[step]))return 174;
			targetDigest[level]=targetFileDigests[level];
		}
		std::array<MethaneRunCheckpoint,3> production;
		std::array<FireProductionCalibration::ResidentStep64Result,3> finalProduction;
		for(std::size_t level=0u;level<stepCounts.size();++level){
			production[level]=beginning;
			const double timeStep=std::ldexp(baseline,-static_cast<int>(level));
			for(std::size_t step=0u;step<stepCounts[level];++step){
				RISE::FireProductionResidentStepRequest request;
				const std::vector<double>& sealedTarget=sealedSchedules[level][step];
				if(!TemporalTargetMatchesSealedSchedule(sealedSchedules[level],step,sealedTarget)||
					!BuildProductionRequest(production[level],sealedTarget,timeStep,request,error))
					return 175;
				RISE::FireProductionResidentStepResult producerProbe;
				if(!RISE::AdvanceFireProductionResidentStepMetal(request,producerProbe,&error)||
					!producerProbe.projection.validationPassed)return 176;
				FireProductionCalibration::ResidentStep64Result advanced;
				const bool mirrorAdvanced=FireProductionCalibration::AdvanceResidentStep64(request,
					static_cast<double>(producerProbe.forceDiagnostics.outwardLambdaPerS),advanced,
					&error);
				const bool mirrorApplied=mirrorAdvanced&&advanced.projection.validationPassed&&
					ApplyProductionMirrorResult(advanced,production[level],error);
				if(!mirrorApplied){std::fprintf(stderr,
					"temporal production level=%zu step=%zu failed advanced=%d validation=%d "
					"pre=%.17g post=%.17g error=%s\n",level,step,mirrorAdvanced?1:0,
					mirrorAdvanced&&advanced.projection.validationPassed?1:0,
					mirrorAdvanced?advanced.projection.maximumPreProjectionResidualPerS:0.0,
					mirrorAdvanced?advanced.projection.maximumPostProjectionResidualPerS:0.0,
					error.c_str());return 177;}
				if(step+1u==stepCounts[level])finalProduction[level]=std::move(advanced);
			}
		}
		std::array<FilteredField,3> productionFiltered,oracleFiltered;
		std::array<FilteredVelocityField,3> productionVelocity,oracleVelocity;
		std::array<std::array<double,9>,3> productionInventory,oracleInventory;
		for(std::size_t level=0u;level<stepCounts.size();++level){
			std::vector<ConservativeVector> productionConservative(production[level].states.size());
			for(std::size_t cell=0u;cell<productionConservative.size();++cell)
				for(std::size_t component=0u;component<9u;++component)
					productionConservative[cell][component]=
						finalProduction[level].conservativeValues[
							component*productionConservative.size()+cell];
			PeriodicMACField productionTerminalVelocity;
			productionTerminalVelocity.component=finalProduction[level].projection.velocityMPerS;
			if(!FilterConservative(beginning,productionConservative,
				beginning.values.characteristicDiameterM,productionFiltered[level])||
				!FilterConservative(beginning,oracle[level].conservative,
					beginning.values.characteristicDiameterM,oracleFiltered[level])||
				!FilterVelocity(beginning,productionTerminalVelocity,
					beginning.values.characteristicDiameterM,productionVelocity[level])||
				!FilterVelocity(beginning,oracle[level].velocityMPerS,
					beginning.values.characteristicDiameterM,oracleVelocity[level]))return 178;
			productionInventory[level]=ComponentInventoryDensity(productionConservative);
			oracleInventory[level]=ComponentInventoryDensity(oracle[level].conservative);
		}
		const std::array<double,9> productionCoarse=FieldDistance(
			productionFiltered[0],productionFiltered[1]);
		const std::array<double,9> productionFine=FieldDistance(
			productionFiltered[1],productionFiltered[2]);
		const std::array<double,9> oracleCoarse=FieldDistance(
			oracleFiltered[0],oracleFiltered[1]);
		const std::array<double,9> oracleFine=FieldDistance(
			oracleFiltered[1],oracleFiltered[2]);
		unsigned int productionRefusals=0u,oracleRefusals=0u;
		unsigned int productionFloors=0u,oracleFloors=0u;
		bool expectedOracleEnergyFloor=false;
		const FireProductionCalibration::FilteredTemporalPlateauAuthority
			oracleEnergyPlateauAuthority={0.0012312438866646748,0.001273209006325096};
		for(std::size_t component=0u;component<9u;++component){
			double productionOrder=0.0,productionDistance=0.0;
			double oracleOrder=0.0,oracleDistance=0.0;
			FireProductionCalibration::FilteredTemporalDistanceMode productionMode=
				FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
			FireProductionCalibration::FilteredTemporalDistanceMode oracleMode=
				FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
			const bool productionAccepted=FireProductionCalibration::FilteredTemporalDistance(
				productionCoarse[component],productionFine[component],1.0,
				nullptr,productionOrder,productionDistance,productionMode);
			const bool oracleAccepted=FireProductionCalibration::FilteredTemporalDistance(
				oracleCoarse[component],
					oracleFine[component],2.0,component==8u?&oracleEnergyPlateauAuthority:nullptr,
					oracleOrder,oracleDistance,oracleMode);
			if(!productionAccepted)++productionRefusals;
			if(!oracleAccepted)++oracleRefusals;
			if(productionMode==FireProductionCalibration::FilteredTemporalDistanceMode::
				MeasuredFloorUpperBound)++productionFloors;
			if(oracleMode==FireProductionCalibration::FilteredTemporalDistanceMode::
				MeasuredFloorUpperBound){++oracleFloors;if(component==8u)
				expectedOracleEnergyFloor=true;}
			std::fprintf(stderr,"temporal scalar component=%zu production_D=%.17g/%.17g "
				"order=%.17g E=%.17g mode=%s accepted=%d oracle_D=%.17g/%.17g "
				"order=%.17g E=%.17g mode=%s accepted=%d\n",
				component,productionCoarse[component],productionFine[component],productionOrder,
				productionDistance,FireProductionCalibration::FilteredTemporalDistanceModeName(
					productionMode),productionAccepted?1:0,oracleCoarse[component],
				oracleFine[component],oracleOrder,oracleDistance,
				FireProductionCalibration::FilteredTemporalDistanceModeName(oracleMode),
				oracleAccepted?1:0);
		}
		const double productionVelocityCoarse=VelocityDistance(productionVelocity[0],
			productionVelocity[1]);
		const double productionVelocityFine=VelocityDistance(productionVelocity[1],
			productionVelocity[2]);
		const double oracleVelocityCoarse=VelocityDistance(oracleVelocity[0],oracleVelocity[1]);
		const double oracleVelocityFine=VelocityDistance(oracleVelocity[1],oracleVelocity[2]);
		double productionVelocityOrder=0.0,productionVelocityDistance=0.0;
		double oracleVelocityOrder=0.0,oracleVelocityDistance=0.0;
		FireProductionCalibration::FilteredTemporalDistanceMode productionVelocityMode=
			FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
		FireProductionCalibration::FilteredTemporalDistanceMode oracleVelocityMode=
			FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
		const bool productionVelocityAccepted=FireProductionCalibration::FilteredTemporalDistance(
			productionVelocityCoarse,productionVelocityFine,1.0,nullptr,productionVelocityOrder,
			productionVelocityDistance,productionVelocityMode);
		const bool oracleVelocityAccepted=FireProductionCalibration::FilteredTemporalDistance(
				oracleVelocityCoarse,oracleVelocityFine,2.0,nullptr,oracleVelocityOrder,
				oracleVelocityDistance,oracleVelocityMode);
		if(!productionVelocityAccepted)++productionRefusals;
		if(!oracleVelocityAccepted)++oracleRefusals;
		if(productionVelocityMode==FireProductionCalibration::FilteredTemporalDistanceMode::
			MeasuredFloorUpperBound)++productionFloors;
		if(oracleVelocityMode==FireProductionCalibration::FilteredTemporalDistanceMode::
			MeasuredFloorUpperBound)++oracleFloors;
		std::fprintf(stderr,"temporal velocity production_D=%.17g/%.17g order=%.17g E=%.17g "
			"mode=%s accepted=%d oracle_D=%.17g/%.17g order=%.17g E=%.17g mode=%s "
			"accepted=%d\n",
			productionVelocityCoarse,
			productionVelocityFine,productionVelocityOrder,productionVelocityDistance,
			FireProductionCalibration::FilteredTemporalDistanceModeName(productionVelocityMode),
			productionVelocityAccepted?1:0,oracleVelocityCoarse,oracleVelocityFine,
			oracleVelocityOrder,oracleVelocityDistance,
			FireProductionCalibration::FilteredTemporalDistanceModeName(oracleVelocityMode),
			oracleVelocityAccepted?1:0);
		for(std::size_t component=0u;component<9u;++component){
			const double productionCoarseDifference=std::fabs(productionInventory[0][component]-
				productionInventory[1][component]);
			const double productionFineDifference=std::fabs(productionInventory[1][component]-
				productionInventory[2][component]);
			const double oracleCoarseDifference=std::fabs(oracleInventory[0][component]-
				oracleInventory[1][component]);
			const double oracleFineDifference=std::fabs(oracleInventory[1][component]-
				oracleInventory[2][component]);
			double productionOrder=0.0,productionDistance=0.0;
			double oracleOrder=0.0,oracleDistance=0.0;
			FireProductionCalibration::FilteredTemporalDistanceMode productionMode=
				FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
			FireProductionCalibration::FilteredTemporalDistanceMode oracleMode=
				FireProductionCalibration::FilteredTemporalDistanceMode::Rejected;
			const bool productionAccepted=FireProductionCalibration::FilteredTemporalDistance(
				productionCoarseDifference,productionFineDifference,1.0,nullptr,productionOrder,
				productionDistance,productionMode);
			const bool oracleAccepted=FireProductionCalibration::FilteredTemporalDistance(
					oracleCoarseDifference,oracleFineDifference,2.0,nullptr,oracleOrder,
					oracleDistance,oracleMode);
			if(!productionAccepted)++productionRefusals;
			if(!oracleAccepted)++oracleRefusals;
			if(productionMode==FireProductionCalibration::FilteredTemporalDistanceMode::
				MeasuredFloorUpperBound)++productionFloors;
			if(oracleMode==FireProductionCalibration::FilteredTemporalDistanceMode::
				MeasuredFloorUpperBound)++oracleFloors;
			std::fprintf(stderr,"temporal ledger component=%zu production_D=%.17g/%.17g "
				"order=%.17g E=%.17g mode=%s accepted=%d oracle_D=%.17g/%.17g "
				"order=%.17g E=%.17g mode=%s accepted=%d\n",
				component,productionCoarseDifference,productionFineDifference,productionOrder,
				productionDistance,FireProductionCalibration::FilteredTemporalDistanceModeName(
					productionMode),productionAccepted?1:0,oracleCoarseDifference,
				oracleFineDifference,oracleOrder,oracleDistance,
				FireProductionCalibration::FilteredTemporalDistanceModeName(oracleMode),
				oracleAccepted?1:0);
		}
		const bool acceptedMatrix=productionRefusals==0u&&oracleRefusals==0u&&
			productionFloors==0u&&oracleFloors==1u&&expectedOracleEnergyFloor;
		std::fprintf(stderr,"temporal refinement complete baseline=%.17g horizon=%.17g "
			"target_sha256=%s/%s/%s amendment_sha256=%s refusals=%u/%u "
			"floor_bounds=%u/%u sole_floor=oracle_scalar_8 accepted=%d\n",baseline,
			8.0*baseline,
			targetDigest[0].c_str(),targetDigest[1].c_str(),targetDigest[2].c_str(),
			expectedAmendment,productionRefusals,oracleRefusals,productionFloors,oracleFloors,
			acceptedMatrix?1:0);
		return acceptedMatrix?192:193;
	}
	bool ApplyAcceptedProductionResult(const RISE::FireProductionResidentStepResult& production,
		const RISE::FireProductionAcceptedManifoldObservation& acceptedObservation,
		MethaneRunCheckpoint& state,std::string& error,
		const unsigned int temperatureWorkerCount=1u)
	{
		if(!acceptedObservation.MatchesAcceptedResidentPayload(production))
			return Fail(&error,"accepted manifold observation no longer matches resident payload");
		// The 1e-3 pressure-deviation check in the binary64 core is an oracle
		// validity detector, not a production thermochemistry-domain consumer.
		// Accepted production publication retains the r60 conservative envelope
		// and table-bounded temperature inversion without inheriting that detector.
		return ApplyProductionResultUnchecked(production,state,error,false,
			temperatureWorkerCount);
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

	struct ProductionConsumerFailure
	{
		std::size_t cell;
		double temperatureK;
		double eosResidual;
		bool viscosityTotal;
		ProductionConsumerFailure() : cell(std::numeric_limits<std::size_t>::max()),
			temperatureK(0.0),eosResidual(0.0),viscosityTotal(false) {}
	};

	ProductionConsumerFailure MeasureProductionConsumerFailure(
		const RISE::FireProductionResidentStepResult& production,const std::size_t cells)
	{
		ProductionConsumerFailure failure;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(std::size_t cell=0u;cell<cells;++cell){ConservativeVector conservative;
			for(std::size_t component=0u;component<9u;++component)
				conservative[component]=production.conservativeValues[component*cells+cell];
			MethaneCellState state=FromConservativeVector(conservative,
				FireStateProducerPrecision::Binary32);std::string error;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(state,fuel.TemperatureMinK(),
				fuel.TemperatureMaxK(),fuel,FireStateProducerPrecision::Binary32,
				state.temperatureK,&error))continue;
			double residual=0.0;
			if(!EquationOfStateResidual(state,fuel,FireStateProducerPrecision::Binary32,
				residual,&error)||residual>1.0e-3){failure.cell=cell;
				failure.temperatureK=state.temperatureK;failure.eosResidual=residual;
				CellMolecularTransportEvaluation molecular;
				failure.viscosityTotal=EvaluateCellMolecularTransport(state,fuel,
					FireSimulationTransportRecord::OpenV1(),FireStateProducerPrecision::Binary32,
					molecular,&error)&&molecular.molecularViscosityPaS>0.0;
				return failure;}
		}
		return failure;
	}

	struct ProductionEOSDeviation
	{
		double signedProbe;
		double maximumAbsolute;
		double signedAtMaximum;
		std::size_t maximumCell;
		ProductionEOSDeviation() : signedProbe(0.0),maximumAbsolute(0.0),
			signedAtMaximum(0.0),maximumCell(0u) {}
	};

	bool MeasureCheckpointEOSDeviation(const MethaneRunCheckpoint& state,
		const std::size_t probeCell,ProductionEOSDeviation& result,
		std::vector<double>* signedDeviation,std::string& error)
	{
		result=ProductionEOSDeviation();
		if(probeCell>=state.states.size())return false;
		if(signedDeviation)signedDeviation->assign(state.states.size(),0.0);
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(std::size_t cell=0u;cell<state.states.size();++cell){double ratio=0.0;
			const ConservativeVector conservative=ToConservativeVector(state.states[cell]);
			if(!AcceptedConservativeVolumeRatio(conservative,fuel,
				state.states[cell].producerPrecision,ratio,&error))return false;
			const double deviation=ratio-1.0;
			if(signedDeviation)(*signedDeviation)[cell]=deviation;
			if(cell==probeCell)result.signedProbe=deviation;
			if(std::fabs(deviation)>result.maximumAbsolute){
				result.maximumAbsolute=std::fabs(deviation);
				result.signedAtMaximum=deviation;result.maximumCell=cell;}
		}
		return true;
	}

	bool MeasureProductionEOSDeviation(const RISE::FireProductionResidentStepResult& production,
		const std::size_t cells,const std::size_t probeCell,ProductionEOSDeviation& result,
		std::string& error)
	{
		result=ProductionEOSDeviation();
		if(probeCell>=cells||production.conservativeValues.size()!=9u*cells)return false;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(std::size_t cell=0u;cell<cells;++cell){ConservativeVector conservative;
			for(std::size_t component=0u;component<9u;++component)
				conservative[component]=production.conservativeValues[component*cells+cell];
			double ratio=0.0;if(!AcceptedConservativeVolumeRatio(conservative,fuel,
				FireStateProducerPrecision::Binary32,ratio,&error))return false;
			const double deviation=ratio-1.0;
			if(cell==probeCell)result.signedProbe=deviation;
			if(std::fabs(deviation)>result.maximumAbsolute){
				result.maximumAbsolute=std::fabs(deviation);
				result.signedAtMaximum=deviation;result.maximumCell=cell;}
		}
		return true;
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

	double Percentile95(std::vector<double> values)
	{
		std::sort(values.begin(),values.end());
		return values[(95u*values.size()+99u)/100u-1u];
	}

	int DiagnoseRoundoff(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 230;
		std::array<std::string,4> targetDigests;
		if(!ReadTargetDigests(directory/"dyadic_targets.v1",targetDigests))return 231;
		MethaneRunCheckpoint state;std::string error;
		if(!BuildAnalyticState(6u,state,error))return 232;
		std::vector<std::vector<double> > sealed;const std::filesystem::path target=
			directory/"oracle_tier6_sdiv_x8.f64";
		if(DigestFile(target)!=targetDigests[2]||!ReadCalibrationDoublePayload(target,
			state.states.size(),8u,sealed))return 233;
		const double flowThrough=6.0*std::sqrt(
			state.values.characteristicDiameterM/Gravity);
		RISE::FireProductionResidentStepRequest request;
		if(!BuildProductionRequest(state,sealed[0],flowThrough/512.0,request,error))return 234;
		// r136 is a frozen two-projection instrumentation proof.  The production
		// default changed to monitored one-projection operation in r168, so bind
		// the historical topology explicitly instead of inheriting that default.
		request.monitorManifoldDiagnostics=true;
		request.enforceManifoldPlateau=true;
		FireProductionRoundoffAdapter::ResidentStepTraceResult trace;
		FireProductionRoundoffWalker::BranchWitness independentBranch;
		const bool independentBranchStopped=FireProductionRoundoffWalker::
			WalkFirstCellXLimiterBranch(request.cellTransport.shape.nx,
				request.cellTransport.shape.ny,request.cellTransport.shape.nz,
				request.cellTransport.componentCount,
				request.cellTransport.boundary[0]==RISE::FireProductionProjectionPeriodic,
				request.cellTransport.boundary[0]==RISE::FireProductionProjectionPressureOpen,
				request.cellTransport.boundary[1]==RISE::FireProductionProjectionPressureOpen,
				request.cellTransport.conservativeValues,request.cellTransport.ambientValues,
				request.cellTransport.frozenVelocityMPerS[0],independentBranch);
		FireProductionRoundoffWalker::PPMQuadraticZeroCertificate ppmCertificate;
		const bool ppmCertified=independentBranchStopped&&
			FireProductionRoundoffWalker::CertifyPPMQuadraticZero(
				independentBranch,ppmCertificate);
		std::fprintf(stderr,"r120 independent_branch stopped=%d line=%zu cell=%zu component=%zu "
			"left=(%.17g +- %.17g, %.9g) right=(%.17g +- %.17g, %.9g) result=%d\n",
			independentBranchStopped?1:0,independentBranch.line,independentBranch.cell,
			independentBranch.component,independentBranch.leftCenter,
			independentBranch.leftRadius,independentBranch.leftRounded,
			independentBranch.rightCenter,independentBranch.rightRadius,
			independentBranch.rightRounded,independentBranch.roundedResult?1:0);
		std::fprintf(stderr,"r123 independent_ppm certified=%d ambiguity=%.17g "
			"arithmetic=%.17g divergence=%.17g linear=(%.17g +- %.17g) "
			"endpoint=(%.17g, %.17g)\n",ppmCertified?1:0,
			ppmCertificate.ambiguityWidth,ppmCertificate.arithmeticResidualBound,
			ppmCertificate.divergenceBound,independentBranch.linearCenter,
			independentBranch.linearRadius,independentBranch.endpointAbsoluteUpper,
			independentBranch.endpointRadius);
		if(!FireProductionRoundoffAdapter::AdvanceResidentStepTrace(request,0.0f,trace,&error)){
			std::fprintf(stderr,"r120 trace failed: %s\n",error.c_str());return 235;}
		std::array<unsigned int,6> projectionBoundary={};
		for(unsigned int side=0u;side<6u;++side)projectionBoundary[side]=
			request.force.boundary[side]==RISE::FireProductionProjectionPressureOpen?2u:
			(request.force.boundary[side]==RISE::FireProductionProjectionWall?1u:0u);
		const std::array<std::size_t,3> projectionExtent={{request.force.shape.nx,
			request.force.shape.ny,request.force.shape.nz}};
		FireProductionRoundoffWalker::ProjectionAposterioriCertificate physicalCertificate,
			restorationCertificate;
		const bool physicalAposteriori=FireProductionRoundoffWalker::
			DeriveProjectionAposterioriBound(projectionExtent,projectionBoundary,
				request.force.shape.cellWidthM,trace.physicalStreaming.densityLower,
				trace.physicalStreaming.densityUpper,
				trace.physicalStreaming.maximumRoundedResidual,
				trace.physicalStreaming.maximumResidualEvaluationRadius,
				trace.physicalStreaming.maximumCrossPrecisionResidualUpper,
				trace.physicalStreaming.maximumRoundedVelocity,
				trace.physicalStreaming.maximumRoundedTarget,
				trace.physicalStreaming.maximumBeginningVelocityRoundingUpper,false,
				trace.physicalStreaming.streamingFaceVelocityL2PerCellUpper,
				trace.physicalStreaming.fp64TerminalFaceL2PerCellUpper,
				trace.physicalStreaming.validationToleranceRounded,
				trace.physicalStreaming.validationToleranceRadius,physicalCertificate);
		const bool restorationAposteriori=FireProductionRoundoffWalker::
			DeriveProjectionAposterioriBound(projectionExtent,projectionBoundary,
				request.force.shape.cellWidthM,trace.restorationStreaming.densityLower,
				trace.restorationStreaming.densityUpper,
				trace.restorationStreaming.maximumRoundedResidual,
				trace.restorationStreaming.maximumResidualEvaluationRadius,
				trace.restorationStreaming.maximumCrossPrecisionResidualUpper,
				trace.restorationStreaming.maximumRoundedVelocity,
				trace.restorationStreaming.maximumRoundedTarget,
				trace.restorationStreaming.maximumBeginningVelocityRoundingUpper,true,
				trace.restorationStreaming.streamingFaceVelocityL2PerCellUpper,
				trace.restorationStreaming.fp64TerminalFaceL2PerCellUpper,
				trace.restorationStreaming.validationToleranceRounded,
				trace.restorationStreaming.validationToleranceRadius,restorationCertificate);
		const bool projectionCertificatesApplied=trace.stages.size()==24u&&
			physicalAposteriori&&restorationAposteriori&&
			FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
				trace.stages[22],trace.physicalStreaming.maximumRoundedResidual,
				trace.physicalStreaming.validationToleranceRounded,
				trace.physicalStreaming.maximumResidualEvaluationRadius,
				trace.physicalStreaming.validationToleranceRadius,
				physicalCertificate.velocityRMSUpper)&&
			FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
				trace.stages[23],trace.restorationStreaming.maximumRoundedResidual,
				trace.restorationStreaming.validationToleranceRounded,
				trace.restorationStreaming.maximumResidualEvaluationRadius,
				trace.restorationStreaming.validationToleranceRadius,
				restorationCertificate.velocityRMSUpper);
		std::array<FireProductionRoundoffWalker::FullStepMetricStage,24> metricStages={};
		if(trace.stages.size()==metricStages.size())for(std::size_t stage=0u;
			stage<metricStages.size();++stage)for(unsigned int channel=0u;channel<12u;
			++channel){metricStages[stage].radiusSum[channel]=
				trace.stages[stage].metricOutputRadiusSum[channel];
			metricStages[stage].radiusSquareSum[channel]=
				trace.stages[stage].metricOutputRadiusSquareSum[channel];
			metricStages[stage].maximumRadius[channel]=
				trace.stages[stage].metricOutputMaximumRadius[channel];
			metricStages[stage].count[channel]=trace.stages[stage].metricOutputCount[channel];}
		FireProductionRoundoffWalker::FullStepRoundoffCertificate fullStepCertificate;
		const bool fullStepDerived=projectionCertificatesApplied&&
			FireProductionRoundoffWalker::DeriveFullStepRoundoffBound(metricStages,
				request.force.shape.CellCount(),physicalCertificate.densityLower,
				physicalCertificate.densityUpper,request.force.shape.cellWidthM,
				request.force.timeStepS,request.force.ambientDensityKGPerM3,
				trace.physicalStreaming.maximumRoundedVelocity,
				trace.physicalStreaming.maximumBeginningVelocityRoundingUpper,
				trace.physicalStreaming.projectionCorrectionL2PerCell,
				trace.restorationStreaming.projectionCorrectionL2PerCell,
				physicalCertificate.velocityRMSUpper,restorationCertificate.velocityRMSUpper,
				fullStepCertificate);
		std::fprintf(stderr,"r136 full_step derived=%d proof_gaps=0x%02x gas_rms=%.17g density_relative=%.17g "
			"provisional_velocity=%.17g physical=%.17g final_velocity=%.17g open=%.17g "
			"correction=%.17g/%.17g\n",
			fullStepDerived?1:0,fullStepCertificate.proofGapBitmap,
			fullStepCertificate.gasDensityRMSUpper,
			fullStepCertificate.densityRelativeUpper,
			fullStepCertificate.provisionalVelocityL2PerCellUpper,
			fullStepCertificate.physicalProjectionVelocityUpper,
			fullStepCertificate.finalVelocityRMSUpper,
			fullStepCertificate.openBoundaryInteractionUpper,
			trace.physicalStreaming.projectionCorrectionL2PerCell,
			trace.restorationStreaming.projectionCorrectionL2PerCell);
		std::fprintf(stderr,"r135 scalar_l1");for(const double value:
			fullStepCertificate.scalarFilteredL1Upper)std::fprintf(stderr," %.17g",value);
		std::fprintf(stderr,"\nr135 scalar_rms");for(const double value:
			fullStepCertificate.scalarRMSUpper)std::fprintf(stderr," %.17g",value);
		std::fprintf(stderr,"\nr135 inventory");for(const double value:
			fullStepCertificate.inventoryPerVolumeUpper)std::fprintf(stderr," %.17g",value);
		std::fprintf(stderr,"\nr135 momentum_l2 %.17g %.17g %.17g\n",
			fullStepCertificate.momentumL2PerCellUpper[0],
			fullStepCertificate.momentumL2PerCellUpper[1],
			fullStepCertificate.momentumL2PerCellUpper[2]);
		std::fprintf(stderr,"r134 physical_aposteriori valid=%d applied=%d rho=[%.17g,%.17g] "
			"lambda0=%.17g lambda=%.17g inverse=%.17g residual=%.17g eval=%.17g "
			"cross=%.17g fp64_gate=%.17g fp64_eval=%.17g fp64_terminal=%.17g "
			"beginning=%.17g feedback=%.17g "
			"max_velocity=%.17g max_target=%.17g face_stream=%.17g gain=%.17g velocity=%.17g "
			"rounded_residual=%.9g tolerance=%.9g "
			"tolerance_rounding=%.17g predicate_margin=%.17g matches=%d/%d\n",
			physicalAposteriori?1:0,
			projectionCertificatesApplied?1:0,physicalCertificate.densityLower,
			physicalCertificate.densityUpper,physicalCertificate.dimensionlessEigenvalueLower,
			physicalCertificate.operatorEigenvalueLower,
			physicalCertificate.inverseOperatorNormUpper,
			physicalCertificate.acceptedRoundedResidual,
			physicalCertificate.residualEvaluationRoundingUpper,
			physicalCertificate.crossPrecisionResidualUpper,
			physicalCertificate.fp64ResidualGateUpper,
			physicalCertificate.fp64ResidualEvaluationUpper,
			physicalCertificate.fp64TerminalFaceL2PerCellUpper,
			physicalCertificate.beginningVelocityRoundingUpper,
			physicalCertificate.fp64FeedbackFactor,
			trace.physicalStreaming.maximumRoundedVelocity,
			trace.physicalStreaming.maximumRoundedTarget,
			physicalCertificate.streamingFaceVelocityL2PerCellUpper,
			physicalCertificate.velocityGainUpper,
			physicalCertificate.velocityRMSUpper,trace.physicalStreaming.maximumRoundedResidual,
			trace.physicalStreaming.validationToleranceRounded,
			physicalCertificate.validationToleranceRoundingUpper,
			physicalCertificate.validationPredicateMarginLower,
			trace.physicalStreaming.roundedResidualMatches?1:0,
			trace.physicalStreaming.roundedVelocityMatches?1:0);
		std::fprintf(stderr,"r134 restoration_aposteriori valid=%d applied=%d rho=[%.17g,%.17g] "
			"lambda0=%.17g lambda=%.17g inverse=%.17g residual=%.17g eval=%.17g "
			"cross=%.17g fp64_gate=%.17g fp64_eval=%.17g fp64_terminal=%.17g "
			"beginning=%.17g feedback=%.17g "
			"max_velocity=%.17g max_target=%.17g face_stream=%.17g gain=%.17g velocity=%.17g "
			"rounded_residual=%.9g tolerance=%.9g "
			"tolerance_rounding=%.17g predicate_margin=%.17g matches=%d/%d\n",
			restorationAposteriori?1:0,
			projectionCertificatesApplied?1:0,
			restorationCertificate.densityLower,restorationCertificate.densityUpper,
			restorationCertificate.dimensionlessEigenvalueLower,
			restorationCertificate.operatorEigenvalueLower,
			restorationCertificate.inverseOperatorNormUpper,
			restorationCertificate.acceptedRoundedResidual,
			restorationCertificate.residualEvaluationRoundingUpper,
			restorationCertificate.crossPrecisionResidualUpper,
			restorationCertificate.fp64ResidualGateUpper,
			restorationCertificate.fp64ResidualEvaluationUpper,
			restorationCertificate.fp64TerminalFaceL2PerCellUpper,
			restorationCertificate.beginningVelocityRoundingUpper,
			restorationCertificate.fp64FeedbackFactor,
			trace.restorationStreaming.maximumRoundedVelocity,
			trace.restorationStreaming.maximumRoundedTarget,
			restorationCertificate.streamingFaceVelocityL2PerCellUpper,
			restorationCertificate.velocityGainUpper,restorationCertificate.velocityRMSUpper,
			trace.restorationStreaming.maximumRoundedResidual,
			trace.restorationStreaming.validationToleranceRounded,
			restorationCertificate.validationToleranceRoundingUpper,
			restorationCertificate.validationPredicateMarginLower,
			trace.restorationStreaming.roundedResidualMatches?1:0,
			trace.restorationStreaming.roundedVelocityMatches?1:0);
		RISECBOR64::Bytes encoded;std::uint32_t unresolvedBitmap=0u,invalidBitmap=0u;
		const char* traceSources[]={RISEFireProductionTrace::SourceManifest::Generator,
			RISEFireProductionTrace::SourceManifest::FireProductionAdvectionHeader,
			RISEFireProductionTrace::SourceManifest::FireProductionAdvectionSource,
			RISEFireProductionTrace::SourceManifest::FireProductionTransportHeader,
			RISEFireProductionTrace::SourceManifest::FireProductionTransportSource,
			RISEFireProductionTrace::SourceManifest::FireProductionForceHeader,
			RISEFireProductionTrace::SourceManifest::FireProductionForceSource,
			RISEFireProductionTrace::SourceManifest::FireProductionProjectionHeader,
			RISEFireProductionTrace::SourceManifest::FireProductionProjectionSource,
			RISEFireProductionTrace::SourceManifest::TraceAdapter,
			RISEFireProductionTrace::SourceManifest::TraceCore,
			RISEFireProductionTrace::SourceManifest::IndependentWalker};
		for(const char* source:traceSources)AppendText(encoded,source);
		for(const auto* certificate:{&physicalCertificate,&restorationCertificate}){
			AppendDouble(encoded,certificate->densityLower);
			AppendDouble(encoded,certificate->densityUpper);
			AppendDouble(encoded,certificate->dimensionlessEigenvalueLower);
			AppendDouble(encoded,certificate->operatorEigenvalueLower);
			AppendDouble(encoded,certificate->inverseOperatorNormUpper);
			AppendDouble(encoded,certificate->velocityGainUpper);
			AppendDouble(encoded,certificate->acceptedRoundedResidual);
			AppendDouble(encoded,certificate->residualEvaluationRoundingUpper);
			AppendDouble(encoded,certificate->crossPrecisionResidualUpper);
			AppendDouble(encoded,certificate->fp64ResidualGateUpper);
			AppendDouble(encoded,certificate->fp64ResidualEvaluationUpper);
			AppendDouble(encoded,certificate->fp64FeedbackFactor);
			AppendDouble(encoded,certificate->fp64TerminalFaceL2PerCellUpper);
			AppendDouble(encoded,certificate->beginningVelocityRoundingUpper);
			AppendDouble(encoded,certificate->validationToleranceRounded);
			AppendDouble(encoded,certificate->validationToleranceRoundingUpper);
			AppendDouble(encoded,certificate->validationPredicateMarginLower);
			AppendInteger(encoded,certificate->validationPredicateSeparated?1u:0u);
			AppendDouble(encoded,certificate->streamingFaceVelocityL2PerCellUpper);
			AppendDouble(encoded,certificate->velocityRMSUpper);
		}
		std::fprintf(stderr,"r120 diagnostic tier=6 stages=%zu schedule=%u\n",trace.stages.size(),
			trace.force.schedule.substepCount);
		std::uint64_t totalBranchObligationCount=0u;
		std::uint64_t totalDischargedBranchObligationCount=0u;
		std::array<std::uint64_t,static_cast<unsigned int>(
			FireProductionRoundoffTrace::BranchSite::Count)> totalSiteCount={},
			totalPendingSiteCount={};
		for(std::size_t index=0u;index<trace.stages.size();++index){
			const FireProductionRoundoffTrace::Observation& stage=trace.stages[index];
			totalBranchObligationCount+=stage.branchObligations.size();
			totalDischargedBranchObligationCount+=stage.dischargedBranchObligationCount;
			AppendInteger(encoded,index);
			for(const std::uint64_t count:stage.operation)AppendInteger(encoded,count);
			for(const double operand:stage.maximumAbsoluteOperand)AppendDouble(encoded,operand);
			for(const double radius:stage.maximumResultRadius)AppendDouble(encoded,radius);
			AppendInteger(encoded,stage.maximumDepth);AppendInteger(encoded,stage.comparisonCount);
			AppendDouble(encoded,stage.minimumBranchMargin);
			AppendDouble(encoded,stage.minimumDenominatorLowerBound);
			AppendDouble(encoded,stage.minimumSqrtDomainLowerBound);
			AppendDouble(encoded,stage.maximumAbsoluteOutput);
			AppendDouble(encoded,stage.maximumOutputRadius);
			for(const double value:stage.metricOutputRadiusSum)AppendDouble(encoded,value);
			for(const double value:stage.metricOutputRadiusSquareSum)AppendDouble(encoded,value);
			for(const double value:stage.metricOutputMaximumRadius)AppendDouble(encoded,value);
			for(const std::uint64_t value:stage.metricOutputCount)AppendInteger(encoded,value);
			for(const std::uint64_t value:stage.nonfiniteMetricOutputRadiusCount)
				AppendInteger(encoded,value);
			for(const std::uint64_t value:stage.firstNonfiniteMetricOutputIndex)
				AppendInteger(encoded,value);
			for(const double value:stage.firstNonfiniteMetricOutputCenter)AppendDouble(encoded,value);
			for(const float value:stage.firstNonfiniteMetricOutputRounded){std::uint32_t bits=0u;
				std::memcpy(&bits,&value,sizeof(bits));AppendInteger(encoded,bits);}
			for(const double value:stage.aposterioriMetricBound)AppendDouble(encoded,value);
			AppendInteger(encoded,stage.aposterioriProjectionCertified?1u:0u);
			AppendDouble(encoded,stage.transportBranchDivergenceBound);
			for(const double divergence:stage.maximumBranchDivergence)
				AppendDouble(encoded,divergence);
			AppendInteger(encoded,stage.unresolvedBranch?1u:0u);
			AppendInteger(encoded,stage.invalidDomain?1u:0u);
			AppendInteger(encoded,stage.arithmeticInvalidDomainCount);
			AppendInteger(encoded,stage.projectionSolveArithmeticInvalidDomainCount);
			AppendInteger(encoded,stage.unresolvedWitnessRecorded?1u:0u);
			AppendDouble(encoded,stage.unresolvedLeftCenter);
			AppendDouble(encoded,stage.unresolvedLeftRadius);
			AppendDouble(encoded,stage.unresolvedRightCenter);
			AppendDouble(encoded,stage.unresolvedRightRadius);
			std::uint32_t leftBits=0u,rightBits=0u;
			std::memcpy(&leftBits,&stage.unresolvedLeftRounded,sizeof(leftBits));
			std::memcpy(&rightBits,&stage.unresolvedRightRounded,sizeof(rightBits));
			AppendInteger(encoded,leftBits);AppendInteger(encoded,rightBits);
			AppendInteger(encoded,stage.unresolvedRoundedResult?1u:0u);
			AppendInteger(encoded,stage.branchObligations.size());
			AppendInteger(encoded,stage.dischargedBranchObligationCount);
			for(const FireProductionRoundoffTrace::BranchObligation& obligation:
				stage.branchObligations){
				++totalSiteCount[static_cast<unsigned int>(obligation.site)];
				if(obligation.certificate==FireProductionRoundoffTrace::
					BranchCertificate::None)++totalPendingSiteCount[
						static_cast<unsigned int>(obligation.site)];
				AppendInteger(encoded,obligation.comparisonOrdinal);
				AppendInteger(encoded,static_cast<unsigned int>(obligation.site));
				AppendDouble(encoded,obligation.predicateCenter);
				AppendDouble(encoded,obligation.predicateRadius);
				AppendInteger(encoded,obligation.roundedResult?1u:0u);
				AppendInteger(encoded,static_cast<unsigned int>(obligation.certificate));
				AppendDouble(encoded,obligation.divergenceBound);
				AppendDouble(encoded,obligation.proofLower);
				AppendDouble(encoded,obligation.proofRequired);
				std::uint32_t inactiveBits=0u,activeBits=0u;
				std::memcpy(&inactiveBits,&obligation.inactiveResultRounded,
					sizeof(inactiveBits));
				std::memcpy(&activeBits,&obligation.activeResultRounded,
					sizeof(activeBits));
				AppendInteger(encoded,inactiveBits);AppendInteger(encoded,activeBits);
			}
			AppendInteger(encoded,stage.invalidDenominatorWitnessRecorded?1u:0u);
			AppendDouble(encoded,stage.invalidDenominatorCenter);
			AppendDouble(encoded,stage.invalidDenominatorRadius);
			std::uint32_t denominatorBits=0u;std::memcpy(&denominatorBits,
				&stage.invalidDenominatorRounded,sizeof(denominatorBits));
			AppendInteger(encoded,denominatorBits);
			if(stage.unresolvedBranch)unresolvedBitmap|=std::uint32_t(1u)<<index;
			if(stage.invalidDomain)invalidBitmap|=std::uint32_t(1u)<<index;
			std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)
				operations+=count;
			std::fprintf(stderr,"r120 stage=%zu operations=%llu depth=%u comparisons=%llu "
				"obligations=%zu discharged=%llu unresolved=%d invalid=%d "
				"output=%.17g radius=%.17g denominator=%.17g "
				"sqrt_domain=%.17g branch_margin=%.17g\n",index,
				static_cast<unsigned long long>(operations),stage.maximumDepth,
				static_cast<unsigned long long>(stage.comparisonCount),
				stage.branchObligations.size(),static_cast<unsigned long long>(
					stage.dischargedBranchObligationCount),
				stage.unresolvedBranch?1:0,stage.invalidDomain?1:0,
				stage.maximumAbsoluteOutput,stage.maximumOutputRadius,
				stage.minimumDenominatorLowerBound,stage.minimumSqrtDomainLowerBound,
				stage.minimumBranchMargin);
			if(stage.unresolvedWitnessRecorded)std::fprintf(stderr,
				"r120 stage=%zu branch_witness left=(%.17g +- %.17g, %.9g) "
				"right=(%.17g +- %.17g, %.9g) rounded_result=%d separated=%d\n",index,
				stage.unresolvedLeftCenter,stage.unresolvedLeftRadius,
				stage.unresolvedLeftRounded,stage.unresolvedRightCenter,
				stage.unresolvedRightRadius,stage.unresolvedRightRounded,
				stage.unresolvedRoundedResult?1:0,
				FireProductionRoundoffWalker::IntervalsAreSeparated(
					stage.unresolvedLeftCenter,stage.unresolvedLeftRadius,
					stage.unresolvedRightCenter,stage.unresolvedRightRadius)?1:0);
			if(stage.invalidDenominatorWitnessRecorded)std::fprintf(stderr,
				"r120 stage=%zu denominator_witness center=%.17g radius=%.17g rounded=%.9g\n",
				index,stage.invalidDenominatorCenter,stage.invalidDenominatorRadius,
				stage.invalidDenominatorRounded);
			std::array<std::uint64_t,static_cast<unsigned int>(
				FireProductionRoundoffTrace::BranchSite::Count)> siteCount={},pendingCount={};
			for(const FireProductionRoundoffTrace::BranchObligation& obligation:
				stage.branchObligations){
				const unsigned int site=static_cast<unsigned int>(obligation.site);
				++siteCount[site];if(obligation.certificate==
					FireProductionRoundoffTrace::BranchCertificate::None)++pendingCount[site];
			}
			std::fprintf(stderr,"r120 stage=%zu obligation_census unknown=%llu/%llu "
				"ppm=%llu/%llu stationary_low=%llu/%llu stationary_high=%llu/%llu "
				"min=%llu/%llu max=%llu/%llu floor=%llu/%llu "
				"ceil=%llu/%llu (pending/total)\n",index,
				static_cast<unsigned long long>(pendingCount[0]),
				static_cast<unsigned long long>(siteCount[0]),
				static_cast<unsigned long long>(pendingCount[1]),
				static_cast<unsigned long long>(siteCount[1]),
				static_cast<unsigned long long>(pendingCount[2]),
				static_cast<unsigned long long>(siteCount[2]),
				static_cast<unsigned long long>(pendingCount[3]),
				static_cast<unsigned long long>(siteCount[3]),
				static_cast<unsigned long long>(pendingCount[4]),
				static_cast<unsigned long long>(siteCount[4]),
				static_cast<unsigned long long>(pendingCount[5]),
				static_cast<unsigned long long>(siteCount[5]),
				static_cast<unsigned long long>(pendingCount[6]),
				static_cast<unsigned long long>(siteCount[6]),
				static_cast<unsigned long long>(pendingCount[7]),
				static_cast<unsigned long long>(siteCount[7]));
			std::fprintf(stderr,"r120 stage=%zu branch_sites",index);
			for(unsigned int site=8u;site<siteCount.size();++site)if(siteCount[site])
				std::fprintf(stderr," %u=%llu/%llu",site,
					static_cast<unsigned long long>(pendingCount[site]),
					static_cast<unsigned long long>(siteCount[site]));
			std::fprintf(stderr," (pending/total)\n");
			std::fprintf(stderr,"r132 stage=%zu metric_radius",index);
			for(unsigned int channel=0u;channel<
				FireProductionRoundoffTrace::Observation::MetricChannelCount;++channel)
				if(stage.metricOutputCount[channel])std::fprintf(stderr,
					" %u=%.17g/%.17g/%.17g/%llu/nonfinite=%llu",
					channel,stage.metricOutputRadiusSum[channel]/static_cast<double>(
						stage.metricOutputCount[channel]),std::sqrt(
						stage.metricOutputRadiusSquareSum[channel]/static_cast<double>(
						stage.metricOutputCount[channel])),stage.metricOutputMaximumRadius[channel],
						static_cast<unsigned long long>(
							stage.metricOutputCount[channel]),static_cast<unsigned long long>(
							stage.nonfiniteMetricOutputRadiusCount[channel]));
			std::fprintf(stderr," (mean/rms/count)\n");
			for(unsigned int channel=0u;channel<
				FireProductionRoundoffTrace::Observation::MetricChannelCount;++channel)
				if(stage.nonfiniteMetricOutputRadiusCount[channel])std::fprintf(stderr,
					"r133 stage=%zu nonfinite_metric channel=%u count=%llu first=%llu "
					"center=%.17g rounded=%.9g\n",index,channel,
					static_cast<unsigned long long>(stage.nonfiniteMetricOutputRadiusCount[channel]),
					static_cast<unsigned long long>(stage.firstNonfiniteMetricOutputIndex[channel]),
					stage.firstNonfiniteMetricOutputCenter[channel],
					stage.firstNonfiniteMetricOutputRounded[channel]);
			std::array<unsigned int,static_cast<unsigned int>(
				FireProductionRoundoffTrace::BranchSite::Count)> printed={};
			for(const FireProductionRoundoffTrace::BranchObligation& obligation:
				stage.branchObligations)if(obligation.certificate==
					FireProductionRoundoffTrace::BranchCertificate::None&&
					printed[static_cast<unsigned int>(obligation.site)]++<2u)
				std::fprintf(stderr,"r120 stage=%zu pending ordinal=%llu site=%u "
					"predicate=(%.17g +- %.17g) rounded=%d proof=(%.17g >= %.17g) "
					"paths=(%.9g, %.9g)\n",index,
					static_cast<unsigned long long>(obligation.comparisonOrdinal),
					static_cast<unsigned int>(obligation.site),obligation.predicateCenter,
					obligation.predicateRadius,obligation.roundedResult?1:0,
					obligation.proofLower,obligation.proofRequired,
					obligation.inactiveResultRounded,obligation.activeResultRounded);
		}
		std::fprintf(stderr,"r124 class_census");
		for(unsigned int site=0u;site<totalSiteCount.size();++site)
			if(totalSiteCount[site])std::fprintf(stderr," %u=%llu/%llu",site,
				static_cast<unsigned long long>(totalPendingSiteCount[site]),
				static_cast<unsigned long long>(totalSiteCount[site]));
		std::fprintf(stderr," (pending/total)\n");
		const double frozenInflowAmbiguity=1.7632415612658968e-38;
		const double frozenInflowScale=22.033558699237727;
		const double frozenInflowUnitFactor=frozenInflowAmbiguity/
			(0x1p-24*frozenInflowScale);
		const double frozenInflowPowerOfTwoFactor=std::max(1.0,std::exp2(std::ceil(
			std::log2(frozenInflowUnitFactor))));
		std::fprintf(stderr,"r127 inflow_width ambiguity=%.17g scale=%.17g "
			"unit_factor=%.17g power_of_two_factor=%.17g\n",frozenInflowAmbiguity,
			frozenInflowScale,frozenInflowUnitFactor,frozenInflowPowerOfTwoFactor);
		std::array<double,static_cast<unsigned int>(
			FireProductionRoundoffTrace::BranchSite::Count)> maximumClassEnvelope={};
		for(const FireProductionRoundoffTrace::Observation& stage:trace.stages)
			for(unsigned int site=0u;site<maximumClassEnvelope.size();++site)
				maximumClassEnvelope[site]=std::max(maximumClassEnvelope[site],
					stage.maximumBranchDivergence[site]);
		std::fprintf(stderr,"branch_class_envelope");
		for(unsigned int site=0u;site<maximumClassEnvelope.size();++site)
			if(maximumClassEnvelope[site]>0.0)std::fprintf(stderr," %u=%.17g",site,
				maximumClassEnvelope[site]);
		std::fprintf(stderr,"\n");
		const std::string traceDigest=RISECBOR64::SHA256Hex(encoded);
		std::fprintf(stderr,"r120 trace_digest=%s unresolved_bitmap=0x%06x "
			"invalid_bitmap=0x%06x trace_generator=%s transport_source=%s\n",
			traceDigest.c_str(),unresolvedBitmap,invalidBitmap,
			RISEFireProductionTrace::SourceManifest::Generator,
			RISEFireProductionTrace::SourceManifest::FireProductionTransportSource);
		bool finiteGatedOutputs=true;
		for(std::size_t index=0u;index<trace.stages.size();++index){
			const auto& stage=trace.stages[index];
			finiteGatedOutputs=finiteGatedOutputs&&((index>=22u&&
				stage.aposterioriProjectionCertified&&std::isfinite(
					stage.aposterioriMetricBound[9]))||(index<22u&&
					std::isfinite(stage.maximumAbsoluteOutput)&&
					std::isfinite(stage.maximumOutputRadius)));}
		if(trace.stages.size()!=24u)return 238;
		const FireProductionRoundoffTrace::Observation& source=trace.stages[21];
		const FireProductionRoundoffTrace::Observation& physical=trace.stages[22];
		const FireProductionRoundoffTrace::Observation& restoration=trace.stages[23];
		std::uint64_t physicalInterpolationObligations=0u,
			restorationInterpolationObligations=0u;
		const bool interpolationTopology=FireProductionRoundoffWalker::
			CountProjectionInterpolationObligations(request.force.shape.nx,
				request.force.shape.ny,request.force.shape.nz,17u,
				physicalInterpolationObligations)&&FireProductionRoundoffWalker::
			CountProjectionInterpolationObligations(request.force.shape.nx,
				request.force.shape.ny,request.force.shape.nz,16u,
				restorationInterpolationObligations);
		std::fprintf(stderr,"r132 projection_interpolation topology=%d physical=%llu "
			"restoration=%llu physical_radius=%.17g restoration_radius=%.17g\n",
			interpolationTopology?1:0,static_cast<unsigned long long>(
				physicalInterpolationObligations),static_cast<unsigned long long>(
				restorationInterpolationObligations),physical.maximumOutputRadius,
			restoration.maximumOutputRadius);
		if(trace.force.schedule.substepCount!=1u||
			traceDigest!="0cf4fda287cf2019ad09a261c883f7b2e51b598a4e6f814ba2538bce86c0df42"||
			unresolvedBitmap!=0u||invalidBitmap!=0u||!finiteGatedOutputs||
			totalBranchObligationCount!=3972326u||
			totalDischargedBranchObligationCount!=3972326u||
			totalBranchObligationCount-totalDischargedBranchObligationCount!=0u||
			frozenInflowAmbiguity!=1.7632415612658968e-38||
			frozenInflowScale!=22.033558699237727||
			frozenInflowPowerOfTwoFactor!=1.0||
			!independentBranchStopped||independentBranch.line!=1u||independentBranch.cell!=6u||
			independentBranch.component!=3u||
			independentBranch.leftCenter!=-7.7486038219110043e-7||
			independentBranch.leftRadius!=1.2337798327030971e-6||
			independentBranch.leftRounded!=-7.152557373046875e-7f||
			independentBranch.rightCenter!=0.0||independentBranch.rightRadius!=0.0||
			independentBranch.rightRounded!=0.0f||!independentBranch.roundedResult||
			FireProductionRoundoffWalker::IntervalsAreSeparated(
				independentBranch.leftCenter,independentBranch.leftRadius,
				independentBranch.rightCenter,independentBranch.rightRadius)||
			!FireProductionRoundoffWalker::IntervalsAreSeparated(
				independentBranch.leftCenter,0.5*independentBranch.leftRadius,
				independentBranch.rightCenter,independentBranch.rightRadius)||
			!ppmCertified||!ppmCertificate.continuousAtSwitch||
			ppmCertificate.ambiguityWidth!=2.008640214894198e-6||
			ppmCertificate.arithmeticResidualBound!=2.6783670818887366e-6||
			ppmCertificate.divergenceBound!=3.1805271356122864e-6||
			!interpolationTopology||physicalInterpolationObligations!=765u||
			restorationInterpolationObligations!=720u||
			physical.maximumBranchDivergence[static_cast<unsigned int>(
				FireProductionRoundoffTrace::BranchSite::FloorBoundary)]!=0.0||
			restoration.maximumBranchDivergence[static_cast<unsigned int>(
				FireProductionRoundoffTrace::BranchSite::FloorBoundary)]!=0.0||
			!std::isinf(physical.maximumOutputRadius)||
			!std::isinf(restoration.maximumOutputRadius)||
			physical.nonfiniteMetricOutputRadiusCount[9]!=21600u||
			physical.nonfiniteMetricOutputRadiusCount[10]!=21600u||
			physical.nonfiniteMetricOutputRadiusCount[11]!=21312u||
			restoration.nonfiniteMetricOutputRadiusCount[9]!=21600u||
			restoration.nonfiniteMetricOutputRadiusCount[10]!=21600u||
			restoration.nonfiniteMetricOutputRadiusCount[11]!=21312u||
			physical.firstNonfiniteMetricOutputCenter[9]!=0.020628967447918926||
			restoration.firstNonfiniteMetricOutputCenter[9]!=0.019031353974387526||
			source.unresolvedBranch||source.invalidDomain||physical.unresolvedBranch||
			physical.invalidDomain||restoration.unresolvedBranch||
			restoration.invalidDomain||!projectionCertificatesApplied||
			physicalCertificate.densityLower!=0.97449028796070902||
			physicalCertificate.densityUpper!=1.1348451536709214||
			physicalCertificate.dimensionlessEigenvalueLower!=0.016975308641975297||
			physicalCertificate.operatorEigenvalueLower!=8.9899266871598034||
			physicalCertificate.inverseOperatorNormUpper!=0.11123561234690459||
			physicalCertificate.residualEvaluationRoundingUpper!=1.3748435749320591e-7||
			physicalCertificate.crossPrecisionResidualUpper!=1.9319781954175433e-6||
			physicalCertificate.fp64ResidualGateUpper!=0.00034336556400244998||
			physicalCertificate.fp64ResidualEvaluationUpper!=1.154938723844404e-14||
			physicalCertificate.fp64TerminalFaceL2PerCellUpper!=5.2704410353592405e-17||
			physicalCertificate.beginningVelocityRoundingUpper!=1.3680506069591611e-08||
			physicalCertificate.fp64FeedbackFactor!=0.23426947265986475||
			physicalCertificate.validationToleranceRoundingUpper!=4.6932956987791455e-11||
			physicalCertificate.validationPredicateMarginLower!=0.00026143109675737545||
			physicalCertificate.streamingFaceVelocityL2PerCellUpper!=9.7212486067771285e-9||
			physicalCertificate.velocityGainUpper!=0.47780222212961759||
			physicalCertificate.velocityRMSUpper!=0.00016499365420669603||
			trace.physicalStreaming.maximumRoundedVelocity!=0.077085278928279877||
			trace.physicalStreaming.maximumRoundedTarget!=0.027697939425706863||
			restorationCertificate.residualEvaluationRoundingUpper!=1.1240225418597112e-6||
			restorationCertificate.crossPrecisionResidualUpper!=2.3628878941959103e-5||
			restorationCertificate.fp64ResidualGateUpper!=0.00046519335364055106||
			restorationCertificate.fp64ResidualEvaluationUpper!=4.7201289803479115e-14||
			restorationCertificate.fp64TerminalFaceL2PerCellUpper!=7.2284267210388352e-17||
			restorationCertificate.beginningVelocityRoundingUpper!=1.3783925480929936e-08||
			restorationCertificate.fp64FeedbackFactor!=1.5955915929254457e-11||
			restorationCertificate.validationToleranceRoundingUpper!=2.7727685625741094e-11||
			restorationCertificate.validationPredicateMarginLower!=0.00044094270347925889||
			restorationCertificate.streamingFaceVelocityL2PerCellUpper!=1.1190657711221316e-8||
			restorationCertificate.velocityRMSUpper!=0.00023357153961206769||
			trace.restorationStreaming.maximumRoundedVelocity!=0.086094409227371216||
			trace.restorationStreaming.maximumRoundedTarget!=0.093038670718669891)return 238;
		static const std::array<double,9> ExpectedScalar={{
			0.00032719950722423746,0.00026175964690034235,0.0013965273494462376,
			0.0046634001371138678,0.00017417247803291333,0.00014697468215851725,
			2.28515847259952e-06,4.8013754918650386e-07,521.81563701838843}};
		static const std::array<double,9> ExpectedScalarRMS={{
			0.0016507572621071557,0.0013206089346941951,0.0078814363904888378,
			0.024610816574923065,0.00087872041765648388,0.00074150164355245502,
			1.152885058045011e-05,2.4223343929965511e-06,8454.3253079109272}};
		static const std::array<double,3> ExpectedMomentum={{0.013505921107054692,
			0.0012696844714689289,0.00011935802483893404}};
		FireProductionRoundoffWalker::FullStepAssumptionRefusal proofRefusal;
		if(fullStepDerived||fullStepCertificate.proofGapBitmap!=0xffu||
			fullStepCertificate.proofComplete||
			!FireProductionRoundoffWalker::RefuteFullStepCandidateAssumptions(proofRefusal)||
			fullStepCertificate.scalarFilteredL1Upper!=ExpectedScalar||
			fullStepCertificate.scalarRMSUpper!=ExpectedScalarRMS||
			fullStepCertificate.inventoryPerVolumeUpper!=ExpectedScalar||
			fullStepCertificate.momentumL2PerCellUpper!=ExpectedMomentum||
			fullStepCertificate.gasDensityRMSUpper!=0.035444612811895516||
			fullStepCertificate.densityRelativeUpper!=0.036372463891938378||
			fullStepCertificate.provisionalVelocityL2PerCellUpper!=0.016724925504348544||
			fullStepCertificate.openBoundaryInteractionUpper!=0.00055034168358969525||
			fullStepCertificate.physicalProjectionVelocityUpper!=0.018948201018992382||
			fullStepCertificate.finalVelocityRMSUpper!=0.021145425678289562||
			trace.physicalStreaming.projectionCorrectionL2PerCell!=0.0050658272508546124||
			trace.restorationStreaming.projectionCorrectionL2PerCell!=0.012757173692842795||
			fullStepCertificate.scalarTransportNonexpansive||
			fullStepCertificate.dualTransportNonexpansive||
			fullStepCertificate.projectionInteractionsIncluded)return 238;
		std::fprintf(stderr,"r136 full-step B_fp32 refused; obligations=%llu/%llu "
			"proof_gaps=0x%02x compression_gain=%.17g product=%.17g/%.17g "
			"cross_component=%.17g candidate_velocity=%.17g metal_measurement=0\n",
			static_cast<unsigned long long>(totalDischargedBranchObligationCount),
			static_cast<unsigned long long>(totalBranchObligationCount),
			fullStepCertificate.proofGapBitmap,
			proofRefusal.conservativeCompressionL2Gain,
			proofRefusal.localizedProductRMS,proofRefusal.productOfRMS,
			proofRefusal.sharedAlphaCrossComponentResponse,
			fullStepCertificate.finalVelocityRMSUpper);
		return 237;
	}

	bool SelectProductionTimeStepForState(const MethaneRunCheckpoint& state,
		const double baseStep,RISE::FireProductionStableTimeStep& selected,
		std::string& error)
	{
		RISE::FireStateProducerPrecision statePrecision=
			RISE::FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(state.states,statePrecision))return false;
		const double previousStep=state.previousStepS;
		RISE::FireProductionAcceptedCheckpointStateView currentAcceptedState;
		RISE::FireProductionProjectionShape acceptedShape;
		std::vector<float> acceptedConservative;
		std::array<std::vector<float>,3> acceptedMomentum,acceptedVelocity;
		std::uint64_t currentAcceptedStateDigest=0u;
		if(state.acceptedSteps>0u){
			if(statePrecision!=RISE::FireStateProducerPrecision::Binary32||
				!AcceptedCheckpointTimelineValid(state)||
				!state.productionManifoldObservation.Available()||
				!BuildCheckpointAcceptedStatePayload(state,acceptedShape,
					acceptedConservative,acceptedMomentum,acceptedVelocity,
					currentAcceptedStateDigest))return false;
			currentAcceptedState.shape=acceptedShape;
			currentAcceptedState.conservativeValues=&acceptedConservative;
			currentAcceptedState.momentum=&acceptedMomentum;
			currentAcceptedState.velocity=&acceptedVelocity;
		} else if(statePrecision!=RISE::FireStateProducerPrecision::Binary64||
			state.simulationTimeS!=0.0||state.previousStepS!=0.0||
			state.lastAcceptedStepS!=0.0||
			!state.values.acceptedTimeStepHistoryS.empty()||
			state.productionManifoldObservation.Available())return false;
		else {
			if(state.dimensions[0u]%BaseDimensions[0u]!=0u)return false;
			const std::size_t tier=state.dimensions[0u]/BaseDimensions[0u];
			if(tier==0u||tier>std::numeric_limits<unsigned int>::max()||
				state.dimensions[1u]!=BaseDimensions[1u]*tier||
				state.dimensions[2u]!=BaseDimensions[2u]*tier)return false;
			MethaneRunCheckpoint canonicalBeginning;
			if(!BuildAnalyticState(static_cast<unsigned int>(tier),canonicalBeginning,error)||
				AnalyticStateDigest(state)!=AnalyticStateDigest(canonicalBeginning))return false;
		}
		const double representedCellWidth=static_cast<double>(static_cast<float>(state.cellWidthM));
		return RISE::SelectFireProductionStableTimeStep(representedCellWidth,
			0.5*representedCellWidth/baseStep,0.0,0.0,previousStep,
			currentAcceptedState,state.productionManifoldObservation,selected,&error);
	}

	int CheckRestorationLong(const std::filesystem::path& directory,
		const std::array<std::string,4>& targetDigests,const bool lifecycleOnly=false)
	{
		const std::size_t StepCount=lifecycleOnly?2u:104u;
		static const std::size_t WarmupCount=8u,PlateauCount=32u;
		static const std::size_t FailingProbeCell=2256u;
		MethaneRunCheckpoint state;std::string error;
		if(!BuildAnalyticState(12u,state,error))return 217;
		std::vector<std::vector<double> > sealed;const std::filesystem::path target=
			directory/"oracle_tier12_sdiv_x8.f64";
		if(DigestFile(target)!=targetDigests[3]||!ReadCalibrationDoublePayload(target,
			state.states.size(),8u,sealed))return 218;
		const double flowThrough=6.0*std::sqrt(
			state.values.characteristicDiameterM/Gravity);
		const double baseStep=flowThrough/512.0;
		std::vector<double> probe(StepCount),fieldMaximum(StepCount),deviceTimes,wallTimes;
		RISECBOR64::Bytes trace;
		std::uint64_t maximumCertified=0u,maximumActual=0u;
		bool acceptedLifecyclePassed=false,authorityMutationREDsPassed=false;
		double selectedAfterResume=0.0,firstGeneration=0.0,firstDrain=0.0;
		std::string selectedAfterResumeLimit;
		const std::filesystem::path lifecycleCheckpoint=
			std::filesystem::temp_directory_path()/"rise_r147_manifold_lifecycle.checkpoint";
		{std::error_code ignored;std::filesystem::remove(lifecycleCheckpoint,ignored);}
		for(std::size_t step=0u;step<StepCount;++step){
			RISE::FireProductionStableTimeStep selected;
			if(!SelectProductionTimeStepForState(state,baseStep,selected,error))return 219;
			if(step==1u){selectedAfterResume=selected.seconds;
				selectedAfterResumeLimit=selected.activeLimit?selected.activeLimit:"";}
			RISE::FireProductionResidentStepRequest request;
			if(!BuildProductionRequest(state,sealed[step%sealed.size()],selected.seconds,
				request,error))return 219;
			RISE::FireProductionResidentStepResult production;
			const std::chrono::steady_clock::time_point beginning=
				std::chrono::steady_clock::now();
			if(!RISE::AdvanceFireProductionResidentStepMetal(request,production,&error)){
				std::fprintf(stderr,"r118 long step=%zu failed: %s\n",step,error.c_str());return 220;}
			const double wall=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-beginning).count();
			if(production.interstageFullGridTransferCount!=0u||
				production.residentProjectionInvocationCount!=2u||
				production.physicalProjection.executedVCycleCount!=17u||
				production.physicalProjection.executedJacobiSweepCount!=
					17u*ExpectedProjectionSweeps(request.force.shape)/16u||
				production.projection.executedVCycleCount!=16u||
				production.projection.executedJacobiSweepCount!=
					ExpectedProjectionSweeps(request.force.shape)||
				!production.physicalProjection.validationPassed||
				!production.projection.validationPassed)return 221;
			ProductionEOSDeviation deviation;
			if(!MeasureProductionEOSDeviation(production,state.states.size(),FailingProbeCell,
				deviation,error))return 222;
			const ProductionAffineResidual affine=MeasureProductionAffineResidual(
				production,state.states.size());
			probe[step]=deviation.signedProbe;fieldMaximum[step]=deviation.signedAtMaximum;
			maximumCertified=std::max(maximumCertified,
				production.combinedCertifiedWorkingSetBytes);
			maximumActual=std::max(maximumActual,production.combinedActualMetalAllocationBytes);
			AppendInteger(trace,step);AppendDouble(trace,deviation.signedProbe);
			AppendDouble(trace,deviation.signedAtMaximum);AppendInteger(trace,deviation.maximumCell);
			auto appendFloat=[&](const float value){std::uint32_t bits=0u;
				std::memcpy(&bits,&value,sizeof(bits));AppendInteger(trace,bits);};
			appendFloat(production.physicalProjection.maximumPreProjectionResidualPerS);
			appendFloat(production.physicalProjection.maximumPostProjectionResidualPerS);
			appendFloat(production.projection.maximumPreProjectionResidualPerS);
			appendFloat(production.projection.maximumPostProjectionResidualPerS);
			AppendInteger(trace,production.physicalProjection.validationPassed?1u:0u);
			AppendInteger(trace,production.projection.validationPassed?1u:0u);
			AppendInteger(trace,production.interstageFullGridTransferCount);
			AppendInteger(trace,production.residentProjectionInvocationCount);
			AppendDouble(trace,affine.maximumScaled);AppendInteger(trace,affine.cell);
			AppendInteger(trace,affine.row);
			const std::string payload=ProductionConservativeDigest(production);
			trace.insert(trace.end(),payload.begin(),payload.end());
			if(step>=WarmupCount){deviceTimes.push_back(production.deviceElapsedMS);
				wallTimes.push_back(wall);}
			if(step==0u){
				const double represented=static_cast<double>(production.representedTimeStepS);
				RISE::FireProductionResidentStepResult physicalValidationMiss;
				setenv("RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE","1",1);
				const bool physicalValidationProbeRan=
					RISE::AdvanceFireProductionResidentStepMetal(request,physicalValidationMiss,&error);
				unsetenv("RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE");
				RISE::FireProductionAcceptedManifoldObservation invalidPhysicalObservation;
				const bool physicalValidationOwnerRED=physicalValidationProbeRan&&
					!physicalValidationMiss.physicalProjection.validationPassed&&
					physicalValidationMiss.projection.validationPassed&&
					physicalValidationMiss.manifoldPlateauPassed&&
					physicalValidationMiss.residentProjectionInvocationCount==2u&&
					physicalValidationMiss.interstageFullGridTransferCount==0u&&
					!physicalValidationMiss.HasAcceptedManifoldToken()&&
					physicalValidationMiss.representedTimeStepS==production.representedTimeStepS&&
					physicalValidationMiss.conservativeProducerPrecision==
						production.conservativeProducerPrecision&&
					physicalValidationMiss.maximumManifoldGeneration==
						production.maximumManifoldGeneration&&
					physicalValidationMiss.maximumAcceptedManifoldDeviation==
						production.maximumAcceptedManifoldDeviation&&
					physicalValidationMiss.requiredRestorationDrainFraction==
						production.requiredRestorationDrainFraction&&
					physicalValidationMiss.deliveredRestorationDrainFraction==
						production.deliveredRestorationDrainFraction&&
					physicalValidationMiss.restorationResidualBandPerS==
						production.restorationResidualBandPerS&&
					physicalValidationMiss.physicalProjection.maximumPreProjectionResidualPerS==
						production.physicalProjection.maximumPreProjectionResidualPerS&&
					physicalValidationMiss.physicalProjection.maximumPostProjectionResidualPerS==
						production.physicalProjection.maximumPostProjectionResidualPerS&&
					physicalValidationMiss.projection.maximumPreProjectionResidualPerS==
						production.projection.maximumPreProjectionResidualPerS&&
					physicalValidationMiss.projection.maximumPostProjectionResidualPerS==
						production.projection.maximumPostProjectionResidualPerS&&
					RISE::FireProductionAcceptedManifoldPayloadDigest(physicalValidationMiss)==
						RISE::FireProductionAcceptedManifoldPayloadDigest(production)&&
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,
						physicalValidationMiss,invalidPhysicalObservation,&error)&&
					!invalidPhysicalObservation.Available();
				RISE::FireProductionResidentStepResult copied=production;
				RISE::FireProductionAcceptedManifoldObservation rejected;
				const bool copyClearsToken=!copied.HasAcceptedManifoldToken();
				const bool wrongStepRejected=!RISE::PublishFireProductionAcceptedManifoldObservation(
					std::nextafter(represented,std::numeric_limits<double>::infinity()),production,
					rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				const float savedPayload=production.conservativeValues.front();
				production.conservativeValues.front()=
					std::nextafter(savedPayload,std::numeric_limits<float>::infinity());
				const bool payloadMutationRejected=
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.conservativeValues.front()=savedPayload;
				const float movedBoundaryValue=production.conservativeValues.back();
				production.conservativeValues.pop_back();
				production.transportedDual.auxiliaryFaceDensity[0].insert(
					production.transportedDual.auxiliaryFaceDensity[0].begin(),movedBoundaryValue);
				const bool vectorBoundaryMutationRejected=
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.transportedDual.auxiliaryFaceDensity[0].erase(
					production.transportedDual.auxiliaryFaceDensity[0].begin());
				production.conservativeValues.push_back(movedBoundaryValue);
				const double savedGeneration=production.maximumManifoldGeneration;
				const double savedRequired=production.requiredRestorationDrainFraction;
				const double savedDelivered=production.deliveredRestorationDrainFraction;
				const double savedBand=production.restorationResidualBandPerS;
				production.maximumManifoldGeneration*=0.5;
				RISE::FireProductionRestorationPlateauValidation coherent;
				const bool coherentDerived=RISE::FireProductionRestorationPlateauWithinBand(
					production.maximumManifoldGeneration,
					production.projection.maximumPreProjectionResidualPerS,
					production.projection.maximumPostProjectionResidualPerS,coherent);
				production.requiredRestorationDrainFraction=coherent.requiredDrainFraction;
				production.deliveredRestorationDrainFraction=coherent.deliveredDrainFraction;
				production.restorationResidualBandPerS=coherent.maximumPostResidualPerS;
				const bool coherentDiagnosticForgeryRejected=coherentDerived&&
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.maximumManifoldGeneration=savedGeneration;
				production.requiredRestorationDrainFraction=savedRequired;
				production.deliveredRestorationDrainFraction=savedDelivered;
				production.restorationResidualBandPerS=savedBand;
				const double savedDeviation=production.maximumAcceptedManifoldDeviation;
				production.maximumAcceptedManifoldDeviation=0.0;
				const bool deviationMutationRejected=
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.maximumAcceptedManifoldDeviation=savedDeviation;
				const bool savedPhysicalValidation=production.physicalProjection.validationPassed;
				production.physicalProjection.validationPassed=false;
				const bool physicalValidationMutationRejected=
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.physicalProjection.validationPassed=savedPhysicalValidation;
				const float savedPhysicalResidual=
					production.physicalProjection.maximumPostProjectionResidualPerS;
				production.physicalProjection.maximumPostProjectionResidualPerS=
					std::nextafter(savedPhysicalResidual,std::numeric_limits<float>::infinity());
				const bool physicalResidualMutationRejected=
					!RISE::PublishFireProductionAcceptedManifoldObservation(represented,production,
						rejected,&error)&&!rejected.Available()&&production.HasAcceptedManifoldToken();
				production.physicalProjection.maximumPostProjectionResidualPerS=savedPhysicalResidual;
				authorityMutationREDsPassed=physicalValidationOwnerRED&&copyClearsToken&&wrongStepRejected&&
					payloadMutationRejected&&vectorBoundaryMutationRejected&&
					coherentDiagnosticForgeryRejected&&
					deviationMutationRejected&&physicalValidationMutationRejected&&
					physicalResidualMutationRejected;
				if(!authorityMutationREDsPassed){std::fprintf(stderr,
					"r147 authority RED failed physical=%d copy=%d step=%d payload=%d boundary=%d coherent=%d deviation=%d validation=%d residual=%d error=%s\n",
					physicalValidationOwnerRED?1:0,copyClearsToken?1:0,wrongStepRejected?1:0,
					payloadMutationRejected?1:0,vectorBoundaryMutationRejected?1:0,
					coherentDiagnosticForgeryRejected?1:0,deviationMutationRejected?1:0,
					physicalValidationMutationRejected?1:0,physicalResidualMutationRejected?1:0,
					error.c_str());return 223;}
			}
			RISE::FireProductionAcceptedManifoldObservation acceptedObservation;
			if(!RISE::PublishFireProductionAcceptedManifoldObservation(
				static_cast<double>(request.force.timeStepS),production,
				acceptedObservation,&error)){std::fprintf(stderr,
					"r147 accepted observation publication failed: %s\n",error.c_str());return 223;}
			if(step==0u){
				RISE::FireProductionAcceptedManifoldObservation replayed;
				const bool replayRejected=!RISE::PublishFireProductionAcceptedManifoldObservation(
					static_cast<double>(request.force.timeStepS),production,replayed,&error)&&
					!replayed.Available()&&!production.HasAcceptedManifoldToken();
				authorityMutationREDsPassed=authorityMutationREDsPassed&&replayRejected;
				float& terminalVelocity=production.projection.velocityMPerS[0].front();
				const float savedTerminalVelocity=terminalVelocity;
				terminalVelocity=std::nextafter(terminalVelocity,
					std::numeric_limits<float>::infinity());
				const std::string stateBeforeRejectedApply=AnalyticStateDigest(state);
				const bool postPublicationMutationRejected=
					!ApplyAcceptedProductionResult(production,acceptedObservation,state,error)&&
					AnalyticStateDigest(state)==stateBeforeRejectedApply;
				terminalVelocity=savedTerminalVelocity;
				authorityMutationREDsPassed=authorityMutationREDsPassed&&
					postPublicationMutationRejected&&
					acceptedObservation.MatchesAcceptedResidentPayload(production);
				if(!authorityMutationREDsPassed){std::fprintf(stderr,
					"r147 post-publication authority RED failed: %s\n",error.c_str());return 223;}
			}
			if(step==0u){firstGeneration=acceptedObservation.MaximumGeneration();
				firstDrain=acceptedObservation.RestorationDrainFraction();}
			if(!ApplyAcceptedProductionResult(production,acceptedObservation,state,error)){
				std::fprintf(stderr,"r118 long consumer step=%zu failed: %s\n",step,error.c_str());
				return 223;}
			const double representedStep=static_cast<double>(production.representedTimeStepS);
			state.simulationTimeS+=representedStep;
			state.previousStepS=representedStep;
			state.lastAcceptedStepS=representedStep;
			++state.acceptedSteps;
			state.values.acceptedTimeStepHistoryS.push_back(representedStep);
			state.productionManifoldObservation=acceptedObservation;
			if(step==0u){
				MethaneRunCheckpoint lostObservation=state;
				lostObservation.productionManifoldObservation=
					RISE::FireProductionAcceptedManifoldObservation();
				RISE::FireProductionStableTimeStep lostObservationSelection;
				const bool lostObservationRejected=!SelectProductionTimeStepForState(
					lostObservation,baseStep,lostObservationSelection,error);
				MethaneRunCheckpoint coordinatedClear=state;
				coordinatedClear.productionManifoldObservation=
					RISE::FireProductionAcceptedManifoldObservation();
				coordinatedClear.acceptedSteps=0u;coordinatedClear.simulationTimeS=0.0;
				coordinatedClear.previousStepS=0.0;coordinatedClear.lastAcceptedStepS=0.0;
				coordinatedClear.values.acceptedTimeStepHistoryS.clear();
				const std::filesystem::path coordinatedClearCheckpoint=
					std::filesystem::temp_directory_path()/"rise_r148_coordinated_clear.checkpoint";
				{std::error_code ignored;
					std::filesystem::remove(coordinatedClearCheckpoint,ignored);}
				const bool coordinatedClearRejected=!SaveMethaneRunCheckpoint(
					coordinatedClearCheckpoint,coordinatedClear,error)&&
					!std::filesystem::exists(coordinatedClearCheckpoint);
				RISE::FireProductionStableTimeStep coordinatedClearSelection;
				const bool coordinatedClearOwnerRejected=!SelectProductionTimeStepForState(
					coordinatedClear,baseStep,coordinatedClearSelection,error);
				forceMalformedManifoldLifecycleWriteForTest=true;
				const bool coordinatedClearWritten=SaveMethaneRunCheckpoint(
					coordinatedClearCheckpoint,coordinatedClear,error);
				forceMalformedManifoldLifecycleWriteForTest=false;
				MethaneRunCheckpoint loadedCoordinatedClear;
				const bool coordinatedClearLoaderRejected=coordinatedClearWritten&&
					!LoadMethaneRunCheckpoint(coordinatedClearCheckpoint,
						loadedCoordinatedClear,error);
				{std::error_code ignored;
					std::filesystem::remove(coordinatedClearCheckpoint,ignored);}
				MethaneRunCheckpoint retaggedClear=coordinatedClear;
				for(MethaneCellState& cell:retaggedClear.states)
					cell.producerPrecision=RISE::FireStateProducerPrecision::Binary64;
				RISE::FireProductionStableTimeStep retaggedClearSelection;
				const bool retaggedClearOwnerRejected=!SelectProductionTimeStepForState(
					retaggedClear,baseStep,retaggedClearSelection,error);
				const bool retaggedClearWriterRejected=!SaveMethaneRunCheckpoint(
					coordinatedClearCheckpoint,retaggedClear,error)&&
					!std::filesystem::exists(coordinatedClearCheckpoint);
				forceMalformedManifoldLifecycleWriteForTest=true;
				const bool retaggedClearWritten=SaveMethaneRunCheckpoint(
					coordinatedClearCheckpoint,retaggedClear,error);
				forceMalformedManifoldLifecycleWriteForTest=false;
				MethaneRunCheckpoint loadedRetaggedClear;
				const bool retaggedClearLoaderRejected=retaggedClearWritten&&
					!LoadMethaneRunCheckpoint(coordinatedClearCheckpoint,
						loadedRetaggedClear,error);
				{std::error_code ignored;
					std::filesystem::remove(coordinatedClearCheckpoint,ignored);}
				MethaneRunCheckpoint intactRetagged=state;
				for(MethaneCellState& cell:intactRetagged.states)
					cell.producerPrecision=RISE::FireStateProducerPrecision::Binary64;
				RISE::FireProductionStableTimeStep intactRetaggedSelection;
				const bool intactRetaggedOwnerRejected=!SelectProductionTimeStepForState(
					intactRetagged,baseStep,intactRetaggedSelection,error);
				MethaneRunCheckpoint lastStepAlias=state;
				lastStepAlias.lastAcceptedStepS=std::nextafter(lastStepAlias.lastAcceptedStepS,
					std::numeric_limits<double>::infinity());
				RISE::FireProductionStableTimeStep lastStepAliasSelection;
				const bool lastStepOwnerRejected=!SelectProductionTimeStepForState(
					lastStepAlias,baseStep,lastStepAliasSelection,error);
				const std::filesystem::path lastStepAliasCheckpoint=
					std::filesystem::temp_directory_path()/"rise_r148_last_step_alias.checkpoint";
				{std::error_code ignored;std::filesystem::remove(lastStepAliasCheckpoint,ignored);}
				const bool lastStepWriterRejected=!SaveMethaneRunCheckpoint(
					lastStepAliasCheckpoint,lastStepAlias,error)&&
					!std::filesystem::exists(lastStepAliasCheckpoint);
				forceMalformedManifoldLifecycleWriteForTest=true;
				const bool lastStepAliasWritten=SaveMethaneRunCheckpoint(
					lastStepAliasCheckpoint,lastStepAlias,error);
				forceMalformedManifoldLifecycleWriteForTest=false;
				MethaneRunCheckpoint loadedLastStepAlias;
				const bool lastStepLoaderRejected=lastStepAliasWritten&&
					!LoadMethaneRunCheckpoint(lastStepAliasCheckpoint,
						loadedLastStepAlias,error);
				{std::error_code ignored;std::filesystem::remove(lastStepAliasCheckpoint,ignored);}
				authorityMutationREDsPassed=authorityMutationREDsPassed&&
					lostObservationRejected&&coordinatedClearRejected&&
					coordinatedClearOwnerRejected&&coordinatedClearLoaderRejected&&
					retaggedClearOwnerRejected&&retaggedClearWriterRejected&&
					retaggedClearLoaderRejected&&intactRetaggedOwnerRejected&&
					lastStepOwnerRejected&&lastStepWriterRejected&&lastStepLoaderRejected;
				if(!lostObservationRejected)std::fprintf(stderr,
					"r148 lost accepted observation aliased to a first step\n");
				if(!(coordinatedClearRejected&&coordinatedClearOwnerRejected&&
					coordinatedClearLoaderRejected&&retaggedClearOwnerRejected&&
					retaggedClearWriterRejected&&retaggedClearLoaderRejected))std::fprintf(stderr,
					"r148 coordinated accepted metadata clear aliased to a first step\n");
				if(!(intactRetaggedOwnerRejected&&lastStepOwnerRejected&&
					lastStepWriterRejected&&lastStepLoaderRejected))std::fprintf(stderr,
					"r148 intact precision retag or last-step lifecycle alias was admitted\n");
				const std::string stateDigestBefore=AnalyticStateDigest(state);
				MethaneRunCheckpoint transplanted=state;
				MethaneRunCheckpoint temperatureTransplanted=state;
				MethaneRunCheckpoint widthTransplanted=state;
				const std::filesystem::path transplantedCheckpoint=
					std::filesystem::temp_directory_path()/"rise_r148_transplanted_authority.checkpoint";
				const std::filesystem::path malformedLifecycleCheckpoint=
					std::filesystem::temp_directory_path()/"rise_r148_malformed_lifecycle.checkpoint";
				{std::error_code ignored;std::filesystem::remove(transplantedCheckpoint,ignored);}
				{std::error_code ignored;std::filesystem::remove(malformedLifecycleCheckpoint,ignored);}
				bool transplantRejected=false,temperatureTransplantRejected=false;
				bool widthNormalizationBound=false;
				if(!transplanted.velocity.component[0].empty()){
					const float original=static_cast<float>(transplanted.velocity.component[0].front());
					transplanted.velocity.component[0].front()=static_cast<double>(
						std::nextafter(original,std::numeric_limits<float>::infinity()));
					std::string transplantError;
					transplantRejected=!SaveMethaneRunCheckpoint(transplantedCheckpoint,
						transplanted,transplantError)&&!std::filesystem::exists(transplantedCheckpoint);
				}
				if(!temperatureTransplanted.states.empty()){
					temperatureTransplanted.states.front().temperatureK=std::nextafter(
						temperatureTransplanted.states.front().temperatureK,
						std::numeric_limits<double>::infinity());
					std::string transplantError;
					suppressExpectedCheckpointStateDiagnostic=true;
					temperatureTransplantRejected=!SaveMethaneRunCheckpoint(transplantedCheckpoint,
						temperatureTransplanted,transplantError)&&
						!std::filesystem::exists(transplantedCheckpoint);
					suppressExpectedCheckpointStateDiagnostic=false;
				}
				widthTransplanted.cellWidthM=std::nextafter(widthTransplanted.cellWidthM,
					std::numeric_limits<double>::infinity());
				{std::uint64_t originalDigest=0u,transplantedDigest=0u;
					widthNormalizationBound=CheckpointAcceptedStateDigest(state,originalDigest)&&
						CheckpointAcceptedStateDigest(widthTransplanted,transplantedDigest)&&
						originalDigest==transplantedDigest&&
						static_cast<float>(state.cellWidthM)==
							static_cast<float>(widthTransplanted.cellWidthM);}
				MethaneRunCheckpoint malformedLifecycle=state;
				++malformedLifecycle.acceptedSteps;
				forceMalformedManifoldLifecycleWriteForTest=true;
				const bool malformedLifecycleWritten=SaveMethaneRunCheckpoint(
					malformedLifecycleCheckpoint,malformedLifecycle,error);
				forceMalformedManifoldLifecycleWriteForTest=false;
				MethaneRunCheckpoint malformedLifecycleLoaded;
				const bool malformedLifecycleLoadRejected=malformedLifecycleWritten&&
					!LoadMethaneRunCheckpoint(malformedLifecycleCheckpoint,
						malformedLifecycleLoaded,error);
				{std::error_code ignored;std::filesystem::remove(malformedLifecycleCheckpoint,ignored);}
				MethaneRunCheckpoint loaded;
				const bool saved=SaveMethaneRunCheckpoint(lifecycleCheckpoint,state,error);
				const bool loadedOK=saved&&LoadMethaneRunCheckpoint(lifecycleCheckpoint,loaded,error);
				{std::error_code ignored;std::filesystem::remove(lifecycleCheckpoint,ignored);}
				RISE::FireProductionStableTimeStep transplantedSelection;
				bool selectorTransplantRejected=false;
				if(loadedOK&&!loaded.velocity.component[0].empty()){
					MethaneRunCheckpoint alternateState=loaded;
					const float original=static_cast<float>(alternateState.velocity.component[0].front());
					alternateState.velocity.component[0].front()=static_cast<double>(
						std::nextafter(original,std::numeric_limits<float>::infinity()));
					RISE::FireProductionProjectionShape alternateShape;
					std::vector<float> alternateConservative;
					std::array<std::vector<float>,3> alternateMomentum,alternateVelocity;
					std::uint64_t alternateStateDigest=0u;
					RISE::FireProductionAcceptedCheckpointStateView alternateView;
					selectorTransplantRejected=BuildCheckpointAcceptedStatePayload(
						alternateState,alternateShape,alternateConservative,
						alternateMomentum,alternateVelocity,alternateStateDigest);
					alternateView.shape=alternateShape;
					alternateView.conservativeValues=&alternateConservative;
					alternateView.momentum=&alternateMomentum;
					alternateView.velocity=&alternateVelocity;
					selectorTransplantRejected=selectorTransplantRejected&&
						!RISE::SelectFireProductionStableTimeStep(
							static_cast<double>(static_cast<float>(alternateState.cellWidthM)),
							0.5*static_cast<double>(static_cast<float>(alternateState.cellWidthM))/baseStep,
							0.0,0.0,
							alternateState.previousStepS,alternateView,
							loaded.productionManifoldObservation,transplantedSelection,&error);
				}
				acceptedLifecyclePassed=transplantRejected&&temperatureTransplantRejected&&
					widthNormalizationBound&&malformedLifecycleLoadRejected&&
					selectorTransplantRejected&&loadedOK&&
					loaded.checkpointFormatVersion==13u&&
					loaded.acceptedSteps==state.acceptedSteps&&
					loaded.simulationTimeS==state.simulationTimeS&&
					loaded.previousStepS==representedStep&&
					loaded.lastAcceptedStepS==representedStep&&
					loaded.productionManifoldObservation.Available()&&
					loaded.productionManifoldObservation.TimeStepS()==representedStep&&
					loaded.productionManifoldObservation.MaximumGeneration()==
						acceptedObservation.MaximumGeneration()&&
					loaded.productionManifoldObservation.RestorationDrainFraction()==
						acceptedObservation.RestorationDrainFraction()&&
					AnalyticStateDigest(loaded)==stateDigestBefore;
				if(!acceptedLifecyclePassed){std::fprintf(stderr,
					"r147 checkpoint lifecycle failed saved=%d loaded=%d version=%llu "
					"steps=%llu history=%zu time=%.17g previous=%.17g last=%.17g canonical=%d error=%s\n",
					saved?1:0,loadedOK?1:0,static_cast<unsigned long long>(loaded.checkpointFormatVersion),
					static_cast<unsigned long long>(state.acceptedSteps),
					state.values.acceptedTimeStepHistoryS.size(),state.simulationTimeS,
					state.previousStepS,state.lastAcceptedStepS,
					[&](){std::uint64_t value=0u;return CheckpointAcceptedStateDigest(state,value)?1:0;}(),
					error.c_str());return 223;}
				state=std::move(loaded);
			}
		}
		if(lifecycleOnly){
			const std::filesystem::path historyAliasCheckpoint=
				std::filesystem::temp_directory_path()/"rise_r148_history_alias.checkpoint";
			const std::filesystem::path timeAliasCheckpoint=
				std::filesystem::temp_directory_path()/"rise_r148_time_alias.checkpoint";
			{std::error_code ignored;std::filesystem::remove(historyAliasCheckpoint,ignored);
				std::filesystem::remove(timeAliasCheckpoint,ignored);}
			MethaneRunCheckpoint historyAlias=state,timeAlias=state;
			bool historyWriterRejected=false,historyLoaderRejected=false;
			bool historyOwnerRejected=false,timeOwnerRejected=false;
			bool timeWriterRejected=false,timeLoaderRejected=false;
			if(historyAlias.values.acceptedTimeStepHistoryS.size()>=2u){
				historyAlias.values.acceptedTimeStepHistoryS.front()*=0.5;
				RISE::FireProductionStableTimeStep ignoredSelection;
				historyOwnerRejected=!SelectProductionTimeStepForState(
					historyAlias,baseStep,ignoredSelection,error);
				historyWriterRejected=!SaveMethaneRunCheckpoint(
					historyAliasCheckpoint,historyAlias,error);
				forceMalformedManifoldLifecycleWriteForTest=true;
				const bool written=SaveMethaneRunCheckpoint(
					historyAliasCheckpoint,historyAlias,error);
				forceMalformedManifoldLifecycleWriteForTest=false;
				MethaneRunCheckpoint loadedAlias;
				historyLoaderRejected=written&&!LoadMethaneRunCheckpoint(
					historyAliasCheckpoint,loadedAlias,error);
			}
			timeAlias.simulationTimeS=std::nextafter(timeAlias.simulationTimeS,
				std::numeric_limits<double>::infinity());
			{RISE::FireProductionStableTimeStep ignoredSelection;
				timeOwnerRejected=!SelectProductionTimeStepForState(
					timeAlias,baseStep,ignoredSelection,error);}
			timeWriterRejected=!SaveMethaneRunCheckpoint(timeAliasCheckpoint,timeAlias,error);
			forceMalformedManifoldLifecycleWriteForTest=true;
			const bool timeAliasWritten=SaveMethaneRunCheckpoint(
				timeAliasCheckpoint,timeAlias,error);
			forceMalformedManifoldLifecycleWriteForTest=false;
			MethaneRunCheckpoint loadedTimeAlias;
			timeLoaderRejected=timeAliasWritten&&!LoadMethaneRunCheckpoint(
				timeAliasCheckpoint,loadedTimeAlias,error);
			{std::error_code ignored;std::filesystem::remove(historyAliasCheckpoint,ignored);
				std::filesystem::remove(timeAliasCheckpoint,ignored);}
			authorityMutationREDsPassed=authorityMutationREDsPassed&&
				historyOwnerRejected&&historyWriterRejected&&historyLoaderRejected&&
				timeOwnerRejected&&timeWriterRejected&&timeLoaderRejected;
			if(!(historyOwnerRejected&&historyWriterRejected&&historyLoaderRejected&&
				timeOwnerRejected&&timeWriterRejected&&timeLoaderRejected)){
				double historyAliasSum=0.0;
				for(const double dt:historyAlias.values.acceptedTimeStepHistoryS)historyAliasSum+=dt;
				RISE::FireStateProducerPrecision aliasPrecision=
					RISE::FireStateProducerPrecision::Unknown;
				const bool homogeneous=HomogeneousStateProducerPrecision(
					historyAlias.states,aliasPrecision);
				std::fprintf(stderr,
					"r148 lifecycle alias RED failed history=%d/%d/%d time=%d/%d/%d count=%zu/%llu "
				"sum=%.17g time=%.17g homogeneous=%d precision=%u error=%s\n",
					historyOwnerRejected?1:0,historyWriterRejected?1:0,
					historyLoaderRejected?1:0,timeOwnerRejected?1:0,
					timeWriterRejected?1:0,timeLoaderRejected?1:0,
				historyAlias.values.acceptedTimeStepHistoryS.size(),
				static_cast<unsigned long long>(historyAlias.acceptedSteps),historyAliasSum,
				historyAlias.simulationTimeS,homogeneous?1:0,
				static_cast<unsigned int>(aliasPrecision),error.c_str());}
			std::fprintf(stderr,"r148 accepted lifecycle checkpoint=%d authority_reds=%d steps=%llu "
				"first_G=%.17g first_r=%.17g selected_after_resume=%.17g limit=%s\n",
				acceptedLifecyclePassed?1:0,authorityMutationREDsPassed?1:0,
				static_cast<unsigned long long>(state.acceptedSteps),firstGeneration,firstDrain,selectedAfterResume,
				selectedAfterResumeLimit.c_str());
			return acceptedLifecyclePassed&&authorityMutationREDsPassed&&state.acceptedSteps==2u&&
				state.productionManifoldObservation.Available()&&
				firstGeneration==0.00012048172357026488&&
				firstDrain==0.99562928290235475&&
				selectedAfterResume==0.0018513042677754071&&
				selectedAfterResumeLimit=="advective_CFL"?255:223;
		}
		double plateau=0.0,fieldPlateau=0.0;
		for(std::size_t step=StepCount-PlateauCount;step<StepCount;++step){
			plateau=std::max(plateau,std::fabs(probe[step]));
			fieldPlateau=std::max(fieldPlateau,std::fabs(fieldMaximum[step]));}
		const double deviceP95=Percentile95(deviceTimes),wallP95=Percentile95(wallTimes);
		const std::string traceDigest=RISECBOR64::SHA256Hex(trace);
		std::fprintf(stderr,"r118 long steps=%zu plateau=%.17g field_plateau=%.17g "
			"last_probe=%.17g last_field=%.17g device_p95_ms=%.17g wall_p95_ms=%.17g "
			"certified=%llu actual=%llu trace=%s final=%s\n",StepCount,plateau,fieldPlateau,
			probe.back(),fieldMaximum.back(),deviceP95,wallP95,
			static_cast<unsigned long long>(maximumCertified),
			static_cast<unsigned long long>(maximumActual),traceDigest.c_str(),
			AnalyticStateDigest(state).c_str());
			if(plateau!=0.00015439012582030287||
				fieldPlateau!=0.00065237316812827295||
				probe.back()!=0.00011211235847818912||
				fieldMaximum.back()!=-0.00040622240705245893||
			maximumCertified!=248479780u||maximumActual!=229518420u||
				traceDigest!="719ee45e254a65dc7b6a37ece81720d27213cd69d761312348d102a78f5f68cb"||
				AnalyticStateDigest(state)!=
					"d9a1a0ea021f38792ff9fa3c1446c239ab66d3981f4154e519e568d0030c9dc2"||
			!acceptedLifecyclePassed||!(plateau<0.001)||!(fieldPlateau<0.001)||
			!std::isfinite(deviceP95)||!std::isfinite(wallP95)||wallP95>200.0)return 224;
		return 228;
	}

	int CheckProduction(const std::filesystem::path& directory,const char* expectedProtocol,
		const char* expectedTargets)
	{
		const char* eosProbeEnvironment=std::getenv("RISE_FIRE_EOS_DRIFT_PROBE");
		if(eosProbeEnvironment&&std::strcmp(eosProbeEnvironment,"1")!=0)return 216;
		const bool eosProbe=eosProbeEnvironment&&std::strcmp(eosProbeEnvironment,"1")==0;
		const char* restorationProbeEnvironment=std::getenv("RISE_FIRE_EOS_RESTORATION_PROBE");
		if(restorationProbeEnvironment&&std::strcmp(restorationProbeEnvironment,"1")!=0)return 226;
		const bool restorationProbe=restorationProbeEnvironment&&
			std::strcmp(restorationProbeEnvironment,"1")==0;
		const char* lifecycleEnvironment=std::getenv("RISE_FIRE_MANIFOLD_LIFECYCLE_PROBE");
		if(lifecycleEnvironment&&std::strcmp(lifecycleEnvironment,"1")!=0)return 256;
		const bool lifecycleProbe=lifecycleEnvironment&&
			std::strcmp(lifecycleEnvironment,"1")==0;
		if((eosProbe?1:0)+(restorationProbe?1:0)+(lifecycleProbe?1:0)>1)return 227;
		if(!expectedProtocol||!expectedTargets||std::strlen(expectedProtocol)!=64u||
			std::strlen(expectedTargets)!=64u||DigestFile(directory/"dyadic_protocol.v1")!=
			expectedProtocol||DigestFile(directory/"dyadic_targets.v1")!=expectedTargets)return 180;
		std::array<std::string,4> targetDigests;if(!ReadTargetDigests(
			directory/"dyadic_targets.v1",targetDigests))return 181;
		if(restorationProbe||lifecycleProbe)
			return CheckRestorationLong(directory,targetDigests,lifecycleProbe);
		if(eosProbe)setenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST","removed",1);
		std::array<MethaneRunCheckpoint,4> states;std::array<FilteredField,4> filtered;
		std::array<FilteredVelocityField,4> productionVelocity;
		std::array<std::array<double,9>,4> productionInventory;
		std::string error;
		for(std::size_t index=0u;index<Tiers.size();++index){
			if(!BuildAnalyticState(Tiers[index],states[index],error))return 182;
			std::vector<std::vector<double> > sealed;const std::filesystem::path target=
				directory/(std::string("oracle_tier")+std::to_string(Tiers[index])+"_sdiv_x8.f64");
			if(DigestFile(target)!=targetDigests[index]||!ReadCalibrationDoublePayload(target,
				states[index].states.size(),8u,sealed))return 183;
			const double flowThrough=6.0*std::sqrt(states[index].values.characteristicDiameterM/Gravity);
			static const std::size_t failingProbeCell=2256u;
			MethaneRunCheckpoint restoredProbeState;
			MethaneRunCheckpoint restoredTrajectoryState=states[index];
			bool restoredProbeReady=false;
			bool restoredProjectionValid=false;
			bool drainedProjectionValid=false;
			double restorationReferenceDeviation=0.0;
			std::array<double,8> observedBaselineProbe={{}};
			std::array<double,8> observedBaselineMaximum={{}};
			std::array<std::size_t,8> observedBaselineMaximumCell={{}};
			std::array<double,8> observedRestoredProbe={{}};
			std::array<double,8> observedRestoredMaximum={{}};
			std::array<std::size_t,8> observedRestoredMaximumCell={{}};
			std::array<double,8> observedRestoredPostResidual={{}};
			std::array<unsigned int,8> observedRestoredValid={{}};
			double observedGeneration=0.0,observedDrainFraction=0.0;
			double observedPredictedPlateau=0.0;
			float observedOneOffRestoredPreResidual=0.0f;
			float observedOneOffRestoredPostResidual=0.0f;
			float observedOneOffDrainedPreResidual=0.0f;
			float observedOneOffDrainedPostResidual=0.0f;
			for(std::size_t step=0u;step<8u;++step){
				ProductionEOSDeviation beginningDeviation;
				std::vector<double> beginningDeviationField;
				if(eosProbe&&index==3u&&!MeasureCheckpointEOSDeviation(states[index],
					failingProbeCell,beginningDeviation,&beginningDeviationField,error))return 200;
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
				const bool expectedTopology=eosProbe?
					(production.residentProjectionInvocationCount==1u&&
					production.projection.executedVCycleCount==16u&&
					production.projection.executedJacobiSweepCount==
						ExpectedProjectionSweeps(request.force.shape)):
					(production.residentProjectionInvocationCount==2u&&
					production.physicalProjection.executedVCycleCount==17u&&
					production.physicalProjection.executedJacobiSweepCount==
						(17u*ExpectedProjectionSweeps(request.force.shape)/16u)&&
					production.projection.executedVCycleCount==16u&&
					production.projection.executedJacobiSweepCount==
						ExpectedProjectionSweeps(request.force.shape));
				if(production.interstageFullGridTransferCount!=0u||!expectedTopology){
					std::fprintf(stderr,"dyadic production topology tier=%u step=%zu transfers=%u "
						"projection=%u cycles=%u sweeps=%llu expected=%llu\n",Tiers[index],step,
						production.interstageFullGridTransferCount,
						production.residentProjectionInvocationCount,
						production.projection.executedVCycleCount,
						static_cast<unsigned long long>(
							production.projection.executedJacobiSweepCount),
						static_cast<unsigned long long>(ExpectedProjectionSweeps(
							request.force.shape)));return 186;}
				ProductionEOSDeviation outputDeviation;
				if(eosProbe&&index==3u){
					if(!MeasureProductionEOSDeviation(production,states[index].states.size(),
						failingProbeCell,outputDeviation,error))return 201;
					observedBaselineProbe[step]=outputDeviation.signedProbe;
					observedBaselineMaximum[step]=outputDeviation.signedAtMaximum;
					observedBaselineMaximumCell[step]=outputDeviation.maximumCell;
					std::fprintf(stderr,"EOSPROBE step=%zu begin_probe=%.17g output_probe=%.17g "
						"begin_max=%.17g begin_max_cell=%zu output_max=%.17g output_max_cell=%zu\n",
						step+1u,beginningDeviation.signedProbe,outputDeviation.signedProbe,
						beginningDeviation.signedAtMaximum,beginningDeviation.maximumCell,
						outputDeviation.signedAtMaximum,outputDeviation.maximumCell);
					ProductionEOSDeviation restoredBeginning;
					std::vector<double> restoredBeginningField;
					if(!MeasureCheckpointEOSDeviation(restoredTrajectoryState,failingProbeCell,
						restoredBeginning,&restoredBeginningField,error))return 209;
					RISE::FireProductionResidentStepRequest restoredTrajectoryRequest;
					if(!BuildProductionRequest(restoredTrajectoryState,sealed[step],flowThrough/512.0,
						restoredTrajectoryRequest,error))return 210;
					for(std::size_t cell=0u;cell<restoredBeginningField.size();++cell){
						const float absoluteReference=static_cast<float>(restoredBeginningField[cell]/
							static_cast<double>(restoredTrajectoryRequest.force.timeStepS));
						restoredTrajectoryRequest.divergenceTargetPerS[cell]=
							restoredTrajectoryRequest.divergenceTargetPerS[cell]+absoluteReference;
					}
					RISE::FireProductionResidentStepResult restoredTrajectory;
					if(!RISE::AdvanceFireProductionResidentStepMetal(restoredTrajectoryRequest,
						restoredTrajectory,&error)||restoredTrajectory.interstageFullGridTransferCount!=0u)
						return 211;
					ProductionEOSDeviation restoredOutput;
					if(!MeasureProductionEOSDeviation(restoredTrajectory,
						restoredTrajectoryState.states.size(),failingProbeCell,restoredOutput,error))
						return 212;
					observedRestoredProbe[step]=restoredOutput.signedProbe;
					observedRestoredMaximum[step]=restoredOutput.signedAtMaximum;
					observedRestoredMaximumCell[step]=restoredOutput.maximumCell;
					observedRestoredPostResidual[step]=
						restoredTrajectory.projection.maximumPostProjectionResidualPerS;
					observedRestoredValid[step]=restoredTrajectory.projection.validationPassed?1u:0u;
					std::fprintf(stderr,"EOSRESTORE step=%zu valid=%d post=%.17g begin_probe=%.17g "
						"output_probe=%.17g output_max=%.17g output_max_cell=%zu\n",step+1u,
						restoredTrajectory.projection.validationPassed?1:0,
						restoredTrajectory.projection.maximumPostProjectionResidualPerS,
						restoredBeginning.signedProbe,restoredOutput.signedProbe,
						restoredOutput.signedAtMaximum,restoredOutput.maximumCell);
					if(!ApplyProductionResultUnchecked(restoredTrajectory,restoredTrajectoryState,error)){
						std::fprintf(stderr,"EOSRESTORE step=%zu consumer failure: %s\n",step+1u,
							error.c_str());return 213;}
					if(step==6u){
						if(!std::isfinite(beginningDeviation.signedProbe)||
							beginningDeviation.signedProbe==0.0)return 207;
						RISE::FireProductionResidentStepRequest restoredRequest=request;
						for(std::size_t cell=0u;cell<beginningDeviationField.size();++cell){
							const float absoluteReference=static_cast<float>(
								beginningDeviationField[cell]/static_cast<double>(request.force.timeStepS));
							restoredRequest.divergenceTargetPerS[cell]=
								restoredRequest.divergenceTargetPerS[cell]+absoluteReference;
						}
						RISE::FireProductionResidentStepResult restored;
						if(!RISE::AdvanceFireProductionResidentStepMetal(restoredRequest,restored,
							&error)){std::fprintf(stderr,"EOSDRAIN restored step-6 call failed: %s\n",
								error.c_str());return 202;}
						restoredProjectionValid=restored.projection.validationPassed;
						observedOneOffRestoredPreResidual=
							restored.projection.maximumPreProjectionResidualPerS;
						observedOneOffRestoredPostResidual=
							restored.projection.maximumPostProjectionResidualPerS;
						if(!restoredProjectionValid)std::fprintf(stderr,
							"EOSDRAIN restored step-6 projection failed pre=%.17g post=%.17g\n",
							restored.projection.maximumPreProjectionResidualPerS,
							restored.projection.maximumPostProjectionResidualPerS);
						if(restored.interstageFullGridTransferCount!=0u){std::fprintf(stderr,
							"EOSDRAIN restored step-6 transferred %u full grids\n",
							restored.interstageFullGridTransferCount);return 202;}
						if(restored.conservativeValues!=production.conservativeValues){
							std::size_t mismatch=0u;while(mismatch<restored.conservativeValues.size()&&
								restored.conservativeValues[mismatch]==production.conservativeValues[mismatch])
								++mismatch;
							std::fprintf(stderr,"EOSDRAIN restored step-6 changed conservative index %zu\n",
								mismatch);return 202;}
						restoredProbeState=states[index];
						if(!ApplyProductionResultUnchecked(restored,restoredProbeState,error))return 203;
						restorationReferenceDeviation=beginningDeviation.signedProbe;
						restoredProbeReady=true;
					}else if(step==7u&&restoredProbeReady){
						RISE::FireProductionResidentStepRequest drainedRequest;
						if(!BuildProductionRequest(restoredProbeState,sealed[step],flowThrough/512.0,
							drainedRequest,error))return 204;
						RISE::FireProductionResidentStepResult drained;
						if(!RISE::AdvanceFireProductionResidentStepMetal(drainedRequest,drained,&error)){
							std::fprintf(stderr,"EOSDRAIN drained step-7 call failed: %s\n",error.c_str());
							return 205;}
						if(drained.interstageFullGridTransferCount!=0u){std::fprintf(stderr,
							"EOSDRAIN drained step-7 invalid projection=%d transfers=%u pre=%.17g post=%.17g\n",
							drained.projection.validationPassed?1:0,
							drained.interstageFullGridTransferCount,
							drained.projection.maximumPreProjectionResidualPerS,
							drained.projection.maximumPostProjectionResidualPerS);return 205;}
						if(!drained.projection.validationPassed)std::fprintf(stderr,
							"EOSDRAIN drained step-7 projection failed pre=%.17g post=%.17g\n",
							drained.projection.maximumPreProjectionResidualPerS,
							drained.projection.maximumPostProjectionResidualPerS);
						drainedProjectionValid=drained.projection.validationPassed;
						observedOneOffDrainedPreResidual=
							drained.projection.maximumPreProjectionResidualPerS;
						observedOneOffDrainedPostResidual=
							drained.projection.maximumPostProjectionResidualPerS;
						ProductionEOSDeviation drainedDeviation;
						if(!MeasureProductionEOSDeviation(drained,states[index].states.size(),
							failingProbeCell,drainedDeviation,error))return 206;
						const double generation=outputDeviation.signedProbe-
							beginningDeviation.signedProbe;
						const double drainedAmount=outputDeviation.signedProbe-
							drainedDeviation.signedProbe;
						const double drainFraction=drainedAmount/restorationReferenceDeviation;
						const double predictedPlateau=generation/drainFraction;
						if(!std::isfinite(generation)||!std::isfinite(drainedAmount)||
							!std::isfinite(drainFraction)||drainFraction==0.0||
							!std::isfinite(predictedPlateau))return 208;
						observedGeneration=generation;observedDrainFraction=drainFraction;
						observedPredictedPlateau=predictedPlateau;
						std::fprintf(stderr,"EOSDRAIN step=%zu restored_valid=%d drained_valid=%d "
							"generation=%.17g reference=%.17g counterfactual=%.17g drained=%.17g "
							"fraction=%.17g plateau=%.17g\n",
							step+1u,restoredProjectionValid?1:0,
							drainedProjectionValid?1:0,generation,
							restorationReferenceDeviation,drainedDeviation.signedProbe,
							drainedAmount,drainFraction,predictedPlateau);
					}
					if(step==7u){
						static const std::array<double,8> expectedBaselineProbe={{
							0.00016243467070786721,0.00029052657505035384,
							0.00042210147392696129,0.00055758879737477507,
							0.000697411360109923,0.00084154390100632526,
							0.00099020977969921375,0.0011434014099940271}};
						static const std::array<double,8> expectedBaselineMaximum={{
							0.00026066224468057619,0.00029600029902177027,
							0.00042210147392696129,0.00055758879737477507,
							0.000697411360109923,0.00084154390100632526,
							0.00099020977969921375,0.0011434014099940271}};
						static const std::array<std::size_t,8> expectedBaselineMaximumCell={{
							79206u,165408u,2256u,2256u,2256u,2256u,2256u,2256u}};
						static const std::array<double,8> expectedRestoredProbe={{
							0.00016243467070786721,0.00015037720940047627,
							0.00011897516933578878,0.0001042666254813529,
							0.00012638825416666499,0.00016850838371063048,
							0.00019317846996003141,0.00018037112652735665}};
						static const std::array<double,8> expectedRestoredMaximum={{
							0.00026066224468057619,0.00015617602691020416,
							-0.0001726687005174643,-0.00026126028448991701,
							0.00021308223822247285,0.00029048005142096045,
							0.00033686677751032512,0.00026469334070777784}};
						static const std::array<std::size_t,8> expectedRestoredMaximumCell={{
							79206u,165408u,39755u,86706u,40u,1u,79200u,47u}};
						static const std::array<double,8> expectedRestoredPostResidual={{
							0x1.ac94p-12,0x1.35p-16,0x1.ac8658p-12,0x1.adf6p-12,
							0x1.3dc8p-16,0x1.a8263p-12,0x1.ace4p-12,0x1.57fp-16}};
						static const std::array<unsigned int,8> expectedRestoredValid={{
							0u,1u,0u,0u,1u,0u,0u,1u}};
						if(observedBaselineProbe!=expectedBaselineProbe||
							observedBaselineMaximum!=expectedBaselineMaximum||
							observedBaselineMaximumCell!=expectedBaselineMaximumCell||
							observedRestoredProbe!=expectedRestoredProbe||
							observedRestoredMaximum!=expectedRestoredMaximum||
							observedRestoredMaximumCell!=expectedRestoredMaximumCell||
							observedRestoredPostResidual!=expectedRestoredPostResidual||
							observedRestoredValid!=expectedRestoredValid||
							observedGeneration!=0.00015319163029481331||
							observedDrainFraction!=1.0030530061213276||
							observedPredictedPlateau!=0.00015272535883939468||
							restoredProjectionValid||drainedProjectionValid||
							observedOneOffRestoredPreResidual!=0x1.fc125ap-2f||
							observedOneOffRestoredPostResidual!=0x1.ac2p-12f||
							observedOneOffDrainedPreResidual!=0x1.1a9968p-1f||
							observedOneOffDrainedPostResidual!=0x1.acbf72p-12f)return 214;
					}
				}
				if((!eosProbe&&!production.physicalProjection.validationPassed)||
					!production.projection.validationPassed){std::fprintf(stderr,
					"dyadic projection validation tier=%u step=%zu physical_valid=%d "
					"physical_pre=%.9g physical_post=%.9g restoration_valid=%d "
					"restoration_pre=%.9g restoration_post=%.9g\n",Tiers[index],step,
					production.physicalProjection.validationPassed?1:0,
					production.physicalProjection.maximumPreProjectionResidualPerS,
					production.physicalProjection.maximumPostProjectionResidualPerS,
					production.projection.validationPassed?1:0,
					production.projection.maximumPreProjectionResidualPerS,
					production.projection.maximumPostProjectionResidualPerS);return 188;}
				if(!eosProbe&&index==0u&&step==0u){std::fprintf(stderr,"dyadic production projection "
					"pre=%.17g post=%.17g complementarity=%.17g mean=%.17g valid=%d\n",
					production.physicalProjection.maximumPreProjectionResidualPerS,
					production.physicalProjection.maximumPostProjectionResidualPerS,
					production.physicalProjection.maximumOpenComplementarityDiscrepancyMPerS,
					production.physicalProjection.removedFineRightHandSideMean,
					production.physicalProjection.validationPassed?1:0);
					std::uint32_t removedMeanBits=0u;std::memcpy(&removedMeanBits,
						&production.physicalProjection.removedFineRightHandSideMean,
						sizeof(removedMeanBits));
					if(!production.physicalProjection.validationPassed||
						production.physicalProjection.maximumPreProjectionResidualPerS!=0x1.7d1296p-3f||
						production.physicalProjection.maximumPostProjectionResidualPerS!=0x1.04afp-21f||
						production.physicalProjection.maximumOpenComplementarityDiscrepancyMPerS!=
							0x1.6a59cep-7f||
						removedMeanBits!=0u)return 189;
					const ProductionAffineResidual calibratingResidual=
						MeasureProductionAffineResidual(production,states[index].states.size());
					const FireAcceptedStateFeasibilityEnvelope& envelope=
						FireSimulationMethaneRecord::PhysicalV1().AcceptedStateFeasibilityEnvelope();
					const double fp32Bound=AcceptedStateRoundoffFactor(envelope,
						FireStateProducerPrecision::Binary32);
					std::fprintf(stderr,"r124 dyadic first residual_abs=%.17g residual_scaled=%.17g "
						"cell=%zu row=%zu digest=%s\n",calibratingResidual.maximumAbsolute,
						calibratingResidual.maximumScaled,calibratingResidual.cell,
						calibratingResidual.row,ProductionConservativeDigest(production).c_str());
					if(calibratingResidual.maximumAbsolute!=5.2451771873310863e-08||
						calibratingResidual.maximumScaled!=5.2451771873310863e-08||
						calibratingResidual.cell!=4915u||calibratingResidual.row!=2u||
						!(calibratingResidual.maximumScaled<fp32Bound)||
						ProductionConservativeDigest(production)!=
							"4ebc18c73162ed9a4f14c0836b4e1b5388a19883780c082ab9f9c75d2bfd5efc")
						return 190;
					std::fprintf(stderr,"dyadic production precision envelope observed=%.17g "
						"bound=%.17g margin=%.17g cell=%zu row=%zu\n",
						calibratingResidual.maximumScaled,fp32Bound,
						fp32Bound/calibratingResidual.maximumScaled,
						calibratingResidual.cell,calibratingResidual.row);}
				if(!ApplyProductionResultUnchecked(production,states[index],error)){
					const ProductionAffineResidual residual=MeasureProductionAffineResidual(
						production,states[index].states.size());
					const ProductionConsumerFailure consumer=MeasureProductionConsumerFailure(
						production,states[index].states.size());
					const std::string digest=ProductionConservativeDigest(production);
					std::fprintf(stderr,"dyadic production affine residual max_abs=%.17g "
						"max_scaled=%.17g cell=%zu row=%zu fp64_envelope=%.17g digest=%s\n",
						residual.maximumAbsolute,residual.maximumScaled,residual.cell,residual.row,
						FireSimulationMethaneRecord::PhysicalV1().AcceptedStateFeasibilityEnvelope().
							kappaEpsilon64*std::numeric_limits<double>::epsilon(),digest.c_str());
					std::fprintf(stderr,"dyadic production state reconstruction tier=%u step=%zu "
						"failed: %s\n",Tiers[index],step,error.c_str());
					std::fprintf(stderr,"dyadic production consumer failure cell=%zu temperature=%.17g "
						"eos=%.17g viscosity_total=%d\n",consumer.cell,
						consumer.temperatureK,consumer.eosResidual,consumer.viscosityTotal?1:0);
					if(index==3u&&step==7u&&consumer.cell==2256u&&
						consumer.temperatureK==348.53712185868289&&
						consumer.eosResidual==0.0011434014099940271&&consumer.viscosityTotal&&
						residual.maximumAbsolute==2.1925594524305645e-07&&
						residual.maximumScaled==2.1925594524305645e-07&&
						residual.cell==134050u&&residual.row==2u&&
						digest=="e5a8cdfd54772cc58c8d58e3a0c32d650a71f9f428cd27c1be3e52e6a60c5b70"&&
						error.find("accepted-state EOS gate")!=std::string::npos)
						return eosProbe?215:191;
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
			if(!FilterVelocity(states[index],states[index].velocity,
				states[index].values.characteristicDiameterM,productionVelocity[index]))return 229;
			productionInventory[index]=ComponentInventoryDensity(conservative);
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
			const bool exact=dA[component]==ExpectedProductionScalarEvidence[component][0]&&
				dB[component]==ExpectedProductionScalarEvidence[component][1]&&
				limitDifference[component]==ExpectedProductionScalarEvidence[component][2];
			accepted=accepted&&exact&&overlap&&approaches;
			std::fprintf(stderr,"dyadic production component=%zu D5_10=%.17g D6_12=%.17g "
				"limit_delta=%.17g radius_sum=%.17g approachA=%d approachB=%d accepted=%d\n",
				component,dA[component],dB[component],limitDifference[component],estimated?
				estimateA.fineRadius+estimateB.fineRadius:0.0,
				coarseAToB[component]>fineAToB[component]?1:0,
				coarseBToA[component]>fineBToA[component]?1:0,
				exact&&overlap&&approaches?1:0);
		}
		const double velocityA=VelocityDistance(productionVelocity[0],productionVelocity[1]);
		const double velocityB=VelocityDistance(productionVelocity[2],productionVelocity[3]);
		const FilteredVelocityField velocityLimitA=ExtrapolateVelocity(
			productionVelocity[0],productionVelocity[1]);
		const FilteredVelocityField velocityLimitB=ExtrapolateVelocity(
			productionVelocity[2],productionVelocity[3]);
		FireProductionCalibration::DyadicDistanceEstimate velocityEstimateA,velocityEstimateB;
		const bool velocityEstimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
			velocityA,VerifiedOrder,velocityEstimateA)&&
			FireProductionCalibration::DyadicDistanceAtVerifiedOrder(velocityB,VerifiedOrder,
				velocityEstimateB);
		const double velocityLimitDelta=VelocityDistance(velocityLimitA,velocityLimitB);
		const bool velocityOverlap=velocityEstimated&&
			FireProductionCalibration::DyadicLimitBallsOverlap(velocityLimitDelta,
				velocityEstimateA,velocityEstimateB);
		const bool velocityApproach=VelocityDistance(productionVelocity[0],velocityLimitB)>
			VelocityDistance(productionVelocity[1],velocityLimitB)&&
			VelocityDistance(productionVelocity[2],velocityLimitA)>
			VelocityDistance(productionVelocity[3],velocityLimitA);
		const bool velocityExact=velocityA==ExpectedProductionVelocityEvidence[0]&&
			velocityB==ExpectedProductionVelocityEvidence[1]&&
			velocityLimitDelta==ExpectedProductionVelocityEvidence[2];
		accepted=accepted&&velocityExact&&velocityOverlap&&velocityApproach;
		std::fprintf(stderr,"dyadic production velocity D5_10=%.17g D6_12=%.17g "
			"limit_delta=%.17g radius_sum=%.17g approach=%d accepted=%d\n",velocityA,
			velocityB,velocityLimitDelta,velocityEstimated?
			velocityEstimateA.fineRadius+velocityEstimateB.fineRadius:0.0,
			velocityApproach?1:0,velocityExact&&velocityOverlap&&velocityApproach?1:0);
		const double factor=std::pow(2.0,VerifiedOrder),denominator=factor-1.0;
		for(std::size_t component=0u;component<9u;++component){
			const double inventoryA=std::fabs(productionInventory[0][component]-
				productionInventory[1][component]);
			const double inventoryB=std::fabs(productionInventory[2][component]-
				productionInventory[3][component]);
			const double limitA=(factor*productionInventory[1][component]-
				productionInventory[0][component])/denominator;
			const double limitB=(factor*productionInventory[3][component]-
				productionInventory[2][component])/denominator;
			FireProductionCalibration::DyadicDistanceEstimate estimateA,estimateB;
			const bool estimated=FireProductionCalibration::DyadicDistanceAtVerifiedOrder(
				inventoryA,VerifiedOrder,estimateA)&&
				FireProductionCalibration::DyadicDistanceAtVerifiedOrder(inventoryB,
					VerifiedOrder,estimateB);
			const double limitDelta=std::fabs(limitA-limitB);
			const bool overlap=estimated&&FireProductionCalibration::DyadicLimitBallsOverlap(
				limitDelta,estimateA,estimateB);
			const bool approach=std::fabs(productionInventory[0][component]-limitB)>
				std::fabs(productionInventory[1][component]-limitB)&&
				std::fabs(productionInventory[2][component]-limitA)>
				std::fabs(productionInventory[3][component]-limitA);
			const bool exact=inventoryA==ExpectedProductionInventoryEvidence[component][0]&&
				inventoryB==ExpectedProductionInventoryEvidence[component][1]&&
				limitDelta==ExpectedProductionInventoryEvidence[component][2];
			accepted=accepted&&exact&&overlap&&approach;
			std::fprintf(stderr,"dyadic production ledger component=%zu D5_10=%.17g "
				"D6_12=%.17g limit_delta=%.17g radius_sum=%.17g approach=%d accepted=%d\n",
				component,inventoryA,inventoryB,limitDelta,estimated?
				estimateA.fineRadius+estimateB.fineRadius:0.0,approach?1:0,
				exact&&overlap&&approach?1:0);
		}
		return accepted?0:189;
	}
}

#endif
