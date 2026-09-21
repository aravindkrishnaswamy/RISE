//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonMap.cpp - Implements the global photon map of type PEL
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2002
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
#include "GlobalPelPhotonMap.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/FiniteMath.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
 int AnchorMedian( int from, int to )
 {
  int median=1;
  while(4*median<=to-from+1) median*=2;
  return 3*median<=to-from+1 ? 2*median+from-1 : to-median+1;
 }
 bool FinitePoint( const Point3& p )
 {
  return IsFiniteDouble(p.x) && IsFiniteDouble(p.y) && IsFiniteDouble(p.z);
 }
 bool FiniteNormal( const Vector3& n )
 {
  return IsFiniteDouble(n.x) && IsFiniteDouble(n.y) && IsFiniteDouble(n.z)
   && Vector3Ops::SquaredModulus(n)>0;
 }
 unsigned int AvailableBytes( const IReadBuffer& buffer )
 {
  // MemoryBuffer::HowFarToEnd is a legacy last-index distance, not the
  // number of readable bytes. Bounds checks use the actual size/cursor.
  const unsigned int cursor=buffer.getCurPos(),size=buffer.Size();
  return cursor<=size ? size-cursor : 0;
 }
 bool LoadError( const char* message )
 {
  GlobalLog()->PrintEx(eLog_Error,"GlobalPelPhotonMap: %s; existing map retained",message);
  return false;
 }
}

GlobalPelPhotonMap::GlobalPelPhotonMap( const unsigned int max_photons,
 const IPhotonTracer* tracer ) :
 PhotonMapDirectionalPelHelper<IrradPhoton>(max_photons,tracer),
 anchorSpacing(0), hasGeometricNormals(true)
{}
GlobalPelPhotonMap::~GlobalPelPhotonMap() {}

void GlobalPelPhotonMap::BalanceAnchors( int from, int to, BoundingBox bounds )
{
 if(to<=from) return;
 const Vector3& e=bounds.GetExtents();
 const unsigned char axis=e.x>e.y && e.x>e.z ? 0 : e.y>e.z ? 1 : 2;
 const int median=AnchorMedian(from,to);
 std::nth_element(anchors.begin()+from,anchors.begin()+median,anchors.begin()+to+1,
  [axis](const CacheAnchor& a,const CacheAnchor& b){return a.position[axis]<b.position[axis];});
 anchors[median].plane=axis;
 BoundingBox left=bounds,right=bounds;
 left.ur[axis]=right.ll[axis]=anchors[median].position[axis];
 BalanceAnchors(from,median-1,left);
 BalanceAnchors(median+1,to,right);
}

void GlobalPelPhotonMap::FindAnchor( const Point3& point,const Vector3& normal,
 int from,int to,Scalar& distance,const CacheAnchor*& nearest ) const
{
 if(to<from) return;
 const int median=AnchorMedian(from,to);
 const CacheAnchor& a=anchors[median];
 const Scalar d2=Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(point,a.position));
 // The original cache intended a similar-surface-normal lookup. Incident
 // photon directions are unrelated to this compatibility test. Keep the
 // 0.9 normal similarity policy, now applied to exact geometric normals.
 if(d2<distance && Vector3Ops::Dot(normal,a.geometricNormal)>0.9) {
  distance=d2;nearest=&a;
 }
 const Scalar side=point[a.plane]-a.position[a.plane];
 if(side<=0) {
  FindAnchor(point,normal,from,median-1,distance,nearest);
  if(side*side<distance) FindAnchor(point,normal,median+1,to,distance,nearest);
 } else {
  FindAnchor(point,normal,median+1,to,distance,nearest);
  if(side*side<distance) FindAnchor(point,normal,from,median-1,distance,nearest);
 }
}

const GlobalPelPhotonMap::CacheAnchor* GlobalPelPhotonMap::FindAnchor(
 const Point3& point,const Vector3& normal ) const
{
 const CacheAnchor* nearest=nullptr;
 Scalar distance=dGatherRadius;
 FindAnchor(point,normal,0,static_cast<int>(anchors.size())-1,distance,nearest);
 return nearest;
}

void GlobalPelPhotonMap::PrecomputeIrradiance( const unsigned int apart,IProgressCallback* progress )
{
 anchors.clear();anchorSpacing=0;
 if(!hasGeometricNormals || apart==0) return;
 anchorSpacing=apart;
 anchors.reserve((vphotons.size()+apart-1)/apart);
 if(progress) progress->SetTitle("Preparing directional global photon anchors: ");
 for(size_t i=0;i<vphotons.size();i+=apart) {
  const IrradPhoton& p=vphotons[i];
  anchors.push_back({p.ptPosition,p.geometricNormal,0});
  if(progress && i%1000==0) progress->Progress(double(i),double(vphotons.size()));
 }
 BalanceAnchors(0,static_cast<int>(anchors.size())-1,bbox);
 if(progress) progress->Progress(1.,1.);
}

void GlobalPelPhotonMap::SetGatherParams( const Scalar radius,const Scalar ellipse,
 const unsigned int minimum,const unsigned int maximum,IProgressCallback* progress )
{
 PhotonMapCore<IrradPhoton>::SetGatherParams(radius,ellipse,minimum,maximum,progress);
 PrecomputeIrradiance(anchorSpacing?anchorSpacing:4,progress);
}

void GlobalPelPhotonMap::RadianceAtAnchor( RISEPel& rad,const CacheAnchor& anchor,
 const RayIntersectionGeometric& query,const IBSDF& bsdf ) const
{
 rad=RISEPel(0.0);
 PhotonDistListType heap;
 LocatePhotons(anchor.position,dGatherRadius,nMaxPhotonsOnGather,heap,0,static_cast<int>(vphotons.size())-1);
 if(heap.size()<=nMinPhotonsOnGather) return;
 if(heap.size()<nMaxPhotonsOnGather) std::make_heap(heap.begin(),heap.end());
 const Scalar radius2=heap[0].distance;
 for(const auto& item:heap) {
  const IrradPhoton& photon=item.element;
  const Scalar height=Vector3Ops::Dot(Vector3Ops::mkVector3(photon.ptPosition,anchor.position),anchor.geometricNormal);
  if(fabs(height)>=radius2*dEllipseRatio) continue;
  const Vector3 wi=photon.incomingDirection;
  // The anchor owns area and spatial kernel; the query owns the material,
  // BSDF support, shading frame and position. Do not rebuild the query at
  // the anchor or replace its geometric support normal.
  const Scalar response=PathVertexEval::RadianceShadingNormalFactor(query.vNormal,anchor.geometricNormal,wi);
  if(response>0) rad=rad+photon.power*bsdf.value(wi,query)*response;
 }
 rad=rad/(PI*radius2);
}

void GlobalPelPhotonMap::RadianceEstimate( RISEPel& rad,
 const RayIntersectionGeometric& query,const IBSDF& bsdf ) const
{
 const CacheAnchor* anchor=FindAnchor(query.ptIntersection,query.vGeomNormal);
 if(anchor) RadianceAtAnchor(rad,*anchor,query,bsdf);
 else RadianceEstimateFromSearch(rad,query,bsdf);
}

bool GlobalPelPhotonMap::Store( const RISEPel& power,const Point3& pos,
 const Vector3& geometricNormal,const Vector3& dir )
{
 if(vphotons.size()>=nMaxPhotons) return false;
 IrradPhoton photon;
 photon.incomingDirection=dir;photon.ptPosition=pos;photon.power=power;photon.geometricNormal=geometricNormal;
 int theta=int(acos(dir.z)*(256.0/PI));
 photon.theta=theta>255?255:static_cast<unsigned char>(theta);
 int phi=int(atan2(dir.y,dir.x)*(256.0/TWO_PI));
 phi=phi>255?255:phi;
 photon.phi=static_cast<unsigned char>(phi<0?phi+256:phi);
 bbox.Include(pos);vphotons.push_back(photon);
 maxPower=r_max(maxPower,ColorMath::MaxValue(power));
 // New positions invalidate the anchor set. The caller balances before
 // querying, then SetGatherParams/PrecomputeIrradiance rebuilds anchors.
 anchors.clear();anchorSpacing=0;
 return true;
}

void GlobalPelPhotonMap::Serialize( IWriteBuffer& buffer ) const
{
 buffer.ResizeForMore(46);
 buffer.setUInt(nMaxPhotons);buffer.setUInt(nPrevScale);
 buffer.setDouble(dGatherRadius);buffer.setDouble(dEllipseRatio);
 buffer.setUInt(nMinPhotonsOnGather);buffer.setUInt(nMaxPhotonsOnGather);
 buffer.setDouble(maxPower);
 // Flag3 preserves exact incident directions as well as geometric normals.
 // Earlier formats lose incident support and require regeneration.
 buffer.setUChar(3);buffer.setUChar(hasGeometricNormals?1:0);buffer.setUInt(anchorSpacing);
 bbox.Serialize(buffer);
 buffer.ResizeForMore(static_cast<unsigned int>(4+97*vphotons.size()));
 buffer.setUInt(static_cast<unsigned int>(vphotons.size()));
 for(const auto& p:vphotons) {
  Point3Ops::Serialize(p.ptPosition,buffer);buffer.setUChar(p.plane);
  ColorUtils::SerializeRGBPel(p.power,buffer);
  buffer.setDouble(p.incomingDirection.x);buffer.setDouble(p.incomingDirection.y);buffer.setDouble(p.incomingDirection.z);
  buffer.setDouble(p.geometricNormal.x);buffer.setDouble(p.geometricNormal.y);buffer.setDouble(p.geometricNormal.z);
 }
}

bool GlobalPelPhotonMap::DeserializeChecked( IReadBuffer& buffer )
{
 if(AvailableBytes(buffer)<41) return LoadError("truncated header");
 const unsigned int maximum=buffer.getUInt(),scaled=buffer.getUInt();
 const Scalar radius=buffer.getDouble(),ellipse=buffer.getDouble();
 const unsigned int minimum=buffer.getUInt(),gather=buffer.getUInt();
 const Scalar power=buffer.getDouble();const unsigned char format=buffer.getUChar();
 if(format==1) return LoadError("legacy scalar irradiance cache lacks incident directions; regenerate from its scene");
 if(format==0 || format==2) return LoadError("legacy compressed directions cannot recover incident support; regenerate from its scene");
 if(format!=3) return LoadError("unsupported format");
 bool geometry=false;unsigned int spacing=0;
 if(format==3) {
  if(AvailableBytes(buffer)<5) return LoadError("truncated directional header");
  const unsigned char state=buffer.getUChar();if(state>1) return LoadError("invalid normal provenance");
  geometry=state!=0;spacing=buffer.getUInt();
  if(!geometry && spacing) return LoadError("anchors require geometric normals");
 }
 if(AvailableBytes(buffer)<52) return LoadError("truncated bounds/count");
 BoundingBox ignored;ignored.Deserialize(buffer);const unsigned int count=buffer.getUInt();
 const unsigned int recordBytes=97;
 if(count>maximum || scaled>count || count>AvailableBytes(buffer)/recordBytes)
  return LoadError("invalid or truncated packet count");
 if(!IsFiniteDouble(radius) || radius<0 || !IsFiniteDouble(ellipse) || ellipse<0 || !IsFiniteDouble(power))
  return LoadError("nonfinite/negative gather parameters");
 PhotonListType packets;packets.reserve(count);
 BoundingBox bounds(Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY));
 for(unsigned int i=0;i<count;++i) {
  IrradPhoton p;Point3Ops::Deserialize(p.ptPosition,buffer);p.plane=buffer.getUChar();
  ColorUtils::DeserializeRGBPel(p.power,buffer);
  p.incomingDirection.x=buffer.getDouble();p.incomingDirection.y=buffer.getDouble();p.incomingDirection.z=buffer.getDouble();
  p.geometricNormal.x=buffer.getDouble();p.geometricNormal.y=buffer.getDouble();p.geometricNormal.z=buffer.getDouble();
  if(!FiniteNormal(p.incomingDirection) || !FinitePoint(p.ptPosition) || p.plane>2 || !IsFiniteDouble(p.power.r) || !IsFiniteDouble(p.power.g) || !IsFiniteDouble(p.power.b) || (geometry && !FiniteNormal(p.geometricNormal)))
   return LoadError("invalid packet data");
  bounds.Include(p.ptPosition);packets.push_back(p);
 }
 // Commit only after every field has been read and validated. Rebalancing
 // also repairs legacy files written with the old inclusive-end KD bug.
 vphotons.swap(packets);bbox=bounds;nMaxPhotons=maximum;nPrevScale=scaled;
 dGatherRadius=radius;dEllipseRatio=ellipse;nMinPhotonsOnGather=minimum;nMaxPhotonsOnGather=gather;maxPower=power;
 hasGeometricNormals=geometry;anchors.clear();anchorSpacing=0;Balance();
 if(spacing) PrecomputeIrradiance(spacing,nullptr);
 return true;
}

void GlobalPelPhotonMap::Deserialize( IReadBuffer& buffer )
{
 DeserializeChecked(buffer);
}
