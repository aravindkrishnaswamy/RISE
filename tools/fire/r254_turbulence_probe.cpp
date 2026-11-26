// Offline moments of accepted states only. No solver stage is advanced.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int FrameMoments(const char* directory,const char* destination){
 openvdb::initialize();const size_t nx=69,ny=69,nz=106;const double dx=.030593116067411014;
 std::vector<std::array<double,4>> sums(nx*ny*nz);
 for(size_t f=0;f<235;++f){
  std::ostringstream name;name<<directory<<"/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto v=openvdb::gridPtrCast<openvdb::Vec3fGrid>(file.readGrid("velocity"));file.close();
  if(!t||!v)return 8;
  const double weight=(f==0||f==234)?.5/234.:1./234.;
  for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   const double theta=t->tree().getValue(openvdb::Coord(int(x),int(y),int(z)))-300.;
   const double w=v->tree().getValue(openvdb::Coord(int(x+1),int(y+1),int(z+1)))[2];
   const double q[4]={w,theta,w*w,theta*theta};
   for(size_t k=0;k<4;++k)sums[x+nx*(y+ny*z)][k]+=weight*q[k];
  }
  std::cerr<<"frame "<<f<<'\n';
 }
 std::ofstream out(destination);out<<std::setprecision(17)<<"z,x,y,height_m,r_m,w,theta,w2,theta2\n";
 for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
  const double xx=(double(x)+.5-.5*nx)*dx,yy=(double(y)+.5-.5*ny)*dx;
  out<<z<<','<<x<<','<<y<<','<<(z+.5)*dx<<','<<std::hypot(xx,yy);
  for(double q:sums[x+nx*(y+ny*z)])out<<','<<q;out<<'\n';
 }
 return out?0:9;
}
int main(int argc,char** argv){
 if(argc==4&&std::string(argv[1])=="--frames")return FrameMoments(argv[2],argv[3]);
 if(argc!=4)return 1;
 std::ifstream list(argv[1]);std::ofstream out(argv[2]),nuout(argv[3]);
 if(!list||!out||!nuout)return 2;
 std::vector<std::array<double,8>> moments;std::vector<std::array<double,5>> nus;
 std::string path,error;double weight=0.,dx=0.;size_t nx=0,ny=0,nz=0;
 while(list>>weight>>path){
  if(weight==0.)continue;
  MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 3;}
  nx=c.dimensions[0];ny=c.dimensions[1];nz=c.dimensions[2];dx=c.cellWidthM;
  const size_t plane=nx*ny,n=c.states.size();const float dxf=static_cast<float>(dx);
  if(moments.empty()){moments.resize(n);nus.resize(nz);}
  if(moments.size()!=n)return 4;
  std::vector<std::array<float,3>> vel(n);
  for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   const size_t i=x+nx*(y+ny*z);
   vel[i][0]=.5f*(float(c.velocity.component[0][x+(nx+1)*(y+ny*z)])+float(c.velocity.component[0][x+1+(nx+1)*(y+ny*z)]));
   vel[i][1]=.5f*(float(c.velocity.component[1][x+nx*(y+(ny+1)*z)])+float(c.velocity.component[1][x+nx*(y+1+(ny+1)*z)]));
   vel[i][2]=.5f*(float(c.velocity.component[2][i])+float(c.velocity.component[2][i+plane]));
  }
  for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   const size_t i=x+nx*(y+ny*z);const double t=c.states[i].temperatureK-300.,w=vel[i][2];double rho=0.;
   for(size_t k=0;k<MethaneCarbon;++k)rho+=std::max(0.,c.states[i].constituent[k]);
   const double vals[8]={w,t,w*w,t*t,rho,rho*w,vel[i][0],vel[i][1]};
   for(size_t k=0;k<8;++k)moments[i][k]+=weight*vals[k];
   const double xx=(double(x)+.5-.5*nx)*dx,yy=(double(y)+.5-.5*ny)*dx;
   if(t<=1.||xx*xx+yy*yy>=.64||x==0||x+1==nx||y==0||y+1==ny||z==0||z+1==nz)continue;
   RISE::FireProductionVremanInput v;v.coefficient=float(FireSimulationTransportRecord::OpenV1().VremanCv());v.directionalWidthsM.fill(dxf);
   const size_t off[3]={1,nx,plane};double a[3][3],norm=0.,beta[3][3]={};
   for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k){a[j][k]=(double(vel[i+off[j]][k])-vel[i-off[j]][k])/(2.*dx);norm+=a[j][k]*a[j][k];v.velocityGradientPerS[3*j+k]=(vel[i+off[j]][k]-vel[i-off[j]][k])/(2.f*dxf);}
   for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k)for(size_t m=0;m<3;++m)beta[j][k]+=dx*dx*a[m][j]*a[m][k];
   const double b=beta[0][0]*beta[1][1]-beta[0][1]*beta[0][1]+beta[0][0]*beta[2][2]-beta[0][2]*beta[0][2]+beta[1][1]*beta[2][2]-beta[1][2]*beta[1][2];
   const double expected=norm>0.?FireSimulationTransportRecord::OpenV1().VremanCv()*std::sqrt(std::max(0.,b)/norm):0.;
   float actual=0.;if(!RISE::EvaluateFireProductionVremanEddyViscosity(v,actual,&error))return 5;
   const double q[5]={actual,expected,std::abs(actual-expected),1.,norm};
   for(size_t k=0;k<5;++k)nus[z][k]+=weight*q[k];
  }
  std::cerr<<c.simulationTimeS<<'\n';
 }
 out<<std::setprecision(17)<<"z,x,y,height_m,r_m,w,theta,w2,theta2,rho,rhow,u,v\n";
 for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
  const double xx=(double(x)+.5-.5*nx)*dx,yy=(double(y)+.5-.5*ny)*dx;
  out<<z<<','<<x<<','<<y<<','<<(z+.5)*dx<<','<<std::hypot(xx,yy);
  for(double q:moments[x+nx*(y+ny*z)])out<<','<<q;
  out<<'\n';
 }
 nuout<<std::setprecision(17)<<"z,height_m,nu_production_sum,nu_fds_sum,absolute_difference_sum,weight,gradient_norm_sum\n";
 for(size_t z=0;z<nz;++z){nuout<<z<<','<<(z+.5)*dx;for(double q:nus[z])nuout<<','<<q;nuout<<'\n';}
 return out&&nuout?0:6;
}
