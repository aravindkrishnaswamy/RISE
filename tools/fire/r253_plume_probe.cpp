// Read-only retained-state integrals; no solver step or production write.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;
 std::ifstream list(argv[1]);std::ofstream out(argv[2]);if(!list||!out)return 2;
 const auto& fuel=FireSimulationMethaneRecord::PhysicalV1();
 std::array<double,MethaneSpeciesCount> h300;std::string error;
 if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(300.,h300.data(),h300.size(),&error))return 3;
 out<<std::setprecision(17)<<"time_s,z,height_m,ambient_K,xlo_K,xhi_K,ylo_K,yhi_K,far08_K,center_K,center_w";
 for(int k=1;k<=5;++k)out<<",m_r"<<k<<",mup_r"<<k<<",cpheat_r"<<k<<",hheat_r"<<k;
 out<<",m_plane,mup_plane,cpheat_plane,hheat_plane,m_hot1,m_hot5,m_hot10,temp_r2,temp_sum,heat_r2,heat_sum,nut_heat_sum,nut_weight,nut_area_sum,nut_max,hot_area\n";
 std::string path;
 while(std::getline(list,path)){
  MethaneRunCheckpoint c;if(!LoadMethaneRunCheckpoint(path,c,error)){std::cerr<<error;return 4;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1],nz=c.dimensions[2],plane=nx*ny;
  const double dx=c.cellWidthM,area=dx*dx;const float dxf=static_cast<float>(dx);
  std::vector<std::array<float,3>> vel(c.states.size());
  auto index=[&](size_t x,size_t y,size_t z){return x+nx*(y+ny*z);};
  for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   const size_t i=index(x,y,z);
   vel[i][0]=.5f*(static_cast<float>(c.velocity.component[0][x+(nx+1)*(y+ny*z)])+static_cast<float>(c.velocity.component[0][x+1+(nx+1)*(y+ny*z)]));
   vel[i][1]=.5f*(static_cast<float>(c.velocity.component[1][x+nx*(y+(ny+1)*z)])+static_cast<float>(c.velocity.component[1][x+nx*(y+1+(ny+1)*z)]));
   vel[i][2]=.5f*(static_cast<float>(c.velocity.component[2][i])+static_cast<float>(c.velocity.component[2][i+plane]));
  }
  for(size_t z=0;z<nz;++z){
   double amb=0.,side[4]={},far=0.;size_t na=0,ns[4]={},nf=0;
   double m[5]={},mu[5]={},qc[5]={},qh[5]={},mp=0.,mup=0.,qcp=0.,qhp=0.,hot[3]={},tr2=0.,ts=0.,hr2=0.,hs=0.,nuheat=0.,nuweight=0.,nuarea=0.,numax=0.,hotarea=0.;
   for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
    size_t i=index(x,y,z);const auto& s=c.states[i];const double t=s.temperatureK,w=vel[i][2];
    const double xx=(static_cast<double>(x)+.5-.5*nx)*dx,yy=(static_cast<double>(y)+.5-.5*ny)*dx,r2=xx*xx+yy*yy;
    const bool sides[4]={x<4,x+4>=nx,y<4,y+4>=ny};
    for(int k=0;k<4;++k)if(sides[k]){side[k]+=t;++ns[k];}
    if(sides[0]||sides[1]||sides[2]||sides[3]){amb+=t;++na;}
    if(r2>.8*.8){far+=t;++nf;}
    std::array<double,MethaneSpeciesCount> cp,h;
    if(!fuel.CpBySpeciesOrderJPerKGK(t,cp.data(),cp.size(),&error)||!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(t,h.data(),h.size(),&error))return 5;
    double rho=0.,rhocp=0.,dh=0.;
    for(size_t a=0;a<MethaneCarbon;++a){const double q=std::max(0.,s.constituent[a]);rho+=q;rhocp+=q*cp[a];dh+=q*(h[a]-h300[a]);}
    const double mf=rho*w*area,qcf=rhocp*w*(t-300.)*area,qhf=dh*w*area;
    mp+=mf;mup+=std::max(0.,mf);qcp+=qcf;qhp+=qhf;
    for(int k=0;k<5;++k)if(r2<(.2*(k+1))*(.2*(k+1))){m[k]+=mf;mu[k]+=std::max(0.,mf);qc[k]+=qcf;qh[k]+=qhf;}
    for(int k=0;k<3;++k)if(w>0.&&t-300.>(k==0?1.:(k==1?5.:10.)))hot[k]+=mf;
    const double tp=std::max(0.,t-300.)*area,hp=std::max(0.,w)*std::max(0.,dh)*area;
    tr2+=r2*tp;ts+=tp;hr2+=r2*hp;hs+=hp;
    if(t>301.&&r2<.8*.8&&x>0&&x+1<nx&&y>0&&y+1<ny&&z>0&&z+1<nz){
     RISE::FireProductionVremanInput v;v.coefficient=static_cast<float>(FireSimulationTransportRecord::OpenV1().VremanCv());v.directionalWidthsM.fill(dxf);
     const size_t off[3]={1,nx,plane};
     for(size_t a=0;a<3;++a)for(size_t b=0;b<3;++b)v.velocityGradientPerS[3*a+b]=(vel[i+off[a]][b]-vel[i-off[a]][b])/(2.f*dxf);
     float nu=0.;if(!RISE::EvaluateFireProductionVremanEddyViscosity(v,nu,&error))return 6;
     nuheat+=hp*nu;nuweight+=hp;nuarea+=nu*area;numax=std::max(numax,static_cast<double>(nu));hotarea+=area;
    }
   }
   const size_t center=index(nx/2,ny/2,z);
   out<<c.simulationTimeS<<','<<z<<','<<(z+.5)*dx<<','<<amb/na;
   for(int k=0;k<4;++k)out<<','<<side[k]/ns[k];
   out<<','<<far/nf<<','<<c.states[center].temperatureK<<','<<vel[center][2];
   for(int k=0;k<5;++k)out<<','<<m[k]<<','<<mu[k]<<','<<qc[k]<<','<<qh[k];
   out<<','<<mp<<','<<mup<<','<<qcp<<','<<qhp;
   for(int k=0;k<3;++k)out<<','<<hot[k];
   out<<','<<tr2<<','<<ts<<','<<hr2<<','<<hs<<','<<nuheat<<','<<nuweight<<','<<nuarea<<','<<numax<<','<<hotarea<<'\n';
  }
  std::cerr<<"retained "<<c.simulationTimeS<<' '<<path<<'\n';
 }
 return out?0:7;
}
