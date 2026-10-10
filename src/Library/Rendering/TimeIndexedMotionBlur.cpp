//////////////////////////////////////////////////////////////////////
//
//  TimeIndexedMotionBlur.cpp - DL-465, see TimeIndexedMotionBlur.h
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TimeIndexedMotionBlur.h"
#include "../Interfaces/IAnimator.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IShader.h"
#include "../Interfaces/ILog.h"
#include "../Objects/Object.h"
#include "../Objects/CSGObject.h"
#include "../Cameras/CameraCommon.h"
#include "../Lights/PointLight.h"
#include "../Lights/SpotLight.h"
#include "../Lights/DirectionalLight.h"
#include "../Lights/AmbientLight.h"
#include <atomic>
#include <typeinfo>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	thread_local TimeIndexedThread* tlsCurrentThread = nullptr;
	std::atomic<unsigned long long> gFramesCreated( 0 );

	//! Why an object cannot be time-indexed, or null if it can.
	const char* ObjectRefusal( const Object* o )
	{
		if( dynamic_cast<const CSGObject*>( o ) ) {
			return "a CSG composite";
		}
		if( o->IsConsumed() ) {
			return "a CSG operand";
		}
		const IShader* pShader = o->GetShader();
		if( pShader && pShader->HasRuntimeData() ) {
			return "an object whose shader keeps runtime data";
		}
		return 0;
	}
}

TimeIndexedFrame::TimeIndexedFrame( const IScene& scene_ ) :
  scene( scene_ ),
  pAnimator( scene_.GetAnimator() ),
  cameraShared( 0 ),
  camera( 0 )
{
}

TimeIndexedFrame::~TimeIndexedFrame()
{
	// Unslot every shared element: outside the pass they answer from
	// themselves again.  Runs after the workers have joined.
	for( std::size_t i = 0; i < entries.size(); i++ ) {
		SetSharedSlot( entries[i], -1 );
	}
}

void TimeIndexedFrame::SetSharedSlot( const Entry& e, const int slot )
{
	// Through the animator's own (non-const) element pointer.
	switch( e.kind ) {
	case kObject:			dynamic_cast<Object*>( e.shared )->SetTimeSlot( slot ); break;
	case kPointLight:		dynamic_cast<PointLight*>( e.shared )->SetTimeSlot( slot ); break;
	case kSpotLight:		dynamic_cast<SpotLight*>( e.shared )->SetTimeSlot( slot ); break;
	case kDirectionalLight:	dynamic_cast<DirectionalLight*>( e.shared )->SetTimeSlot( slot ); break;
	case kAmbientLight:		dynamic_cast<AmbientLight*>( e.shared )->SetTimeSlot( slot ); break;
	}
}

int TimeIndexedFrame::SlotOf( const void* typed ) const
{
	for( std::size_t i = 0; i < entries.size(); i++ ) {
		if( entries[i].typed == typed ) {
			return static_cast<int>( i );
		}
	}
	return -1;
}

TimeIndexedFrame* TimeIndexedFrame::TryCreate( const IScene& scene, std::string& reason )
{
	IAnimator* pAnimator = scene.GetAnimator();
	const IObjectManager* pObjects = scene.GetObjects();
	if( !pAnimator || !pObjects ) {
		reason = "no animator or object manager";
		return 0;
	}
	// A thread that is already inside a time-indexed pass would read its
	// own camera clone back through Scene::GetCamera below.
	if( TimeIndexed::tlsSlots ) {
		reason = "nested time-indexed pass";
		return 0;
	}

	TimeIndexedFrame* f = new TimeIndexedFrame( scene );
	GlobalLog()->PrintNew( f, __FILE__, __LINE__, "time-indexed frame" );

	struct Fail {
		static TimeIndexedFrame* Out( TimeIndexedFrame* fr, std::string& r, const std::string& why ) {
			r = why;
			// Nothing has been slotted yet (slots are written last).
			fr->entries.clear();
			GlobalLog()->PrintDelete( fr, __FILE__, __LINE__ );
			delete fr;
			return 0;
		}
	};

	std::vector<IKeyframable*> animated;
	pAnimator->GetActiveAnimatedElements( animated );
	for( std::size_t k = 0; k < animated.size(); k++ ) {
		IKeyframable* elem = animated[k];
		Entry e;
		e.shared = elem;
		e.animated = true;
		e.typed = 0;
		if( Object* o = dynamic_cast<Object*>( elem ) ) {
			if( const char* why = ObjectRefusal( o ) ) {
				return Fail::Out( f, reason, std::string( "a keyframed object is " ) + why );
			}
			e.kind = kObject;
			e.typed = static_cast<const Object*>( o );
		} else if( CameraCommon* c = dynamic_cast<CameraCommon*>( elem ) ) {
			// Only the scene's active camera generates rays; a keyframed
			// inactive camera is never read by the render (the per-sample
			// path moves it too, to no effect).
			if( static_cast<const ICamera*>( c ) == scene.GetCamera() ) {
				f->cameraShared = elem;
				f->camera = c;
			}
			continue;
		} else if( PointLight* l = dynamic_cast<PointLight*>( elem ) ) {
			e.kind = kPointLight;
			e.typed = static_cast<const PointLight*>( l );
		} else if( SpotLight* l = dynamic_cast<SpotLight*>( elem ) ) {
			e.kind = kSpotLight;
			e.typed = static_cast<const SpotLight*>( l );
		} else if( DirectionalLight* l = dynamic_cast<DirectionalLight*>( elem ) ) {
			e.kind = kDirectionalLight;
			e.typed = static_cast<const DirectionalLight*>( l );
		} else if( AmbientLight* l = dynamic_cast<AmbientLight*>( elem ) ) {
			e.kind = kAmbientLight;
			e.typed = static_cast<const AmbientLight*>( l );
		} else {
			std::vector<String> params;
			pAnimator->GetActiveAnimatedParameters( elem, params );
			std::string why = std::string( "a keyframed " ) + typeid( *elem ).name();
			if( !params.empty() ) {
				why = why + " ('" + std::string( params[0].c_str() ) + "')";
			}
			return Fail::Out( f, reason, why );
		}
		f->entries.push_back( e );
	}

	// The per-sample hierarchy re-compose (DL-457/463), replayed on the
	// clones: every node it touches is slotted, parents before children.
	std::vector< std::pair<IObjectPriv*, IObjectPriv*> > plan;
	if( !pObjects->GetPerSampleRecomposePlan( plan ) ) {
		return Fail::Out( f, reason, "a per-sample hierarchy re-compose no shutter sweep narrowed" );
	}
	for( std::size_t k = 0; k < plan.size(); k++ ) {
		Object* node = dynamic_cast<Object*>( plan[k].first );
		if( !node ) {
			return Fail::Out( f, reason, "a hierarchy node that is not a plain object" );
		}
		if( const char* why = ObjectRefusal( node ) ) {
			return Fail::Out( f, reason, std::string( "a moving hierarchy node is " ) + why );
		}
		int slot = f->SlotOf( static_cast<const Object*>( node ) );
		if( slot < 0 ) {
			Entry e;
			e.kind = kObject;
			e.shared = node;
			e.typed = static_cast<const Object*>( node );
			e.animated = false;
			f->entries.push_back( e );
			slot = static_cast<int>( f->entries.size() ) - 1;
		}
		Recompose r;
		r.node = slot;
		r.parent = -1;
		r.parentShared = 0;
		if( plan[k].second ) {
			const Object* parent = dynamic_cast<const Object*>( plan[k].second );
			if( !parent ) {
				return Fail::Out( f, reason, "a hierarchy parent that is not a plain object" );
			}
			r.parent = f->SlotOf( parent );
			if( r.parent < 0 ) {
				r.parentShared = parent;
			}
		}
		f->recompose.push_back( r );
	}

	// Slot every shared element last, so a refusal above leaves the scene
	// untouched.
	for( std::size_t i = 0; i < f->entries.size(); i++ ) {
		f->SetSharedSlot( f->entries[i], static_cast<int>( i ) );
	}
	gFramesCreated.fetch_add( 1, std::memory_order_relaxed );
	return f;
}

unsigned long long TimeIndexedFrame::CreatedCount()
{
	return gFramesCreated.load( std::memory_order_relaxed );
}

TimeIndexedThread::TimeIndexedThread( const TimeIndexedFrame* frame_ ) :
  frame( frame_ ),
  camera( 0 ),
  previousTable( 0 ),
  previousCurrent( 0 )
{
	table.slots = 0;
	table.count = 0;
	table.camera = 0;
	if( !frame ) {
		return;
	}

	const std::size_t n = frame->entries.size();
	slots.assign( n, static_cast<const void*>( 0 ) );
	keyframable.assign( n, static_cast<IKeyframable*>( 0 ) );
	objects.assign( n, static_cast<Object*>( 0 ) );
	owned.reserve( n + 1 );

	for( std::size_t i = 0; i < n; i++ ) {
		const TimeIndexedFrame::Entry& e = frame->entries[i];
		switch( e.kind ) {
		case TimeIndexedFrame::kObject: {
			Object* c = static_cast<const Object*>( e.typed )->ClonePoseHolder();
			slots[i] = static_cast<const void*>( static_cast<const Object*>( c ) );
			keyframable[i] = c;
			objects[i] = c;
			owned.push_back( c );
			break; }
		case TimeIndexedFrame::kPointLight: {
			PointLight* c = static_cast<const PointLight*>( e.typed )->CloneForTimeView();
			slots[i] = static_cast<const void*>( static_cast<const PointLight*>( c ) );
			keyframable[i] = c;
			owned.push_back( c );
			break; }
		case TimeIndexedFrame::kSpotLight: {
			SpotLight* c = static_cast<const SpotLight*>( e.typed )->CloneForTimeView();
			slots[i] = static_cast<const void*>( static_cast<const SpotLight*>( c ) );
			keyframable[i] = c;
			owned.push_back( c );
			break; }
		case TimeIndexedFrame::kDirectionalLight: {
			DirectionalLight* c = static_cast<const DirectionalLight*>( e.typed )->CloneForTimeView();
			slots[i] = static_cast<const void*>( static_cast<const DirectionalLight*>( c ) );
			keyframable[i] = c;
			owned.push_back( c );
			break; }
		case TimeIndexedFrame::kAmbientLight: {
			AmbientLight* c = static_cast<const AmbientLight*>( e.typed )->CloneForTimeView();
			slots[i] = static_cast<const void*>( static_cast<const AmbientLight*>( c ) );
			keyframable[i] = c;
			owned.push_back( c );
			break; }
		}
	}
	if( frame->camera ) {
		camera = frame->camera->CloneForTimeView();
		owned.push_back( camera );
	}

	table.slots = slots.empty() ? 0 : &slots[0];
	table.count = static_cast<unsigned int>( n );
	table.camera = camera;
	previousTable = TimeIndexed::tlsSlots;
	TimeIndexed::tlsSlots = &table;
	previousCurrent = tlsCurrentThread;
	tlsCurrentThread = this;
}

TimeIndexedThread::~TimeIndexedThread()
{
	if( !frame ) {
		return;
	}
	TimeIndexed::tlsSlots = previousTable;
	tlsCurrentThread = previousCurrent;
	for( std::size_t i = 0; i < owned.size(); i++ ) {
		owned[i]->release();
	}
}

TimeIndexedThread* TimeIndexedThread::Current()
{
	return tlsCurrentThread;
}

void TimeIndexedThread::PoseAt( const Scalar time )
{
	if( !frame ) {
		return;
	}
	// 1. Every animated element, exactly as IAnimator::EvaluateAtTime would
	//    move it, but into this thread's clone.
	for( std::size_t i = 0; i < keyframable.size(); i++ ) {
		if( frame->entries[i].animated ) {
			frame->pAnimator->EvaluateElementAtTimeInto( frame->entries[i].shared, time, *keyframable[i] );
		}
	}
	if( camera ) {
		frame->pAnimator->EvaluateElementAtTimeInto( frame->cameraShared, time, *camera );
	}
	// 2. The hierarchy re-compose ObjectManager::RecomposeAnimatedHierarchy
	//    runs per sample, on the clones (parents before children).
	for( std::size_t k = 0; k < frame->recompose.size(); k++ ) {
		const TimeIndexedFrame::Recompose& r = frame->recompose[k];
		Matrix4 parentWorld = Matrix4Ops::Identity();
		if( r.parent >= 0 ) {
			parentWorld = objects[r.parent]->GetFinalTransformMatrix();
		} else if( r.parentShared ) {
			parentWorld = r.parentShared->GetFinalTransformMatrix();
		}
		objects[r.node]->FinalizeTransformations( parentWorld );
	}
}
