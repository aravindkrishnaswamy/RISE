// r271 (read-only): exact MAC velocity divergence (1/s) on horizontal planes of retained RISE checkpoints, for comparison
// with FDS's DIVERGENCE slices (cell values in both codes). Layers 1,3,6,9,16,32,65 (z = 0.05-2.0 m).
// Output float32 [state][layer][y][x]. argv: out file, checkpoints.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 const int Layers[7]={1,3,6,9,16,32,65};FILE* o=std::fopen(argv[1],"wb");if(!o)return 2;
 for(int a=2;a<argc;++a){
  MethaneRunCheckpoint checkpoint;std::string error;
  if(!LoadMethaneRunCheckpoint(argv[a],checkpoint,error)){std::cerr<<error<<'\n';return 3;}
  const std::size_t nx=checkpoint.dimensions[0],ny=checkpoint.dimensions[1],nz=checkpoint.dimensions[2];const double dx=checkpoint.cellWidthM;
  const std::vector<double>* f=checkpoint.velocity.component.data();
  if(f[0].size()!=(nx+1)*ny*nz||f[1].size()!=nx*(ny+1)*nz||f[2].size()!=nx*ny*(nz+1))return 4;
  std::vector<float> plane(nx*ny);
  for(const int z:Layers){for(std::size_t y=0;y<ny;++y)for(std::size_t x=0;x<nx;++x)
    plane[y*nx+x]=static_cast<float>((f[0][(z*ny+y)*(nx+1)+x+1]-f[0][(z*ny+y)*(nx+1)+x]+f[1][(z*(ny+1)+y+1)*nx+x]-f[1][(z*(ny+1)+y)*nx+x]
     +f[2][((z+1)*ny+y)*nx+x]-f[2][(z*ny+y)*nx+x])/dx);
   std::fwrite(plane.data(),sizeof(float),plane.size(),o);}
  std::cerr<<"state "<<argv[a]<<'\n';}
 std::fclose(o);return 0;
}
