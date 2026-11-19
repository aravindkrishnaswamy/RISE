// r271: per-frame heat-release signals from retained RISE frames (read-only).
// Output per frame (float64): single centre-cell column W, all z.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 if(argc!=4)return 2;
 const int frames=atoi(argv[2]);const double dx=0.030593116067411014,v=dx*dx*dx;
 openvdb::initialize();FILE* o=fopen(argv[3],"wb");
 for(int f=0;f<frames;++f){
  std::ostringstream name;name<<argv[1]<<"/frames/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto Q=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("reaction"));file.close();
  if(!Q)return 3;
  std::vector<double> out(1,0.0);
  for(auto it=Q->cbeginValueOn();it;++it){
   const openvdb::Coord c=it.getCoord();const double w=double(*it)*v;
   if(c.z()<0||c.z()>=106)continue;
   if(c.x()==34&&c.y()==34)out[0]+=w;
  }
  fwrite(out.data(),8,out.size(),o);
 }
 fclose(o);return 0;}
