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
			std::vector<std::vector<double> > coordinate(count,
				std::vector<double>(reconstruction.nullity,0.0));
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
					for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
						coordinate[cell][basis] += reconstruction.orthonormalBasis[
							row*reconstruction.nullity+basis]*cells[cell][row];
					}
				}
			}
			for( unsigned int axis=0; axis<3; ++axis ) {
				slopes[axis].assign(count,std::array<double,MethaneMassStateDimension>());
				for( std::size_t cell=0; cell<count; ++cell ) {
					slopes[axis][cell].fill(0.0);
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					const std::size_t next = PeriodicNext(shape,cell,axis);
					for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
						const double slope = MCScalarSlope(
							coordinate[cell][basis]-coordinate[previous][basis],
							coordinate[next][basis]-coordinate[cell][basis]);
						for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
							slopes[axis][cell][row] += reconstruction.orthonormalBasis[
								row*reconstruction.nullity+basis]*slope;
						}
					}
				}
			}
			// An identically absent inventory is an exact invariant (not merely a
			// nonnegative inequality).  Preserve the methane zero-soot limit and any
			// other exact-zero constituent without a roundoff-sized basis leakage.
			for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
				bool identicallyZero=true;
				for( const ConservativeVector& cell:cells ) identicallyZero=
					identicallyZero && cell[row]==0.0;
				if( identicallyZero ) for( unsigned int axis=0; axis<3; ++axis ) for(
					std::size_t cell=0; cell<count; ++cell ) slopes[axis][cell][row]=0.0;
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
			std::string* error = 0
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
			if( !InvariantMCMassSlopes3D(shape,cells,fuel.ConservativeReconstruction(),
				massSlope,error) ) return false;
			std::array<std::vector<double>,3> energySlope;
			for( unsigned int axis=0; axis<3; ++axis ) {
				if( faceVelocityMPerS.component[axis].size() != count ) {
					return Fail(error,"fire solver 3-D periodic velocity shape is invalid");
				}
				energySlope[axis].assign(count,0.0);
				result.low[axis].assign(count,ConservativeVector());
				result.high[axis].assign(count,ConservativeVector());
				result.nonadvectiveMass[axis].assign(count,
					std::array<double,MethaneMassStateDimension>());
				result.nonadvectiveEnergy[axis].assign(count,0.0);
				for( std::size_t cell=0; cell<count; ++cell ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					const std::size_t next = PeriodicNext(shape,cell,axis);
					energySlope[axis][cell] = MCScalarSlope(
						cells[cell][MethaneMassStateDimension]-
							cells[previous][MethaneMassStateDimension],
						cells[next][MethaneMassStateDimension]-
							cells[cell][MethaneMassStateDimension]);
				}
			}
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			for( unsigned int axis=0; axis<3; ++axis ) for( std::size_t left=0;
				left<count; ++left ) {
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
					return Fail(error,"fire solver 3-D periodic transport state is outside its domain");
				}
				const double rhoD = HarmonicMean(totalLeft*diffusivityM2PerS[left],
					totalRight*diffusivityM2PerS[right]);
				std::vector<double> raw(MethaneMassStateDimension,0.0), projected;
				raw[0] = -rhoD*(cells[right][0]/totalRight-cells[left][0]/totalLeft)/
					shape.cellWidthM;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					raw[1+species] = -rhoD*(cells[right][1+species]/totalRight-
						cells[left][1+species]/totalLeft)/shape.cellWidthM;
				}
				// This is the sole physical mass-flux correction: exactly N_C N_C^T Jtilde.
				if( !fuel.NonadvectiveFluxProjection().Project(raw,projected,error) ||
					projected.size() != MethaneMassStateDimension ) return false;
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					result.nonadvectiveMass[axis][left][component] = projected[component];
				}
				const double faceTemperature = 0.5*(temperatureK[left]+temperatureK[right]);
				double enthalpyFlux = 0.0;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					double enthalpy = 0.0;
					if( !thermochemistry.SensibleEnthalpyJPerKG(names[species],faceTemperature,
						enthalpy,error) ) return false;
					enthalpyFlux += enthalpy*projected[1+species];
				}
				const double conductivity = HarmonicMean(conductivityWPerMK[left],
					conductivityWPerMK[right]);
				const double physicalEnergy = enthalpyFlux-conductivity*
					(temperatureK[right]-temperatureK[left])/shape.cellWidthM;
				result.nonadvectiveEnergy[axis][left] = physicalEnergy;
				const std::size_t donor = velocity >= 0.0 ? left : right;
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					const double highValue = velocity >= 0.0 ? cells[left][component]+
						0.5*massSlope[axis][left][component] : cells[right][component]-
						0.5*massSlope[axis][right][component];
					result.low[axis][left][component] = velocity*cells[donor][component]+
						projected[component];
					result.high[axis][left][component] = velocity*highValue+projected[component];
				}
				const double highEnergy = velocity >= 0.0 ?
					cells[left][MethaneMassStateDimension]+0.5*energySlope[axis][left] :
					cells[right][MethaneMassStateDimension]-0.5*energySlope[axis][right];
				result.low[axis][left][MethaneMassStateDimension] =
					velocity*cells[donor][MethaneMassStateDimension]+physicalEnergy;
				result.high[axis][left][MethaneMassStateDimension] =
					velocity*highEnergy+physicalEnergy;
			}
			return true;
		}

		inline bool FrozenPacketDeltas3D(
			const std::vector<MethaneSourcePacket>& packet,
			const std::size_t count,
			std::vector<ConservativeVector>& delta,
			std::string* error = 0
			)
		{
			if( packet.size() != count ) {
				return Fail(error,"fire solver 3-D frozen-packet shape is invalid");
			}
			delta.assign(count,ConservativeVector());
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					const double value = packet[cell].constituentDelta[species];
					if( !std::isfinite(value) ) return Fail(error,
						"fire solver 3-D frozen packet contains a non-finite mass delta");
					delta[cell][1+species] = value;
				}
				if( !std::isfinite(packet[cell].sensibleEnergyDeltaJPerM3) ) return Fail(error,
					"fire solver 3-D frozen packet contains a non-finite energy delta");
				delta[cell][MethaneMassStateDimension] =
					packet[cell].sensibleEnergyDeltaJPerM3;
			}
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
			std::string* error = 0
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
				if( !CertifiedMassConstraintSatisfied(beginning[cell],
					fuel.ConservativeReconstruction()) ) return Fail(error,
					"fire solver 3-D FCT input violates the certified affine invariant");
				low[cell] = beginning[cell]+frozenSourceDelta[cell];
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					low[cell] = low[cell]+scale*(flux.low[axis][previous]-flux.low[axis][cell]);
					correction[2*axis][cell] = scale*(flux.high[axis][previous]-
						flux.low[axis][previous]);
					correction[2*axis+1][cell] = -scale*(flux.high[axis][cell]-
						flux.low[axis][cell]);
				}
				const double tolerance = 512.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(low[cell][MethaneMassStateDimension]));
				if( !ConservativeStateFeasible(low[cell],ambientEnthalpy,
					adiabaticEnthalpy,tolerance) ) return Fail(error,
					"fire solver 3-D low-order FCT state is infeasible");
				if( !CertifiedMassConstraintSatisfied(low[cell],
					fuel.ConservativeReconstruction()) ) return Fail(error,
					"fire solver 3-D low-order FCT state violates the certified affine invariant");
			}
			const std::size_t inequalityCount = 4+MethaneSpeciesCount;
			std::vector<std::vector<double> > ratio(count,
				std::vector<double>(inequalityCount,1.0));
			for( std::size_t cell=0; cell<count; ++cell ) {
				double stateScale=1.0;
				for( std::size_t component=0; component<MethaneConservativeDimension;
					++component ) stateScale=std::max(stateScale,std::fabs(low[cell][component]));
				const double outwardBudget=4096.0*std::numeric_limits<double>::epsilon()*
					stateScale;
				for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
					const double budget = std::max(0.0,-InequalityValue(low[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy))+outwardBudget;
					double requested = 0.0;
					for( unsigned int direction=0; direction<6; ++direction ) requested +=
						std::max(0.0,InequalityValue(correction[direction][cell],inequality,
							ambientEnthalpy,adiabaticEnthalpy));
					ratio[cell][inequality] = requested > 0.0 ?
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
								ratio[left][inequality]);
						}
						if( InequalityValue(correction[2*axis][right],inequality,
							ambientEnthalpy,adiabaticEnthalpy) > 0.0 ) {
							faceAlpha[axis][left] = std::min(faceAlpha[axis][left],
								ratio[right][inequality]);
						}
					}
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
			for(std::size_t component=0;component<MethaneMassStateDimension;++component){
				bool absent=true;double scaleValue=1.0;
				for(std::size_t cell=0;cell<count;++cell){absent=absent&&
					beginning[cell][component]==0.0&&frozenSourceDelta[cell][component]==0.0;
					scaleValue=std::max(scaleValue,std::fabs(candidate[cell][component]));}
				if(absent){const double envelope=4096.0*std::numeric_limits<double>::epsilon()*
					scaleValue;for(std::size_t cell=0;cell<count;++cell){if(std::fabs(
					candidate[cell][component])>envelope)return Fail(error,
					"fire solver 3-D FCT created a finite absent inventory");
					candidate[cell][component]=0.0;}}
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				const double tolerance = 2048.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(candidate[cell][MethaneMassStateDimension]));
				if( !ConservativeStateFeasible(candidate[cell],ambientEnthalpy,
					adiabaticEnthalpy,tolerance) || !CertifiedMassConstraintSatisfied(
					candidate[cell],fuel.ConservativeReconstruction()) ) return Fail(error,
					"fire solver 3-D shared FCT result violates a nodal invariant");
			}
			result.swap(candidate);
			return true;
		}

		// Test reference for spatial/temporal convergence.  It owns no production
		// state and is deliberately not callable through fire_simulator.
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
			std::vector<double> temperature0, temperature1;
			if( !InvertPeriodicTemperatures(beginning,thermochemistry,temperature0,error) )
				return false;
			PeriodicFluxPair3D flux0;
			if( !BuildPeriodicFluxPair3D(shape,beginning,temperature0,velocity,diffusivity,
				conductivity,fuel,thermochemistry,flux0,error) ) return false;
			if( donorOnly ) flux0.high=flux0.low;
			const std::vector<ConservativeVector> zeroDelta(beginning.size());
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if( !ApplyPeriodicSharedFCT3D(shape,beginning,flux0,zeroDelta,config,fuel,
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
			return ApplyPeriodicSharedFCT3D(shape,beginning,averaged,zeroDelta,config,fuel,
				thermochemistry,result,acceptedAlpha,error);
		}

		struct DebugMacCormackNegativeControl
		{
			double relativeInventoryError;
			double maximumLocalConservativeError;
			bool clampActivated;
			DebugMacCormackNegativeControl() : relativeInventoryError(0.0),
				maximumLocalConservativeError(0.0),clampActivated(false) {}
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
				double densityScale=1.0;
				for(const double density:physical.constituent)
					densityScale=std::max(densityScale,std::fabs(density));
				const double forwardTolerance=2048.0*
					std::numeric_limits<double>::epsilon()*densityScale;
				for(double& density:physical.constituent) if(density<0.0 &&
					density>=-forwardTolerance) density=0.0;
				if(physical.rhoTotalZ<0.0 && physical.rhoTotalZ>=-forwardTolerance)
					physical.rhoTotalZ=0.0;
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
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			if( state.size() != count || temperatureK.size() != count ||
				sourceDelta.size() != count || !std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ) {
				return Fail(error,"fire solver 3-D divergence target arrays are malformed");
			}
			result.assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				ConservativeVector rate = (1.0/deltaTimeS)*sourceDelta[cell];
				for( unsigned int axis=0; axis<3; ++axis ) {
					if( flux.nonadvectiveMass[axis].size() != count ||
						flux.nonadvectiveEnergy[axis].size() != count ) return Fail(error,
						"fire solver 3-D physical flux shape is invalid");
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					for( std::size_t component=0; component<MethaneMassStateDimension;
						++component ) rate[component] +=
						(flux.nonadvectiveMass[axis][previous][component]-
						flux.nonadvectiveMass[axis][cell][component])/shape.cellWidthM;
					rate[MethaneMassStateDimension] +=
						(flux.nonadvectiveEnergy[axis][previous]-
						flux.nonadvectiveEnergy[axis][cell])/shape.cellWidthM;
				}
				if( !DivergenceFromDiscreteRate(state[cell],rate,temperatureK[cell],
					thermochemistry,result[cell],error) ) return false;
			}
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
							upper = massFlux(nextComponent)*0.5*(
								faceVelocity.component[component][face]+
								faceVelocity.component[component][nextComponent]);
							lower = massFlux(face)*0.5*(
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

		inline bool BuildOpenStageTransport3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const OpenMACField3D& faceVelocity,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<double>& diffusivity,
			std::vector<double>& conductivity,
			std::vector<double>& viscosity,
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();
			if(state.size()!=count || temperatureK.size()!=count) return Fail(error,
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
			diffusivity.assign(count,0.0);conductivity.assign(count,0.0);viscosity.assign(count,0.0);
			const double widths[3]={shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
			for(std::size_t cell=0;cell<count;++cell){
				MethaneCellState physical=FromConservativeVector(state[cell]);
				physical.temperatureK=temperatureK[cell];
				double densityScale=1.0;for(const double density:physical.constituent)
					densityScale=std::max(densityScale,std::fabs(density));
				const double forwardTolerance=2048.0*std::numeric_limits<double>::epsilon()*
					densityScale;
				for(double& density:physical.constituent) if(density<0.0&&
					density>=-forwardTolerance) density=0.0;
				if(physical.rhoTotalZ<0.0&&physical.rhoTotalZ>=-forwardTolerance)
					physical.rhoTotalZ=0.0;
				double gradient[3][3]={};
				for(unsigned int derivative=0;derivative<3;++derivative){
					const std::size_t x=cell%shape.nx;
					const std::size_t y=(cell/shape.nx)%shape.ny;
					const std::size_t z=cell/(shape.nx*shape.ny);
					const std::size_t coordinate=derivative==0?x:(derivative==1?y:z);
					const std::size_t extent=derivative==0?shape.nx:(derivative==1?shape.ny:shape.nz);
					const std::size_t previous=coordinate?PeriodicPrevious(shape,cell,derivative):cell;
					const std::size_t next=coordinate+1<extent?PeriodicNext(shape,cell,derivative):cell;
					const double distance=(coordinate&&coordinate+1<extent)?2.0*shape.cellWidthM:
						shape.cellWidthM;
					for(unsigned int component=0;component<3;++component)
						gradient[derivative][component]=(cellVelocity[component][next]-
							cellVelocity[component][previous])/distance;
				}
				CellTransportEvaluation evaluation;
				if(!EvaluateCellTransport(physical,gradient,widths,dns,thermochemistry,
					transport,evaluation,error)) return false;
				diffusivity[cell]=evaluation.totalDiffusivityM2PerS;
				conductivity[cell]=evaluation.effectiveConductivityWPerMK;
				viscosity[cell]=evaluation.effectiveViscosityPaS;
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
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();
			PeriodicMACField periodicVelocity;
			for(unsigned int axis=0;axis<3;++axis){
				periodicVelocity.component[axis].resize(count);
				for(std::size_t cell=0;cell<count;++cell) periodicVelocity.component[axis][cell]=
					projection.velocityMPerS.component[axis][OpenUpperFaceForCell3D(shape,cell,axis)];
			}
			PeriodicFluxPair3D interior;
			if(!BuildPeriodicFluxPair3D(shape,state,temperatureK,periodicVelocity,diffusivity,
				conductivity,fuel,thermochemistry,interior,error)) return false;
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
						result.high[axis][face]=interior.high[axis][cell];
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
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();
			if(beginning.size()!=count || sourceDelta.size()!=count) return Fail(error,
				"fire solver open FCT state shape is invalid");
			std::array<double,MethaneSpeciesCount> ambientEnthalpy,adiabaticEnthalpy;
			if(!FireSimulationEnthalpyBounds(config,thermochemistry,ambientEnthalpy,
				adiabaticEnthalpy,error)) return false;
			const double scale=config.deltaTimeS/shape.cellWidthM;
			std::vector<ConservativeVector> low(count);
			std::array<std::vector<ConservativeVector>,6> correction;
			for(unsigned int direction=0;direction<6;++direction)
				correction[direction].assign(count,ConservativeVector());
			for(std::size_t cell=0;cell<count;++cell){
				low[cell]=beginning[cell]+sourceDelta[cell];
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					if(flux.low[axis].size()!=OpenMACFaceCount3D(shape,axis)) return Fail(error,
						"fire solver open FCT face shape is invalid");
					low[cell]=low[cell]+scale*(flux.low[axis][lower]-flux.low[axis][upper]);
					correction[2*axis][cell]=scale*(flux.high[axis][lower]-flux.low[axis][lower]);
					correction[2*axis+1][cell]=-scale*(flux.high[axis][upper]-flux.low[axis][upper]);
				}
				const double tolerance=1024.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(low[cell][MethaneMassStateDimension]));
				if(!ConservativeStateFeasible(low[cell],ambientEnthalpy,adiabaticEnthalpy,
					tolerance) || !CertifiedMassConstraintSatisfied(low[cell],
					fuel.ConservativeReconstruction())) return Fail(error,
					"fire solver open low-order state is infeasible");
			}
			const std::size_t inequalityCount=4+MethaneSpeciesCount;
			std::vector<std::vector<double> > ratio(count,std::vector<double>(inequalityCount,1.0));
			for(std::size_t cell=0;cell<count;++cell) for(std::size_t inequality=0;
				inequality<inequalityCount;++inequality){
				double stateScale=1.0;for(std::size_t component=0;
					component<MethaneConservativeDimension;++component) stateScale=
					std::max(stateScale,std::fabs(low[cell][component]));
				const double budget=std::max(0.0,-InequalityValue(low[cell],inequality,
					ambientEnthalpy,adiabaticEnthalpy))+4096.0*
					std::numeric_limits<double>::epsilon()*stateScale;
				double requested=0.0;for(unsigned int direction=0;direction<6;++direction)
					requested+=std::max(0.0,InequalityValue(correction[direction][cell],
						inequality,ambientEnthalpy,adiabaticEnthalpy));
				ratio[cell][inequality]=requested>0.0?std::min(1.0,budget/requested):1.0;
			}
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				alpha[axis].assign(faceCount,1.0);
				for(std::size_t cell=0;cell<count;++cell){
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
							std::min(alpha[axis][face],ratio[cell][inequality]);
						if(InequalityValue(correction[2*axis][right],inequality,
							ambientEnthalpy,adiabaticEnthalpy)>0.0) alpha[axis][face]=
							std::min(alpha[axis][face],ratio[right][inequality]);
					}
				}
			}
			std::vector<ConservativeVector> candidate=low;
			for(std::size_t cell=0;cell<count;++cell) for(unsigned int axis=0;axis<3;++axis){
				const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
					z=cell/(shape.nx*shape.ny);
				const std::size_t coordinate=axis==0?x:(axis==1?y:z);
				const std::size_t extent=axis==0?shape.nx:(axis==1?shape.ny:shape.nz);
				if(coordinate+1>=extent) continue;
				const std::size_t face=OpenUpperFaceForCell3D(shape,cell,axis);
				const std::size_t right=PeriodicNext(shape,cell,axis);
				const ConservativeVector accepted=scale*alpha[axis][face]*(
					flux.high[axis][face]-flux.low[axis][face]);
				candidate[cell]=candidate[cell]-accepted;candidate[right]=candidate[right]+accepted;
			}
			for(std::size_t component=0;component<MethaneMassStateDimension;++component){
				bool absent=true;double scaleValue=1.0;
				for(std::size_t cell=0;cell<count;++cell){absent=absent&&
					beginning[cell][component]==0.0&&sourceDelta[cell][component]==0.0;
					scaleValue=std::max(scaleValue,std::fabs(candidate[cell][component]));}
				if(absent){const double envelope=4096.0*std::numeric_limits<double>::epsilon()*
					scaleValue;for(std::size_t cell=0;cell<count;++cell){if(std::fabs(
					candidate[cell][component])>envelope)return Fail(error,
					"fire solver open FCT created a finite absent inventory");
					candidate[cell][component]=0.0;}}
			}
			for(std::size_t cell=0;cell<count;++cell) if(!CertifiedMassConstraintSatisfied(
				candidate[cell],fuel.ConservativeReconstruction())) return Fail(error,
				"fire solver open FCT result violates the affine invariant");
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
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();result.assign(count,0.0);
			for(std::size_t cell=0;cell<count;++cell){
				ConservativeVector rate=(1.0/deltaTimeS)*sourceDelta[cell];
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					for(std::size_t component=0;component<MethaneMassStateDimension;++component)
						rate[component]+=(flux.nonadvectiveMass[axis][lower][component]-
							flux.nonadvectiveMass[axis][upper][component])/shape.cellWidthM;
					rate[MethaneMassStateDimension]+=(flux.nonadvectiveEnergy[axis][lower]-
						flux.nonadvectiveEnergy[axis][upper])/shape.cellWidthM;
				}
				if(!DivergenceFromDiscreteRate(state[cell],rate,temperature[cell],thermochemistry,
					result[cell],error)) return false;
			}
			return true;
		}

		inline OpenMACField3D OpenCompatibleMomentumFluxDivergence3D(
			const PeriodicMACShape& shape,
			const OpenFluxPair3D& flux,
			const std::array<std::vector<double>,3>& alpha,
			const OpenMACField3D& velocity
			)
		{
			const std::size_t count=shape.CellCount();
			std::array<std::vector<double>,3> gasFlux,cellVelocity,cellDivergence;
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				gasFlux[axis].assign(faceCount,0.0);cellVelocity[axis].assign(count,0.0);
				cellDivergence[axis].assign(count,0.0);
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
				for(std::size_t cell=0;cell<count;++cell) cellVelocity[axis][cell]=0.5*(
					velocity.component[axis][OpenLowerFaceForCell3D(shape,cell,axis)]+
					velocity.component[axis][OpenUpperFaceForCell3D(shape,cell,axis)]);
			}
			for(unsigned int component=0;component<3;++component) for(std::size_t cell=0;
				cell<count;++cell){
				for(unsigned int axis=0;axis<3;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					auto transported=[&](const std::size_t face,const bool upperFace){
						if(axis==component) return velocity.component[component][face];
						const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
							z=cell/(shape.nx*shape.ny);
						const std::size_t coordinate=axis==0?x:(axis==1?y:z);
						const std::size_t extent=axis==0?shape.nx:(axis==1?shape.ny:shape.nz);
						double value=cellVelocity[component][cell];unsigned int samples=1;
						if(upperFace&&coordinate+1<extent){value+=cellVelocity[component][
							PeriodicNext(shape,cell,axis)];++samples;}
						if(!upperFace&&coordinate){value+=cellVelocity[component][
							PeriodicPrevious(shape,cell,axis)];++samples;}
						return value/static_cast<double>(samples);
					};
					cellDivergence[component][cell]+=(gasFlux[axis][upper]*
						transported(upper,true)-gasFlux[axis][lower]*transported(lower,false))/
						shape.cellWidthM;
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
					double value=0.0;unsigned int samples=0;
					if(normal){value+=cellDivergence[component][shape.Index(component==0?
						normal-1:x,component==1?normal-1:y,component==2?normal-1:z)];++samples;}
					if(normal+1<normalCount){value+=cellDivergence[component][shape.Index(
						component==0?normal:x,component==1?normal:y,component==2?normal:z)];++samples;}
					result.component[component][OpenMACFaceIndex3D(shape,component,x,y,z)]=
						samples?value/static_cast<double>(samples):0.0;
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
			OpenBoundaryConfig3D openBoundary;
			double injectedTemperatureK;
			ConservativeAdvance3DConfig() : projectionTolerancePerS(0.0),dns(false),
				periodicBoundaries(true),injectedTemperatureK(0.0)
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
			std::string* error = 0
			)
		{
			const std::size_t count = shape.CellCount();
			std::vector<double> temperature;
			if( !InvertPeriodicTemperatures(state,thermochemistry,temperature,error) ) return false;
			std::vector<double> target(count,0.0), priorMassFlux(3*count,0.0);
			std::array<std::vector<double>,3> priorAlpha;
			std::vector<double> priorDiffusivity(count,0.0),priorConductivity(count,0.0),
				priorViscosity(count,0.0);
			result.picardResidualPerS.clear();
			for( std::size_t iteration=0; iteration<64; ++iteration ) {
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
					diffusivity,conductivity,fuel,thermochemistry,flux,error) ) return false;
				std::array<std::vector<double>,3> nextAlpha;
				if( solvePredictorLimiter ) {
					std::vector<ConservativeVector> predictor;
					if( !ApplyPeriodicSharedFCT3D(shape,state,flux,frozenSourceDelta,
						config.transport,fuel,thermochemistry,predictor,nextAlpha,error) ) return false;
				}
				std::vector<double> nextTarget;
				if( !PeriodicDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,flux,
					frozenSourceDelta,config.transport.deltaTimeS,thermochemistry,nextTarget,error) )
					return false;
				double targetResidual = 0.0, massResidual = 0.0, alphaResidual = 0.0,
					coefficientResidual = 0.0;
				for( std::size_t cell=0; cell<count; ++cell ) {
					targetResidual = std::max(targetResidual,std::fabs(nextTarget[cell]-target[cell]));
					for( unsigned int axis=0; axis<3; ++axis ) {
						const double massFlux = projection.momentumKGPerM2S.component[axis][cell];
						if( iteration ) massResidual = std::max(massResidual,std::fabs(
							massFlux-priorMassFlux[axis*count+cell])/shape.cellWidthM);
						priorMassFlux[axis*count+cell] = massFlux;
						if( solvePredictorLimiter && iteration ) alphaResidual = std::max(
							alphaResidual,std::fabs(nextAlpha[axis][cell]-priorAlpha[axis][cell]));
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
					alphaResidual,coefficientResidual}));
				if( solvePredictorLimiter ) priorAlpha = nextAlpha;
				target.swap(nextTarget);
				if( targetResidual <= config.projectionTolerancePerS && iteration &&
					massResidual <= config.projectionTolerancePerS &&
					alphaResidual <= config.projectionTolerancePerS &&
					coefficientResidual <= config.projectionTolerancePerS ) {
					// Reproject and rebuild once at the accepted target so every returned
					// coefficient, flux and momentum is consistent with the returned velocity.
					if( !ProjectPeriodicMACVelocity3D(shape,GasDensityFromConservative(state),
						unprojectedMomentum,target,config.transport.deltaTimeS,
						config.projectionTolerancePerS,result.projection,error) ||
						!BuildPeriodicStageTransport3D(shape,state,temperature,
							result.projection.velocityMPerS,config.dns,thermochemistry,transport,
							result.diffusivityM2PerS,result.conductivityWPerMK,
							result.dynamicViscosityPaS,error) ||
						!BuildPeriodicFluxPair3D(shape,state,temperature,result.projection.velocityMPerS,
							result.diffusivityM2PerS,result.conductivityWPerMK,fuel,
							thermochemistry,result.flux,error) ) return false;
					std::vector<double> verifiedTarget;
					if( !PeriodicDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,
						result.flux,frozenSourceDelta,config.transport.deltaTimeS,
						thermochemistry,verifiedTarget,error) ) return false;
					std::array<std::vector<double>,3> verifiedAlpha;
					if( solvePredictorLimiter ) {
						std::vector<ConservativeVector> verifiedPredictor;
						if( !ApplyPeriodicSharedFCT3D(shape,state,result.flux,frozenSourceDelta,
							config.transport,fuel,thermochemistry,verifiedPredictor,
							verifiedAlpha,error) ) return false;
					}
					double finalResidual=0.0;
					for( std::size_t cell=0;cell<count;++cell ) {
						finalResidual=std::max(finalResidual,std::fabs(verifiedTarget[cell]-
							target[cell]));
						for(unsigned int axis=0;axis<3;++axis){
							const double finalMass=result.projection.momentumKGPerM2S.component[
								axis][cell];
							finalResidual=std::max(finalResidual,std::fabs(finalMass-
								priorMassFlux[axis*count+cell])/shape.cellWidthM);
							if(solvePredictorLimiter) finalResidual=std::max(finalResidual,
								std::fabs(verifiedAlpha[axis][cell]-nextAlpha[axis][cell]));
						}
					}
					if(finalResidual>config.projectionTolerancePerS){
						target.swap(verifiedTarget);
						if(solvePredictorLimiter) priorAlpha=verifiedAlpha;
						continue;
					}
					result.divergenceTargetPerS = verifiedTarget;
					result.faceAlpha = solvePredictorLimiter ? verifiedAlpha : nextAlpha;
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
			ConservativeStage3D r0, r1, r2;
			ConservativeAdvance3DResult() : maximumDivergenceResidualPerS(0.0),
				maximumBoundaryHeadResidualPa(0.0) {}
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
		};

		struct OpenConservativeAdvance3DResult
		{
			std::vector<ConservativeVector> conservative;
			OpenMACField3D momentumKGPerM2S,velocityMPerS;
			std::vector<double> stepAverageDynamicPressurePa;
			std::array<std::vector<double>,3> faceAlpha;
			std::vector<double> divergenceHeunPerS;
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
			if(!BuildRelativeBuoyancyMomentumRate3D(shape,GasDensityFromConservative(state),
				config.openBoundary,config.gravityMPerS2,result,error)) return false;
			PeriodicMACField periodicVelocity,periodicViscous;
			for(unsigned int axis=0;axis<3;++axis){periodicVelocity.component[axis].resize(
				shape.CellCount());for(std::size_t cell=0;cell<shape.CellCount();++cell)
				periodicVelocity.component[axis][cell]=projection.velocityMPerS.component[axis][
					OpenUpperFaceForCell3D(shape,cell,axis)];}
			if(!RemainingMomentumRHS3D(shape,state,periodicVelocity,viscosity,
				std::vector<ConservativeVector>(shape.CellCount()),config.transport.deltaTimeS,
				config.openBoundary.ambientDensityKGPerM3,std::array<double,3>{{0.0,0.0,0.0}},
				periodicViscous,error)) return false;
			for(unsigned int axis=0;axis<3;++axis)for(std::size_t cell=0;cell<shape.CellCount();
				++cell){const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
					z=cell/(shape.nx*shape.ny);const std::size_t coordinate=axis==0?x:
					(axis==1?y:z);const std::size_t extent=axis==0?shape.nx:
					(axis==1?shape.ny:shape.nz);if(coordinate+1<extent) result.component[axis][
					OpenUpperFaceForCell3D(shape,cell,axis)]+=periodicViscous.component[axis][cell];}
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
					if(samples) result.component[axis][face]+=
						projection.velocityMPerS.component[axis][face]*phase/samples;
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
			const OpenMACProjection3DResult* stage0,
			const OpenMACProjection3DResult* stage1,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			OpenConservativeStage3D& result,
			std::string* error=0
			)
		{
			const std::size_t count=shape.CellCount();std::vector<double> temperature;
			if(!InvertPeriodicTemperatures(state,thermochemistry,temperature,error)) return false;
			std::vector<double> target(count,0.0),priorMass;
			std::array<std::vector<double>,3> priorAlpha;
			for(unsigned int axis=0;axis<3;++axis) priorMass.insert(priorMass.end(),
				OpenMACFaceCount3D(shape,axis),0.0);
			result.picardResidualPerS.clear();
			OpenMACProjection3DResult priorProjection;
			for(std::size_t iteration=0;iteration<64;++iteration){
				OpenMACProjection3DResult projection;
				OpenMACField3D nonpressure,unprojected=baseMomentum;
				if(iteration && !BuildOpenNonpressureMomentumRHS3D(shape,state,priorProjection,
					result.dynamicViscosityPaS,sourceDelta,config,nonpressure,error)) return false;
				if(iteration) for(unsigned int axis=0;axis<3;++axis) for(std::size_t face=0;
					face<unprojected.component[axis].size();++face) unprojected.component[axis][face]+=
					config.transport.deltaTimeS*nonpressure.component[axis][face];
				const bool finalStage=stage0&&stage1;
				const bool projectionOK=finalStage?ProjectPressureOpenMACVelocity3DFinal(shape,
					GasDensityFromConservative(state),unprojected,target,config.openBoundary,
					*stage0,*stage1,config.transport.deltaTimeS,config.projectionTolerancePerS,
					projection,error):ProjectPressureOpenMACVelocity3D(shape,
					GasDensityFromConservative(state),unprojected,target,config.openBoundary,
					config.transport.deltaTimeS,config.projectionTolerancePerS,projection,error);
				if(!projectionOK) return false;
				std::vector<double> diffusivity,conductivity,viscosity;
				if(!BuildOpenStageTransport3D(shape,state,temperature,projection.velocityMPerS,
					config.dns,thermochemistry,transport,diffusivity,conductivity,viscosity,error))
					return false;
				OpenMACField3D evaluatedNonpressure;
				if(!BuildOpenNonpressureMomentumRHS3D(shape,state,projection,viscosity,
					sourceDelta,config,evaluatedNonpressure,error)) return false;
				OpenFluxPair3D flux;
				if(!BuildOpenFluxPair3D(shape,state,temperature,projection,diffusivity,conductivity,
					config.openBoundary,config.transport.ambientTemperatureK,
					config.injectedTemperatureK,fuel,thermochemistry,flux,error)) return false;
				std::array<std::vector<double>,3> nextAlpha;
				if(predictorLimiter){std::vector<ConservativeVector> predictor;
					if(!ApplyOpenSharedFCT3D(shape,state,flux,sourceDelta,config.transport,fuel,
						thermochemistry,predictor,nextAlpha,error)) return false;}
				std::vector<double> nextTarget;
				if(!OpenDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,flux,
					sourceDelta,config.transport.deltaTimeS,thermochemistry,nextTarget,error)) return false;
				double residual=0.0,massResidual=0.0,alphaResidual=0.0;std::size_t offset=0;
				for(std::size_t cell=0;cell<count;++cell) residual=std::max(residual,
					std::fabs(nextTarget[cell]-target[cell]));
				for(unsigned int axis=0;axis<3;++axis){
					for(std::size_t face=0;face<projection.momentumKGPerM2S.component[axis].size();
						++face){
						if(iteration) massResidual=std::max(massResidual,std::fabs(
							projection.momentumKGPerM2S.component[axis][face]-
							priorMass[offset+face])/shape.cellWidthM);
						priorMass[offset+face]=projection.momentumKGPerM2S.component[axis][face];
					}
					offset+=projection.momentumKGPerM2S.component[axis].size();
					if(predictorLimiter&&iteration) for(std::size_t face=0;face<nextAlpha[axis].size();
						++face) alphaResidual=std::max(alphaResidual,std::fabs(nextAlpha[axis][face]-
						priorAlpha[axis][face]));
				}
				result.picardResidualPerS.push_back(std::max({residual,massResidual,alphaResidual}));
				target.swap(nextTarget);priorProjection=projection;
				if(predictorLimiter) priorAlpha=nextAlpha;
				result.dynamicViscosityPaS=viscosity;
				if(iteration&&residual<=config.projectionTolerancePerS&&
					massResidual<=config.projectionTolerancePerS&&
					alphaResidual<=config.projectionTolerancePerS){
					OpenMACProjection3DResult acceptedProjection;
					const bool acceptedOK=finalStage?ProjectPressureOpenMACVelocity3DFinal(shape,
						GasDensityFromConservative(state),unprojected,target,config.openBoundary,
						*stage0,*stage1,config.transport.deltaTimeS,config.projectionTolerancePerS,
						acceptedProjection,error):ProjectPressureOpenMACVelocity3D(shape,
						GasDensityFromConservative(state),unprojected,target,config.openBoundary,
						config.transport.deltaTimeS,config.projectionTolerancePerS,
						acceptedProjection,error);
					if(!acceptedOK) return false;
					std::vector<double> acceptedDiffusivity,acceptedConductivity,acceptedViscosity;
					if(!BuildOpenStageTransport3D(shape,state,temperature,
						acceptedProjection.velocityMPerS,config.dns,thermochemistry,transport,
						acceptedDiffusivity,acceptedConductivity,acceptedViscosity,error)) return false;
					OpenFluxPair3D acceptedFlux;
					if(!BuildOpenFluxPair3D(shape,state,temperature,acceptedProjection,
						acceptedDiffusivity,acceptedConductivity,config.openBoundary,
						config.transport.ambientTemperatureK,config.injectedTemperatureK,fuel,
						thermochemistry,acceptedFlux,error)) return false;
					std::vector<double> verifiedTarget;
					if(!OpenDivergenceTargetFromPhysicalFlux3D(shape,state,temperature,acceptedFlux,
						sourceDelta,config.transport.deltaTimeS,thermochemistry,verifiedTarget,error))
						return false;
					double verification=0.0;for(std::size_t cell=0;cell<count;++cell)
						verification=std::max(verification,std::fabs(verifiedTarget[cell]-target[cell]));
					if(verification>config.projectionTolerancePerS){target.swap(verifiedTarget);
						priorProjection=acceptedProjection;continue;}
					result.projection=acceptedProjection;result.flux=acceptedFlux;
					result.faceAlpha=nextAlpha;result.divergenceTargetPerS=verifiedTarget;
					result.diffusivityM2PerS=acceptedDiffusivity;
					result.conductivityWPerMK=acceptedConductivity;
					result.dynamicViscosityPaS=acceptedViscosity;
					return BuildOpenNonpressureMomentumRHS3D(shape,state,acceptedProjection,
						acceptedViscosity,sourceDelta,config,result.nonpressureMomentumRHS,error);
				}
			}
			return Fail(error,"fire solver open conservative Picard stage did not converge");
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
				config.transport.cellWidthM != shape.cellWidthM ) return Fail(error,
				"fire solver owning 3-D advance input is malformed");
			for( unsigned int axis=0; axis<3; ++axis ) if(
				beginningMomentum.component[axis].size() != count ) return Fail(error,
				"fire solver owning 3-D momentum shape is invalid");
			std::vector<ConservativeVector> sourceDelta;
			if( !FrozenPacketDeltas3D(frozenPacket,count,sourceDelta,error) ) return false;
			ConservativeAdvance3DResult candidate;
			if( !SolveConservativeStage3D(shape,beginning,beginningMomentum,sourceDelta,config,
				true,fuel,thermochemistry,transport,candidate.r0,error) ) return false;
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if( !ApplyPeriodicSharedFCT3D(shape,beginning,candidate.r0.flux,sourceDelta,
				config.transport,fuel,thermochemistry,predictor,predictorAlpha,error) ) return false;
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
				false,fuel,thermochemistry,transport,candidate.r1,error) ) return false;
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
				candidate.faceAlpha,error) ) return false;
			std::vector<double> acceptedTemperature;
			if( !InvertPeriodicTemperatures(candidate.conservative,thermochemistry,
				acceptedTemperature,error) || !PeriodicDivergenceTargetFromPhysicalFlux3D(shape,
				candidate.conservative,acceptedTemperature,averaged,sourceDelta,
				config.transport.deltaTimeS,thermochemistry,candidate.divergenceHeunPerS,error) )
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
			if( !SolveConservativeStage3D(shape,candidate.conservative,finalMomentum,sourceDelta,
				config,false,fuel,thermochemistry,transport,candidate.r2,error) ) return false;
			candidate.momentumKGPerM2S = candidate.r2.projection.momentumKGPerM2S;
			candidate.velocityMPerS = candidate.r2.projection.velocityMPerS;
			candidate.stepAverageDynamicPressurePa =
				candidate.r2.projection.stepAverageDynamicPressurePa;
			result = candidate;
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
			if(!FrozenPacketDeltas3D(frozenPacket,count,sourceDelta,error)) return false;
			OpenConservativeAdvance3DResult candidate;
			if(!SolveOpenConservativeStage3D(shape,beginning,beginningMomentum,sourceDelta,
				config,true,0,0,fuel,thermochemistry,transport,candidate.r0,error)) return false;
			std::vector<ConservativeVector> predictor;
			std::array<std::vector<double>,3> predictorAlpha;
			if(!ApplyOpenSharedFCT3D(shape,beginning,candidate.r0.flux,sourceDelta,
				config.transport,fuel,thermochemistry,predictor,predictorAlpha,error)) return false;
			const OpenMACField3D predictorAdvection=OpenCompatibleMomentumFluxDivergence3D(
				shape,candidate.r0.flux,predictorAlpha,candidate.r0.projection.velocityMPerS);
			OpenMACField3D predictorMomentum=beginningMomentum;
			for(unsigned int axis=0;axis<3;++axis) for(std::size_t face=0;face<
				predictorMomentum.component[axis].size();++face) predictorMomentum.component[axis][face]+=
				config.transport.deltaTimeS*(candidate.r0.nonpressureMomentumRHS.component[axis][face]-
				predictorAdvection.component[axis][face]);
			if(!SolveOpenConservativeStage3D(shape,predictor,predictorMomentum,sourceDelta,
				config,false,0,0,fuel,thermochemistry,transport,candidate.r1,error)) return false;
			OpenFluxPair3D averaged;
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
				fuel,thermochemistry,candidate.conservative,candidate.faceAlpha,error)) return false;
			std::vector<double> acceptedTemperature;
			if(!InvertPeriodicTemperatures(candidate.conservative,thermochemistry,
				acceptedTemperature,error) || !OpenDivergenceTargetFromPhysicalFlux3D(shape,
				candidate.conservative,acceptedTemperature,averaged,sourceDelta,
				config.transport.deltaTimeS,thermochemistry,candidate.divergenceHeunPerS,error))
				return false;
			const OpenMACField3D advection0=OpenCompatibleMomentumFluxDivergence3D(shape,
				candidate.r0.flux,candidate.faceAlpha,candidate.r0.projection.velocityMPerS);
			const OpenMACField3D advection1=OpenCompatibleMomentumFluxDivergence3D(shape,
				candidate.r1.flux,candidate.faceAlpha,candidate.r1.projection.velocityMPerS);
			OpenMACField3D finalMomentum=beginningMomentum;
			for(unsigned int axis=0;axis<3;++axis) for(std::size_t face=0;face<
				finalMomentum.component[axis].size();++face) finalMomentum.component[axis][face]+=
				0.5*config.transport.deltaTimeS*(candidate.r0.nonpressureMomentumRHS.component[
				axis][face]+candidate.r1.nonpressureMomentumRHS.component[axis][face]-
				advection0.component[axis][face]-advection1.component[axis][face]);
			if(!SolveOpenConservativeStage3D(shape,candidate.conservative,finalMomentum,
				sourceDelta,config,false,&candidate.r0.projection,&candidate.r1.projection,
				fuel,thermochemistry,transport,candidate.r2,error)) return false;
			candidate.momentumKGPerM2S=candidate.r2.projection.momentumKGPerM2S;
			candidate.velocityMPerS=candidate.r2.projection.velocityMPerS;
			candidate.stepAverageDynamicPressurePa=
				candidate.r2.projection.stepAverageDynamicPressurePa;
			result=candidate;return true;
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
			if(config.periodicBoundaries) return AdvancePeriodicConservative3DImplementation(
				shape,beginning,beginningMomentum,frozenPacket,config,fuel,thermochemistry,
				transport,result,error);
			OpenConservativeAdvance3DResult open;
			OpenMACField3D openBeginningMomentum;
			openBeginningMomentum.component=beginningMomentum.component;
			if(!AdvanceOpenConservative3DImplementation(shape,beginning,openBeginningMomentum,
				frozenPacket,config,fuel,thermochemistry,transport,open,error)) return false;
			ConservativeAdvance3DResult candidate;
			candidate.conservative=open.conservative;candidate.momentumKGPerM2S.component=
				open.momentumKGPerM2S.component;candidate.velocityMPerS.component=
				open.velocityMPerS.component;candidate.stepAverageDynamicPressurePa=
				open.stepAverageDynamicPressurePa;candidate.faceAlpha=open.faceAlpha;
			candidate.divergenceHeunPerS=open.divergenceHeunPerS;
			candidate.maximumDivergenceResidualPerS=
				open.r2.projection.maximumDivergenceResidualPerS;
			candidate.maximumBoundaryHeadResidualPa=
				open.r2.projection.maximumBoundaryHeadResidualPa;
			result=candidate;return true;
		}

#endif
