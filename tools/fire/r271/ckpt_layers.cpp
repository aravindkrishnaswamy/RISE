// r271 (section 25, read-only): dump the lowest layers of retained RISE checkpoints as near_planes.cpp does for frames.
// Output float32 [checkpoint][z][c=T,u,v,w][y][x], velocities as two-face cell averages; times to <out>.times.
// argv: layers, out file, then checkpoints.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc<4)return 1;const size_t NZ=std::stoul(argv[1]);FILE* o=fopen(argv[2],"wb");std::ofstream times(std::string(argv[2])+".times");
 times<<std::setprecision(17);std::vector<float> buf;
 for(int a=3;a<argc;++a){MethaneRunCheckpoint c;std::string error;if(!LoadMethaneRunCheckpoint(argv[a],c,error)){std::cerr<<error<<'\n';return 2;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1];buf.assign(nx*ny,0.f);
  const auto& U=c.velocity.component[0];const auto& V=c.velocity.component[1];const auto& W=c.velocity.component[2];
  for(size_t z=0;z<NZ;++z)for(int f=0;f<4;++f){
   for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){double q;
    if(f==0)q=c.states[x+nx*(y+ny*z)].temperatureK;
    else if(f==1)q=.5*(U[x+(nx+1)*(y+ny*z)]+U[x+1+(nx+1)*(y+ny*z)]);
    else if(f==2)q=.5*(V[x+nx*(y+(ny+1)*z)]+V[x+nx*(y+1+(ny+1)*z)]);
    else q=.5*(W[x+nx*(y+ny*z)]+W[x+nx*(y+ny*(z+1))]);
    buf[x+nx*y]=float(q);}
   fwrite(buf.data(),4,buf.size(),o);}
  times<<c.simulationTimeS<<'\n';}
 fclose(o);return 0;
}
