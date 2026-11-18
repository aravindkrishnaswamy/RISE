// preview only: multiply the carbon (soot) grid by a factor; every other grid is copied unchanged.
// argv: in.vdb out.vdb factor
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdlib>
int main(int argc,char** argv){
 if(argc!=4)return 2;openvdb::initialize();
 openvdb::io::File in(argv[1]);in.open();openvdb::GridPtrVecPtr grids=in.getGrids();
 openvdb::MetaMap::Ptr meta=in.getMetadata();in.close();const float f=static_cast<float>(std::atof(argv[3]));bool found=false;
 for(auto& g:*grids)if(g->getName()=="carbon"){auto c=openvdb::gridPtrCast<openvdb::FloatGrid>(g);if(!c)return 3;found=true;
  for(auto it=c->beginValueOn();it;++it)it.setValue(*it*f);}
 if(!found)return 3;
 openvdb::io::File out(argv[2]);out.write(*grids,*meta);out.close();return 0;}
