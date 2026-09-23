//////////////////////////////////////////////////////////////////////
//
//  TranslucentPelPhotonMap.cpp - Implements the caustic photon map of type PEL
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2002
//  Tabs: 4
//  Comments:  The code here is an implementation from Henrik Wann
//             Jensen's book Realistic Image Synthesis Using 
//             Photon Mapping.  Much of the code is influeced or
//             taken from the sample code in the back of his book.
//			   I have however used STD data structures rather than
//			   reinventing the wheel as he does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentPelPhotonMap.h"
#include "../Materials/TranslucentSPF.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/FiniteMath.h"
#include <limits>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
 unsigned int AvailableBytes( const IReadBuffer& buffer )
 {
  const unsigned int cursor=buffer.getCurPos(),size=buffer.Size();
  return cursor<=size ? size-cursor : 0;
 }
 bool LoadError( const char* message )
 {
  GlobalLog()->PrintEx(eLog_Error,"TranslucentPelPhotonMap: %s; existing map retained",message);
  return false;
 }
 bool FinitePoint( const Point3& p )
 {
  return IsFiniteDouble(p.x) && IsFiniteDouble(p.y) && IsFiniteDouble(p.z);
 }
 bool FiniteDirection( const Vector3& w )
 {
  return IsFiniteDouble(w.x) && IsFiniteDouble(w.y) && IsFiniteDouble(w.z)
   && Vector3Ops::SquaredModulus(w)>0;
 }
}

TranslucentPelPhotonMap::TranslucentPelPhotonMap( const unsigned int maximum,
 const IPhotonTracer* tracer ) : PhotonMapCore<TranslucentPhoton>(maximum,tracer) {}
TranslucentPelPhotonMap::~TranslucentPelPhotonMap() {}

bool TranslucentPelPhotonMap::Store( const RISEPel&,const Point3& )
{
 GlobalLog()->PrintEasyError("TranslucentPelPhotonMap::Store requires incident direction and packet response kind; no photon stored");
 return false;
}

bool TranslucentPelPhotonMap::Store( const RISEPel& power,const Point3& position,
 const Vector3& incomingDirection,const bool diffuseExit )
{
 if(vphotons.size()>=nMaxPhotons || ColorMath::MaxValue(power)<=0) return false;
 TranslucentPhoton photon;
 photon.ptPosition=position;photon.power=power;
 photon.incomingDirection=incomingDirection;photon.diffuseExit=diffuseExit;
 bbox.Include(position);vphotons.push_back(photon);
 maxPower=r_max(maxPower,ColorMath::MaxValue(power));
 return true;
}

void TranslucentPelPhotonMap::RadianceEstimate( RISEPel& rad,
 const RayIntersectionGeometric& query,const IBSDF& bsdf, const IORStack* pIorStack ) const
{
 rad=RISEPel(0.0);
 PhotonDistListType heap;
 LocatePhotons(query.ptIntersection,dGatherRadius,nMaxPhotonsOnGather,heap,0,static_cast<int>(vphotons.size())-1);
 if(heap.size()<=nMinPhotonsOnGather) return;
 if(heap.size()<nMaxPhotonsOnGather) std::make_heap(heap.begin(),heap.end());
 const Scalar radius2=heap[0].distance;
 // Co-located packets define no finite-area density, for either kind.
 if(radius2<=0) return;
 const Vector3 wo=-query.ray.Dir();
 // The dedicated exit packet already contains Beer*(1-scattering). Its
 // remaining angular law is the material's clipped cosine exit lobe, whose
 // axis is oriented toward the true geometric exterior (DL45/DL70).
 const Vector3 outside=query.HasTrueGeomSide()?query.UnflippedGeomNormal():query.vNormal;
 const Vector3 axis=TranslucentSPFDetail::OrientedLobeAxis(query.vNormal,outside);
 const bool seesExit=Vector3Ops::Dot(wo,outside)>0 && Vector3Ops::Dot(wo,axis)>0;
 const Scalar valid=TranslucentSPFDetail::ExitValidFraction(axis,outside);
 for(const auto& item:heap) {
  const TranslucentPhoton& photon=item.element;
  const Scalar response=PathVertexEval::RadianceShadingNormalFactor(query.vNormal,query.vGeomNormal,photon.incomingDirection);
  if(response==0) continue;
  if(photon.diffuseExit) {
   // p_exit(wo) * importance-adjoint / |Ng.wo| cancels to
   // |Ns.wi| / (|Ng.wi| * pi * valid). Do not apply the query's
   // front-reflection BSDF or Beer attenuation a second time.
   if(seesExit) rad=rad+photon.power*(response/(PI*valid));
  } else {
   rad=rad+photon.power*bsdf.valueStateful(photon.incomingDirection,query,pIorStack)*response;
  }
 }
 rad=rad/(PI*radius2);
}

void TranslucentPelPhotonMap::Serialize( IWriteBuffer& buffer ) const
{
 buffer.ResizeForMore(52);
 // This prefix cannot be a valid legacy (maxPhotons,prevScale) pair.
 buffer.setUInt(0);buffer.setUInt(std::numeric_limits<unsigned int>::max());buffer.setUInt(1);
 buffer.setUInt(nMaxPhotons);buffer.setUInt(nPrevScale);
 buffer.setDouble(dGatherRadius);buffer.setDouble(dEllipseRatio);
 buffer.setUInt(nMinPhotonsOnGather);buffer.setUInt(nMaxPhotonsOnGather);buffer.setDouble(maxPower);
 bbox.Serialize(buffer);
 buffer.ResizeForMore(static_cast<unsigned int>(4+74*vphotons.size()));
 buffer.setUInt(static_cast<unsigned int>(vphotons.size()));
 for(const auto& p:vphotons) {
  Point3Ops::Serialize(p.ptPosition,buffer);buffer.setUChar(p.plane);ColorUtils::SerializeRGBPel(p.power,buffer);
  buffer.setDouble(p.incomingDirection.x);buffer.setDouble(p.incomingDirection.y);buffer.setDouble(p.incomingDirection.z);
  buffer.setUChar(p.diffuseExit?1:0);
 }
}

bool TranslucentPelPhotonMap::DeserializeChecked( IReadBuffer& buffer )
{
 if(AvailableBytes(buffer)<12) return LoadError("truncated header");
 const unsigned int zero=buffer.getUInt(),marker=buffer.getUInt(),version=buffer.getUInt();
 if(zero!=0 || marker!=std::numeric_limits<unsigned int>::max())
  return LoadError("legacy directionless map cannot reconstruct its response; regenerate from its scene");
 if(version!=1) return LoadError("unsupported format version");
 if(AvailableBytes(buffer)<92) return LoadError("truncated parameters/bounds/count");
 const unsigned int maximum=buffer.getUInt(),scaled=buffer.getUInt();
 const Scalar radius=buffer.getDouble(),ellipse=buffer.getDouble();
 const unsigned int minimum=buffer.getUInt(),gather=buffer.getUInt();const Scalar power=buffer.getDouble();
 BoundingBox ignored;ignored.Deserialize(buffer);const unsigned int count=buffer.getUInt();
 if(count>maximum || scaled>count || count>AvailableBytes(buffer)/74)
  return LoadError("invalid or truncated packet count");
 if(!IsFiniteDouble(radius) || radius<0 || !IsFiniteDouble(ellipse) || ellipse<0 || !IsFiniteDouble(power))
  return LoadError("nonfinite/negative gather parameters");
 PhotonListType packets;packets.reserve(count);
 BoundingBox bounds(Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY));
 for(unsigned int i=0;i<count;++i) {
  TranslucentPhoton p;Point3Ops::Deserialize(p.ptPosition,buffer);p.plane=buffer.getUChar();ColorUtils::DeserializeRGBPel(p.power,buffer);
  p.incomingDirection.x=buffer.getDouble();p.incomingDirection.y=buffer.getDouble();p.incomingDirection.z=buffer.getDouble();
  const unsigned char kind=buffer.getUChar();
  if(!FinitePoint(p.ptPosition) || p.plane>2 || !FiniteDirection(p.incomingDirection) || kind>1 || !IsFiniteDouble(p.power.r) || !IsFiniteDouble(p.power.g) || !IsFiniteDouble(p.power.b))
   return LoadError("invalid directional packet");
  p.diffuseExit=kind!=0;bounds.Include(p.ptPosition);packets.push_back(p);
 }
 vphotons.swap(packets);bbox=bounds;nMaxPhotons=maximum;nPrevScale=scaled;
 dGatherRadius=radius;dEllipseRatio=ellipse;nMinPhotonsOnGather=minimum;nMaxPhotonsOnGather=gather;maxPower=power;
 Balance();return true;
}

void TranslucentPelPhotonMap::Deserialize( IReadBuffer& buffer ) { DeserializeChecked(buffer); }

void TranslucentPelPhotonMap::ScalePhotonPower( const Scalar scale )
{
 for(unsigned int i=nPrevScale;i<vphotons.size();++i) vphotons[i].power=vphotons[i].power*scale;
 nPrevScale=static_cast<unsigned int>(vphotons.size());
 PhotonMapCore<TranslucentPhoton>::ScalePhotonPower(scale);
}
