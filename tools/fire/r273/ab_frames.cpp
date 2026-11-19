// r273: soot and temperature statistics per retained frame of an A/B arm (read-only).
// argv: nx nz dx frame.vdb...   Per frame: soot inventory (mg; the carbon channel is g/m3), its share in cells above 1300 K and above 1000 K, the gas
// volume above 1300 K (litres), heat release (kW, the frame's reaction channel), peak temperature (K), and the centre
// 3x3 mean temperature at 0.1, 0.3 and 0.6 m.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
int main(int argc,char** argv){
 if(argc<5)return 2;const int nx=atoi(argv[1]),nz=atoi(argv[2]);const double dx=atof(argv[3]),v=dx*dx*dx;
 openvdb::initialize();
 std::printf("# frame inventory_mg share_above_1300K share_above_1000K hot_volume_L hrr_kW peak_T_K Tc_0.1m Tc_0.3m Tc_0.6m\n");
 for(int f=4;f<argc;++f){
  openvdb::io::File file(argv[f]);file.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto c=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("carbon"));
  auto r=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("reaction"));file.close();
  if(!t||!c||!r)return 3;
  double inv=0,hot=0,warm=0,vol=0,hrr=0,peak=0,tc[3]={0,0,0};const double hz[3]={0.1,0.3,0.6};
  for(int z=0;z<nz;++z)for(int y=0;y<nx;++y)for(int x=0;x<nx;++x){
   const openvdb::Coord k(x,y,z);const double cv=c->tree().getValue(k),tv=t->tree().getValue(k);
   inv+=cv*v;if(tv>1300.0){hot+=cv*v;vol+=v;}if(tv>1000.0)warm+=cv*v;hrr+=r->tree().getValue(k)*v;peak=std::max(peak,tv);}
  for(int h=0;h<3;++h){const int z=std::min(nz-1,int(hz[h]/dx));for(int y=nx/2-1;y<=nx/2+1;++y)for(int x=nx/2-1;x<=nx/2+1;++x)
   tc[h]+=t->tree().getValue(openvdb::Coord(x,y,z))/9.0;}
  std::printf("%3d %9.3f %6.3f %6.3f %7.2f %7.2f %7.0f %6.0f %6.0f %6.0f\n",f-4,inv*1e3,inv>0?hot/inv:0.0,inv>0?warm/inv:0.0,vol*1e3,hrr*1e-3,peak,tc[0],tc[1],tc[2]);
 }
 return 0;}
