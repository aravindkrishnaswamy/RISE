#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponse.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace RISE::HyLIoS;
namespace {
int failures=0;
void Check(bool ok, const std::string& message)
{
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Near(double actual, double expected, const std::string& message, double tolerance=2e-13)
{
    // Scale BEFORE subtracting: tiny positive expectations must reject zero,
    // without overflowing a reciprocal or underflowing an absolute tolerance.
    int exponent=0;
    const double normalizedExpected=std::frexp(expected,&exponent);
    const double normalizedActual=std::scalbn(actual,-exponent);
    const bool agrees=expected==0 ? actual==0 :
        ((expected>0 ? actual>0 : actual<0) && std::isfinite(normalizedActual) &&
        std::abs(normalizedActual-normalizedExpected)<=tolerance*std::abs(normalizedExpected));
    std::ostringstream detail;
    detail<<message<<" actual="<<std::hexfloat<<actual<<" expected="<<expected;
    Check(std::isfinite(actual) && agrees,detail.str());
}
template<class Exception, class F> void Throws(F f, const std::string& message)
{
    try { f(); Check(false,message+" (accepted)"); }
    catch (const Exception&) {}
    catch (const std::exception& e) { Check(false,message+" (wrong exception: "+e.what()+")"); }
}
LocalResponsePoint Centre(const LocalResponseGrid& grid, std::size_t cell)
{
    const auto np=grid.phiEdges.size()-1, ti=cell/np, pi=cell%np;
    return {(grid.tEdges[ti]+grid.tEdges[ti+1])/2,(grid.phiEdges[pi]+grid.phiEdges[pi+1])/2};
}
double ReferenceArea(const LocalResponseGrid& grid, std::size_t cell)
{
    const auto np=grid.phiEdges.size()-1, ti=cell/np, pi=cell%np;
    return (grid.tEdges[ti+1]-grid.tEdges[ti])*(grid.phiEdges[pi+1]-grid.phiEdges[pi])/2;
}
double Selector32(double u)
{
    return std::scalbn(std::floor(std::scalbn(u,32)),-32);
}
LocalResponseDefinition Uniform(double diffuse, double mirror)
{
    LocalResponseDefinition d;
    d.incident={{0,0.15,1},{0,0.6,2.7,LocalResponseTwoPi}};
    d.outgoing={{0,0.4,1},{0,1.2,LocalResponseTwoPi}};
    d.wavelengthNm={500};
    d.continuous.assign(6*4,diffuse/LocalResponsePi);
    d.mirror.assign(6,mirror);
    return d;
}

void NonsymmetricUnequalArea()
{
    LocalResponseDefinition d;
    d.incident={{0,0.2,1},{0,0.7,LocalResponseTwoPi}};
    d.outgoing={{0,0.1,1},{0,1.1,LocalResponseTwoPi}};
    d.wavelengthNm={500};
    const double mass[4][4]={{.3,.1,0,.04},{.1,.5,.05,.02},{.6,0,.02,.02},{.2,.1,.3,.04}};
    d.mirror={.02,.03,.04,.05};
    for (const auto& row:mass)
        for (std::size_t j=0;j<4;++j) d.continuous.push_back(row[j]/ReferenceArea(d.outgoing,j));
    const auto response=HyLIoSLocalResponse::Create(d);
    double forwardPower=0, transposePower=0;
    for (std::size_t i=0;i<4;++i) {
        double expected=d.mirror[i];
        for (double m:mass[i]) expected+=m;
        Near(response->ForwardRowMassNM(Centre(d.incident,i),500),expected,"forward row integrates returned mass");
        forwardPower+=ReferenceArea(d.incident,i)*expected;
    }
    for (std::size_t j=0;j<4;++j) {
        const auto wo=Centre(d.outgoing,j);
        double expected=0;
        for (std::size_t i=0;i<4;++i)
            expected+=mass[i][j]/ReferenceArea(d.outgoing,j)*ReferenceArea(d.incident,i);
        const double c=response->CameraContinuousMassNM(wo,500);
        Near(c,expected,"transpose column uses incident projected area");
        transposePower+=ReferenceArea(d.outgoing,j)*c;
        const std::size_t inverseCell=response->IncidentCell(HyLIoSLocalResponse::PlanarMirror(wo));
        const double atom=d.mirror[inverseCell], z=c+atom;
        Near(response->CameraMirrorMassNM(wo,500),atom,"mirror coefficient queries inverse-map incident cell");
        double integratedPdf=0, prefix=0;
        for (std::size_t i=0;i<4;++i) {
            const auto wi=Centre(d.incident,i);
            const double f=mass[i][j]/ReferenceArea(d.outgoing,j);
            Near(response->EvaluateContinuousNM(wi,wo,500),f,"nonsymmetric finite f is unmodified");
            Near(response->CameraPdfNM(wi,wo,500),f*std::sqrt(wi.t)/z,"mixture PDF includes delta event probability");
            const auto np=d.incident.phiEdges.size()-1, ti=i/np, pi=i%np;
            // Independent solid-angle quadrature: dOmega=dmu*dphi; q is linear
            // in mu in each cell, so one midpoint in mu is analytically exact.
            const double mu0=std::sqrt(d.incident.tEdges[ti]), mu1=std::sqrt(d.incident.tEdges[ti+1]);
            const double mu=(mu0+mu1)/2;
            integratedPdf+=response->CameraPdfNM({mu*mu,wi.phi},wo,500)*(mu1-mu0)*
                (d.incident.phiEdges[pi+1]-d.incident.phiEdges[pi]);
            const double cellMass=f*ReferenceArea(d.incident,i);
            if (cellMass>0) {
                const double cellDraw=(prefix+cellMass/2)/c;
                const double continuousDraw=c<=atom ? 0 : std::nextafter(1.0,0.0);
                const auto sample=response->SampleCameraNM(wo,500,continuousDraw,.5,cellDraw,.31,.77,{53});
                Check(sample.event==LocalResponseSample::Event::Continuous,"positive continuous event selected");
                Check(response->IncidentCell(sample.incident)==i,"CDF selects transpose mass, not forward row");
                Near(sample.pdf,response->CameraPdfNM(sample.incident,wo,500),"sample reports aggregate query PDF");
                Near(sample.weight,z,"continuous weight is column plus mirror, even above one");
                Near(response->EvaluateContinuousNM(sample.incident,wo,500)*std::sqrt(sample.incident.t)/sample.pdf,
                    z,"finite f*cos/pdf matches mixture throughput");
            }
            prefix+=cellMass;
        }
        Near(integratedPdf,c/z,"solid-angle PDF integrates to continuous branch probability");
    }
    for (std::size_t i=0;i<4;++i) transposePower+=ReferenceArea(d.incident,i)*d.mirror[i];
    Near(forwardPower,transposePower,"integrated forward power equals projected transpose power");
    Check(response->CameraContinuousMassNM(Centre(d.outgoing,0),500)>1,"passive rows permit column mass greater than one");
    const auto a=Centre(d.incident,0), b=Centre(d.outgoing,3);
    Check(response->EvaluateContinuousNM(a,b,500)!=response->EvaluateContinuousNM(b,a,500),"nonreciprocity is preserved");
    const double original=response->EvaluateContinuousNM(a,b,500);
    d.continuous.assign(d.continuous.size(),0);
    d.mirror.assign(d.mirror.size(),0);
    Near(response->EvaluateContinuousNM(a,b,500),original,"published response owns immutable source copies");
}

void LambertMirrorMixture()
{
    for (const auto& coefficients:std::vector<std::vector<double>>{{.7,0},{0,.8},{.3,.4}}) {
        const double rho=coefficients[0], atom=coefficients[1];
        const auto response=HyLIoSLocalResponse::Create(Uniform(rho,atom));
        const LocalResponsePoint wo{.37,1.77}, wi{.28,.53};
        Near(response->ForwardRowMassNM(wi,500),rho+atom,"Lambert/mirror forward row analytic mass");
        Near(response->CameraContinuousMassNM(wo,500),rho,"Lambert column analytic mass");
        Near(response->CameraMirrorMassNM(wo,500),atom,"planar mirror inverse-map Jacobian is unity");
        Near(response->CameraPdfNM(wi,wo,500),rho*std::sqrt(wi.t)/(LocalResponsePi*(rho+atom)),"analytic mixed PDF");
        const auto sample=response->SampleCameraNM(wo,500,.999,.5,.4,.5,.6,{53});
        Near(sample.weight,rho+atom,"all components carry total mixture weight");
        if (atom>0) {
            Check(sample.event==LocalResponseSample::Event::PlanarMirror,"mirror branch selected");
            Near(sample.pdf,0,"delta event reports zero solid-angle PDF");
            Near(sample.eventProbability,atom/(rho+atom),"mirror branch probability");
            const auto incident=HyLIoSLocalResponse::ToDirection(sample.incident), outgoing=HyLIoSLocalResponse::ToDirection(wo);
            Near(incident.x,-outgoing.x,"mirror retains actual query x, no bin-center map");
            Near(incident.y,-outgoing.y,"mirror retains actual query y");
            Near(incident.z,outgoing.z,"mirror retains actual query cosine");
            Near(response->CameraPdfNM(sample.incident,wo,500),rho*std::sqrt(wo.t)/(LocalResponsePi*(rho+atom)),
                "query at atom direction still gives continuous sub-density");
        } else Check(sample.event==LocalResponseSample::Event::Continuous,"Lambert-only continuous branch");
    }
}

void SpectralSupportAndNodeMixture()
{
    LocalResponseDefinition constant;
    constant.incident={{0,1},{0,LocalResponseTwoPi}}; constant.outgoing=constant.incident;
    constant.wavelengthNm={250,2500}; constant.continuous={.1,.1}; constant.mirror={0,0};
    const auto same=HyLIoSLocalResponse::Create(constant);
    for (double nm:std::vector<double>{std::nextafter(250.0,2500.0),std::nextafter(2500.0,250.0),1375}) {
        Near(same->EvaluateContinuousNM({.5,.3},{.7,.8},nm),.1,"constant F survives near-endpoint interpolation");
        Near(same->CameraContinuousMassNM({.7,.8},nm),.1*LocalResponsePi,"constant column survives near-endpoint interpolation");
        same->RequireSamplingSupport({.7,.8},nm,{32});
        const auto sample=same->SampleCameraNM({.7,.8},nm,.5,.5,.5,.5,.5,{32});
        Near(sample.weight,.1*LocalResponsePi,"redundant spectral node does not reject endpoint sampling");
    }
    LocalResponseDefinition proportional;
    proportional.incident={{0,.25,.6,1},{0,LocalResponseTwoPi}}; proportional.outgoing={{0,1},{0,LocalResponseTwoPi}};
    proportional.wavelengthNm={250,2500}; proportional.continuous={.01,.02,.04,.03,.06,.12}; proportional.mirror.assign(6,0);
    const auto equivalent=HyLIoSLocalResponse::Create(proportional);
    equivalent->RequireSamplingSupport({.7,.8},std::nextafter(250.0,2500.0),{32});
    Check(equivalent->SampleCameraNM({.7,.8},std::nextafter(250.0,2500.0),.5,.5,.5,.5,.5,{32}).event==
        LocalResponseSample::Event::Continuous,"exact proportional angular laws avoid redundant tiny latent node");
    // A one-ulp physical shape change must not be swallowed by approximate
    // normalized-PDF equality or an overflowing cross-product comparison.
    proportional.continuous.back()=std::nextafter(.12,1.0);
    const auto distinct=HyLIoSLocalResponse::Create(proportional);
    Throws<std::domain_error>([&]{distinct->RequireSamplingSupport({.7,.8},std::nextafter(250.0,2500.0),{32});},
        "one-ulp nonproportional shape retains its separate spectral proposal");
    std::uint64_t state=0x1f2e3d4c5b6a7988ULL;
    for (int k=0;k<32;++k) {
        state=state*6364136223846793005ULL+1442695040888963407ULL;
        const std::uint64_t m0=(state>>17)|1U;
        state=state*6364136223846793005ULL+1442695040888963407ULL;
        const std::uint64_t m1=(state>>17)|1U;
        auto exact=proportional; exact.continuous.clear();
        for (const auto mantissa:std::vector<std::uint64_t>{m0,m1})
            for (const std::uint64_t shape:std::vector<std::uint64_t>{1,3,5})
                exact.continuous.push_back(std::ldexp(static_cast<double>(mantissa*shape),-55));
        HyLIoSLocalResponse::Create(exact)->RequireSamplingSupport({.7,.8},std::nextafter(250.0,2500.0),{32});
    }
    auto subnormal=proportional;
    subnormal.incident={{0,.5,1},{0,LocalResponseTwoPi}};
    const double unit=std::numeric_limits<double>::denorm_min();
    subnormal.continuous={16*unit,32*unit,48*unit,96*unit}; subnormal.mirror.assign(4,0);
    const auto subnormalLaw=HyLIoSLocalResponse::Create(subnormal);
    subnormalLaw->RequireSamplingSupport({.7,.8},std::nextafter(250.0,2500.0),{32});
    Near(subnormalLaw->EvaluateContinuousNM({.2,.1},{.7,.8},std::nextafter(250.0,2500.0)),16*unit,
        "subnormal proportional coefficients survive redundant-node bypass");
    auto d=Uniform(0,.7);
    d.wavelengthNm={500,700};
    d.continuous.resize(48,.4/LocalResponsePi);
    d.mirror.resize(12,0);
    const auto response=HyLIoSLocalResponse::Create(d);
    const LocalResponsePoint wo{.48,2.2}, wi{.77,5.0};
    Check(response->Covers(500,700),"complete closed node coverage");
    Check(!response->Covers(499,700)&&!response->Covers(500,701)&&!response->Covers(700,500),"no extrapolation or reversed interval");
    Check(response->SampleCameraNM(wo,500,.1,.5,.7,.2,.3,{53}).event==LocalResponseSample::Event::PlanarMirror,
        "atom-only wavelength does not borrow continuous support");
    Check(response->SampleCameraNM(wo,700,.9,.5,.7,.2,.3,{53}).event==LocalResponseSample::Event::Continuous,
        "continuous-only wavelength does not borrow atom support");
    Near(response->EvaluateContinuousNM(wi,wo,500),0,"dark continuous hero remains dark");
    Near(response->EvaluateContinuousNM(wi,wo,600),.2/LocalResponsePi,"interpolate physical f");
    Near(response->CameraContinuousMassNM(wo,600),.2,"interpolate physical column");
    Near(response->CameraMirrorMassNM(wo,600),.35,"interpolate mirror coefficient only");
    Near(response->CameraPdfNM(wi,wo,600),.2*std::sqrt(wi.t)/(LocalResponsePi*.55),"spectral mixed PDF normalization");
    const auto c=response->SampleCameraNM(wo,600,.1,.5,.7,.2,.3,{53});
    const auto m=response->SampleCameraNM(wo,600,.9,.5,.7,.2,.3,{53});
    Check(c.event==LocalResponseSample::Event::Continuous&&m.event==LocalResponseSample::Event::PlanarMirror,"spectral mixture changes branch support");
    Near(c.weight,.55,"spectral continuous total mixture weight"); Near(m.weight,.55,"spectral mirror total mixture weight");

    // Two disjoint incoming supports with substantially unequal column masses.
    // A normalized-PDF interpolation using 50/50 node choice fails this test.
    LocalResponseDefinition unequal;
    unequal.incident={{0,.2,1},{0,LocalResponseTwoPi}};
    unequal.outgoing={{0,1},{0,LocalResponseTwoPi}};
    unequal.wavelengthNm={500,700};
    unequal.continuous={.1/LocalResponsePi,0,0,.8/LocalResponsePi};
    unequal.mirror={0,0,0,0};
    const auto r=HyLIoSLocalResponse::Create(unequal);
    const double p0=.01/(.01+.32);
    const auto first=r->SampleCameraNM(wo,600,.5,p0/2,.5,.5,.5,{53});
    const auto second=r->SampleCameraNM(wo,600,.5,p0+(1-p0)/2,.5,.5,.5,{53});
    Check(r->IncidentCell(first.incident)==0&&r->IncidentCell(second.incident)==1,"spectral nodes weighted by physical column masses");
    constexpr int n=10000;
    int count0=0;
    double weightedT=0;
    for (int k=0;k<n;++k) {
        const auto sample=r->SampleCameraNM(wo,600,.5,(k+.5)/n,.5,.5,.5,{53});
        if (r->IncidentCell(sample.incident)==0) ++count0;
        weightedT+=sample.weight*sample.incident.t;
    }
    Check(std::abs(count0-n*p0)<=1,"stratified event measure follows column-weighted spectral mixture");
    // Constant radiance in each incident cell: independent analytic transpose.
    Near(weightedT/n,.01*.1+.32*.6,"transpose estimator equals analytic piecewise source integral",4e-5);
    Throws<std::out_of_range>([&]{response->EvaluateContinuousNM(wi,wo,499);},"finite f refuses out-of-domain wavelength");
    Throws<std::out_of_range>([&]{response->SampleCameraNM(wo,701,.5,.5,.5,.5,.5,{53});},"sampler refuses out-of-domain wavelength");
    Throws<std::out_of_range>([&]{response->RequireCoverage(400,700);},"preparation checks complete requested interval");
    const auto single=HyLIoSLocalResponse::Create(Uniform(.1,0));
    Check(single->Covers(500,500)&&!single->Covers(500,500.01),"single spectral node covers only itself");
}

void EmptyAndBoundaries()
{
    auto d=Uniform(0,0);
    const auto zero=HyLIoSLocalResponse::Create(d);
    const LocalResponsePoint wo{.1,.2};
    const auto none=zero->SampleCameraNM(wo,500,0,.5,0,.5,.5,{53});
    Check(none.event==LocalResponseSample::Event::None,"empty column emits no event");
    Near(none.weight,0,"empty event carries no weight"); Near(zero->CameraPdfNM(wo,wo,500),0,"empty PDF is zero");
    // Some zero columns coexist with nonzero columns, not just an all-zero table.
    d.continuous[0]=.1;
    const auto sparse=HyLIoSLocalResponse::Create(d);
    Near(sparse->CameraContinuousMassNM(Centre(d.outgoing,1),500),0,"individual empty column remains empty");
    Check(sparse->SampleCameraNM(Centre(d.outgoing,1),500,0,.5,0,.5,.5,{53}).event==LocalResponseSample::Event::None,"sparse empty column emits no ray");
    // Spectral interpolation with a zero upper-node column must never select
    // that empty CDF, including immediately below the continuous/delta split.
    auto disappearing=Uniform(.2,.4);
    disappearing.wavelengthNm={500,700};
    disappearing.continuous.resize(48,0); disappearing.mirror.resize(12,.4);
    const auto fade=HyLIoSLocalResponse::Create(disappearing);
    const double split=fade->CameraContinuousMassNM(wo,600)/
        (fade->CameraContinuousMassNM(wo,600)+fade->CameraMirrorMassNM(wo,600));
    Check(fade->SampleCameraNM(wo,600,std::nextafter(split,0.0),.5,0,.5,.5,{53}).event==LocalResponseSample::Event::Continuous,
        "last continuous event avoids empty spectral-node CDF");
    Check(zero->IncidentCell({.15,.6})==4,"interior boundaries belong to upper half-open cell");
    Check(zero->IncidentCell({.2,LocalResponseTwoPi})==3,"periodic azimuth endpoint belongs to zero bin");
    Check(zero->IncidentCell({1,5.8})==3,"normal incidence has canonical phi zero");
    Near(HyLIoSLocalResponse::CanonicalPoint({.5,-.3}).phi,LocalResponseTwoPi-.3,"negative azimuth wraps with declared orientation");
    const auto from=HyLIoSLocalResponse::FromDirection({1,1,2});
    Near(from.t,2.0/3,"Cartesian helper normalizes t"); Near(from.phi,LocalResponsePi/4,"right-handed azimuth convention");
    const double huge=std::numeric_limits<double>::max();
    Near(HyLIoSLocalResponse::FromDirection({huge,huge,huge}).t,1.0/3,"finite huge Cartesian vector avoids norm overflow");
    const double denorm=std::numeric_limits<double>::denorm_min();
    Near(HyLIoSLocalResponse::FromDirection({0,0,denorm}).t,1,"pure subnormal normal direction");
    const auto mixed=HyLIoSLocalResponse::FromDirection({denorm,2*denorm,3*denorm});
    Near(mixed.t,9.0/14,"mixed subnormal normalization avoids reciprocal overflow");
    Near(mixed.phi,std::atan2(2.0,1.0),"mixed subnormal azimuth survives scaling");
    Near(HyLIoSLocalResponse::FromDirection({-denorm,-denorm,denorm}).t,1.0/3,"signed mixed subnormal normalization");

    auto grazing=Uniform(.5,0);
    grazing.incident={{0,1e-8,1},{0,LocalResponseTwoPi}};
    grazing.continuous.assign(8,.5/LocalResponsePi); grazing.mirror.assign(2,0);
    const auto g=HyLIoSLocalResponse::Create(grazing);
    const auto sample=g->SampleCameraNM(wo,500,0,.5,0,.25,.5,{53});
    Near(sample.incident.t,2.5e-9,"near-grazing projected cell sampling uses t, not uniform mu");
    Near(sample.pdf,.5*std::sqrt(sample.incident.t)/(LocalResponsePi*.5),"near-grazing solid-angle Jacobian uses mu");
    const double smallest=std::nextafter(0.0,1.0), largest=std::nextafter(1.0,0.0);
    const auto bottom=g->SampleCameraNM(wo,500,0,.5,0,smallest,smallest,{53});
    const auto top=g->SampleCameraNM(wo,500,largest,.5,largest,largest,largest,{53});
    Check(bottom.incident.t>0&&g->IncidentCell(bottom.incident)==0,"rounded lower sample stays inside selected grazing cell");
    Check(top.incident.t<1&&g->IncidentCell(top.incident)==1,"rounded upper sample stays inside selected final cell");
}

void InvalidAndBudgets()
{
    const double nan=std::numeric_limits<double>::quiet_NaN(), inf=std::numeric_limits<double>::infinity();
    for (double invalid:std::vector<double>{-1,nan,inf}) {
        auto d=Uniform(.1,0); d.continuous[0]=invalid;
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"invalid continuous coefficient rejected");
        d=Uniform(.1,0); d.mirror[0]=invalid;
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"invalid mirror coefficient rejected");
    }
    auto d=Uniform(.8,.3);
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"continuous plus atom violates row passivity");
    d=Uniform(.1,0); d.continuous.pop_back();
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"coefficient extent rejected");
    d=Uniform(.1,0); d.mirror.clear();
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"missing explicit atom coefficient extent rejected");
    for (int kind=0;kind<5;++kind) {
        d=Uniform(.1,0);
        // Keep the original cell cardinality so coefficient-extent validation
        // cannot hide a missing duplicate-edge rejection in ValidateAxis.
        if (kind==0) d.incident.tEdges={0,0,1};
        if (kind==1) d.incident.phiEdges.back()=6;
        if (kind==2) d.outgoing.tEdges[1]=nan;
        if (kind==3) d.outgoing.tEdges={0,std::nextafter(0.0,1.0),1};
        if (kind==4) d.wavelengthNm[0]=0;
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},kind==0 ?
            "duplicate grid edge rejected with matching coefficient extents" : "invalid grid/spectral domain rejected");
    }
    d=Uniform(.1,0); d.wavelengthNm={500,500}; d.continuous.resize(48,.1); d.mirror.resize(12,0);
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"duplicate spectral nodes rejected");
    d=Uniform(.1,0);
    const auto response=HyLIoSLocalResponse::Create(d);
    auto limits=LocalResponseLimits{};
    limits.maximumStorageBytes=response->Storage().storageBytes;
    Check(HyLIoSLocalResponse::Create(d,limits)->Storage().storageBytes==limits.maximumStorageBytes,"exact logical owned-storage budget accepted");
    --limits.maximumStorageBytes;
    Throws<std::length_error>([&]{HyLIoSLocalResponse::Create(d,limits);},"one byte below complete logical storage rejected");
    const std::size_t logicalDoubles=3+4+3+3+1+24+24+6+6+4+4;
    // Object byte count is not exposed; compare two dimension shapes to cancel it.
    const auto s1=HyLIoSLocalResponse::EstimateStorage(1,2,3,2,2);
    const auto s2=HyLIoSLocalResponse::EstimateStorage(2,2,3,2,2);
    Near(static_cast<double>(s2.storageBytes-s1.storageBytes),static_cast<double>((1+24+24+6+4)*sizeof(double)+4+4*sizeof(int)),
        "storage includes wavelength, F, CDF, mirror, interval minima, exponents and identity flags");
    Check(s1.storageBytes>logicalDoubles*sizeof(double),"storage includes object plus all logical array payloads");
    limits=LocalResponseLimits{}; limits.maximumCellsPerGrid=5;
    Throws<std::length_error>([&]{HyLIoSLocalResponse::Create(d,limits);},"cell cardinality cap enforced");
    const auto maximum=std::numeric_limits<std::size_t>::max();
    limits={maximum,maximum,maximum,maximum};
    Throws<std::length_error>([&]{HyLIoSLocalResponse::EstimateStorage(1,maximum,2,1,1,limits);},"dimension multiplication overflow rejected without allocation");
    Throws<std::length_error>([&]{HyLIoSLocalResponse::EstimateStorage(1,maximum/32,1,1,1,limits);},"storage byte overflow rejected without allocation");
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::EstimateStorage(0,1,1,1,1);},"zero cardinality rejected");
    const LocalResponsePoint wo{.5,.3};
    for (double invalid:std::vector<double>{-1,1,nan,inf})
        Throws<std::invalid_argument>([&]{response->SampleCameraNM(wo,500,invalid,.5,.5,.5,.5,{53});},"invalid event draw rejected");
    Throws<std::invalid_argument>([&]{response->SampleCameraNM(wo,500,.5,.5,.5,0,.5,{53});},"closed projected lower draw rejected");
    Throws<std::invalid_argument>([&]{response->SampleCameraNM(wo,500,.5,.5,.5,.5,1,{53});},"closed projected upper draw rejected");
    Throws<std::invalid_argument>([&]{response->CameraPdfNM({0,0},wo,500);},"grazing query excluded explicitly");
    Throws<std::invalid_argument>([&]{response->CameraPdfNM({.2,nan},wo,500);},"NaN angle rejected");
    Throws<std::out_of_range>([&]{response->CameraContinuousMassNM(wo,nan);},"NaN wavelength rejected");
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::FromDirection({0,0,0});},"zero Cartesian direction rejected");
    Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::FromDirection({0,0,-1});},"inward Cartesian direction rejected");
    // A very ill-scaled table is refused rather than dropping positive support
    // while constructing a finite-precision cumulative distribution.
    LocalResponseDefinition lost;
    lost.incident={{0,.5,1},{0,LocalResponseTwoPi}};
    lost.outgoing={{0,1},{0,LocalResponseTwoPi}};
    lost.wavelengthNm={500}; lost.continuous={.1,1e-100}; lost.mirror={0,0};
    Throws<std::overflow_error>([&]{HyLIoSLocalResponse::Create(lost);},"CDF rounding cannot silently erase positive cell support");
    lost.continuous={std::numeric_limits<double>::denorm_min(),0};
    lost.incident.tEdges={0,.1,1};
    const auto belowOutput=HyLIoSLocalResponse::Create(lost);
    belowOutput->RequireSamplingSupport({.5,.3},500,{32});
    Near(belowOutput->CameraPdfNM({.05,.3},{.5,.3},500),std::sqrt(.05)/(.1*LocalResponsePi),
        "scaled proposal survives a physical mass below final scalar-output precision");
    Throws<std::overflow_error>([&]{belowOutput->CameraContinuousMassNM({.5,.3},500);},
        "unrepresentable final physical mass fails explicitly without banning the input table");
}

void TinyPositiveAndFiniteSamplingSupport()
{
    constexpr double tiny=1e-200;
    const auto r=HyLIoSLocalResponse::Create(Uniform(tiny,0));
    const LocalResponsePoint wo{.5,.2}, wi{.4,.3};
    Near(r->EvaluateContinuousNM(wi,wo,500),tiny/LocalResponsePi,"tiny positive f remains positive");
    Near(r->CameraContinuousMassNM(wo,500),tiny,"tiny positive physical column is not floored");
    const auto sample=r->SampleCameraNM(wo,500,.5,.5,.5,.5,.5,{32});
    Near(sample.weight,tiny,"tiny positive throughput is not floored or zeroed");
    Near(sample.pdf,std::sqrt(sample.incident.t)/LocalResponsePi,"tiny scale cancels only in proposal PDF");

    // Individually representable rare continuous and node branches have a
    // product below one Sobol32 interval. They need independent selector draws.
    LocalResponseDefinition rare;
    rare.incident={{0,.5,1},{0,LocalResponseTwoPi}}; rare.outgoing={{0,1},{0,LocalResponseTwoPi}};
    rare.wavelengthNm={500,700}; rare.mirror={.5,.5,.5,.5};
    rare.continuous={4e-12/LocalResponsePi,0,0,4*(1e-6-1e-12)/LocalResponsePi};
    const auto independent=HyLIoSLocalResponse::Create(rare);
    constexpr double sobolStep=0x1p-32;
    independent->RequireSamplingSupport(wo,600,{32});
    const auto rareSample=independent->SampleCameraNM(wo,600,sobolStep,sobolStep,.5,.5,.5,{32});
    Check(rareSample.event==LocalResponseSample::Event::Continuous && independent->IncidentCell(rareSample.incident)==0,
        "independent node draw retains rare-times-rare finite-grid support");
    Near(rareSample.pdf,independent->CameraPdfNM(rareSample.incident,wo,600),"rare branch reports aggregate physical PDF");
    Near(rareSample.weight,independent->CameraContinuousMassNM(wo,600)+independent->CameraMirrorMassNM(wo,600),
        "rare branch carries unchanged complete mixture mass");

    auto eventLimited=Uniform(1e-12,.5);
    const auto e=HyLIoSLocalResponse::Create(eventLimited);
    Check(e->Covers(500,500),"optical coverage remains true when a requested RNG grid lacks support");
    Throws<std::domain_error>([&]{e->RequireSamplingSupport(wo,500,{32});},"rare event rejects insufficient RNG grid");
    e->RequireSamplingSupport(wo,500,{53});
    Near(e->EvaluateContinuousNM(wi,wo,500),1e-12/LocalResponsePi,"support rejection never changes physical coefficient");

    auto nodeLimited=rare; nodeLimited.mirror.assign(4,0);
    nodeLimited.continuous={4e-14/LocalResponsePi,0,0,.4/LocalResponsePi};
    const auto n=HyLIoSLocalResponse::Create(nodeLimited);
    Throws<std::domain_error>([&]{n->RequireSamplingSupport(wo,600,{32});},"rare spectral node rejects insufficient RNG grid");
    n->RequireSamplingSupport(wo,600,{53});

    auto cellLimited=nodeLimited; cellLimited.wavelengthNm={500};
    cellLimited.continuous={1e-12,.1}; cellLimited.mirror={0,0};
    const auto c=HyLIoSLocalResponse::Create(cellLimited);
    Throws<std::domain_error>([&]{c->RequireSamplingSupport(wo,500,{32});},"rare CDF cell rejects insufficient RNG grid");
    c->RequireSamplingSupport(wo,500,{53});
    Throws<std::domain_error>([&]{c->SampleCameraNM(wo,500,.5,.5,.5,.5,.5,{32});},"sampler defensively enforces all conditional intervals");
    Throws<std::invalid_argument>([&]{c->RequireSamplingSupport(wo,500,{0});},"zero grid precision rejected");
    Throws<std::invalid_argument>([&]{c->RequireSamplingSupport(wo,500,{54});},"unsupported discrete grid precision rejected");

    // Small nonredundant interpolation can introduce physically positive support
    // below the discrete proposal grid; evaluation is still defined there.
    auto changing=rare; changing.wavelengthNm={250,2500}; changing.continuous={.1,0,0,.1}; changing.mirror.assign(4,0);
    const auto support=HyLIoSLocalResponse::Create(changing);
    const double almost=std::nextafter(250.0,2500.0);
    Near(support->EvaluateContinuousNM({.8,.2},wo,almost),
        (0x1p-45/2250.0)*.1,"tiny newly appearing spectral response stays positive");
    Throws<std::domain_error>([&]{support->RequireSamplingSupport(wo,almost,{53});},"optical interval alone does not prove all interpolated sampling support");
}

void SubnormalScaleInvariantProposal()
{
    const double unit=std::numeric_limits<double>::denorm_min();
    const LocalResponsePoint wo{.5,.2};
    for (const double second:std::vector<double>{16,48}) {
        LocalResponseDefinition d;
        d.incident={{0,.25,1},{0,LocalResponseTwoPi}}; d.outgoing={{0,1},{0,LocalResponseTwoPi}};
        d.wavelengthNm={500}; d.continuous={16*unit,second*unit}; d.mirror={0,0};
        const auto r=HyLIoSLocalResponse::Create(d);
        r->RequireSamplingSupport(wo,500,{32});
        const double massUnits=LocalResponsePi*(16*.25+second*.75);
        const double probability0=16*.25/(16*.25+second*.75);
        constexpr int n=100000;
        int count0=0;
        double sourceEstimateUnits=0;
        for (int k=0;k<n;++k) {
            const auto s=r->SampleCameraNM(wo,500,.5,.5,Selector32((k+.5)/n),.5,.5,{32});
            const bool first=r->IncidentCell(s.incident)==0;
            if (first) ++count0;
            sourceEstimateUnits+=std::scalbn(s.weight,1074)*(first ? 2 : .25);
        }
        Check(std::abs(count0-n*probability0)<=1,"subnormal amplitude cannot bias independently known incoming-cell frequency");
        double integratedPdf=0;
        for (std::size_t i=0;i<2;++i) {
            const double t0=d.incident.tEdges[i],t1=d.incident.tEdges[i+1];
            const double mu=(std::sqrt(t0)+std::sqrt(t1))/2;
            const double coefficientUnits=i==0 ? 16 : second;
            const double pdf=r->CameraPdfNM({mu*mu,.3},wo,500);
            Near(pdf,coefficientUnits*mu/massUnits,"subnormal f/column cancellation retains normal PDF accuracy");
            integratedPdf+=pdf*(std::sqrt(t1)-std::sqrt(t0))*LocalResponseTwoPi;
        }
        Near(integratedPdf,1,"subnormal table PDF integrates independently to one");
        const auto s=r->SampleCameraNM(wo,500,.5,.5,.1,.5,.5,{32});
        const double coefficientUnits=r->IncidentCell(s.incident)==0 ? 16 : second;
        Near(coefficientUnits*std::sqrt(s.incident.t)/s.pdf,massUnits,"scaled f*cos/pdf agrees before final physical-output rounding");
        Check(std::abs(std::scalbn(s.weight,1074)-massUnits)<=.5,"physical throughput rounds only at final subnormal output");
        Near(r->CameraContinuousMassNM(wo,500),std::scalbn(massUnits,-1074),"physical column has one final subnormal rounding");
        const double exactSourceUnits=LocalResponsePi*(16*.25*2+second*.75*.25);
        Check(std::abs(sourceEstimateUnits/n-exactSourceUnits)<=1.01,"tiny source integral has only final output-ULP and selector quantization error");
    }

    LocalResponseDefinition mixture;
    mixture.incident={{0,.25,1},{0,LocalResponseTwoPi}}; mixture.outgoing={{0,1},{0,LocalResponseTwoPi}};
    mixture.wavelengthNm={500,700}; mixture.continuous={16*unit,16*unit,32*unit,32*unit};
    mixture.mirror={8*unit,8*unit,16*unit,16*unit};
    const auto r=HyLIoSLocalResponse::Create(mixture);
    const double zUnits=24*LocalResponsePi+12, mirrorProbability=12/zUnits;
    Near(r->CameraPdfNM({.125,.3},{.5,.2},600),24*std::sqrt(.125)/zUnits,
        "spectral continuous/delta mixture normalizes before subnormal physical rounding");
    constexpr int n=100000;
    int mirrorCount=0;
    for (int k=0;k<n;++k)
        if (r->SampleCameraNM({.5,.2},600,Selector32((k+.5)/n),.5,.5,.5,.5,{32}).event==LocalResponseSample::Event::PlanarMirror)
            ++mirrorCount;
    Check(std::abs(mirrorCount-n*mirrorProbability)<=1,"subnormal mirror event frequency uses unrounded normalized mass");
    mixture.continuous={16*unit,32*unit,48*unit,16*unit}; mixture.mirror.assign(4,0);
    const auto different=HyLIoSLocalResponse::Create(mixture);
    Near(different->CameraPdfNM({.125,.3},wo,600),32*std::sqrt(.125)/(26*LocalResponsePi),
        "nonproportional spectral nodes retain scaled physical interpolation in PDF");
    Check(different->IncidentCell(different->SampleCameraNM(wo,600,.5,.75,.145,.5,.5,{32}).incident)==1,
        "selected spectral node uses scaled rather than quantized subnormal CDF");

    // The column scale must follow F*A, not F alone. Here F is normal while
    // every nonzero incoming cell area is subnormal. The third cell is dark.
    LocalResponseDefinition angular;
    angular.incident={{0,16*unit,48*unit,1},{0,LocalResponseTwoPi}};
    angular.outgoing={{0,1},{0,LocalResponseTwoPi}}; angular.wavelengthNm={500};
    angular.continuous={.25,.25,0}; angular.mirror={0,0,0};
    const auto tinyAngle=HyLIoSLocalResponse::Create(angular);
    tinyAngle->RequireSamplingSupport(wo,500,{32});
    int firstCount=0;
    for (int k=0;k<n;++k)
        if (tinyAngle->IncidentCell(tinyAngle->SampleCameraNM(wo,500,.5,.5,Selector32((k+.5)/n),.5,.5,{32}).incident)==0) ++firstCount;
    Check(std::abs(firstCount-n/3.0)<=1,"subnormal projected areas preserve known one-third CDF probability");
    double tinyIntegratedPdf=0;
    for (std::size_t i=0;i<2;++i) {
        const double dt=angular.incident.tEdges[i+1]-angular.incident.tEdges[i];
        const double t=angular.incident.tEdges[i]+dt*.5, mu=std::sqrt(t);
        const double pdf=tinyAngle->CameraPdfNM({t,.3},wo,500);
        Near(pdf,std::scalbn(mu,1074)/(48*LocalResponsePi),"subnormal projected-area scale cancels before PDF return");
        // q/mu is cell-constant. Integrate its independently known dt*dphi/2
        // measure in scaled units; do not quantize a subnormal area in the oracle.
        tinyIntegratedPdf+=pdf*(std::scalbn(dt,1074)*LocalResponsePi)/std::scalbn(mu,1074);
    }
    Near(tinyIntegratedPdf,1,"tiny angular-support PDF integrates under independent projected-angle measure");
}

LocalResponseDefinition SplitRow(std::size_t cells, double f, double mirror)
{
    LocalResponseDefinition d;
    d.incident={{0,1},{0,LocalResponseTwoPi}}; d.outgoing.tEdges={0,1};
    for (std::size_t j=0;j<=cells;++j)
        d.outgoing.phiEdges.push_back(LocalResponseTwoPi*static_cast<double>(j)/static_cast<double>(cells));
    d.wavelengthNm={500}; d.continuous.assign(cells,f); d.mirror={mirror};
    return d;
}

void ExactForwardPassivityAndSplitInvariance()
{
    const LocalResponsePoint wi{.5,.3};
    for (std::size_t cells:std::vector<std::size_t>{1,7,256,65536}) {
        auto d=SplitRow(cells,1e-12/LocalResponsePi,1);
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"unit mirror cannot hide aggregate positive continuous mass");
        d=SplitRow(cells,std::numeric_limits<double>::denorm_min(),1);
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"unit mirror plus even one ultra-tiny represented contribution is overfull");

        for (int exponent:std::vector<int>{-40,-48,-55,-57}) {
            d=SplitRow(cells,std::scalbn(1.0,exponent),.5);
            const auto r=HyLIoSLocalResponse::Create(d);
            const double expected=.5+std::scalbn(LocalResponsePi,exponent);
            Check(r->ForwardRowMassNM(wi,500)==expected,"row report preserves aggregate continuous mass and rounds once independent of cell split");
        }

        // dt=1 and the phi widths telescope, so the represented areas sum
        // EXACTLY to the binary64 pi constant. Power-of-two F products are exact.
        constexpr double exactUnitMirror=0x1.b7812aeef4ba0p-3;
        d=SplitRow(cells,.25,exactUnitMirror);
        const auto exact=HyLIoSLocalResponse::Create(d);
        Check(exact->ForwardRowMassNM(wi,500)==1,"exact passive unit row is accepted for every angular split");
        d.mirror[0]=std::nextafter(exactUnitMirror,1.0);
        Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"one-ULP mirror excess is rejected even when total rounds to one");
        d.mirror[0]=std::nextafter(exactUnitMirror,0.0);
        Check(HyLIoSLocalResponse::Create(d)->ForwardRowMassNM(wi,500)<=1,"neighboring genuinely passive row remains accepted");
    }
    // Independent exact-rational fixture classification:
    // Fraction.from_float(f)*Fraction.from_float(math.pi)+Fraction.from_float(m)
    // is compared with Fraction(1). No rounded f*pi product defines acceptance.
    struct Fixture { double f,mirror; bool passive; double rounded; };
    const Fixture fixtures[]={
        {0x1.999999999999ap-4,0x1.5f26844b18790p-1,false,0},
        {0x1.999999999999ap-4,0x1.5f26844b1878fp-1,true,0x1.fffffffffffffp-1},
        {0x1.999999999999ap-3,0x1.7c9a112c61e40p-2,false,0},
        {0x1.999999999999ap-3,0x1.7c9a112c61e3fp-2,true,1},
        {0x1.3333333333333p-2,0x1.d738ce1496b00p-5,true,1},
        {0x1.45f306dc9c883p-2,0,false,0},
        {0x1.45f306dc9c882p-2,0,true,0x1.fffffffffffffp-1},
        {0x1p-55,0x1.fffffffffffffp-1,true,1},
        {0x1p-54,0x1.fffffffffffffp-1,false,0}
    };
    for (std::size_t cells:std::vector<std::size_t>{1,65536}) {
        for (const auto& fixture:fixtures) {
            const auto d=SplitRow(cells,fixture.f,fixture.mirror);
            if (fixture.passive)
                Check(HyLIoSLocalResponse::Create(d)->ForwardRowMassNM(wi,500)==fixture.rounded,
                    "exact-rational passive near-unit fixture accepted and rounded once to reference");
            else
                Throws<std::invalid_argument>([&]{HyLIoSLocalResponse::Create(d);},"exact-rational overfull near-unit fixture rejected");
        }
    }
    auto spectral=SplitRow(2,.5,0x1.b7812aeef4ba0p-3);
    spectral.wavelengthNm={500,700}; spectral.continuous={.5,0,0,.5};
    spectral.mirror={0x1.b7812aeef4ba0p-3,0x1.b7812aeef4ba0p-3};
    const auto interpolated=HyLIoSLocalResponse::Create(spectral);
    Check(interpolated->ForwardRowMassNM(wi,600)==1,"exact midpoint reconstruction remains passive");
    Check(interpolated->ForwardRowMassNM(wi,513)==1,
        "one represented minority and exact complement preserve a passive nonproportional spectral law");
    for (double nm:std::vector<double>{std::nextafter(500.0,700.0),513,600,687,std::nextafter(700.0,500.0)}) {
        Check(interpolated->ForwardRowMassNM(wi,nm)==1,"shared exact convex row stays passive at both endpoints and interior");
        const auto wo=Centre(spectral.outgoing,0);
        const double distance=nm<=600 ? nm-500 : 700-nm;
        const double w=distance/200;
        const double f=nm<=600 ? std::fma(-w,.5,.5) : .5*w;
        Near(interpolated->EvaluateContinuousNM(wi,wo,nm),f,"evaluation uses same represented minority/complement law");
        const double mass=LocalResponsePi*f;
        Near(interpolated->CameraPdfNM(wi,wo,nm),f*std::sqrt(wi.t)/(mass+spectral.mirror[0]),
            "PDF uses same exact convex law approximated at scalar output");
        if (nm==513 || nm==600 || nm==687) {
            const double event=mass<=spectral.mirror[0] ? 0 : .9;
            const auto sample=interpolated->SampleCameraNM(wo,nm,event,.5,.5,.5,.5,{32});
            Check(sample.event==LocalResponseSample::Event::Continuous,"shared convex-law continuation event selected");
            Near(sample.pdf,interpolated->CameraPdfNM(sample.incident,wo,nm),"sample reports shared convex-law aggregate PDF");
        }
    }
    struct RoundedFixture { double f,mirror,row; };
    // Same Fraction oracle, now exercising all product bits and widely separated
    // exponent ranges rather than just the comparison with unit passivity.
    const RoundedFixture rounding[]={
        {0x1.f9add3739635fp-4,0x1.e0652141ef0dcp-3,0x1.3eadae5a3225dp-1},
        {0x1.56e1fc2f8f359p-997,0x1.8f2b061aea072p-964,0x1.8f2b061d04a07p-964},
        {0x1.56e1fc2f8f359p-997,0x1.56e1fc2f8f359p-997,0x1.63052a209a896p-995},
        {0x1.87e92154ef7acp-665,0x1.50a6110d6a9b8p-698,0x1.33ce508989250p-663},
        {0x1.bff2ee48e0530p-333,0x1p-1,0x1p-1},
        {0x0.0000000000010p-1022,0,0x0.0000000000032p-1022},
        {0,0x1.f9add3739635fp-4,0x1.f9add3739635fp-4},
        {0x1.999999999999ap-3,0x1p-2,0x1.c1b2f769cf0e0p-1}
    };
    for (std::size_t cells:std::vector<std::size_t>{1,7,65536})
        for (const auto& fixture:rounding)
            Check(HyLIoSLocalResponse::Create(SplitRow(cells,fixture.f,fixture.mirror))->ForwardRowMassNM(wi,500)==fixture.row,
                "exact row rounding matches independent rational oracle across cell splits and exponent ranges");
}

void MirrorCellBoundaryAndCanonicalWrap()
{
    auto verify=[&](double edge, double phi, std::size_t expectedCell, const std::string& label) {
        for (const auto& coefficients:std::vector<std::vector<double>>{{1,0},{0,1},{.25,.75}}) {
            LocalResponseDefinition d;
            d.incident={{0,1},{0,edge,LocalResponseTwoPi}}; d.outgoing={{0,1},{0,LocalResponseTwoPi}};
            d.wavelengthNm={500}; d.continuous={0,0}; d.mirror=coefficients;
            const auto r=HyLIoSLocalResponse::Create(d);
            const LocalResponsePoint wo{.5,phi};
            const double mass=coefficients[expectedCell];
            Near(r->CameraMirrorMassNM(wo,500),mass,label+" inverse-map coefficient uses exact shifted cell");
            const auto sample=r->SampleCameraNM(wo,500,.5,.5,.5,.5,.5,{32});
            Near(sample.weight,mass,label+" sampling keeps expected mirror mass");
            if (mass>0) {
                Check(sample.event==LocalResponseSample::Event::PlanarMirror,label+" positive atom never terminates at a rounded seam");
                Check(r->IncidentCell(sample.incident)==expectedCell,label+" sampled mirror representative stays in true dyadic target cell");
                Check(sample.incident.t==wo.t,label+" mirror preserves actual polar coordinate");
            } else Check(sample.event==LocalResponseSample::Event::None,label+" true zero mirror coefficient emits no event");
        }
    };
    // Independent dyadic/Fraction fixtures: binary64 pi is exact here; add it
    // to lower-half phi or subtract it from upper-half phi in exact arithmetic,
    // then choose the greatest binary64 not above that exact result.
    struct Seam { double phi, floorPhi; std::size_t cell; };
    const Seam seams[]={
        {0,0x1.921fb54442d18p+1,1},
        {0x1.921fb54442d17p+1,0x1.921fb54442d17p+2,1},
        {0x1.921fb54442d18p+1,0,0},
        {0x1.921fb54442d19p+1,0x1p-51,0},
        {0x1.921fb54442d17p+2,0x1.921fb54442d16p+1,0},
        {0x0.0000000000001p-1022,0x1.921fb54442d18p+1,1}
    };
    for (const auto& seam:seams) {
        verify(LocalResponsePi,seam.phi,seam.cell,"pi/2pi seam");
        Check(HyLIoSLocalResponse::PlanarMirror({.5,seam.phi}).phi==seam.floorPhi,
            "mirror scalar equals independent exact-dyadic downward representative");
    }
    // For every binary64 edge E in [pi,2pi), E-pi is exact by Sterbenz.
    // Its predecessor, itself and successor map respectively below, at and
    // above E in exact arithmetic. These expected cells do not call the map.
    for (double edge:std::vector<double>{3.5,4,4.5,5.125,6}) {
        const double preimage=edge-LocalResponsePi;
        const double below=std::nextafter(preimage,0.0);
        verify(edge,below,0,"arbitrary upper-half target edge predecessor");
        verify(edge,preimage,1,"arbitrary upper-half exact target edge");
        verify(edge,std::nextafter(preimage,LocalResponsePi),1,"arbitrary upper-half target edge successor");
        Check(HyLIoSLocalResponse::PlanarMirror({.5,below}).phi==std::nextafter(edge,0.0),
            "rounded sum above exact arbitrary edge is moved to its downward representative");
        const double negative=edge-LocalResponseTwoPi;
        Check(HyLIoSLocalResponse::CanonicalPoint({.5,std::nextafter(negative,-LocalResponseTwoPi)}).phi==std::nextafter(edge,0.0),
            "negative canonical wrap also preserves arbitrary binary64 cell side");
    }
    for (double anchor:std::vector<double>{4,4.5,6}) {
        const double edge=anchor-LocalResponsePi;
        verify(edge,std::nextafter(anchor,LocalResponsePi),0,"upper-half subtract predecessor");
        verify(edge,anchor,1,"upper-half subtract exact edge");
        verify(edge,std::nextafter(anchor,LocalResponseTwoPi),1,"upper-half subtract successor");
    }
    const double unit=std::numeric_limits<double>::denorm_min();
    Check(HyLIoSLocalResponse::CanonicalPoint({.5,-unit}).phi==std::nextafter(LocalResponseTwoPi,0.0),
        "tiny negative canonical angle stays below two-pi instead of aliasing zero");
    for (double x:std::vector<double>{1,std::numeric_limits<double>::max()}) {
        const auto belowAxis=HyLIoSLocalResponse::FromDirection({x,-unit,x});
        Check(belowAxis.phi==std::nextafter(LocalResponseTwoPi,0.0),"Cartesian negative-y seam keeps the below-axis angular side");
        verify(LocalResponsePi,belowAxis.phi,0,"Cartesian inverse mirror below positive-x axis");
        const auto aboveNegativeAxis=HyLIoSLocalResponse::FromDirection({-x,unit,x});
        Check(aboveNegativeAxis.phi==std::nextafter(LocalResponsePi,0.0),"Cartesian positive-y side of negative-x axis stays below pi");
        verify(LocalResponsePi,aboveNegativeAxis.phi,1,"Cartesian inverse mirror above negative-x axis");
    }
    Check(HyLIoSLocalResponse::FromDirection({1,-0.0,1}).phi==0,"exact signed-zero axis remains canonical zero");
}
void SupportedProposalAnalytic()
{
    LocalResponseDefinition d;
    d.incident={{0,.2,1},{0,.7,LocalResponseTwoPi}};
    d.outgoing={{0,.1,1},{0,1.1,LocalResponseTwoPi}};
    d.wavelengthNm={500,700};
    const double masses[2][4][4]={
        {{.3,.1,0,.04},{.1,.5,.05,.02},{.6,0,.02,.02},{.2,.1,.3,.04}},
        {{.05,0,.2,.1},{.3,.05,0,.2},{0,.6,.1,.02},{.5,.02,.02,0}}};
    for (const auto& wavelength:masses)
        for (const auto& row:wavelength)
            for (std::size_t j=0;j<4;++j) d.continuous.push_back(row[j]/ReferenceArea(d.outgoing,j));
    d.mirror={.02,.03,.04,.05, .1,0,.15,.2};
    const auto core=HyLIoSLocalResponse::Create(d);
    const auto p=HyLIoSSupportedCameraProposal::Create(core,{32});
    for (double nm:{500.,std::nextafter(500.,700.),600.,std::nextafter(700.,500.),700.}) {
        const double dl=nm-500, dh=700-nm;
        const bool primaryLo=dl<=dh;
        const double minority=std::min(dl,dh)/(dl+dh);
        const auto physical=[&](double a,double b) { return primaryLo ? std::fma(minority,b-a,a) : std::fma(minority,a-b,b); };
        for (std::size_t j=0;j<4;++j) {
            const auto wo=Centre(d.outgoing,j);
            double expectation=0, reference=0, integratedPdf=0;
            for (std::size_t i=0;i<4;++i) {
                const double f0=masses[0][i][j]/ReferenceArea(d.outgoing,j);
                const double f1=masses[1][i][j]/ReferenceArea(d.outgoing,j);
                const double f=physical(f0,f1);
                const double area=ReferenceArea(d.incident,i);
                const auto sample=p->SampleCameraNM(wo,nm,0,i,0,0xffffffffU);
                Check(sample.event==LocalResponseSample::Event::Continuous,"supported proposal emits continuous zero-weight draws too");
                Check(core->IncidentCell(sample.incident)==i,"supported midpoint endpoints preserve selected cell");
                Near(sample.eventProbability,.5,"supported continuous branch has exact half probability");
                Near(sample.weight,8*area*f,"supported continuous weight uses interpolated nonsymmetric physical F");
                Near(sample.pdf,std::sqrt(sample.incident.t)/(8*area),"supported PDF uses actual uniform cell law");
                Near(p->CameraPdfNM(sample.incident,wo,nm),sample.pdf,"supported NEE PDF equals continuation subdensity");
                expectation+=sample.weight/8;
                reference+=area*f;
                const auto np=d.incident.phiEdges.size()-1, ti=i/np, pi=i%np;
                const double mu0=std::sqrt(d.incident.tEdges[ti]),mu1=std::sqrt(d.incident.tEdges[ti+1]);
                const double mu=(mu0+mu1)/2;
                integratedPdf+=p->CameraPdfNM({mu*mu,Centre(d.incident,i).phi},wo,nm)*(mu1-mu0)*
                    (d.incident.phiEdges[pi+1]-d.incident.phiEdges[pi]);
                // Independent nonconstant incident radiance per cell.
                expectation+=(i+1)*sample.weight/8;
                reference+=(i+1)*area*f;
            }
            Near(integratedPdf,.5,"supported continuous PDF integrates to exact branch probability");
            const auto inverse=HyLIoSLocalResponse::PlanarMirror(wo);
            const auto i=core->IncidentCell(inverse);
            const double atom=physical(d.mirror[i],d.mirror[4+i]);
            const auto sample=p->SampleCameraNM(wo,nm,1,0,0,0);
            Near(sample.weight,2*atom,"supported mirror weight uses inverse-map incident coefficient");
            Near(sample.pdf,0,"supported mirror event is delta");
            Near(sample.eventProbability,.5,"supported mirror branch always has exact half probability");
            Near(sample.incident.t,inverse.t,"supported mirror follows actual polar query");
            Near(sample.incident.phi,inverse.phi,"supported mirror follows actual azimuth query");
            Near(p->CameraPdfNM(inverse,wo,nm),std::sqrt(inverse.t)/(8*ReferenceArea(d.incident,i)),
                "supported NEE at mirror direction keeps continuous subdensity");
            Near(expectation+.5*sample.weight,reference+atom,"supported independent transpose expectation with variable incident radiance");
        }
    }
    Check(core->CameraContinuousMassNM(Centre(d.outgoing,0),500)>1,"supported proposal permits transpose columns above one");
}

void SupportedEndpointClosure()
{
    LocalResponseDefinition d;
    d.incident={{0,1},{0,1,LocalResponseTwoPi}};
    d.outgoing={{0,1},{0,LocalResponseTwoPi}};
    d.wavelengthNm={500,700}; d.continuous={0,.1,.2,0}; d.mirror={.4,0,0,.5};
    const auto core=HyLIoSLocalResponse::Create(d);
    const auto p=HyLIoSSupportedCameraProposal::Create(core,{32});
    const LocalResponsePoint wo{.3,2};
    const double near=std::nextafter(500.,700.);
#ifdef HYLIOS_PROVE_OLD_PROPOSAL
    try { core->RequireSamplingSupport(wo,near,{32}); }
    catch (const std::exception&) { Check(false,"supported-domain-closure old latent node fails nextafter wavelength"); return; }
#else
    const auto rare=p->SampleCameraNM(wo,near,0,0,0,0);
    Check(rare.weight>0,"supported-domain-closure tiny minority retains physical support without latent selection");
#endif
    for (double nm:{500.,near,600.,std::nextafter(700.,500.),700.})
        for (auto outgoing:std::vector<LocalResponsePoint>{{std::numeric_limits<double>::denorm_min(),0},{1,0},{.2,1},{.2,5}})
            for (unsigned event:{0U,1U}) for (std::size_t cell:{std::size_t{0},std::size_t{1}})
                for (std::uint64_t word:{std::uint64_t{0},std::uint64_t{0xffffffffU}}) {
                    const auto s=p->SampleCameraNM(outgoing,nm,event,cell,word,word);
                    Check(std::isfinite(s.weight) && s.weight>=0,"supported entire-domain probes produce finite physical weights");
                    Check(event==1 || (s.pdf>0 && std::isfinite(s.pdf)),"supported entire-domain probes retain positive continuous PDF");
                }
    for (const auto& values:std::vector<std::vector<double>>{{0,0},{0,.8},{.1,0}}) {
        auto uniform=d; uniform.wavelengthNm={500}; uniform.continuous.assign(2,values[0]); uniform.mirror.assign(2,values[1]);
        const auto proposal=HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(uniform),{32});
        for (unsigned event:{0U,1U}) {
            const auto s=proposal->SampleCameraNM(wo,500,event,0,0,0);
            Check(s.event==(event==0 ? LocalResponseSample::Event::Continuous : LocalResponseSample::Event::PlanarMirror),
                "supported zero physical events preserve constant proposal branches");
            if (values[event]==0) Near(s.weight,0,"supported zero physical contribution has legitimate zero weight");
        }
    }
    auto subnormal=d; subnormal.wavelengthNm={500};
    const double unit=std::numeric_limits<double>::denorm_min();
    subnormal.continuous.assign(2,16*unit);subnormal.mirror.assign(2,16*unit);
    const auto small=HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(subnormal),{32});
    Near(small->SampleCameraNM(wo,500,0,0,0,0).weight,32*unit,
        "supported scaled arithmetic retains representable constant subnormal continuous weights");
    Near(small->SampleCameraNM(wo,500,1,0,0,0).weight,32*unit,
        "supported scaled arithmetic retains representable constant subnormal mirror weights");
    Near(p->CameraPdfNM({std::numeric_limits<double>::denorm_min(),0},wo,500),
        std::sqrt(std::numeric_limits<double>::denorm_min())/2,"supported grazing NEE PDF remains representable");
    std::cout<<"ok: supported-domain-closure endpoint and zero-component controls\n";
}

void SupportedPreparationRefusalsAndIntegerGrid()
{
    auto d=Uniform(.1,.2); // Six incident cells: cannot have exact uniform power-of-two word PMF.
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(d),{32});},
        "supported nonpower2 grid explicitly refused before shading");
    d.incident={{0,.25,1},{0,2,LocalResponseTwoPi}};
    d.continuous.assign(16,.1/LocalResponsePi);d.mirror.assign(4,.2);
    const auto p=HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(d),{4});
    std::size_t counts[4]={};
    double polarMean[4]={},azimuthMean[4]={};
    for (unsigned event=0;event<2;++event)
        for (std::size_t cell=0;cell<4;++cell)
            for (std::uint64_t u=0;u<16;++u) for (std::uint64_t v=0;v<16;++v) {
                const auto s=p->SampleCameraNM({.4,2},500,event,cell,u,v);
                Near(s.eventProbability,.5,"supported integer event selection matches declared probability");
                if (event!=0) continue;
                const auto selected=p->Response().IncidentCell(s.incident);
                ++counts[selected];polarMean[selected]+=s.incident.t;azimuthMean[selected]+=s.incident.phi;
            }
    for (std::size_t i=0;i<4;++i) {
        Check(counts[i]==256,"supported exact integer enumeration gives every cell equal PMF");
        Near(polarMean[i]/256,Centre(d.incident,i).t,"supported full midpoint polar grid has analytic mean");
        Near(azimuthMean[i]/256,Centre(d.incident,i).phi,"supported full midpoint azimuth grid has analytic mean");
    }
    auto maximum=d;maximum.incident={{0,1},{0}};maximum.outgoing={{0,1},{0,LocalResponseTwoPi}};
    for (std::size_t i=1;i<=65536;++i) maximum.incident.phiEdges.push_back(LocalResponseTwoPi*static_cast<double>(i)/65536);
    maximum.continuous.assign(65536,0);maximum.mirror.assign(65536,0);
    const auto largest=HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(maximum),{8});
    const auto last=largest->SampleCameraNM({.4,2},500,0,65535,255,255);
    Check(largest->Response().IncidentCell(last.incident)==65535,
        "supported default maximum cell cardinality has finite integer and midpoint support");
    Throws<std::invalid_argument>([&]{p->SampleCameraNM({.4,2},500,2,0,0,0);},"supported invalid event word rejected");
    Throws<std::invalid_argument>([&]{p->SampleCameraNM({.4,2},500,0,4,0,0);},"supported invalid cell word rejected");
    Throws<std::invalid_argument>([&]{p->SampleCameraNM({.4,2},500,0,0,16,0);},"supported invalid midpoint word rejected");
    Throws<std::out_of_range>([&]{p->CameraPdfNM({.4,2},{.4,2},499);},"supported NEE enforces optical coverage");
    Throws<std::invalid_argument>([]{HyLIoSSupportedCameraProposal::Create({}, {32});},"supported null response rejected");
    auto tiny=d;tiny.incident={{0,1},{0,0x1p-1020,LocalResponseTwoPi}};
    tiny.continuous.assign(8,0);tiny.mirror.assign(2,0);
    // The tiny cell's midpoint grid resolves at one bit, but its inverse-area
    // PDF exceeds the conservative scalar envelope: numeric preflight must fail.
    tiny.incident.phiEdges[1]=0x1p-1024;
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(tiny),{1});},
        "supported tiny-area PDF overflow refused during preparation");
    tiny.incident.phiEdges[1]=4*std::numeric_limits<double>::denorm_min();
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(tiny),{32});},
        "supported unresolvable within-cell midpoint grid refused during preparation");
    auto vanishing=d;vanishing.wavelengthNm={500,700};vanishing.continuous.assign(32,0);
    vanishing.mirror.assign(8,0);vanishing.mirror[4]=std::numeric_limits<double>::denorm_min();
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(vanishing),{32});},
        "supported positive spectral weight underflow refused over entire interval");
    auto extreme=d;extreme.wavelengthNm={std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max()};
    extreme.continuous.assign(32,0);extreme.mirror.assign(8,0);
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(extreme),{32});},
        "supported unrepresentable spectral coordinate refused over entire interval");
    Throws<std::domain_error>([&]{HyLIoSSupportedCameraProposal::Create(HyLIoSLocalResponse::Create(d),{52});},
        "supported excessive midpoint cardinality refused by actual grid resolution");
}

void MultilinearAngularInterpolation()
{
    LocalResponseDefinition d;
    d.wavelengthNm = {500.0, 600.0};
    // 2x2 incident (t in {0,0.5,1}, phi in {0,pi,2pi}) x 2x1 outgoing (t in {0,0.5,1})
    d.incident = {{0.0, 0.5, 1.0}, {0.0, LocalResponsePi, LocalResponseTwoPi}};
    d.outgoing = {{0.0, 0.5, 1.0}, {0.0, LocalResponseTwoPi}};
    // 2 wavelengths * 4 incident * 2 outgoing = 16 entries (strictly passive: row sum <= 0.52)
    // At 500 nm: i=(it,ip), j=ot
    //   (0,0)->[0.02, 0.04], (0,1)->[0.06, 0.08], (1,0)->[0.10, 0.12], (1,1)->[0.14, 0.16]
    // At 600 nm: half of 500 nm
    d.continuous = {
        0.02, 0.04,  0.06, 0.08,  0.10, 0.12,  0.14, 0.16,
        0.01, 0.02,  0.03, 0.04,  0.05, 0.06,  0.07, 0.08
    };
    d.mirror = {
        0.01, 0.03, 0.05, 0.07,
        0.02, 0.06, 0.10, 0.14
    };
    const auto r = HyLIoSLocalResponse::Create(d);

    // 1. Exact agreement at cell centers (t = 0.25 or 0.75, phi = pi/2 or 3*pi/2)
    const LocalResponsePoint c00{0.25, 0.5 * LocalResponsePi};
    const LocalResponsePoint c11{0.75, 1.5 * LocalResponsePi};
    const LocalResponsePoint o0{0.25, 0.5};
    const LocalResponsePoint o1{0.75, 0.5};
    Near(r->EvaluateContinuousInterpolatedNM(c00, o0, 500.0), 0.02, "interp exact at cell center (0,0)->0");
    Near(r->EvaluateContinuousInterpolatedNM(c11, o1, 500.0), 0.16, "interp exact at cell center (1,1)->1");
    Near(r->EvaluateContinuousInterpolatedNM(c11, o1, 550.0), 0.12, "interp spectral midpoint at cell center");

    // 2. Exact midpoint between (t_in=0.25, t_out=0.25) and (t_in=0.75, t_out=0.75) at phi=pi/2
    //    Average of 0.02, 0.04, 0.10, 0.12 = 0.07
    Near(r->EvaluateContinuousInterpolatedNM({0.5, 0.5 * LocalResponsePi}, {0.5, 0.5}, 500.0),
         0.07, "bilinear midpoint in (t_in, t_out)");

    // 3. Periodic phi wraparound continuity across phi=0 / 2*pi (midway between 3*pi/2 and pi/2)
    //    At t_in=0.25, t_out=0.25, phi=0: average of (0,0)->0 (0.02) and (0,1)->0 (0.06) = 0.04
    const double atZero = r->EvaluateContinuousInterpolatedNM({0.25, 0.0}, o0, 500.0);
    const double justBelow = r->EvaluateContinuousInterpolatedNM({0.25, LocalResponseTwoPi - 1e-9}, o0, 500.0);
    const double justAbove = r->EvaluateContinuousInterpolatedNM({0.25, 1e-9}, o0, 500.0);
    Near(atZero, 0.04, "periodic phi wraparound midpoint at phi=0");
    Near(justBelow, atZero, "periodic phi continuous from below 2*pi", 1e-7);
    Near(justAbove, atZero, "periodic phi continuous from above 0", 1e-7);

    // 4. Mirror interpolation at cell center and midpoint
    //    PlanarMirror({0.5, 0.5*pi}) = {0.5, 1.5*pi}, midway between (0,1) [0.03] and (1,1) [0.07] -> 0.05
    Near(r->EvaluateMirrorInterpolatedNM({0.5, 0.5 * LocalResponsePi}, 500.0),
         0.05, "mirror bilinear midpoint in t");
    //    At 550 nm (spectral midpoint between 0.05 and 0.10 -> 0.075)
    Near(r->EvaluateMirrorInterpolatedNM({0.5, 0.5 * LocalResponsePi}, 550.0),
         0.075, "mirror bilinear + spectral midpoint");
}

} // namespace

int main()
{
    try {
        NonsymmetricUnequalArea(); LambertMirrorMixture(); SpectralSupportAndNodeMixture();
        EmptyAndBoundaries(); InvalidAndBudgets(); TinyPositiveAndFiniteSamplingSupport(); SubnormalScaleInvariantProposal();
        ExactForwardPassivityAndSplitInvariance();
        MirrorCellBoundaryAndCanonicalWrap();
        SupportedProposalAnalytic(); SupportedEndpointClosure(); SupportedPreparationRefusalsAndIntegerGrid();
        MultilinearAngularInterpolation();
    } catch (const std::exception& e) { ++failures; std::cerr<<"Unexpected exception: "<<e.what()<<'\n'; }
    if (failures) { std::cerr<<failures<<" failed checks\n"; return 1; }
    std::cout<<"HyLIoS local response analytic/control checks passed\n";
    return 0;
}
