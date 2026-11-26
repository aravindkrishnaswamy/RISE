// r273 (read-only): soot Planck-mean optical depth from retained frames, which carry carbon (g/m3) and temperature
// but no gas species. kappa = 3.83*6*pi*E(m)*fv*T/C2 with E(m)=0.26 and 1800 kg/m3 (the solver's soot law).
// argv: nx nz dx frame.vdb...  Prints the frame-mean optical depth along x through the axis and the largest chord
// at several heights, the peak kappa, and the six-flux column escape of the axis cell (as the kernel computes it).
// Build: c++ -std=c++17 -O2 -I/opt/homebrew/opt/openvdb/include -I/opt/homebrew/include frame_tau.cpp
//        -L/opt/homebrew/opt/openvdb/lib -Wl,-rpath,/opt/homebrew/opt/openvdb/lib -lopenvdb -ltbb
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char** argv){
 if(argc<5)return 2;const int nx=atoi(argv[1]),nz=atoi(argv[2]);const double dx=atof(argv[3]);const int frames=argc-4;
 const double k0=3.83*6.0*M_PI*0.26/0.014387768775039337/1800.0*1e-3;   // per (g/m3 * K)
 const double heights[7]={0.05,0.1,0.14,0.2,0.3,0.5,0.8};
 std::vector<double> tauAxis(7,0),tauMax(7,0),kMax(7,0),escAxis(7,0);
 openvdb::initialize();
 for(int f=4;f<argc;++f){
  openvdb::io::File file(argv[f]);file.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto c=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("carbon"));file.close();
  if(!t||!c)return 3;
  auto ac=c->getConstAccessor();auto at=t->getConstAccessor();
  std::vector<double> k(std::size_t(nx)*nx*nz);
  for(int z=0;z<nz;++z)for(int y=0;y<nx;++y)for(int x=0;x<nx;++x){const openvdb::Coord q(x,y,z);
   k[(std::size_t(z)*nx+y)*nx+x]=k0*std::max(0.0f,ac.getValue(q))*at.getValue(q);}
  auto K=[&](int x,int y,int z){return k[(std::size_t(z)*nx+y)*nx+x];};
  for(int h=0;h<7;++h){const int z=std::min(nz-1,int(heights[h]/dx));double best=0,kTop=0;
   for(int y=0;y<nx;++y){double s=0;for(int x=0;x<nx;++x){s+=K(x,y,z)*dx;kTop=std::max(kTop,K(x,y,z));}
    if(y==nx/2)tauAxis[h]+=s/frames;best=std::max(best,s);}
   tauMax[h]+=best/frames;kMax[h]+=kTop/frames;
   // six-flux escape of the axis cell
   const int cx=nx/2,cy=nx/2;double d[6]={0,0,0,0,0,0};
   for(int x=0;x<cx;++x)d[0]+=K(x,cy,z)*dx;for(int x=cx+1;x<nx;++x)d[1]+=K(x,cy,z)*dx;
   for(int y=0;y<cy;++y)d[2]+=K(cx,y,z)*dx;for(int y=cy+1;y<nx;++y)d[3]+=K(cx,y,z)*dx;
   for(int zz=0;zz<z;++zz)d[4]+=K(cx,cy,zz)*dx;for(int zz=z+1;zz<nz;++zz)d[5]+=K(cx,cy,zz)*dx;
   const double own=K(cx,cy,z)*dx,ownT=own>0?-std::expm1(-own)/own:1.0;double e=0;for(double v:d)e+=std::exp(-v);
   escAxis[h]+=ownT*e/6.0/frames;}
 }
 std::printf("# %d frames; height_m tau_axis tau_max_chord frame-mean_peak_kappa_1/m axis_six_flux_escape\n",frames);
 for(int h=0;h<7;++h)std::printf("%5.2f %7.3f %7.3f %7.3f %7.3f\n",heights[h],tauAxis[h],tauMax[h],kMax[h],escAxis[h]);
 return 0;}
