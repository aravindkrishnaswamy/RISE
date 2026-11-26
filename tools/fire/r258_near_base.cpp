// Offline MAC diagnostics; retained checkpoints only, no step is advanced.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=4)return 1;
 std::ifstream inputs(argv[1]);std::ofstream out(argv[2]),inlet(argv[3]);
 if(!inputs||!out||!inlet)return 2;
 out<<std::setprecision(17)<<"z,x,y,height_m,r_m,mean_w,mean_divergence,mean_abs_divergence,mean_divergence_squared\n";
 inlet<<std::setprecision(17)<<"x,y,r_m,mean_w\n";
 const size_t nx=69,ny=69;const double dx=.030593116067411014;
 std::vector<std::array<double,4>> sums(nx*ny*14);std::vector<double> floor(nx*ny);
 double weight=0.;std::string path,error;
 while(inputs>>weight>>path){
  if(weight==0.)continue;
  MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 3;}
  if(c.dimensions[0]!=nx||c.dimensions[1]!=ny||c.dimensions[2]!=106)return 4;
  auto u=[&](size_t x,size_t y,size_t z){return c.velocity.component[0][x+(nx+1)*(y+ny*z)];};
  auto v=[&](size_t x,size_t y,size_t z){return c.velocity.component[1][x+nx*(y+(ny+1)*z)];};
  auto w=[&](size_t x,size_t y,size_t z){return c.velocity.component[2][x+nx*(y+ny*z)];};
  for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x)floor[x+nx*y]+=weight*w(x,y,0);
  for(size_t z=3;z<=16;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   double d=(u(x+1,y,z)-u(x,y,z)+v(x,y+1,z)-v(x,y,z)+w(x,y,z+1)-w(x,y,z))/dx;
   const double q[4]={.5*(w(x,y,z)+w(x,y,z+1)),d,std::abs(d),d*d};
   for(size_t k=0;k<4;++k)sums[x+nx*(y+ny*(z-3))][k]+=weight*q[k];
  }
 }
 for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
  double r=std::hypot((double(x)-34)*dx,(double(y)-34)*dx);inlet<<x<<','<<y<<','<<r<<','<<floor[x+nx*y]<<'\n';
  for(size_t z=3;z<=16;++z){out<<z<<','<<x<<','<<y<<','<<(z+.5)*dx<<','<<r;for(double q:sums[x+nx*(y+ny*(z-3))])out<<','<<q;out<<'\n';}
 }
 return out&&inlet?0:5;
}
