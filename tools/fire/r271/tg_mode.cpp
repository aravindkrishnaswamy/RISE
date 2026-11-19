// r271 operator damping (read-only): Taylor-Green mode amplitude in tg_driver checkpoints.
// argv: modes m, then checkpoints. Per checkpoint: time, amplitude A = projection of the MAC face (u,v) onto the
// initial TG shape over the flat band core z in [1.9,2.7] m, off-mode share of (u,v,w) energy there, max |w|,
// max T - 300 K there (plume contamination), and the Vreman velocity-decay rate the field implies on the production
// legacy stencil, sigma_V = <nu_t 2 S:S>/<|u|^2> (Cv=0.07, 2dx central gradients of cell-centred velocity).
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc<3)return 1;const double m=std::stod(argv[1]),Pi=3.14159265358979323846;
 std::cout<<std::setprecision(6)<<"time_s A off_mode_share max_w max_dT sigma_V\n";
 for(int a=2;a<argc;++a){MethaneRunCheckpoint c;std::string error;if(!LoadMethaneRunCheckpoint(argv[a],c,error)){std::cerr<<error<<'\n';return 2;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1],nz=c.dimensions[2];const double h=c.cellWidthM,k=m*Pi/(nx*h);
  auto face=[&](int ax,size_t x,size_t y,size_t z){return ax==0?x+(nx+1)*(y+ny*z):ax==1?x+nx*(y+(ny+1)*z):x+nx*(y+ny*z);};
  const auto& V=c.velocity.component;double proj=0,norm=0,total=0,maxW=0,maxT=0;
  auto inCore=[&](size_t z){const double Z=(z+.5)*h;return Z>=1.9&&Z<=2.7;};
  for(size_t z=0;z<nz;++z){if(!inCore(z))continue;
   for(size_t y=0;y<ny;++y)for(size_t x=0;x<=nx;++x){const double s=std::sin(k*x*h)*std::cos(k*(y+.5)*h),u=V[0][face(0,x,y,z)];proj+=u*s;norm+=s*s;total+=u*u;}
   for(size_t y=0;y<=ny;++y)for(size_t x=0;x<nx;++x){const double s=-std::cos(k*(x+.5)*h)*std::sin(k*y*h),v=V[1][face(1,x,y,z)];proj+=v*s;norm+=s*s;total+=v*v;}
   for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){const double w=V[2][face(2,x,y,z)];total+=w*w;maxW=std::max(maxW,std::abs(w));
    maxT=std::max(maxT,c.states[x+nx*(y+ny*z)].temperatureK-300.);}}
  const double A=proj/norm;
  auto U=[&](int ax,size_t x,size_t y,size_t z){return .5*(V[ax][face(ax,x,y,z)]+V[ax][face(ax,x+(ax==0),y+(ax==1),z+(ax==2))]);};
  double num=0,den=0;
  for(size_t z=1;z+1<nz;++z){if(!inCore(z))continue;for(size_t y=1;y+1<ny;++y)for(size_t x=1;x+1<nx;++x){double g[3][3];
   for(int d=0;d<3;++d)for(int q=0;q<3;++q){size_t lo[3]={x,y,z},hi[3]={x,y,z};--lo[d];++hi[d];g[d][q]=(U(q,hi[0],hi[1],hi[2])-U(q,lo[0],lo[1],lo[2]))/(2*h);}
   double a2=0,b[3][3]={};for(int i=0;i<3;++i)for(int j=0;j<3;++j){a2+=g[i][j]*g[i][j];for(int r=0;r<3;++r)b[i][j]+=h*h*g[r][i]*g[r][j];}
   const double B=b[0][0]*b[1][1]-b[0][1]*b[0][1]+b[0][0]*b[2][2]-b[0][2]*b[0][2]+b[1][1]*b[2][2]-b[1][2]*b[1][2];
   const double nut=a2>0?.07*std::sqrt(std::max(0.,B)/a2):0.;double SS=0;for(int i=0;i<3;++i)for(int j=0;j<3;++j)SS+=std::pow(.5*(g[i][j]+g[j][i]),2);
   num+=nut*2*SS;den+=U(0,x,y,z)*U(0,x,y,z)+U(1,x,y,z)*U(1,x,y,z)+U(2,x,y,z)*U(2,x,y,z);}}
  std::cout<<c.simulationTimeS<<' '<<A<<' '<<1-A*A*norm/total<<' '<<maxW<<' '<<maxT<<' '<<num/den<<'\n';}
 return 0;
}
