// Read-only extension of r249's retained-state probe. No stepping or simulation writes.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
namespace {
bool ParcelPass(const MethaneCellState& s,const FireSimulationMethaneRecord& f,
 const std::array<double,MethaneSpeciesCount>& hc,bool& passes,std::string& e)
{
 passes=false;const double F=s.constituent[MethaneCH4],O=s.constituent[MethaneO2];
 if(F<=0.0||O<=0.0)return true;
 const double st=f.StoichiometricOxygenKGPerKGFuel();
 const bool lean=F<=O/st;
 // Parcel per unit original fuel: phi/F = st/O in lean cells, 1/F otherwise.
 // This avoids subtracting almost the entire cell energy in the dilute limit.
 const double scale=lean?st/O:1.0/F;
 const double extent=lean?1.0:(O/F)/st;
 std::array<double,MethaneSpeciesCount> h;
 if(!f.SensibleEnthalpiesBySpeciesOrderJPerKG(s.temperatureK,h.data(),h.size(),&e))return false;
 double available=extent*f.LowerHeatingValueJPerKG(),required=0;
 for(size_t k=0;k<MethaneSpeciesCount;++k){
  const double q=k==MethaneCH4?1.0:s.constituent[k]*scale;
  const double post=q+extent*f.PrimaryReactionDelta()[k];
  available+=q*h[k];required+=post*hc[k];
 }
 passes=available>=required;return true;
}
std::vector<bool> Reach(const IgnitionGrid& g,const std::vector<bool>& vertices,const std::vector<bool>& starts)
{
 const size_t n=g.cells.size(),plane=g.nx*g.ny;std::vector<bool> seen(n,false);
 std::queue<size_t> pending;
 for(size_t i=0;i<n;++i)if(starts[i]&&vertices[i]){seen[i]=true;pending.push(i);}
 while(!pending.empty()){
  const size_t i=pending.front();pending.pop();const size_t x=i%g.nx,y=(i/g.nx)%g.ny,z=i/plane;
  const size_t neighbors[6]={x?i-1:n,x+1<g.nx?i+1:n,y?i-g.nx:n,y+1<g.ny?i+g.nx:n,z?i-plane:n,z+1<g.nz?i+plane:n};
  for(size_t j:neighbors)if(j<n&&vertices[j]&&!seen[j]){seen[j]=true;pending.push(j);}
 }
 return seen;
}
}
int main(int argc,char** argv)
{
 if(argc!=3)return 1;
 std::ifstream inputs(argv[1]);std::ofstream profile(argv[2]);
 if(!inputs||!profile)return 2;
 profile<<std::setprecision(17)<<"step,time_s,z,height_m,total_kg,a_kg,b_kg,c_kg,d_kg,e_kg\n";
 std::cout<<std::setprecision(17)<<"step,time_s,total_kg,a_kg,b_kg,c_kg,d_kg,e_kg,a_graph_kg,b_graph_kg,seed_cells,parcel_pass_kg,c_new_kg,c_top_donor_kg_s,d_top_donor_kg_s\n";
 const auto& f=FireSimulationMethaneRecord::PhysicalV1();const auto& tr=FireSimulationTransportRecord::OpenV1();
 std::array<double,MethaneSpeciesCount> hc;std::string e;
 if(!f.SensibleEnthalpiesBySpeciesOrderJPerKG(tr.CriticalFlameTemperatureK(),hc.data(),hc.size(),&e))return 3;
 // Small offline controls: disconnected cold pocket remains excluded; no seed means no component.
 IgnitionGrid control;control.nx=5;control.ny=1;control.nz=1;control.cells.resize(5);
 const std::vector<bool> vertices={true,true,false,true,true},starts={true,false,false,false,false};
 if(Reach(control,vertices,starts)!=std::vector<bool>({true,true,false,false,false})||
    Reach(control,vertices,std::vector<bool>(5,false))!=std::vector<bool>(5,false))return 11;
 const double st=f.StoichiometricOxygenKGPerKGFuel();
 for(double airScale:std::array<double,3>{{1.,10.,.1}}){
  MethaneCellState state;state.temperatureK=300.;state.constituent.fill(0.);
  state.constituent[MethaneCH4]=1.;state.constituent[MethaneO2]=st*airScale;
  state.constituent[MethaneN2]=st*airScale*f.AmbientMassFractions()[MethaneN2]/f.AmbientMassFractions()[MethaneO2];
  if(!f.MixtureSensibleEnergyBySpeciesOrderJPerM3(state.constituent.data(),state.constituent.size(),300.,state.sensibleEnergyJPerM3,&e))return 12;
  bool pass=false;if(!ParcelPass(state,f,hc,pass,e))return 13;
  if((airScale>=1.&&!pass)||(airScale==.1&&pass))return 14;
 }
 std::cerr<<"OFFLINE_CONTROLS disconnected_pocket=pass seedless=pass stoichiometric=pass lean=pass rich=pass\n";
 std::string path;
 while(std::getline(inputs,path)){
  MethaneRunCheckpoint c;
  if(!LoadMethaneRunCheckpoint(path,c,e)){std::cerr<<path<<' '<<e;return 4;}
  if(c.dimensions!=std::array<size_t,3>{{69,69,106}}||c.simulationTimeS<10.466040127156969)return 5;
  IgnitionGrid g;g.nx=c.dimensions[0];g.ny=c.dimensions[1];g.nz=c.dimensions[2];g.cells=std::move(c.states);
  const size_t n=g.cells.size(),plane=g.nx*g.ny;
  FireCase::AuthoredV1 authored;authored.poolDiameterM=.30;authored.seed=1234;
  FireCase::DerivedV1 derived;derived.nx=g.nx;derived.ny=g.ny;derived.nz=g.nz;derived.cellWidthM=c.cellWidthM;
  derived.extentXM=g.nx*c.cellWidthM;derived.extentYM=g.ny*c.cellWidthM;
  derived.sourceAreaM2=3.141592653589793238462643383279502884*.15*.15;
  std::vector<uint8_t> mask;std::vector<double> pattern;
  if(!FireCase::BuildPilotMask(authored,derived,mask,e)||!FireCase::BuildSourcePattern(authored,derived,pattern,e))return 6;
  g.pilotMask.resize(n,false);
  for(size_t i=0;i<n;++i)g.pilotMask[i]=ProductionPilotCommandCell(i,g.nx,g.ny,mask,pattern);
  std::vector<bool> aGraph;
  if(!BuildIgnitionEligibility(g,f,f,tr,aGraph,&e)){std::cerr<<e;return 7;}
  std::vector<bool> parcel(n,false),warm(n,false),seeds(n,false);
  size_t seedCount=0;
  for(size_t i=0;i<n;++i){
   bool passes=false;if(!ParcelPass(g.cells[i],f,hc,passes,e))return 8;parcel[i]=passes;
   warm[i]=parcel[i]&&g.cells[i].temperatureK>f.PilotTemperatureK();
   seeds[i]=warm[i]&&(g.pilotMask[i]||g.cells[i].temperatureK>f.AutoignitionTemperatureK());
   if(seeds[i])++seedCount;
  }
  const auto bGraph=Reach(g,warm,seeds);
  const auto connected=Reach(g,parcel,bGraph); // existing warm seeded components; no new cold seeds
  std::vector<bool> edge=warm;
  for(size_t i=0;i<n;++i)if(parcel[i]&&!warm[i]){
   const size_t x=i%g.nx,y=(i/g.nx)%g.ny,z=i/plane;
   const size_t neighbors[6]={x?i-1:n,x+1<g.nx?i+1:n,y?i-g.nx:n,y+1<g.ny?i+g.nx:n,z?i-plane:n,z+1<g.nz?i+plane:n};
   for(size_t j:neighbors)if(j<n&&warm[j]){edge[i]=true;break;}
  }
  const auto edgeGraph=Reach(g,edge,seeds);
  std::array<std::vector<bool>,5> gates={{aGraph,bGraph,connected,parcel,edgeGraph}};
  // Preserve the production floor-holder exception, evaluating only its CFT trial differently.
  for(size_t i=0;i<plane;++i){
   bool holder=false;
   if(!ProductionEstablishedFlameHolderEligible(i,g.nx,g.ny,mask,pattern,true,g.cells[i],f,tr,holder,&e))return 9;
   gates[0][i]=gates[0][i]||holder;
   if(ProductionSourceBoundaryContactCell(i,g.nx,g.ny,mask,pattern)&&parcel[i]){
    gates[1][i]=true;gates[2][i]=true;gates[4][i]=true;
   }
  }
  const double dx=static_cast<float>(c.cellWidthM),dv=dx*dx*dx,area=dx*dx;
  std::array<double,5> totals={{0,0,0,0,0}};double all=0,aGraphMass=0,bGraphMass=0,passMass=0,newMass=0,cTop=0,dTop=0;
  for(size_t z=0;z<g.nz;++z){
   std::array<double,5> layer={{0,0,0,0,0}};double layerAll=0;
   for(size_t j=0;j<plane;++j){const size_t i=z*plane+j;const double F=g.cells[i].constituent[MethaneCH4],m=F*dv;
    layerAll+=m;if(aGraph[i])aGraphMass+=m;if(bGraph[i])bGraphMass+=m;if(parcel[i])passMass+=m;
    if(gates[2][i]&&!gates[0][i])newMass+=m;
    for(size_t v=0;v<5;++v)if(gates[v][i])layer[v]+=m;
    if(z==g.nz-1){const double rate=F*std::max(0.,c.velocity.component[2][g.nz*plane+j])*area;
     if(gates[2][i])cTop+=rate;if(gates[3][i])dTop+=rate;}
   }
   profile<<c.acceptedSteps<<','<<c.simulationTimeS<<','<<z<<','<<(z+.5)*dx<<','<<layerAll;
   all+=layerAll;for(size_t v=0;v<5;++v){profile<<','<<layer[v];totals[v]+=layer[v];}profile<<'\n';
  }
  std::cout<<c.acceptedSteps<<','<<c.simulationTimeS<<','<<all;
  for(double x:totals)std::cout<<','<<x;
  std::cout<<','<<aGraphMass<<','<<bGraphMass<<','<<seedCount<<','<<passMass<<','<<newMass<<','<<cTop<<','<<dTop<<std::endl;
  if(!std::cout||!profile)return 10;
 }
 return 0;
}
