// Read-only source reconstruction on retained r251 beginnings; never advances a solver stage.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=5)return 1;
 std::ifstream list(argv[1]);std::ofstream out(argv[3]);std::string error;
 MethaneRunCheckpoint final;
 if(!list||!out||!LoadMethaneRunCheckpoint(argv[2],final,error)){std::cerr<<error;return 2;}
 const auto& fuel=FireSimulationMethaneRecord::PhysicalV1();const auto& tr=FireSimulationTransportRecord::OpenV1();
 FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();authored.poolDiameterM=.3;authored.heatReleaseRateKW=33.;authored.envelope={{0.,1.}};authored.durationS=25.091040127156969;authored.quality="dstar";authored.numericDStarTier=8.;authored.seed=1234;authored.outputFramesPerS=16.;authored.plumeLaw=true;
 FireCase::RecordV1 record;const auto aerosol=AerosolRecord(),chem=SyntheticChemRecord();
 std::vector<std::string> refs={RISECBOR64::SHA256Hex(fuel.RecordBytes()),RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),RISECBOR64::SHA256Hex(tr.RecordBytes()),RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem),ProductionBurnerPolicyId()};
 if(!FireCase::BuildMethaneV1(authored,fuel,refs,record,error)){std::cerr<<error;return 3;}
 if(record.caseRecordId!=final.caseRecordId){std::cerr<<"case mismatch "<<record.caseRecordId<<' '<<final.caseRecordId;return 4;}
 PeriodicMACShape shape;shape.nx=record.derived.nx;shape.ny=record.derived.ny;shape.nz=record.derived.nz;shape.cellWidthM=record.derived.cellWidthM;const size_t n=shape.CellCount(),plane=shape.nx*shape.ny;
 MethaneCellState ambient,injected;ambient.temperatureK=injected.temperatureK=300.;
 auto state=[&](const std::vector<double>& f,bool inject,MethaneCellState& s){double iw=0.;for(size_t k=0;k<MethaneCarbon;++k)iw+=f[k]/fuel.FindSpecies(fuel.SpeciesOrder()[k].c_str())->molecularWeightKGPerKMol;const double rho=fuel.ThermodynamicPressurePa()/(8314.46261815324*300.*iw);for(size_t k=0;k<MethaneSpeciesCount;++k)s.constituent[k]=rho*f[k];s.rhoTotalZ=inject?s.TotalDensity():0.;return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(s),300.,s.sensibleEnergyJPerM3,&error);};
 if(!state(fuel.AmbientMassFractions(),false,ambient)||!state(fuel.InjectedMassFractions(),true,injected)){std::cerr<<error;return 5;}
 std::vector<double> pattern;std::vector<uint8_t> pilot;
 if(!FireCase::BuildSourcePattern(authored,record.derived,pattern,error)||!FireCase::BuildPilotMask(authored,record.derived,pilot,error)){std::cerr<<error;return 6;}
 OpenBoundaryConfig3D boundary;boundary.ambientDensityKGPerM3=ambient.GasDensity();boundary.injectedGasDensityKGPerM3=injected.GasDensity();boundary.ambientState=ToConservativeVector(ambient);boundary.injectedState=ToConservativeVector(injected);const double referenceVelocity=std::sqrt(9.80665*record.derived.characteristicDiameterM);boundary.velocityToleranceMPerS=1.e-3*referenceVelocity;boundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*boundary.velocityToleranceMPerS;boundary.bottomFuelMask.resize(plane,false);boundary.bottomFuelMassFluxKGPerM2S.resize(plane,0.);
 for(size_t i=0;i<plane;++i)if(pattern[i]!=0.){boundary.bottomFuelMask[i]=true;boundary.bottomFuelMassFluxKGPerM2S[i]=record.derived.nominalFuelFluxKGPerM2S*pattern[i];}
 out<<std::setprecision(17)<<"time_s,accepted_step,weight,z,height_m,next_dt_s,endpoint_uses_last_dt,total_heat_W,warm_heat_W,edge_heat_W,holder_cold_heat_W,unclassified_cold_heat_W,warm_burning_cells,edge_burning_cells,holder_cold_burning_cells,unclassified_cold_burning_cells,cold_parcel_ineligible_fuel_kg\n";
 std::ofstream check(argv[4]);check<<std::setprecision(17)<<"time_s,step,total_heat_W,chemical_J,dt_s,from_budget_W,relative_difference,endpoint\n";
 double weight;std::string path;
 while(list>>weight>>path){
  if(weight<=0.)continue;MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 7;}
  if(c.dimensions!=final.dimensions||c.states.size()!=n)return 8;
  const bool endpoint=c.acceptedSteps==final.acceptedSteps;const double dt=endpoint?c.lastAcceptedStepS:final.values.acceptedTimeStepHistoryS.at(c.acceptedSteps);
  std::vector<ConservativeVector> q(n);std::vector<double> temp(n);IgnitionGrid grid;grid.nx=shape.nx;grid.ny=shape.ny;grid.nz=shape.nz;grid.cells=c.states;grid.pilotMask.resize(n,false);
  RISE::FireProductionFrozenMethaneSourceRequest req;req.shape.nx=shape.nx;req.shape.ny=shape.ny;req.shape.nz=shape.nz;req.shape.cellWidthM=static_cast<float>(shape.cellWidthM);req.timeStepS=static_cast<float>(dt);req.beginningTimeS=c.simulationTimeS;req.attemptIdentity=c.acceptedSteps+1;req.caseRecordEnvelope=record.envelopeBytes;req.beginningConservativeValues.resize(9*n);req.eligibilityBeginningConservativeValues.resize(9*n);req.pilotCommandMask.resize(n);req.sourceBoundaryContactMask.resize(n);req.mixingTimeS.resize(n);req.pilotEstablished=true;req.predictiveRadiation=false;req.workerCount=8;
  for(size_t i=0;i<n;++i){q[i]=ToConservativeVector(c.states[i]);temp[i]=c.states[i].temperatureK;double ratio=0.,t=0.;if(!fuel.InvertAcceptedConservativeStateByComponentOrder(q[i].value.data(),q[i].value.size(),record.derived.pilotAmbientTemperatureK,record.derived.maximumAcceptedTemperatureK,FireStateProducerPrecision::Binary32,t,ratio,&error)){std::cerr<<error;return 9;}grid.cells[i].temperatureK=t;grid.pilotMask[i]=ProductionPilotCommandCell(i,shape.nx,shape.ny,pilot,pattern);req.pilotCommandMask[i]=grid.pilotMask[i]?1:0;req.sourceBoundaryContactMask[i]=ProductionSourceBoundaryContactCell(i,shape.nx,shape.ny,pilot,pattern)?1:0;for(size_t k=0;k<9;++k)req.beginningConservativeValues[k*n+i]=req.eligibilityBeginningConservativeValues[k*n+i]=static_cast<float>(q[i][k]);}
  OpenMACField3D v;v.component=c.velocity.component;std::vector<CellTransportEvaluation> transport;
  if(!BuildOpenStageTransportEvaluations3D(shape,q,temp,v,boundary,false,fuel,tr,FireStateProducerPrecision::Binary32,transport,&error,8)){std::cerr<<error;return 10;}
  for(size_t i=0;i<n;++i)if(!ComputeMixingTimeS(c.states[i],transport[i],tr,shape.cellWidthM,ambient.GasDensity(),9.80665,false,req.mixingTimeS[i],&error)){std::cerr<<error;return 11;}
  std::vector<bool> eligible;if(!BuildIgnitionEligibility(grid,fuel,fuel,tr,eligible,&error)){std::cerr<<error;return 12;}
  RISE::FireProductionFrozenSourcePacketSeal source;if(!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(req,source,&error)){std::cerr<<error;return 13;}
  const double dx=req.shape.cellWidthM,dv=dx*dx*dx;std::array<double,8> all={};
  for(size_t z=0;z<shape.nz;++z){std::array<double,8> layer={};double blocked=0.;
   for(size_t j=0;j<plane;++j){const size_t i=z*plane+j;const auto& cell=grid.cells[i];const double h=source.GasHeatReleaseWPerM3()[i]*dv;if(h<0.||!std::isfinite(h))return 14;
    size_t cls=0;if(cell.temperatureK>fuel.PilotTemperatureK())cls=0;else if(eligible[i])cls=1;else if(z==0&&req.sourceBoundaryContactMask[i])cls=2;else cls=3;
    layer[cls]+=h;if(h>0.)layer[4+cls]+=1.;
    if(!eligible[i]&&cell.temperatureK<=fuel.PilotTemperatureK()&&cell.constituent[MethaneCH4]>0.&&cell.constituent[MethaneO2]>0.){bool pass=false;if(!StoichiometricParcelPassesCFT(cell,fuel,fuel,tr.CriticalFlameTemperatureK(),pass,&error))return 15;if(pass)blocked+=cell.constituent[MethaneCH4]*dv;}
   }
   double sum=0.;for(size_t k=0;k<4;++k)sum+=layer[k];out<<c.simulationTimeS<<','<<c.acceptedSteps<<','<<weight<<','<<z<<','<<(z+.5)*shape.cellWidthM<<','<<dt<<','<<endpoint<<','<<sum;for(double value:layer)out<<','<<value;out<<','<<blocked<<'\n';for(size_t k=0;k<8;++k)all[k]+=layer[k];
  }
  double total=0.;for(size_t k=0;k<4;++k)total+=all[k];double chemical=0.;
  if(!endpoint){std::ostringstream budget;budget<<"/Volumes/SSD/RISE/fire_production_calibration/r251_tier8_full_window/budgets/step_"<<std::setw(6)<<std::setfill('0')<<c.acceptedSteps+1<<".csv";std::ifstream b(budget.str());std::string line;while(std::getline(b,line))if(line.rfind("# step_source ",0)==0){chemical=std::stod(line.substr(14));break;}}
  check<<c.simulationTimeS<<','<<c.acceptedSteps<<','<<total<<','<<chemical<<','<<dt<<','<<(endpoint?0.:chemical/dt)<<','<<(endpoint?0.:total/(chemical/dt)-1.)<<','<<endpoint<<'\n';check.flush();out.flush();std::cerr<<"source_only "<<c.acceptedSteps<<' '<<c.simulationTimeS<<" total_W="<<total<<" edge_W="<<all[1]<<" unclassified_W="<<all[3]<<'\n';
 }
 return out&&check?0:16;
}
