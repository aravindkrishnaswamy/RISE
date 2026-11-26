// r271 near field (read-only): dump T,u,v,w at the lowest NZ cell layers of retained RISE frames.
// Output float32 [frame][z][c=T,u,v,w(,q)][y][x]; cell-centred, same indexing as r270 dump.cpp.
// argv: frames dir, first frame, frame count, layers, out file [, "reaction" to add q = heat release W/m3].
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
int main(int argc,char** argv){
 if(argc!=6&&argc!=7)return 2;
 const int first=atoi(argv[2]),frames=atoi(argv[3]),NZ=atoi(argv[4]),C=argc==7&&std::string(argv[6])=="reaction"?5:4;
 if(argc==7&&C!=5)return 2;
 openvdb::initialize();FILE* o=fopen(argv[5],"wb");if(!o)return 2;std::vector<float> buf(69*69);
 for(int f=first;f<first+frames;++f){
  std::ostringstream name;name<<argv[1]<<"/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto T=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));
  auto Q=C==5?openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("reaction")):openvdb::FloatGrid::Ptr();file.close();
  if(!T||!V||(C==5&&!Q))return 3;
  for(int k=0;k<NZ;++k)for(int c=0;c<C;++c){
   for(int y=0;y<69;++y)for(int x=0;x<69;++x){
    if(c==0)buf[x+69*y]=T->tree().getValue(openvdb::Coord(x,y,k));
    else if(c==4)buf[x+69*y]=Q->tree().getValue(openvdb::Coord(x,y,k));
    else buf[x+69*y]=V->tree().getValue(openvdb::Coord(x+1,y+1,k+1))[c-1];}
   fwrite(buf.data(),4,buf.size(),o);}
 }
 fclose(o);return 0;}
