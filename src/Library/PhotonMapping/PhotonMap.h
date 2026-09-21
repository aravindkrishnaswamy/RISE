//////////////////////////////////////////////////////////////////////
//
//  PhotonMap.h - Definition of the photon map class which is what
//                does all the work in photon mapping
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

#ifndef PHOTON_MAP_
#define PHOTON_MAP_

#include "../Interfaces/IPhotonMap.h"
#include "../Interfaces/IPhotonTracer.h"
#include "../Utilities/Reference.h"
#include "../Utilities/BoundingBox.h"
#include "Photon.h"
#include "../Utilities/PathVertexEval.h"
#include <vector>
#include <algorithm>
#include <limits>
#include <type_traits>
#include "../Interfaces/IReadBuffer.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/FiniteMath.h"

namespace RISE
{
	//
	// This is implementation of common functions in photon maps (balancing code...)
	//
	namespace Implementation
	{
        // The producer samples one declared uniform wavelength law per map.
        // Stored power is conditional spectral power divided by all shots.
        struct SpectralPhotonSamplingLaw {
            Scalar begin=0,end=0,yIntegral=0;
            unsigned count=0;
            std::vector<Scalar> representatives;
            bool Configure(Scalar a,Scalar b,unsigned n) {
                if(!IsFiniteDouble(a)||!IsFiniteDouble(b)||a<=0||b<=a||n==0||!IsFiniteDouble(b-a))return false;
                const Scalar integral=ColorUtils::CIE_Y_Integral(a,b);
                if(!IsFiniteDouble(integral)||integral<=0)return false;
                std::vector<Scalar> grid;
                if(n<10000){
                    const Scalar step=(b-a)/n;grid.reserve(n);grid.push_back(a);
                    for(unsigned i=1;i<n;++i){const Scalar value=a+i*step;if(value<=grid.back()||value>=b)return false;grid.push_back(value);}
                }
                begin=a;end=b;count=n;yIntegral=integral;representatives.swap(grid);return true;
            }
            bool Configured()const{return count!=0;}
            Scalar GridValue(unsigned i)const{return representatives[i];}
            Scalar Sample(Scalar u)const {
                if(count<10000)return GridValue(static_cast<unsigned>(u*count));
                const Scalar value=begin+u*(end-begin);
                // A rounded addition can land exactly on the excluded upper
                // endpoint. Keep the same draw in its representable interval.
                return value<end?value:std::nextafter(end,begin);
            }
            bool Contains(Scalar nm)const {
                if(!Configured()||!IsFiniteDouble(nm)||nm<begin||nm>=end)return false;
                if(count>=10000)return true;
                unsigned lo=0,hi=count;
                while(lo<hi){const unsigned mid=lo+(hi-lo)/2;if(GridValue(mid)<nm)lo=mid+1;else hi=mid;}
                return lo<count&&GridValue(lo)==nm;
            }
            Scalar WindowMass(Scalar nm,Scalar halfWidth)const {
                if(!Configured()||!IsFiniteDouble(nm)||nm<begin||nm>=end||!IsFiniteDouble(halfWidth)||halfWidth<0)return 0;
                if(count>=10000){
                    // Add clipped half-widths rather than subtracting two
                    // nearby wavelengths (which loses narrow-band mass).
                    const Scalar left=std::min(halfWidth,nm-begin);
                    const Scalar right=std::min(halfWidth,end-nm);
                    return (left+right)/(end-begin);
                }
                // Binary searches use the same subtraction predicate as the
                // gather, avoiding ceil/floor errors at represented grid edges.
                unsigned lo=0,hi=count;
                while(lo<hi){const unsigned mid=lo+(hi-lo)/2;const Scalar v=GridValue(mid);if(v<nm&&nm-v>halfWidth)lo=mid+1;else hi=mid;}
                const unsigned first=lo;hi=count;
                while(lo<hi){const unsigned mid=lo+(hi-lo)/2;const Scalar v=GridValue(mid);if(v<=nm||v-nm<=halfWidth)lo=mid+1;else hi=mid;}
                return Scalar(lo-first)/count;
            }
            Scalar IntegralScale()const{return Configured()?(end-begin)/yIntegral:0;}
        };

		template< class PhotType >
		class PhotonMapCore :
			public virtual IPhotonMap,
			public virtual Reference
		{
		protected:
			typedef std::vector< distance_container< PhotType > >	PhotonDistListType;
			typedef std::vector< PhotType >							PhotonListType;

			PhotonListType	vphotons;

			unsigned int	nMaxPhotons;
			unsigned int	nPrevScale;

			Scalar			dGatherRadius;
			Scalar			dEllipseRatio;
			unsigned int	nMinPhotonsOnGather;
			unsigned int	nMaxPhotonsOnGather;

			Scalar			maxPower;

			BoundingBox	bbox;

			const IPhotonTracer*	pTracer;					// The photon tracer that made this photon map

			PhotonMapCore(
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				) :
			nMaxPhotons( max_photons ),
			nPrevScale( 0 ),
			dGatherRadius( 1.0 ),
			dEllipseRatio( 0.05 ),
			nMinPhotonsOnGather( 20 ),
			nMaxPhotonsOnGather( 150 ),
			maxPower( 0 ),
			pTracer( tracer )
			{
				vphotons.reserve( max_photons );

				bbox = BoundingBox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

				if( pTracer ) {
					pTracer->addref();
				}
			}

			virtual ~PhotonMapCore()
			{
				Shutdown();
			}

			void Shutdown()
			{
				safe_release( pTracer );
			}

			static int less_than_X(const PhotType& lhs, const PhotType& rhs) { return lhs.ptPosition.x < rhs.ptPosition.x; }
			static int less_than_Y(const PhotType& lhs, const PhotType& rhs) { return lhs.ptPosition.y < rhs.ptPosition.y; }
			static int less_than_Z(const PhotType& lhs, const PhotType& rhs) { return lhs.ptPosition.z < rhs.ptPosition.z; }

			// Counts the number of photons at the particular location
			void CountPhotonsAt(
				const Point3&			loc,								// the location from which to search for photons
				const Scalar			maxDist,							// the maximum radius to look for photons
				const unsigned int		max,								// maximum number of photons to look for
				unsigned int&			cnt									// count so far
				) const
			{
				CountPhotonsAt( loc, maxDist, max, 0, static_cast<int>(vphotons.size())-1, cnt );
			}

			// Counts the number of photons at the particular location
			void CountPhotonsAt(
				const Point3&			loc,								// the location from which to search for photons
				const Scalar			maxDist,							// the maximum radius to look for photons
				const unsigned int		max,								// maximum number of photons to look for
				const int				from,								// index to search from
				const int				to,									// index to search to
				unsigned int&			cnt									// count so far
				) const
			{
				// sanity check
				if( to-from < 0 ) {
					return;
				}

				// Don't bother if we already at the maximum
				if( cnt >= max ) {
					return;
				}

				// Compute a new median
				int median = 1;

				while( (4*median) <= (to-from+1) ) {
					median += median;
				}

				if( (3*median) <= (to-from+1) ) {
					median += median;
					median += from - 1;
				} else {
					median = to-median + 1;
				}

				// Compute the distance to the photon
				Vector3 v = Vector3Ops::mkVector3( loc, vphotons[ median ].ptPosition );
				Scalar	distanceToPhoton = Vector3Ops::SquaredModulus(v);

				if( distanceToPhoton < maxDist )
				{
					// We've found a photon!
					cnt++;
				}

				int axis = vphotons[median].plane;

				Scalar distance2 = loc[axis] - vphotons[median].ptPosition[axis];
				Scalar sqrD2 = distance2*distance2;

				// At tangency the far half cannot contribute (strict radius),
				// but the near half still can. Never skip both.
				if( sqrD2 >= maxDist ) {
					if( distance2 <= 0 ) {
						CountPhotonsAt( loc, maxDist, max, from, median-1, cnt );
					} else {
						CountPhotonsAt( loc, maxDist, max, median+1, to, cnt );
					}
				}

				// Search both sides of the tree
				if( sqrD2 < maxDist ) {
					CountPhotonsAt( loc, maxDist, max, from, median-1, cnt );
					CountPhotonsAt( loc, maxDist, max, median+1, to, cnt );
				}
			}

			// finds the nearest photons in the photon map
			void LocatePhotons(
				const Point3&			loc,								// the location from which to search for photons
				const Scalar			maxDist,							// the maximum radius to look for photons
				const unsigned int		nPhotons,							// number of photons to use
				PhotonDistListType&		heap,								// the heap containing the photons (results)
				const int				from,								// index to search from
				const int				to									// index to search to
			) const
			{
				// sanity check
				if( nPhotons == 0 || to-from < 0 ) {
					return;
				}

				// Compute a new median
				int median = 1;

				while( (4*median) <= (to-from+1) ) {
					median += median;
				}

				if( (3*median) <= (to-from+1) ) {
					median += median;
					median += from - 1;
				} else {
					median = to-median + 1;
				}

				// Compute the distance to the photon
				const Vector3 v = Vector3Ops::mkVector3( loc, vphotons[ median ].ptPosition );
				const Scalar distanceToPhoton = Vector3Ops::SquaredModulus(v);


				Scalar md = maxDist;

				if( distanceToPhoton < md )
				{
					// We've found a photon!
					// Insert into candidate list
					heap.push_back( distance_container<PhotType>( vphotons[median], distanceToPhoton ));

					// Build the heap
					if( heap.size() == nPhotons-1 ) {
						std::make_heap( heap.begin(), heap.end() );
						// Build before the next push, but keep the original search
						// radius until all k candidates exist. Shrinking at k-1
						// can exclude the required kth (more distant) record.
					} else if( heap.size() >= nPhotons ) {
						std::push_heap( heap.begin(), heap.end() );

						if( heap.size() > nPhotons ) {
							// We got too many so pop the last one, which will be furthest away
							std::pop_heap( heap.begin(), heap.end() );
							heap.pop_back();
						}
						md = heap[0].distance;
					}

				}

				const int axis = vphotons[median].plane;

				const Scalar distance2 = loc[axis] - vphotons[median].ptPosition[axis];
				const Scalar sqrD2 = distance2*distance2;

				if( sqrD2 >= md ) {
					if( distance2 <= 0 ) {
						LocatePhotons( loc, md, nPhotons, heap, from, median-1 );
					} else {
						LocatePhotons( loc, md, nPhotons, heap, median+1, to );
					}
				}

				// Search both sides of the tree
				if( sqrD2 < md ) {
					LocatePhotons( loc, md, nPhotons, heap, from, median-1 );
					LocatePhotons( loc, md, nPhotons, heap, median+1, to );
				}
			}

			// Finds all the photons within a given radius
			void LocateAllPhotons(
				const Point3&			loc,								// the location from which to search for photons
				const Scalar			maxDist,							// the maximum radius to look for photons
				PhotonListType&			photons,							// the list containing all the photons
				const int				from,								// index to search from
				const int				to									// index to search to
			) const
			{
				// sanity check
				if( to-from < 0 ) {
					return;
				}

				// Compute a new median
				int median = 1;

				while( (4*median) <= (to-from+1) ) {
					median += median;
				}

				if( (3*median) <= (to-from+1) ) {
					median += median;
					median += from - 1;
				} else {
					median = to-median + 1;
				}

				// Compute the distance to the photon
				const Vector3 v = Vector3Ops::mkVector3( loc, vphotons[ median ].ptPosition );
				const Scalar distanceToPhoton = Vector3Ops::SquaredModulus(v);

				if( distanceToPhoton < maxDist ) {
					// We've found a photon!
					// Insert into candidate list
					photons.push_back( vphotons[median] );
				}

				const int axis = vphotons[median].plane;

				const Scalar distance2 = loc[axis] - vphotons[median].ptPosition[axis];
				const Scalar sqrD2 = distance2*distance2;

				if( sqrD2 >= maxDist ) {
					if( distance2 <= 0 ) {
						LocateAllPhotons( loc, maxDist, photons, from, median-1 );
					} else {
						LocateAllPhotons( loc, maxDist, photons, median+1, to );
					}
				}

				// Search both sides of the tree
				if( sqrD2 < maxDist ) {
					LocateAllPhotons( loc, maxDist, photons, from, median-1 );
					LocateAllPhotons( loc, maxDist, photons, median+1, to );
				}
			}

			void BalanceSegment( const int from, const int to )
			{
				// Sanity check
				if( to-from <= 0 ) {
					return;
				}

				// Find the axis to split along
				unsigned char axis = 2;

				const Vector3& extents = bbox.GetExtents();

				if( (extents.x) > (extents.y) &&
					(extents.x) > (extents.z) ) {
					axis = 0;
				} else if( extents.y > extents.z ) {
					axis = 1;
				}

				// Compute a new median
				int median = 1;

				while( (4*median) <= (to-from+1) ) {
					median += median;
				}

				if( (3*median) <= (to-from+1) ) {
					median += median;
					median += from - 1;
				} else {
					median = to-median + 1;
				}

				// Recursive bounds include to; nth_element takes an exclusive end.
				// Omitting that record breaks the KD half-space invariant.
				// Now sort
				switch( axis )
				{
				case 0:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to+1, less_than_X );
					break;
				case 1:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to+1, less_than_Y );
					break;
				case 2:
				default:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to+1, less_than_Z );
					break;
				}

				// Partition the photon block around the median
				vphotons[median].plane = axis;

				{
					// Build the left segment
					const Scalar tmp = bbox.ur[axis];
					bbox.ur[axis] = vphotons[median].ptPosition[axis];
					BalanceSegment( from, median-1 );
					bbox.ur[axis] = tmp;
				}

				{
					// Build the right segment
					const Scalar tmp = bbox.ll[axis];
					bbox.ll[axis] = vphotons[median].ptPosition[axis];
					BalanceSegment( median+1, to );
					bbox.ll[axis] = tmp;
				}
			}

			//! Tells the photon map to re-generate itself
			bool Regenerate( const Scalar time ) const
			{
				if( pTracer ) {
					return pTracer->TracePhotons( nMaxPhotons, time, true, 0 );
				} else {
					GlobalLog()->PrintEasyError( "PhotonMap:: Asked to regenerate but there is no tracer!" );
					return false;
				}
			}

		public:
			// balance creates a left-balanced kd-tree from the flat photon array.
			// This is called before the photon map is used for rasterization
			void Balance()
			{
				BalanceSegment( 0, static_cast<int>(vphotons.size())-1 );
			}

			BoundingBox GetBoundingBox()
			{
				return bbox;
			}

			void SetGatherParams( const Scalar radius, const Scalar ellipse_ratio, const unsigned int nminphotons, const unsigned int nmaxphotons, IProgressCallback* pFunc )
			{
				dGatherRadius = radius;
				dEllipseRatio = ellipse_ratio;
				nMinPhotonsOnGather = nminphotons;
				nMaxPhotonsOnGather = nmaxphotons;

				if( dGatherRadius <= 0 ) {
					const Scalar maxRadius = 1.4 * sqrt( maxPower * nmaxphotons );
	//				const Scalar maxRadius = INV_PI * sqrt( nmaxphotons * maxPower / 0.05 );
					dGatherRadius = maxRadius;
					GlobalLog()->PrintEx( eLog_Event, "PhotonMap::ScalePhotonPower:: Changed gather radius to new max radius: %f", dGatherRadius );
				}

				dGatherRadius *= dGatherRadius;
			}

			void GetGatherParams( Scalar& radius, Scalar& ellipse_ratio, unsigned int& nminphotons, unsigned int& nmaxphotons )
			{
				radius = sqrt(dGatherRadius);
				ellipse_ratio = dEllipseRatio;
				nminphotons = nMinPhotonsOnGather;
				nmaxphotons = nMaxPhotonsOnGather;
			}

			unsigned int NumStored( ){ return static_cast<unsigned int>(vphotons.size()); }
			unsigned int MaxPhotons( ){ return nMaxPhotons; }

			// scale = 1/number of emmitted photons
			void ScalePhotonPower( const Scalar scale )
			{
				maxPower *= scale;
			}

		};

		// Helper class for directional Pel photons
		template< class PhotType >
		class PhotonMapDirectionalHelper :
			public PhotonMapCore<PhotType>
		{
		protected:
			typedef std::vector< distance_container< PhotType > >	PhotonDistListType;
			typedef std::vector< PhotType >							PhotonListType;

            // Shared transactional format body for exact-direction Pel and
            // spectral maps. Derived maps serialize their own sampling law.
            struct ExactPacketState {
                unsigned maximum,scaled,minimum,gather;
                Scalar radius,ellipse,power;
                BoundingBox bounds;
                PhotonListType packets;
            };
            static unsigned ReadableBytes(const IReadBuffer& b) {
                return b.getCurPos()<=b.Size()?b.Size()-b.getCurPos():0;
            }
            static bool PacketLoadError(const char* message) {
                GlobalLog()->PrintEx(eLog_Error,"DirectionalPhotonMap: %s; existing map retained",message);
                return false;
            }
            static bool ValidDirection(const Vector3& w) {
                return IsFiniteDouble(w.x)&&IsFiniteDouble(w.y)&&IsFiniteDouble(w.z)
                    && Vector3Ops::SquaredModulus(w)>0;
            }
            static void WriteExactPrefix(IWriteBuffer& b,unsigned kind) {
                b.ResizeForMore(16);b.setUInt(0);b.setUInt(std::numeric_limits<unsigned>::max());b.setUInt(2);b.setUInt(kind);
            }
            static bool ReadExactPrefix(IReadBuffer& b,unsigned kind) {
                if(ReadableBytes(b)<16)return PacketLoadError("truncated format header");
                const unsigned maximum=b.getUInt(),marker=b.getUInt();
                if(maximum!=0||marker!=std::numeric_limits<unsigned>::max())
                    return PacketLoadError("legacy compressed directions cannot recover incident support; regenerate from its scene");
                const unsigned version=b.getUInt(),storedKind=b.getUInt();
                if(version!=2||storedKind!=kind)return PacketLoadError("unsupported format or photon-map kind");
                return true;
            }
            void WriteExactBody(IWriteBuffer& b)const {
                constexpr unsigned recordBytes=std::is_same<PhotType,SpectralPhoton>::value?65:73;
                b.ResizeForMore(static_cast<unsigned>(92+recordBytes*this->vphotons.size()));
                b.setUInt(this->nMaxPhotons);b.setUInt(this->nPrevScale);b.setDouble(this->dGatherRadius);b.setDouble(this->dEllipseRatio);
                b.setUInt(this->nMinPhotonsOnGather);b.setUInt(this->nMaxPhotonsOnGather);b.setDouble(this->maxPower);this->bbox.Serialize(b);
                b.setUInt(static_cast<unsigned>(this->vphotons.size()));
                for(const auto& p:this->vphotons){
                    Point3Ops::Serialize(p.ptPosition,b);b.setUChar(p.plane);
                    if constexpr(std::is_same<PhotType,SpectralPhoton>::value)b.setDouble(p.power);
                    else ColorUtils::SerializeRGBPel(p.power,b);
                    b.setDouble(p.incomingDirection.x);b.setDouble(p.incomingDirection.y);b.setDouble(p.incomingDirection.z);
                    if constexpr(std::is_same<PhotType,SpectralPhoton>::value)b.setDouble(p.nm);
                }
            }
            static bool ReadExactBody(IReadBuffer& b,ExactPacketState& state) {
                if(ReadableBytes(b)<92)return PacketLoadError("truncated packet header");
                state.maximum=b.getUInt();state.scaled=b.getUInt();state.radius=b.getDouble();state.ellipse=b.getDouble();
                state.minimum=b.getUInt();state.gather=b.getUInt();state.power=b.getDouble();BoundingBox ignored;ignored.Deserialize(b);
                const unsigned count=b.getUInt();constexpr unsigned recordBytes=std::is_same<PhotType,SpectralPhoton>::value?65:73;
                if(count>state.maximum||state.scaled>count||count>ReadableBytes(b)/recordBytes)return PacketLoadError("invalid or truncated packet count");
                if(!IsFiniteDouble(state.radius)||state.radius<0||!IsFiniteDouble(state.ellipse)||state.ellipse<0||!IsFiniteDouble(state.power))return PacketLoadError("invalid gather parameters");
                state.bounds=BoundingBox(Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY));
                state.packets.reserve(count);
                for(unsigned i=0;i<count;++i){
                    PhotType p;Point3Ops::Deserialize(p.ptPosition,b);p.plane=b.getUChar();
                    if constexpr(std::is_same<PhotType,SpectralPhoton>::value)p.power=b.getDouble();
                    else ColorUtils::DeserializeRGBPel(p.power,b);
                    p.incomingDirection.x=b.getDouble();p.incomingDirection.y=b.getDouble();p.incomingDirection.z=b.getDouble();
                    if constexpr(std::is_same<PhotType,SpectralPhoton>::value){
                        p.nm=b.getDouble();if(!IsFiniteDouble(p.power)||!IsFiniteDouble(p.nm))return PacketLoadError("nonfinite spectral packet");
                    }else if(!IsFiniteDouble(p.power.r)||!IsFiniteDouble(p.power.g)||!IsFiniteDouble(p.power.b))return PacketLoadError("nonfinite Pel packet");
                    if(!IsFiniteDouble(p.ptPosition.x)||!IsFiniteDouble(p.ptPosition.y)||!IsFiniteDouble(p.ptPosition.z)||p.plane>2||!ValidDirection(p.incomingDirection))return PacketLoadError("invalid packet geometry");
                    state.bounds.Include(p.ptPosition);state.packets.push_back(p);
                }
                return true;
            }
            void CommitExactBody(ExactPacketState& state) {
                this->vphotons.swap(state.packets);this->bbox=state.bounds;this->nMaxPhotons=state.maximum;this->nPrevScale=state.scaled;
                this->dGatherRadius=state.radius;this->dEllipseRatio=state.ellipse;this->nMinPhotonsOnGather=state.minimum;
                this->nMaxPhotonsOnGather=state.gather;this->maxPower=state.power;this->Balance();
            }

			// These look up tables are to speed up the computation of sin and cos
			Scalar		costheta[256];
			Scalar		sintheta[256];
			Scalar		cosphi[256];
			Scalar		sinphi[256];

			PhotonMapDirectionalHelper(
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				) :
			PhotonMapCore<PhotType>( max_photons, tracer )
			{
				// Initialize the conversion tables
				for( int i=0; i<256; i++ ) {
					Scalar	angle = Scalar(i) * (1.0/256.0)*PI;
					costheta[i] = cos( angle );
					sintheta[i] = sin( angle );
					cosphi[i] = cos( 2.0*angle );
					sinphi[i] = sin( 2.0*angle );
				}
			}

			virtual ~PhotonMapDirectionalHelper()
			{
			}

			// Returns the direction of a photon
			inline Vector3 PhotonDir( const unsigned char theta, const unsigned char phi ) const
			{
				return Vector3(
					sintheta[ theta ] * cosphi[ phi ],
					sintheta[ theta ] * sinphi[ phi ],
					costheta[ theta ] );
			}


		public:

			// scale = 1/number of emmitted photons
			void ScalePhotonPower( const Scalar scale )
			{
				for( unsigned int i=this->nPrevScale; i<this->vphotons.size(); i++ ) {
					this->vphotons[i].power = this->vphotons[i].power * scale;
				}

				this->nPrevScale = static_cast<unsigned int>(this->vphotons.size());

				PhotonMapCore<PhotType>::ScalePhotonPower( scale );
			}
		};

		// Helper class for directional Pel photons
		template< class PhotType >
		class PhotonMapDirectionalPelHelper :
			public PhotonMapDirectionalHelper<PhotType>
		{
		protected:
			typedef std::vector< distance_container< PhotType > >	PhotonDistListType;
			typedef std::vector< PhotType >							PhotonListType;

			PhotonMapDirectionalPelHelper(
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				) :
			PhotonMapDirectionalHelper<PhotType>( max_photons, tracer )
			{
			}

			virtual ~PhotonMapDirectionalPelHelper()
			{
			}

			//! Estimates a RISEPel irradiance by actually searching through the photons
			//! and doing computations
			void IrradianceEstimateFromSearch(
				RISEPel&						irrad,					// returned irradiance
				const Point3&					point,					// point to evaluate the estimate
				const Vector3&					normal					// normal at point of estimate
				) const
			{
				irrad = RISEPel( 0, 0, 0 );

				// locate the nearest photons
				PhotonDistListType heap;
				this->LocatePhotons( point, this->dGatherRadius, this->nMaxPhotonsOnGather, heap, 0, static_cast<int>(this->vphotons.size())-1 );

				if( heap.size() > this->nMinPhotonsOnGather )
				{
					// They haven't been sorted yet, since the list isn't full
					if( heap.size() < this->nMaxPhotonsOnGather ) {
						std::make_heap( heap.begin(), heap.end() );
					}

					const Scalar farthest_away = heap[0].distance;
					// A zero-radius neighborhood has no finite-area density estimate.
					if( farthest_away <= 0 ) return;

					typename PhotonDistListType::const_iterator i, e;

					for( i=heap.begin(), e=heap.end(); i!=e; i++ ) {
						const PhotType& p = i->element;

						const Scalar maxNDist = farthest_away * this->dEllipseRatio;
						const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, point );
						const Scalar pcos = Vector3Ops::Dot( vec, normal );

						if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
							const Vector3 vPhotonDir = p.incomingDirection;
							if( Vector3Ops::Dot(vPhotonDir,normal) > 0 ) {
								irrad = irrad + p.power;
							}
						}
					}

					irrad = irrad * (1.0/(PI*farthest_away));
				}
			}

			//! Estimates a RISEPel radiance by actually searching through the photons
			//! and doing computations, this is alternate code
			void RadianceEstimateFromSearch(
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const
			{
				rad = RISEPel( 0, 0, 0 );

				// locate the nearest photons
				PhotonDistListType heap;
				this->LocatePhotons( ri.ptIntersection, this->dGatherRadius, this->nMaxPhotonsOnGather, heap, 0, static_cast<int>(this->vphotons.size())-1 );

				if( heap.size() > this->nMinPhotonsOnGather )
				{
					// They haven't been sorted yet, since the list isn't full
					if( heap.size() < this->nMaxPhotonsOnGather ) {
						std::make_heap( heap.begin(), heap.end() );
					}

					const Scalar farthest_away = heap[0].distance;
					// A zero-radius neighborhood has no finite-area density estimate.
					if( farthest_away <= 0 ) return;
					const Scalar alpha = 0.918;
					const Scalar beta = 1.953;
					// Jensen's Gaussian gather filter (RISURPM 2001, eq. 7.7) reshapes the
					// per-photon weight but must preserve energy: its DISK AVERAGE has to be
					// 1, i.e. (1/(PI r^2)) * INT_0^r wpg(d) 2 PI d dd == 1.  With alpha=0.918,
					// beta=1.953 that integral is only ~0.531 (Jensen normalizes his CONE
					// filter by the analogous 1-2/(3k) factor; the Gaussian needs its own),
					// so dividing by PI r^2 alone biased the radiance LOW by ~1.88x — the
					// caustic map rendered refractive caustics at ~0.50x an unbiased BDPT
					// reference.  Divide by the closed-form disk integral (substitute
					// t = d^2/(2 r^2)) so the filter only shapes the kernel, never rescales
					// its energy:  N = 2 alpha [ (1-1/D)/2 + (1-e^(-beta/2))/(beta D) ],
					// D = 1 - e^(-beta)  (evaluates to ~0.5311).
					const Scalar gaussD = 1.0 - exp(-beta);
					const Scalar gaussNorm = 2.0*alpha*( 0.5*(1.0 - 1.0/gaussD) + (1.0 - exp(-beta*0.5))/(beta*gaussD) );
					const Scalar invArea = 1.0 / (PI * farthest_away * gaussNorm);
					const Scalar maxNDist = farthest_away * this->dEllipseRatio;

					// Sum irradiance from all photons
					typename PhotonDistListType::const_iterator i, e;
					for( i=heap.begin(), e=heap.end(); i!=e; i++ )
					{
						const PhotType& p = (*i).element;
						const Vector3 vPhotonDir = p.incomingDirection;
						const Scalar response = PathVertexEval::RadianceShadingNormalFactor(
							ri.vNormal, ri.vGeomNormal, vPhotonDir );

						// Material evaluation owns reflection/transmission support.
						if( response > 0 ) {
							const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
							// Thin-surface "ellipsoid" clamp: rejects
							// photons stored on the OTHER side of a thin
							// wall.  This is a same-physical-surface test
							// (Jensen 2001 §6.1) and must use the
							// GEOMETRIC normal — bumpy walls otherwise
							// either falsely reject same-face photons
							// (dark splotches) or admit photons through
							// the back side (light leak).
							const Scalar pcos = Vector3Ops::Dot( vec, ri.vGeomNormal );

							// Change the projection to an ellipse
							if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
								// Filter the samples using a gaussian filter as described in Jensen's course notes
								const Scalar wpg = alpha * ( 1.0 - ((1-exp(-beta * (i->distance/(2.0*farthest_away))))/(1-exp(-beta))));
								rad = rad + (p.power * (wpg * response) * brdf.value( vPhotonDir, ri ));
							}
						}
					}

					rad = rad * invArea;
				}
			}

		public:

			// Stores the given photon with direction
			bool Store( const RISEPel& power, const Point3& pos, const Vector3& dir )
			{
				if( this->vphotons.size() >= this->nMaxPhotons ) {
					return false;
				}

				if( ColorMath::MaxValue(power) <= 0 ) {
					return false;
				}

				PhotType p;

				p.ptPosition = pos;
				p.power = power;
				p.incomingDirection = dir;

				int theta = int( acos( dir.z ) * (256.0 / PI) );
				theta = theta > 255 ? 255 : theta;
				p.theta = (unsigned char)(theta);

				int phi = int( atan2( dir.y, dir.x ) * (256.0/TWO_PI) );
				phi = phi > 255 ? 255 : phi;
				phi = phi < 0 ? phi+256 : phi;

				p.phi = (unsigned char)(phi);

				this->bbox.Include( p.ptPosition );
				this->vphotons.push_back( p );
				this->maxPower = r_max( this->maxPower, ColorMath::MaxValue(power) );

				return true;
			}
		};
        class PhotonMapDirectionalSpectralHelper : public ISpectralPhotonMap,
            public PhotonMapDirectionalHelper<SpectralPhoton>
        {
        protected:
            SpectralPhotonSamplingLaw samplingLaw;
            PhotonMapDirectionalSpectralHelper(unsigned maximum,const IPhotonTracer* tracer)
                :PhotonMapDirectionalHelper<SpectralPhoton>(maximum,tracer){}
            void WriteSpectralMap(IWriteBuffer& b,unsigned kind,Scalar halfWidth)const {
                WriteExactPrefix(b,kind);b.ResizeForMore(static_cast<unsigned>(32+8*samplingLaw.representatives.size()));
                b.setDouble(samplingLaw.begin);b.setDouble(samplingLaw.end);b.setUInt(samplingLaw.count);b.setDouble(halfWidth);
                b.setUInt(static_cast<unsigned>(samplingLaw.representatives.size()));
                for(Scalar value:samplingLaw.representatives)b.setDouble(value);
                WriteExactBody(b);
            }
            bool ReadSpectralMap(IReadBuffer& b,unsigned kind,Scalar& halfWidth) {
                if(!ReadExactPrefix(b,kind))return false;
                if(ReadableBytes(b)<32)return PacketLoadError("truncated wavelength law");
                const Scalar a=b.getDouble(),end=b.getDouble();const unsigned n=b.getUInt();const Scalar width=b.getDouble();
                SpectralPhotonSamplingLaw law;
                // An unconfigured empty map is a valid serializable state. It
                // still cannot accept a packet until explicitly configured.
                if((n ? !law.Configure(a,end,n) : a!=0||end!=0)||!IsFiniteDouble(width)||width<0)return PacketLoadError("invalid wavelength law or kernel");
                const unsigned gridCount=b.getUInt();
                if(gridCount!=(n<10000?n:0)||gridCount>ReadableBytes(b)/8)return PacketLoadError("invalid or truncated represented wavelength grid");
                // The finite law is uniform over these exact representatives.
                // Retain them across compiler/platform rounding differences.
                for(unsigned i=0;i<gridCount;++i){
                    const Scalar value=b.getDouble();
                    if(!IsFiniteDouble(value)||value<a||value>=end||(i&&value<=law.representatives[i-1]))return PacketLoadError("invalid represented wavelength grid");
                    law.representatives[i]=value;
                }
                ExactPacketState state;if(!ReadExactBody(b,state))return false;
                if(!n&&!state.packets.empty())return PacketLoadError("unconfigured map contains packets");
                for(const auto& packet:state.packets)if(!law.Contains(packet.nm))return PacketLoadError("packet wavelength outside declared sampling law");
                CommitExactBody(state);samplingLaw=law;halfWidth=width;return true;
            }
            bool StoreSpectral(Scalar power,Scalar nm,const Point3& position,const Vector3& direction) {
                if(!samplingLaw.Contains(nm)){
                    GlobalLog()->PrintEasyError("SpectralPhotonMap::Store requires a configured sampling law and a wavelength in its support; no photon stored");return false;
                }
                if(vphotons.size()>=nMaxPhotons)return false;
                SpectralPhoton p;p.ptPosition=position;p.power=power;p.nm=nm;p.incomingDirection=direction;
                const int theta=int(acos(direction.z)*(256.0/PI));p.theta=static_cast<unsigned char>(theta>255?255:theta);
                int phi=int(atan2(direction.y,direction.x)*(256.0/TWO_PI));phi=phi>255?255:phi;p.phi=static_cast<unsigned char>(phi<0?phi+256:phi);
                bbox.Include(position);vphotons.push_back(p);maxPower=r_max(maxPower,power);return true;
            }
        public:
            bool ConfigureWavelengthSampling(Scalar begin,Scalar end,unsigned count) override {
                SpectralPhotonSamplingLaw next;
                if(!vphotons.empty()||!next.Configure(begin,end,count))return false;
                samplingLaw=next;return true;
            }
            bool GetWavelengthSampling(Scalar& begin,Scalar& end,unsigned& count)const override {
                if(!samplingLaw.Configured())return false;
                begin=samplingLaw.begin;end=samplingLaw.end;count=samplingLaw.count;return true;
            }
            Scalar SampleWavelength(Scalar u)const{return samplingLaw.Sample(u);}
        };

	}
}

#endif
