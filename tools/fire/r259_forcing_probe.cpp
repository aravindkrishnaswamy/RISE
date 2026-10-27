// Read retained frames/checkpoints only; no solver stage is advanced.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int Frames(const char* directory,const char* destination){
 openvdb::initialize();std::ofstream out(destination);out<<std::setprecision(17)<<"frame,time_s,z,height_m,slab_heat_W,plane_heat_W_per_m,center_rate_W_m3,rim_rate_W_m3\n";
 std::ifstream stamps(std::filesystem::path(directory).parent_path()/"frame_times.csv");std::string line;std::getline(stamps,line);std::vector<double> times;
 while(std::getline(stamps,line)){auto first=line.find(','),last=line.find(',',first+1);times.push_back(std::stod(line.substr(first+1,last-first-1)));}if(times.size()!=235)return 2;
 for(int frame=0;frame<235;++frame){std::ostringstream path;path<<directory<<"/frame_"<<std::setw(5)<<std::setfill('0')<<frame;
  double time=times[size_t(frame)];
  openvdb::io::File input(path.str()+".vdb");input.open();auto g=openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid("reaction"));input.close();if(!g)return 3;
  const double dx=g->voxelSize().x();
  for(int z=0;z<106;++z){double sum=0.;for(int y=0;y<69;++y)for(int x=0;x<69;++x){double q=g->tree().getValue(openvdb::Coord(x,y,z));if(!std::isfinite(q)||q<0.)return 4;sum+=q;}
   out<<frame<<','<<time<<','<<z<<','<<(z+.5)*dx<<','<<sum*dx*dx*dx<<','<<sum*dx*dx<<','<<g->tree().getValue(openvdb::Coord(34,34,z))<<','<<g->tree().getValue(openvdb::Coord(38,34,z))<<'\n';
  }
 }
 return out?0:5;
}
int Inlet(const char* list,const char* destination){
 const auto& f=FireSimulationMethaneRecord::PhysicalV1();double invW=0.;
 for(size_t k=0;k<MethaneCarbon;++k)invW+=f.InjectedMassFractions()[k]/f.FindSpecies(f.SpeciesOrder()[k].c_str())->molecularWeightKGPerKMol;
 const double rho=f.ThermodynamicPressurePa()/(8314.46261815324*300.*invW);
 std::ifstream inputs(list);std::ofstream out(destination);out<<std::setprecision(17)<<"time_s,weight,x,y,j_kg_m2_s,rho_CH4_kg_m3,rho_cell_kg_m3,rho_dual_kg_m3,u_expected_CH4_m_s,u_expected_dual_m_s,u_face_m_s,momentum_face_kg_m2_s\n";
 double weight=0.;std::string path,error;std::vector<double> pattern;
 while(inputs>>weight>>path){if(weight==0.)continue;MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 6;}
  if(pattern.empty()){FireCase::AuthoredV1 a;a.seed=1234;a.poolDiameterM=.3;FireCase::DerivedV1 d;d.nx=69;d.ny=69;d.nz=106;d.cellWidthM=c.cellWidthM;d.extentXM=69*c.cellWidthM;d.extentYM=d.extentXM;d.extentZM=106*c.cellWidthM;d.sourceAreaM2=.25*3.141592653589793238462643383279502884*.3*.3;if(!FireCase::BuildSourcePattern(a,d,pattern,error))return 7;}
  for(size_t face=0;face<pattern.size();++face)if(pattern[face]>0.){double gas=0.;for(size_t k=0;k<MethaneCarbon;++k)gas+=c.states[face].constituent[k];double j=33000./(f.LowerHeatingValueJPerKG()*.25*3.141592653589793238462643383279502884*.3*.3)*pattern[face];double dual=.5*(rho+gas);
   out<<c.simulationTimeS<<','<<weight<<','<<face%69<<','<<face/69<<','<<j<<','<<rho<<','<<gas<<','<<dual<<','<<j/rho<<','<<j/dual<<','<<c.velocity.component[2][face]<<','<<c.momentum.component[2][face]<<'\n';
  }
 }
 return out?0:8;
}
int main(int argc,char** argv){if(argc!=4)return 1;if(std::string(argv[1])=="frames")return Frames(argv[2],argv[3]);if(std::string(argv[1])=="inlet")return Inlet(argv[2],argv[3]);return 1;}
