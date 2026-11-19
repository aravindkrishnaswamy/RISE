// r271 section 25 (read-only): MAC velocity divergence (1/s), temperature, gas density and methane mass fraction of the lowest layers of retained RISE checkpoints.
// Output float32 [checkpoint][z][f=div,T,rho,Y_CH4][y][x]; times to <out>.times. argv: layers, out file, then checkpoints.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc<4)return 1;const size_t NZ=std::stoul(argv[1]);FILE* o=fopen(argv[2],"wb");std::ofstream times(std::string(argv[2])+".times");
 times<<std::setprecision(17);std::vector<float> buf;
 for(int a=3;a<argc;++a){MethaneRunCheckpoint c;std::string error;if(!LoadMethaneRunCheckpoint(argv[a],c,error)){std::cerr<<error<<'\n';return 2;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1];const double h=c.cellWidthM;buf.assign(nx*ny,0.f);
  const auto& U=c.velocity.component[0];const auto& V=c.velocity.component[1];const auto& W=c.velocity.component[2];
  for(size_t z=0;z<NZ;++z){for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x)
    buf[x+nx*y]=float((U[x+1+(nx+1)*(y+ny*z)]-U[x+(nx+1)*(y+ny*z)]+V[x+nx*(y+1+(ny+1)*z)]-V[x+nx*(y+(ny+1)*z)]+W[x+nx*(y+ny*(z+1))]-W[x+nx*(y+ny*z)])/h);
   fwrite(buf.data(),4,buf.size(),o);
   for(int f=1;f<4;++f){for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){const auto& s=c.states[x+nx*(y+ny*z)];
     buf[x+nx*y]=float(f==1?s.temperatureK:f==2?s.GasDensity():std::max(0.0,s.constituent[MethaneCH4])/s.GasDensity());}
    fwrite(buf.data(),4,buf.size(),o);}}
  times<<c.simulationTimeS<<'\n';}
 fclose(o);return 0;
}
