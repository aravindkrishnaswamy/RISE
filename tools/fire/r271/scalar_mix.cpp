// r271 scalar mixing (read-only): on retained r270 states, size the two ways RISE's heat/species
// diffusivity differs from FDS. (1) The scalar kernel recomputes Vreman nu_t from 2dx cell-centred
// gradients (evaluate_resident_transport); FDS reuses the momentum mu_t (compact diagonal gradients).
// (2) RISE averages face diffusivity harmonically; FDS arithmetically (divg.f90 RHO_D_DZDX, KDTDX).
// Output per height band: ring means of nu_t, and the plane sum of |lateral diffusive heat flux|
// (rho cp nu/Pr + k_mol) dT/dx over x/y faces for the four combinations. Interior cells only.
#define main FireSequenceSuiteMain
#include "../../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc<2)return 1;const double Cv=.07,Pr=.7,cp=1150.;
 const double bands[][2]={{.06,.16},{.24,.34},{.45,.55},{.70,.80},{.94,1.04},{1.46,1.56}};const int NB=6;
 const double rings[]={0.,.1,.2,.3,.5};
 std::vector<double> nuW(NB*4),nuC(NB*4),cnt(NB*4),flux(NB*4),budget(NB*4*3);
 const double cylinders[]={.1,.15,.2,.3};
 int states=0;
 for(int a=1;a<argc;++a){
  MethaneRunCheckpoint c;std::string error;if(!LoadMethaneRunCheckpoint(argv[a],c,error)){std::cerr<<error<<'\n';return 2;}
  const size_t nx=c.dimensions[0],ny=c.dimensions[1],nz=c.dimensions[2];const double dx=c.cellWidthM;
  auto cell=[&](size_t x,size_t y,size_t z){return x+nx*(y+ny*z);};
  auto face=[&](int ax,size_t x,size_t y,size_t z){return ax==0?x+(nx+1)*(y+ny*z):ax==1?x+nx*(y+(ny+1)*z):x+nx*(y+ny*z);};
  auto U=[&](int ax,size_t x,size_t y,size_t z){size_t X=x+(ax==0),Y=y+(ax==1),Z=z+(ax==2);
   return .5*(c.velocity.component[ax][face(ax,x,y,z)]+c.velocity.component[ax][face(ax,X,Y,Z)]);};
  std::vector<double> nw(nx*ny*nz,0.),nc(nx*ny*nz,0.),K[2];
  auto vreman=[&](const double g[3][3]){double a2=0,b[3][3]={};for(int i=0;i<3;++i)for(int j=0;j<3;++j){a2+=g[i][j]*g[i][j];
    for(int m=0;m<3;++m)b[i][j]+=dx*dx*g[m][i]*g[m][j];}
   const double B=b[0][0]*b[1][1]-b[0][1]*b[0][1]+b[0][0]*b[2][2]-b[0][2]*b[0][2]+b[1][1]*b[2][2]-b[1][2]*b[1][2];
   return a2==0?0.:Cv*std::sqrt(std::max(0.,B)/a2);};
  for(size_t z=1;z+1<nz;++z)for(size_t y=1;y+1<ny;++y)for(size_t x=1;x+1<nx;++x){
   double gw[3][3],gc[3][3];const size_t p[3]={x,y,z};
   for(int d=0;d<3;++d)for(int k=0;k<3;++k){size_t lo[3]={x,y,z},hi[3]={x,y,z};--lo[d];++hi[d];
    gw[d][k]=(U(k,hi[0],hi[1],hi[2])-U(k,lo[0],lo[1],lo[2]))/(2*dx);gc[d][k]=gw[d][k];
    if(d==k){size_t h[3]={p[0],p[1],p[2]};++h[k];gc[d][k]=(c.velocity.component[k][face(k,h[0],h[1],h[2])]-c.velocity.component[k][face(k,x,y,z)])/dx;}}
   nw[cell(x,y,z)]=vreman(gw);nc[cell(x,y,z)]=vreman(gc);}
  for(int v=0;v<2;++v){K[v].assign(nx*ny*nz,0.);const auto& n=v?nc:nw;
   for(size_t i=0;i<K[v].size();++i){const double T=c.states[i].temperatureK,rho=c.states[i].GasDensity();
    K[v][i]=rho*cp*n[i]/Pr+.0241*std::pow(T/273.15,.81);}}
  for(int b=0;b<NB;++b)for(size_t z=1;z+1<nz;++z){const double h=(z+.5)*dx;if(h<bands[b][0]||h>=bands[b][1])continue;
   for(size_t y=1;y+1<ny;++y)for(size_t x=1;x+1<nx;++x){const size_t i=cell(x,y,z);
    const double X=(x+.5-.5*nx)*dx,Y=(y+.5-.5*ny)*dx,r=std::hypot(X,Y);
    for(int q=0;q<4;++q)if(r>=rings[q]&&r<rings[q+1]){nuW[b*4+q]+=nw[i];nuC[b*4+q]+=nc[i];cnt[b*4+q]+=1;}
    for(int ax=0;ax<2;++ax){if((ax==0?x:y)+2>=(ax==0?nx:ny))continue;const size_t j=ax==0?cell(x+1,y,z):cell(x,y+1,z);
     const double dT=std::abs(c.states[j].temperatureK-c.states[i].temperatureK)/dx*dx*dx;
     for(int v=0;v<2;++v){const double a=K[v][i],bb=K[v][j];
      flux[b*4+2*v]+=2*a*bb/(a+bb)*dT;flux[b*4+2*v+1]+=.5*(a+bb)*dT;}
     // Outward heat through cylinders: faces whose two cells straddle radius R.
     const double Xj=ax==0?X+dx:X,Yj=ax==0?Y:Y+dx,rj=std::hypot(Xj,Yj);
     for(int q=0;q<4;++q){const double R=cylinders[q];if((r<R)==(rj<R))continue;const double sign=(rj>=R)?1.:-1.;
      const double Ti=c.states[i].temperatureK,Tj=c.states[j].temperatureK;
      const double u=c.velocity.component[ax][ax==0?face(0,x+1,y,z):face(1,x,y+1,z)];
      const double rho=.5*(c.states[i].GasDensity()+c.states[j].GasDensity());
      budget[(b*4+q)*3+0]+=sign*rho*cp*u*(.5*(Ti+Tj)-300.)*dx*dx;
      const double a=K[0][i],bb=K[0][j],e=K[1][i],f=K[1][j];
      budget[(b*4+q)*3+1]-=sign*2*a*bb/(a+bb)*(Tj-Ti)*dx;budget[(b*4+q)*3+2]-=sign*.5*(e+f)*(Tj-Ti)*dx;}}}}
  ++states;std::cerr<<"state "<<argv[a]<<" t="<<c.simulationTimeS<<'\n';
 }
 std::cout<<std::setprecision(4)<<"states "<<states<<"\n1. Mean scalar nu_t (m2/s x1e3) by ring, RISE wide-gradient / FDS-style compact-diagonal\n   band(m)    r<0.1          0.1-0.2        0.2-0.3        0.3-0.5\n";
 for(int b=0;b<NB;++b){std::cout<<"  "<<bands[b][0]<<"-"<<bands[b][1]<<"  ";for(int q=0;q<4;++q)std::cout<<std::setw(6)<<1e3*nuW[b*4+q]/cnt[b*4+q]<<" / "<<std::setw(6)<<1e3*nuC[b*4+q]/cnt[b*4+q]<<"  ";std::cout<<'\n';}
 std::cout<<"\n2. Plane sum of |lateral diffusive heat flux| per state (kW), and ratio to RISE now (wide nu_t, harmonic face mean)\n   band(m)    wide+harm(RISE)  wide+arith  compact+harm  compact+arith(FDS)\n";
 for(int b=0;b<NB;++b){const double* f=&flux[b*4];const double base=f[0];
  std::cout<<"  "<<bands[b][0]<<"-"<<bands[b][1]<<"   "<<std::setw(8)<<f[0]/states/1e3<<"        "<<f[1]/base<<"       "<<f[2]/base<<"        "<<f[3]/base<<'\n';}
 std::cout<<"\n3. Outward heat through cylinders of radius R per band, mean of states (kW): resolved rho cp u_r theta / SGS+molecular RISE now / FDS-style\n   band(m)    R=0.10                 R=0.15                 R=0.20                 R=0.30\n";
 for(int b=0;b<NB;++b){std::cout<<"  "<<bands[b][0]<<"-"<<bands[b][1]<<"  ";
  for(int q=0;q<4;++q){const double* g=&budget[(b*4+q)*3];std::cout<<std::setw(6)<<g[0]/states/1e3<<" "<<std::setw(6)<<g[1]/states/1e3<<" "<<std::setw(6)<<g[2]/states/1e3<<"   ";}std::cout<<'\n';}
 return 0;
}
