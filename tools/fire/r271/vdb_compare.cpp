// r271: compare two VDB frames grid by grid, value by value (file bytes differ across OpenVDB versions).
// argv: a.vdb b.vdb. Prints per grid: active voxel counts, voxels that differ, max |difference|. Exit 0 iff identical values.
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
template<class G> void Compare(const G& a,const G& b,const std::string& name,bool& same){
 size_t n=0,diff=0;double maxd=0;
 for(auto it=a.cbeginValueOn();it;++it){++n;const auto va=*it;const auto vb=b.tree().getValue(it.getCoord());
  if(!(va==vb)){++diff;maxd=std::max(maxd,double(openvdb::math::Abs(va-vb)[0]));}}
 const size_t nb=b.activeVoxelCount();if(diff||n!=nb)same=false;
 std::printf("%-24s active %zu / %zu  differing %zu  max|d| %.3g\n",name.c_str(),n,nb,diff,maxd);}
template<> void Compare<openvdb::FloatGrid>(const openvdb::FloatGrid& a,const openvdb::FloatGrid& b,const std::string& name,bool& same){
 size_t n=0,diff=0;double maxd=0;
 for(auto it=a.cbeginValueOn();it;++it){++n;const float va=*it,vb=b.tree().getValue(it.getCoord());
  if(!(va==vb)&&!(std::isnan(va)&&std::isnan(vb))){++diff;maxd=std::max(maxd,double(std::fabs(va-vb)));}}
 const size_t nb=b.activeVoxelCount();if(diff||n!=nb)same=false;
 std::printf("%-24s active %zu / %zu  differing %zu  max|d| %.3g\n",name.c_str(),n,nb,diff,maxd);}
int main(int argc,char** argv){
 if(argc!=3)return 2;openvdb::initialize();std::map<std::string,openvdb::GridBase::Ptr> A,B;
 for(int f=0;f<2;++f){std::ifstream in(argv[1+f],std::ios::binary);if(!in)return 2;openvdb::io::Stream stream(in);for(auto g:*stream.getGrids())(f?B:A)[g->getName()]=g;}
 bool same=A.size()==B.size();
 for(auto& [name,ga]:A){auto it=B.find(name);if(it==B.end()){std::printf("%s missing in b\n",name.c_str());same=false;continue;}
  if(auto fa=openvdb::gridPtrCast<openvdb::FloatGrid>(ga)){auto fb=openvdb::gridPtrCast<openvdb::FloatGrid>(it->second);if(!fb){same=false;continue;}Compare(*fa,*fb,name,same);}
  else if(auto va=openvdb::gridPtrCast<openvdb::Vec3SGrid>(ga)){auto vb=openvdb::gridPtrCast<openvdb::Vec3SGrid>(it->second);if(!vb){same=false;continue;}Compare(*va,*vb,name,same);}
  else{std::printf("%s: unsupported grid type %s\n",name.c_str(),ga->type().c_str());same=false;}}
 std::printf(same?"IDENTICAL VALUES\n":"VALUES DIFFER\n");return same?0:1;}
