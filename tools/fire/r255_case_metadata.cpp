#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=2&&argc!=3)return 1;MethaneRunCheckpoint c;std::string error;
 if(!LoadMethaneRunCheckpoint(argv[1],c,error))return 2;
 if(argc==3){
  std::cout<<std::setprecision(17)<<"z,height_m,temperature_rise_K,velocity_m_s,plane_heat_W\n";
  for(size_t z=0;z<c.dimensions[2];++z)std::cout<<z<<','<<(z+.5)*c.cellWidthM<<','<<c.centerlineTemperatureIntegral[z]/c.centerlineStatisticsDurationS-300.<<','<<c.centerlineVelocityIntegral[z]/c.centerlineStatisticsDurationS<<','<<c.planeHeatReleaseIntegral[z]/c.centerlineStatisticsDurationS<<'\n';
  return std::cout?0:4;
 }
 const auto& f=FireSimulationMethaneRecord::PhysicalV1();
 std::cout<<std::setprecision(17)<<"dx "<<c.cellWidthM<<"\nambient";
 for(double x:f.AmbientMassFractions())std::cout<<' '<<x;
 std::cout<<"\nLHV "<<f.LowerHeatingValueJPerKG()<<"\n";
 FireCase::AuthoredV1 a;a.seed=1234;a.poolDiameterM=.3;
 FireCase::DerivedV1 d;d.nx=c.dimensions[0];d.ny=c.dimensions[1];d.nz=c.dimensions[2];d.cellWidthM=c.cellWidthM;d.extentXM=d.nx*d.cellWidthM;d.extentYM=d.ny*d.cellWidthM;d.sourceAreaM2=.25*3.141592653589793238462643383279502884*.3*.3;
 std::vector<double> pattern;if(!FireCase::BuildSourcePattern(a,d,pattern,error)){std::cerr<<error;return 3;}
 for(size_t i=0;i<pattern.size();++i)if(pattern[i]>0)std::cout<<"source "<<i%d.nx<<' '<<i/d.nx<<' '<<pattern[i]<<'\n';
 return 0;
}
