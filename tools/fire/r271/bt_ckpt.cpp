// r271 small-scale advection test (section 24, read-only): projections of the checkpoint temperature (verified against
// the conserved energy on load) onto cos/sin(k x') over the core (z 1.9-2.7 m, x and y cells 10-58), x' from the domain
// centre, as bt_driver's per-step lines. argv: n (cells per wavelength), then checkpoints. Output: time_s ac as mean_u.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc<3)return 1;const double n=std::stod(argv[1]),Pi=3.14159265358979323846;
 std::cout<<std::setprecision(9)<<"time_s ac as mean_u\n";
 for(int a=2;a<argc;++a){MethaneRunCheckpoint c;std::string error;if(!LoadMethaneRunCheckpoint(argv[a],c,error)){std::cerr<<error<<'\n';return 2;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1],nz=c.dimensions[2];const double h=c.cellWidthM,k=2*Pi/(n*h),X0=.5*nx*h;
  double c2=0,s2=0,pc=0,ps=0,us=0;size_t un=0;
  for(size_t z=0;z<nz;++z){const double Z=(z+.5)*h;if(Z<1.9||Z>2.7)continue;
   for(size_t y=10;y<59;++y)for(size_t x=10;x<59;++x){const double X=(x+.5)*h,T=c.states[x+nx*(y+ny*z)].temperatureK-300.;
    const double cc=std::cos(k*(X-X0)),ss=std::sin(k*(X-X0));c2+=cc*cc;s2+=ss*ss;pc+=cc*T;ps+=ss*T;
    us+=.5*(c.velocity.component[0][x+(nx+1)*(y+ny*z)]+c.velocity.component[0][x+1+(nx+1)*(y+ny*z)]);++un;}}
  std::cout<<c.simulationTimeS<<' '<<pc/c2<<' '<<ps/s2<<' '<<us/un<<'\n';}
 return 0;
}
