// r271 scalar mixing (read-only): RISE's scalar-kernel Vreman nu_t (2dx central gradients of cell-centred
// velocity, Cv=0.07) from retained frames; ring means by height band, as scalar_mix.cpp section 1.
// argv: frames dir, first frame, frame count.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>
int main(int argc,char** argv){
 if(argc!=4)return 2;const int N=69,NZ=106,first=atoi(argv[2]),count=atoi(argv[3]);const double dx=0.030593116067411014,Cv=.07;
 const double bands[][2]={{.06,.16},{.24,.34},{.45,.55},{.70,.80},{.94,1.04},{1.46,1.56}};const double rings[]={0,.1,.2,.3,.5};
 std::vector<double> sum(24,0.),cnt(24,0.);openvdb::initialize();std::vector<float> u(3*size_t(N)*N*NZ);
 auto I=[&](int c,int x,int y,int z){return size_t(c)*N*N*NZ+x+N*(y+size_t(N)*z);};
 for(int f=first;f<first+count;++f){
  std::ostringstream name;name<<argv[1]<<"/frame_"<<std::setw(5)<<std::setfill('0')<<f<<".vdb";
  openvdb::io::File file(name.str());file.open();auto V=openvdb::gridPtrCast<openvdb::Vec3SGrid>(file.readGrid("velocity"));file.close();
  if(!V)return 3;auto a=V->getConstAccessor();
  for(int z=0;z<NZ;++z)for(int y=0;y<N;++y)for(int x=0;x<N;++x){auto v=a.getValue(openvdb::Coord(x+1,y+1,z+1));for(int c=0;c<3;++c)u[I(c,x,y,z)]=v[c];}
  for(int b=0;b<6;++b)for(int z=1;z+1<NZ;++z){const double h=(z+.5)*dx;if(h<bands[b][0]||h>=bands[b][1])continue;
   for(int y=1;y+1<N;++y)for(int x=1;x+1<N;++x){double g[3][3];const int p[3]={x,y,z};
    for(int d=0;d<3;++d)for(int c=0;c<3;++c){int lo[3]={x,y,z},hi[3]={x,y,z};--lo[d];++hi[d];g[d][c]=(u[I(c,hi[0],hi[1],hi[2])]-u[I(c,lo[0],lo[1],lo[2])])/(2*dx);}
    (void)p;double a2=0,B[3][3]={};for(int i=0;i<3;++i)for(int j=0;j<3;++j){a2+=g[i][j]*g[i][j];for(int m=0;m<3;++m)B[i][j]+=dx*dx*g[m][i]*g[m][j];}
    const double bb=B[0][0]*B[1][1]-B[0][1]*B[0][1]+B[0][0]*B[2][2]-B[0][2]*B[0][2]+B[1][1]*B[2][2]-B[1][2]*B[1][2];
    const double nu=a2==0?0.:Cv*std::sqrt(std::max(0.,bb)/a2);const double r=std::hypot((x-34)*dx,(y-34)*dx);
    for(int q=0;q<4;++q)if(r>=rings[q]&&r<rings[q+1]){sum[b*4+q]+=nu;cnt[b*4+q]+=1;}}}
 }
 for(int b=0;b<6;++b){std::printf("  %.2f-%.2f ",bands[b][0],bands[b][1]);for(int q=0;q<4;++q)std::printf("  %6.3f",1e3*sum[b*4+q]/cnt[b*4+q]);std::printf("\n");}
 return 0;}
