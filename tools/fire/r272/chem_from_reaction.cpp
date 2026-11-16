// r272: derive a render sequence whose chemiluminescence channels follow the heat release only.
// The capstone exporter writes chem_* = scale * max(reaction/reaction_max, (T-300)/(T_max-300)); the thermal term
// makes the whole warm plume glow. This tool rewrites chem_CH/C2/CO2 as fixed fractions of the local reaction
// (W/m3 of band power per W/m3 of heat release) and copies every other grid unchanged.
// argv: in.vdb out.vdb fraction_CH   (C2 and CO2 keep the exporter's 50/120 and 8/120 ratios)
// With out.vdb = "-" it only prints the frame's reaction maximum and chem_CH maximum.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
int main(int argc,char** argv){
 if(argc!=4)return 2;openvdb::initialize();
 openvdb::io::File in(argv[1]);in.open();openvdb::GridPtrVecPtr grids=in.getGrids();
 openvdb::MetaMap::Ptr meta=in.getMetadata();in.close();
 openvdb::FloatGrid::Ptr reaction,chem[3];const char* names[3]={"chem_CH","chem_C2","chem_CO2"};
 for(auto& g:*grids){if(g->getName()=="reaction")reaction=openvdb::gridPtrCast<openvdb::FloatGrid>(g);
  for(int i=0;i<3;++i)if(g->getName()==names[i])chem[i]=openvdb::gridPtrCast<openvdb::FloatGrid>(g);}
 if(!reaction||!chem[0]||!chem[1]||!chem[2])return 3;
 float rmax=0.0f,cmax=0.0f;
 for(auto it=reaction->cbeginValueOn();it;++it)if(*it>rmax)rmax=*it;
 for(auto it=chem[0]->cbeginValueOn();it;++it)if(*it>cmax)cmax=*it;
 if(std::strcmp(argv[2],"-")==0){std::printf("%.9g %.9g\n",rmax,cmax);return 0;}
 const double f=std::atof(argv[3]);const double ratio[3]={1.0,50.0/120.0,8.0/120.0};
 for(int i=0;i<3;++i)for(auto it=chem[i]->beginValueOn();it;++it){
  const float r=reaction->tree().getValue(it.getCoord());it.setValue(static_cast<float>(f*ratio[i]*(r>0.0f?r:0.0f)));}
 openvdb::io::File out(argv[2]);out.write(*grids,*meta);out.close();return 0;}
