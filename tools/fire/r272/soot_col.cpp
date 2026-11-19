// r272: centreline and layer soot statistics from retained RISE frames (read-only).
// argv: nx nz frame.vdb...   Prints, per height layer, the time-mean of: centre 3x3 temperature (K),
// centre 3x3 carbon (g/m3), centre soot mass fraction (carbon / gas density at 1 atm, M = 28.9 g/mol),
// centre soot volume fraction (ppm, 1800 kg/m3), and the layer-integrated carbon (g/m per unit height / dx^2).
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char** argv){
 if(argc<4)return 2;const int nx=atoi(argv[1]),nz=atoi(argv[2]);const int frames=argc-3;
 openvdb::initialize();
 std::vector<double> T(nz,0.0),C(nz,0.0),Y(nz,0.0),L(nz,0.0),Cmax(nz,0.0);double total=0.0;
 for(int f=0;f<frames;++f){
  openvdb::io::File file(argv[3+f]);file.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto c=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("carbon"));file.close();
  if(!t||!c)return 3;
  const int c0=nx/2-1;
  for(int z=0;z<nz;++z){
   double ts=0.0,cs=0.0,ys=0.0;
   for(int y=c0;y<c0+3;++y)for(int x=c0;x<c0+3;++x){
    const double tv=t->tree().getValue(openvdb::Coord(x,y,z)),cv=c->tree().getValue(openvdb::Coord(x,y,z));
    ts+=tv;cs+=cv;ys+=cv*1e-3/(101325.0*0.0289/(8.314462*tv));}
   T[z]+=ts/9.0;C[z]+=cs/9.0;Y[z]+=ys/9.0;
   double layer=0.0,mx=0.0;
   for(int y=0;y<nx;++y)for(int x=0;x<nx;++x){const double cv=c->tree().getValue(openvdb::Coord(x,y,z));layer+=cv;if(cv>mx)mx=cv;}
   L[z]+=layer;total+=layer;if(mx>Cmax[z])Cmax[z]=mx;}
 }
 std::printf("# frames=%d mean_total_carbon_cells_g_per_m3=%.6g\n# z_layer T_c(K) carbon_c(g/m3) Y_soot_c(kg/kg) fv_c(ppm) layer_sum(g/m3*cells) layer_max(g/m3)\n",frames,total/frames);
 for(int z=0;z<nz;++z)std::printf("%3d %8.1f %10.4e %10.4e %8.4f %10.4e %10.4e\n",z,T[z]/frames,C[z]/frames,Y[z]/frames,
  C[z]/frames*1e-3/1800.0*1e6,L[z]/frames,Cmax[z]);
 return 0;}
