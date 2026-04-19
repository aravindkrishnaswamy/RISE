//////////////////////////////////////////////////////////////////////
//
//  Observable.h - Generic notification mixin for concrete RISE classes.
//
//  A subject inherits from Observable and calls NotifyObservers() when its
//  state has changed in a way that downstream consumers should react to.
//  Consumers register a callback via Attach() and receive a Token they can
//  use to Detach().  Use the RAII Subscription helper to make Detach
//  automatic in the consumer's destructor.
//
//  Threading model
//  ---------------
//  Not safe for concurrent Attach / Detach / NotifyObservers.  Designed for
//  the RISE animator path: keyframe updates and notifications fire on the
//  main thread between frames, after all per-frame parameter pushes land
//  but before tile workers spin up for the next frame.  Do not call
//  NotifyObservers() from a worker thread.
//
//  Lifetime
//  --------
//  The subject holds raw callbacks.  A consumer that disappears without
//  detaching leaves a dangling callback that will fire on the next
//  notification — undefined behaviour.  Use Subscription as a member of
//  the consumer, declared AFTER any addref'd subject pointer, so the
//  subscription's destructor runs first and calls Detach while the subject
//  is still alive.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBSERVABLE_
#define OBSERVABLE_

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		// Concrete-class mixin.  Subjects inherit publicly from Observable and
		// call NotifyObservers() from their own state-changing code paths.
		// Designed to be added without disturbing existing abstract interfaces:
		// consumers reach the mixin via dynamic_cast<Observable*>, so out-of-tree
		// subclasses that don't inherit it are simply non-observable (today's
		// behaviour).
		class Observable
		{
		public:
			typedef std::function<void()> Callback;
			typedef unsigned int          Token;

			// Register a callback to fire on every NotifyObservers() call.
			// Returns an opaque token used by Detach().  Callable on a const
			// Observable* because the observer list is mutable — subjects
			// commonly hand out const pointers.
			Token Attach( Callback cb ) const
			{
				const Token tok = ++m_nextToken;
				m_observers.push_back( Entry{ tok, std::move(cb) } );
				return tok;
			}

			// Unregister a previously-attached callback.  Silently ignores
			// unknown tokens, so it is safe to call after the subject's own
			// state has been partially torn down.
			void Detach( Token tok ) const
			{
				m_observers.erase(
					std::remove_if(
						m_observers.begin(), m_observers.end(),
						[tok]( const Entry& e ){ return e.tok == tok; } ),
					m_observers.end() );
			}

		protected:
			Observable() : m_nextToken(0) {}
			virtual ~Observable() {}

			// Invoke from the subject after its observable state has changed.
			// Each callback fires at most once per notification.  Iterates over
			// a snapshot of tokens so a callback is free to Attach or Detach
			// (including itself or others) during the call; each remaining
			// callback is re-looked-up by token before invocation so we never
			// fire one that was detached mid-iteration.  O(n·m) per notify,
			// where n = snapshot size and m = current observer count — fine
			// for the small observer lists expected here (typically 1–2).
			void NotifyObservers() const
			{
				std::vector<Token> snapshot;
				snapshot.reserve( m_observers.size() );
				for( const Entry& e : m_observers ) snapshot.push_back( e.tok );

				for( Token tok : snapshot ) {
					// Re-find by token in case an earlier callback detached this one.
					// Also take a local copy of the callback in case an earlier
					// callback mutated m_observers (invalidating iterators).
					Callback cb;
					bool found = false;
					for( const Entry& e : m_observers ) {
						if( e.tok == tok ) { cb = e.cb; found = true; break; }
					}
					if( found ) cb();
				}
			}

		private:
			struct Entry
			{
				Token    tok;
				Callback cb;
			};

			mutable Token              m_nextToken;
			mutable std::vector<Entry> m_observers;

			Observable( const Observable& ) = delete;
			Observable& operator=( const Observable& ) = delete;
		};

		// RAII subscription handle.  Construct from a subject pointer and a
		// callback; destructor detaches.  Move-only.  Empty when default-
		// constructed or moved-from — both states are safe to destroy.
		//
		// Destruction ordering for consumers
		// ----------------------------------
		// Detach() must execute while the subject is still alive.  There are
		// two common consumer patterns:
		//
		// 1. Consumer holds the subject via a member smart pointer and does
		//    NOT touch it in the destructor body.  In that case, member
		//    destruction order alone is enough: declare the Subscription
		//    member AFTER the subject smart-pointer member, so reverse
		//    declaration order destructs the Subscription first (while the
		//    subject is still alive via the still-undestructed smart pointer).
		//
		// 2. Consumer holds the subject via a raw pointer + manual addref/
		//    release (the common RISE pattern) and releases it in its own
		//    destructor body.  Member destruction happens AFTER the body
		//    runs, so declaration order alone does NOT help — by the time
		//    the Subscription member destructs, the subject has already been
		//    released and is potentially freed.  Consumers in this pattern
		//    MUST explicitly reset the subscription at the top of the
		//    destructor body, before any release call, e.g.:
		//
		//        ~Consumer() {
		//            m_subscription = Subscription();   // detach while subject is alive
		//            m_pSubject->release();
		//            m_pSubject = 0;
		//        }
		//
		//    The move-assignment operator detaches the previous binding
		//    before overwriting, so this one-liner is the full fix.
		class Subscription
		{
		public:
			Subscription()
				: m_subject( 0 ), m_tok( 0 )
			{}

			Subscription( const Observable* subject, Observable::Callback cb )
				: m_subject( subject )
				, m_tok( subject ? subject->Attach( std::move(cb) ) : 0 )
			{}

			Subscription( const Subscription& ) = delete;
			Subscription& operator=( const Subscription& ) = delete;

			Subscription( Subscription&& other ) noexcept
				: m_subject( other.m_subject )
				, m_tok( other.m_tok )
			{
				other.m_subject = 0;
			}

			Subscription& operator=( Subscription&& other ) noexcept
			{
				if( this != &other ) {
					if( m_subject ) m_subject->Detach( m_tok );
					m_subject       = other.m_subject;
					m_tok           = other.m_tok;
					other.m_subject = 0;
				}
				return *this;
			}

			~Subscription()
			{
				if( m_subject ) {
					m_subject->Detach( m_tok );
				}
			}

		private:
			const Observable* m_subject;
			Observable::Token m_tok;
		};
	}
}

#endif
