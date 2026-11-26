// r271 check of evaluate_resident_transport_compact (read-only). The qualification comparator runs the
// production transport pipeline on a retained r270 state; run once per switch setting, then compare the
// GPU change in diffusivity, (D_on - D_off)*Sc_t, with an independent fp64 nu_t(compact) - nu_t(wide).
// argv: dump CHECKPOINT OUT.bin | compare CHECKPOINT OFF.bin ON.bin
#define main FireSequenceSuiteMain
#include "../../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc<4)return 1;const std::string mode=argv[1];MethaneRunCheckpoint c;std::string error;
 if(!LoadMethaneRunCheckpoint(argv[2],c,error)){std::cerr<<error<<'\n';return 2;}
 FireProductionResidentTransportComparatorRequest q;q.shape.nx=c.dimensions[0];q.shape.ny=c.dimensions[1];q.shape.nz=c.dimensions[2];
 q.shape.cellWidthM=float(c.cellWidthM);q.stage=FireProductionProjectedHeunStage::R0;q.boundary.fill(FireProductionProjectionWall);
 q.attemptIdentity=UINT64_C(0x2710000000000001);q.parentCandidateIdentity=UINT64_C(0x2710000000000002);q.projectionIdentity=UINT64_C(0x2710000000000003);
 const size_t cells=q.shape.CellCount(),nx=q.shape.nx,ny=q.shape.ny,nz=q.shape.nz;
 for(unsigned s=0;s<6;++s)q.fuelInletBoundaryFace[s].assign(s<2?ny*nz:(s<4?nx*nz:nx*ny),0u);
 q.conservativeValues.assign(9*cells,0.f);q.temperatureK.resize(cells);
 for(size_t i=0;i<cells;++i){const ConservativeVector v=ToConservativeVector(c.states[i]);
  for(size_t k=0;k<MethaneConservativeDimension;++k)q.conservativeValues[k*cells+i]=float(v[k]);q.temperatureK[i]=float(c.states[i].temperatureK);}
 for(int a=0;a<3;++a){q.projectedVelocityMPerS[a].resize(c.velocity.component[a].size());
  for(size_t f=0;f<c.velocity.component[a].size();++f)q.projectedVelocityMPerS[a][f]=float(c.velocity.component[a][f]);}
 if(mode=="dump"){FireProductionResidentTransportComparatorResult r;
  std::uint64_t certified=0;FireProductionResidentTransportMetalWorkingSetBytes(q.shape,certified);
  bool ok=false;for(int attempt=0;attempt<2&&!ok;++attempt){ok=EvaluateFireProductionResidentTransportMetalComparator(q,r,&error);
   std::cerr<<"attempt "<<attempt<<" ok="<<ok<<" commits="<<r.commandCommitCount<<" reads="<<r.terminalStagingCount<<" certified="<<certified<<" actual="<<r.actualMetalAllocationBytes<<" "<<error<<'\n';}
  if(!ok)return 3;
  std::ofstream o(argv[3],std::ios::binary);o.write(reinterpret_cast<const char*>(r.diffusivityM2PerS.data()),r.diffusivityM2PerS.size()*4);
  std::cout<<"compact="<<FireProductionForceMetalCompactViscousStencil()<<" values="<<r.diffusivityM2PerS.size()<<'\n';return o?0:4;}
 if(mode!="compare"||argc!=5)return 1;
 auto load=[&](const char* p){std::vector<float> v(cells);std::ifstream f(p,std::ios::binary);f.read(reinterpret_cast<char*>(v.data()),cells*4);return v;};
 const auto off=load(argv[3]),on=load(argv[4]);const double dx=q.shape.cellWidthM,Sc=FireSimulationTransportRecord::OpenV1().TurbulentSchmidt();
 auto face=[&](int a,size_t x,size_t y,size_t z){return a==0?x+(nx+1)*(y+ny*z):a==1?x+nx*(y+(ny+1)*z):x+nx*(y+ny*z);};
 auto F=[&](int a,size_t x,size_t y,size_t z){return double(q.projectedVelocityMPerS[a][face(a,x,y,z)]);};
 auto U=[&](int a,size_t x,size_t y,size_t z){return .5*(F(a,x,y,z)+F(a,x+(a==0),y+(a==1),z+(a==2)));};
 auto vreman=[&](const double g[3][3]){double a2=0,b[3][3]={};for(int i=0;i<3;++i)for(int j=0;j<3;++j){a2+=g[i][j]*g[i][j];for(int m=0;m<3;++m)b[i][j]+=dx*dx*g[m][i]*g[m][j];}
  const double B=b[0][0]*b[1][1]-b[0][1]*b[0][1]+b[0][0]*b[2][2]-b[0][2]*b[0][2]+b[1][1]*b[2][2]-b[1][2]*b[1][2];return a2==0?0.:.07*std::sqrt(std::max(0.,B)/a2);};
 double maxErr=0,maxRef=0,sumGpu=0,sumCpu=0;size_t n=0;
 for(size_t z=1;z+1<nz;++z)for(size_t y=1;y+1<ny;++y)for(size_t x=1;x+1<nx;++x){double gw[3][3],gc[3][3];
  for(int d=0;d<3;++d)for(int k=0;k<3;++k){size_t lo[3]={x,y,z},hi[3]={x,y,z};--lo[d];++hi[d];
   gw[d][k]=(U(k,hi[0],hi[1],hi[2])-U(k,lo[0],lo[1],lo[2]))/(2*dx);gc[d][k]=d==k?(F(k,x+(k==0),y+(k==1),z+(k==2))-F(k,x,y,z))/dx:gw[d][k];}
  const size_t i=x+nx*(y+ny*z);const double cpu=vreman(gc)-vreman(gw),gpu=(double(on[i])-double(off[i]))*Sc;
  maxErr=std::max(maxErr,std::abs(gpu-cpu));maxRef=std::max(maxRef,std::abs(cpu));sumGpu+=gpu;sumCpu+=cpu;++n;}
 std::cout<<std::setprecision(6)<<"interior cells "<<n<<"\nmean delta nu_t GPU "<<sumGpu/n<<" fp64 "<<sumCpu/n
  <<"\nmax |GPU-fp64| "<<maxErr<<" (max |delta| "<<maxRef<<", ratio "<<maxErr/maxRef<<")\n";
 return maxErr<=1e-3*maxRef+1e-8?0:5;
}
