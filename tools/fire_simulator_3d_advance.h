//////////////////////////////////////////////////////////////////////
//
//  fire_simulator_3d_advance.h - Owning 3-D conservative fire advance
//
//  Included only by fire_simulator_core.h inside RISE::FireSim.
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_SIMULATOR_3D_ADVANCE_
#define RISE_FIRE_SIMULATOR_3D_ADVANCE_
		static const std::size_t kMaximumCoupledPicardIterations=64u;

		struct PeriodicFluxPair3D
		{
			std::array<std::vector<ConservativeVector>,3> low;
			std::array<std::vector<ConservativeVector>,3> high;
			std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3>
				nonadvectiveMass;
			std::array<std::vector<double>,3> nonadvectiveEnergy;
		};

		inline std::size_t PeriodicShift3D(
			const PeriodicMACShape& shape,
			const std::size_t cell,
			const unsigned int axis,
			const int direction
			)
		{
			return direction < 0 ? PeriodicPrevious(shape,cell,axis) :
				PeriodicNext(shape,cell,axis);
		}

		inline bool ValidatePeriodicShape3D(
			const PeriodicMACShape& shape,
			std::string* error = 0
			)
		{
			return (shape.nx >= 3 && shape.ny >= 3 && shape.nz >= 3 &&
				shape.nx <= std::numeric_limits<std::size_t>::max()/shape.ny &&
				shape.nx*shape.ny <= std::numeric_limits<std::size_t>::max()/shape.nz &&
				std::isfinite(shape.cellWidthM) && shape.cellWidthM > 0.0 &&
				std::isfinite(1.0/shape.cellWidthM)) ||
				Fail(error,"fire solver 3-D conservative grid is malformed");
		}

		inline bool InvariantMCMassSlopes3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& cells,
			const FireCertifiedNullspace& reconstruction,
			std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3>& slopes,
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( !ValidatePeriodicShape3D(shape,error) || cells.size() != count ||
				reconstruction.stateDimension != MethaneMassStateDimension ||
				reconstruction.orthonormalBasis.size() !=
					reconstruction.stateDimension*reconstruction.nullity ) {
				return Fail(error,"fire solver 3-D invariant reconstruction dimensions are invalid");
			}
			std::vector<double> coordinate(count*reconstruction.nullity,0.0);
			std::array<bool,MethaneMassStateDimension> identicallyZero;
			identicallyZero.fill(true);
			for( std::size_t cell=0; cell<count; ++cell ) {
				for(std::size_t row=0;row<MethaneMassStateDimension;++row)
					identicallyZero[row]=identicallyZero[row]&&cells[cell][row]==0.0;
				for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
					for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
					coordinate[cell*reconstruction.nullity+basis] += reconstruction.orthonormalBasis[
							row*reconstruction.nullity+basis]*cells[cell][row];
					}
				}
			}
			std::vector<std::vector<double> > absentNormals;
			for(std::size_t row=0;row<MethaneMassStateDimension;++row)if(identicallyZero[row]){
				std::vector<double> normal(reconstruction.nullity);
				for(std::size_t basis=0;basis<reconstruction.nullity;++basis)normal[basis]=
					reconstruction.orthonormalBasis[row*reconstruction.nullity+basis];
				for(const std::vector<double>& prior:absentNormals){
					double dot=0.0;
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						dot+=normal[basis]*prior[basis];
					}
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						normal[basis]-=dot*prior[basis];
					}
				}
				double norm=0.0;for(const double value:normal)norm+=value*value;
				if(norm>256.0*std::numeric_limits<double>::epsilon()){norm=std::sqrt(norm);
					for(double& value:normal)value/=norm;absentNormals.push_back(normal);}}
			std::vector<double> coordinateSlope(reconstruction.nullity,0.0);
			for( unsigned int axis=0; axis<3; ++axis ) {
				slopes[axis].assign(count,std::array<double,MethaneMassStateDimension>());
				for( std::size_t cell=0; cell<count; ++cell ) {
					slopes[axis][cell].fill(0.0);
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					const std::size_t next = PeriodicNext(shape,cell,axis);
					std::fill(coordinateSlope.begin(),coordinateSlope.end(),0.0);
					for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
						coordinateSlope[basis] = MCScalarSlope(
							coordinate[cell*reconstruction.nullity+basis]-
								coordinate[previous*reconstruction.nullity+basis],
							coordinate[next*reconstruction.nullity+basis]-
								coordinate[cell*reconstruction.nullity+basis]);
					}
					for(const std::vector<double>& normal:absentNormals){double dot=0.0;
						for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
							dot+=coordinateSlope[basis]*normal[basis];
						}
						for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
							coordinateSlope[basis]-=dot*normal[basis];
						}}
					for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
						for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
							slopes[axis][cell][row] += reconstruction.orthonormalBasis[
								row*reconstruction.nullity+basis]*coordinateSlope[basis];
						}
					}
					for(std::size_t row=0;row<MethaneMassStateDimension;++row)if(
						identicallyZero[row])slopes[axis][cell][row]=0.0;
				}
			}
			return true;
		}

		inline bool BuildPeriodicFluxPair3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& cells,
			const std::vector<double>& temperatureK,
			const PeriodicMACField& faceVelocityMPerS,
			const std::vector<double>& diffusivityM2PerS,
			const std::vector<double>& conductivityWPerMK,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			PeriodicFluxPair3D& result,
			std::string* error = 0,
			const unsigned int workerCount = 1u,
			const bool buildHighOrder = true
			)
		{
			const std::size_t count = shape.CellCount();
			if( !ValidatePeriodicShape3D(shape,error) || cells.size() != count ||
				temperatureK.size() != count || diffusivityM2PerS.size() != count ||
				conductivityWPerMK.size() != count ) {
				return Fail(error,"fire solver 3-D periodic transport arrays are malformed");
			}
			std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3>
				massSlope;
			if( buildHighOrder && !InvariantMCMassSlopes3D(shape,cells,fuel.ConservativeReconstruction(),
				massSlope,error) ) return false;
			std::array<std::vector<double>,3> energySlope;
			for( unsigned int axis=0; axis<3; ++axis ) {
				if( faceVelocityMPerS.component[axis].size() != count ) {
					return Fail(error,"fire solver 3-D periodic velocity shape is invalid");
				}
				if(buildHighOrder)energySlope[axis].assign(count,0.0);
				result.low[axis].assign(count,ConservativeVector());
				result.high[axis].assign(count,ConservativeVector());
				result.nonadvectiveMass[axis].assign(count,
					std::array<double,MethaneMassStateDimension>());
				result.nonadvectiveEnergy[axis].assign(count,0.0);
				if(buildHighOrder)for( std::size_t cell=0; cell<count; ++cell ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					const std::size_t next = PeriodicNext(shape,cell,axis);
					energySlope[axis][cell] = MCScalarSlope(
						cells[cell][MethaneMassStateDimension]-
							cells[previous][MethaneMassStateDimension],
						cells[next][MethaneMassStateDimension]-
							cells[cell][MethaneMassStateDimension]);
				}
			}
			const std::size_t workCount=3u*count;
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(workCount)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureWork(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker) threads.emplace_back([&,worker]() {
				const std::size_t first=workCount*worker/workers;
				const std::size_t last=workCount*(worker+1u)/workers;
				for(std::size_t work=first;work<last;++work) {
				const unsigned int axis=static_cast<unsigned int>(work/count);
				const std::size_t left=work%count;
				const std::size_t right = PeriodicNext(shape,left,axis);
				double totalLeft = 0.0, totalRight = 0.0;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					totalLeft += cells[left][1+species];
					totalRight += cells[right][1+species];
				}
				const double velocity = faceVelocityMPerS.component[axis][left];
				if( totalLeft <= 0.0 || totalRight <= 0.0 || !std::isfinite(totalLeft) ||
					!std::isfinite(totalRight) || !std::isfinite(temperatureK[left]) ||
					!std::isfinite(temperatureK[right]) || !std::isfinite(velocity) ||
					!std::isfinite(diffusivityM2PerS[left]) || diffusivityM2PerS[left] < 0.0 ||
					!std::isfinite(diffusivityM2PerS[right]) || diffusivityM2PerS[right] < 0.0 ||
					!std::isfinite(conductivityWPerMK[left]) || conductivityWPerMK[left] < 0.0 ||
					!std::isfinite(conductivityWPerMK[right]) || conductivityWPerMK[right] < 0.0 ) {
					failureWork[worker]=work;
					failureMessage[worker]="fire solver 3-D periodic transport state is outside its domain";
					break;
				}
				const double rhoD = HarmonicMean(totalLeft*diffusivityM2PerS[left],
					totalRight*diffusivityM2PerS[right]);
				std::array<double,MethaneMassStateDimension> raw,projected;
				raw.fill(0.0);projected.fill(0.0);
				bool identicalMassState=true;
				for(std::size_t component=0;component<MethaneMassStateDimension;++component)
					identicalMassState=identicalMassState&&cells[left][component]==
						cells[right][component];
				if(!identicalMassState){
					raw[0] = -rhoD*(cells[right][0]/totalRight-cells[left][0]/totalLeft)/
						shape.cellWidthM;
					for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
						raw[1+species] = -rhoD*(cells[right][1+species]/totalRight-
							cells[left][1+species]/totalLeft)/shape.cellWidthM;
					}
				}
				// This is the sole physical mass-flux correction: exactly N_C N_C^T Jtilde.
				std::string faceError;
				if( !fuel.NonadvectiveFluxProjection().Project(raw.data(),raw.size(),
					projected.data(),projected.size(),&faceError) ) {
					failureWork[worker]=work;failureMessage[worker]=faceError;break;
				}
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					result.nonadvectiveMass[axis][left][component] = projected[component];
				}
				const double faceTemperature = 0.5*(temperatureK[left]+temperatureK[right]);
				double enthalpyFlux = 0.0;
				std::array<double,MethaneSpeciesCount> speciesEnthalpy;
				if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(faceTemperature,
					speciesEnthalpy.data(),speciesEnthalpy.size(),&faceError)) {
					failureWork[worker]=work;failureMessage[worker]=faceError;break;
				}
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					enthalpyFlux += speciesEnthalpy[species]*projected[1+species];
				}
				const double conductivity = HarmonicMean(conductivityWPerMK[left],
					conductivityWPerMK[right]);
				const double physicalEnergy = enthalpyFlux-conductivity*
					(temperatureK[right]-temperatureK[left])/shape.cellWidthM;
				result.nonadvectiveEnergy[axis][left] = physicalEnergy;
				const std::size_t donor = velocity >= 0.0 ? left : right;
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					result.low[axis][left][component] = velocity*cells[donor][component]+
						projected[component];
					if(buildHighOrder){const double highValue=velocity>=0.0?cells[left][component]+
						0.5*massSlope[axis][left][component]:cells[right][component]-
						0.5*massSlope[axis][right][component];
						result.high[axis][left][component]=velocity*highValue+projected[component];}
					else result.high[axis][left][component]=result.low[axis][left][component];
				}
				result.low[axis][left][MethaneMassStateDimension] =
					velocity*cells[donor][MethaneMassStateDimension]+physicalEnergy;
				if(buildHighOrder){const double highEnergy=velocity>=0.0?
					cells[left][MethaneMassStateDimension]+0.5*energySlope[axis][left]:
					cells[right][MethaneMassStateDimension]-0.5*energySlope[axis][right];
					result.high[axis][left][MethaneMassStateDimension]=velocity*highEnergy+physicalEnergy;}
				else result.high[axis][left][MethaneMassStateDimension]=
					result.low[axis][left][MethaneMassStateDimension];
				}
			});
			for(std::thread& thread:threads) thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureWork[worker]<firstFailure){
				firstFailure=failureWork[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			return true;
		}

		inline bool FrozenPacketDeltas3D(
			const std::vector<MethaneSourcePacket>& packet,
			const std::size_t count,
			std::vector<ConservativeVector>& delta,
			const unsigned int workerCount,
			std::string* error = 0
			)
		{
			if( packet.size() != count ) {
				return Fail(error,"fire solver 3-D frozen-packet shape is invalid");
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					const double value = packet[cell].constituentDelta[species];
					if( !std::isfinite(value) ) return Fail(error,
						"fire solver 3-D frozen packet contains a non-finite mass delta");
				}
				if( !std::isfinite(packet[cell].sensibleEnergyDeltaJPerM3) ) return Fail(error,
					"fire solver 3-D frozen packet contains a non-finite energy delta");
			}
			delta.assign(count,ConservativeVector());
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker) threads.emplace_back([&,worker]() {
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell) {
					for(std::size_t species=0;species<MethaneSpeciesCount;++species)
						delta[cell][1+species]=packet[cell].constituentDelta[species];
					delta[cell][MethaneMassStateDimension]=packet[cell].sensibleEnergyDeltaJPerM3;
				}
			});
			for(std::thread& thread:threads) thread.join();
			return true;
		}

		inline bool FrozenPacketExpansionAdmissible3D(
			const std::vector<ConservativeVector>& beginning,
			const std::vector<MethaneSourcePacket>& packet,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			const unsigned int workerCount,
			std::string* error = 0
			)
		{
			const std::size_t count=beginning.size();
			if(count==0||packet.size()!=count||workerCount==0u)
				return Fail(error,"fire solver source-expansion arrays are malformed");
			std::vector<double> temperatureK;
			if(!InvertPeriodicTemperaturesWithinBounds(beginning,thermochemistry,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				temperatureK,error,workerCount))return false;
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell){
					std::string cellError;
					if(!FrozenSourcePacketExpansionAdmissible(beginning[cell],temperatureK[cell],
						packet[cell],deltaTimeS,thermochemistry,0,&cellError)){
						failureCell[worker]=cell;failureMessage[worker]=cellError;break;
					}
				}
			});
			for(std::thread& thread:threads)thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure){
				std::ostringstream message;message<<"fire solver source packet cell "<<firstFailure<<": "
					<<failureMessage[failedWorker];return Fail(error,message.str());
			}
			return true;
		}

		inline void FrozenPacketProjectionPairs3D(
			const std::vector<MethaneSourcePacket>& packet,
			std::vector<double>& energyDeltaJPerM3,
			std::vector<double>& expansionIntegral
			)
		{
			energyDeltaJPerM3.resize(packet.size());
			expansionIntegral.resize(packet.size());
			for(std::size_t cell=0;cell<packet.size();++cell){
				energyDeltaJPerM3[cell]=packet[cell].pilotEnergyDeltaJPerM3;
				expansionIntegral[cell]=packet[cell].pilotExpansionIntegral;
			}
		}

		struct LimiterPicardAcceptance3D
		{
			std::array<std::vector<double>,3> faceAlpha;
			double maximumFaceDiscrepancy=0.0;
			bool discontinuousClass=false;
		};

		inline bool SelectLimiterPicardAcceptance3D(
			const std::array<std::vector<double>,3>& acceptingIteration,
			const std::array<std::vector<double>,3>& verification,
			const double projectionTolerance,
			LimiterPicardAcceptance3D& result,
			std::string* error=0)
		{
			if(!std::isfinite(projectionTolerance)||projectionTolerance<0.0)
				return Fail(error,"fire solver limiter acceptance tolerance is invalid");
			LimiterPicardAcceptance3D candidate;
			for(unsigned int axis=0;axis<3;++axis){
				if(acceptingIteration[axis].size()!=verification[axis].size())
					return Fail(error,"fire solver limiter acceptance shape is invalid");
				candidate.faceAlpha[axis].resize(verification[axis].size());
				for(std::size_t face=0;face<verification[axis].size();++face){
					const double next=acceptingIteration[axis][face];
					const double verified=verification[axis][face];
					if(!std::isfinite(next)||!std::isfinite(verified)||next<0.0||next>1.0||
						verified<0.0||verified>1.0) return Fail(error,
							"fire solver limiter acceptance coefficient is invalid");
					candidate.maximumFaceDiscrepancy=std::max(candidate.maximumFaceDiscrepancy,
						std::fabs(verified-next));
				}
			}
			candidate.discontinuousClass=candidate.maximumFaceDiscrepancy>projectionTolerance;
			for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;
				face<verification[axis].size();++face) candidate.faceAlpha[axis][face]=
					candidate.discontinuousClass?std::min(acceptingIteration[axis][face],
						verification[axis][face]):verification[axis][face];
			result=std::move(candidate);
			return true;
		}

		inline bool ApplyPeriodicSharedFCT3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const PeriodicFluxPair3D& flux,
			const std::vector<ConservativeVector>& frozenSourceDelta,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::array<std::vector<double>,3>& faceAlpha,
			std::string* error = 0,
			const std::array<std::vector<double>,3>* acceptedFaceAlpha = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( !ValidatePeriodicShape3D(shape,error) || beginning.size() != count ||
				frozenSourceDelta.size() != count || !std::isfinite(config.deltaTimeS) ||
				config.deltaTimeS <= 0.0 ) {
				return Fail(error,"fire solver 3-D FCT input is malformed");
			}
			for( unsigned int axis=0; axis<3; ++axis ) {
				if( flux.low[axis].size() != count || flux.high[axis].size() != count ) {
					return Fail(error,"fire solver 3-D FCT flux shape is invalid");
				}
			}
			std::array<double,MethaneSpeciesCount> ambientEnthalpy, adiabaticEnthalpy;
			if( !FireSimulationEnthalpyBounds(config,thermochemistry,
				ambientEnthalpy,adiabaticEnthalpy,error) ) return false;
			const double scale = config.deltaTimeS/shape.cellWidthM;
			std::vector<ConservativeVector> low(count);
			std::array<std::vector<ConservativeVector>,6> correction;
			for( unsigned int direction=0; direction<6; ++direction ) {
				correction[direction].assign(count,ConservativeVector());
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				if(!AcceptedStateAdmissible(beginning[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,error))return false;
				low[cell] = beginning[cell]+frozenSourceDelta[cell];
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					low[cell] = low[cell]+scale*(flux.low[axis][previous]-flux.low[axis][cell]);
					correction[2*axis][cell] = scale*(flux.high[axis][previous]-
						flux.low[axis][previous]);
					correction[2*axis+1][cell] = -scale*(flux.high[axis][cell]-
						flux.low[axis][cell]);
				}
				if(!AcceptedStateAdmissible(low[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,error))return false;
			}
			const std::size_t inequalityCount = 4+MethaneSpeciesCount;
			std::vector<double> ratio(count*inequalityCount,1.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				std::array<ConservativeVector,6> limiterCorrection={};
				for(unsigned int direction=0;direction<6;++direction)
					limiterCorrection[direction]=correction[direction][cell];
				for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
					// Reserve the record-derived limiter assembly bound for the six face
					// accumulations performed after this ratio is formed.  Mass
					// inequalities must never inherit the energy component's unit scale.
					const double scaleLowerBound=CertifiedLimiterScaleLowerBound(low[cell],
						limiterCorrection,6,inequality,ambientEnthalpy,adiabaticEnthalpy);
					const double budget=CertifiedLimiterInequalityBudget(low[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy,fuel,scaleLowerBound);
					double requested = 0.0;
					for( unsigned int direction=0; direction<6; ++direction ) requested +=
						std::max(0.0,InequalityValue(correction[direction][cell],inequality,
							ambientEnthalpy,adiabaticEnthalpy));
				ratio[cell*inequalityCount+inequality] = requested > 0.0 ?
					std::min(1.0,budget/requested) : 1.0;
				}
			}
			for( unsigned int axis=0; axis<3; ++axis ) {
				faceAlpha[axis].assign(count,1.0);
				for( std::size_t left=0; left<count; ++left ) {
					const std::size_t right = PeriodicNext(shape,left,axis);
					for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
						if( InequalityValue(correction[2*axis+1][left],inequality,
							ambientEnthalpy,adiabaticEnthalpy) > 0.0 ) {
							faceAlpha[axis][left] = std::min(faceAlpha[axis][left],
								ratio[left*inequalityCount+inequality]);
						}
						if( InequalityValue(correction[2*axis][right],inequality,
							ambientEnthalpy,adiabaticEnthalpy) > 0.0 ) {
							faceAlpha[axis][left] = std::min(faceAlpha[axis][left],
								ratio[right*inequalityCount+inequality]);
						}
					}
				}
			}
			if(acceptedFaceAlpha){
				for(unsigned int axis=0;axis<3;++axis){
					if((*acceptedFaceAlpha)[axis].size()!=faceAlpha[axis].size()) return Fail(error,
						"fire solver 3-D accepted limiter shape is invalid");
					for(std::size_t face=0;face<faceAlpha[axis].size();++face){
						const double accepted=(*acceptedFaceAlpha)[axis][face];
						if(!std::isfinite(accepted)||accepted<0.0||accepted>faceAlpha[axis][face])
							return Fail(error,"fire solver 3-D accepted limiter exceeds its certificate");
					}
					faceAlpha[axis]=(*acceptedFaceAlpha)[axis];
				}
			}
			std::vector<ConservativeVector> candidate = low;
			for( unsigned int axis=0; axis<3; ++axis ) for( std::size_t left=0;
				left<count; ++left ) {
				const std::size_t right = PeriodicNext(shape,left,axis);
				const ConservativeVector accepted = scale*faceAlpha[axis][left]*
					(flux.high[axis][left]-flux.low[axis][left]);
				candidate[left] = candidate[left]-accepted;
				candidate[right] = candidate[right]+accepted;
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				if(!AcceptedStateAdmissible(candidate[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,error))return false;
			}
			result.swap(candidate);
			return true;
		}

		// Test reference for spatial/temporal convergence.  It owns no production
		// state and is deliberately not callable through fire_simulator.
		inline bool ReferenceAdvancePeriodicTransportHeun3DWithSource(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const PeriodicMACField& velocity,
			const std::vector<double>& diffusivity,
			const std::vector<double>& conductivity,
			const std::vector<ConservativeVector>& frozenSourceDelta,
			const bool donorOnly,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::array<std::vector<double>,3>& acceptedAlpha,
			std::string* error = 0
			)
		{
			std::vector<double> temperature0, temperature1;
			if( !InvertPeriodicTemperatures(beginning,thermochemistry,temperature0,error) )
				return false;
			PeriodicFluxPair3D flux0;
			if( !BuildPeriodicFluxPair3D(shape,beginning,temperature0,velocity,diffusivity,
				conductivity,fuel,thermochemistry,flux0,error) ) return false;
			if( donorOnly ) flux0.high=flux0.low;
			if(frozenSourceDelta.size()!=beginning.size()) return Fail(error,
				"fire solver 3-D reference source shape is invalid");
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if( !ApplyPeriodicSharedFCT3D(shape,beginning,flux0,frozenSourceDelta,config,fuel,
				thermochemistry,predictor,predictorAlpha,error) ||
				!InvertPeriodicTemperatures(predictor,thermochemistry,temperature1,error) )
				return false;
			PeriodicFluxPair3D flux1, averaged;
			if( !BuildPeriodicFluxPair3D(shape,predictor,temperature1,velocity,diffusivity,
				conductivity,fuel,thermochemistry,flux1,error) ) return false;
			if( donorOnly ) flux1.high=flux1.low;
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t count=beginning.size();
				averaged.low[axis].resize(count); averaged.high[axis].resize(count);
				averaged.nonadvectiveMass[axis].resize(count);
				averaged.nonadvectiveEnergy[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) {
					averaged.low[axis][face]=0.5*(flux0.low[axis][face]+flux1.low[axis][face]);
					averaged.high[axis][face]=0.5*(flux0.high[axis][face]+flux1.high[axis][face]);
					averaged.nonadvectiveEnergy[axis][face]=0.5*(
						flux0.nonadvectiveEnergy[axis][face]+flux1.nonadvectiveEnergy[axis][face]);
					for( std::size_t component=0; component<MethaneMassStateDimension; ++component )
						averaged.nonadvectiveMass[axis][face][component]=0.5*(
							flux0.nonadvectiveMass[axis][face][component]+
							flux1.nonadvectiveMass[axis][face][component]);
				}
			}
			return ApplyPeriodicSharedFCT3D(shape,beginning,averaged,frozenSourceDelta,config,fuel,
				thermochemistry,result,acceptedAlpha,error);
		}

		inline bool ReferenceAdvancePeriodicTransportHeun3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const PeriodicMACField& velocity,
			const std::vector<double>& diffusivity,
			const std::vector<double>& conductivity,
			const bool donorOnly,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::array<std::vector<double>,3>& acceptedAlpha,
			std::string* error = 0
			)
		{
			return ReferenceAdvancePeriodicTransportHeun3DWithSource(shape,beginning,velocity,
				diffusivity,conductivity,std::vector<ConservativeVector>(beginning.size()),donorOnly,
				config,fuel,thermochemistry,result,acceptedAlpha,error);
		}

		struct DebugMacCormackNegativeControl
		{
			double relativeInventoryError;
			double maximumLocalConservativeError;
			double maximumAffineResidual;
			bool clampActivated;
			DebugMacCormackNegativeControl() : relativeInventoryError(0.0),
				maximumLocalConservativeError(0.0),maximumAffineResidual(0.0),
				clampActivated(false) {}
		};

		// Semi-Lagrangian MacCormack is retained only as V3(c)'s deliberately bad
		// comparator.  It returns diagnostics, never a conservative state, so it is
		// structurally incapable of committing production fields.
		inline bool EvaluateDebugMacCormackNegativeControl1D(
			const std::vector<double>& density,
			const std::vector<double>& cellVelocityMPerS,
			const double cellWidthM,
			const double deltaTimeS,
			DebugMacCormackNegativeControl& result,
			std::string* error = 0
			)
		{
			const std::size_t count=density.size();
			if( count<8 || cellVelocityMPerS.size()!=count || !std::isfinite(cellWidthM) ||
				cellWidthM<=0.0 || !std::isfinite(deltaTimeS) || deltaTimeS<=0.0 ) return
				Fail(error,"fire solver debug MacCormack fixture is malformed");
			auto sample=[&]( const std::vector<double>& field, double coordinate ) {
				coordinate-=std::floor(coordinate/static_cast<double>(count))*
					static_cast<double>(count);
				const std::size_t left=static_cast<std::size_t>(std::floor(coordinate))%count;
				const std::size_t right=(left+1)%count;
				const double fraction=coordinate-std::floor(coordinate);
				return (1.0-fraction)*field[left]+fraction*field[right];
			};
			std::vector<double> forward(count,0.0), backward(count,0.0), corrected(count,0.0);
			for( std::size_t cell=0;cell<count;++cell ) {
				if( !std::isfinite(density[cell]) || density[cell]<0.0 ||
					!std::isfinite(cellVelocityMPerS[cell]) ) return Fail(error,
					"fire solver debug MacCormack fixture is non-finite");
				const double courant=deltaTimeS*cellVelocityMPerS[cell]/cellWidthM;
				forward[cell]=sample(density,static_cast<double>(cell)-courant);
			}
			for( std::size_t cell=0;cell<count;++cell ) {
				const double courant=deltaTimeS*cellVelocityMPerS[cell]/cellWidthM;
				backward[cell]=sample(forward,static_cast<double>(cell)+courant);
				const double unlimited=forward[cell]+0.5*(density[cell]-backward[cell]);
				const double lo=std::min({density[(cell+count-1)%count],density[cell],
					density[(cell+1)%count]});
				const double hi=std::max({density[(cell+count-1)%count],density[cell],
					density[(cell+1)%count]});
				corrected[cell]=std::max(lo,std::min(hi,unlimited));
				result.clampActivated=result.clampActivated || corrected[cell]!=unlimited;
			}
			double before=0.0,after=0.0;
			for( std::size_t cell=0;cell<count;++cell ) { before+=density[cell];
				after+=corrected[cell]; }
			result.relativeInventoryError=std::fabs(after-before)/std::max(1.0,std::fabs(before));
			result.maximumLocalConservativeError=0.0;
			for( std::size_t cell=0;cell<count;++cell ) {
				const std::size_t previous=(cell+count-1)%count;
				const double faceRight=0.5*(cellVelocityMPerS[cell]+
					cellVelocityMPerS[(cell+1)%count]);
				const double faceLeft=0.5*(cellVelocityMPerS[previous]+cellVelocityMPerS[cell]);
				const double donorRight=faceRight>=0.0?density[cell]:density[(cell+1)%count];
				const double donorLeft=faceLeft>=0.0?density[previous]:density[cell];
				const double conservative=density[cell]-deltaTimeS/cellWidthM*(
					faceRight*donorRight-faceLeft*donorLeft);
				result.maximumLocalConservativeError=std::max(
					result.maximumLocalConservativeError,std::fabs(corrected[cell]-conservative));
			}
			return (std::isfinite(result.relativeInventoryError) &&
				std::isfinite(result.maximumLocalConservativeError)) || Fail(error,
				"fire solver debug MacCormack diagnostic overflowed");
		}

		// Full-state V3(c) diagnostic.  Only aggregate failure measures escape;
		// the semi-Lagrangian candidate is never returned and therefore cannot be
		// installed as a production conservative state.
		inline bool EvaluateDebugMacCormackAffineNegativeControl1D(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& cellVelocityMPerS,
			const double cellWidthM,
			const double deltaTimeS,
			const FireCertifiedNullspace& reconstruction,
			DebugMacCormackNegativeControl& result,
			std::string* error=0
			)
		{
			if(state.size()<8||cellVelocityMPerS.size()!=state.size())return Fail(error,
				"fire solver full-state debug MacCormack fixture is malformed");
			std::vector<ConservativeVector> corrected(state.size());
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				std::vector<double> field(state.size());
				for(std::size_t cell=0;cell<state.size();++cell)field[cell]=state[cell][component];
				const std::size_t count=field.size();
				auto sample=[&](const std::vector<double>& values,double coordinate){
					coordinate-=std::floor(coordinate/static_cast<double>(count))*
						static_cast<double>(count);
					const std::size_t left=static_cast<std::size_t>(std::floor(coordinate))%count;
					const std::size_t right=(left+1)%count;
					const double fraction=coordinate-std::floor(coordinate);
					return (1.0-fraction)*values[left]+fraction*values[right];
				};
				std::vector<double> forward(count),backward(count),candidate(count);
				for(std::size_t cell=0;cell<count;++cell){
					const double courant=deltaTimeS*cellVelocityMPerS[cell]/cellWidthM;
					forward[cell]=sample(field,static_cast<double>(cell)-courant);
				}
				double before=0.0,after=0.0;
				for(std::size_t cell=0;cell<count;++cell){
					const double courant=deltaTimeS*cellVelocityMPerS[cell]/cellWidthM;
					backward[cell]=sample(forward,static_cast<double>(cell)+courant);
					const double unlimited=forward[cell]+0.5*(field[cell]-backward[cell]);
					const double lo=std::min({field[(cell+count-1)%count],field[cell],
						field[(cell+1)%count]});
					const double hi=std::max({field[(cell+count-1)%count],field[cell],
						field[(cell+1)%count]});
					candidate[cell]=std::max(lo,std::min(hi,unlimited));
					result.clampActivated=result.clampActivated||candidate[cell]!=unlimited;
					before+=field[cell];after+=candidate[cell];
					corrected[cell][component]=candidate[cell];
				}
				result.relativeInventoryError=std::max(result.relativeInventoryError,
					std::fabs(after-before)/std::max(1.0,std::fabs(before)));
			}
			for(const ConservativeVector& cell:corrected)for(std::size_t row=0;
				row<reconstruction.constraintRows;++row){
				double residual=0.0;for(std::size_t column=0;
					column<reconstruction.stateDimension;++column)residual+=
						reconstruction.constraintMatrix[row*reconstruction.stateDimension+column]*
						cell[column];
				result.maximumAffineResidual=std::max(result.maximumAffineResidual,
					std::fabs(residual));
			}
			return (std::isfinite(result.relativeInventoryError)&&
				std::isfinite(result.maximumAffineResidual))||Fail(error,
					"fire solver full-state debug MacCormack diagnostic overflowed");
		}

		inline bool BuildPeriodicStageTransport3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const PeriodicMACField& faceVelocityMPerS,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<double>& diffusivityM2PerS,
			std::vector<double>& conductivityWPerMK,
			std::vector<double>& dynamicViscosityPaS,
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( !ValidatePeriodicShape3D(shape,error) || state.size() != count ||
				temperatureK.size() != count || !transport.IsValid() ) return Fail(error,
				"fire solver 3-D stage transport input is malformed");
			std::array<std::vector<double>,3> cellVelocity;
			for( unsigned int component=0; component<3; ++component ) {
				if( faceVelocityMPerS.component[component].size() != count ) return Fail(error,
					"fire solver 3-D stage velocity shape is invalid");
				cellVelocity[component].assign(count,0.0);
				for( std::size_t cell=0; cell<count; ++cell ) cellVelocity[component][cell] =
					0.5*(faceVelocityMPerS.component[component][cell]+
					faceVelocityMPerS.component[component][PeriodicPrevious(shape,cell,component)]);
			}
			diffusivityM2PerS.assign(count,0.0);
			conductivityWPerMK.assign(count,0.0);
			dynamicViscosityPaS.assign(count,0.0);
			const double widths[3] = {shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
			for( std::size_t cell=0; cell<count; ++cell ) {
				MethaneCellState physical = FromConservativeVector(state[cell]);
				physical.temperatureK = temperatureK[cell];
				double gradient[3][3] = {};
				for( unsigned int derivative=0; derivative<3; ++derivative ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,derivative);
					const std::size_t next = PeriodicNext(shape,cell,derivative);
					for( unsigned int component=0; component<3; ++component ) {
						gradient[derivative][component] = (cellVelocity[component][next]-
							cellVelocity[component][previous])/(2.0*shape.cellWidthM);
					}
				}
				CellTransportEvaluation evaluation;
				if( !EvaluateCellTransport(physical,gradient,widths,dns,thermochemistry,
					transport,evaluation,error) ) return false;
				diffusivityM2PerS[cell] = evaluation.totalDiffusivityM2PerS;
				conductivityWPerMK[cell] = evaluation.effectiveConductivityWPerMK;
				dynamicViscosityPaS[cell] = evaluation.effectiveViscosityPaS;
			}
			return true;
		}

		inline bool PeriodicDivergenceTargetFromPhysicalFlux3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const PeriodicFluxPair3D& flux,
			const std::vector<ConservativeVector>& sourceDelta,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& result,
			std::string* error = 0,
			const unsigned int workerCount = 1u,
			const std::vector<double>* projectionEnergyDeltaJPerM3 = 0,
			const std::vector<double>* projectionExpansionIntegral = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( state.size() != count || temperatureK.size() != count ||
				sourceDelta.size() != count || !std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ) {
				return Fail(error,"fire solver 3-D divergence target arrays are malformed");
			}
			if((projectionEnergyDeltaJPerM3==0)!=(projectionExpansionIntegral==0)||
				(projectionEnergyDeltaJPerM3&&(projectionEnergyDeltaJPerM3->size()!=count||
					projectionExpansionIntegral->size()!=count)))return Fail(error,
					"fire solver 3-D projection-source pair is malformed");
			for(unsigned int axis=0;axis<3;++axis) if(flux.nonadvectiveMass[axis].size()!=count ||
				flux.nonadvectiveEnergy[axis].size()!=count) return Fail(error,
					"fire solver 3-D physical flux shape is invalid");
			std::vector<double> candidate(count,0.0);
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker) threads.emplace_back([&,worker]() {
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell) {
					ConservativeVector increment = sourceDelta[cell];
					if(projectionEnergyDeltaJPerM3)
						increment[MethaneMassStateDimension]-=(*projectionEnergyDeltaJPerM3)[cell];
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					for( std::size_t component=0; component<MethaneMassStateDimension;
						++component ) increment[component] += deltaTimeS*
							(flux.nonadvectiveMass[axis][previous][component]-
							flux.nonadvectiveMass[axis][cell][component])/shape.cellWidthM;
					increment[MethaneMassStateDimension] += deltaTimeS*
						(flux.nonadvectiveEnergy[axis][previous]-
						flux.nonadvectiveEnergy[axis][cell])/shape.cellWidthM;
				}
				std::string cellError;
				if(!DivergenceFromDiscreteIncrement(state[cell],increment,temperatureK[cell],
					deltaTimeS,thermochemistry,candidate[cell],&cellError)) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				if(projectionExpansionIntegral)candidate[cell]+=
					(*projectionExpansionIntegral)[cell]/deltaTimeS;
				}
			});
			for(std::thread& thread:threads) thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker) if(failureCell[worker]<firstFailure) {
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure) return Fail(error,failureMessage[failedWorker]);
			result.swap(candidate);
			return true;
		}

		inline void GasPrimalSubfluxes3D(
			const PeriodicFluxPair3D& flux,
			std::array<std::vector<double>,3>& lowAdvection,
			std::array<std::vector<double>,3>& highAdvection,
			std::array<std::vector<double>,3>& physicalDiffusion
			)
		{
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t count = flux.low[axis].size();
				lowAdvection[axis].assign(count,0.0);
				highAdvection[axis].assign(count,0.0);
				physicalDiffusion[axis].assign(count,0.0);
				for( std::size_t face=0; face<count; ++face ) {
					for( std::size_t species=0; species<MethaneCarbon; ++species ) {
						const double physical = flux.nonadvectiveMass[axis][face][1+species];
						physicalDiffusion[axis][face] += physical;
						lowAdvection[axis][face] += flux.low[axis][face][1+species]-physical;
						highAdvection[axis][face] += flux.high[axis][face][1+species]-physical;
					}
				}
			}
		}

		inline std::array<std::vector<double>,3> CompatibleMomentumFluxDivergence3D(
			const PeriodicMACShape& shape,
			const std::array<std::vector<double>,3>& acceptedGasAdvection,
			const std::array<std::vector<double>,3>& physicalGasDiffusion,
			const PeriodicMACField& faceVelocity
			)
		{
			const std::size_t count = shape.CellCount();
			std::array<std::vector<double>,3> result;
			for( unsigned int component=0; component<3; ++component ) {
				result[component].assign(count,0.0);
				for( std::size_t face=0; face<count; ++face ) {
					double divergence = 0.0;
					for( unsigned int derivative=0; derivative<3; ++derivative ) {
						const std::size_t nextComponent = PeriodicNext(shape,face,component);
						const std::size_t previousDerivative =
							PeriodicPrevious(shape,face,derivative);
						const std::size_t nextComponentPreviousDerivative =
							PeriodicPrevious(shape,nextComponent,derivative);
						auto massFlux = [&]( const std::size_t index ) {
							return acceptedGasAdvection[derivative][index]+
								physicalGasDiffusion[derivative][index];
						};
						double upper = 0.0, lower = 0.0;
						if( derivative == component ) {
							// Restrict the primal gas flux to the momentum control-volume
							// boundary with the same I_i used for staggered density.  This is
							// the discrete compatibility identity D_i I_i = I_rho,i D.
							upper = 0.25*(massFlux(face)+massFlux(nextComponent))*(
								faceVelocity.component[component][face]+
								faceVelocity.component[component][nextComponent]);
							lower = 0.25*(massFlux(previousDerivative)+massFlux(face))*(
								faceVelocity.component[component][previousDerivative]+
								faceVelocity.component[component][face]);
						} else {
							const std::size_t nextDerivative =
								PeriodicNext(shape,face,derivative);
							upper = 0.25*(massFlux(face)+massFlux(nextComponent))*(
								faceVelocity.component[component][face]+
								faceVelocity.component[component][nextDerivative]);
							lower = 0.25*(massFlux(previousDerivative)+
								massFlux(nextComponentPreviousDerivative))*(
								faceVelocity.component[component][previousDerivative]+
								faceVelocity.component[component][face]);
						}
						divergence += (upper-lower)/shape.cellWidthM;
					}
					result[component][face] = divergence;
				}
			}
			return result;
		}

		inline bool RemainingMomentumRHS3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const PeriodicMACField& faceVelocity,
			const std::vector<double>& dynamicViscosityPaS,
			const std::vector<ConservativeVector>& frozenSourceDelta,
			const double deltaTimeS,
			const double ambientGasDensityKGPerM3,
			const std::array<double,3>& gravityMPerS2,
			PeriodicMACField& result,
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( state.size() != count || dynamicViscosityPaS.size() != count ||
				frozenSourceDelta.size() != count || !std::isfinite(deltaTimeS) ||
				deltaTimeS <= 0.0 || !std::isfinite(ambientGasDensityKGPerM3) ||
				ambientGasDensityKGPerM3 <= 0.0 ) return Fail(error,
				"fire solver 3-D nonpressure momentum input is malformed");
			std::array<std::vector<double>,3> cellVelocity;
			for( unsigned int component=0; component<3; ++component ) {
				if( faceVelocity.component[component].size() != count ||
					!std::isfinite(gravityMPerS2[component]) ) return Fail(error,
					"fire solver 3-D momentum field is malformed");
				cellVelocity[component].assign(count,0.0);
				result.component[component].assign(count,0.0);
				for( std::size_t cell=0; cell<count; ++cell ) cellVelocity[component][cell] =
					0.5*(faceVelocity.component[component][cell]+
					faceVelocity.component[component][PeriodicPrevious(shape,cell,component)]);
			}
			std::array<std::array<std::vector<double>,3>,3> stress;
			for( unsigned int component=0; component<3; ++component ) for( unsigned int
				derivative=0; derivative<3; ++derivative ) stress[component][derivative].
				assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !std::isfinite(dynamicViscosityPaS[cell]) ||
					dynamicViscosityPaS[cell] < 0.0 ) return Fail(error,
					"fire solver 3-D viscosity is invalid");
				double gradient[3][3] = {}, divergence = 0.0;
				for( unsigned int derivative=0; derivative<3; ++derivative ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,derivative);
					const std::size_t next = PeriodicNext(shape,cell,derivative);
					for( unsigned int component=0; component<3; ++component ) gradient[
						derivative][component] = (cellVelocity[component][next]-
						cellVelocity[component][previous])/(2.0*shape.cellWidthM);
					divergence += gradient[derivative][derivative];
				}
				for( unsigned int component=0; component<3; ++component ) for( unsigned int
					derivative=0; derivative<3; ++derivative ) stress[component][derivative][cell] =
					dynamicViscosityPaS[cell]*(gradient[derivative][component]+
					gradient[component][derivative]-(component==derivative ?
					(2.0/3.0)*divergence : 0.0));
			}
			for( unsigned int component=0; component<3; ++component ) for( std::size_t face=0;
				face<count; ++face ) {
				const std::size_t right = PeriodicNext(shape,face,component);
				double viscous = (stress[component][component][right]-
					stress[component][component][face])/shape.cellWidthM;
				for( unsigned int derivative=0; derivative<3; ++derivative ) {
					if( derivative == component ) continue;
					const std::size_t previous = PeriodicPrevious(shape,face,derivative);
					const std::size_t next = PeriodicNext(shape,face,derivative);
					const std::size_t previousRight = PeriodicPrevious(shape,right,derivative);
					const std::size_t nextRight = PeriodicNext(shape,right,derivative);
					viscous += 0.25*(stress[component][derivative][next]+
						stress[component][derivative][nextRight]-
						stress[component][derivative][previous]-
						stress[component][derivative][previousRight])/shape.cellWidthM;
				}
				const double gasDensity = 0.5*(FromConservativeVector(state[face]).GasDensity()+
					FromConservativeVector(state[right]).GasDensity());
				double phaseLeft = 0.0, phaseRight = 0.0;
				for( std::size_t species=0; species<MethaneCarbon; ++species ) {
					phaseLeft += frozenSourceDelta[face][1+species]/deltaTimeS;
					phaseRight += frozenSourceDelta[right][1+species]/deltaTimeS;
				}
				result.component[component][face] = viscous+
					(gasDensity-ambientGasDensityKGPerM3)*gravityMPerS2[component]+
					faceVelocity.component[component][face]*0.5*(phaseLeft+phaseRight);
				if( !std::isfinite(result.component[component][face]) ) return Fail(error,
					"fire solver 3-D nonpressure momentum RHS overflowed");
			}
			return true;
		}

		struct OpenFluxPair3D
		{
			std::array<std::vector<ConservativeVector>,3> low,high;
			std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3>
				nonadvectiveMass;
			std::array<std::vector<double>,3> nonadvectiveEnergy;
			std::array<bool,MethaneMassStateDimension> boundaryCanSupply;
			OpenFluxPair3D(){boundaryCanSupply.fill(false);}
		};

		inline std::size_t OpenUpperFaceForCell3D(
			const PeriodicMACShape& shape,
			const std::size_t cell,
			const unsigned int axis
			)
		{
			const std::size_t x=cell%shape.nx;
			const std::size_t y=(cell/shape.nx)%shape.ny;
			const std::size_t z=cell/(shape.nx*shape.ny);
			return OpenMACFaceIndex3D(shape,axis,x+(axis==0),y+(axis==1),z+(axis==2));
		}

		inline std::size_t OpenLowerFaceForCell3D(
			const PeriodicMACShape& shape,
			const std::size_t cell,
			const unsigned int axis
			)
		{
			const std::size_t x=cell%shape.nx;
			const std::size_t y=(cell/shape.nx)%shape.ny;
			const std::size_t z=cell/(shape.nx*shape.ny);
			return OpenMACFaceIndex3D(shape,axis,x,y,z);
		}

		inline bool BuildOpenStageTransportEvaluations3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const OpenMACField3D& faceVelocity,
			const OpenBoundaryConfig3D& boundary,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<CellTransportEvaluation>& evaluations,
			std::string* error=0,
			const unsigned int workerCount=1u
			)
		{
			const std::size_t count=shape.CellCount();
			if(!ValidateOpenBoundaryConfig3D(shape,boundary,error)||state.size()!=count ||
				temperatureK.size()!=count) return Fail(error,
				"fire solver open stage transport shape is invalid");
			std::array<std::vector<double>,3> cellVelocity;
			for(unsigned int component=0;component<3;++component){
				if(faceVelocity.component[component].size()!=OpenMACFaceCount3D(shape,component))
					return Fail(error,"fire solver open stage velocity shape is invalid");
				cellVelocity[component].assign(count,0.0);
				for(std::size_t cell=0;cell<count;++cell) cellVelocity[component][cell]=0.5*(
					faceVelocity.component[component][OpenLowerFaceForCell3D(shape,cell,component)]+
					faceVelocity.component[component][OpenUpperFaceForCell3D(shape,cell,component)]);
			}
			evaluations.assign(count,CellTransportEvaluation());
			const double widths[3]={shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
			auto boundaryKind=[&](const unsigned int side,const std::size_t x,
				const std::size_t y,const std::size_t z){
				const std::size_t first=side<2?y:x,second=side<4?z:y;
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
				unsigned int kind=boundary.kind[side];
				if(side==4&&!boundary.bottomFuelMask.empty()&&boundary.bottomFuelMask[index])
					kind=FuelInletBoundary3D;
				return kind;
			};
			auto boundaryVelocity=[&](const unsigned int side,const unsigned int component,
				const std::size_t x,const std::size_t y,const std::size_t z){
				const unsigned int normal=side/2;
				if(component!=normal)return 0.0;
				std::size_t fx=x,fy=y,fz=z;
				if(normal==0)fx=(side&1)?shape.nx:0;
				if(normal==1)fy=(side&1)?shape.ny:0;
				if(normal==2)fz=(side&1)?shape.nz:0;
				return faceVelocity.component[component][OpenMACFaceIndex3D(shape,component,fx,fy,fz)];
			};
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell){
				MethaneCellState physical=FromConservativeVector(state[cell]);
				physical.temperatureK=temperatureK[cell];
				double gradient[3][3]={};
				for(unsigned int derivative=0;derivative<3;++derivative){
					const std::size_t x=cell%shape.nx;
					const std::size_t y=(cell/shape.nx)%shape.ny;
					const std::size_t z=cell/(shape.nx*shape.ny);
					const std::size_t coordinate=derivative==0?x:(derivative==1?y:z);
					const std::size_t extent=derivative==0?shape.nx:(derivative==1?shape.ny:shape.nz);
					const std::size_t previous=coordinate?PeriodicPrevious(shape,cell,derivative):cell;
					const std::size_t next=coordinate+1<extent?PeriodicNext(shape,cell,derivative):cell;
					for(unsigned int component=0;component<3;++component){
						double previousValue=cellVelocity[component][previous],
							nextValue=cellVelocity[component][next];
						if(coordinate==0){const unsigned int side=2*derivative;
							if(boundaryKind(side,x,y,z)==PressureOpenBoundary3D)
								previousValue=cellVelocity[component][cell];
							else previousValue=2.0*boundaryVelocity(side,component,x,y,z)-
								cellVelocity[component][cell];}
						if(coordinate+1==extent){const unsigned int side=2*derivative+1;
							if(boundaryKind(side,x,y,z)==PressureOpenBoundary3D)
								nextValue=cellVelocity[component][cell];
							else nextValue=2.0*boundaryVelocity(side,component,x,y,z)-
								cellVelocity[component][cell];}
						gradient[derivative][component]=(nextValue-previousValue)/
							(2.0*shape.cellWidthM);
					}
				}
				std::string cellError;
				if(!EvaluateCellTransport(physical,gradient,widths,dns,thermochemistry,
					transport,evaluations[cell],&cellError)){
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				}
			});
			for(std::thread& thread:threads)thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			return true;
		}

		inline bool BuildOpenStageTransport3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const OpenMACField3D& faceVelocity,
			const OpenBoundaryConfig3D& boundary,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<double>& diffusivity,
			std::vector<double>& conductivity,
			std::vector<double>& viscosity,
			std::string* error=0,
			const unsigned int workerCount=1u
			)
		{
			std::vector<CellTransportEvaluation> evaluations;
			if(!BuildOpenStageTransportEvaluations3D(shape,state,temperatureK,faceVelocity,
				boundary,dns,thermochemistry,transport,evaluations,error,workerCount)) return false;
			diffusivity.resize(evaluations.size());
			conductivity.resize(evaluations.size());
			viscosity.resize(evaluations.size());
			for(std::size_t cell=0;cell<evaluations.size();++cell) {
				diffusivity[cell]=evaluations[cell].totalDiffusivityM2PerS;
				conductivity[cell]=evaluations[cell].effectiveConductivityWPerMK;
				viscosity[cell]=evaluations[cell].effectiveViscosityPaS;
			}
			return true;
		}

		inline bool BuildOpenFluxPair3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const OpenMACProjection3DResult& projection,
			const std::vector<double>& diffusivity,
			const std::vector<double>& conductivity,
			const OpenBoundaryConfig3D& boundary,
			const double ambientTemperatureK,
			const double injectedTemperatureK,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			OpenFluxPair3D& result,
			std::string* error=0,
			const unsigned int workerCount=1u
		)
		{
			result.boundaryCanSupply.fill(false);
			if(!ValidateOpenBoundaryConfig3D(shape,boundary,error) || state.size()!=shape.CellCount() ||
				temperatureK.size()!=shape.CellCount() || diffusivity.size()!=shape.CellCount() ||
				conductivity.size()!=shape.CellCount() || !std::isfinite(ambientTemperatureK) ||
				ambientTemperatureK<=0.0 || !std::isfinite(injectedTemperatureK) ||
				injectedTemperatureK<=0.0) return Fail(error,
				"fire solver open flux-pair input is malformed");
			for(unsigned int axis=0;axis<3;++axis)if(projection.velocityMPerS.component[axis].size()!=
				OpenMACFaceCount3D(shape,axis)) return Fail(error,
				"fire solver open flux-pair velocity shape is invalid");
			for(unsigned int side=0;side<6;++side)if(projection.inflow[side].size()!=
				OpenBoundaryFaceCount3D(shape,side)) return Fail(error,
				"fire solver open flux-pair active-set shape is invalid");
			const std::size_t count=shape.CellCount();
			auto boundaryGhost=[&](const std::size_t cell,const unsigned int axis,
				const bool positive)->const ConservativeVector& {
				const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
					z=cell/(shape.nx*shape.ny);
				const unsigned int side=2*axis+(positive?1u:0u);
				const std::size_t first=axis==0?y:x;
				const std::size_t second=axis==2?y:z;
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
				unsigned int kind=boundary.kind[side];
				if(side==4&&!boundary.bottomFuelMask.empty()&&boundary.bottomFuelMask[index])
					kind=FuelInletBoundary3D;
				if(kind==FuelInletBoundary3D) return boundary.injectedState;
				if(kind==PressureOpenBoundary3D&&projection.inflow[side][index])
					return boundary.ambientState;
				return state[cell];
			};
			PeriodicMACField periodicVelocity;
			for(unsigned int axis=0;axis<3;++axis){
				periodicVelocity.component[axis].resize(count);
				for(std::size_t cell=0;cell<count;++cell) periodicVelocity.component[axis][cell]=
					projection.velocityMPerS.component[axis][OpenUpperFaceForCell3D(shape,cell,axis)];
			}
			PeriodicFluxPair3D interior;
			if(!BuildPeriodicFluxPair3D(shape,state,temperatureK,periodicVelocity,diffusivity,
				conductivity,fuel,thermochemistry,interior,error,workerCount,false)) return false;
			// Replace periodic reconstruction coordinates with boundary-aware MC
			// coordinates.  Ambient/injected ghosts are physical states; outflow and
			// adiabatic ghosts are the adjacent interior state (zero normal slope).
			const FireCertifiedNullspace& reconstruction=fuel.ConservativeReconstruction();
			std::vector<double> coordinate(count*reconstruction.nullity,0.0);
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			std::vector<std::thread> slopeThreads;
			for(unsigned int worker=0;worker<workers;++worker)slopeThreads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell)for(std::size_t basis=0;
					basis<reconstruction.nullity;++basis)for(std::size_t row=0;
					row<MethaneMassStateDimension;++row) coordinate[cell*reconstruction.nullity+basis]+=
						reconstruction.orthonormalBasis[row*reconstruction.nullity+basis]*state[cell][row];
			});
			for(std::thread& thread:slopeThreads)thread.join();slopeThreads.clear();
			std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3> openSlope;
			std::array<std::vector<double>,3> openEnergySlope;
			for(unsigned int axis=0;axis<3;++axis){
				openSlope[axis].assign(count,std::array<double,MethaneMassStateDimension>());
				openEnergySlope[axis].assign(count,0.0);
			}
			const std::size_t slopeWorkCount=3u*count;
			for(unsigned int worker=0;worker<workers;++worker)slopeThreads.emplace_back([&,worker](){
				const std::size_t first=slopeWorkCount*worker/workers;
				const std::size_t last=slopeWorkCount*(worker+1u)/workers;
				for(std::size_t work=first;work<last;++work){
					const unsigned int axis=static_cast<unsigned int>(work/count);
					const std::size_t cell=work%count;
					const std::size_t extent=axis==0?shape.nx:(axis==1?shape.ny:shape.nz);
					openSlope[axis][cell].fill(0.0);
					const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
						z=cell/(shape.nx*shape.ny);
					const std::size_t position=axis==0?x:(axis==1?y:z);
					const ConservativeVector& previousState=position?state[PeriodicPrevious(
						shape,cell,axis)]:boundaryGhost(cell,axis,false);
					const ConservativeVector& nextState=position+1<extent?state[PeriodicNext(
						shape,cell,axis)]:boundaryGhost(cell,axis,true);
					bool uniformStencil=true;
					for(std::size_t row=0;row<MethaneConservativeDimension;++row)
						uniformStencil=uniformStencil&&previousState[row]==state[cell][row]&&
							nextState[row]==state[cell][row];
					if(uniformStencil)continue;
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						double previousCoordinate=0.0,nextCoordinate=0.0;
						for(std::size_t row=0;row<MethaneMassStateDimension;++row){
							const double coefficient=reconstruction.orthonormalBasis[
								row*reconstruction.nullity+basis];
							previousCoordinate+=coefficient*previousState[row];
							nextCoordinate+=coefficient*nextState[row];
						}
						const double slope=MCScalarSlope(
							coordinate[cell*reconstruction.nullity+basis]-previousCoordinate,
							nextCoordinate-coordinate[cell*reconstruction.nullity+basis]);
						for(std::size_t row=0;row<MethaneMassStateDimension;++row)
							openSlope[axis][cell][row]+=reconstruction.orthonormalBasis[
								row*reconstruction.nullity+basis]*slope;
					}
					openEnergySlope[axis][cell]=MCScalarSlope(state[cell][MethaneMassStateDimension]-
						previousState[MethaneMassStateDimension],nextState[MethaneMassStateDimension]-
						state[cell][MethaneMassStateDimension]);
				}
			});
			for(std::thread& thread:slopeThreads)thread.join();slopeThreads.clear();
			std::array<bool,MethaneMassStateDimension> absentFromDomain;
			absentFromDomain.fill(true);
			for(const ConservativeVector& cell:state)for(std::size_t row=0;
				row<MethaneMassStateDimension;++row)absentFromDomain[row]=
					absentFromDomain[row]&&cell[row]==0.0;
			bool ambientCanSupply=false,fuelCanSupply=false;
			for(unsigned int side=0;side<6;++side)ambientCanSupply=ambientCanSupply||
				boundary.kind[side]==PressureOpenBoundary3D;
			for(const bool isFuel:boundary.bottomFuelMask)fuelCanSupply=fuelCanSupply||isFuel;
			for(std::size_t row=0;row<MethaneMassStateDimension;++row)
				absentFromDomain[row]=absentFromDomain[row]&&
					(!ambientCanSupply||boundary.ambientState[row]==0.0)&&
					(!fuelCanSupply||boundary.injectedState[row]==0.0);
			std::vector<std::vector<double> > openAbsentNormals;
			for(std::size_t row=0;row<MethaneMassStateDimension;++row)if(absentFromDomain[row]){
				std::vector<double> normal(reconstruction.nullity);
				for(std::size_t basis=0;basis<reconstruction.nullity;++basis)normal[basis]=
					reconstruction.orthonormalBasis[row*reconstruction.nullity+basis];
				for(const std::vector<double>& prior:openAbsentNormals){
					double dot=0.0;
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						dot+=normal[basis]*prior[basis];
					}
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						normal[basis]-=dot*prior[basis];
					}
				}
				double norm=0.0;for(const double value:normal)norm+=value*value;
				if(norm>256.0*std::numeric_limits<double>::epsilon()){norm=std::sqrt(norm);
					for(double& value:normal)value/=norm;openAbsentNormals.push_back(normal);}}
			for(unsigned int worker=0;worker<workers;++worker)slopeThreads.emplace_back([&,worker](){
				std::vector<double> openCoordinateSlope(reconstruction.nullity,0.0);
				const std::size_t first=slopeWorkCount*worker/workers;
				const std::size_t last=slopeWorkCount*(worker+1u)/workers;
				for(std::size_t work=first;work<last;++work){
				const unsigned int axis=static_cast<unsigned int>(work/count);
				const std::size_t cell=work%count;
				std::fill(openCoordinateSlope.begin(),openCoordinateSlope.end(),0.0);
				for(std::size_t basis=0;basis<reconstruction.nullity;++basis)for(std::size_t row=0;
					row<MethaneMassStateDimension;++row)openCoordinateSlope[basis]+=
						reconstruction.orthonormalBasis[row*reconstruction.nullity+basis]*
						openSlope[axis][cell][row];
				for(const std::vector<double>& normal:openAbsentNormals){
					double dot=0.0;
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						dot+=openCoordinateSlope[basis]*normal[basis];
					}
					for(std::size_t basis=0;basis<reconstruction.nullity;++basis){
						openCoordinateSlope[basis]-=dot*normal[basis];
					}
				}
				openSlope[axis][cell].fill(0.0);for(std::size_t basis=0;basis<reconstruction.nullity;
					++basis)for(std::size_t row=0;row<MethaneMassStateDimension;++row)
						openSlope[axis][cell][row]+=reconstruction.orthonormalBasis[
							row*reconstruction.nullity+basis]*openCoordinateSlope[basis];
				for(std::size_t row=0;row<MethaneMassStateDimension;++row)if(absentFromDomain[row])
					openSlope[axis][cell][row]=0.0;
				}
			});
			for(std::thread& thread:slopeThreads)thread.join();slopeThreads.clear();
			std::vector<double> rhoDiffusivity(count,0.0);
			for(std::size_t cell=0;cell<count;++cell){
				double total=0.0;for(std::size_t species=0;species<MethaneSpeciesCount;++species)
					total+=state[cell][1+species];
				rhoDiffusivity[cell]=total*diffusivity[cell];
			}
			OpenBoundaryFluxField3D boundaryFlux;
			if(!BuildOpenBoundaryFluxField3D(shape,state,temperatureK,rhoDiffusivity,
				conductivity,boundary,projection,ambientTemperatureK,injectedTemperatureK,
				fuel,thermochemistry,boundaryFlux,error)) return false;
			bool hasFuelBoundary=false;
			for(const bool active:boundary.bottomFuelMask)hasFuelBoundary=hasFuelBoundary||active;
			for(std::size_t component=0;component<MethaneMassStateDimension;++component){
				for(unsigned int side=0;side<6;++side)if(
					boundary.kind[side]==PressureOpenBoundary3D)
					result.boundaryCanSupply[component]=result.boundaryCanSupply[component]||
						boundary.ambientState[component]!=0.0;
				if(hasFuelBoundary)result.boundaryCanSupply[component]=
					result.boundaryCanSupply[component]||boundary.injectedState[component]!=0.0;
			}
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				result.low[axis].assign(faceCount,ConservativeVector());
				result.high[axis].assign(faceCount,ConservativeVector());
				result.nonadvectiveMass[axis].assign(faceCount,
					std::array<double,MethaneMassStateDimension>());
				result.nonadvectiveEnergy[axis].assign(faceCount,0.0);
				const std::size_t normalExtent=axis==0?shape.nx:(axis==1?shape.ny:shape.nz);
				for(std::size_t cell=0;cell<count;++cell){
					const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
						z=cell/(shape.nx*shape.ny);
					const std::size_t coordinate=axis==0?x:(axis==1?y:z);
					if(coordinate+1<normalExtent){
						const std::size_t face=OpenUpperFaceForCell3D(shape,cell,axis);
						result.low[axis][face]=interior.low[axis][cell];
						const std::size_t right=PeriodicNext(shape,cell,axis);
						const double velocity=projection.velocityMPerS.component[axis][face];
						for(std::size_t component=0;component<MethaneMassStateDimension;++component){
							const double highValue=velocity>=0.0?state[cell][component]+
								0.5*openSlope[axis][cell][component]:state[right][component]-
								0.5*openSlope[axis][right][component];
							result.high[axis][face][component]=velocity*highValue+
								interior.nonadvectiveMass[axis][cell][component];
						}
						const double highEnergy=velocity>=0.0?state[cell][MethaneMassStateDimension]+
							0.5*openEnergySlope[axis][cell]:state[right][MethaneMassStateDimension]-
							0.5*openEnergySlope[axis][right];
						result.high[axis][face][MethaneMassStateDimension]=velocity*highEnergy+
							interior.nonadvectiveEnergy[axis][cell];
						result.nonadvectiveMass[axis][face]=interior.nonadvectiveMass[axis][cell];
						result.nonadvectiveEnergy[axis][face]=interior.nonadvectiveEnergy[axis][cell];
					}
				}
			}
			for(unsigned int side=0;side<6;++side){
				const unsigned int axis=side/2;const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				for(std::size_t second=0;second<secondCount;++second) for(std::size_t first=0;
					first<firstCount;++first){
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
					const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
					const double orientation=positive?1.0:-1.0;
					result.low[axis][face]=orientation*boundaryFlux.side[side][index].totalOutwardFlux;
					result.high[axis][face]=result.low[axis][face];
					for(std::size_t component=0;component<MethaneMassStateDimension;++component)
						result.nonadvectiveMass[axis][face][component]=orientation*
							boundaryFlux.side[side][index].nonadvectiveMassOutwardFlux[component];
					result.nonadvectiveEnergy[axis][face]=orientation*
						boundaryFlux.side[side][index].nonadvectiveEnergyOutwardFlux;
				}
			}
			return true;
		}

		inline bool ApplyOpenSharedFCT3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const OpenFluxPair3D& flux,
			const std::vector<ConservativeVector>& sourceDelta,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::array<std::vector<double>,3>& alpha,
			std::string* error=0,
			const unsigned int workerCount=1u,
			const std::array<std::vector<double>,3>* acceptedAlpha=0
			)
		{
			if(!ValidatePeriodicShape3D(shape,error))return false;
			const std::size_t count=shape.CellCount();
			if(beginning.size()!=count || sourceDelta.size()!=count ||
				!std::isfinite(config.deltaTimeS) || config.deltaTimeS<=0.0 ||
				!std::isfinite(shape.cellWidthM) || shape.cellWidthM<=0.0) return Fail(error,
				"fire solver open FCT state shape is invalid");
			for(unsigned int axis=0;axis<3;++axis){const std::size_t faceCount=
				OpenMACFaceCount3D(shape,axis);if(flux.low[axis].size()!=faceCount ||
				flux.high[axis].size()!=faceCount || flux.nonadvectiveMass[axis].size()!=faceCount ||
				flux.nonadvectiveEnergy[axis].size()!=faceCount) return Fail(error,
					"fire solver open FCT face shape is invalid");}
			std::array<double,MethaneSpeciesCount> ambientEnthalpy,adiabaticEnthalpy;
			if(!FireSimulationEnthalpyBounds(config,thermochemistry,ambientEnthalpy,
				adiabaticEnthalpy,error)) return false;
			const double scale=config.deltaTimeS/shape.cellWidthM;
			std::vector<ConservativeVector> low(count);
			std::array<std::vector<ConservativeVector>,6> correction;
			for(unsigned int direction=0;direction<6;++direction)
				correction[direction].assign(count,ConservativeVector());
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell){
					std::string localError;
					if(!AcceptedStateAdmissible(beginning[cell],ambientEnthalpy,
						adiabaticEnthalpy,fuel,&localError)){failureCell[worker]=cell;
						failureMessage[worker]=localError;break;}
					low[cell]=beginning[cell]+sourceDelta[cell];
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					low[cell]=low[cell]+scale*(flux.low[axis][lower]-flux.low[axis][upper]);
					correction[2*axis][cell]=scale*(flux.high[axis][lower]-flux.low[axis][lower]);
					correction[2*axis+1][cell]=-scale*(flux.high[axis][upper]-flux.low[axis][upper]);
				}
				if(!AcceptedStateAdmissible(low[cell],ambientEnthalpy,adiabaticEnthalpy,
					fuel,&localError)){std::ostringstream message;message << localError <<
					": cell=" << cell << " values=";
					for(std::size_t component=0;component<MethaneConservativeDimension;++component)
						message << (component?",":"") << low[cell][component];
					failureCell[worker]=cell;failureMessage[worker]=message.str();break;}
				}
			});
			for(std::thread& thread:threads)thread.join();threads.clear();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			const std::size_t inequalityCount=4+MethaneSpeciesCount;
			std::vector<double> ratio(count*inequalityCount,1.0);
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell){
				std::array<ConservativeVector,6> limiterCorrection={};
				for(unsigned int direction=0;direction<6;++direction)
					limiterCorrection[direction]=correction[direction][cell];
				for(std::size_t inequality=0;inequality<inequalityCount;++inequality){
				// Match the periodic operator's certified outward-rounding reserve.  An
				// exact-zero budget would let a physically inactive fp64 MC trace force
				// alpha from one to zero and prevent the R0 Picard fixed point even though
				// the accepted correction is many orders below the state envelope.
				const double scaleLowerBound=CertifiedLimiterScaleLowerBound(low[cell],
					limiterCorrection,6,inequality,ambientEnthalpy,adiabaticEnthalpy);
				const double budget=CertifiedLimiterInequalityBudget(low[cell],inequality,
					ambientEnthalpy,adiabaticEnthalpy,fuel,scaleLowerBound);
				double requested=0.0;for(unsigned int direction=0;direction<6;++direction)
					requested+=std::max(0.0,InequalityValue(correction[direction][cell],
						inequality,ambientEnthalpy,adiabaticEnthalpy));
				ratio[cell*inequalityCount+inequality]=requested>0.0?
					std::min(1.0,budget/requested):1.0;
			}}
			});
			for(std::thread& thread:threads)thread.join();threads.clear();
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				alpha[axis].assign(faceCount,1.0);
			}
			const std::size_t faceWorkCount=3u*count;
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=faceWorkCount*worker/workers;
				const std::size_t last=faceWorkCount*(worker+1u)/workers;
				for(std::size_t work=first;work<last;++work){
					const unsigned int axis=static_cast<unsigned int>(work/count);
					const std::size_t cell=work%count;
					const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
						z=cell/(shape.nx*shape.ny);
					const std::size_t coordinate=axis==0?x:(axis==1?y:z);
					const std::size_t extent=axis==0?shape.nx:(axis==1?shape.ny:shape.nz);
					if(coordinate+1>=extent) continue;
					const std::size_t face=OpenUpperFaceForCell3D(shape,cell,axis);
					const std::size_t right=PeriodicNext(shape,cell,axis);
					for(std::size_t inequality=0;inequality<inequalityCount;++inequality){
						if(InequalityValue(correction[2*axis+1][cell],inequality,
							ambientEnthalpy,adiabaticEnthalpy)>0.0) alpha[axis][face]=
							std::min(alpha[axis][face],ratio[cell*inequalityCount+inequality]);
						if(InequalityValue(correction[2*axis][right],inequality,
							ambientEnthalpy,adiabaticEnthalpy)>0.0) alpha[axis][face]=
							std::min(alpha[axis][face],ratio[right*inequalityCount+inequality]);
					}
				}
			});
			for(std::thread& thread:threads)thread.join();threads.clear();
			if(acceptedAlpha){
				for(unsigned int axis=0;axis<3;++axis){
					if((*acceptedAlpha)[axis].size()!=alpha[axis].size()) return Fail(error,
						"fire solver open accepted limiter shape is invalid");
					for(std::size_t face=0;face<alpha[axis].size();++face){
						const double accepted=(*acceptedAlpha)[axis][face];
						if(!std::isfinite(accepted)||accepted<0.0||accepted>alpha[axis][face])
							return Fail(error,"fire solver open accepted limiter exceeds its certificate");
					}
					alpha[axis]=(*acceptedAlpha)[axis];
				}
			}
			std::vector<ConservativeVector> candidate=low;
			for(unsigned int worker=0;worker<workers;++worker)threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell)for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					candidate[cell]=candidate[cell]+alpha[axis][lower]*correction[2*axis][cell];
					candidate[cell]=candidate[cell]+alpha[axis][upper]*correction[2*axis+1][cell];
				}
			});
			for(std::thread& thread:threads)thread.join();threads.clear();
			for(std::size_t cell=0;cell<count;++cell){
				if(!AcceptedStateAdmissible(candidate[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,error))return false;
			}
			result.swap(candidate);return true;
		}

		inline bool OpenDivergenceTargetFromPhysicalFlux3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperature,
			const OpenFluxPair3D& flux,
			const std::vector<ConservativeVector>& sourceDelta,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& result,
			std::string* error=0,
			const unsigned int workerCount=1u,
			const std::vector<double>* projectionEnergyDeltaJPerM3=0,
			const std::vector<double>* projectionExpansionIntegral=0
			)
		{
			const std::size_t count=shape.CellCount();
			if(state.size()!=count||temperature.size()!=count||sourceDelta.size()!=count||
				!std::isfinite(deltaTimeS)||deltaTimeS<=0.0) return Fail(error,
					"fire solver open divergence target arrays are malformed");
			if((projectionEnergyDeltaJPerM3==0)!=(projectionExpansionIntegral==0)||
				(projectionEnergyDeltaJPerM3&&(projectionEnergyDeltaJPerM3->size()!=count||
					projectionExpansionIntegral->size()!=count)))return Fail(error,
					"fire solver open projection-source pair is malformed");
			for(unsigned int axis=0;axis<3;++axis) if(flux.nonadvectiveMass[axis].size()!=
				OpenMACFaceCount3D(shape,axis)||flux.nonadvectiveEnergy[axis].size()!=
				OpenMACFaceCount3D(shape,axis)) return Fail(error,
					"fire solver open divergence physical flux shape is invalid");
			std::vector<double> candidate(count,0.0);
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(count)));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker) threads.emplace_back([&,worker](){
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell){
					ConservativeVector increment=sourceDelta[cell];
					if(projectionEnergyDeltaJPerM3)
						increment[MethaneMassStateDimension]-=(*projectionEnergyDeltaJPerM3)[cell];
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					for(std::size_t component=0;component<MethaneMassStateDimension;++component)
						increment[component]+=deltaTimeS*(flux.nonadvectiveMass[axis][lower][component]-
							flux.nonadvectiveMass[axis][upper][component])/shape.cellWidthM;
					increment[MethaneMassStateDimension]+=deltaTimeS*(flux.nonadvectiveEnergy[axis][lower]-
						flux.nonadvectiveEnergy[axis][upper])/shape.cellWidthM;
				}
				std::string cellError;
				if(!DivergenceFromDiscreteIncrement(state[cell],increment,temperature[cell],
					deltaTimeS,thermochemistry,candidate[cell],&cellError)){
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				if(projectionExpansionIntegral)candidate[cell]+=
					(*projectionExpansionIntegral)[cell]/deltaTimeS;
				}
			});
			for(std::thread& thread:threads)thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			result.swap(candidate);
			return true;
		}

		inline OpenMACField3D OpenCompatibleMomentumFluxDivergence3D(
			const PeriodicMACShape& shape,
			const OpenFluxPair3D& flux,
			const std::array<std::vector<double>,3>& alpha,
			const OpenMACField3D& velocity,
			const OpenBoundaryConfig3D* boundary=0
			)
		{
			std::array<std::vector<double>,3> gasFlux;
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				gasFlux[axis].assign(faceCount,0.0);
				for(std::size_t face=0;face<faceCount;++face){
					double low=0.0,high=0.0,physical=0.0;
					for(std::size_t species=0;species<MethaneCarbon;++species){
						physical+=flux.nonadvectiveMass[axis][face][1+species];
						low+=flux.low[axis][face][1+species]-
							flux.nonadvectiveMass[axis][face][1+species];
						high+=flux.high[axis][face][1+species]-
							flux.nonadvectiveMass[axis][face][1+species];
					}
					gasFlux[axis][face]=low+alpha[axis][face]*(high-low)+physical;
				}
			}
			OpenMACField3D result;
			for(unsigned int component=0;component<3;++component){
				result.component[component].assign(OpenMACFaceCount3D(shape,component),0.0);
				const std::size_t normalCount=component==0?shape.nx+1:
					(component==1?shape.ny+1:shape.nz+1);
				const std::size_t firstCount=component==0?shape.ny:shape.nx;
				const std::size_t secondCount=component==2?shape.ny:shape.nz;
				for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;
					first<firstCount;++first)for(std::size_t normal=0;normal<normalCount;++normal){
					std::size_t x=0,y=0,z=0;if(component==0){x=normal;y=first;z=second;}
					if(component==1){x=first;y=normal;z=second;}
					if(component==2){x=first;y=second;z=normal;}
					const std::size_t componentFace=OpenMACFaceIndex3D(shape,component,x,y,z);
					double divergence=0.0;
					for(unsigned int derivative=0;derivative<3;++derivative){
						if(derivative==component){
							const std::size_t previousNormal=normal?normal-1:normal;
							const std::size_t nextNormal=normal+1<normalCount?normal+1:normal;
							std::size_t px=x,py=y,pz=z,nx=x,ny=y,nz=z;
							if(component==0){px=previousNormal;nx=nextNormal;}
							if(component==1){py=previousNormal;ny=nextNormal;}
							if(component==2){pz=previousNormal;nz=nextNormal;}
							const std::size_t previousFace=OpenMACFaceIndex3D(shape,component,px,py,pz);
							const std::size_t nextFace=OpenMACFaceIndex3D(shape,component,nx,ny,nz);
							const double upper=0.25*(gasFlux[component][componentFace]+
								gasFlux[component][nextFace])*(velocity.component[component][componentFace]+
								velocity.component[component][nextFace]);
							const double lower=0.25*(gasFlux[component][previousFace]+
								gasFlux[component][componentFace])*(velocity.component[component][previousFace]+
								velocity.component[component][componentFace]);
							const double normalScale=normal==0||normal+1==normalCount?2.0:1.0;
							divergence+=normalScale*(upper-lower)/shape.cellWidthM;
						}else{
							const std::size_t derivativeExtent=derivative==0?shape.nx:
								(derivative==1?shape.ny:shape.nz);
							const std::size_t derivativePosition=derivative==0?x:(derivative==1?y:z);
							const std::size_t lowerBoundary=derivativePosition;
							const std::size_t upperBoundary=derivativePosition+1;
							const std::size_t componentExtent=component==0?shape.nx:
								(component==1?shape.ny:shape.nz);
							const std::size_t componentLower=normal?normal-1:0;
							const std::size_t componentUpper=normal<componentExtent?normal:componentExtent-1;
							auto derivativeFlux=[&](const std::size_t componentCell,
								const std::size_t derivativeBoundary){
								std::size_t fx=x,fy=y,fz=z;
								if(component==0)fx=componentCell;if(component==1)fy=componentCell;
								if(component==2)fz=componentCell;
								if(derivative==0)fx=derivativeBoundary;
								if(derivative==1)fy=derivativeBoundary;
								if(derivative==2)fz=derivativeBoundary;
								return gasFlux[derivative][OpenMACFaceIndex3D(shape,derivative,fx,fy,fz)];
							};
							auto shiftedVelocity=[&](const bool upper){
								std::size_t vx=x,vy=y,vz=z;
								const std::size_t shifted=upper?std::min(derivativePosition+1,
									derivativeExtent-1):(derivativePosition?derivativePosition-1:0);
								if(derivative==0)vx=shifted;if(derivative==1)vy=shifted;
								if(derivative==2)vz=shifted;
								return velocity.component[component][OpenMACFaceIndex3D(shape,component,vx,vy,vz)];
							};
							auto prescribedDerivativeBoundary=[&](const unsigned int side){
								if(!boundary)return false;
								const std::size_t cellX=std::min(x,shape.nx-1);
								const std::size_t cellY=std::min(y,shape.ny-1);
								const std::size_t cellZ=std::min(z,shape.nz-1);
								const std::size_t first=side<2?cellY:cellX;
								const std::size_t second=side<4?cellZ:cellY;
								const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
								unsigned int kind=boundary->kind[side];
								if(side==4&&!boundary->bottomFuelMask.empty()&&
									boundary->bottomFuelMask[index])kind=FuelInletBoundary3D;
								return kind!=PressureOpenBoundary3D;
							};
							double upper=0.25*(derivativeFlux(componentLower,upperBoundary)+
								derivativeFlux(componentUpper,upperBoundary))*(
								velocity.component[component][componentFace]+shiftedVelocity(true));
							double lower=0.25*(derivativeFlux(componentLower,lowerBoundary)+
								derivativeFlux(componentUpper,lowerBoundary))*(shiftedVelocity(false)+
								velocity.component[component][componentFace]);
							// Wall and fuel-bed ghosts carry no tangential momentum.  The
							// corresponding dual-control-volume boundary product is zero;
							// pressure-open faces retain the interior tangential trace.
							if(derivativePosition==0&&prescribedDerivativeBoundary(2*derivative))
								lower=0.0;
							if(derivativePosition+1==derivativeExtent&&
								prescribedDerivativeBoundary(2*derivative+1))upper=0.0;
							divergence+=(upper-lower)/shape.cellWidthM;
						}
					}
					if(boundary){
						const bool lowerBoundary=normal==0,upperBoundary=normal+1==normalCount;
						if(lowerBoundary||upperBoundary){
							const unsigned int side=2*component+(upperBoundary?1u:0u);
							const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
							unsigned int kind=boundary->kind[side];
							if(side==4&&!boundary->bottomFuelMask.empty()&&
								boundary->bottomFuelMask[index])kind=FuelInletBoundary3D;
							if(kind!=PressureOpenBoundary3D)divergence=0.0;
						}
					}
					result.component[component][componentFace]=divergence;
				}
			}
			return result;
		}

		struct ConservativeAdvance3DConfig
		{
			PeriodicTransportConfig transport;
			std::array<double,3> gravityMPerS2;
			double projectionTolerancePerS;
			bool dns;
			bool periodicBoundaries;
			bool retainStageDiagnostics;
			unsigned int workerCount;
			OpenBoundaryConfig3D openBoundary;
			double injectedTemperatureK;
			ConservativeAdvance3DConfig() : projectionTolerancePerS(0.0),dns(false),
				periodicBoundaries(true),retainStageDiagnostics(false),workerCount(1u),
				injectedTemperatureK(0.0)
			{
				gravityMPerS2.fill(0.0);
			}
		};

		struct ConservativeStage3D
		{
			PeriodicFluxPair3D flux;
			PeriodicMACProjection3DResult projection;
			PeriodicMACField nonpressureMomentumRHS;
			std::array<std::vector<double>,3> faceAlpha;
			std::vector<double> divergenceTargetPerS;
			std::vector<double> picardResidualPerS;
			std::vector<double> diffusivityM2PerS;
			std::vector<double> conductivityWPerMK;
			std::vector<double> dynamicViscosityPaS;
			double maximumLimiterClassDiscrepancy=0.0;
			bool limiterDiscontinuousClass=false;
		};

		inline bool SolveConservativeStage3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const PeriodicMACField& unprojectedMomentum,
			const std::vector<ConservativeVector>& frozenSourceDelta,
			const ConservativeAdvance3DConfig& config,
			const bool solvePredictorLimiter,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			ConservativeStage3D& result,
			std::string* error = 0,
			const std::vector<double>* projectionEnergyDeltaJPerM3 = 0,
			const std::vector<double>* projectionExpansionIntegral = 0,
			const std::vector<ConservativeVector>* heunBeginning = 0,
			const PeriodicFluxPair3D* firstStageFlux = 0,
			const bool scalarAcceptanceStage = true
			)
		{
			const std::size_t count = shape.CellCount();
			std::vector<double> temperature;
			if( !InvertPeriodicTemperaturesWithinBounds(state,thermochemistry,
				config.transport.ambientTemperatureK,config.transport.adiabaticTemperatureK,
				temperature,error,config.workerCount) ) return false;
			std::vector<double> target(count,0.0), priorMassFlux(3*count,0.0);
			std::vector<double> priorDiffusivity(count,0.0),priorConductivity(count,0.0),
				priorViscosity(count,0.0);
			if((heunBeginning==0)!=(firstStageFlux==0) ||
				(heunBeginning&&heunBeginning->size()!=count))return Fail(error,
					"fire solver periodic manifold candidate context is malformed");
			auto acceptedCandidate=[&](const PeriodicFluxPair3D& secondFlux,
				std::vector<ConservativeVector>& candidate,
				std::array<std::vector<double>,3>& alpha)->bool{
				if(!heunBeginning)return ApplyPeriodicSharedFCT3D(shape,state,secondFlux,
					frozenSourceDelta,config.transport,fuel,thermochemistry,candidate,alpha,error);
				PeriodicFluxPair3D averaged;
				for(unsigned int axis=0;axis<3;++axis){
					averaged.low[axis].resize(count);averaged.high[axis].resize(count);
					averaged.nonadvectiveMass[axis].resize(count);
					averaged.nonadvectiveEnergy[axis].resize(count);
					for(std::size_t face=0;face<count;++face){
						averaged.low[axis][face]=0.5*(firstStageFlux->low[axis][face]+
							secondFlux.low[axis][face]);
						averaged.high[axis][face]=0.5*(firstStageFlux->high[axis][face]+
							secondFlux.high[axis][face]);
						averaged.nonadvectiveEnergy[axis][face]=0.5*(
							firstStageFlux->nonadvectiveEnergy[axis][face]+
							secondFlux.nonadvectiveEnergy[axis][face]);
						for(std::size_t component=0;component<MethaneMassStateDimension;++component)
							averaged.nonadvectiveMass[axis][face][component]=0.5*(
								firstStageFlux->nonadvectiveMass[axis][face][component]+
								secondFlux.nonadvectiveMass[axis][face][component]);
					}
				}
				return ApplyPeriodicSharedFCT3D(shape,*heunBeginning,averaged,
					frozenSourceDelta,config.transport,fuel,thermochemistry,candidate,alpha,error);
			};
			result.picardResidualPerS.clear();
			for( std::size_t iteration=0; iteration<kMaximumCoupledPicardIterations; ++iteration ) {
				PeriodicMACProjection3DResult projection;
				if( !ProjectPeriodicMACVelocity3D(shape,GasDensityFromConservative(state),
					unprojectedMomentum,target,config.transport.deltaTimeS,
					config.projectionTolerancePerS,projection,error) ) return false;
				std::vector<double> diffusivity, conductivity, viscosity;
				if( !BuildPeriodicStageTransport3D(shape,state,temperature,projection.velocityMPerS,
					config.dns,thermochemistry,transport,diffusivity,conductivity,viscosity,error) )
					return false;
				PeriodicFluxPair3D flux;
				if( !BuildPeriodicFluxPair3D(shape,state,temperature,projection.velocityMPerS,
					diffusivity,conductivity,fuel,thermochemistry,flux,error,
					config.workerCount) ) return false;
				std::array<std::vector<double>,3> nextAlpha;
				std::vector<ConservativeVector> stageCandidate;
				if(scalarAcceptanceStage&&!acceptedCandidate(flux,stageCandidate,nextAlpha))return false;
				std::vector<double> nextTarget;
				if(iteration==0u||!scalarAcceptanceStage){
					if( !PeriodicDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,flux,
						frozenSourceDelta,config.transport.deltaTimeS,thermochemistry,nextTarget,error,
						config.workerCount,projectionEnergyDeltaJPerM3,projectionExpansionIntegral) )
						return false;
				}else if(!ManifoldExactDivergenceTarget(target,stageCandidate,
					config.transport.deltaTimeS,thermochemistry,nextTarget,error,true))return false;
				double targetResidual = 0.0, massResidual = 0.0,
					coefficientResidual = 0.0;
				for( std::size_t cell=0; cell<count; ++cell ) {
					targetResidual = std::max(targetResidual,std::fabs(nextTarget[cell]-target[cell]));
					for( unsigned int axis=0; axis<3; ++axis ) {
						const double massFlux = projection.momentumKGPerM2S.component[axis][cell];
						if( iteration ) massResidual = std::max(massResidual,std::fabs(
							massFlux-priorMassFlux[axis*count+cell])/shape.cellWidthM);
						priorMassFlux[axis*count+cell] = massFlux;
					}
					if(iteration) coefficientResidual=std::max({coefficientResidual,
						std::fabs(diffusivity[cell]-priorDiffusivity[cell]),
						std::fabs(conductivity[cell]-priorConductivity[cell]),
						std::fabs(viscosity[cell]-priorViscosity[cell])});
					priorDiffusivity[cell]=diffusivity[cell];
					priorConductivity[cell]=conductivity[cell];
					priorViscosity[cell]=viscosity[cell];
				}
				result.picardResidualPerS.push_back(std::max({targetResidual,massResidual,
					coefficientResidual}));
				target.swap(nextTarget);
				if( targetResidual <= config.projectionTolerancePerS && iteration &&
					massResidual <= config.projectionTolerancePerS &&
					coefficientResidual <= config.projectionTolerancePerS ) {
					// Reproject and rebuild once at the accepted target so every returned
					// coefficient, flux and momentum is consistent with the returned velocity.
					if( !ProjectPeriodicMACVelocity3D(shape,GasDensityFromConservative(state),
						unprojectedMomentum,target,config.transport.deltaTimeS,
						config.projectionTolerancePerS,result.projection,error) ||
						!BuildPeriodicStageTransport3D(shape,state,temperature,
							result.projection.velocityMPerS,config.dns,thermochemistry,transport,
							result.diffusivityM2PerS,result.conductivityWPerMK,
							result.dynamicViscosityPaS,error) ) return false;
					if(!BuildPeriodicFluxPair3D(shape,state,temperature,
						result.projection.velocityMPerS,result.diffusivityM2PerS,
						result.conductivityWPerMK,fuel,thermochemistry,result.flux,error,
						config.workerCount))return false;
					std::vector<double> verifiedTarget;
					std::array<std::vector<double>,3> verifiedAlpha;
					std::vector<ConservativeVector> verifiedCandidate;
					if(scalarAcceptanceStage){
						if(!acceptedCandidate(result.flux,verifiedCandidate,verifiedAlpha)||
							!ManifoldExactDivergenceTarget(target,verifiedCandidate,
								config.transport.deltaTimeS,thermochemistry,verifiedTarget,error,true))
							return false;
					}else if(!PeriodicDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,
						result.flux,frozenSourceDelta,config.transport.deltaTimeS,thermochemistry,
						verifiedTarget,error,config.workerCount,projectionEnergyDeltaJPerM3,
						projectionExpansionIntegral))return false;
					std::vector<double> acceptedMass;
					acceptedMass.reserve(3u*count);
					for(unsigned int axis=0;axis<3;++axis)acceptedMass.insert(acceptedMass.end(),
						result.projection.momentumKGPerM2S.component[axis].begin(),
						result.projection.momentumKGPerM2S.component[axis].end());
					double finalResidual=0.0;
					if(!PicardContinuousVerificationResidual(verifiedTarget,target,acceptedMass,
						priorMassFlux,result.diffusivityM2PerS,diffusivity,
						result.conductivityWPerMK,conductivity,result.dynamicViscosityPaS,
						viscosity,shape.cellWidthM,finalResidual,error)) return false;
					if(finalResidual>config.projectionTolerancePerS){
						target.swap(verifiedTarget);
						continue;
					}
					LimiterPicardAcceptance3D limiterAcceptance;
					if(solvePredictorLimiter){
						if(!SelectLimiterPicardAcceptance3D(nextAlpha,verifiedAlpha,
							config.projectionTolerancePerS,limiterAcceptance,error)) return false;
						std::vector<ConservativeVector> certifiedPredictor;
						std::array<std::vector<double>,3> certifiedAlpha;
						if(!ApplyPeriodicSharedFCT3D(shape,state,result.flux,frozenSourceDelta,
							config.transport,fuel,thermochemistry,certifiedPredictor,certifiedAlpha,
							error,&limiterAcceptance.faceAlpha)) return false;
						std::vector<double> certifiedTarget;
						if(!ManifoldExactDivergenceTarget(target,certifiedPredictor,
							config.transport.deltaTimeS,thermochemistry,certifiedTarget,error,true))return false;
						double certifiedResidual=0.0;
						for(std::size_t cell=0;cell<count;++cell)certifiedResidual=std::max(
							certifiedResidual,std::fabs(certifiedTarget[cell]-target[cell]));
						if(certifiedResidual>config.projectionTolerancePerS){
							target.swap(certifiedTarget);continue;
						}
					}
					result.divergenceTargetPerS = verifiedTarget;
					result.faceAlpha = solvePredictorLimiter ? limiterAcceptance.faceAlpha : verifiedAlpha;
					result.maximumLimiterClassDiscrepancy=solvePredictorLimiter?
						limiterAcceptance.maximumFaceDiscrepancy:0.0;
					result.limiterDiscontinuousClass=solvePredictorLimiter&&
						limiterAcceptance.discontinuousClass;
					return RemainingMomentumRHS3D(shape,state,result.projection.velocityMPerS,
						result.dynamicViscosityPaS,frozenSourceDelta,
						config.transport.deltaTimeS,config.transport.ambientGasDensityKGPerM3,
						config.gravityMPerS2,result.nonpressureMomentumRHS,error);
				}
			}
			return Fail(error,"fire solver 3-D coupled Picard stage did not converge");
		}

		struct ConservativeAdvance3DResult
		{
			std::vector<ConservativeVector> conservative;
			PeriodicMACField momentumKGPerM2S;
			PeriodicMACField velocityMPerS;
			std::vector<double> stepAverageDynamicPressurePa;
			std::array<std::vector<double>,3> faceAlpha;
			std::vector<double> divergenceHeunPerS;
			double maximumDivergenceResidualPerS;
			double maximumBoundaryHeadResidualPa;
			unsigned int effectiveWorkerCount;
			double maximumLimiterClassDiscrepancy;
			unsigned int discontinuousLimiterClassCount;
			ConservativeStage3D r0, r1, r2;
			ConservativeAdvance3DResult() : maximumDivergenceResidualPerS(0.0),
				maximumBoundaryHeadResidualPa(0.0),effectiveWorkerCount(0u),
				maximumLimiterClassDiscrepancy(0.0),discontinuousLimiterClassCount(0u) {}
		};

		struct OpenConservativeStage3D
		{
			OpenFluxPair3D flux;
			OpenMACProjection3DResult projection;
			OpenMACField3D nonpressureMomentumRHS;
			std::array<std::vector<double>,3> faceAlpha;
			std::vector<double> divergenceTargetPerS;
			std::vector<double> picardResidualPerS;
			std::vector<double> diffusivityM2PerS,conductivityWPerMK,dynamicViscosityPaS;
			double maximumLimiterClassDiscrepancy=0.0;
			bool limiterDiscontinuousClass=false;
		};

		struct OpenConservativeAdvance3DResult
		{
			std::vector<ConservativeVector> conservative;
			OpenMACField3D momentumKGPerM2S,velocityMPerS;
			std::vector<double> stepAverageDynamicPressurePa;
			std::array<std::vector<double>,3> faceAlpha;
			std::vector<double> divergenceHeunPerS;
			double maximumLimiterClassDiscrepancy=0.0;
			unsigned int discontinuousLimiterClassCount=0u;
			OpenConservativeStage3D r0,r1,r2;
		};

		inline bool BuildOpenNonpressureMomentumRHS3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const OpenMACProjection3DResult& projection,
			const std::vector<double>& viscosity,
			const std::vector<ConservativeVector>& sourceDelta,
			const ConservativeAdvance3DConfig& config,
			OpenMACField3D& result,
			std::string* error=0
		)
		{
			const std::size_t expectedCellCount=shape.CellCount();
			if(state.size()!=expectedCellCount || viscosity.size()!=expectedCellCount ||
				sourceDelta.size()!=expectedCellCount || !std::isfinite(config.transport.deltaTimeS) ||
				config.transport.deltaTimeS<=0.0) return Fail(error,
				"fire solver open momentum input is malformed");
			for(unsigned int axis=0;axis<3;++axis)if(
				projection.velocityMPerS.component[axis].size()!=OpenMACFaceCount3D(shape,axis))
				return Fail(error,"fire solver open momentum velocity shape is invalid");
			if(!BuildRelativeBuoyancyMomentumRate3D(shape,GasDensityFromConservative(state),
				config.openBoundary,config.gravityMPerS2,result,error)) return false;
			const std::size_t cellCount=shape.CellCount();
			if(viscosity.size()!=cellCount) return Fail(error,
				"fire solver open momentum viscosity shape is invalid");
			std::array<std::vector<double>,3> cellVelocity;
			for(unsigned int component=0;component<3;++component){
				cellVelocity[component].assign(cellCount,0.0);
				for(std::size_t cell=0;cell<cellCount;++cell)cellVelocity[component][cell]=0.5*(
					projection.velocityMPerS.component[component][OpenLowerFaceForCell3D(shape,cell,component)]+
					projection.velocityMPerS.component[component][OpenUpperFaceForCell3D(shape,cell,component)]);
			}
			auto boundaryKind=[&](const unsigned int side,const std::size_t x,
				const std::size_t y,const std::size_t z){
				const std::size_t first=side<2?y:x;
				const std::size_t second=side<4?z:y;
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
				unsigned int kind=config.openBoundary.kind[side];
				if(side==4&&!config.openBoundary.bottomFuelMask.empty()&&
					config.openBoundary.bottomFuelMask[index])kind=FuelInletBoundary3D;
				return kind;
			};
			auto boundaryVelocity=[&](const unsigned int side,const unsigned int component,
				const std::size_t x,const std::size_t y,const std::size_t z){
				const unsigned int normal=side/2;
				if(component!=normal)return 0.0;
				std::size_t fx=x,fy=y,fz=z;
				if(normal==0)fx=(side&1)?shape.nx:0;
				if(normal==1)fy=(side&1)?shape.ny:0;
				if(normal==2)fz=(side&1)?shape.nz:0;
				return projection.velocityMPerS.component[component][
					OpenMACFaceIndex3D(shape,component,fx,fy,fz)];
			};
			std::array<std::array<std::vector<double>,3>,3> stress;
			for(unsigned int component=0;component<3;++component)for(unsigned int derivative=0;
				derivative<3;++derivative)stress[component][derivative].assign(cellCount,0.0);
			for(std::size_t cell=0;cell<cellCount;++cell){
				if(!std::isfinite(viscosity[cell])||viscosity[cell]<0.0)return Fail(error,
					"fire solver open momentum viscosity is invalid");
				const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,z=cell/(shape.nx*shape.ny);
				double gradient[3][3]={},divergence=0.0;
				for(unsigned int derivative=0;derivative<3;++derivative){
					const std::size_t position=derivative==0?x:(derivative==1?y:z);
					const std::size_t extent=derivative==0?shape.nx:(derivative==1?shape.ny:shape.nz);
					const std::size_t previous=position?PeriodicPrevious(shape,cell,derivative):cell;
					const std::size_t next=position+1<extent?PeriodicNext(shape,cell,derivative):cell;
					for(unsigned int component=0;component<3;++component){
						double previousValue=cellVelocity[component][previous];
						double nextValue=cellVelocity[component][next];
						if(position==0){
							const unsigned int side=2*derivative;
							if(boundaryKind(side,x,y,z)==PressureOpenBoundary3D)
								previousValue=cellVelocity[component][cell];
							else previousValue=2.0*boundaryVelocity(side,component,x,y,z)-
								cellVelocity[component][cell];
						}
						if(position+1==extent){
							const unsigned int side=2*derivative+1;
							if(boundaryKind(side,x,y,z)==PressureOpenBoundary3D)
								nextValue=cellVelocity[component][cell];
							else nextValue=2.0*boundaryVelocity(side,component,x,y,z)-
								cellVelocity[component][cell];
						}
						gradient[derivative][component]=(nextValue-previousValue)/
							(2.0*shape.cellWidthM);
					}
					divergence+=gradient[derivative][derivative];
				}
				for(unsigned int component=0;component<3;++component)for(unsigned int derivative=0;
					derivative<3;++derivative)stress[component][derivative][cell]=viscosity[cell]*(
					gradient[derivative][component]+gradient[component][derivative]-
					(component==derivative?(2.0/3.0)*divergence:0.0));
			}
			for(unsigned int component=0;component<3;++component){
				const std::size_t normalCount=component==0?shape.nx+1:(component==1?shape.ny+1:shape.nz+1);
				const std::size_t firstCount=component==0?shape.ny:shape.nx;
				const std::size_t secondCount=component==2?shape.ny:shape.nz;
				for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;
					first<firstCount;++first)for(std::size_t normal=1;normal+1<normalCount;++normal){
					std::size_t x=0,y=0,z=0;if(component==0){x=normal;y=first;z=second;}
					if(component==1){x=first;y=normal;z=second;}if(component==2){x=first;y=second;z=normal;}
					const std::size_t left=shape.Index(component==0?normal-1:x,
						component==1?normal-1:y,component==2?normal-1:z);
					const std::size_t right=shape.Index(component==0?normal:x,
						component==1?normal:y,component==2?normal:z);
					double viscous=(stress[component][component][right]-stress[component][component][left])/
						shape.cellWidthM;
					for(unsigned int derivative=0;derivative<3;++derivative){
						if(derivative==component)continue;
						const std::size_t leftPosition=derivative==0?x:(derivative==1?y:z);
						const std::size_t extent=derivative==0?shape.nx:(derivative==1?shape.ny:shape.nz);
						const std::size_t leftPrevious=leftPosition?PeriodicPrevious(shape,left,derivative):left;
						const std::size_t rightPrevious=leftPosition?PeriodicPrevious(shape,right,derivative):right;
						const std::size_t leftNext=leftPosition+1<extent?PeriodicNext(shape,left,derivative):left;
						const std::size_t rightNext=leftPosition+1<extent?PeriodicNext(shape,right,derivative):right;
						const double distance=(leftPosition&&leftPosition+1<extent)?4.0*shape.cellWidthM:
							2.0*shape.cellWidthM;
						viscous+=(stress[component][derivative][leftNext]+stress[component][derivative][rightNext]-
							stress[component][derivative][leftPrevious]-stress[component][derivative][rightPrevious])/
							distance;
					}
					result.component[component][OpenMACFaceIndex3D(shape,component,x,y,z)]+=viscous;
				}
			}
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t normalCount=axis==0?shape.nx+1:(axis==1?shape.ny+1:shape.nz+1);
				const std::size_t firstCount=axis==0?shape.ny:shape.nx;
				const std::size_t secondCount=axis==2?shape.ny:shape.nz;
				for(std::size_t second=0;second<secondCount;++second) for(std::size_t first=0;
					first<firstCount;++first) for(std::size_t normal=0;normal<normalCount;++normal){
					std::size_t x=0,y=0,z=0;if(axis==0){x=normal;y=first;z=second;}
					if(axis==1){x=first;y=normal;z=second;}if(axis==2){x=first;y=second;z=normal;}
					const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
					double phase=0.0;unsigned int samples=0;
					if(normal){const std::size_t cx=axis==0?normal-1:x,cy=axis==1?normal-1:y,
						cz=axis==2?normal-1:z;const std::size_t cell=shape.Index(cx,cy,cz);
						for(std::size_t species=0;species<MethaneCarbon;++species){
							phase+=sourceDelta[cell][1+species]/config.transport.deltaTimeS;
						}
						++samples;
					}
					if(normal+1<normalCount){const std::size_t cx=axis==0?normal:x,
						cy=axis==1?normal:y,cz=axis==2?normal:z;const std::size_t cell=shape.Index(cx,cy,cz);
						for(std::size_t species=0;species<MethaneCarbon;++species){
							phase+=sourceDelta[cell][1+species]/config.transport.deltaTimeS;
						}
						++samples;
					}
					// The outside half of a boundary momentum control volume has no
					// internal phase source; retain the same arithmetic I_rho restriction.
					const double restriction=normal==0||normal+1==normalCount?0.5:
						1.0/static_cast<double>(samples);
					if(samples) result.component[axis][face]+=
						projection.velocityMPerS.component[axis][face]*phase*restriction;
				}
			}
			return true;
		}

		inline bool SolveOpenConservativeStage3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const OpenMACField3D& baseMomentum,
			const std::vector<ConservativeVector>& sourceDelta,
			const ConservativeAdvance3DConfig& config,
			const bool predictorLimiter,
			const std::array<std::vector<bool>,6>* activeSetSeed,
			const OpenMACProjection3DResult* stage0,
			const OpenMACProjection3DResult* stage1,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			OpenConservativeStage3D& result,
			std::string* error=0,
			const std::vector<double>* projectionEnergyDeltaJPerM3=0,
			const std::vector<double>* projectionExpansionIntegral=0,
			const std::vector<ConservativeVector>* heunBeginning=0,
			const OpenFluxPair3D* firstStageFlux=0,
			const bool scalarAcceptanceStage=true
			)
		{
			const std::size_t count=shape.CellCount();std::vector<double> temperature;
			if(!InvertPeriodicTemperaturesWithinBounds(state,thermochemistry,
				config.transport.ambientTemperatureK,config.transport.adiabaticTemperatureK,
				temperature,error,config.workerCount)) return false;
			OpenBoundaryConfig3D stageBoundary=config.openBoundary;
			if(activeSetSeed) stageBoundary.priorInflow=*activeSetSeed;
			std::vector<double> target(count,0.0),priorMass;
			std::array<std::vector<bool>,6> priorInflow=stageBoundary.priorInflow;
			std::vector<double> priorDiffusivity(count,0.0),priorConductivity(count,0.0),
				priorViscosity(count,0.0);
			if((heunBeginning==0)!=(firstStageFlux==0)||
				(heunBeginning&&heunBeginning->size()!=count))return Fail(error,
					"fire solver open manifold candidate context is malformed");
			auto acceptedCandidate=[&](const OpenFluxPair3D& secondFlux,
				std::vector<ConservativeVector>& candidate,
				std::array<std::vector<double>,3>& alpha)->bool{
				if(!heunBeginning)return ApplyOpenSharedFCT3D(shape,state,secondFlux,sourceDelta,
					config.transport,fuel,thermochemistry,candidate,alpha,error,config.workerCount);
				OpenFluxPair3D averaged;
				for(std::size_t component=0;component<MethaneMassStateDimension;++component)
					averaged.boundaryCanSupply[component]=firstStageFlux->boundaryCanSupply[component]||
						secondFlux.boundaryCanSupply[component];
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
					averaged.low[axis].resize(faceCount);averaged.high[axis].resize(faceCount);
					averaged.nonadvectiveMass[axis].resize(faceCount);
					averaged.nonadvectiveEnergy[axis].resize(faceCount);
					for(std::size_t face=0;face<faceCount;++face){
						averaged.low[axis][face]=0.5*(firstStageFlux->low[axis][face]+
							secondFlux.low[axis][face]);
						averaged.high[axis][face]=0.5*(firstStageFlux->high[axis][face]+
							secondFlux.high[axis][face]);
						averaged.nonadvectiveEnergy[axis][face]=0.5*(
							firstStageFlux->nonadvectiveEnergy[axis][face]+
							secondFlux.nonadvectiveEnergy[axis][face]);
						for(std::size_t component=0;component<MethaneMassStateDimension;++component)
							averaged.nonadvectiveMass[axis][face][component]=0.5*(
								firstStageFlux->nonadvectiveMass[axis][face][component]+
								secondFlux.nonadvectiveMass[axis][face][component]);
					}
				}
				return ApplyOpenSharedFCT3D(shape,*heunBeginning,averaged,sourceDelta,
					config.transport,fuel,thermochemistry,candidate,alpha,error,config.workerCount);
			};
			for(unsigned int axis=0;axis<3;++axis) priorMass.insert(priorMass.end(),
				OpenMACFaceCount3D(shape,axis),0.0);
			double lastTargetResidual=0.0,lastMassResidual=0.0,
				lastCoefficientResidual=0.0;
			bool lastActiveSetChanged=false;
			result.picardResidualPerS.clear();
			for(std::size_t iteration=0;iteration<kMaximumCoupledPicardIterations;++iteration){
				// The deadband is history-valued: each nonlinear iterate must retain the
				// classification published by the immediately preceding iterate.  Passing
				// the stage-entry seed forever makes a threshold face alternate while the
				// convergence check compares against a history the projector never saw.
				stageBoundary.priorInflow=priorInflow;
				OpenMACProjection3DResult projection;
				// The owning tableau has already assembled the fixed momentum passed to this
				// stage: M^n for R0, M*dagger for R1, and M^{n+1,dagger} for R2.  The
				// Picard loop recomputes A-hat for the next tableau assembly, but must not
				// add it again to the momentum being projected.
				const OpenMACField3D& unprojected=baseMomentum;
				const bool finalStage=stage0&&stage1;
				const bool projectionOK=finalStage?ProjectPressureOpenMACVelocity3DFinal(shape,
					GasDensityFromConservative(state),unprojected,target,stageBoundary,
					*stage0,*stage1,config.transport.deltaTimeS,config.projectionTolerancePerS,
					projection,error,config.workerCount):ProjectPressureOpenMACVelocity3D(shape,
					GasDensityFromConservative(state),unprojected,target,stageBoundary,
					config.transport.deltaTimeS,config.projectionTolerancePerS,projection,error,
					config.workerCount);
				if(!projectionOK) return false;
				std::vector<double> diffusivity,conductivity,viscosity;
				if(!BuildOpenStageTransport3D(shape,state,temperature,projection.velocityMPerS,
					stageBoundary,config.dns,thermochemistry,transport,diffusivity,conductivity,
					viscosity,error,config.workerCount))
					return false;
				OpenMACField3D evaluatedNonpressure;
				if(!BuildOpenNonpressureMomentumRHS3D(shape,state,projection,viscosity,
					sourceDelta,config,evaluatedNonpressure,error)) return false;
				OpenFluxPair3D flux;
				if(!BuildOpenFluxPair3D(shape,state,temperature,projection,diffusivity,conductivity,
					stageBoundary,config.transport.ambientTemperatureK,
					config.injectedTemperatureK,fuel,thermochemistry,flux,error,
					config.workerCount)) return false;
				std::array<std::vector<double>,3> nextAlpha;
				std::vector<ConservativeVector> stageCandidate;
				if(scalarAcceptanceStage&&!acceptedCandidate(flux,stageCandidate,nextAlpha)){
					if(error)*error=std::string("[r70 stage candidate] ")+*error;
					return false;
				}
				std::vector<double> nextTarget;
				if(iteration==0u||!scalarAcceptanceStage){
					if(!OpenDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,flux,
						sourceDelta,config.transport.deltaTimeS,thermochemistry,nextTarget,error,
						config.workerCount,projectionEnergyDeltaJPerM3,projectionExpansionIntegral))
						return false;
				}else if(!ManifoldExactDivergenceTarget(target,stageCandidate,
					config.transport.deltaTimeS,thermochemistry,nextTarget,error))return false;
				double residual=0.0,massResidual=0.0,
					coefficientResidual=0.0;bool activeSetChanged=false;std::size_t offset=0;
				if(iteration)for(unsigned int side=0;side<6;++side)
					activeSetChanged=activeSetChanged||projection.inflow[side]!=priorInflow[side];
				for(std::size_t cell=0;cell<count;++cell) residual=std::max(residual,
					std::fabs(nextTarget[cell]-target[cell]));
				for(std::size_t cell=0;cell<count;++cell){
					if(iteration)coefficientResidual=std::max({coefficientResidual,
						std::fabs(diffusivity[cell]-priorDiffusivity[cell]),
						std::fabs(conductivity[cell]-priorConductivity[cell]),
						std::fabs(viscosity[cell]-priorViscosity[cell])});
					priorDiffusivity[cell]=diffusivity[cell];priorConductivity[cell]=conductivity[cell];
					priorViscosity[cell]=viscosity[cell];
				}
				for(unsigned int axis=0;axis<3;++axis){
					for(std::size_t face=0;face<projection.momentumKGPerM2S.component[axis].size();
						++face){
						if(iteration) massResidual=std::max(massResidual,std::fabs(
							projection.momentumKGPerM2S.component[axis][face]-
							priorMass[offset+face])/shape.cellWidthM);
						priorMass[offset+face]=projection.momentumKGPerM2S.component[axis][face];
					}
					offset+=projection.momentumKGPerM2S.component[axis].size();
				}
				result.picardResidualPerS.push_back(std::max({residual,massResidual,
					coefficientResidual}));
				lastTargetResidual=residual;lastMassResidual=massResidual;
				lastCoefficientResidual=coefficientResidual;
				lastActiveSetChanged=activeSetChanged;
				target.swap(nextTarget);
				priorInflow=projection.inflow;
				result.dynamicViscosityPaS=viscosity;
				if(iteration&&residual<=config.projectionTolerancePerS&&
					massResidual<=config.projectionTolerancePerS&&
					coefficientResidual<=config.projectionTolerancePerS&&!activeSetChanged){
					OpenMACProjection3DResult acceptedProjection;
					OpenBoundaryConfig3D acceptedBoundary=stageBoundary;
					acceptedBoundary.priorInflow=projection.inflow;
					const bool acceptedOK=finalStage?ProjectPressureOpenMACVelocity3DFinal(shape,
						GasDensityFromConservative(state),unprojected,target,acceptedBoundary,
						*stage0,*stage1,config.transport.deltaTimeS,config.projectionTolerancePerS,
						acceptedProjection,error,config.workerCount):ProjectPressureOpenMACVelocity3D(shape,
						GasDensityFromConservative(state),unprojected,target,acceptedBoundary,
						config.transport.deltaTimeS,config.projectionTolerancePerS,
						acceptedProjection,error,config.workerCount);
					if(!acceptedOK) return false;
					std::vector<double> acceptedDiffusivity,acceptedConductivity,acceptedViscosity;
					if(!BuildOpenStageTransport3D(shape,state,temperature,
						acceptedProjection.velocityMPerS,acceptedBoundary,config.dns,thermochemistry,transport,
						acceptedDiffusivity,acceptedConductivity,acceptedViscosity,error,
						config.workerCount)) return false;
					OpenFluxPair3D acceptedFlux;
					if(!BuildOpenFluxPair3D(shape,state,temperature,acceptedProjection,
						acceptedDiffusivity,acceptedConductivity,acceptedBoundary,
						config.transport.ambientTemperatureK,config.injectedTemperatureK,fuel,
						thermochemistry,acceptedFlux,error,config.workerCount))return false;
					std::array<std::vector<double>,3> verifiedAlpha;
					std::vector<ConservativeVector> verifiedCandidate;
					std::vector<double> verifiedTarget;
					if(scalarAcceptanceStage&&!acceptedCandidate(acceptedFlux,verifiedCandidate,
						verifiedAlpha)){
						if(error)*error=std::string("[r70 verified candidate] ")+*error;
						return false;
					}
					if(scalarAcceptanceStage){
						if(!ManifoldExactDivergenceTarget(target,verifiedCandidate,
							config.transport.deltaTimeS,thermochemistry,verifiedTarget,error))return false;
					}else if(!OpenDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,
						acceptedFlux,sourceDelta,config.transport.deltaTimeS,thermochemistry,
						verifiedTarget,error,config.workerCount,projectionEnergyDeltaJPerM3,
						projectionExpansionIntegral))return false;
					std::vector<double> acceptedMass,iterationMass;
					for(unsigned int axis=0;axis<3;++axis){
						acceptedMass.insert(acceptedMass.end(),
							acceptedProjection.momentumKGPerM2S.component[axis].begin(),
							acceptedProjection.momentumKGPerM2S.component[axis].end());
						iterationMass.insert(iterationMass.end(),
							projection.momentumKGPerM2S.component[axis].begin(),
							projection.momentumKGPerM2S.component[axis].end());
					}
					double verification=0.0;
					if(!PicardContinuousVerificationResidual(verifiedTarget,target,acceptedMass,
						iterationMass,acceptedDiffusivity,diffusivity,acceptedConductivity,
						conductivity,acceptedViscosity,viscosity,shape.cellWidthM,verification,error))
						return false;
					bool verifiedActiveSet=true;for(unsigned int side=0;side<6;++side)
						verifiedActiveSet=verifiedActiveSet&&acceptedProjection.inflow[side]==
							projection.inflow[side];
					if(verification>config.projectionTolerancePerS||!verifiedActiveSet){
						target.swap(verifiedTarget);priorInflow=acceptedProjection.inflow;
						continue;}
					LimiterPicardAcceptance3D limiterAcceptance;
					if(predictorLimiter){
						if(!SelectLimiterPicardAcceptance3D(nextAlpha,verifiedAlpha,
							config.projectionTolerancePerS,limiterAcceptance,error)) return false;
						std::vector<ConservativeVector> certifiedPredictor;
						std::array<std::vector<double>,3> certifiedAlpha;
						if(!ApplyOpenSharedFCT3D(shape,state,acceptedFlux,sourceDelta,
							config.transport,fuel,thermochemistry,certifiedPredictor,certifiedAlpha,error,
							config.workerCount,&limiterAcceptance.faceAlpha)) return false;
						std::vector<double> certifiedTarget;
						if(!ManifoldExactDivergenceTarget(target,certifiedPredictor,
							config.transport.deltaTimeS,thermochemistry,certifiedTarget,error))return false;
						double certifiedResidual=0.0;
						for(std::size_t cell=0;cell<count;++cell)certifiedResidual=std::max(
							certifiedResidual,std::fabs(certifiedTarget[cell]-target[cell]));
						if(certifiedResidual>config.projectionTolerancePerS){
							target.swap(certifiedTarget);continue;
						}
					}
					result.projection=std::move(acceptedProjection);
					result.flux=std::move(acceptedFlux);
					result.faceAlpha=predictorLimiter?std::move(limiterAcceptance.faceAlpha):
						std::move(verifiedAlpha);
					result.maximumLimiterClassDiscrepancy=predictorLimiter?
						limiterAcceptance.maximumFaceDiscrepancy:0.0;
					result.limiterDiscontinuousClass=predictorLimiter&&
						limiterAcceptance.discontinuousClass;
					result.divergenceTargetPerS=std::move(verifiedTarget);
					result.diffusivityM2PerS=acceptedDiffusivity;
					result.conductivityWPerMK=acceptedConductivity;
					result.dynamicViscosityPaS=acceptedViscosity;
					return BuildOpenNonpressureMomentumRHS3D(shape,state,result.projection,
						acceptedViscosity,sourceDelta,config,result.nonpressureMomentumRHS,error);
				}
			}
			std::ostringstream message;
			message << "fire solver open conservative Picard stage did not converge";
			if(!result.picardResidualPerS.empty()) message << ": first=" <<
				result.picardResidualPerS.front() << " last=" <<
				result.picardResidualPerS.back() << " minimum=" <<
				*std::min_element(result.picardResidualPerS.begin(),
					result.picardResidualPerS.end()) << " target=" << lastTargetResidual <<
				" mass=" << lastMassResidual <<
				" coefficient=" << lastCoefficientResidual << " active_set=" <<
				(lastActiveSetChanged?1:0) << " tolerance=" << config.projectionTolerancePerS;
			return Fail(error,message.str());
		}

		inline bool AdvancePeriodicConservative3DImplementation(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const PeriodicMACField& beginningMomentum,
			const std::vector<MethaneSourcePacket>& frozenPacket,
			const ConservativeAdvance3DConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			ConservativeAdvance3DResult& result,
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( !ValidatePeriodicShape3D(shape,error) || beginning.size() != count ||
				!fuel.IsValid() || !thermochemistry.IsValid() || !transport.IsValid() ||
				!std::isfinite(config.projectionTolerancePerS) ||
				config.projectionTolerancePerS <= 0.0 ||
				!std::isfinite(config.transport.deltaTimeS) ||
				config.transport.deltaTimeS <= 0.0 ||
				config.transport.cellWidthM != shape.cellWidthM || config.workerCount==0u ) return Fail(error,
				"fire solver owning 3-D advance input is malformed");
			for( unsigned int axis=0; axis<3; ++axis ) if(
				beginningMomentum.component[axis].size() != count ) return Fail(error,
				"fire solver owning 3-D momentum shape is invalid");
			std::vector<ConservativeVector> sourceDelta;
			if(!FrozenPacketExpansionAdmissible3D(beginning,frozenPacket,
				config.transport.deltaTimeS,thermochemistry,config.workerCount,error)||
				!FrozenPacketDeltas3D(frozenPacket,count,sourceDelta,config.workerCount,error))return false;
			std::vector<double> projectionEnergyDeltaJPerM3,projectionExpansionIntegral;
			FrozenPacketProjectionPairs3D(frozenPacket,projectionEnergyDeltaJPerM3,
				projectionExpansionIntegral);
			ConservativeAdvance3DResult candidate;
			candidate.effectiveWorkerCount=std::max(1u,std::min(config.workerCount,
				static_cast<unsigned int>(count)));
			if( !SolveConservativeStage3D(shape,beginning,beginningMomentum,sourceDelta,config,
				true,fuel,thermochemistry,transport,candidate.r0,error,
				&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral) ) return false;
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if( !ApplyPeriodicSharedFCT3D(shape,beginning,candidate.r0.flux,sourceDelta,
				config.transport,fuel,thermochemistry,predictor,predictorAlpha,error,
				&candidate.r0.faceAlpha) ) return false;
			candidate.maximumLimiterClassDiscrepancy=
				candidate.r0.maximumLimiterClassDiscrepancy;
			candidate.discontinuousLimiterClassCount=
				candidate.r0.limiterDiscontinuousClass?1u:0u;
			std::array<std::vector<double>,3> low0, high0, diffusion0;
			GasPrimalSubfluxes3D(candidate.r0.flux,low0,high0,diffusion0);
			std::array<std::vector<double>,3> predictorAccepted;
			for( unsigned int axis=0; axis<3; ++axis ) {
				predictorAccepted[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) predictorAccepted[axis][face] =
					low0[axis][face]+predictorAlpha[axis][face]*(high0[axis][face]-
					low0[axis][face]);
			}
			const std::array<std::vector<double>,3> predictorDivergence =
				CompatibleMomentumFluxDivergence3D(shape,predictorAccepted,diffusion0,
					candidate.r0.projection.velocityMPerS);
			PeriodicMACField predictorMomentum;
			for( unsigned int axis=0; axis<3; ++axis ) {
				predictorMomentum.component[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) predictorMomentum.component[axis][face] =
					beginningMomentum.component[axis][face]+config.transport.deltaTimeS*(
					candidate.r0.nonpressureMomentumRHS.component[axis][face]-
					predictorDivergence[axis][face]);
			}
			if( !SolveConservativeStage3D(shape,predictor,predictorMomentum,sourceDelta,config,
				false,fuel,thermochemistry,transport,candidate.r1,error,
				&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral,&beginning,
				&candidate.r0.flux,true) ) return false;
			PeriodicFluxPair3D averaged;
			for( unsigned int axis=0; axis<3; ++axis ) {
				averaged.low[axis].resize(count); averaged.high[axis].resize(count);
				averaged.nonadvectiveMass[axis].resize(count);
				averaged.nonadvectiveEnergy[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) {
					averaged.low[axis][face] = 0.5*(candidate.r0.flux.low[axis][face]+
						candidate.r1.flux.low[axis][face]);
					averaged.high[axis][face] = 0.5*(candidate.r0.flux.high[axis][face]+
						candidate.r1.flux.high[axis][face]);
					averaged.nonadvectiveEnergy[axis][face] = 0.5*(
						candidate.r0.flux.nonadvectiveEnergy[axis][face]+
						candidate.r1.flux.nonadvectiveEnergy[axis][face]);
					for( std::size_t component=0; component<MethaneMassStateDimension; ++component )
						averaged.nonadvectiveMass[axis][face][component] = 0.5*(
							candidate.r0.flux.nonadvectiveMass[axis][face][component]+
							candidate.r1.flux.nonadvectiveMass[axis][face][component]);
				}
			}
			if( !ApplyPeriodicSharedFCT3D(shape,beginning,averaged,sourceDelta,
				config.transport,fuel,thermochemistry,candidate.conservative,
				candidate.faceAlpha,error,&candidate.r1.faceAlpha) ) return false;
			std::vector<double> acceptedTemperature;
			if( !InvertPeriodicTemperaturesWithinBounds(candidate.conservative,thermochemistry,
				config.transport.ambientTemperatureK,config.transport.adiabaticTemperatureK,
				acceptedTemperature,error,config.workerCount) || !PeriodicDivergenceTargetFromPhysicalFlux3D(shape,
				candidate.conservative,acceptedTemperature,averaged,sourceDelta,
				config.transport.deltaTimeS,thermochemistry,candidate.divergenceHeunPerS,error,
				config.workerCount,&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral) )
				return false;
			std::array<std::vector<double>,3> low1, high1, diffusion1, accepted0, accepted1;
			GasPrimalSubfluxes3D(candidate.r1.flux,low1,high1,diffusion1);
			for( unsigned int axis=0; axis<3; ++axis ) {
				accepted0[axis].resize(count); accepted1[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) {
					accepted0[axis][face] = low0[axis][face]+candidate.faceAlpha[axis][face]*
						(high0[axis][face]-low0[axis][face]);
					accepted1[axis][face] = low1[axis][face]+candidate.faceAlpha[axis][face]*
						(high1[axis][face]-low1[axis][face]);
				}
			}
			const std::array<std::vector<double>,3> divergence0 =
				CompatibleMomentumFluxDivergence3D(shape,accepted0,diffusion0,
					candidate.r0.projection.velocityMPerS);
			const std::array<std::vector<double>,3> divergence1 =
				CompatibleMomentumFluxDivergence3D(shape,accepted1,diffusion1,
					candidate.r1.projection.velocityMPerS);
			PeriodicMACField finalMomentum;
			for( unsigned int axis=0; axis<3; ++axis ) {
				finalMomentum.component[axis].resize(count);
				for( std::size_t face=0; face<count; ++face ) finalMomentum.component[axis][face] =
					beginningMomentum.component[axis][face]+0.5*config.transport.deltaTimeS*(
					candidate.r0.nonpressureMomentumRHS.component[axis][face]+
					candidate.r1.nonpressureMomentumRHS.component[axis][face]-
						divergence0[axis][face]-divergence1[axis][face]);
			}
			if(!config.retainStageDiagnostics){
				candidate.r0=ConservativeStage3D();
				candidate.r1=ConservativeStage3D();
			}
			if( !SolveConservativeStage3D(shape,candidate.conservative,finalMomentum,sourceDelta,
				config,false,fuel,thermochemistry,transport,candidate.r2,error,
				&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral,0,0,false) ) return false;
			candidate.momentumKGPerM2S = candidate.r2.projection.momentumKGPerM2S;
			candidate.velocityMPerS = candidate.r2.projection.velocityMPerS;
			candidate.stepAverageDynamicPressurePa =
				candidate.r2.projection.stepAverageDynamicPressurePa;
			if(!config.retainStageDiagnostics){
				candidate.r0=ConservativeStage3D();
				candidate.r1=ConservativeStage3D();
				candidate.r2=ConservativeStage3D();
			}
			result = std::move(candidate);
			return true;
		}

		inline bool AdvanceOpenConservative3DImplementation(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const OpenMACField3D& beginningMomentum,
			const std::vector<MethaneSourcePacket>& frozenPacket,
			const ConservativeAdvance3DConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			OpenConservativeAdvance3DResult& result,
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();
			if(!ValidateOpenBoundaryConfig3D(shape,config.openBoundary,error) ||
				beginning.size()!=count || config.injectedTemperatureK<=0.0) return Fail(error,
				"fire solver owning open advance input is malformed");
			for(unsigned int axis=0;axis<3;++axis) if(beginningMomentum.component[axis].size()!=
				OpenMACFaceCount3D(shape,axis)) return Fail(error,
				"fire solver owning open momentum shape is invalid");
			std::vector<ConservativeVector> sourceDelta;
			if(!FrozenPacketExpansionAdmissible3D(beginning,frozenPacket,
				config.transport.deltaTimeS,thermochemistry,config.workerCount,error)||
				!FrozenPacketDeltas3D(frozenPacket,count,sourceDelta,config.workerCount,error))return false;
			std::vector<double> projectionEnergyDeltaJPerM3,projectionExpansionIntegral;
			FrozenPacketProjectionPairs3D(frozenPacket,projectionEnergyDeltaJPerM3,
				projectionExpansionIntegral);
			OpenConservativeAdvance3DResult candidate;
			if(!SolveOpenConservativeStage3D(shape,beginning,beginningMomentum,sourceDelta,
				config,true,0,0,0,fuel,thermochemistry,transport,candidate.r0,error,
				&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral)){
				if(error)*error=std::string("R0: ")+*error;return false;}
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if(!ApplyOpenSharedFCT3D(shape,beginning,candidate.r0.flux,sourceDelta,
				config.transport,fuel,thermochemistry,predictor,predictorAlpha,error,
				config.workerCount,&candidate.r0.faceAlpha)) return false;
			candidate.maximumLimiterClassDiscrepancy=
				candidate.r0.maximumLimiterClassDiscrepancy;
			candidate.discontinuousLimiterClassCount=
				candidate.r0.limiterDiscontinuousClass?1u:0u;
			const OpenMACField3D predictorAdvection=OpenCompatibleMomentumFluxDivergence3D(
				shape,candidate.r0.flux,
				predictorAlpha,candidate.r0.projection.velocityMPerS,
				&config.openBoundary);
			OpenMACField3D predictorMomentum=beginningMomentum;
			for(unsigned int axis=0;axis<3;++axis) for(std::size_t face=0;face<
				predictorMomentum.component[axis].size();++face) predictorMomentum.component[axis][face]+=
				config.transport.deltaTimeS*(candidate.r0.nonpressureMomentumRHS.component[axis][face]-
				predictorAdvection.component[axis][face]);
			if(!SolveOpenConservativeStage3D(shape,predictor,predictorMomentum,sourceDelta,
				config,false,&candidate.r0.projection.inflow,0,0,fuel,thermochemistry,transport,
				candidate.r1,error,&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral,
				&beginning,&candidate.r0.flux,true))
				{if(error)*error=std::string("R1: ")+*error;return false;}
			OpenFluxPair3D averaged;
			for(std::size_t component=0;component<MethaneMassStateDimension;++component)
				averaged.boundaryCanSupply[component]=candidate.r0.flux.boundaryCanSupply[component]||
					candidate.r1.flux.boundaryCanSupply[component];
			for(unsigned int axis=0;axis<3;++axis){const std::size_t faceCount=
				OpenMACFaceCount3D(shape,axis);averaged.low[axis].resize(faceCount);
				averaged.high[axis].resize(faceCount);averaged.nonadvectiveMass[axis].resize(faceCount);
				averaged.nonadvectiveEnergy[axis].resize(faceCount);
				for(std::size_t face=0;face<faceCount;++face){averaged.low[axis][face]=0.5*(
					candidate.r0.flux.low[axis][face]+candidate.r1.flux.low[axis][face]);
					averaged.high[axis][face]=0.5*(candidate.r0.flux.high[axis][face]+
					candidate.r1.flux.high[axis][face]);averaged.nonadvectiveEnergy[axis][face]=
					0.5*(candidate.r0.flux.nonadvectiveEnergy[axis][face]+
					candidate.r1.flux.nonadvectiveEnergy[axis][face]);
					for(std::size_t component=0;component<MethaneMassStateDimension;++component)
						averaged.nonadvectiveMass[axis][face][component]=0.5*(candidate.r0.flux.
						nonadvectiveMass[axis][face][component]+candidate.r1.flux.nonadvectiveMass[
						axis][face][component]);}}
			if(!ApplyOpenSharedFCT3D(shape,beginning,averaged,sourceDelta,config.transport,
				fuel,thermochemistry,candidate.conservative,candidate.faceAlpha,error,
				config.workerCount,&candidate.r1.faceAlpha)) return false;
			std::vector<double> acceptedTemperature;
			if(!InvertPeriodicTemperaturesWithinBounds(candidate.conservative,thermochemistry,
				config.transport.ambientTemperatureK,config.transport.adiabaticTemperatureK,
				acceptedTemperature,error,config.workerCount) || !OpenDivergenceTargetFromPhysicalFlux3D(shape,
				candidate.conservative,acceptedTemperature,averaged,sourceDelta,
				config.transport.deltaTimeS,thermochemistry,candidate.divergenceHeunPerS,error,
				config.workerCount,&projectionEnergyDeltaJPerM3,&projectionExpansionIntegral))
				return false;
			const OpenMACField3D advection0=OpenCompatibleMomentumFluxDivergence3D(shape,
				candidate.r0.flux,
				candidate.faceAlpha,candidate.r0.projection.velocityMPerS,
				&config.openBoundary);
			const OpenMACField3D advection1=OpenCompatibleMomentumFluxDivergence3D(shape,
				candidate.r1.flux,
				candidate.faceAlpha,candidate.r1.projection.velocityMPerS,
				&config.openBoundary);
			OpenMACField3D finalMomentum=beginningMomentum;
			for(unsigned int axis=0;axis<3;++axis) for(std::size_t face=0;face<
				finalMomentum.component[axis].size();++face) finalMomentum.component[axis][face]+=
				0.5*config.transport.deltaTimeS*(candidate.r0.nonpressureMomentumRHS.component[
				axis][face]+candidate.r1.nonpressureMomentumRHS.component[axis][face]-
					advection0.component[axis][face]-advection1.component[axis][face]);
			if(!config.retainStageDiagnostics){
				candidate.r0.flux=OpenFluxPair3D();candidate.r1.flux=OpenFluxPair3D();
				candidate.r0.nonpressureMomentumRHS=OpenMACField3D();
				candidate.r1.nonpressureMomentumRHS=OpenMACField3D();
				candidate.r0.faceAlpha={};candidate.r1.faceAlpha={};
				candidate.r0.diffusivityM2PerS.clear();candidate.r1.diffusivityM2PerS.clear();
				candidate.r0.conductivityWPerMK.clear();candidate.r1.conductivityWPerMK.clear();
				candidate.r0.dynamicViscosityPaS.clear();candidate.r1.dynamicViscosityPaS.clear();
			}
			if(!SolveOpenConservativeStage3D(shape,candidate.conservative,finalMomentum,
				sourceDelta,config,false,&candidate.r1.projection.inflow,&candidate.r0.projection,
				&candidate.r1.projection,
				fuel,thermochemistry,transport,candidate.r2,error,&projectionEnergyDeltaJPerM3,
				&projectionExpansionIntegral,0,0,false)){
				if(error)*error=std::string("R2: ")+*error;return false;}
			candidate.momentumKGPerM2S=candidate.r2.projection.momentumKGPerM2S;
			candidate.velocityMPerS=candidate.r2.projection.velocityMPerS;
			candidate.stepAverageDynamicPressurePa=
				candidate.r2.projection.stepAverageDynamicPressurePa;
			if(!config.retainStageDiagnostics)candidate.r2=OpenConservativeStage3D();
			result=std::move(candidate);return true;
		}

		inline bool AdvanceConservative3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& beginning,
			const PeriodicMACField& beginningMomentum,
			const std::vector<MethaneSourcePacket>& frozenPacket,
			const ConservativeAdvance3DConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			ConservativeAdvance3DResult& result,
			std::string* error=0
			)
		{
			if(config.workerCount==0u) return Fail(error,
				"fire solver owning 3-D worker count is invalid");
			if(config.periodicBoundaries) return AdvancePeriodicConservative3DImplementation(
				shape,beginning,beginningMomentum,frozenPacket,config,fuel,thermochemistry,
				transport,result,error);
			OpenConservativeAdvance3DResult open;
			OpenMACField3D openBeginningMomentum;
			openBeginningMomentum.component=beginningMomentum.component;
			if(!AdvanceOpenConservative3DImplementation(shape,beginning,openBeginningMomentum,
				frozenPacket,config,fuel,thermochemistry,transport,open,error)) return false;
			ConservativeAdvance3DResult candidate;
			candidate.effectiveWorkerCount=std::max(1u,std::min(config.workerCount,
				static_cast<unsigned int>(shape.CellCount())));
			candidate.conservative=std::move(open.conservative);
			candidate.momentumKGPerM2S.component=std::move(open.momentumKGPerM2S.component);
			candidate.velocityMPerS.component=std::move(open.velocityMPerS.component);
			candidate.stepAverageDynamicPressurePa=
				open.stepAverageDynamicPressurePa;candidate.faceAlpha=open.faceAlpha;
			candidate.divergenceHeunPerS=open.divergenceHeunPerS;
			candidate.maximumDivergenceResidualPerS=
				open.r2.projection.maximumDivergenceResidualPerS;
			candidate.maximumBoundaryHeadResidualPa=
				open.r2.projection.maximumBoundaryHeadResidualPa;
			candidate.maximumLimiterClassDiscrepancy=open.maximumLimiterClassDiscrepancy;
			candidate.discontinuousLimiterClassCount=open.discontinuousLimiterClassCount;
			if(config.retainStageDiagnostics){
				candidate.r0.flux.low=std::move(open.r0.flux.low);
				candidate.r0.flux.high=std::move(open.r0.flux.high);
				candidate.r0.flux.nonadvectiveMass=std::move(open.r0.flux.nonadvectiveMass);
				candidate.r0.flux.nonadvectiveEnergy=std::move(open.r0.flux.nonadvectiveEnergy);
				candidate.r0.faceAlpha=std::move(open.r0.faceAlpha);
				candidate.r0.maximumLimiterClassDiscrepancy=
					open.r0.maximumLimiterClassDiscrepancy;
				candidate.r0.limiterDiscontinuousClass=open.r0.limiterDiscontinuousClass;
				candidate.r0.picardResidualPerS=std::move(open.r0.picardResidualPerS);
				candidate.r1.picardResidualPerS=std::move(open.r1.picardResidualPerS);
				candidate.r2.picardResidualPerS=std::move(open.r2.picardResidualPerS);
			}
			result=std::move(candidate);return true;
		}

#endif
