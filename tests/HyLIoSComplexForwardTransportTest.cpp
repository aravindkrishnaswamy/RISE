#define main individual_test_main
#include "HyLIoSForwardTransportTest.cpp"
#undef main
#include <cfenv>
namespace {
constexpr auto Discard=ForwardPostScatterPolicy::DiscardAfterScatterV1;
constexpr auto Null=ForwardPlacementPolicy::NullCollisionPlacementV1;
std::array<ForwardLayer,6> ComplexLayers(std::size_t count,double host=0,double child=0,double index=1) {
    auto layers=Layers();for(auto& layer:layers) layer.thicknessMetres=10;
    layers[0].hostAbsorptionPerMetre=host;
    layers[0].organelle=ForwardOrganelle{ForwardOrganization::ExplicitComplexPackV1,.2,.25,.25,child,index,1.25,count};
    return layers;
}
ForwardState EnvelopeEntry(const HyLIoSForwardTransport& k) {
    auto s=Enter(k);Sequence travel{-std::expm1(-.12*3)};k.Advance(s,travel,1);
    Sequence projected{0,0};k.Advance(s,projected,1);
    Check(s.Counters().acceptedParticles==1&&s.Counters().acceptedComplexes==1,"one accepted outer envelope counted once");
    Near(s.Position().z,3,1e-15,"outer geometric rate is 3*f/(4*R), distance ends at envelope entry");
    return s;
}
void BuildCentred(const HyLIoSForwardTransport& k,ForwardState& s) {
    Sequence proposal{.5,.5,.5,.5,0};k.Advance(s,proposal,1);
    Check(s.Phase()==ForwardPhase::ComplexHostFlight&&s.Counters().complexLayoutProposals==1,"one five-draw proposal builds centred child");
}
void FixedLayoutBeer() {
    for(std::size_t count:{0U,1U,2U}) {
        HyLIoSForwardTransport k(ComplexLayers(count,.2,.7),1400,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
        auto s=EnvelopeEntry(k);const auto before=s.Counters().pathLengthMetres;
        if(count==1) BuildCentred(k,s);
        if(count==2) {
            Sequence first{.5,.5,.25,.5,0},second{.5,.5,.75,.5,0};
            k.Advance(s,first,1);k.Advance(s,second,1);
        }
        Check(s.Counters().pathLengthMetres==before&&s.Counters().hostPathLengthMetres[0]==before,"layout construction never advances travel or absorption clocks");
        Random rng(1);for(int step=0;step<20&&s.Counters().complexExits==0;++step) k.Advance(s,rng,1);
        Check(s.Counters().complexExits==1&&(s.Phase()==ForwardPhase::ForwardPerturbation||(count==0&&s.Phase()==ForwardPhase::BulkFlight)),"known layout reaches envelope exit before final angular event");
        const double childLength=.5*static_cast<double>(count),hostLength=3+2.5-childLength;
        Near(s.Counters().particlePathLengthMetres[0],childLength,3e-15,"fixed layout exact sum of true child chords");
        Near(s.Counters().hostPathLengthMetres[0],hostLength,3e-15,"fixed layout exact sum of outer and internal host gaps");
        Near(s.Weight(),std::exp(-.2*hostLength-.7*childLength),3e-15,"fixed layout independent host/child Beer oracle");
        Check(s.Counters().complexChildEntries==count&&s.Counters().complexChildExits==count,"empty/one/two layout child entries follow real geometry");
        Check(s.Counters().perturbationProposals==0,"child transmission never applies final exponential law");
        Sequence delta{};if(count!=0) k.Advance(s,delta,1);
        Check(delta.index==0&&s.Phase()==ForwardPhase::BulkFlight,"envelope 1400nm final delta consumes no angular RNG");
    }
}
void TouchingAndRevisit() {
    auto layers=ComplexLayers(1);layers[0].organelle->semiMinorMetres=1.25;layers[0].organelle->semiMajorMetres=1.25;
    HyLIoSForwardTransport coincident(layers,550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto same=EnvelopeEntry(coincident);BuildCentred(coincident,same);
    Sequence empty{};coincident.Advance(same,empty,1);
    Check(same.Phase()==ForwardPhase::ParticleChord&&same.Counters().complexChildEntries==1,"child touching envelope owns zero-distance entering boundary");
    Sequence transmit{.5};coincident.Advance(same,transmit,1);coincident.Advance(same,empty,1);
    Check(same.Counters().complexChildExits==1&&same.Counters().complexExits==1,"owned child exit precedes coincident envelope exit");
    Near(same.Counters().particlePathLengthMetres[0],2.5,2e-15,"coincident child/envelope has complete real diameter");
    Sequence kick{0,0,0,0};coincident.Advance(same,kick,1);
    Check(same.Counters().perturbationProposals==1&&kick.index==4,"nonempty envelope applies final angular law exactly once");

    HyLIoSForwardTransport touching(ComplexLayers(2),1400,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto contact=EnvelopeEntry(touching);Sequence lower{.5,.5,.375,.5,0},upper{.5,.5,.625,.5,0};
    touching.Advance(contact,lower,1);touching.Advance(contact,upper,1);Random r(52);
    while(contact.Counters().complexExits==0&&contact.Termination()==ForwardTermination::None) touching.Advance(contact,r,1);
    Check(contact.Counters().complexChildEntries==2&&contact.Counters().complexChildExits==2,"touching other child remains a legitimate zero-gap entry");
    Near(contact.Counters().particlePathLengthMetres[0],1,2e-15,"touching pair retains both full chords");

    HyLIoSForwardTransport tangent(ComplexLayers(1),1400,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto graze=EnvelopeEntry(tangent);Sequence offset{.625,.5,.5,.5,0};tangent.Advance(graze,offset,1);tangent.Advance(graze,empty,1);
    Check(graze.Counters().complexChildEntries==0&&graze.Counters().complexExits==1,"exact sphere tangency has zero chord and never enters child");
    Near(graze.Counters().hostPathLengthMetres[0],5.5,2e-15,"exact child tangency leaves entire envelope diameter to host");

    HyLIoSForwardTransport revisit(ComplexLayers(2,0,0,2),550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto s=EnvelopeEntry(revisit);Sequence first{.5,.5,.25,.5,0},second{.5,.5,.75,.5,0};
    revisit.Advance(s,first,1);revisit.Advance(s,second,1);
    revisit.Advance(s,empty,1);Sequence firstExit{.9};revisit.Advance(s,firstExit,1);
    revisit.Advance(s,empty,1);Sequence reflect{0};revisit.Advance(s,reflect,1);
    Sequence inward{0,0};revisit.Advance(s,inward,1);Sequence secondExit{.9};revisit.Advance(s,secondExit,1);
    revisit.Advance(s,empty,1);Sequence revisitExit{.9};revisit.Advance(s,revisitExit,1);revisit.Advance(s,empty,1);
    Check(s.Termination()==ForwardTermination::None&&s.Counters().complexExits==1&&s.Counters().complexChildEntries==3,"two actual children permit three entries without optical count cap");
    Check(s.Counters().complexChildReflections==1&&s.Counters().complexChildExits==3,"reflected chord remains inside same child; revisit is a new entry");
    Near(s.Counters().particlePathLengthMetres[0],2,3e-15,"two-child revisit independently sums four real diameter chords");
    Near(s.Counters().hostPathLengthMetres[0],5,3e-15,"two-child revisit independently sums outer flight and four host gaps");
    Check(s.Counters().perturbationProposals==1,"child reflection kick occurs, per-child final angular kicks do not");
}
void CoincidentEnvelopeOwnership() {
    constexpr double radius=1e-4,fraction=.05,rate=3*fraction/(4*radius);
    Random proposals(0x434f494e43494445ULL);
    for(std::size_t count:{0U,1U}) for(int trial=0;trial<160;++trial) {
        auto layers=Layers();for(auto& layer:layers) layer.thicknessMetres=20*radius;
        layers[0].organelle=ForwardOrganelle{ForwardOrganization::ExplicitComplexPackV1,fraction,radius,radius,0,1,radius,count};
        HyLIoSForwardTransport k(layers,1400,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
        SlabVector incoming{2*proposals.Get1D()-1,2*proposals.Get1D()-1,.1+.9*proposals.Get1D()};
        auto state=k.Begin({0,0,0},incoming);const auto boundary=state.PendingBoundary();
        k.ResumeBoundary(state,boundary.token,{BoundaryAction::Transmit,SlabSide::Interior,incoming,1});
        const double entryDistance=5*radius/state.Direction().z;
        Sequence travel{-std::expm1(-rate*entryDistance)};k.Advance(state,travel,1);
        const double radial=.01+.98*proposals.Get1D(),azimuth=proposals.Get1D();
        Sequence projected{radial,azimuth};k.Advance(state,projected,1);
        Check(state.Counters().acceptedComplexes==1,"coincident envelope fixture accepts off-centre sampled entry");
        if(count!=0) {Sequence child{.1,.2,.3,.4,.5};k.Advance(state,child,1);}
        Random optics(static_cast<std::uint64_t>(trial)+31);
        for(int work=0;work<8&&state.Termination()==ForwardTermination::None&&state.Counters().complexExits==0;++work) k.Advance(state,optics,1);
        Check(state.Termination()==ForwardTermination::None&&state.Counters().complexExits==1,
            "owned off-centre envelope entry/full-child exit completes without uncertain surface reintersection");
        Check(state.Counters().complexChildEntries==count&&state.Counters().complexChildExits==count&&state.Counters().particleExits==count,
            "coincident child owns exactly one complete entry and exit, empty envelope owns none");
        const double chord=2*radius*std::sqrt(1-radial);
        Near(state.Counters().hostPathLengthMetres[0],entryDistance+(count==0 ? chord:0),2e-17,
            "coincident envelope ownership keeps exact host distance without microscopic extra gap");
        Near(state.Counters().particlePathLengthMetres[0],count==0 ? 0:chord,2e-18,
            "coincident full child owns the independently sampled sphere chord");
        Check(state.Weight()==1&&Absorbed(state)==0,"coincident ownership preserves unit live power and no absorption");
        Sequence noDraw{};if(count!=0) k.Advance(state,noDraw,1);
        Check(state.Phase()==ForwardPhase::BulkFlight&&state.Counters().perturbationProposals==0,
            "coincident envelope keeps the single 1400nm final delta and resumes host");
    }
}
void LayoutBudgets() {
    HyLIoSForwardTransport k(ComplexLayers(2,.2,.7),550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto s=EnvelopeEntry(k);const double path=s.Counters().pathLengthMetres,weight=s.Weight(),absorbed=Absorbed(s);
    Sequence first{.5,.5,.5,.5,0};k.Advance(s,first,1);auto snapshot=s;
    for(int proposal=0;proposal<256;++proposal) {Sequence same{.5,.5,.5,.5,0};k.Advance(s,same,1);}
    Sequence distinct{.5,.5,.875,.5,0};k.Advance(snapshot,distinct,1);
    Check(snapshot.Phase()==ForwardPhase::ComplexHostFlight&&snapshot.Counters().complexLayoutProposals==2&&snapshot.Counters().complexLayoutRestarts==0,"copied resumable state owns its partial layout independently across another copy's restart");
    Check(s.Counters().complexLayoutProposals==257&&s.Counters().complexLayoutRestarts==1&&s.Counters().complexLayoutOverlapRejections==256&&s.Counters().complexLayoutPairChecks==256,"construction restart preserves exact proposal/rejection/pair-check counters");
    Check(s.Counters().pathLengthMetres==path&&s.Weight()==weight&&Absorbed(s)==absorbed,"257 construction proposals consume no travel or physical power");
    auto cancelled=s;k.Cancel(cancelled);k.StopForBudget(s);
    Check(cancelled.Termination()==ForwardTermination::Cancelled&&s.Termination()==ForwardTermination::BudgetExceeded&&cancelled.Weight()==weight&&s.Weight()==weight,"complex cancellation/budget retain live unresolved power");
    Sequence noDraw{};k.Advance(s,noDraw,100);k.Advance(cancelled,noDraw,100);
    Check(noDraw.index==0,"terminal construction cancellation/budget consumes no RNG");
    auto invalid=EnvelopeEntry(k);Sequence bad{.5,.5,1,.5,0};k.Advance(invalid,bad,1);
    Check(invalid.Termination()==ForwardTermination::InvalidSample&&invalid.Weight()==weight,"invalid layout sample never becomes absorption");
}
void EmptyVisibleSlab() {
    auto layers=Layers(80,1.3);layers[0].organelle=ForwardOrganelle{ForwardOrganization::ExplicitComplexPackV1,.002,.00002,.00005,999999,7,.0001,0};
    HyLIoSForwardTransport k(layers,550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto exact=Enter(k);Sequence travel{-std::expm1(-15*.0002)};k.Advance(exact,travel,1);Sequence projection{0,0};k.Advance(exact,projection,1);
    Sequence noDraw{};k.Advance(exact,noDraw,1);
    Check(exact.Counters().complexExits==1&&exact.Phase()==ForwardPhase::BulkFlight&&exact.Direction().z==1&&exact.Counters().perturbationProposals==0,"visible empty complex has no final angular event or child optics");
    Check(exact.Counters().complexChildEntries==0&&Sum(exact.ParticleAbsorbedByLayer())==0,"visible empty complex never absorbs as child material");
    HyLIoSForwardTransport analog(layers,550,1,0,AbsorptionEstimator::Analog,0,Null,Discard);
    HyLIoSSlabTransport slab(Slabs(80,1.3),550,1,AbsorptionEstimator::Weighted,0);
    Moments a,w,control;std::uint64_t envelopes=0;
    for(int seed=1;seed<=3000;++seed) {
        Random ra(static_cast<std::uint64_t>(seed)*17),rw(static_cast<std::uint64_t>(seed)*23),rs(static_cast<std::uint64_t>(seed)*31);
        const auto sa=Run(analog,ra),sw=Run(k,rw);const auto sc=RunSlab(slab,rs);
        Check(Returned(sa)||sa.Termination()==ForwardTermination::Absorbed,"empty complex analog history completes");
        Check(Returned(sw)||sw.Termination()==ForwardTermination::Absorbed,"empty complex weighted history completes");
        a.Add(Returned(sa)?sa.Weight():0);w.Add(Returned(sw)?sw.Weight():0);control.Add(sc.Weight());
        envelopes+=sa.Counters().acceptedComplexes+sw.Counters().acceptedComplexes;
        Check(sa.Counters().perturbationProposals==0&&sw.Counters().perturbationProposals==0,"empty complex never introduces angular draws at visible wavelength");
    }
    Check(envelopes>0,"visible empty complex slab control actually exercises envelope events");
    Check(std::abs(a.Mean()-control.Mean())<=5*std::hypot(a.SE(),control.SE()),"visible empty complex analog matches independent slab mean");
    Check(std::abs(w.Mean()-control.Mean())<=5*std::hypot(w.SE(),control.SE()),"visible empty complex weighted matches independent slab mean");
    std::cout<<"empty visible n=3000 envelopes="<<envelopes<<" analog="<<a.Mean()<<" weighted="<<w.Mean()<<" slab="<<control.Mean()<<'\n';
}

void NearestSphereReference() {
    for(int step=0;step<=60;++step) {

        const double dx=static_cast<double>(step)/80;const SlabVector d{dx,0,std::sqrt(1-dx*dx)};
        HyLIoSForwardTransport k(ComplexLayers(2),1400,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
        auto state=k.Begin({0,0,0},d);const auto e=state.PendingBoundary();k.ResumeBoundary(state,e.token,{BoundaryAction::Transmit,SlabSide::Interior,d,1});
        Sequence travel{-std::expm1(-.12*3)},projection{0,0},first{.5,.5,.25,.5,0},second{.5,.5,.75,.5,0};
        k.Advance(state,travel,1);k.Advance(state,projection,1);k.Advance(state,first,1);k.Advance(state,second,1);
        // Independent naive sphere quadratic, used only at these ordinary
        // magnitudes and away from tangency. Production scans general prolates.
        const auto dir=state.Direction();
        const auto projected=SampleProlateProjectedHit({1.25,1.25,{0,0,1}},{dir.x,dir.y,dir.z},0,0);
        long double expected=2.5L;bool child=false;
        for(double centre:{-.5,.5}) {
            const long double x=projected.chord.entryPointMetres.x,z=projected.chord.entryPointMetres.z-centre;
            const long double A=static_cast<long double>(dir.x)*dir.x+static_cast<long double>(dir.z)*dir.z;
            const long double b=x*dir.x+z*dir.z,c=x*x+z*z-.25L*.25L,disc=b*b-A*c;
            if(disc<=0) continue;
            const long double entry=(-b-std::sqrt(disc))/std::sqrt(A);
            if(entry>=0&&entry<expected) {expected=entry;child=true;}
        }
        // Decimal(100), exact binary64-input polynomial oracle for the
        // deliberately retained near-tangent step40. On arm64 long double is
        // binary64, so the ordinary reference above cannot resolve its sign.
        if(step==40) {
            Check(dir.x==.50000000000000011&&dir.z==.86602540378443882&&projected.chord.entryPointMetres.x==-.62499999999999989&&projected.chord.entryPointMetres.z==-1.082531754730548,"near-tangent high-precision oracle input tuple is unchanged");
            expected=1.6830126965630604843973119973056605864L;child=true;
        }
        Sequence noDraw{};k.Advance(state,noDraw,1);
        Near(state.Counters().hostPathLengthMetres[0],3+static_cast<double>(expected),8e-15,"nearest general-child hit matches independent sphere quadratic reference");
        Check(state.Counters().complexChildEntries==(child ? 1U:0U),"nearest sphere reference agrees on child event versus envelope exit");
    }
}
std::array<ForwardLayer,6> S3Layers() {
    constexpr double radius=.41e-6;
    auto layers=Layers();for(auto& layer:layers) layer.thicknessMetres=20*radius;
    layers[0].hostAbsorptionPerMetre=200000;
    layers[0].organelle=ForwardOrganelle{ForwardOrganization::ExplicitComplexPackV1,.2,.085e-6,.205e-6,1000000,1.7,radius,5};
    return layers;
}
ForwardState S3Envelope(const HyLIoSForwardTransport& k,AbsorptionEstimator estimator) {
    constexpr double radius=.41e-6,host=200000,entry=3*radius;
    auto state=k.Begin({0,0,0},{0,0,1});const auto e=state.PendingBoundary();
    // Normalize the state at the conditioned envelope entry to live weight1.
    // The forced analog clock merely conditions survival to this prescribed
    // entry. Weighted pre-entry Beer is cancelled by a public boundary proposal;
    // all statistics subtract pre-entry host absorption and concern this single
    // accepted complex, not an unconditioned specimen reflectance estimate.
    k.ResumeBoundary(state,e.token,{BoundaryAction::Transmit,SlabSide::Interior,{0,0,1},estimator==AbsorptionEstimator::Weighted ? std::exp(host*entry):1});
    Sequence analog{-std::expm1(-host*10*radius),-std::expm1(-.45)},weighted{-std::expm1(-.45)};
    if(estimator==AbsorptionEstimator::Analog) k.Advance(state,analog,1);else k.Advance(state,weighted,1);
    Sequence projection{0,0};k.Advance(state,projection,1);
    Near(state.Weight(),1,3e-16,"conditioned S3 envelope begins unit live power");
    Check(state.Counters().acceptedComplexes==1,"conditioned S3 control accepts its prescribed envelope");
    return state;
}
void S3EstimatorAndQuantum() {
    constexpr int count=300;
    Moments escape[3],host[3],child[3];std::uint64_t proposals=0,pairs=0,restarts=0,entries=0,rr=0;
    for(int estimator=0;estimator<3;++estimator) {
        const auto mode=estimator==0 ? AbsorptionEstimator::Analog:AbsorptionEstimator::Weighted;
        HyLIoSForwardTransport k(S3Layers(),550,1,0,mode,estimator==2 ? .8:0,Null,Discard);
        for(int seed=1;seed<=count;++seed) {
            auto state=S3Envelope(k,mode);const double before=Sum(state.HostAbsorbedByLayer());Random rng(static_cast<std::uint64_t>(seed)*19+12345+static_cast<std::uint64_t>(estimator)*1000000);
            while(state.Termination()==ForwardTermination::None&&state.Counters().complexExits==0&&state.Counters().workUnits<30000) k.Advance(state,rng,1);
            Check(state.Counters().complexExits==1||state.Termination()==ForwardTermination::Absorbed||state.Termination()==ForwardTermination::RouletteKilled,"default S3 N5 conditional history finishes without geometry or packing failure");
            if(state.Counters().complexExits==1) while(state.Termination()==ForwardTermination::None&&state.Phase()==ForwardPhase::ForwardPerturbation) k.Advance(state,rng,1);
            escape[estimator].Add(state.Counters().complexExits==1?state.Weight():0);
            host[estimator].Add(Sum(state.HostAbsorbedByLayer())-before);child[estimator].Add(Sum(state.ParticleAbsorbedByLayer()));
            proposals+=state.Counters().complexLayoutProposals;pairs+=state.Counters().complexLayoutPairChecks;restarts+=state.Counters().complexLayoutRestarts;entries+=state.Counters().complexChildEntries;rr+=state.Counters().rouletteTrials;
        }
    }
    for(int mode=1;mode<3;++mode) {
        for(const auto* moments:{escape,host,child}) Check(std::abs(moments[0].Mean()-moments[mode].Mean())<=5*std::hypot(moments[0].SE(),moments[mode].SE()),"S3 N5 analog/weighted/roulette agree on escaped, host and child power within combined SE");
    }
    Check(entries>0&&rr>0,"S3 N5 estimators exercise real child entries and compensated roulette");
    std::cout<<"S3 conditional n="<<count<<" escape="<<escape[0].Mean()<<','<<escape[1].Mean()<<','<<escape[2].Mean()<<" host="<<host[0].Mean()<<','<<host[1].Mean()<<','<<host[2].Mean()<<" child="<<child[0].Mean()<<','<<child[1].Mean()<<','<<child[2].Mean()<<" proposals="<<proposals<<" pairs="<<pairs<<" restarts="<<restarts<<" entries="<<entries<<" roulette="<<rr<<'\n';
    HyLIoSForwardTransport k(S3Layers(),550,1,0,AbsorptionEstimator::Weighted,.8,Null,Discard);
    for(int seed=1;seed<=30;++seed) {
        Random one(static_cast<std::uint64_t>(seed)),many(static_cast<std::uint64_t>(seed));
        const auto a=Bounded(k,one,1),b=Bounded(k,many,64);
        Check(a.Termination()==b.Termination()&&a.Weight()==b.Weight()&&a.Position().x==b.Position().x&&a.Position().z==b.Position().z&&a.Position().y==b.Position().y&&a.Phase()==b.Phase()&&a.FailureDetail()==b.FailureDetail()&&a.Direction().y==b.Direction().y&&a.Direction().x==b.Direction().x&&a.Direction().z==b.Direction().z&&one.draws==many.draws&&a.Counters().workUnits==b.Counters().workUnits&&a.Counters().complexLayoutProposals==b.Counters().complexLayoutProposals&&a.Counters().complexLayoutRestarts==b.Counters().complexLayoutRestarts&&a.Counters().complexChildEntries==b.Counters().complexChildEntries&&a.Counters().complexLayoutPairChecks==b.Counters().complexLayoutPairChecks&&a.Counters().complexLayoutContainmentRejections==b.Counters().complexLayoutContainmentRejections&&a.Counters().complexLayoutOverlapRejections==b.Counters().complexLayoutOverlapRejections&&a.Counters().complexChildExits==b.Counters().complexChildExits&&a.Counters().complexChildReflections==b.Counters().complexChildReflections&&a.Counters().acceptedComplexes==b.Counters().acceptedComplexes&&a.Counters().complexExits==b.Counters().complexExits&&a.Counters().complexHostSegments==b.Counters().complexHostSegments&&a.HostAbsorbedByLayer()==b.HostAbsorbedByLayer()&&a.ParticleAbsorbedByLayer()==b.ParticleAbsorbedByLayer(),"complex construction/optics quantum partition preserves RNG, counters and physical accounting");
    }
    auto pd=S3Layers();pd[4].organelle=pd[0].organelle;
    Reject([&]{HyLIoSForwardTransport unsupported(pd,550,1,1,AbsorptionEstimator::Analog,0,Null,Discard);},"PD complexes with nonzero fiber clock explicitly unsupported");
}

void NumericalControls() {
    HyLIoSForwardTransport k(ComplexLayers(1),550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto build=EnvelopeEntry(k);const double initial=build.Weight();Sequence unsupported{.5,.5,.5,.5,0x1p-200};k.Advance(build,unsupported,1);
    Check(build.Termination()==ForwardTermination::NumericalFailure&&build.FailureDetail()==ForwardFailureDetail::OverlapUnsupportedRange&&build.Weight()==initial&&build.Counters().nullPlacements==0,"unsupported layout predicate propagates unresolved numerical failure, never null/retry/absorb");
    auto ray=EnvelopeEntry(k);BuildCentred(k,ray);const double path=ray.Counters().pathLengthMetres;
    const int original=std::fegetround();Check(std::fesetround(FE_DOWNWARD)==0,"numerical fault fixture can select unsupported FP environment");
    Sequence noDraw{};k.Advance(ray,noDraw,1);const int restore=std::fesetround(original);
    Check(restore==0,"numerical fault fixture restores FP environment");
    Check(ray.Termination()==ForwardTermination::NumericalFailure&&ray.FailureDetail()==ForwardFailureDetail::ArithmeticOrGeometry&&ray.Weight()==1&&ray.Counters().pathLengthMetres==path&&ray.Counters().nullPlacements==0,"general intersector exception propagates before host travel without null/retry/absorb");
}

void ComplexDiscardOwnership() {
    HyLIoSForwardTransport k(ComplexLayers(1,.2,.7),550,1,0,AbsorptionEstimator::Weighted,0,Null,Discard);
    auto s=EnvelopeEntry(k);BuildCentred(k,s);Sequence noDraw{};k.Advance(s,noDraw,1);Sequence exit{.5};k.Advance(s,exit,1);k.Advance(s,noDraw,1);
    Check(s.Counters().complexExits==1&&s.Counters().complexChildExits==1,"discard fixture leaves one real child and its envelope");
    const double weight=s.Weight(),hostBefore=s.HostAbsorbedByLayer()[0],childBefore=s.ParticleAbsorbedByLayer()[0],hostPath=s.Counters().hostPathLengthMetres[0],childPath=s.Counters().particlePathLengthMetres[0];
    Sequence inward{.99995,.99995,0,0};k.Advance(s,inward,1);
    Check(s.Phase()==ForwardPhase::BulkFlight&&s.Termination()==ForwardTermination::None&&s.Counters().inwardPostExitEvents==1&&s.Counters().perturbationProposals==1,"one final inward complex kick completes under discard");
    constexpr double gap=.001;Sequence travel{-std::expm1(-.12*gap)};k.Advance(s,travel,1);
    Near(s.Counters().hostPathLengthMetres[0],hostPath+gap,4e-15,"discarded complex interior owns exact host distance");
    Near(s.Weight(),weight*std::exp(-.2*gap),3e-15,"discarded complex interior owns host Beer absorption");
    Near(s.HostAbsorbedByLayer()[0],hostBefore+weight*(-std::expm1(-.2*gap)),3e-15,"discarded complex host loss tallied exactly once");
    Check(s.ParticleAbsorbedByLayer()[0]==childBefore&&s.Counters().particlePathLengthMetres[0]==childPath,"discarded layout adds no melanosome chord or absorption");
    // New central envelope overlaps the old radius1.25 envelope, while its
    // centre is farther than 1.25+.25 from the old centred radius.25 child.
    // Therefore exclusion proves retention of the outer envelope, not a child.
    const double centreDistance=std::sqrt(1.25*1.25+(1.25+gap)*(1.25+gap)+2*1.25*(1.25+gap)*s.Direction().z);
    Check(centreDistance>1.5&&centreDistance<2.5,"discard predecessor fixture geometrically distinguishes envelope from last child");
    Sequence newEnvelope{0,0};k.Advance(s,newEnvelope,1);
    Check(s.Counters().overlapRejections==1&&s.Counters().nullPlacements==1&&s.Counters().acceptedComplexes==1&&s.Counters().acceptedParticles==1,"discard retains envelope predecessor after removing all active children");
}

}
int main(){try {FixedLayoutBeer();TouchingAndRevisit();CoincidentEnvelopeOwnership();LayoutBudgets();EmptyVisibleSlab();NearestSphereReference();S3EstimatorAndQuantum();NumericalControls();ComplexDiscardOwnership();std::cout<<"PASS "<<checks<<" complex forward checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
