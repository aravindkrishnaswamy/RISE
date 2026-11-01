// Time means of full 3D fields from retained RISE frames (read-only).
// Output float64 [7][z][y][x]: T, u, v, w, u/T, v/T, w/T
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 // argv: runDir frames outFile
 const int N=69,NZ=106;const size_t C=size_t(N)*N*NZ;int frames=atoi(argv[2]);
 openvdb::initialize();std::vector<double> m(7*C,0.0);
 for(int f=0;f<frames;++f){
  std::ostringstream name;name<<argv[1]<<"/frames/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto T=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!T||!V)return 3;
  auto ta=T->getConstAccessor();auto va=V->getConstAccessor();
  for(int z=0;z<NZ;++z)for(int y=0;y<N;++y)for(int x=0;x<N;++x){
   const size_t i=x+N*(y+size_t(N)*z);const double t=ta.getValue(openvdb::Coord(x,y,z));
   const auto v=va.getValue(openvdb::Coord(x+1,y+1,z+1));
   m[i]+=t;for(int c=0;c<3;++c){m[(1+c)*C+i]+=v[c];m[(4+c)*C+i]+=v[c]/t;}
  }
 }
 for(auto& x:m)x/=frames;FILE* o=fopen(argv[3],"wb");fwrite(m.data(),8,m.size(),o);fclose(o);return 0;}
