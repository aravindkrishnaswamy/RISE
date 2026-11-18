// Offline retained checkpoint mass flux only; no solver step is advanced.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;std::ifstream list(argv[1]);std::ofstream out(argv[2]);if(!list||!out)return 2;
 out<<std::setprecision(17)<<"time_s,z,height_m,net_r08_kg_s,up_r08_kg_s,up_plane_kg_s\n";
 std::string path,error;
 while(std::getline(list,path)){
  MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 3;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1],plane=nx*ny;const double dx=c.cellWidthM;
  for(double height:{1.,2.,3.}){
   const size_t z=size_t(std::round(height/dx-.5));if(z>=c.dimensions[2])return 4;
   double net=0.,up=0.,all=0.;
   for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
    const size_t i=x+nx*(y+ny*z);double rho=0.;for(size_t k=0;k<MethaneCarbon;++k)rho+=std::max(0.,c.states[i].constituent[k]);
    const float w=.5f*(float(c.velocity.component[2][i])+float(c.velocity.component[2][i+plane]));
    const double flux=rho*w*dx*dx,xx=(double(x)+.5-.5*nx)*dx,yy=(double(y)+.5-.5*ny)*dx;
    all+=std::max(0.,flux);if(xx*xx+yy*yy<.64){net+=flux;up+=std::max(0.,flux);}
   }
   out<<c.simulationTimeS<<','<<z<<','<<(z+.5)*dx<<','<<net<<','<<up<<','<<all<<'\n';
  }
 }
 return out?0:5;
}
