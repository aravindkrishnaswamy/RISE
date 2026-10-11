//////////////////////////////////////////////////////////////////////
//
//  Timeline.cpp - Implementation of the Timeline class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Timeline.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Interfaces/ILog.h"
#include <algorithm>
#include <typeinfo>
#include "KeyframableHelper.h"
#include "../Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
    // Borrowed scratch: final results reach only the exact audited copying target.
    // Parameter's arithmetic writes via setValue; it needs no refcount/mutex.
    template<class T>
    class ArithmeticScratch final : public IKeyframeParameter {
        T value;
    public:
        explicit ArithmeticScratch(const T& v,unsigned id=0) : value(v) { paramid=id; }
        void* getValue() const override { return const_cast<T*>(&value); }
        void setValue(void* v) override { value=*static_cast<T*>(v); }
        void Add(IKeyframeParameter&,const IKeyframeParameter&,const IKeyframeParameter&) override { std::abort(); }
        void Subtract(IKeyframeParameter&,const IKeyframeParameter&,const IKeyframeParameter&) override { std::abort(); }
        void ScalarMult(IKeyframeParameter&,const IKeyframeParameter&,const Scalar&) override { std::abort(); }
        IKeyframeParameter* Clone() const override { std::abort(); }
        void addref() const override { std::abort(); }
        bool release() const override { std::abort(); }
        unsigned int refcount() const override { return 1; }
    };

    template<class T>
    bool LinearScratch(const IKeyframeParameter& a, const IKeyframeParameter& b,
                       Scalar d, IKeyframable& target)
    {
        if (typeid(a) != typeid(Parameter<T>) || typeid(b) != typeid(Parameter<T>)) return false;
        if(typeid(target)==typeid(Object)) {
            // Transformable copies all numeric parameters synchronously;
            // subclasses/custom targets retain the owned protocol below.
            ArithmeticScratch<T> left(*static_cast<T*>(a.getValue()),a.getID());
            ArithmeticScratch<T> right(*static_cast<T*>(b.getValue()));
            auto& op=const_cast<IKeyframeParameter&>(a);
            op.ScalarMult(left,a,1.0-d);
            op.ScalarMult(right,b,d);
            op.Add(left,left,right);
            target.SetIntermediateValue(left);return true;
        }
        // Keep an owned final parameter: targets may retain it with addref.
        // This one clone (and its mutex allocation) remains as the residual.
        KeyframeParameterDispatch result(*a.Clone(), true);
        auto& left=result.GetParam();
        ArithmeticScratch<T> right(*static_cast<T*>(b.getValue()));
        auto& op = const_cast<IKeyframeParameter&>(a);
        op.ScalarMult(left, a, 1.0-d);
        op.ScalarMult(right, b, d);
        op.Add(left, left, right);
        target.SetIntermediateValue(left);
        return true;
    }
}

Timeline::Timeline( 
	IKeyframable* pElem,			///< [in] Element we are keyframing
	const String& param 
	) : 
  pElement( pElem ),
  paramname( param ),
  start( 0 ),
  end( 0 )
{
	if( pElement ) {
		pElement->addref();
	}

	pInterp = new LinearInterpolator<KeyframeParameterDispatch>();
	GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "timeline default interpolator" );
}

Timeline::~Timeline()
{
	safe_release( pElement );
	safe_release( pInterp );

	for( KeyframesList::iterator it = keyframes.begin(); it!=keyframes.end(); it++ ) {
		GlobalLog()->PrintDelete( (*it), __FILE__, __LINE__ );
		delete (*it);
	}

	keyframes.clear();
}

bool Timeline::InsertKeyframe( 
	const String& value, 
	const Scalar time,
	IFullInterpolator<KeyframeParameterDispatch>* pInterp
	)
{
	IKeyframeParameter* pKF = pElement->KeyframeFromParameters( paramname, value );

	if( !pKF ) {
		GlobalLog()->PrintEasyError( "Timeline::InsertKeyframe: Element didn't want to add a keyframe here" );
		return false;
	}

	Keyframe* kf = new Keyframe( *pKF, time, pInterp );
	GlobalLog()->PrintNew( kf, __FILE__, __LINE__, "keyframe" );
	safe_release( pKF );

	keyframes.push_back( kf );
	std::sort( keyframes.begin(), keyframes.end(), &Keyframe::KeyframeCompare );
	EvaluateRange();
	CreateRuntimeKeyframesList();
	return true;
}

void Timeline::EvaluateRange( )
{
	if( keyframes.size() < 1 )
	{
		start = end = 0;
		return;
	}
	else if( keyframes.size() < 2 )
	{
		start = end = (*(keyframes.begin()))->time;
		return;
	}

	start = (*(keyframes.begin()))->time;
	end = (*(keyframes.end()-1))->time;
}

void Timeline::CreateRuntimeKeyframesList()
{
	runtimekeyframes.clear();
	runtimekeyframes.reserve( keyframes.size() );

	for( KeyframesList::const_iterator it = keyframes.begin(); it != keyframes.end(); it++ ) {
		runtimekeyframes.push_back( KeyframeParameterDispatch((*it)->param) );
	}
}

void Timeline::GetTimeRange( Scalar& begin, Scalar& end )
{
	begin = this->start;
	end = this->end;
}

void Timeline::EvaluateAtTime( const Scalar time )
{
	EvaluateInto( time, *pElement );
}

void Timeline::EvaluateInto( const Scalar time, IKeyframable& target ) const
{
	// Use an interpolator and evaluate at the current time
	// There is only one keyframe, which means we are asymptotic...
	// We only render the one frame we are to render
	if( start == end ) {
		if( time == start ) {
			target.SetIntermediateValue( (*keyframes.begin())->param );
			return;
		}
	}

	if( time <= start ) {
		target.SetIntermediateValue( (*keyframes.begin())->param );
		return;
	}

	if( time >= end ) {
		target.SetIntermediateValue( (*(keyframes.end()-1))->param );
		return;
	}

	// Otherwise we need to find the two keyframes we are to interpolate
	// and call the interpolator to interpolate them
	KeyframesList::const_iterator	i, e;
	RuntimeKeyframesList::const_iterator m=runtimekeyframes.begin();

	for( i=keyframes.begin(), e=keyframes.end()-1; i!=e; i++, m++ )
	{
		if( time >= (*i)->time &&
			time <= (*(i+1))->time
			)
		{
			// We've found our keyframes
			// We need to store the result somewhere, we'll ask a keyframeparameter
			// to clone itself so we can store it there
			const Keyframe* before = *i;
			const Keyframe* after = *(i+1);

			const Scalar d = (time - before->time) / (after->time - before->time);

			// Preserve the built-in linear expression a*(1-d) + b*d.
            // Unknown parameter/interpolator subclasses retain their virtual path.
            if (!before->pInterp || typeid(*before->pInterp) == typeid(LinearInterpolator<KeyframeParameterDispatch>)) {
                const auto& a = before->param;
                const auto& b = after->param;
                if (LinearScratch<Point3>(a,b,d,target) || LinearScratch<Vector3>(a,b,d,target) ||
                    LinearScratch<Scalar>(a,b,d,target) || LinearScratch<RISEPel>(a,b,d,target) ||
                    LinearScratch<Vector2>(a,b,d,target)) return;
            }

			RuntimeKeyframesList::const_iterator s = m;
			RuntimeKeyframesList::const_iterator t = (m+1);

			if( before->pInterp ) {
				KeyframeParameterDispatch result = before->pInterp->Interpolate2Values( runtimekeyframes, s, t, d );
				target.SetIntermediateValue( result.GetParam() );
			} else {
				KeyframeParameterDispatch result = pInterp->Interpolate2Values( runtimekeyframes, s, t, d );
				target.SetIntermediateValue( result.GetParam() );
			}
			return;
		}
	}
}

