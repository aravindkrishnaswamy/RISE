// Dump temperature and velocity planes from retained RISE frames (read-only).
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 // argv: runDir frames outFile
 const int Z[]={9,16,24,32,49,65,81,98};const int NZ=8;int first=atoi(argv[2]),last=atoi(argv[4]);
 openvdb::initialize();FILE* o=fopen(argv[3],"wb");std::vector<float> buf(69*69);
 for(int f=first;f<=last;++f){
  std::ostringstream name;name<<argv[1]<<"/frames/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto T=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!T||!V)return 3;
  for(int k=0;k<NZ;++k){
   for(int c=0;c<4;++c){
    for(int y=0;y<69;++y)for(int x=0;x<69;++x){
     if(c==0)buf[x+69*y]=T->tree().getValue(openvdb::Coord(x,y,Z[k]));
     else buf[x+69*y]=V->tree().getValue(openvdb::Coord(x+1,y+1,Z[k]+1))[c-1];}
    fwrite(buf.data(),4,buf.size(),o);}
  }
 }
 fclose(o);return 0;}
