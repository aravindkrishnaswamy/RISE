#include "pch.h"
#include "SimulationSolver.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool DigestValid(const std::string& digest)
{
	return digest.size()==64 && std::all_of(digest.begin(),digest.end(),[](char c){
		return (c>='0' && c<='9') || (c>='a' && c<='f');
	});
}

struct Registry {
	std::mutex mutex;
	std::vector<std::pair<std::string,SolverFactory>> factories;
};
Registry& TheRegistry()
{
	static Registry registry;
	return registry;
}
} // namespace

const char* BackendStatusName(BackendStatus status)
{
	switch (status) {
	case BackendStatus::Supported: return "supported";
	case BackendStatus::Experimental: return "experimental";
	case BackendStatus::Unsupported: break;
	}
	return "unsupported";
}

std::string EvidenceDigest(const std::string& id,const V& configuration,double error,double tolerance,bool passed)
{
	Bytes encoded;
	if (!std::isfinite(error) || !std::isfinite(tolerance) ||
		!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.evidence.v1"),V::String(id),configuration,
			V::Float(error),V::Float(tolerance),V::Bool(passed)}),encoded,nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

std::string ScopedEvidenceDigest(const std::string& caseDigest,const EvidenceConditions& conditions,
	const std::string& scope)
{
	if (conditions.empty() && scope.empty()) return caseDigest;
	V::Members map;
	for (const auto& c : conditions) {
		V::Values values;
		for (const auto& v : c.second) values.push_back(V::String(v));
		map.emplace_back(c.first,V::ArrayValue(values));
	}
	Bytes encoded;
	if (!DigestValid(caseDigest) || !ValidEvidenceConditions(conditions) ||
		!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.evidence.scope.v1"),V::String(caseDigest),
			V::MapValue(map),V::String(scope)}),encoded,nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

std::string EffectiveFidelity(const Evidence& evidence)
{
	if (evidence.fidelity!="predictive" || evidence.cases.empty() || evidence.scope.empty() ||
		!RISECBOR64::IsUTF8NFC(evidence.scope) || !ValidEvidenceConditions(evidence.conditions)) return "preview";
	for (const auto& c : evidence.cases)
		if (!c.passed || !std::isfinite(c.error) || !std::isfinite(c.tolerance) || c.tolerance<0 ||
			!(c.error<=c.tolerance) || !DigestValid(c.digest) || c.id.empty()) return "preview";
	return "predictive";
}

bool ValidSideOutputName(const std::string& name)
{
	if (name.empty() || name.size()>64 || name[0]=='.' || name.find_first_of("/\\:")!=std::string::npos)
		return false;
	// Cache-owned and worker-owned names (SimulationCache.cpp, SimulationWorker.cpp).
	if (name=="manifest.cbor" || name=="writer.lock" || name.rfind("rise-sim.",0)==0) return false;
	return (name.size()<6 || name.compare(name.size()-6,6,".frame")!=0) &&
		(name.size()<7 || name.compare(name.size()-7,7,".static")!=0);
}

bool PortsCompatible(const PortDeclaration& offer,const PortDeclaration& accept,std::string& error)
{
	if (!offer.offers || accept.offers) return Fail(error,"port binding must connect an offered port to an accepted port");
	if (offer.codec!=accept.codec) return Fail(error,"port codecs differ: "+offer.codec+" vs "+accept.codec);
	if (offer.quantity!=accept.quantity || offer.units!=accept.units)
		return Fail(error,"port quantities or units differ: "+offer.quantity+" ["+offer.units+"] vs "+
			accept.quantity+" ["+accept.units+"]");
	if (offer.channels.size()!=accept.channels.size())
		return Fail(error,"port channel lists differ (a port adapter must convert them)");
	for (std::size_t i=0;i<offer.channels.size();++i) {
		const auto& a=offer.channels[i];
		const auto& b=accept.channels[i];
		if (a.name!=b.name || a.quantity!=b.quantity || a.units!=b.units || a.components!=b.components)
			return Fail(error,"port channel "+a.name+" ["+a.units+"] differs from "+b.name+" ["+b.units+"]");
	}
	if (!offer.domain.empty() && !accept.domain.empty() && offer.domain!=accept.domain)
		return Fail(error,"port domains differ (a port adapter must resample)");
	return true;
}

bool ISceneInputs::SampleTransform(const std::string& name,double,RigidTransform&,std::string& error) const
{
	return Fail(error,"no transform input named "+name);
}

bool ISceneInputs::ResolveStatic(const std::string& digest,std::size_t,std::shared_ptr<const Bytes>&,std::string& error) const
{
	return Fail(error,"no static payload "+digest+" among the scene inputs");
}

const ISceneInputs& EmptySceneInputs()
{
	static const NoSceneInputs empty;
	return empty;
}

bool IFrameSink::PublishStatic(const std::string&,std::shared_ptr<const Bytes>,std::string&,std::string& error)
{
	return Fail(error,"this frame sink does not take static payloads");
}

bool IPortExchange::OfferStatic(const std::string&,std::shared_ptr<const Bytes>,std::string&)
{
	return true;
}

PortStatus IPortExchange::ResolveStatic(const std::string& digest,std::size_t,std::shared_ptr<const Bytes>&,
	std::string& error)
{
	error="static payload not offered: "+digest;
	return PortStatus::Unavailable;
}

StaticPayloadResolver IPortExchange::Resolver()
{
	return [this](const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& bytes,std::string& error) {
		return ResolveStatic(digest,budget,bytes,error)==PortStatus::Ready;
	};
}

bool NoSceneInputs::Identity(const std::string& name,std::string&,std::string& error) const
{
	return Fail(error,"no scene input named "+name);
}

bool NoSceneInputs::Sample(const std::string& name,double,std::shared_ptr<const Bytes>&,std::string&,
	std::string& error) const
{
	return Fail(error,"no scene input named "+name);
}

bool RegisterSolver(const std::string& id,SolverFactory factory,std::string& error)
{
	if (id.empty() || id.find('\0')!=std::string::npos || !RISECBOR64::IsUTF8NFC(id) || !factory)
		return Fail(error,"invalid solver id or factory");
	auto& r=TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& f : r.factories) if (f.first==id) return Fail(error,"solver already registered: "+id);
	r.factories.emplace_back(id,std::move(factory));
	return true;
}

std::unique_ptr<ISimulationSolver> CreateSolver(const std::string& id,std::string& error)
{
	SolverFactory factory;
	{
		auto& r=TheRegistry();
		std::lock_guard<std::mutex> hold(r.mutex);
		for (const auto& f : r.factories) if (f.first==id) factory=f.second;
	}
	if (!factory) { error="unknown solver: "+id; return nullptr; }
	auto solver=factory();
	if (!solver) error="solver factory failed: "+id;
	return solver;
}

std::vector<std::string> RegisteredSolvers()
{
	auto& r=TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	std::vector<std::string> ids;
	for (const auto& f : r.factories) ids.push_back(f.first);
	std::sort(ids.begin(),ids.end());
	return ids;
}

void PortRelay::Connect(const std::string& accepted,const std::string& offered)
{
	routes_.emplace_back(accepted,offered);
}

bool PortRelay::Offer(const std::string& port,double time,std::shared_ptr<const Bytes> encoded,std::string& error)
{
	if (!encoded || !std::isfinite(time)) return Fail(error,"invalid port offer");
	auto it=std::find_if(offers_.begin(),offers_.end(),[&](const auto& o){return o.first==port;});
	if (it==offers_.end()) { offers_.push_back({port,{}}); it=offers_.end()-1; }
	auto& samples=it->second;
	if (!samples.empty() && samples.back().time>time) return Fail(error,"port offers must not go back in time");
	if (!samples.empty() && samples.back().time==time) samples.back().bytes=std::move(encoded);
	else samples.push_back({time,std::move(encoded)});
	return true;
}

bool PortRelay::OfferStatic(const std::string&,std::shared_ptr<const Bytes> payload,std::string& error)
{
	if (!payload) return Fail(error,"invalid static payload offer");
	const auto digest=RISECBOR64::SHA256Hex(*payload);
	for (const auto& s : statics_) if (s.first==digest) return true;
	statics_.emplace_back(digest,std::move(payload));
	return true;
}

PortStatus PortRelay::ResolveStatic(const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& payload,
	std::string& error)
{
	for (const auto& s : statics_) if (s.first==digest) {
		if (s.second->size()>budget) { error="static payload exceeds the budget"; return PortStatus::Failed; }
		payload=s.second;
		return PortStatus::Ready;
	}
	error="static payload not offered: "+digest;
	return PortStatus::Unavailable;
}

PortStatus PortRelay::Accept(const std::string& port,double time,std::shared_ptr<const Bytes>& encoded,std::string& error)
{
	std::string source=port;
	bool routed=routes_.empty();
	for (const auto& r : routes_) if (r.first==port) { source=r.second; routed=true; }
	if (!routed) { error="port not connected: "+port; return PortStatus::Unbound; }
	const auto it=std::find_if(offers_.begin(),offers_.end(),[&](const auto& o){return o.first==source;});
	if (it==offers_.end()) { error="nothing offered on "+source; return PortStatus::Unavailable; }
	const auto& s=it->second;
	auto after=std::upper_bound(s.begin(),s.end(),time,[](double t,const Sample& x){return t<x.time;});
	if (after==s.begin()) { error="no offer at or before requested time"; return PortStatus::Unavailable; }
	encoded=std::prev(after)->bytes;
	return PortStatus::Ready;
}

CachedPortFeed::CachedPortFeed(std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot,std::size_t byteBudget)
	: snapshot_(std::move(snapshot)), budget_(byteBudget)
{
}

void CachedPortFeed::Bind(const std::string& accepted,const std::string& output)
{
	bindings_.emplace_back(accepted,output);
}

bool CachedPortFeed::Offer(const std::string&,double,std::shared_ptr<const Bytes>,std::string&)
{
	return true;
}

PortStatus CachedPortFeed::ResolveStatic(const std::string& digest,std::size_t budget,
	std::shared_ptr<const Bytes>& payload,std::string& error)
{
	if (!snapshot_) { error="no snapshot"; return PortStatus::Unbound; }
	const auto status=snapshot_->ReadStatic(digest,std::min(budget,budget_),payload,error);
	if (status==FrameStatus::Ready) return PortStatus::Ready;
	return status==FrameStatus::Unavailable ? PortStatus::Unavailable : PortStatus::Failed;
}

PortStatus CachedPortFeed::Accept(const std::string& port,double time,std::shared_ptr<const Bytes>& encoded,
	std::string& error)
{
	const auto binding=std::find_if(bindings_.begin(),bindings_.end(),[&](const auto& b){return b.first==port;});
	if (binding==bindings_.end() || !snapshot_) { error="port not bound: "+port; return PortStatus::Unbound; }
	const CacheEntry* best=nullptr;
	for (const auto& e : snapshot_->Entries())
		if (e.output==binding->second && e.time<=time && (!best || e.time>best->time)) best=&e;
	if (!best) { error="no cached value at or before requested time"; return PortStatus::Unavailable; }
	std::shared_ptr<const Bytes> bytes;
	const auto status=snapshot_->Read(best->index,best->output,budget_,bytes,error);
	if (status==FrameStatus::Unavailable) return PortStatus::Unavailable;
	if (status!=FrameStatus::Ready) return PortStatus::Failed;
	encoded=std::move(bytes);
	return PortStatus::Ready;
}

} }
