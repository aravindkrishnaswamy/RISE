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
#include <vector>
#include <algorithm>

namespace RISE
{
	//
	// This is implementation of common functions in photon maps (balancing code...)
	//
	namespace Implementation
	{
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

				bbox = BoundingBox( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );

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
				CountPhotonsAt( loc, maxDist, max, 0, vphotons.size()-1, cnt );
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

				if( sqrD2 > maxDist ) {
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


				Scalar md = maxDist;

				if( distanceToPhoton < md )
				{
					// We've found a photon!
					// Insert into candidate list
					heap.push_back( distance_container<PhotType>( vphotons[median], distanceToPhoton ));

					// Build the heap
					if( heap.size() == nPhotons-1 ) {
						std::make_heap( heap.begin(), heap.end() );
						md = heap[0].distance;
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

				if( sqrD2 > md ) {
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

				if( sqrD2 > maxDist ) {
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

				// Now sort
				switch( axis )
				{
				case 0:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to, less_than_X );
					break;
				case 1:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to, less_than_Y );
					break;
				case 2:
				default:
					std::nth_element( vphotons.begin()+from, vphotons.begin()+median, vphotons.begin()+to, less_than_Z );
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
				BalanceSegment( 0, vphotons.size()-1 );
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

			unsigned int NumStored( ){ return vphotons.size(); }
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

			// Finds the nearest photon facing the right direction
			void LocateNearestPhoton(
				const Point3&			loc,								// the location from which to search for photons
				const Vector3&			normal,								// the normal
				const Scalar			maxDist,							// the maximum radius to look for photons
				distance_container<PhotType>&		nearest					// the nearest photon
				) const
			{
				LocateNearestPhotonRecursive( loc, normal, maxDist, 0, this->vphotons.size()-1, nearest );
			}

			// Locate nearest photon recursive algorithm
			void LocateNearestPhotonRecursive(
				const Point3&			loc,								// the location from which to search for photons
				const Vector3&			normal,								// the normal
				const Scalar			maxDist,							// the maximum radius to look for photons
				const int				from,								// index to search from
				const int				to,									// index to search to
				distance_container<PhotType>&		nearest					// the nearest photon
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
				const Vector3 v = Vector3Ops::mkVector3( loc, this->vphotons[ median ].ptPosition );
				const Scalar distanceToPhoton = Vector3Ops::SquaredModulus(v);

				Scalar md = maxDist;

				if( distanceToPhoton < md &&
					distanceToPhoton < nearest.distance )
				{
					// Only accept if the photon's normal is similar to ours
					const Vector3 vPhotonDir = this->PhotonDir(this->vphotons[median].theta,this->vphotons[median].phi);
					const Scalar dirdiff = Vector3Ops::Dot( vPhotonDir, normal );
					if( dirdiff > 0 && dirdiff < 0.9) {
						md = distanceToPhoton;
						nearest = distance_container<PhotType>( this->vphotons[median], distanceToPhoton );
					}
				}

				const int axis = this->vphotons[median].plane;

				const Scalar distance2 = loc[axis] - this->vphotons[median].ptPosition[axis];
				const Scalar sqrD2 = distance2*distance2;

				if( sqrD2 > md ) {
					if( distance2 <= 0 ) {
						LocateNearestPhotonRecursive( loc, normal, md, from, median-1, nearest );
					} else {
						LocateNearestPhotonRecursive( loc, normal, md, median+1, to, nearest );
					}
				}

				// Search both sides of the tree
				if( sqrD2 < md ) {
					LocateNearestPhotonRecursive( loc, normal, md, from, median-1, nearest );
					LocateNearestPhotonRecursive( loc, normal, md, median+1, to, nearest );
				}
			}

		public:

			// scale = 1/number of emmitted photons
			void ScalePhotonPower( const Scalar scale )
			{
				for( unsigned int i=this->nPrevScale; i<this->vphotons.size(); i++ ) {
					this->vphotons[i].power = this->vphotons[i].power * scale;
				}

				this->nPrevScale = this->vphotons.size();

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
				this->LocatePhotons( point, this->dGatherRadius, this->nMaxPhotonsOnGather, heap, 0, this->vphotons.size()-1 );

				if( heap.size() > this->nMinPhotonsOnGather )
				{
					// They haven't been sorted yet, since the list isn't full
					if( heap.size() < this->nMaxPhotonsOnGather ) {
						std::make_heap( heap.begin(), heap.end() );
					}

					const Scalar farthest_away = heap[0].distance;

					typename PhotonDistListType::const_iterator i, e;

					for( i=heap.begin(), e=heap.end(); i!=e; i++ ) {
						const PhotType& p = i->element;

						const Scalar maxNDist = farthest_away * this->dEllipseRatio;
						const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, point );
						const Scalar pcos = Vector3Ops::Dot( vec, normal );

						if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
							const Vector3 vPhotonDir = this->PhotonDir(p.theta,p.phi);
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
				this->LocatePhotons( ri.ptIntersection, this->dGatherRadius, this->nMaxPhotonsOnGather, heap, 0, this->vphotons.size()-1 );

				if( heap.size() > this->nMinPhotonsOnGather )
				{
					// They haven't been sorted yet, since the list isn't full
					if( heap.size() < this->nMaxPhotonsOnGather ) {
						std::make_heap( heap.begin(), heap.end() );
					}

					const Scalar farthest_away = heap[0].distance;
					const Scalar alpha = 0.918;
					const Scalar beta = 1.953;
					const Scalar invArea = 1.0 / (PI * farthest_away);
					const Scalar maxNDist = farthest_away * this->dEllipseRatio;

					// Sum irradiance from all photons
					typename PhotonDistListType::const_iterator i, e;
					for( i=heap.begin(), e=heap.end(); i!=e; i++ )
					{
						const PhotType& p = (*i).element;
						const Vector3 vPhotonDir = this->PhotonDir( p.theta, p.phi );
						const Scalar cos = Vector3Ops::Dot( vPhotonDir, ri.vNormal );

						if( cos > 0.001 ) {
							const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
							const Scalar pcos = Vector3Ops::Dot( vec, ri.vNormal );

							// Change the projection to an ellipse
							if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
								// Filter the samples using a gaussian filter as described in Jensen's course notes
								const Scalar wpg = alpha * ( 1.0 - ((1-exp(-beta * (i->distance/(2.0*farthest_away))))/(1-exp(-beta))));
								rad = rad + (p.power * wpg * brdf.value( vPhotonDir, ri ));
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
	}
}

#endif
