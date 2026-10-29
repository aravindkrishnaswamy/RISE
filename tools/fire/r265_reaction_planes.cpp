// Read retained OpenVDB reaction frames; no solver code or timestep invocation.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <array>
#include <cmath>
#include <iostream>
int main(int argc,char** argv){
 if(argc!=3)return 1;const std::filesystem::path run=argv[1];std::ifstream stamps(run/"frame_times.csv");
 std::string line;std::getline(stamps,line);std::vector<double> times;
 while(std::getline(stamps,line)){const auto first=line.find(','),last=line.find(',',first+1);times.push_back(std::stod(line.substr(first+1,last-first-1)));}
 if(times.size()!=235)return 2;std::array<std::vector<double>,2> sums;for(auto& a:sums)a.assign(69*69,0.);
 openvdb::initialize();double width=0.;
 for(size_t frame=0;frame<times.size();++frame){
  const double lower=frame?times[frame-1]:times[frame],upper=frame+1<times.size()?times[frame+1]:times[frame];
  const double weight=.5*(upper-lower)/(times.back()-times.front());
  std::ostringstream name;name<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame<<".vdb";
  openvdb::io::File file((run/"frames"/name.str()).string());file.open();auto grid=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("reaction"));file.close();if(!grid)return 3;
  width=grid->voxelSize().x();if(!(width>0.))return 4;
  for(size_t p=0;p<2;++p){const int z=p?16:9;for(int y=0;y<69;++y)for(int x=0;x<69;++x){const double q=grid->tree().getValue(openvdb::Coord(x,y,z));if(!std::isfinite(q)||q<0.)return 5;sums[p][x+69*y]+=weight*q;}}
  if(frame%25==0)std::cerr<<"retained frame "<<frame<<" time="<<times[frame]<<'\n';
 }
 std::ofstream out(argv[2]);out<<std::setprecision(17)<<"z,x,y,height_m,r_m,mean_HRR_W_m3\n";
 for(size_t p=0;p<2;++p){const int z=p?16:9;for(int y=0;y<69;++y)for(int x=0;x<69;++x)out<<z<<','<<x<<','<<y<<','<<(z+.5)*width<<','<<std::hypot((x-34)*width,(y-34)*width)<<','<<sums[p][x+69*y]<<'\n';}
 return out?0:6;
}
