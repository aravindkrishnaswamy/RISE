//////////////////////////////////////////////////////////////////////
//
//  WorkQueue.h - The work queue is what stores all the work this 
//    particular worker is doing
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORK_QUEUE_
#define WORK_QUEUE_

#include "../Interfaces/IRasterImage.h"		// for Rect
#include "../Utilities/Reference.h"
#include "../Utilities/Ray.h"
#include "../Utilities/MemoryBuffer.h"

#include <deque>

class WorkQueue : public virtual Implementation::Reference
{
public:
	enum WorkRayTypes {
		eCell,						///< This means we are getting first crack at it, its part of a cell
		ePrimary					///< Primary means some other guy started it, we are getting another crack
	};

protected:
	virtual ~WorkQueue();

	struct WORK_RAY
	{
		WorkRayTypes		type;	///< Type of work ray
		Ray					r;		///< The ray we are to trace
		unsigned int		x;		///< The x of destination pixel to get the final color
		unsigned int		y;		///< The y of destination pixel to get the final color
		IMemoryBuffer*		mb;		///< Callstack, used for primary rays
	};

	IFXMUTEX						mutQueueAccess;		///< Class level Mutex lock
	typedef std::deque<WORK_RAY>	RayListType;
	RayListType						worklist;			///< List of rays to be processed

public:
	WorkQueue();

	// Adds the cell to the end of the queue
	void AddToEnd( const Ray& r, const unsigned int x, const unsigned int y );

	// Adds the incomplete ray to the end of the queue
	void AddIncompleteToEnd( const Ray& r, const unsigned int x, const unsigned int y, IMemoryBuffer* mb );

	// Pops the front of the queue and returns it
	bool GetFront( Ray& r, unsigned int& x, unsigned int& y, IMemoryBuffer*& mb );

	// Returns the size of the queue
	unsigned int Size();
};

#endif

