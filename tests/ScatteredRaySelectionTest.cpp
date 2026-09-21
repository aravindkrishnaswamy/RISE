// Frozen legacy decision code from b8be6e7353807c9561516ffa1885ed02ac3d65c8.
// Deliberately independent snapshot: pointer identity is a compatibility
// requirement, including historical count/type shortcuts and CDF boundaries.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "../src/Library/Interfaces/ISPF.h"
using namespace RISE;
class FrozenLegacy : public ScatteredRayContainer {
public:
    ScatteredRay* RandomlySelect(double,bool) const;
    ScatteredRay* RandomlySelectNonDiffuse(double,bool) const;
    ScatteredRay* RandomlySelectDiffuse(double,bool) const;
};
//! From the rays stored, randomly returns one given a value
ScatteredRay* FrozenLegacy::RandomlySelect(
		const double random,										///< [in] Random number to use in ray selection
		const bool bNM												///< [in] Should the spectral values be used when selecting?
		) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( freeidx == 1 ) {
		return &rays[0];
	}

	if( freeidx == 2 ) {
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
		cdf[i] = total + prob;
		total += prob;
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( random < (cdf[i]/total) ) {
				return &rays[i];
			}
		}
	}

	return 0;
}

//! From the rays stored, randomly returns a non diffuse ray
ScatteredRay* FrozenLegacy::RandomlySelectNonDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type!=ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	bool valid[kCapacity];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type!=ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					return &rays[i];
				}
			}
		}
	}


	return 0;
}


//! From the rays stored, randomly returns a diffuse ray
ScatteredRay* FrozenLegacy::RandomlySelectDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type==ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	bool valid[kCapacity];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type==ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					return &rays[i];
				}
			}
		}
	}

	return 0;
}


namespace {
int checks=0,failures=0;
void Check(bool ok,const char* text) {++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
ScatteredRay* Old(const FrozenLegacy& c,int mode,double u,bool nm) {
 return mode==0?c.RandomlySelect(u,nm):mode==1?c.RandomlySelectNonDiffuse(u,nm):c.RandomlySelectDiffuse(u,nm);
}
ScatteredRay* New(const ScatteredRayContainer& c,int mode,double u,bool nm,double* q) {
 return mode==0?c.RandomlySelect(u,nm,q):mode==1?c.RandomlySelectNonDiffuse(u,nm,q):c.RandomlySelectDiffuse(u,nm,q);
}
bool Eligible(const ScatteredRay& r,int mode) {return mode==0||(mode==1?(r.type!=ScatteredRay::eRayDiffuse):(r.type==ScatteredRay::eRayDiffuse));}
double Weight(const ScatteredRay& r,bool nm) {return nm?r.krayNM:ColorMath::MaxValue(r.kray);}
void Cases() {
 for(unsigned n=0;n<=ScatteredRayContainer::kCapacity;++n) for(int scenario=0;scenario<6;++scenario) {
  FrozenLegacy c;
  std::vector<RISEPel> savedPel;std::vector<double> savedNM;std::vector<const IORStack*> savedStack;
  for(unsigned i=0;i<n;++i) {
   ScatteredRay r; r.type=(scenario==5 ? (i==0?ScatteredRay::eRayDiffuse:ScatteredRay::eRayReflection) : (i%2?ScatteredRay::eRayDiffuse:ScatteredRay::eRayReflection));
   const double w=scenario==0?0:scenario==1?NEARZERO/(4*(n+1)):scenario==2?(i==0?0x1p-70:1):scenario==3?(i==0?1:0):double(i+1)/16;
   r.kray=RISEPel(w,w/2,w/4);r.krayNM=(scenario==4?double(n-i)/16:w);
   r.pdf=.125*(i+1);r.isDelta=(i%2==0);r.ray=Ray(Point3(i,0,0),Vector3(0,0,1));
   r.ior_stack=new IORStack(1.0+i*.125);
   savedPel.push_back(r.kray);savedNM.push_back(r.krayNM);savedStack.push_back(r.ior_stack);
   Check(c.AddScatteredRay(r),"fixture admitted");
  }
  for(int mode=0;mode<3;++mode) for(bool nm:{false,true}) {
   double total=0;unsigned eligible=0;for(unsigned i=0;i<n;++i)if(Eligible(c[i],mode)){total+=Weight(c[i],nm);++eligible;}
   std::vector<double> variates={0,0x1p-71,.25,.5,.75,std::nextafter(1.,0.)};
   if(total>NEARZERO){double partial=0;for(unsigned i=0;i<n;++i)if(Eligible(c[i],mode)){partial+=Weight(c[i],nm);const double edge=partial/total;if(edge>=0&&edge<1)variates.push_back(edge);if(edge>0&&edge<=1)variates.push_back(std::nextafter(edge,0.));}}
   for(double u:variates) {
    const auto* expected=Old(c,mode,u,nm);double q=-1;
    const auto* got=New(c,mode,u,nm,&q);
    Check(got==expected,"selected pointer matches frozen legacy");
    Check(New(c,mode,u,nm,nullptr)==expected,"probability omitted preserves pointer");
    double want=0;
    if(got){const bool shortcut=mode==0?n==1:(n<=2&&eligible==1);want=shortcut?1:Weight(*got,nm)/total;}
    Check(q==want,"probability is shortcut1 or selected eligible weight/total");
    for(unsigned i=0;i<n;++i){Check(c[i].pdf==.125*(i+1)&&c[i].isDelta==(i%2==0)&&c[i].ray.origin.x==i&&c[i].ray.Dir().z==1&&c[i].ior_stack==savedStack[i]&&c[i].ior_stack->top()==1.0+i*.125&&c[i].krayNM==savedNM[i]&&c[i].kray.r==savedPel[i].r&&c[i].kray.g==savedPel[i].g&&c[i].kray.b==savedPel[i].b,"selection preserves ray/pdf/IOR metadata");}
   }
   // Stratified CDF frequencies provide a separate distribution check.
   // Finite sampling bounds each outcome's count error by at most two.
   if(total>NEARZERO) {
    unsigned hits[ScatteredRayContainer::kCapacity]={};const unsigned count=4096;
    for(unsigned j=0;j<count;++j){double q;auto* selected=New(c,mode,(j+.5)/count,nm,&q);if(selected)++hits[selected-&c[0]];}
    for(unsigned i=0;i<n;++i){const double prob=Eligible(c[i],mode)?Weight(c[i],nm)/total:0;Check(std::fabs(double(hits[i])-prob*count)<=2,"empirical CDF agrees with reported distribution");}
   }
  }
 }
}
}
int main(){Cases();std::printf("ScatteredRaySelectionTest checks=%d failures=%d\n",checks,failures);return failures?1:0;}
