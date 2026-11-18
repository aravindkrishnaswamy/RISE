// r271 burner face (read-only): composition of the lowest cell layers in retained RISE checkpoints.
// Per layer k=0..3 and radial shell (|r-a|<dx/2, a = 0..0.31 m), time-and-shell means of T (K), gas density (kg/m3),
// mixture molecular weight M = rho R T / p (kg/kmol, comparable with FDS's DENSITY and TEMPERATURE slices), Y_CH4, Y_O2,
// and the radiative loss: unscaled thin exchange e = kP 4 sigma (T^4 - 300^4) (EvaluateGasExchange, the per-cell emission
// FDS keeps unscaled outside flame cells) and RISE's applied loss beta e, beta = chi_r Q / sum e V with one global factor.
// Q is not in the checkpoint: chi_r Q is taken as 0.2 x 32.81 kW (r270 window-mean heat release), so beta is approximate.
// Also the exact MAC divergence of each cell (1/s) and the w on its top face (m/s), from the checkpoint face velocities.
// Last, rms w over time on the burner-edge ring 0.06<=r<0.12 m at layers 0,1,3,6,9,12,16, two ways: the top-face value
// (what FDS writes for a CELL_CENTERED W slice, dump.f90 GAS_PHASE_OUTPUT) and the cell average of the two faces
// (what RISE frames hold).
// argv: checkpoints.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
namespace {
const int Shells=11,Layers=4,Quantities=9;std::vector<double> betas;
const int RingLayers[7]={0,1,3,6,9,12,16};std::vector<double> ringFace,ringAverage;std::vector<bool> ringCell;
int Run(const std::filesystem::path& checkpointPath,std::vector<double>& acc){
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)){std::cerr<<error<<'\n';return 2;}
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		PeriodicMACShape shape;shape.nx=checkpoint.dimensions[0];shape.ny=checkpoint.dimensions[1];shape.nz=checkpoint.dimensions[2];
		shape.cellWidthM=checkpoint.cellWidthM;const double dx=shape.cellWidthM;
		if(checkpoint.states.size()!=shape.CellCount())return 3;
		const std::size_t nx=shape.nx,ny=shape.ny;const std::vector<double>* f=checkpoint.velocity.component.data();
		if(f[0].size()!=(nx+1)*ny*shape.nz||f[1].size()!=nx*(ny+1)*shape.nz||f[2].size()!=nx*ny*(shape.nz+1))return 5;
		const double p=fuel.ThermodynamicPressurePa(),R=8314.46261815324,V=dx*dx*dx;
		std::vector<double> exchange(shape.CellCount(),0.);double sum=0.;
		for(std::size_t cell=0;cell<shape.CellCount();++cell){GasExchangeEvaluation e;const MethaneCellState& st=checkpoint.states[cell];
		 if(!EvaluateGasExchange(st,st.temperatureK,300.0,fuel,FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1(),e,&error)){std::cerr<<error<<'\n';return 4;}
		 exchange[cell]=e.exchangeWPerM3;sum+=e.exchangeWPerM3*V;}
		const double beta=0.2*32.81e3/sum;betas.push_back(beta);
		const std::size_t plane=nx*ny;if(ringFace.empty()){ringFace.assign(7*plane*2,0.);ringAverage.assign(7*plane*2,0.);ringCell.assign(plane,false);
		 for(std::size_t y=0;y<ny;++y)for(std::size_t x=0;x<nx;++x){const double r=std::hypot((x+.5)*dx-.5*nx*dx,(y+.5)*dx-.5*ny*dx);ringCell[y*nx+x]=r>=.06&&r<.12;}}
		for(int l=0;l<7;++l){const std::size_t z=RingLayers[l];for(std::size_t c=0;c<plane;++c){if(!ringCell[c])continue;
		 const double top=f[2][(z+1)*plane+c],average=.5*(f[2][z*plane+c]+top);
		 ringFace[(l*plane+c)*2]+=top;ringFace[(l*plane+c)*2+1]+=top*top;ringAverage[(l*plane+c)*2]+=average;ringAverage[(l*plane+c)*2+1]+=average*average;}}
		for(std::size_t z=0;z<static_cast<std::size_t>(Layers);++z)for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){
		 const double rx=(x+.5)*dx-.5*shape.nx*dx,ry=(y+.5)*dx-.5*shape.ny*dx,r=std::hypot(rx,ry);
		 const int s=static_cast<int>(std::lround(r/dx));if(s>=Shells||std::abs(r-s*dx)>=.5*dx)continue;
		 const std::size_t cell=x+shape.nx*(y+shape.ny*z);const MethaneCellState& st=checkpoint.states[cell];const double rho=st.GasDensity();
		 double* a=&acc[(z*Shells+s)*(Quantities+1)];
		 a[0]+=st.temperatureK;a[1]+=rho;a[2]+=rho*R*st.temperatureK/p;a[3]+=st.constituent[MethaneCH4]/rho;a[4]+=st.constituent[MethaneO2]/rho;
		 a[5]+=exchange[cell]*1e-6;a[6]+=beta*exchange[cell]*1e-6;
		 a[7]+=(f[0][(z*ny+y)*(nx+1)+x+1]-f[0][(z*ny+y)*(nx+1)+x]+f[1][(z*(ny+1)+y+1)*nx+x]-f[1][(z*(ny+1)+y)*nx+x]
		  +f[2][((z+1)*ny+y)*nx+x]-f[2][(z*ny+y)*nx+x])/dx;a[8]+=f[2][((z+1)*ny+y)*nx+x];a[9]+=1.;}
		std::cerr<<"state "<<checkpointPath.filename().string()<<" t="<<checkpoint.simulationTimeS<<'\n';return 0;}
}
int main(int argc,char** argv){
 std::vector<double> acc(Layers*Shells*(Quantities+1),0.);for(int a=1;a<argc;++a){const int rc=Run(argv[a],acc);if(rc)return rc;}
 const char* name[Quantities]={"T","rho","M","Y_CH4","Y_O2","e MW/m3","beta e","div 1/s","w top"};
 double b=0.;for(double x:betas)b+=x;std::cout<<std::fixed<<"states "<<argc-1<<"  mean beta "<<std::setprecision(3)<<b/betas.size()<<"\n";
 for(int z=0;z<Layers;++z){std::cout<<"layer "<<z<<"   r(m):";for(int s=0;s<Shells;++s)std::cout<<std::setw(7)<<std::setprecision(3)<<s*0.030593116067411014;std::cout<<'\n';
  for(int q=0;q<Quantities;++q){std::cout<<"  "<<std::setw(9)<<name[q]<<"   ";for(int s=0;s<Shells;++s){const double* a=&acc[(z*Shells+s)*(Quantities+1)];
   std::cout<<std::setw(7)<<std::setprecision(q==0?0:3)<<a[q]/a[9];}std::cout<<'\n';}}
 std::cout<<"ring 0.06-0.12 m rms w (m/s): layer  z_face(m)  top face  |  z_centre(m)  cell average\n";
 const std::size_t plane=ringCell.size();const double n=argc-1,d=0.030593116067411014;
 for(int l=0;l<7;++l){double rf=0,ra=0,c=0;for(std::size_t i=0;i<plane;++i){if(!ringCell[i])continue;const double* a=&ringFace[(l*plane+i)*2];const double* b=&ringAverage[(l*plane+i)*2];
  rf+=std::sqrt(std::max(0.,a[1]/n-a[0]*a[0]/(n*n)));ra+=std::sqrt(std::max(0.,b[1]/n-b[0]*b[0]/(n*n)));c+=1;}
  std::cout<<"  "<<std::setw(2)<<RingLayers[l]<<"     "<<std::setprecision(3)<<(RingLayers[l]+1)*d<<"     "<<rf/c<<"     |    "<<(RingLayers[l]+.5)*d<<"     "<<ra/c<<'\n';}
 return 0;
}
