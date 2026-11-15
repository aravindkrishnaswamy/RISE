// r272: soot budget from retained RISE frames (read-only).
// argv: nx nz dx frame.vdb...   Prints, per height layer, the time-mean upward soot flux (mg/s) through the layer,
// the carbon inventory (mg), and the share of that inventory in cells hotter than 1300 K (the record's oxidation
// threshold) and hotter than 1000 K. Velocity is the frame's cell-registered vec3 grid (index offset +1).
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char** argv){
 if(argc<5)return 2;const int nx=atoi(argv[1]),nz=atoi(argv[2]);const double dx=atof(argv[3]);const int frames=argc-4;
 openvdb::initialize();
 std::vector<double> F(nz,0.0),M(nz,0.0),H(nz,0.0),W(nz,0.0);
 for(int f=0;f<frames;++f){
  openvdb::io::File file(argv[4+f]);file.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto c=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("carbon"));
  auto v=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!t||!c||!v)return 3;
  for(int z=0;z<nz;++z)for(int y=0;y<nx;++y)for(int x=0;x<nx;++x){
   const openvdb::Coord k(x,y,z);const double cv=c->tree().getValue(k),tv=t->tree().getValue(k);
   const double w=v->tree().getValue(openvdb::Coord(x+1,y+1,z+1)).z();
   F[z]+=cv*w*dx*dx*1e3;M[z]+=cv*dx*dx*dx*1e3;if(tv>1300.0)H[z]+=cv*dx*dx*dx*1e3;if(tv>1000.0)W[z]+=cv*dx*dx*dx*1e3;}
 }
 std::printf("# frames=%d\n# z_layer height_m flux_mg_per_s inventory_mg share_above_1300K share_above_1000K\n",frames);
 for(int z=0;z<nz;++z)std::printf("%3d %6.3f %9.4f %9.5f %6.3f %6.3f\n",z,(z+0.5)*dx,F[z]/frames,M[z]/frames,M[z]>0?H[z]/M[z]:0.0,M[z]>0?W[z]/M[z]:0.0);
 return 0;}
