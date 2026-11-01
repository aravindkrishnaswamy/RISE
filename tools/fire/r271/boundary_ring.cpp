// r271 (c): per-frame outermost-cell rings of retained RISE frames (read-only): side columns and top layer.
// argv: runDir frames outFile. Output float32 per frame, per cell [T,u,v,w], cells in order (xmin,xmax,ymin,ymax planes [z][69]) then top layer [69][69] (y-major).
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 const int N=69,NZ=106;int frames=atoi(argv[2]);openvdb::initialize();FILE* o=fopen(argv[3],"wb");
 for(int f=0;f<frames;++f){
  std::ostringstream name;name<<argv[1]<<"/frames/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();
  auto T=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
  auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!T||!V)return 3;auto ta=T->getConstAccessor();auto va=V->getConstAccessor();
  std::vector<float> out;out.reserve(4*(4*NZ*N+N*N));
  auto put=[&](int x,int y,int z){out.push_back(ta.getValue(openvdb::Coord(x,y,z)));
   auto v=va.getValue(openvdb::Coord(x+1,y+1,z+1));for(int c=0;c<3;++c)out.push_back(v[c]);};
  for(int z=0;z<NZ;++z)for(int y=0;y<N;++y)put(0,y,z);
  for(int z=0;z<NZ;++z)for(int y=0;y<N;++y)put(N-1,y,z);
  for(int z=0;z<NZ;++z)for(int x=0;x<N;++x)put(x,0,z);
  for(int z=0;z<NZ;++z)for(int x=0;x<N;++x)put(x,N-1,z);
  for(int y=0;y<N;++y)for(int x=0;x<N;++x)put(x,y,NZ-1);
  fwrite(out.data(),4,out.size(),o);
 }
 fclose(o);return 0;}
