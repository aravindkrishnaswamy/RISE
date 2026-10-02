//////////////////////////////////////////////////////////////////////
//
//  DL381RandomWalkReciprocityMC.cpp - an INDEPENDENT reference (no RISE
//    code) for DL-381: the DL-375 delta-lit random-walk scene
//    (tests/BDPTStrategyBalanceTest.cpp, kSceneDeltaLitRandomWalkDL375)
//    at roughness 0, light-traced with five boundary models.
//
//  Scene: omni (20 W/sr) at (1.8,-0.4,-0.3); sphere r 0.55 at
//  (0,-0.45,0.6), smooth boundary n 1.3, homogeneous isotropic interior
//  sigma_a 0.1 / sigma_s 10; Lambertian wall z = 0, |x|,|y| <= 1,
//  albedo 0.5, double sided.  The pinhole at (0,0,3.5) (fov 30, 32x32)
//  is replaced by an aperture disk of radius 0.15 at the pinhole; a
//  photon reaching it is binned by the pinhole-centre projection of its
//  last surface point, with RISE's pixel centres (ndc col/16 - 1,
//  1 - row/16).  The interior walk is RandomWalkSSS::SampleExit's loop
//  (one iteration per scatter OR boundary event, cap 64).
//
//  Modes (the only thing that changes is the two boundary ends):
//    0  physics: refracted entry, refracted exit (exact; reciprocal)
//    1  control: Lambertian sphere, albedo 0.5 (validates the harness)
//    2  LIGHT-family model (what BDPT/VCM/MLT's light family estimates):
//       refracted entry, cosine exit weighted Ft(cos)/c
//    3  EYE-family model (what PT estimates): Lambertian entry -- the
//       light-tracing adjoint of PT's NEE end, a cosine-sampled inward
//       start weighted n^2 (1 - F_int)/c (mean 1) -- refracted exit
//    4  symmetric model: Lambertian entry AND cosine exit
//  Tallies: A drops photons whose first interaction is a specular sphere
//  reflection (the class PT reaches at roughness 0); B keeps them but
//  drops their direct light->mirror->camera hit (the class the light
//  family reaches with the omni behind index-matched glass).
//  Output: sphere-interior / wall pixel means (the DL-375 masks) with
//  standard errors over 16 independent batches.
//
//  Build/run:  clang++ -O3 -std=c++17 -o mc tools/DL381RandomWalkReciprocityMC.cpp
//              ./mc <mode> <photons>      (4e8 photons: ~10 s on 16 cores)
//  Results (2026-10-02, 4e8 photons; mode 1 at 1.6e9) are in
//  docs/DL381_RANDOM_WALK_RECIPROCITY.md.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

struct V { double x, y, z; };
static inline V operator+(V a, V b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline V operator-(V a, V b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline V operator*(V a, double s){ return {a.x*s,a.y*s,a.z*s}; }
static inline double dot(V a, V b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline V norm(V a){ double l=std::sqrt(dot(a,a)); return a*(1.0/l); }
static inline V cross(V a, V b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }

struct Rng { uint64_t s[2];
	explicit Rng(uint64_t seed){ s[0]=seed*0x9E3779B97F4A7C15ULL+1; s[1]=(seed+7)*0xD1B54A32D192ED03ULL+3; for(int i=0;i<20;i++) next(); }
	uint64_t next(){ uint64_t s1=s[0]; const uint64_t s0=s[1]; s[0]=s0; s1^=s1<<23; s[1]=s1^s0^(s1>>17)^(s0>>26); return s[1]+s0; }
	double u(){ return (next()>>11)*(1.0/9007199254740992.0); } };

static const V Cs = {0,-0.45,0.6}; static const double R = 0.55, NI = 1.3;
static const double SA = 0.1, SS = 10.0, ST = SA+SS;
static const V Lp = {1.8,-0.4,-0.3}; static const double I0 = 20.0;
static const V Cam = {0,0,3.5}; static const double AP = 0.15;
static const double TAN15 = 0.2679491924311227;

static double Fresnel(double ci, double n1, double n2){ // unpolarized, ci>=0
	double s2 = (n1/n2)*(n1/n2)*(1-ci*ci); if(s2>=1) return 1;
	double ct = std::sqrt(1-s2);
	double rs = (n1*ci-n2*ct)/(n1*ci+n2*ct), rp = (n2*ci-n1*ct)/(n2*ci+n1*ct);
	return 0.5*(rs*rs+rp*rp); }
static bool Refract(V d, V n, double eta, V& t){ // n against d, eta=n1/n2
	double ci = -dot(d,n); double k = 1-eta*eta*(1-ci*ci); if(k<0) return false;
	t = norm(d*eta + n*(eta*ci-std::sqrt(k))); return true; }
static V Reflect(V d, V n){ return d - n*(2*dot(d,n)); }
static void Frame(V n, V& a, V& b){ a = norm(std::fabs(n.x)>0.5 ? cross(n,{0,1,0}) : cross(n,{1,0,0})); b = cross(n,a); }
static V CosDir(V n, Rng& r){ double u1=r.u(), u2=r.u(); double ct=std::sqrt(u1), st=std::sqrt(1-u1), ph=2*M_PI*u2; V a,b; Frame(n,a,b); return norm(a*(st*std::cos(ph))+b*(st*std::sin(ph))+n*ct); }
static V IsoDir(Rng& r){ double z=1-2*r.u(), ph=2*M_PI*r.u(), s=std::sqrt(std::max(0.0,1-z*z)); return {s*std::cos(ph),s*std::sin(ph),z}; }

// ray-sphere: return t>eps of first hit (from outside or inside)
static double HitSphere(V o, V d, bool inside){ V oc=o-Cs; double b=dot(oc,d), c=dot(oc,oc)-R*R, q=b*b-c; if(q<0) return -1; double s=std::sqrt(q);
	if(inside) return -b+s; double t=-b-s; return t>1e-9 ? t : -1; }
static double HitWall(V o, V d){ if(std::fabs(d.z)<1e-15) return -1; double t=-o.z/d.z; if(t<=1e-9) return -1; V p=o+d*t; return (std::fabs(p.x)<=1&&std::fabs(p.y)<=1)?t:-1; }

// Hemispherical cosine-weighted mean of the exact Fresnel transmission
// (exterior 1 -> NI), c = 2 int Ft(mu) mu dmu.
static double CNorm(){ double s=0; const int n=200000; for(int i=0;i<n;i++){ double mu=(i+0.5)/n; s+= (1-Fresnel(mu,1,NI))*mu; } return 2*s/n; }

static int RegionOf(int row, int col){ // DL-375 SphereHitsDL375
	const double c[3]={0,-0.45,0.6}, o[3]={0,0,3.5}; int hits=0;
	for(int di=-1;di<=1;di++) for(int dj=-1;dj<=1;dj++){ double x=(col+dj+0.5)/16.0-1, y=1-(row+di+0.5)/16.0; double d[3]={x*TAN15,y*TAN15,-1}; double l=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); for(double&v:d) v/=l;
		double oc[3]={o[0]-c[0],o[1]-c[1],o[2]-c[2]}; double b=d[0]*oc[0]+d[1]*oc[1]+d[2]*oc[2]; double q=b*b-(oc[0]*oc[0]+oc[1]*oc[1]+oc[2]*oc[2]-0.55*0.55); if(q>0) hits++; }
	return hits==9?1:(hits==0?2:0); }

struct Tally { std::vector<double> A, B; Tally():A(1024,0),B(1024,0){} };

int main(int argc, char** argv){
	const int mode = argc>1 ? std::atoi(argv[1]) : 0;
	const long long N = argc>2 ? std::atoll(argv[2]) : 20000000LL;
	const int nb = 16; const unsigned nt = std::thread::hardware_concurrency();
	const double cnorm = CNorm();
	// emit only into the cone that subtends the sphere
	const double dl = std::sqrt(dot(Cs-Lp,Cs-Lp)); const double cosMax = std::sqrt(1-(R/dl)*(R/dl));
	const double omega = 2*M_PI*(1-cosMax); const V axis = norm(Cs-Lp);
	std::vector<Tally> tal(nb); std::atomic<int> next(0);
	auto work = [&](){ for(;;){ int b = next++; if(b>=nb) return; Rng rng(1234567ULL+b*7919ULL+mode*104729ULL); Tally& T=tal[b];
		const long long n = N/nb; const double phi0 = I0*omega/(double)N;
		for(long long k=0;k<n;k++){
			double ct = 1-rng.u()*(1-cosMax), st=std::sqrt(1-ct*ct), ph=2*M_PI*rng.u(); V a,bb; Frame(axis,a,bb);
			V d = norm(a*(st*std::cos(ph))+bb*(st*std::sin(ph))+axis*ct), o = Lp; double w = phi0;
			bool first = true, firstSpec = false, lastFirstSpec = false;
			for(int depth=0; depth<200 && w>0; depth++){
				double ts = HitSphere(o,d,false), tw = HitWall(o,d), tc = -1;
				if(d.z>0){ double t=(Cam.z-o.z)/d.z; V p=o+d*t; if((p.x-Cam.x)*(p.x-Cam.x)+(p.y-Cam.y)*(p.y-Cam.y)<=AP*AP) tc=t; }
				double tmin=1e300; int what=-1;
				if(ts>0&&ts<tmin){tmin=ts;what=0;} if(tw>0&&tw<tmin){tmin=tw;what=1;} if(tc>0&&tc<tmin){tmin=tc;what=2;}
				if(what<0) break;
				if(what==2){ // camera aperture: pixel from pinhole projection of the origin
					if(first) break; V dc = o-Cam; if(dc.z>=0) break;
					double xn=(dc.x/(-dc.z))/TAN15, yn=(dc.y/(-dc.z))/TAN15; int col=(int)std::floor((xn+1)*16+0.5), row=(int)std::floor((1-yn)*16+0.5); // RISE pixel centres sit at ndc col/16-1 (DL-368)
					if(col<0||col>31||row<0||row>31) break;
					double xc=(col/16.0-1)*TAN15, yc=(1-row/16.0)*TAN15; double cp=1.0/std::sqrt(1+xc*xc+yc*yc);
					double omp = (TAN15/16.0)*(TAN15/16.0)*cp*cp*cp; double L = w/(M_PI*AP*AP*d.z*omp);
					if(!firstSpec) T.A[row*32+col]+=L; if(!lastFirstSpec) T.B[row*32+col]+=L; break; }
				V p = o+d*tmin; lastFirstSpec=false;
				if(what==1){ // wall
					V n = {0,0,d.z<0?1.0:-1.0}; w*=0.5; o=p; d=CosDir(n,rng); first=false; continue; }
				V n = norm(p-Cs); // outward
				if(mode==1){ w*=0.5; o=p; d=CosDir(n,rng); first=false; continue; }
				double ci = -dot(d,n); double F = Fresnel(ci,1,NI);
				if(rng.u()<F){ if(first){ firstSpec=true; lastFirstSpec=true; } o=p; d=Reflect(d,n); first=false; continue; }
				first=false; V t; Refract(d,n,1/NI,t); d=t; o=p;
				if(mode==3||mode==4){ // Lambertian ENTRY: the light-tracing adjoint of the eye family's NEE end (cosine-sampled inward start, weight eta^2 (1-F_int)/c, mean 1)
					V cd = CosDir(n*(-1.0),rng); double ct2 = -dot(cd,n); w *= NI*NI*(1-Fresnel(ct2,NI,1))/cnorm; d=cd; }
				// random walk inside, loop identical to RandomWalkSSS (cap 64)
				bool exited=false;
				for(int it=0; it<64; it++){
					double te = HitSphere(o,d,true); double s = -std::log(std::max(1e-300,1-rng.u()))/ST;
					if(s<te){ o=o+d*s; w*=SS/ST; d=IsoDir(rng); continue; }
					V q = o+d*te; V nn = norm(q-Cs); double cint = dot(d,nn); // >0
					double Fi = Fresnel(cint,NI,1);
					if(rng.u()<Fi){ o=q; d=Reflect(d,nn); continue; }
					if(mode==2||mode==4){ double cosT; V cd = CosDir(nn,rng); cosT=dot(cd,nn); w*= (1-Fresnel(cosT,1,NI))/cnorm; d=cd; }
					else { V tt; Refract(d,nn*(-1.0),NI,tt); d=tt; }
					o=q; exited=true; break; }
				if(!exited) { w=0; break; }
			}
		} } };
	std::vector<std::thread> th; for(unsigned i=0;i<nt;i++) th.emplace_back(work); for(auto& t:th) t.join();
	// region means per batch
	double mS[2][16], mW[2][16];
	for(int b=0;b<nb;b++) for(int k=0;k<2;k++){ double s=0,w=0; int ns=0,nw=0; for(int r=0;r<32;r++) for(int c=0;c<32;c++){ int g=RegionOf(r,c); double v=(k?tal[b].B:tal[b].A)[r*32+c]*nb; if(g==1){s+=v;ns++;} else if(g==2){w+=v;nw++;} } mS[k][b]=s/ns; mW[k][b]=w/nw; }
	for(int k=0;k<2;k++){ double a=0,a2=0,c=0,c2=0; for(int b=0;b<nb;b++){a+=mS[k][b];a2+=mS[k][b]*mS[k][b];c+=mW[k][b];c2+=mW[k][b]*mW[k][b];}
		a/=nb;c/=nb; double sa=std::sqrt(std::max(0.0,(a2/nb-a*a)/(nb-1))), sc=std::sqrt(std::max(0.0,(c2/nb-c*c)/(nb-1)));
		std::printf("mode %d N %lld tally %c: sphere %.6f +/- %.6f   wall %.6f +/- %.6f  (cnorm %.6f)\n", mode, N, k?'B':'A', a, sa, c, sc, cnorm); }
	return 0; }
