//////////////////////////////////////////////////////////////////////
//
//  IORStack.h - Optical-IOR and enclosure state for transport
//
//  The optical stack drives Snell/Fresnel/TIR decisions; the enclosure
//  stack drives innermost-exclusive medium lookup.  Ordinary dielectric
//  push/pop updates both; enclosure-only boundaries never enter the
//  optical stack.
//
//  Known limitation (IObject*-pointer keying):
//    Works for closed watertight meshes, nested different-material
//    volumes, and disjoint or concentric same-material volumes (see
//    tests/IORStackTest.cpp).  Fails if a single volume is modeled as
//    multiple distinct IObject*s of the same material ("slab-from-planes")
//    and the ray hits another refractor downstream (each plane pushes an
//    entry, polluting the outer IOR).  Use a closed mesh or CSG wrapper
//    instead; single-slab cases are covered by Optics::CalculateRefractedRay.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 25, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOR_STACK_
#define IOR_STACK_

// Out-of-line marker for IORStack's context slow paths (DL-622 review).
#if defined(_MSC_VER)
	#define RISE_IORSTACK_COLD __declspec(noinline)
#else
	#define RISE_IORSTACK_COLD __attribute__((noinline))
#endif


#include "../Interfaces/IReference.h"
#include "../Interfaces/IObject.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace RISE
{
	class IScene;

	class IORStack
	{
	public:
		//! Open-sheet crossing payload (material + geometry) stored out-of-line
		//! in sparse copy-on-write `MyIORStack::ctxs` so wavelength re-indexing
		//! (`IORStackSeeding::ReindexStackForWavelength`, SMS, fallback refresh)
		//! can re-evaluate sheet IOR without re-probing.  Closed-medium pushes
		//! attach none (`IORDATA` stays trivially copyable; closed entries probe
		//! via `ProbeObjectSurfaceHit`), and stack copies only bump the refcount.
		struct EntryContext
		{
			const IMaterial* pMaterial;
			Point3           ptIntersection;
			Point3           ptObjIntersec;
			Point2           ptCoord;
			Vector3          vNormal;
			Vector3          vGeomNormal;
			Vector3          rayDir;
			RasterizerState  rast;
			bool             bHasContext;
			bool             bRepushParentIor;
			bool             bGeomNormalOrientedToRay;
			bool             bGeomNormalRayDerived;
			bool             bProvablyNoInterior;
			bool             bOpenSheet;

			EntryContext() :
			  pMaterial( 0 ),
			  ptIntersection( 0, 0, 0 ),
			  ptObjIntersec( 0, 0, 0 ),
			  ptCoord( 0, 0 ),
			  vNormal( 0, 1, 0 ),
			  vGeomNormal( 0, 1, 0 ),
			  rayDir( 0, -1, 0 ),
			  rast( nullRasterizerState ),
			  bHasContext( false ),
			  bRepushParentIor( false ),
			  bGeomNormalOrientedToRay( false ),
			  bGeomNormalRayDerived( false ),
			  bProvablyNoInterior( false ),
			  bOpenSheet( false )
			{}

			static EntryContext FromGeometric(
				const RayIntersectionGeometric& rig,
				const IMaterial* pMat,
				const bool bRepushParent = false
				)
			{
				EntryContext ctx;
				ctx.pMaterial                = pMat;
				ctx.ptIntersection           = rig.ptIntersection;
				ctx.ptObjIntersec            = rig.ptObjIntersec;
				ctx.ptCoord                  = rig.ptCoord;
				ctx.vNormal                  = rig.vNormal;
				ctx.vGeomNormal              = rig.vGeomNormal;
				ctx.rayDir                   = rig.ray.Dir();
				ctx.rast                     = rig.rast;
				ctx.bHasContext              = true;
				ctx.bRepushParentIor         = bRepushParent;
				ctx.bGeomNormalOrientedToRay = rig.bGeomNormalOrientedToRay;
				ctx.bGeomNormalRayDerived    = rig.bGeomNormalRayDerived;
				ctx.bProvablyNoInterior      = rig.bProvablyNoInterior;
				ctx.bOpenSheet               = rig.bOpenSheet;
				return ctx;
			}

			void PopulateGeometric( RayIntersectionGeometric& rig ) const
			{
				rig.bHit                     = true;
				rig.ptIntersection           = ptIntersection;
				rig.ptObjIntersec            = ptObjIntersec;
				rig.ptCoord                  = ptCoord;
				rig.vNormal                  = vNormal;
				rig.vGeomNormal              = vGeomNormal;
				rig.ray                      = Ray( ptIntersection, rayDir );
				rig.rast                     = rast;
				rig.bGeomNormalOrientedToRay = bGeomNormalOrientedToRay;
				rig.bGeomNormalRayDerived    = bGeomNormalRayDerived;
				rig.bProvablyNoInterior      = bProvablyNoInterior;
				rig.bOpenSheet               = bOpenSheet;
			}
		};

	protected:
		struct IORDATA
		{
			const IObject* pObj;
			Scalar ior;
			bool bOpenSheet;

			IORDATA() :
			pObj( 0 ),
			ior( 0 ),
			bOpenSheet( false )
			{}

			IORDATA(
				const IObject* pObj_,
				Scalar ior_,
				bool bOpenSheet_ = false
				) : 
			pObj( pObj_ ),
			ior( ior_ ),
			bOpenSheet( bOpenSheet_ )
			{}
		};

		//! Inline small-buffer stack (`kInlineCap = 4`: environment + 3 media)
		//! avoiding heap allocation on typical paths; deeper stacks spill to
		//! `m_heap` via `RISE_IORSTACK_COLD` helpers.  All mutations go through
		//! `pushEntry` / `find_and_destroy` to keep `ContextList` indices aligned.
		class MyIORStack
		{
		public:

			static const std::size_t kInlineCap = 4;
			typedef std::pair<std::size_t, EntryContext> IndexedContext;

			//! Sparse, copy-on-write `(stackIndex, EntryContext)` list sorted by
			//! index; shared across copies via intrusive refcount.
			struct ContextList
			{
				std::atomic<int> refs;
				std::vector<IndexedContext> items;
				ContextList() : refs( 1 ) {}
			};

		private:

			IORDATA m_inline[kInlineCap];
			std::size_t m_size;
			std::vector<IORDATA>* m_heap;

			//! Invariant: when `m_size <= kInlineCap`, live entries are in
			//! `m_inline[0..m_size-1]` (`m_heap` null or empty spare buffer);
			//! otherwise `(*m_heap)[0..m_size-1]` holds all live entries.
			inline IORDATA* entries() { return ( m_size > kInlineCap ) ? m_heap->data() : m_inline; }
			inline const IORDATA* entries() const { return ( m_size > kInlineCap ) ? m_heap->data() : m_inline; }

			RISE_IORSTACK_COLD static std::vector<IORDATA>* CloneHeap( const std::vector<IORDATA>& src )
			{
				return new std::vector<IORDATA>( src );
			}

			RISE_IORSTACK_COLD static void FreeHeap( std::vector<IORDATA>* p )
			{
				delete p;
			}

			RISE_IORSTACK_COLD void AssignHeap( const std::vector<IORDATA>& src )
			{
				if( m_heap ) {
					*m_heap = src;
				} else {
					m_heap = new std::vector<IORDATA>( src );
				}
				m_size = m_heap->size();
			}

			RISE_IORSTACK_COLD void SpillAndPush( const IORDATA& d )
			{
				if( m_size <= kInlineCap ) {
					if( !m_heap ) {
						std::unique_ptr<std::vector<IORDATA>> heap( new std::vector<IORDATA>() );
						heap->reserve( kInlineCap * 2 );
						for( std::size_t i = 0; i < m_size; ++i ) {
							heap->push_back( m_inline[i] );
						}
						heap->push_back( d );
						m_size = heap->size();
						m_heap = heap.release();
						return;
					}
					m_heap->clear();
					for( std::size_t i = 0; i < m_size; ++i ) {
						m_heap->push_back( m_inline[i] );
					}
				}
				m_heap->push_back( d );
				m_size = m_heap->size();
			}

			RISE_IORSTACK_COLD void EraseHeapAt( const std::size_t idx )
			{
				m_heap->erase( m_heap->begin() + static_cast<std::ptrdiff_t>( idx ) );
				const std::size_t newSize = m_heap->size();
				if( newSize <= kInlineCap ) {
					const IORDATA* src = m_heap->data();
					for( std::size_t i = 0; i < newSize; ++i ) {
						m_inline[i] = src[i];
					}
					m_heap->clear();
				}
				m_size = newSize;
			}

		public:

			//! Null when no entry carries a context; all context management is
			//! kept out-of-line (`RISE_IORSTACK_COLD`) to keep the hot path small.
			ContextList* ctxs;

			MyIORStack() : m_size( 0 ), m_heap( 0 ), ctxs( 0 ) {}
			MyIORStack( const MyIORStack& s ) :
			  m_size( s.m_size ),
			  m_heap( 0 ),
			  ctxs( s.ctxs )
			{
				if( m_size <= kInlineCap ) {
					for( std::size_t i = 0; i < m_size; ++i ) {
						m_inline[i] = s.m_inline[i];
					}
				} else {
					m_heap = CloneHeap( *s.m_heap );
				}
				if( ctxs ) {
					Retain( ctxs );
				}
			}
			MyIORStack( MyIORStack&& s ) noexcept :
			  m_size( s.m_size ),
			  m_heap( 0 ),
			  ctxs( s.ctxs )
			{
				if( m_size <= kInlineCap ) {
					for( std::size_t i = 0; i < m_size; ++i ) {
						m_inline[i] = s.m_inline[i];
					}
					if( s.m_size > 0 ) {
						s.m_size = 1;
					}
				} else {
					const IORDATA env = ( *s.m_heap )[0];
					m_heap = s.m_heap;
					s.m_heap = 0;
					s.m_inline[0] = env;
					s.m_size = 1;
				}
				s.ctxs = 0;
			}
			MyIORStack& operator=( const MyIORStack& s )
			{
				if( this == &s ) {
					return *this;
				}
				if( s.m_size <= kInlineCap ) {
					if( m_heap ) {
						m_heap->clear();
					}
					for( std::size_t i = 0; i < s.m_size; ++i ) {
						m_inline[i] = s.m_inline[i];
					}
					m_size = s.m_size;
				} else {
					AssignHeap( *s.m_heap );
				}
				if( ctxs != s.ctxs ) {
					AssignContexts( s.ctxs );
				}
				return *this;
			}
			MyIORStack& operator=( MyIORStack&& s ) noexcept
			{
				if( this == &s ) {
					return *this;
				}
				if( s.m_size <= kInlineCap ) {
					if( m_heap ) {
						m_heap->clear();
					}
					for( std::size_t i = 0; i < s.m_size; ++i ) {
						m_inline[i] = s.m_inline[i];
					}
					m_size = s.m_size;
					if( s.m_size > 0 ) {
						s.m_size = 1;
					}
				} else {
					const IORDATA env = ( *s.m_heap )[0];
					if( m_heap ) {
						FreeHeap( m_heap );
					}
					m_heap = s.m_heap;
					m_size = m_heap->size();
					s.m_heap = 0;
					s.m_inline[0] = env;
					s.m_size = 1;
				}
				if( ctxs ) {
					Release( ctxs );
				}
				ctxs = s.ctxs;
				s.ctxs = 0;
				return *this;
			}
			~MyIORStack()
			{
				if( m_heap ) {
					FreeHeap( m_heap );
				}
				if( ctxs ) {
					Release( ctxs );
				}
			}

			inline IORDATA& top() { return entries()[m_size - 1]; }
			inline const IORDATA& top() const { return entries()[m_size - 1]; }
			inline std::size_t size() const { return m_size; }
			inline bool empty() const { return m_size == 0; }

			static const EntryContext& NoContext()
			{
				static const EntryContext none;
				return none;
			}

			RISE_IORSTACK_COLD static void Retain( ContextList* p )
			{
				p->refs.fetch_add( 1, std::memory_order_relaxed );
			}

			RISE_IORSTACK_COLD static void Release( ContextList* p )
			{
				if( p->refs.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
					delete p;
				}
			}

			RISE_IORSTACK_COLD void AssignContexts( ContextList* p )
			{
				if( p ) {
					Retain( p );
				}
				if( ctxs ) {
					Release( ctxs );
				}
				ctxs = p;
			}

			//! Replaces the context list by @a items (empty -> null).
			RISE_IORSTACK_COLD void ReplaceContexts( std::vector<IndexedContext>& items )
			{
				ContextList* next = 0;
				if( !items.empty() ) {
					next = new ContextList();
					next->items.swap( items );
				}
				if( ctxs ) {
					Release( ctxs );
				}
				ctxs = next;
			}

			//! Attaches @a ctx to the TOP entry (already pushed, so a failed
			//! allocation here leaves the entry without a context, never a
			//! context indexing an entry that does not exist).
			RISE_IORSTACK_COLD void AppendContext( const EntryContext& ctx )
			{
				std::vector<IndexedContext> items;
				if( ctxs ) {
					items = ctxs->items;
				}
				items.push_back( IndexedContext( m_size - 1, ctx ) );
				ReplaceContexts( items );
			}

			//! Drops the context at @a idx (if any) and shifts those above it.
			RISE_IORSTACK_COLD void EraseContextIndex( const std::size_t idx )
			{
				if( ctxs->items.empty() || ctxs->items.back().first < idx ) {
					return;
				}
				std::vector<IndexedContext> items;
				for( std::size_t k = 0; k < ctxs->items.size(); ++k ) {
					const IndexedContext& ec = ctxs->items[k];
					if( ec.first < idx ) items.push_back( ec );
					else if( ec.first > idx ) items.push_back( IndexedContext( ec.first - 1, ec.second ) );
				}
				ReplaceContexts( items );
			}

			void pushEntry( const IORDATA& d, const EntryContext* pCtx )
			{
				if( m_size < kInlineCap ) {
					m_inline[m_size++] = d;
				} else {
					SpillAndPush( d );
				}
				if( pCtx && pCtx->bHasContext ) {
					AppendContext( *pCtx );
				}
			}

			RISE_IORSTACK_COLD void insertEntryAt( const std::size_t idx, const IORDATA& d, const EntryContext* pCtx )
			{
				const std::size_t pos = idx < m_size ? idx : m_size;
				if( pos == m_size ) {
					pushEntry( d, pCtx );
					return;
				}
				if( m_size < kInlineCap ) {
					for( std::size_t j = m_size; j > pos; --j ) {
						m_inline[j] = m_inline[j - 1];
					}
					m_inline[pos] = d;
					++m_size;
				} else if( m_size == kInlineCap ) {
					SpillAndPush( m_inline[kInlineCap - 1] );
					for( std::size_t j = kInlineCap - 1; j > pos; --j ) {
						( *m_heap )[j] = ( *m_heap )[j - 1];
					}
					( *m_heap )[pos] = d;
				} else {
					m_heap->insert( m_heap->begin() + static_cast<std::ptrdiff_t>( pos ), d );
					m_size = m_heap->size();
				}
				const bool attach = pCtx && pCtx->bHasContext;
				if( ctxs || attach ) {
					std::vector<IndexedContext> items;
					bool placed = false;
					if( ctxs ) {
						for( std::size_t k = 0; k < ctxs->items.size(); ++k ) {
							const IndexedContext& ec = ctxs->items[k];
							if( !placed && ec.first >= pos ) {
								if( attach ) items.push_back( IndexedContext( pos, *pCtx ) );
								placed = true;
							}
							items.push_back( IndexedContext( ec.first >= pos ? ec.first + 1 : ec.first, ec.second ) );
						}
					}
					if( !placed && attach ) {
						items.push_back( IndexedContext( pos, *pCtx ) );
					}
					ReplaceContexts( items );
				}
			}

            std::vector<const IObject*> ObjectKeys() const {
                std::vector<const IObject*> result;
                const IORDATA* p = entries();
                for( std::size_t i = 0; i < m_size; ++i )
                    if( p[i].pObj ) result.push_back( p[i].pObj );
                return result;
            }
            Scalar EnvironmentIOR() const { return entries()[0].ior; }
            std::size_t depth() const { return m_size; }
            const IObject* objectAt( std::size_t i ) const { return entries()[i].pObj; }
            Scalar iorAt( std::size_t i ) const { return entries()[i].ior; }
            void setIORAt( std::size_t i, Scalar ior ) { entries()[i].ior = ior; }
            bool topIsOpenSheet() const { return m_size == 0 ? false : entries()[m_size - 1].bOpenSheet; }
            bool isOpenSheetAt( std::size_t i ) const { return i < m_size ? entries()[i].bOpenSheet : false; }
            const EntryContext& contextAt( std::size_t i ) const {
                if( ctxs ) {
                    for( std::size_t k = 0; k < ctxs->items.size(); ++k ) {
                        if( ctxs->items[k].first == i ) return ctxs->items[k].second;
                    }
                }
                return NoContext();
            }
            RISE_IORSTACK_COLD void setContextAt( std::size_t i, const EntryContext& ctx ) {
                if( i >= m_size ) return;
                if( !ctxs && !ctx.bHasContext ) return;
                std::vector<IndexedContext> items;
                bool placed = false;
                if( ctxs ) {
                    for( std::size_t k = 0; k < ctxs->items.size(); ++k ) {
                        const IndexedContext& e = ctxs->items[k];
                        if( !placed && e.first >= i ) {
                            if( ctx.bHasContext ) items.push_back( IndexedContext( i, ctx ) );
                            placed = true;
                            if( e.first == i ) continue;
                        }
                        items.push_back( e );
                    }
                }
                if( !placed && ctx.bHasContext ) items.push_back( IndexedContext( i, ctx ) );
                ReplaceContexts( items );
            }
            const EntryContext& topContext() const { return contextAt( m_size - 1 ); }
            const EntryContext* findContext( const IObject* r ) const {
                if( !ctxs ) return 0;
                const IORDATA* p = entries();
                for( std::size_t k = ctxs->items.size(); k-- > 0; ) {
                    const IndexedContext& e = ctxs->items[k];
                    if( e.first < m_size && p[e.first].pObj == r ) {
                        return &e.second;
                    }
                }
                return 0;
            }
            bool SameInterfaces(const MyIORStack& other) const {
                if(m_size != other.m_size) return false;
                const IORDATA* a = entries();
                const IORDATA* b = other.entries();
                for(std::size_t i=0; i<m_size; ++i)
                    if(a[i].pObj != b[i].pObj || a[i].ior != b[i].ior || a[i].bOpenSheet != b[i].bOpenSheet) return false;
                return true;
            }

			bool find_and_destroy( const IObject* r )
			{
				IORDATA* p = entries();
				for( std::size_t i = m_size; i-- > 0; ) {
					if( p[i].pObj == r ) {
						if( ctxs ) {
							EraseContextIndex( i );
						}
						if( m_size > kInlineCap ) {
							EraseHeapAt( i );
						} else {
							for( std::size_t j = i + 1; j < m_size; ++j ) {
								m_inline[j - 1] = m_inline[j];
							}
							--m_size;
						}
						return true;
					}
				}

				return false;
			}

			bool containsObject( const IObject* r ) const
			{
				const IORDATA* p = entries();
				for( std::size_t i = m_size; i-- > 0; ) {
					if( p[i].pObj == r ) {
						return true;
					}
				}
				return false;
			}

			//! Fire r40: true when this stack's object keys (environment
			//! excluded) appear, in order, within @a enclosing.
			bool isObjectSubsequenceOf( const std::vector<const IObject*>& enclosing ) const
			{
				const IORDATA* p = entries();
				std::vector<const IObject*>::const_iterator next = enclosing.begin();
				for( std::size_t i = 0; i < m_size; ++i ) {
					if( !p[i].pObj ) continue;
					while( next != enclosing.end() && *next != p[i].pObj ) ++next;
					if( next == enclosing.end() ) return false;
					++next;
				}
				return true;
			}

			std::size_t objectCount() const
			{
				const IORDATA* p = entries();
				std::size_t n = 0;
				for( std::size_t i = 0; i < m_size; ++i ) {
					if( p[i].pObj ) ++n;
				}
				return n;
			}
		};

		MyIORStack iorstack;

		//! Separate medium-enclosure list (outermost first).  Null means
		//! identical to the optical stack's object keys (heap-free fast path).
		//! Materialized on `pushEnclosure()`, kept in lockstep by optical ops,
		//! and released when equal again.  Invariant: optical object keys are
		//! an ordered subsequence of `*pEnclosure`.
		std::vector<const IObject*>* pEnclosure;

		//! Object about to be shaded (used by `containsCurrent`/`push`/`pop`).
		//! Intentionally non-`mutable` so nested ray casts cannot clobber a
		//! caller's `const IORStack&`; callers changing it must copy first.
		const IObject* pCurrentObject;

		//! Exterior IOR the crossing refracted FROM when different from
		//! `before.top()` (0 = use `before.top()`).  Set by open-sheet crossing
		//! resolution on the scattered stack and read by `RadianceEtaScale`.
		//! Not copied by copy ctor/assignment; transferred (and zeroed on
		//! source) by move ctor/assignment.
		Scalar crossingEtaFrom;
		const IScene* pProbeScene;

		RISE_IORSTACK_COLD static std::vector<const IObject*>* CloneEnclosure( const std::vector<const IObject*>* p )
		{
			return p ? new std::vector<const IObject*>( *p ) : 0;
		}

		RISE_IORSTACK_COLD static void FreeEnclosure( std::vector<const IObject*>* p )
		{
			delete p;
		}

		RISE_IORSTACK_COLD void AssignEnclosure( const std::vector<const IObject*>* p )
		{
			if( p ) {
				if( pEnclosure ) {
					*pEnclosure = *p;
				} else {
					pEnclosure = new std::vector<const IObject*>( *p );
				}
			} else if( pEnclosure ) {
				FreeEnclosure( pEnclosure );
				pEnclosure = 0;
			}
		}

		//! Builds the explicit enclosure list from the optical keys.
		RISE_IORSTACK_COLD void MaterializeEnclosure()
		{
			if( !pEnclosure ) {
				pEnclosure = new std::vector<const IObject*>( iorstack.ObjectKeys() );
			}
		}

		RISE_IORSTACK_COLD static bool EnclosureContains( const std::vector<const IObject*>& e, const IObject* pObj )
		{
			for( std::size_t i = e.size(); i-- > 0; ) {
				if( e[i] == pObj ) return true;
			}
			return false;
		}

		//! Removes the innermost occurrence of @a pObj.
		RISE_IORSTACK_COLD static bool EnclosureErase( std::vector<const IObject*>& e, const IObject* pObj )
		{
			for( std::size_t i = e.size(); i-- > 0; ) {
				if( e[i] == pObj ) {
					e.erase( e.begin() + static_cast<std::ptrdiff_t>( i ) );
					return true;
				}
			}
			return false;
		}

		RISE_IORSTACK_COLD void EnclosurePushBack( const IObject* pObj )
		{
			pEnclosure->push_back( pObj );
		}

		//! Optical entry @a opticalIdx was just inserted (InsertOpenSheetAt):
		//! place it in the enclosure list immediately before the enclosure
		//! position of the optical entry now above it, or on top when none is.
		RISE_IORSTACK_COLD void EnclosureInsertForOptical( const std::size_t opticalIdx, const IObject* pObj )
		{
			std::vector<const IObject*>& e = *pEnclosure;
			std::size_t cursor = 0;
			for( std::size_t i = 0; i < iorstack.depth(); ++i ) {
				const IObject* pKey = iorstack.objectAt( i );
				if( i == opticalIdx || !pKey ) {
					continue;
				}
				while( cursor < e.size() && e[cursor] != pKey ) ++cursor;
				if( cursor >= e.size() ) {
					break;
				}
				if( i > opticalIdx ) {
					e.insert( e.begin() + static_cast<std::ptrdiff_t>( cursor ), pObj );
					return;
				}
				++cursor;
			}
			e.push_back( pObj );
		}

		//! Drops the explicit enclosure list once it again equals the optical keys.
		RISE_IORSTACK_COLD void MaybeReleaseEnclosure()
		{
			if( pEnclosure && pEnclosure->size() == iorstack.objectCount() &&
				iorstack.isObjectSubsequenceOf( *pEnclosure ) ) {
				FreeEnclosure( pEnclosure );
				pEnclosure = 0;
			}
		}

		inline void AssertStackInvariant() const
		{
#ifndef NDEBUG
			assert( !pEnclosure || iorstack.isObjectSubsequenceOf( *pEnclosure ) );
#endif
		}

	public:
		// Explicit to prevent bare `0` implicitly constructing IORStack(0).
		explicit IORStack( const Scalar ior ) :
		  pEnclosure( 0 ),
		  pCurrentObject( 0 ),
		  crossingEtaFrom( 0 ),
		  pProbeScene( 0 )
		{
			// An empty IOR stack always has the environment's IOR
			iorstack.pushEntry( IORDATA(0,ior,false), 0 );
		}

		IORStack( const IORStack& s ) :
		  iorstack( s.iorstack ),
		  pEnclosure( s.pEnclosure ? CloneEnclosure( s.pEnclosure ) : 0 ),
		  pCurrentObject( s.pCurrentObject ),
		  crossingEtaFrom( 0 ),
		  pProbeScene( s.pProbeScene )
		{
			AssertStackInvariant();
		}

		IORStack( IORStack&& s ) noexcept :
		  iorstack( std::move( s.iorstack ) ),
		  pEnclosure( s.pEnclosure ),
		  pCurrentObject( s.pCurrentObject ),
		  crossingEtaFrom( s.crossingEtaFrom ),
		  pProbeScene( s.pProbeScene )
		{
			// The moved-from optical stack keeps only its environment entry,
			// whose enclosure state is the implicit (null) one.
			s.pEnclosure = 0;
			s.pCurrentObject = 0;
			s.crossingEtaFrom = 0;
			s.pProbeScene = 0;
		}

		IORStack& operator=( const IORStack& s )
		{
			if( this == &s ) {
				return *this;
			}
			iorstack = s.iorstack;
			if( pEnclosure || s.pEnclosure ) {
				AssignEnclosure( s.pEnclosure );
			}
			pCurrentObject = s.pCurrentObject;
			crossingEtaFrom = 0;
			pProbeScene = s.pProbeScene;
			AssertStackInvariant();
			return *this;
		}

		IORStack& operator=( IORStack&& s ) noexcept
		{
			if( this == &s ) {
				return *this;
			}
			iorstack = std::move( s.iorstack );
			if( pEnclosure ) {
				FreeEnclosure( pEnclosure );
			}
			pEnclosure = s.pEnclosure;
			pCurrentObject = s.pCurrentObject;
			crossingEtaFrom = s.crossingEtaFrom;
			pProbeScene = s.pProbeScene;
			s.pEnclosure = 0;
			s.pCurrentObject = 0;
			s.crossingEtaFrom = 0;
			s.pProbeScene = 0;
			return *this;
		}

		//! DL-382: the scene used for medium containment probes.
		inline void SetMediumProbeScene( const IScene* scene ) { pProbeScene = scene; }
		inline const IScene* MediumProbeScene() const { return pProbeScene; }

		//! DL-345: see `crossingEtaFrom`.
		inline void SetCrossingEtaFrom( const Scalar eta ) { crossingEtaFrom = eta; }
		inline Scalar CrossingEtaFrom() const { return crossingEtaFrom; }

		~IORStack()
		{
			if( pEnclosure ) {
				FreeEnclosure( pEnclosure );
			}
		}

		// Enter an optical boundary.  Dielectric transport updates both the
		// optical and enclosure state in one operation.
		inline void push( const Scalar ior )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::push Asked to push item onto stack with no object" );
			} else {
				iorstack.pushEntry( IORDATA(pCurrentObject,ior,false), 0 );
				if( pEnclosure ) {
					EnclosurePushBack( pCurrentObject );
				}
				AssertStackInvariant();
			}
		}

		inline void push( const Scalar ior, const EntryContext& ctx )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::push Asked to push item onto stack with no object" );
			} else {
				iorstack.pushEntry( IORDATA(pCurrentObject,ior,false), &ctx );
				if( pEnclosure ) {
					EnclosurePushBack( pCurrentObject );
				}
				AssertStackInvariant();
			}
		}

		//! DL-382: pushes an entry marked as entered through an open sheet face.
		inline void pushOpenSheet( const Scalar ior )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pushOpenSheet Asked to push item onto stack with no object" );
			} else {
				iorstack.pushEntry( IORDATA(pCurrentObject,ior,true), 0 );
				if( pEnclosure ) {
					EnclosurePushBack( pCurrentObject );
				}
				AssertStackInvariant();
			}
		}

		inline void pushOpenSheet( const Scalar ior, const EntryContext& ctx )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pushOpenSheet Asked to push item onto stack with no object" );
			} else {
				iorstack.pushEntry( IORDATA(pCurrentObject,ior,true), &ctx );
				if( pEnclosure ) {
					EnclosurePushBack( pCurrentObject );
				}
				AssertStackInvariant();
			}
		}

		inline bool topIsOpenSheet() const { return iorstack.topIsOpenSheet(); }
		inline bool isOpenSheetAt( const std::size_t i ) const { return iorstack.isOpenSheetAt( i ); }

		// Exit an optical boundary.  The matching enclosure entry leaves with it.
		inline void pop()
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pop Asked to pop item onto stack but no object" );
			} else {
				// Don't pop the default air entry in the IOR stack
				if( iorstack.size() > 1 ) {
					if( !iorstack.containsObject( pCurrentObject ) ) {
						GlobalLog()->PrintEasyWarning( "IORStack::pop Failed to find object to pop IOR for" );
					} else if( pEnclosure && !EnclosureContains( *pEnclosure, pCurrentObject ) ) {
						GlobalLog()->PrintEasyWarning( "IORStack::pop Failed to find matching enclosure entry" );
					} else {
						iorstack.find_and_destroy( pCurrentObject );
						if( pEnclosure ) {
							EnclosureErase( *pEnclosure, pCurrentObject );
						}
						AssertStackInvariant();
					}
				} else {
					GlobalLog()->PrintEasyWarning( "IORStack::pop Trying to pop IOR stack with only global IOR in it, cannot allow." );
				}
			}
		}

		// Enter a boundary that changes only medium enclosure.  The ambient
		// optical IOR and every refraction decision remain unchanged.
		inline void pushEnclosure()
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pushEnclosure Asked to push item with no object" );
			} else {
				MaterializeEnclosure();
				EnclosurePushBack( pCurrentObject );
				AssertStackInvariant();
			}
		}

		// Exit an enclosure-only boundary.  Refuse to remove an object that is
		// also optical; such objects must use pop() so the two states stay paired.
		inline void popEnclosure()
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::popEnclosure Asked to pop item with no object" );
			} else if( iorstack.containsObject( pCurrentObject ) ) {
				GlobalLog()->PrintEasyWarning( "IORStack::popEnclosure Cannot remove an optical boundary" );
			} else if( !pEnclosure || !EnclosureErase( *pEnclosure, pCurrentObject ) ) {
				GlobalLog()->PrintEasyWarning( "IORStack::popEnclosure Failed to find object" );
			} else {
				MaybeReleaseEnclosure();
				AssertStackInvariant();
			}
		}

		// Returns the IOR of the top of the stack
		inline Scalar top() const
		{
			return iorstack.top().ior;
		}

        // Read-only identity snapshot, outermost first. Keys are opaque;
        // callers must resolve them against scene objects before dereferencing.
        inline std::vector<const IObject*> ObjectKeys() const { return iorstack.ObjectKeys(); }

        // Root index survives a wavelength-specific containment replay.
        inline Scalar EnvironmentIOR() const { return iorstack.EnvironmentIOR(); }
        inline bool SameInterfaces(const IORStack& other) const
        { return iorstack.SameInterfaces(other.iorstack) && SameEnclosure(other); }

		// Fire r40: equal medium-enclosure membership (both implicit, or the
		// same explicit list).  Optical membership is compared separately.
		inline bool SameEnclosure( const IORStack& other ) const
		{
			if( !pEnclosure && !other.pEnclosure ) {
				return true;
			}
			std::vector<const IObject*> a, b;
			AppendObjectStack( a );
			other.AppendObjectStack( b );
			return a == b;
		}


		// Authoritative "is ray inside current object" test for refraction
		// (more reliable than normal sign at grazing angles).  Keys on
		// IObject* — see file header for the slab-from-planes limitation.
		inline bool containsCurrent() const
		{
			if( !pCurrentObject ) {
				return false;
			}
			return iorstack.containsObject( pCurrentObject );
		}

		// Checks enclosure membership without changing the optical interpretation
		// of containsCurrent(), which remains the refraction entering/exiting test.
		inline bool containsCurrentEnclosure() const
		{
			if( !pCurrentObject ) {
				return false;
			}
			return pEnclosure ? EnclosureContains( *pEnclosure, pCurrentObject )
				: iorstack.containsObject( pCurrentObject );
		}

		// Updates the innermost medium's IOR for graded-index media
		// (GradedIndexMedium.h); never overwrites entry 0 (environment).
		inline void SetTopIOR( const Scalar ior )
		{
			if( iorstack.size() > 1 ) {
				iorstack.top().ior = ior;
			}
		}

		// Indexed access to stack entries, outermost first (entry 0 is the
		// environment and never rewritten); used by GradedIndexMedium::Advance
		// to keep enclosing graded-medium IORs current across nested exits.
		inline std::size_t Depth() const { return iorstack.depth(); }
		inline const IObject* ObjectAt( const std::size_t i ) const { return iorstack.objectAt( i ); }
		inline Scalar IORAt( const std::size_t i ) const { return iorstack.iorAt( i ); }
		inline void SetIORAt( const std::size_t i, const Scalar ior )
		{
			if( i > 0 && i < iorstack.depth() ) {
				iorstack.setIORAt( i, ior );
			}
		}
		inline const EntryContext& ContextAt( const std::size_t i ) const { return iorstack.contextAt( i ); }
		inline void SetContextAt( const std::size_t i, const EntryContext& ctx )
		{
			if( i > 0 && i < iorstack.depth() ) {
				iorstack.setContextAt( i, ctx );
			}
		}
		inline void InsertOpenSheetAt( const std::size_t i, const IObject* pObj, const Scalar ior, const EntryContext& ctx )
		{
			if( pObj && i > 0 ) {
				const std::size_t pos = i < iorstack.depth() ? i : iorstack.depth();
				iorstack.insertEntryAt( i, IORDATA( pObj, ior, true ), &ctx );
				if( pEnclosure ) {
					EnclosureInsertForOptical( pos, pObj );
				}
				AssertStackInvariant();
			}
		}
		inline const EntryContext& TopContext() const { return iorstack.topContext(); }
		inline const EntryContext* FindEntryContext( const IObject* pObj ) const
		{
			return pObj ? iorstack.findContext( pObj ) : 0;
		}

		// Returns the innermost enclosing object used for medium resolution
		// (the enclosure stack's top; fire r40).  Returns 0 for the
		// environment (root entry with no object).  Equals the optical
		// stack's top object unless an enclosure-only boundary is innermost.
		// Analogous to Cycles' volume stack top entry.
		inline const IObject* topObject() const
		{
			if( pEnclosure ) {
				return pEnclosure->empty() ? 0 : pEnclosure->back();
			}
			return iorstack.top().pObj;
		}

		// Returns the object at the top of the OPTICAL IOR stack -- the entry
		// `top()`, `topIsOpenSheet()` and `pop()` act on.  Code that pairs the
		// top object with those optical operations must use this, not
		// topObject(), so an enclosure-only boundary never stands in for it.
		inline const IObject* opticalTopObject() const
		{
			return iorstack.top().pObj;
		}

		// Appends enclosing objects from outermost to innermost.  Shadow-medium
		// walks use the complete ordering so exiting an inner medium restores
		// the next outer medium rather than incorrectly falling back to world.
		inline void AppendObjectStack( std::vector<const IObject*>& objects ) const
		{
			if( pEnclosure ) {
				objects.insert( objects.end(), pEnclosure->begin(), pEnclosure->end() );
			} else {
				for( std::size_t i = 0; i < iorstack.depth(); ++i ) {
					if( const IObject* pObj = iorstack.objectAt( i ) ) {
						objects.push_back( pObj );
					}
				}
			}
		}

		// Test/debug witness for the r40 invariant.  Internal mutators assert the
		// same property in non-NDEBUG builds.
		inline bool DebugOpticalStackIsEnclosureSubsequence() const
		{
			return !pEnclosure || iorstack.isObjectSubsequenceOf( *pEnclosure );
		}

		inline const IObject* currentObject() const
		{
			return pCurrentObject;
		}

		// Sets the current object.  Non-const since DL-315 -- see
		// pCurrentObject's doc.
		inline void SetCurrentObject( const IObject* pObj )
		{
			if( pObj ) {
				pCurrentObject = pObj;
			} else {
				GlobalLog()->PrintEasyWarning( "IORStack::SetCurrentObject Called with no object" );
			}
		}
	};

	//! Basic-radiance scale `(eta_before / eta_after)^2` for one interface
	//! crossing, RADIANCE mode only (`L / n^2` invariance; Veach 1997 §5.2).
	//! Apply on camera-rooted walks (PT, BDPT/VCM/MLT eye subpaths, shader-op
	//! chain, final gather); NEVER on importance/flux walks (light subpaths,
	//! photon tracers, SMS photon seeds).  Excluded from `ScatteredRay::kray`
	//! (`ISPF.h`) and applied here from the walk's before/after stacks.
	//!
	//! Reads `after->CrossingEtaFrom()` (if > 0, set for open sheets) or
	//! `before.top()`, and `after->top()`.  Edge cases:
	//!  - Overlapping/slab-from-planes solids can desync `top()` from SPF locals.
	//!  - Open sheets supply the actual refracted-from index via `CrossingEtaFrom()`.
	//!  - Graded-index media (`docs/DEBT_LEDGER.md`): `GradedIndexMedium::Advance`
	//!    applies the interior `(n_start/n_end)^2` factor and updates `before.top()`
	//!    to the local exit index.  Both halves are coupled — never change one
	//!    without the other (`RadianceEtaScaleGradedIndexTest`, `GradedIndexInteriorFactorTest`).
	//!
	//! @param before  walk's current IOR stack at the scattering vertex
	//! @param after   scattered ray's stack, or NULL if unchanged (note: internal
	//!                reflection may pass a copy with unchanged top IOR)
	//! @return        `(eta_before / eta_after)^2`, or exact 1 when equal / invalid
	inline Scalar RadianceEtaScale( const IORStack& before, const IORStack* after )
	{
		if( !after ) {
			return Scalar( 1 );
		}
		const Scalar etaBefore = after->CrossingEtaFrom() > Scalar( 0 )
			? after->CrossingEtaFrom() : before.top();
		const Scalar etaAfter  = after->top();
		// Exact 1 for equal IORs (avoids FP division on reflection/same-IOR)
		// and guards against authored non-positive IORs producing Inf/0.
		if( etaBefore == etaAfter || etaBefore <= Scalar( 0 ) || etaAfter <= Scalar( 0 ) ) {
			return Scalar( 1 );
		}
		const Scalar ratio = etaBefore / etaAfter;
		return ratio * ratio;
	}
}

#endif
