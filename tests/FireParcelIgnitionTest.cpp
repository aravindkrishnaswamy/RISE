#include "../src/Library/Utilities/FireProductionSourceKernel.h"
#include <cstdio>
using namespace RISE;
using namespace RISE::FireSim;
namespace {
MethaneCellState Mixture(const FireSimulationMethaneRecord& fuel,double yf,double temperature)
{
 MethaneCellState state;state.temperatureK=temperature;
 double inverseWeight=0.0;
 for(std::size_t k=0;k<MethaneSpeciesCount;++k){
  state.constituent[k]=(1.0-yf)*fuel.AmbientMassFractions()[k]+(k==MethaneCH4?yf:0.0);
  if(k<MethaneCarbon)inverseWeight+=state.constituent[k]/fuel.FindSpecies(fuel.SpeciesOrder()[k].c_str())->molecularWeightKGPerKMol;
 }
 const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*temperature*inverseWeight);
 for(double& q:state.constituent)q*=density;
 state.rhoTotalZ=yf*density;std::string error;
 if(!fuel.MixtureSensibleEnergyBySpeciesOrderJPerM3(state.constituent.data(),state.constituent.size(),temperature,state.sensibleEnergyJPerM3,&error)){
  std::fprintf(stderr,"mixture construction: %s\n",error.c_str());std::exit(2);
 }
 return state;
}
}
int main()
{
 const auto& fuel=FireSimulationMethaneRecord::PhysicalV1();
 const auto& transport=FireSimulationTransportRecord::OpenV1();
 // A hot seeded flame, a two-cell cold lean chain, a fuel-free gap, then a
 // warm but unseeded pocket. The first cold lean cell is the regression witness.
 IgnitionGrid grid;grid.nx=6;grid.ny=grid.nz=1;
 grid.cells={Mixture(fuel,.05,900),Mixture(fuel,.001,300),Mixture(fuel,.001,300),
             Mixture(fuel,0,300),Mixture(fuel,.05,700),Mixture(fuel,.001,300)};
 grid.pilotMask.assign(6,false);
 std::vector<bool> eligible;std::string error;
 if(!BuildIgnitionEligibility(grid,fuel,fuel,transport,eligible,&error)){
  std::fprintf(stderr,"eligibility failed: %s\n",error.c_str());return 1;
 }
 if(!eligible[0]||!eligible[1]||eligible[2]||eligible[4]||eligible[5]){
  std::fprintf(stderr,"FAIL: hot seed/cold edge/remote pocket: %d %d %d %d\n",int(eligible[0]),int(eligible[1]),int(eligible[4]),int(eligible[5]));return 1;
 }
 bool parcel=false;
 const auto lean=Mixture(fuel,.001,300);
 if(!StoichiometricParcelPassesCFT(lean,fuel,fuel,transport.CriticalFlameTemperatureK(),parcel,&error)||!parcel)return 1;
 double wholeCellTemperature=0;
 if(!TrialAdiabaticTemperatureK(lean,fuel,fuel,wholeCellTemperature,&error)||wholeCellTemperature>=transport.CriticalFlameTemperatureK())return 1;

 // Direct unnormalized FDS enthalpy oracle, including lean and rich mixtures.
 for(double yf:std::array<double,5>{{.001,.01,.05,.2,.8}})
  for(double temperature:std::array<double,3>{{300,600,900}}){
   const auto state=Mixture(fuel,yf,temperature);
   const double F=state.constituent[MethaneCH4],O=state.constituent[MethaneO2];
   const double st=fuel.StoichiometricOxygenKGPerKGFuel();
   const double phi=std::min(1.,st*F/O),extent=std::min(F,O/st);
   std::array<double,MethaneSpeciesCount> h0,h1;
   if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(temperature,h0.data(),h0.size(),&error)||
      !fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(transport.CriticalFlameTemperatureK(),h1.data(),h1.size(),&error))return 1;
   double initial=extent*fuel.LowerHeatingValueJPerKG(),critical=0;
   for(std::size_t k=0;k<MethaneSpeciesCount;++k){
    const double before=state.constituent[k]*(k==MethaneCH4?1.:phi);
    initial+=before*h0[k];critical+=(before+extent*fuel.PrimaryReactionDelta()[k])*h1[k];
   }
   bool actual=false;
   if(!StoichiometricParcelPassesCFT(state,fuel,fuel,transport.CriticalFlameTemperatureK(),actual,&error)||actual!=(initial>=critical)){
    std::fprintf(stderr,"FAIL: FDS enthalpy oracle yf=%g T=%g\n",yf,temperature);return 1;}
  }
 // Six face neighbors qualify; a diagonal reached through those waived cells does not.
 IgnitionGrid cube;cube.nx=cube.ny=cube.nz=3;cube.cells.assign(27,Mixture(fuel,0,300));cube.pilotMask.assign(27,false);
 cube.cells[13]=Mixture(fuel,.05,900);
 const std::array<std::size_t,6> faces={{12,14,10,16,4,22}};
 for(std::size_t cell:faces)cube.cells[cell]=lean;
 cube.cells[17]=lean;
 if(!BuildIgnitionEligibility(cube,fuel,fuel,transport,eligible,&error)||eligible[17])return 1;
 for(std::size_t cell:faces)if(!eligible[cell]){std::fprintf(stderr,"FAIL: face %zu excluded\n",cell);return 1;}
 std::puts("PASS: lean parcel vs whole cell; one-cell flame edge; no cold chaining; seedless remote exclusion");
 return 0;
}
