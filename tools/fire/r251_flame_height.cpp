// Read-only instantaneous HRR percentiles from retained production VDB frames.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>
int main(int argc,char** argv)
{
 if(argc!=3)return 1;
 openvdb::initialize();
 std::vector<std::filesystem::path> paths;
 for(const auto& entry:std::filesystem::directory_iterator(argv[1]))
  if(entry.path().extension()==".vdb")paths.push_back(entry.path());
 std::sort(paths.begin(),paths.end());
 std::ofstream output(argv[2]);if(!output||paths.empty())return 2;
 std::vector<double> priorPlanes,integral;double firstTime=0.,priorTime=0.,voxelSize=0.;
 output<<std::setprecision(17)<<"frame,time_s,total_hrr_W,h95_realized_m,h97_realized_m,h99_realized_m,h99_nominal_m\n";
 for(const auto& path:paths){
  auto timePath=path;timePath.replace_extension(".time");double time=0.;
  std::ifstream stamp(timePath);if(!(stamp>>time))return 3;
  openvdb::io::File input(path.string());input.open();
  auto grid=openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid("reaction"));
  if(!grid)return 4;
  const double dx=grid->voxelSize().z(),volume=dx*dx*dx;
  const auto bounds=grid->evalActiveVoxelBoundingBox();
  if(bounds.min().z()!=0)return 5;
  std::vector<double> planes(static_cast<size_t>(bounds.max().z()+1),0.);
  auto accessor=grid->getConstAccessor();
  for(int z=0;z<=bounds.max().z();++z)for(int y=bounds.min().y();y<=bounds.max().y();++y)
   for(int x=bounds.min().x();x<=bounds.max().x();++x){
    const double value=accessor.getValue(openvdb::Coord(x,y,z));
    if(!std::isfinite(value)||value<0.)return 6;
    planes[static_cast<size_t>(z)]+=value*volume;
   }
  const double total=std::accumulate(planes.begin(),planes.end(),0.);
  if(!(total>0.))return 7;
  if(priorPlanes.empty()){integral.assign(planes.size(),0.);firstTime=time;voxelSize=dx;}
  else{
   if(time<=priorTime||planes.size()!=priorPlanes.size()||dx!=voxelSize)return 9;
   for(size_t z=0;z<planes.size();++z)integral[z]+=.5*(priorPlanes[z]+planes[z])*(time-priorTime);
  }
  auto height=[&](double target){double cumulative=0.;for(size_t z=0;z<planes.size();++z){
   if(planes[z]>0.&&cumulative+planes[z]>=target)return (z+(target-cumulative)/planes[z])*dx;
   cumulative+=planes[z];}return -1.;};
  output<<path.filename().string()<<','<<time<<','<<total<<','<<height(.95*total)<<','
   <<height(.97*total)<<','<<height(.99*total)<<','<<height(.99*33000.)<<'\n';
  input.close();
  priorPlanes=std::move(planes);priorTime=time;
 }
 output.close();if(!output||!(priorTime>firstTime))return 8;
 std::ofstream profile(std::string(argv[2])+".mean_profile.csv");
 profile<<std::setprecision(17)<<"z,lower_m,upper_m,center_m,statistics_duration_s,integrated_heat_J,mean_heat_W\n";
 for(size_t z=0;z<integral.size();++z)profile<<z<<','<<z*voxelSize<<','<<(z+1)*voxelSize<<','
  <<(z+.5)*voxelSize<<','<<priorTime-firstTime<<','<<integral[z]<<','<<integral[z]/(priorTime-firstTime)<<'\n';
 profile.close();return profile?0:10;
}
