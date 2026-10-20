// Read-only extraction from r251's retained, already centered VDB fields.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
int main(int argc,char** argv){
 if(argc!=3)return 1;
 openvdb::initialize();std::ofstream out(argv[2]);
 out<<std::setprecision(17)<<"frame,z,theta_K,w_m_s\n";
 for(int frame=0;frame<235;++frame){
  std::ostringstream path;path<<argv[1]<<"/frame_"<<std::setw(5)<<std::setfill('0')<<frame<<".vdb";
  openvdb::io::File f(path.str());f.open();
  auto t=openvdb::gridPtrCast<openvdb::FloatGrid>(f.readGrid("temperature"));
  auto v=openvdb::gridPtrCast<openvdb::Vec3fGrid>(f.readGrid("velocity"));f.close();
  if(!t||!v)return 2;
  for(int z:{32,65,98})out<<frame<<','<<z<<','<<t->tree().getValue(openvdb::Coord(34,34,z))-300.<<','<<v->tree().getValue(openvdb::Coord(35,35,z+1))[2]<<'\n';
 }
 return out?0:3;
}
