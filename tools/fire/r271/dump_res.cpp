// r271 section 33: dump.cpp for any grid. Temperature and cell-centred velocity planes of retained RISE frames.
// argv: run dir (holding frames/frame_NNNNN.vdb), frame count, out file, N (cells per side), layer indices...
// Output float32 [frame][layer][c=T,u,v,w][y][x]. Prints the temperature grid's active bounding box of frame 0.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 if(argc<6)return 1;const int frames=atoi(argv[2]),N=atoi(argv[4]);std::vector<int> Z;for(int i=5;i<argc;++i)Z.push_back(atoi(argv[i]));
 openvdb::initialize();FILE* o=fopen(argv[3],"wb");std::vector<float> buf(size_t(N)*N);
 for(int f=0;f<frames;++f){
  std::ostringstream name;name<<argv[1]<<"/frames/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto T=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!T||!V)return 3;
  if(f==0)std::cout<<"bbox "<<T->evalActiveVoxelBoundingBox()<<" voxel "<<T->voxelSize()<<'\n';
  for(int z:Z)for(int c=0;c<4;++c){
   for(int y=0;y<N;++y)for(int x=0;x<N;++x){
    if(c==0)buf[x+size_t(N)*y]=T->tree().getValue(openvdb::Coord(x,y,z));
    else buf[x+size_t(N)*y]=V->tree().getValue(openvdb::Coord(x+1,y+1,z+1))[c-1];}
   fwrite(buf.data(),4,buf.size(),o);}
 }
 fclose(o);return 0;}
