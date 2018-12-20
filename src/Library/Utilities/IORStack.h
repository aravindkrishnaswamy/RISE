//////////////////////////////////////////////////////////////////////
//
//  IORStack.h - Index of refraction stack
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

#include "../Interfaces/IReference.h"
#include "../Interfaces/IObject.h"
#include <stack>
#include <vector>

namespace RISE
{
	class IORStack
	{
	protected:

		struct IORDATA
		{
			const IObject* pObj;
			Scalar ior;

			IORDATA(
				const IObject* pObj_,
				Scalar ior_
				) : 
			pObj( pObj_ ),
			ior( ior_ )
			{}

			IORDATA( const IORDATA& that ) : 
			  pObj( that.pObj ),
			  ior( that.ior )
			{}
		};

		class MyIORStack : public std::stack< IORDATA, std::vector<IORDATA> >
		{
		public:

			MyIORStack(){};
			MyIORStack( const MyIORStack& s ) : 
			  std::stack< IORDATA, std::vector<IORDATA> >( s )
			{}

			~MyIORStack()
			{};

			bool find_and_destroy( const IObject* r )
			{
				std::vector<IORDATA>::reverse_iterator i, e;
				for( i=c.rbegin(), e=c.rend(); i!=e; i++ ) {
					if( i->pObj == r ) {
						c.erase( (i.base())-1 );
						return true;
					}
				}

				return false;
			}
		};	

		MyIORStack iorstack;
		mutable const IObject* pCurrentObject;

	public:
		IORStack( const Scalar ior ) :
		  pCurrentObject( 0 )
		{
			// An empty IOR stack always has the environment's IOR
			iorstack.push( IORDATA(0,ior) );
		}

		IORStack( const IORStack& s ) : 
		  iorstack( s.iorstack ),
		  pCurrentObject( s.pCurrentObject )
		{}

		~IORStack()
		{
		}

		// Ability to push stuff onto the stack
		inline void push( const Scalar ior )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::push Asked to push item onto stack with no object" );
			} else {
				iorstack.push( IORDATA(pCurrentObject,ior) );
			}
		}

		// Ability to pop something off the stack
		inline void pop()
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pop Asked to pop item onto stack but no object" );
			} else {
				// Don't pop the default air entry in the IOR stack
				if( iorstack.size() > 1 ) {
					if( !iorstack.find_and_destroy( pCurrentObject ) ) {
						GlobalLog()->PrintEasyWarning( "IORStack::pop Failed to find object to pop IOR for" );
					}
				} else {
					GlobalLog()->PrintEasyWarning( "IORStack::pop Trying to pop IOR stack with only global IOR in it, cannot allow." );
				}
			}
		}

		// Returns the IOR of the top of the stack
		inline Scalar top() const
		{
			return iorstack.top().ior;
		}

		// Sets the current object
		inline void SetCurrentObject( const IObject* pObj ) const
		{
			if( pObj ) {
				pCurrentObject = pObj;
			} else {
				GlobalLog()->PrintEasyWarning( "IORStack::SetCurrentObject Called with no object" );
			}
		}
	};
}

#endif


