//////////////////////////////////////////////////////////////////////
//
//  GenericManager.h - Declaration of a generic manager, who is
//  capable of managing stuff.  Managers for materials, painters
//  and RayIntersectionModifiers use this guy
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January  18, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GENERIC_MANAGER_
#define GENERIC_MANAGER_

#include "../Interfaces/IManager.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RString.h"
#include <map>
#include <vector>

namespace RISE
{
	// D35 record-during-derive (slice 1, docs/agentic-redesign/21-stable-apply-and-resolver.md
	// §8): when DeriveToJob installs these sinks -- SETUP-TIME only, during its PASS-2 chunk
	// bracketing; NULL by default, so the normal path pays a single predictable branch and
	// nothing else -- GenericManager records each PRODUCED entity (AddItem) and each RESOLVED
	// entity (GetItem-found) by pointer.  The derive then builds the reference graph from the
	// engine's ACTUAL production+resolution, not a parallel heuristic, so the two cannot drift.
	// Keyed by entity POINTER: a multiple-inheritance sub-object pointer (the same painter seen
	// as IPainter via the colour manager and as IFunction2D via the func-2D manager) records
	// under each base, but both AddItems run in the same chunk's bracket, so both pointers map
	// to the one producer.  See Cst.cpp DeriveToJob.
	inline thread_local std::vector<const void*>* g_cstProductionSink = nullptr;
	inline thread_local std::vector<const void*>* g_cstResolutionSink = nullptr;

	template< class T >
	class GenericManager : public virtual Implementation::Reference, public virtual IManager<T>
	{
	protected:

		struct ITEMREFERENCES
		{
			IDeletedCallback<T>&	pFunc;
			IReference&				pCaller;

			ITEMREFERENCES( IDeletedCallback<T>& a, IReference& c ) :
			pFunc( a ), pCaller( c )
			{}

			ITEMREFERENCES( const ITEMREFERENCES& item ) :
			pFunc( item.pFunc ), pCaller( item.pCaller )
			{}

			ITEMREFERENCES& operator=( const ITEMREFERENCES& item )
			{
				pFunc = item.pFunc;
				pCaller = item.pCaller;
				return *this;
			}
		};

		typedef std::vector<ITEMREFERENCES>				ReferencesListType;
		typedef std::pair<T*,ReferencesListType>		ItemType;
		typedef std::map<String,ItemType>				ItemListType;

		ItemListType		items;

		// P1: parallel per-name registration serial, bumped on every AddItem, so a
		// remove+re-add under the same name yields a NEW serial (identity change).
		unsigned long long							m_nNextSerial;
		std::map<String,unsigned long long>			m_serials;

		virtual ~GenericManager( )
		{
			Shutdown();
		}

	public:
		GenericManager( )
		: m_nNextSerial( 0 )
		{}

		bool		AddItem( T* pItem, const char* szName )
		{
			if( !szName || !pItem ) {
				GlobalLog()->PrintSourceError( "GenericManager::AddItem:: name ptr bad or item ptr bad", __FILE__, __LINE__ );
				return false;
			}

			// Before adding make sure there already isn't an item of that name hanging around
			if( GetItem( szName ) ) {
				GlobalLog()->PrintEx( eLog_Error, "GenericManager::AddItem:: Item of same name \"%s\" already exists", szName );
				return false;
			}

			// Now add it
			pItem->addref();

			String vecName( szName );
			items[vecName] = std::pair<T*,ReferencesListType>( pItem, ReferencesListType() );
			m_serials[vecName] = ++m_nNextSerial;   // P1: identity serial
			if( g_cstProductionSink ) g_cstProductionSink->push_back( static_cast<const void*>( pItem ) );   // D35: PRODUCED entity
			return true;
		}

		bool		RemoveItem( const char* szName )
		{
			if( !szName ) {
				GlobalLog()->PrintSourceError( "GenericManager::RemoveItem:: name ptr bad", __FILE__, __LINE__ );
				return false;
			}

			String vecName( szName );
			typename ItemListType::iterator	elem = items.find( vecName );

			if( elem == items.end() ) {
				GlobalLog()->PrintEx( eLog_Warning, "GenericManager::RemoveItem:: '%s' not found", szName );
				return false;
			}

			typename ReferencesListType::iterator	m, n;
			for( m=elem->second.second.begin(), n=elem->second.second.end(); m!=n; m++ ) {
				ITEMREFERENCES&	itemcb = *m;
				itemcb.pFunc( *(elem->second.first) );
				itemcb.pCaller.release();
			}

			// Release the manager's own ref on the item.  AddItem
			// addref'd; without this matching release the item leaks
			// every time RemoveItem succeeds.  Pre-existed across all
			// managers but only mattered once cameras started being
			// removed at runtime; the other managers are essentially
			// load-once / shutdown-once in practice and Shutdown's
			// safe_release covers them on teardown.
			safe_release( elem->second.first );
			items.erase( elem );
			m_serials.erase( vecName );   // P1: drop identity serial (a re-add gets a fresh one)
			return true;
		}

		T*			GetItem( const char* szName ) const
		{
			if( !szName ) {
				GlobalLog()->PrintSourceError( "GenericManager::GetItem:: name ptr bad", __FILE__, __LINE__ );
				return 0;
			}

			String vecName( szName );
			typename ItemListType::const_iterator	elem = items.find( vecName );

			if( elem == items.end() ) {
				return 0;
			}

			if( g_cstResolutionSink ) g_cstResolutionSink->push_back( static_cast<const void*>( (*elem).second.first ) );   // D35: RESOLVED entity
			return (*elem).second.first;
		}

		// P1: see IManager::GetItemSerial.
		unsigned long long	GetItemSerial( const char* szName ) const
		{
			if( !szName ) return 0;
			String vecName( szName );
			typename std::map<String,unsigned long long>::const_iterator it = m_serials.find( vecName );
			return ( it == m_serials.end() ) ? 0 : it->second;
		}

		T*			RequestItemUse( const char* szName, IDeletedCallback<T>& pFunc, IReference& pCaller )
		{
			// Check pointers
			if( !szName ) {
				GlobalLog()->PrintSourceError( "GenericManager::RequestItemUse:: name ptr bad", __FILE__, __LINE__ );
				return 0;
			}

			String vecName( szName );
			typename ItemListType::iterator	elem = items.find( vecName );

			if( elem == items.end() ) {
				GlobalLog()->PrintEx( eLog_Warning, "GenericManager::RequestItemUse:: '%s' not found", szName );
				return 0;
			}

			ITEMREFERENCES		itemcb( pFunc, pCaller );
			elem->second.second.push_back( itemcb );
			pCaller.addref();
			return elem->second.first;
		}

		bool		NoLongerUsingItem( T& pItem, IDeletedCallback<T>& pFunc )
		{
			// Find the item
			typename ItemListType::iterator		i, e;
			for( i=items.begin(), e=items.end(); i!=e; i++ ) {
				if( i->second.first == &pItem ) {
					// Found it!, now look for the call back
					typename ReferencesListType::iterator	m, n;
					for( m=i->second.second.begin(), n=i->second.second.end(); m!=n; m++ )
					{
						ITEMREFERENCES&	itemcb = *m;
						if( itemcb.pFunc == pFunc ) {
							// We found it, now blow this entry away.
							itemcb.pCaller.release();
							i->second.second.erase( m );
							return true;
						}
					}
				}
			}

			GlobalLog()->PrintSourceError( "GenericManager::NoLongerUsingItem:: Could not find requested item for given cb func and custom", __FILE__, __LINE__ );

			return false;
		}

		//
		// Prepares for a shutdown by releasing all references on all objects
		//
		void		Shutdown( )
		{
			typename ItemListType::iterator		i, e;
			for( i=items.begin(), e=items.end(); i!=e; i++ )
			{
				typename ReferencesListType::iterator	m, n;
				for( m=i->second.second.begin(), n=i->second.second.end(); m!=n; m++ ) {
					ITEMREFERENCES&	itemcb = (*m);
					itemcb.pFunc( *i->second.first );
					itemcb.pCaller.release();
				}

				i->second.second.clear();
				safe_release( i->second.first );
			}

			items.clear();
			m_serials.clear();   // P1
		}

		unsigned int	getItemCount( ) const{ return static_cast<unsigned int>(items.size()); }

		void			EnumerateItemNames( IEnumCallback<const char*>& pFunc ) const
		{
			typename ItemListType::const_iterator		i, e;
			for( i=items.begin(), e=items.end(); i!=e; i++ ) {
				pFunc( i->first.c_str() );
			}
		}

		void			EnumerateItems( IEnumCallback<T>& pFunc ) const
		{
			typename ItemListType::const_iterator		i, e;
			for( i=items.begin(), e=items.end(); i!=e; i++ ) {
				pFunc( i->second.first );
			}
		}
	};
}

#endif
